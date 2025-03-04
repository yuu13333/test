/****************************************************************************
**
** Copyright (C) 2018 The Qt Company Ltd.
** Contact: https://www.qt.io/licensing/
**
** This file is part of the QtNetwork module of the Qt Toolkit.
**
** $QT_BEGIN_LICENSE:LGPL$
** Commercial License Usage
** Licensees holding valid commercial Qt licenses may use this file in
** accordance with the commercial license agreement provided with the
** Software or, alternatively, in accordance with the terms contained in
** a written agreement between you and The Qt Company. For licensing terms
** and conditions see https://www.qt.io/terms-conditions. For further
** information use the contact form at https://www.qt.io/contact-us.
**
** GNU Lesser General Public License Usage
** Alternatively, this file may be used under the terms of the GNU Lesser
** General Public License version 3 as published by the Free Software
** Foundation and appearing in the file LICENSE.LGPL3 included in the
** packaging of this file. Please review the following information to
** ensure the GNU Lesser General Public License version 3 requirements
** will be met: https://www.gnu.org/licenses/lgpl-3.0.html.
**
** GNU General Public License Usage
** Alternatively, this file may be used under the terms of the GNU
** General Public License version 2.0 or (at your option) the GNU General
** Public license version 3 or any later version approved by the KDE Free
** Qt Foundation. The licenses are as published by the Free Software
** Foundation and appearing in the file LICENSE.GPL2 and LICENSE.GPL3
** included in the packaging of this file. Please review the following
** information to ensure the GNU General Public License requirements will
** be met: https://www.gnu.org/licenses/gpl-2.0.html and
** https://www.gnu.org/licenses/gpl-3.0.html.
**
** $QT_END_LICENSE$
**
****************************************************************************/

// #define QSSLSOCKET_DEBUG

#include "qssl_p.h"
#include "qsslsocket.h"
#include "qsslsocket_schannel_p.h"
#include "qsslcertificate.h"
#include "qsslcertificateextension.h"
#include "qsslcertificate_p.h"
#include "qsslcipher_p.h"

#include <QtCore/qscopeguard.h>
#include <QtCore/qoperatingsystemversion.h>
#include <QtCore/qregularexpression.h>
#include <QtCore/qdatastream.h>
#include <QtCore/qmutex.h>

#define SECURITY_WIN32
#include <security.h>
#include <schnlsp.h>

// Not defined in MinGW
#ifndef SECBUFFER_ALERT
#define SECBUFFER_ALERT 17
#endif
#ifndef SECPKG_ATTR_APPLICATION_PROTOCOL
#define SECPKG_ATTR_APPLICATION_PROTOCOL 35
#endif

// Another missing MinGW define
#ifndef SEC_E_APPLICATION_PROTOCOL_MISMATCH
#define SEC_E_APPLICATION_PROTOCOL_MISMATCH _HRESULT_TYPEDEF_(0x80090367L)
#endif

// Also not defined in MinGW.......
#ifndef SP_PROT_TLS1_SERVER
#define SP_PROT_TLS1_SERVER 0x00000040
#endif
#ifndef SP_PROT_TLS1_CLIENT
#define SP_PROT_TLS1_CLIENT 0x00000080
#endif
#ifndef SP_PROT_TLS1_0_SERVER
#define SP_PROT_TLS1_0_SERVER SP_PROT_TLS1_SERVER
#endif
#ifndef SP_PROT_TLS1_0_CLIENT
#define SP_PROT_TLS1_0_CLIENT SP_PROT_TLS1_CLIENT
#endif
#ifndef SP_PROT_TLS1_0
#define SP_PROT_TLS1_0 (SP_PROT_TLS1_0_CLIENT | SP_PROT_TLS1_0_SERVER)
#endif
#ifndef SP_PROT_TLS1_1_SERVER
#define SP_PROT_TLS1_1_SERVER 0x00000100
#endif
#ifndef SP_PROT_TLS1_1_CLIENT
#define SP_PROT_TLS1_1_CLIENT 0x00000200
#endif
#ifndef SP_PROT_TLS1_1
#define SP_PROT_TLS1_1 (SP_PROT_TLS1_1_CLIENT | SP_PROT_TLS1_1_SERVER)
#endif
#ifndef SP_PROT_TLS1_2_SERVER
#define SP_PROT_TLS1_2_SERVER 0x00000400
#endif
#ifndef SP_PROT_TLS1_2_CLIENT
#define SP_PROT_TLS1_2_CLIENT 0x00000800
#endif
#ifndef SP_PROT_TLS1_2
#define SP_PROT_TLS1_2 (SP_PROT_TLS1_2_CLIENT | SP_PROT_TLS1_2_SERVER)
#endif
#ifndef SP_PROT_TLS1_3_SERVER
#define SP_PROT_TLS1_3_SERVER 0x00001000
#endif
#ifndef SP_PROT_TLS1_3_CLIENT
#define SP_PROT_TLS1_3_CLIENT 0x00002000
#endif
#ifndef SP_PROT_TLS1_3
#define SP_PROT_TLS1_3 (SP_PROT_TLS1_3_CLIENT | SP_PROT_TLS1_3_SERVER)
#endif

#define CONST_REINTERPRET_CAST(target_type, value) \
    const_cast<target_type>(reinterpret_cast<const target_type>(value))

/*
    @future!:

    - Transmitting intermediate certificates
        - Look for a way to avoid putting intermediate certificates in the certificate store
        - No documentation on how to send the chain
        - A stackoverflow question on this from 3 years ago implies schannel only sends intermediate
            certificates if it's "in the system or user certificate store".
                - https://stackoverflow.com/q/30156584/2493610
    - PSK support
        - Was added in Windows 10 (it seems), documentation at time of writing is sparse/non-existent.
            - Specifically about how to supply credentials when they're requested.
            - Or how to recognize that they're requested in the first place.
        - Skip certificate verification.
        - Check if "PSK-only" is still required to do PSK _at all_ (all-around bad solution).
        - Check if SEC_I_INCOMPLETE_CREDENTIALS is still returned for both "missing certificate" and
            "missing PSK" when calling InitializeSecurityContext in "performHandshake".

    Medium priority:
    - Setting cipher-suites (or ALG_ID)
        - People have survived without it in WinRT
    - peerCertificateChain() is different in Schannel in that it includes the entire chain, not just
        the certificates that the peer sent. This means the root certificate is found in this chain.
        Which I would argue makes sense for a list whose purpose is displaying the chain.
        But this should be up for discussion. Relevant (support-reported) bug: QTBUG-20119
    - ALPN:
        For HTTP2. Note: Windows 8.1 and up ONLY.

    Low priority:
    - Possibly make RAII wrappers for SecBuffer (which I commonly create QScopeGuards for)
    - Perform the '@future' optimization in "transmit()"

*/

QT_BEGIN_NAMESPACE

namespace {
SecBuffer createSecBuffer(void *ptr, unsigned long length, unsigned long bufferType)
{
    return SecBuffer{ length, bufferType, ptr };
}

SecBuffer createSecBuffer(QByteArray &buffer, unsigned long bufferType)
{
    return createSecBuffer(buffer.data(), static_cast<unsigned long>(buffer.length()), bufferType);
}

QString schannelError(qint32 status)
{
    switch (status) {
    case SEC_E_INSUFFICIENT_MEMORY:
        return QSslSocket::tr("Insufficient memory");
    case SEC_E_INTERNAL_ERROR:
        return QSslSocket::tr("Internal error");
    case SEC_E_INVALID_HANDLE:
        return QSslSocket::tr("An internal handle was invalid");
    case SEC_E_INVALID_TOKEN:
        return QSslSocket::tr("An internal token was invalid");
    case SEC_E_LOGON_DENIED:
        // According to the link below we get this error when Schannel receives TLS1_ALERT_ACCESS_DENIED
        // https://docs.microsoft.com/en-us/windows/desktop/secauthn/schannel-error-codes-for-tls-and-ssl-alerts
        return QSslSocket::tr("Access denied");
    case SEC_E_NO_AUTHENTICATING_AUTHORITY:
        return QSslSocket::tr("No authority could be contacted for authorization");
    case SEC_E_NO_CREDENTIALS:
        return QSslSocket::tr("No credentials");
    case SEC_E_TARGET_UNKNOWN:
        return QSslSocket::tr("The target is unknown or unreachable");
    case SEC_E_UNSUPPORTED_FUNCTION:
        return QSslSocket::tr("An unsupported function was requested");
    case SEC_E_WRONG_PRINCIPAL:
        // SNI error
        return QSslSocket::tr("The hostname provided does not match the one received from the peer");
    case SEC_E_APPLICATION_PROTOCOL_MISMATCH:
        return QSslSocket::tr("No common protocol exists between the client and the server");
    case SEC_E_ILLEGAL_MESSAGE:
        return QSslSocket::tr("Unexpected or badly-formatted message received");
    case SEC_E_ENCRYPT_FAILURE:
        return QSslSocket::tr("The data could not be encrypted");
    case SEC_E_ALGORITHM_MISMATCH:
        return QSslSocket::tr("No cipher suites in common");
    case SEC_E_UNKNOWN_CREDENTIALS:
        // This can mean "invalid argument" in some cases...
        return QSslSocket::tr("The credentials were not recognized / Invalid argument");
    default:
        return QSslSocket::tr("Unknown error occurred: %1").arg(status);
    }
}

DWORD fromQtSslProtocol(QSsl::SslProtocol protocol)
{
    DWORD protocols = SP_PROT_NONE;
    switch (protocol) {
    case QSsl::UnknownProtocol:
        return SP_PROT_NONE;
    case QSsl::DtlsV1_0:
    case QSsl::DtlsV1_2:
    case QSsl::DtlsV1_0OrLater:
    case QSsl::DtlsV1_2OrLater:
        return SP_PROT_NONE; // Not supported at the moment (@future)
    case QSsl::AnyProtocol:
        protocols = SP_PROT_TLS1_0 | SP_PROT_TLS1_1 | SP_PROT_TLS1_2;
        // @future Add TLS 1.3 when supported by Windows!
        break;
    case QSsl::SslV2:
    case QSsl::SslV3:
        return SP_PROT_NONE; // Not supported
    case QSsl::TlsV1SslV3:
        protocols = SP_PROT_TLS1_0;
        break;
    case QSsl::TlsV1_0:
        protocols = SP_PROT_TLS1_0;
        break;
    case QSsl::TlsV1_1:
        protocols = SP_PROT_TLS1_1;
        break;
    case QSsl::TlsV1_2:
        protocols = SP_PROT_TLS1_2;
        break;
    case QSsl::TlsV1_3:
        if ((false)) // @future[0/1] Replace with version check once it's supported in Windows
            protocols = SP_PROT_TLS1_3;
        else
            protocols = SP_PROT_NONE;
        break;
    case QSsl::SecureProtocols: // TLS v1.0 and later is currently considered secure
    case QSsl::TlsV1_0OrLater:
        // For the "OrLater" protocols we fall through from one to the next, adding all of them
        // in ascending order
        protocols = SP_PROT_TLS1_0;
        Q_FALLTHROUGH();
    case QSsl::TlsV1_1OrLater:
        protocols |= SP_PROT_TLS1_1;
        Q_FALLTHROUGH();
    case QSsl::TlsV1_2OrLater:
        protocols |= SP_PROT_TLS1_2;
        Q_FALLTHROUGH();
    case QSsl::TlsV1_3OrLater:
        if ((false)) // @future[1/1] Also replace this with a version check
            protocols |= SP_PROT_TLS1_3;
        else if (protocol == QSsl::TlsV1_3OrLater)
            protocols = SP_PROT_NONE; // if TlsV1_3OrLater was specifically chosen we should fail
        break;
    }
    return protocols;
}

/*!
    \internal
    Used when converting the established session's \a protocol back to
    Qt's own SslProtocol type.

    Only one protocol should be passed in at a time.
*/
QSsl::SslProtocol toQtSslProtocol(DWORD protocol)
{
#define MAP_PROTOCOL(sp_protocol, q_protocol) \
    if (protocol & sp_protocol) {             \
        Q_ASSERT(!(protocol & ~sp_protocol)); \
        return q_protocol;                    \
    }

    MAP_PROTOCOL(SP_PROT_TLS1_0, QSsl::TlsV1_0)
    MAP_PROTOCOL(SP_PROT_TLS1_1, QSsl::TlsV1_1)
    MAP_PROTOCOL(SP_PROT_TLS1_2, QSsl::TlsV1_2)
    MAP_PROTOCOL(SP_PROT_TLS1_3, QSsl::TlsV1_3)
#undef MAP_PROTOCOL
    Q_UNREACHABLE();
    return QSsl::UnknownProtocol;
}

/*!
    \internal
    Used by verifyCertContext to check if a client cert is used by a server or vice versa.
*/
bool netscapeWrongCertType(const QList<QSslCertificateExtension> &extensions, bool isClient)
{
    const auto netscapeIt = std::find_if(
            extensions.cbegin(), extensions.cend(),
            [](const QSslCertificateExtension &extension) {
                const auto netscapeCertType = QStringLiteral("2.16.840.1.113730.1.1");
                return extension.oid() == netscapeCertType;
            });
    if (netscapeIt != extensions.cend()) {
        const QByteArray netscapeCertTypeByte = netscapeIt->value().toByteArray();
        int netscapeCertType = 0;
        QDataStream dataStream(netscapeCertTypeByte);
        dataStream >> netscapeCertType;
        if (dataStream.status() != QDataStream::Status::Ok)
            return true;
        const int expectedPeerCertType = isClient ? NETSCAPE_SSL_SERVER_AUTH_CERT_TYPE
                                                  : NETSCAPE_SSL_CLIENT_AUTH_CERT_TYPE;
        if ((netscapeCertType & expectedPeerCertType) == 0)
            return true;
    }
    return false;
}

/*!
    \internal
    Used by verifyCertContext to check the basicConstraints certificate
    extension to see if the certificate is a certificate authority.
    Returns false if the certificate does not have the basicConstraints
    extension or if it is not a certificate authority.
*/
bool isCertificateAuthority(const QList<QSslCertificateExtension> &extensions)
{
    auto it = std::find_if(extensions.cbegin(), extensions.cend(),
                           [](const QSslCertificateExtension &extension) {
                               return extension.name() == QLatin1String("basicConstraints");
                           });
    if (it != extensions.cend()) {
        QVariantMap basicConstraints = it->value().toMap();
        return basicConstraints.value(QLatin1String("ca"), false).toBool();
    }
    return false;
}

/*!
    \internal
    Returns true if the attributes we requested from the context/handshake have
    been given.
*/
bool matchesContextRequirements(DWORD attributes, DWORD requirements,
                                QSslSocket::PeerVerifyMode verifyMode,
                                bool isClient)
{
#ifdef QSSLSOCKET_DEBUG
#define DEBUG_WARN(message) qCWarning(lcSsl, message)
#else
#define DEBUG_WARN(message)
#endif

#define CHECK_ATTRIBUTE(attributeName)                                                                 \
    do {                                                                                               \
        const DWORD req##attributeName = isClient ? ISC_REQ_##attributeName : ASC_REQ_##attributeName; \
        const DWORD ret##attributeName = isClient ? ISC_RET_##attributeName : ASC_RET_##attributeName; \
        if (!(requirements & req##attributeName) != !(attributes & ret##attributeName)) {              \
            DEBUG_WARN("Missing attribute \"" #attributeName "\"");                                    \
            return false;                                                                              \
        }                                                                                              \
    } while (false)

    CHECK_ATTRIBUTE(CONFIDENTIALITY);
    CHECK_ATTRIBUTE(REPLAY_DETECT);
    CHECK_ATTRIBUTE(SEQUENCE_DETECT);
    CHECK_ATTRIBUTE(STREAM);
    if (verifyMode == QSslSocket::PeerVerifyMode::VerifyPeer)
        CHECK_ATTRIBUTE(MUTUAL_AUTH);

    // This one is manual because there is no server / ASC_ version
    if (isClient) {
        const auto reqManualCredValidation = ISC_REQ_MANUAL_CRED_VALIDATION;
        const auto retManualCredValidation = ISC_RET_MANUAL_CRED_VALIDATION;
        if (!(requirements & reqManualCredValidation) != !(attributes & retManualCredValidation)) {
            DEBUG_WARN("Missing attribute \"MANUAL_CRED_VALIDATION\"");
            return false;
        }
    }

    return true;
#undef CHECK_ATTRIBUTE
#undef DEBUG_WARN
}
} // anonymous namespace

bool QSslSocketPrivate::s_loadRootCertsOnDemand = true;
bool QSslSocketPrivate::s_loadedCiphersAndCerts = false;

void QSslSocketPrivate::ensureInitialized()
{
    static QMutex initMutex;

    if (s_loadedCiphersAndCerts)
        return;
    const QMutexLocker locker(&initMutex);
    if (s_loadedCiphersAndCerts)
        return;
    s_loadedCiphersAndCerts = true;

    setDefaultCaCertificates(systemCaCertificates());
    s_loadRootCertsOnDemand = true; // setDefaultCaCertificates sets it to false, re-enable it.

    resetDefaultCiphers();
}

void QSslSocketPrivate::resetDefaultCiphers()
{
    setDefaultSupportedCiphers(QSslSocketBackendPrivate::defaultCiphers());
    setDefaultCiphers(QSslSocketBackendPrivate::defaultCiphers());
}

void QSslSocketPrivate::resetDefaultEllipticCurves()
{
    Q_UNIMPLEMENTED();
}

bool QSslSocketPrivate::supportsSsl()
{
    return true;
}

QList<QSslCertificate> QSslSocketPrivate::systemCaCertificates()
{
    // Copied from qsslsocket_openssl.cpp's systemCaCertificates function.
    QList<QSslCertificate> systemCerts;
    HCERTSTORE hSystemStore = CertOpenSystemStore(0, L"ROOT");
    if (hSystemStore) {
        PCCERT_CONTEXT pc = nullptr;
        while ((pc = CertFindCertificateInStore(hSystemStore, X509_ASN_ENCODING, 0, CERT_FIND_ANY,
                                                nullptr, pc))) {
            systemCerts.append(QSslCertificatePrivate::QSslCertificate_from_CERT_CONTEXT(pc));
        }
        CertCloseStore(hSystemStore, 0);
    }
    return systemCerts;
}

long QSslSocketPrivate::sslLibraryVersionNumber()
{
    const auto os = QOperatingSystemVersion::current();
    return (os.majorVersion() << 24) | ((os.minorVersion() & 0xFF) << 16) | (os.microVersion() & 0xFFFF);
}

QString QSslSocketPrivate::sslLibraryVersionString()
{
    const auto os = QOperatingSystemVersion::current();
    return QString::fromLatin1("Secure Channel, %1 %2.%3.%4")
            .arg(os.name(),
                 QString::number(os.majorVersion()),
                 QString::number(os.minorVersion()),
                 QString::number(os.microVersion()));
}

long QSslSocketPrivate::sslLibraryBuildVersionNumber()
{
    // There is no separate build version
    return sslLibraryVersionNumber();
}

QString QSslSocketPrivate::sslLibraryBuildVersionString()
{
    const auto os = QOperatingSystemVersion::current();
    return QString::fromLatin1("%1.%2.%3")
            .arg(QString::number(os.majorVersion()),
                 QString::number(os.minorVersion()),
                 QString::number(os.microVersion()));
}

QSslSocketBackendPrivate::QSslSocketBackendPrivate()
{
    SecInvalidateHandle(&credentialHandle);
    SecInvalidateHandle(&contextHandle);
    ensureInitialized();
}

QSslSocketBackendPrivate::~QSslSocketBackendPrivate()
{
    closeCertificateStores();
    deallocateContext();
    freeCredentialsHandle();
}

bool QSslSocketBackendPrivate::sendToken(void *token, unsigned long tokenLength, bool emitError)
{
    qint64 written = plainSocket->write(static_cast<const char *>(token), tokenLength);
    if (written < 0 || written != qint64(tokenLength)) {
        // Failed to write/buffer everything
        if (emitError)
            setErrorAndEmit(plainSocket->error(), plainSocket->errorString());
        return false;
    }
    return true;
}

QString QSslSocketBackendPrivate::targetName() const
{
    // Used for SNI extension
    return verificationPeerName.isEmpty() ? q_func()->peerName() : verificationPeerName;
}

ULONG QSslSocketBackendPrivate::getContextRequirements()
{
    const bool isClient = mode == QSslSocket::SslClientMode;
    ULONG req = 0;

    req |= ISC_REQ_ALLOCATE_MEMORY; // allocate memory for buffers automatically
    req |= ISC_REQ_CONFIDENTIALITY; // encrypt messages
    req |= ISC_REQ_REPLAY_DETECT; // detect replayed messages
    req |= ISC_REQ_SEQUENCE_DETECT; // detect out of sequence messages
    req |= ISC_REQ_STREAM; // Support a stream-oriented connection

    if (isClient) {
        req |= ISC_REQ_MANUAL_CRED_VALIDATION; // Manually validate certificate
    } else {
        switch (configuration.peerVerifyMode) {
        case QSslSocket::PeerVerifyMode::VerifyNone:
        // There doesn't seem to be a way to ask for an optional client cert :-(
        case QSslSocket::PeerVerifyMode::AutoVerifyPeer:
        case QSslSocket::PeerVerifyMode::QueryPeer:
            break;
        case QSslSocket::PeerVerifyMode::VerifyPeer:
            req |= ASC_REQ_MUTUAL_AUTH;
            break;
        }
    }

    return req;
}

bool QSslSocketBackendPrivate::acquireCredentialsHandle()
{
    const bool isClient = mode == QSslSocket::SslClientMode;
    const DWORD protocols = fromQtSslProtocol(configuration.protocol);
    if (protocols == SP_PROT_NONE) {
        setErrorAndEmit(QAbstractSocket::SslInvalidUserDataError,
                        QSslSocket::tr("Invalid protocol chosen"));
        return false;
    }

    const CERT_CHAIN_CONTEXT *chainContext = nullptr;
    const CERT_CONTEXT *localCert = nullptr;
    auto freeCertChain = qScopeGuard([&chainContext]() {
        if (chainContext)
            CertFreeCertificateChain(chainContext);
    });

    DWORD certsCount = 0;
    // Set up our certificate stores before trying to use one...
    initializeCertificateStores();

    // Check if user has specified a certificate chain but it could not be loaded.
    // This happens if there was something wrong with the certificate chain or there was no private
    // key.
    if (!configuration.localCertificateChain.isEmpty() && !localCertificateStore)
        return true; // 'true' because "tst_QSslSocket::setEmptyKey" expects us to not disconnect

    if (localCertificateStore != nullptr) {
        CERT_CHAIN_FIND_BY_ISSUER_PARA findParam;
        ZeroMemory(&findParam, sizeof(findParam));
        findParam.cbSize = sizeof(findParam);
        findParam.pszUsageIdentifier = isClient ? szOID_PKIX_KP_CLIENT_AUTH : szOID_PKIX_KP_SERVER_AUTH;

        // There should only be one chain in our store, so.. we grab that one.
        chainContext = CertFindChainInStore(localCertificateStore,
                                            X509_ASN_ENCODING,
                                            0,
                                            CERT_CHAIN_FIND_BY_ISSUER,
                                            &findParam,
                                            nullptr);
        if (!chainContext) {
            setErrorAndEmit(QAbstractSocket::SocketError::SslInvalidUserDataError,
                            QSslSocket::tr("The certificate provided can not be used for a %1.")
                                    .arg(isClient ? QSslSocket::tr("client")
                                                  : QSslSocket::tr("server")));
            return false;
        }
        localCert = chainContext->rgpChain[0]->rgpElement[0]->pCertContext;
        certsCount = 1;

        if (chainContext->rgpChain[0]->cElement > 1) {
            // @future: this works, but leaves the certificate in the system store
            HCERTSTORE myStore = CertOpenStore(CERT_STORE_PROV_SYSTEM, 0, 0,
                                               CERT_SYSTEM_STORE_CURRENT_USER | CERT_STORE_OPEN_EXISTING_FLAG,
                                               L"My");
            if (myStore) {
                for (DWORD i = 1; i < chainContext->rgpChain[0]->cElement; i++) {
                    CertAddCertificateContextToStore(myStore,
                                                     chainContext->rgpChain[0]->rgpElement[i]->pCertContext,
                                                     CERT_STORE_ADD_NEWER, nullptr);
                }
                CertCloseStore(myStore, 0);
            }
        }
    }

    SCHANNEL_CRED cred{
        SCHANNEL_CRED_VERSION, // dwVersion
        certsCount, // cCreds
        &localCert, // paCred (certificate(s) containing a private key for authentication)
        nullptr, // hRootStore

        0, // cMappers (reserved)
        nullptr, // aphMappers (reserved)

        0, // cSupportedAlgs
        nullptr, // palgSupportedAlgs (nullptr = system default) @future: QSslCipher-related

        protocols, // grbitEnabledProtocols
        0, // dwMinimumCipherStrength (0 = system default)
        0, // dwMaximumCipherStrength (0 = system default)
        0, // dwSessionLifespan (0 = schannel default, 10 hours)
        SCH_CRED_REVOCATION_CHECK_CHAIN_EXCLUDE_ROOT
                | SCH_CRED_NO_DEFAULT_CREDS, // dwFlags
        0 // dwCredFormat (must be 0)
    };

    TimeStamp expiration{};
    auto status = AcquireCredentialsHandle(nullptr, // pszPrincipal (unused)
                                           const_cast<wchar_t *>(UNISP_NAME), // pszPackage
                                           isClient ? SECPKG_CRED_OUTBOUND : SECPKG_CRED_INBOUND, // fCredentialUse
                                           nullptr, // pvLogonID (unused)
                                           &cred, // pAuthData
                                           nullptr, // pGetKeyFn (unused)
                                           nullptr, // pvGetKeyArgument (unused)
                                           &credentialHandle, // phCredential
                                           &expiration // ptsExpir
    );

    if (status != SEC_E_OK) {
        setErrorAndEmit(QAbstractSocket::SslInternalError, schannelError(status));
        return false;
    }
    return true;
}

void QSslSocketBackendPrivate::deallocateContext()
{
    if (SecIsValidHandle(&contextHandle)) {
        DeleteSecurityContext(&contextHandle);
        SecInvalidateHandle(&contextHandle);
    }
}

void QSslSocketBackendPrivate::freeCredentialsHandle()
{
    if (SecIsValidHandle(&credentialHandle)) {
        FreeCredentialsHandle(&credentialHandle);
        SecInvalidateHandle(&credentialHandle);
    }
}

void QSslSocketBackendPrivate::closeCertificateStores()
{
    if (localCertificateStore) {
        CertCloseStore(localCertificateStore, 0);
        localCertificateStore = nullptr;
    }
    if (peerCertificateStore) {
        CertCloseStore(peerCertificateStore, 0);
        peerCertificateStore = nullptr;
    }
    if (caCertificateStore) {
        CertCloseStore(caCertificateStore, 0);
        caCertificateStore = nullptr;
    }
}

bool QSslSocketBackendPrivate::createContext()
{
    Q_ASSERT(mode == QSslSocket::SslClientMode);
    ULONG contextReq = getContextRequirements();

    SecBuffer outBuffers[3];
    outBuffers[0] = createSecBuffer(nullptr, 0, SECBUFFER_TOKEN);
    outBuffers[1] = createSecBuffer(nullptr, 0, SECBUFFER_ALERT);
    outBuffers[2] = createSecBuffer(nullptr, 0, SECBUFFER_EMPTY);
    auto freeBuffers = qScopeGuard([&outBuffers]() {
        for (auto i = 0ull; i < ARRAYSIZE(outBuffers); i++) {
            if (outBuffers[i].pvBuffer)
                FreeContextBuffer(outBuffers[i].pvBuffer);
        }
    });
    SecBufferDesc outputBufferDesc{
        SECBUFFER_VERSION,
        ARRAYSIZE(outBuffers),
        outBuffers
    };

    TimeStamp expiry;

    auto status = InitializeSecurityContext(&credentialHandle, // phCredential
                                            nullptr, // phContext
                                            CONST_REINTERPRET_CAST(SEC_WCHAR *, targetName().utf16()), // pszTargetName
                                            contextReq, // fContextReq
                                            0, // Reserved1
                                            0, // TargetDataRep (unused)
                                            nullptr, // pInput (no input at the moment @future: alpn)
                                            0, // Reserved2
                                            &contextHandle, // phNewContext
                                            &outputBufferDesc, // pOutput
                                            &contextAttributes, // pfContextAttr
                                            &expiry // ptsExpiry
    );

    // This is the first call to InitializeSecurityContext, so theoretically "CONTINUE_NEEDED"
    // should be the only non-error return-code here.
    if (status != SEC_I_CONTINUE_NEEDED) {
        setErrorAndEmit(QAbstractSocket::SslInternalError,
                        QSslSocket::tr("Error creating SSL context (%1)").arg(schannelError(status)));
        return false;
    }

    if (!sendToken(outBuffers[0].pvBuffer, outBuffers[0].cbBuffer))
        return false;
    schannelState = SchannelState::PerformHandshake;
    return true;
}

bool QSslSocketBackendPrivate::acceptContext()
{
    Q_ASSERT(mode == QSslSocket::SslServerMode);
    ULONG contextReq = getContextRequirements();

    intermediateBuffer += plainSocket->readAll();
    if (intermediateBuffer.isEmpty())
        return true; // definitely need more data..

    SecBuffer inBuffers[2];
    inBuffers[0] = createSecBuffer(intermediateBuffer, SECBUFFER_TOKEN);
    inBuffers[1] = createSecBuffer(nullptr, 0, SECBUFFER_EMPTY);
    SecBufferDesc inputBufferDesc{
        SECBUFFER_VERSION,
        ARRAYSIZE(inBuffers),
        inBuffers
    };

    SecBuffer outBuffers[3];
    outBuffers[0] = createSecBuffer(nullptr, 0, SECBUFFER_TOKEN);
    outBuffers[1] = createSecBuffer(nullptr, 0, SECBUFFER_ALERT);
    outBuffers[2] = createSecBuffer(nullptr, 0, SECBUFFER_EMPTY);
    auto freeBuffers = qScopeGuard([&outBuffers]() {
        for (auto i = 0ull; i < ARRAYSIZE(outBuffers); i++) {
            if (outBuffers[i].pvBuffer)
                FreeContextBuffer(outBuffers[i].pvBuffer);
        }
    });
    SecBufferDesc outputBufferDesc{
        SECBUFFER_VERSION,
        ARRAYSIZE(outBuffers),
        outBuffers
    };

    TimeStamp expiry;
    auto status = AcceptSecurityContext(
            &credentialHandle, // phCredential
            nullptr, // phContext
            &inputBufferDesc, // pInput
            contextReq, // fContextReq
            0, // TargetDataRep (unused)
            &contextHandle, // phNewContext
            &outputBufferDesc, // pOutput
            &contextAttributes, // pfContextAttr
            &expiry // ptsTimeStamp
    );

    if (inBuffers[1].BufferType == SECBUFFER_EXTRA) {
        // https://docs.microsoft.com/en-us/windows/desktop/secauthn/extra-buffers-returned-by-schannel
        // inBuffers[1].cbBuffer indicates the amount of bytes _NOT_ processed, the rest need to
        // be stored.
        intermediateBuffer = intermediateBuffer.right(int(inBuffers[1].cbBuffer));
    } else if (status != SEC_E_INCOMPLETE_MESSAGE) {
        intermediateBuffer.clear();
    }

    if (status != SEC_I_CONTINUE_NEEDED) {
        setErrorAndEmit(QAbstractSocket::SslHandshakeFailedError,
                        QSslSocket::tr("Error creating SSL context (%1)").arg(schannelError(status)));
        return false;
    }
    if (!sendToken(outBuffers[0].pvBuffer, outBuffers[0].cbBuffer))
        return false;
    schannelState = SchannelState::PerformHandshake;
    return true;
}

bool QSslSocketBackendPrivate::performHandshake()
{
    if (plainSocket->state() == QAbstractSocket::UnconnectedState) {
        setErrorAndEmit(QAbstractSocket::RemoteHostClosedError,
                        QSslSocket::tr("The TLS/SSL connection has been closed"));
        return false;
    }
#ifdef QSSLSOCKET_DEBUG
    qCDebug(lcSsl) << "Bytes available from socket:" << plainSocket->bytesAvailable();
    qCDebug(lcSsl) << "intermediateBuffer size:" << intermediateBuffer.size();
#endif

    intermediateBuffer += plainSocket->readAll();
    if (intermediateBuffer.isEmpty())
        return true; // no data, will fail

    SecBuffer inputBuffers[2];
    inputBuffers[0] = createSecBuffer(intermediateBuffer, SECBUFFER_TOKEN);
    inputBuffers[1] = createSecBuffer(nullptr, 0, SECBUFFER_EMPTY);
    SecBufferDesc inputBufferDesc{
        SECBUFFER_VERSION,
        ARRAYSIZE(inputBuffers),
        inputBuffers
    };

    SecBuffer outBuffers[3];
    outBuffers[0] = createSecBuffer(nullptr, 0, SECBUFFER_TOKEN);
    outBuffers[1] = createSecBuffer(nullptr, 0, SECBUFFER_ALERT);
    outBuffers[2] = createSecBuffer(nullptr, 0, SECBUFFER_EMPTY);
    auto freeBuffers = qScopeGuard([&outBuffers]() {
        for (auto i = 0ull; i < ARRAYSIZE(outBuffers); i++) {
            if (outBuffers[i].pvBuffer)
                FreeContextBuffer(outBuffers[i].pvBuffer);
        }
    });
    SecBufferDesc outputBufferDesc{
        SECBUFFER_VERSION,
        ARRAYSIZE(outBuffers),
        outBuffers
    };

    ULONG contextReq = getContextRequirements();
    TimeStamp expiry;
    auto status = InitializeSecurityContext(&credentialHandle, // phCredential
                                            &contextHandle, // phContext
                                            CONST_REINTERPRET_CAST(SEC_WCHAR *, targetName().utf16()), // pszTargetName
                                            contextReq, // fContextReq
                                            0, // Reserved1
                                            0, // TargetDataRep (unused)
                                            &inputBufferDesc, // pInput
                                            0, // Reserved2
                                            nullptr, // phNewContext (we already have one)
                                            &outputBufferDesc, // pOutput
                                            &contextAttributes, // pfContextAttr
                                            &expiry // ptsExpiry
    );

    if (inputBuffers[1].BufferType == SECBUFFER_EXTRA) {
        // https://docs.microsoft.com/en-us/windows/desktop/secauthn/extra-buffers-returned-by-schannel
        // inputBuffers[1].cbBuffer indicates the amount of bytes _NOT_ processed, the rest need to
        // be stored.
        intermediateBuffer = intermediateBuffer.right(int(inputBuffers[1].cbBuffer));
    } else {
        // Clear the buffer if we weren't asked for more data
        if (status != SEC_E_INCOMPLETE_MESSAGE)
            intermediateBuffer.clear();
    }
    switch (status) {
    case SEC_E_OK:
        // Need to transmit a final token in the handshake if 'cbBuffer' is non-zero.
        if (!sendToken(outBuffers[0].pvBuffer, outBuffers[0].cbBuffer))
            return false;
        schannelState = SchannelState::VerifyHandshake;
        return true;
    case SEC_I_CONTINUE_NEEDED:
        if (!sendToken(outBuffers[0].pvBuffer, outBuffers[0].cbBuffer))
            return false;
        // Must call InitializeSecurityContext again later (done through continueHandshake)
        return true;
    case SEC_I_INCOMPLETE_CREDENTIALS:
        // Schannel takes care of picking certificate to send (other than the one we can specify),
        // so if we get here then that means we don't have a certificate the server accepts.
        setErrorAndEmit(QAbstractSocket::SslHandshakeFailedError,
                        QSslSocket::tr("Server did not accept any certificate we could present."));
        return false;
    case SEC_I_CONTEXT_EXPIRED:
        // "The message sender has finished using the connection and has initiated a shutdown."
        if (outBuffers[0].BufferType == SECBUFFER_TOKEN) {
            if (!sendToken(outBuffers[0].pvBuffer, outBuffers[0].cbBuffer))
                return false;
        }
        if (!shutdown) { // we did not initiate this
            setErrorAndEmit(QAbstractSocket::RemoteHostClosedError,
                            QSslSocket::tr("The TLS/SSL connection has been closed"));
        }
        return true;
    case SEC_E_INCOMPLETE_MESSAGE:
        // Simply incomplete, wait for more data
        return true;
    case SEC_E_ALGORITHM_MISMATCH:
        setErrorAndEmit(QAbstractSocket::SslHandshakeFailedError,
                        QSslSocket::tr("Algorithm mismatch"));
        shutdown = true; // skip sending the "Shutdown" alert
        return false;
    }

    // Note: We can get here if the connection is using TLS 1.2 and the server certificate uses
    // MD5, which is not allowed in Schannel. This causes an "invalid token" error during handshake.
    // (If you came here investigating an error: md5 is insecure, update your certificate)
    setErrorAndEmit(QAbstractSocket::SslHandshakeFailedError,
                    QSslSocket::tr("Handshake failed: %1").arg(schannelError(status)));
    return false;
}

bool QSslSocketBackendPrivate::verifyHandshake()
{
    Q_Q(QSslSocket);

    const bool isClient = mode == QSslSocket::SslClientMode;
#define CHECK_STATUS(status)                                                  \
    if (status != SEC_E_OK) {                                                 \
        setErrorAndEmit(QAbstractSocket::SslInternalError,                    \
                        QSslSocket::tr("Failed to query the TLS context: %1") \
                                .arg(schannelError(status)));                 \
        return false;                                                         \
    }

    // Everything is set up, now make sure there's nothing wrong and query some attributes...
    if (!matchesContextRequirements(contextAttributes, getContextRequirements(),
                                    configuration.peerVerifyMode, isClient)) {
        setErrorAndEmit(QAbstractSocket::SslHandshakeFailedError,
                        QSslSocket::tr("Did not get the required attributes for the connection."));
        return false;
    }

    // Get stream sizes (to know the max size of a message and the size of the header and trailer)
    auto status = QueryContextAttributes(&contextHandle,
                                         SECPKG_ATTR_STREAM_SIZES,
                                         &streamSizes);
    CHECK_STATUS(status);

    // Get session cipher info
    status = QueryContextAttributes(&contextHandle,
                                    SECPKG_ATTR_CONNECTION_INFO,
                                    &connectionInfo);
    CHECK_STATUS(status);
#undef CHECK_STATUS

    // Verify certificate
    CERT_CONTEXT *certificateContext = nullptr;
    auto freeCertificate = qScopeGuard([&certificateContext]() {
        if (certificateContext)
            CertFreeCertificateContext(certificateContext);
    });
    status = QueryContextAttributes(&contextHandle,
                                    SECPKG_ATTR_REMOTE_CERT_CONTEXT,
                                    &certificateContext);

    // QueryPeer can (currently) not work in Schannel since Schannel itself doesn't have a way to
    // ask for a certificate and then still be OK if it's not received.
    // To work around this we don't request a certificate at all for QueryPeer.
    // For servers AutoVerifyPeer is supposed to be treated the same as QueryPeer.
    // This means that servers using Schannel will only request client certificate for "VerifyPeer".
    if ((!isClient && configuration.peerVerifyMode == QSslSocket::PeerVerifyMode::VerifyPeer)
        || (isClient && configuration.peerVerifyMode != QSslSocket::PeerVerifyMode::VerifyNone
            && configuration.peerVerifyMode != QSslSocket::PeerVerifyMode::QueryPeer)) {
        if (status != SEC_E_OK) {
#ifdef QSSLSOCKET_DEBUG
            qCDebug(lcSsl) << "Couldn't retrieve peer certificate, status:"
                           << schannelError(status);
#endif
            const QSslError error{ QSslError::NoPeerCertificate };
            sslErrors += error;
            emit q->peerVerifyError(error);
            if (q->state() != QAbstractSocket::ConnectedState)
                return false;
        }
    }

    // verifyCertContext returns false if the user disconnected while it was checking errors.
    if (certificateContext && sslErrors.isEmpty() && !verifyCertContext(certificateContext))
        return false;

    if (!checkSslErrors() || state != QAbstractSocket::ConnectedState) {
#ifdef QSSLSOCKET_DEBUG
        qCDebug(lcSsl) << __func__ << "was unsuccessful. Paused:" << paused;
#endif
        // If we're paused then checkSslErrors returned false, but it's not an error
        return paused && state == QAbstractSocket::ConnectedState;
    }

    schannelState = SchannelState::Done;
    peerCertVerified = true;
    return true;
}

bool QSslSocketBackendPrivate::renegotiate()
{
    SecBuffer outBuffers[3];
    outBuffers[0] = createSecBuffer(nullptr, 0, SECBUFFER_TOKEN);
    outBuffers[1] = createSecBuffer(nullptr, 0, SECBUFFER_ALERT);
    outBuffers[2] = createSecBuffer(nullptr, 0, SECBUFFER_EMPTY);
    auto freeBuffers = qScopeGuard([&outBuffers]() {
        for (auto i = 0ull; i < ARRAYSIZE(outBuffers); i++) {
            if (outBuffers[i].pvBuffer)
                FreeContextBuffer(outBuffers[i].pvBuffer);
        }
    });
    SecBufferDesc outputBufferDesc{
        SECBUFFER_VERSION,
        ARRAYSIZE(outBuffers),
        outBuffers
    };

    ULONG contextReq = getContextRequirements();
    TimeStamp expiry;
    SECURITY_STATUS status;
    if (mode == QSslSocket::SslClientMode) {
        status = InitializeSecurityContext(&credentialHandle, // phCredential
                                           &contextHandle, // phContext
                                           CONST_REINTERPRET_CAST(SEC_WCHAR *, targetName().utf16()), // pszTargetName
                                           contextReq, // fContextReq
                                           0, // Reserved1
                                           0, // TargetDataRep (unused)
                                           nullptr, // pInput (nullptr for renegotiate)
                                           0, // Reserved2
                                           nullptr, // phNewContext (we already have one)
                                           &outputBufferDesc, // pOutput
                                           &contextAttributes, // pfContextAttr
                                           &expiry // ptsExpiry
        );
    } else {
        status = AcceptSecurityContext(
                &credentialHandle, // phCredential
                &contextHandle, // phContext
                nullptr, // pInput
                contextReq, // fContextReq
                0, // TargetDataRep (unused)
                nullptr, // phNewContext
                &outputBufferDesc, // pOutput
                &contextAttributes, // pfContextAttr,
                &expiry // ptsTimeStamp
        );
    }
    if (status == SEC_I_CONTINUE_NEEDED) {
        schannelState = SchannelState::PerformHandshake;
        return sendToken(outBuffers[0].pvBuffer, outBuffers[0].cbBuffer);
    }
    setErrorAndEmit(QAbstractSocket::SslHandshakeFailedError,
                    QSslSocket::tr("Renegotiation was unsuccessful: %1").arg(schannelError(status)));
    return false;
}

/*!
    \internal
    reset the state in preparation for reuse of socket
*/
void QSslSocketBackendPrivate::reset()
{
    closeCertificateStores(); // certificate stores could've changed
    deallocateContext();
    freeCredentialsHandle(); // in case we already had one (@future: session resumption requires re-use)

    connectionInfo = {};
    streamSizes = {};

    contextAttributes = 0;
    intermediateBuffer.clear();
    schannelState = SchannelState::InitializeHandshake;

    connectionEncrypted = false;
    shutdown = false;
    peerCertVerified = false;
    renegotiating = false;
}

void QSslSocketBackendPrivate::startClientEncryption()
{
    if (connectionEncrypted)
        return; // let's not mess up the connection...
    reset();
    continueHandshake();
}

void QSslSocketBackendPrivate::startServerEncryption()
{
    if (connectionEncrypted)
        return; // let's not mess up the connection...
    reset();
    continueHandshake();
}

void QSslSocketBackendPrivate::transmit()
{
    Q_Q(QSslSocket);

    if (schannelState != SchannelState::Done) {
        continueHandshake();
        return;
    }

    if (connectionEncrypted) { // encrypt data in writeBuffer and write it to plainSocket
        qint64 totalBytesWritten = 0;
        qint64 writeBufferSize;
        while ((writeBufferSize = writeBuffer.size()) > 0) {
            QByteArray plaintext;
            {
                // Try to read 'cbMaximumMessage' bytes from buffer before encrypting.
                int size = int(std::min(writeBufferSize, qint64(streamSizes.cbMaximumMessage)));
                plaintext.resize(size);
                // Use peek() here instead of read() so we don't lose data if encryption fails.
                qint64 copied = writeBuffer.peek(plaintext.data(), size);
                Q_ASSERT(copied == size);
            }
            QByteArray header(int(streamSizes.cbHeader), '\0');
            QByteArray trailer(int(streamSizes.cbTrailer), '\0');

            SecBuffer inputBuffers[4]{
                // @future[0/1]: optimize by using one container for all fields...
                createSecBuffer(header, SECBUFFER_STREAM_HEADER),
                createSecBuffer(plaintext, SECBUFFER_DATA),
                createSecBuffer(trailer, SECBUFFER_STREAM_TRAILER),
                createSecBuffer(nullptr, 0, SECBUFFER_EMPTY)
            };
            SecBufferDesc message{
                SECBUFFER_VERSION,
                ARRAYSIZE(inputBuffers),
                inputBuffers
            };
            auto status = EncryptMessage(&contextHandle, 0, &message, 0);
            if (status != SEC_E_OK) {
                setErrorAndEmit(QAbstractSocket::SslInternalError,
                                QSslSocket::tr("Schannel failed to encrypt data: %1")
                                        .arg(schannelError(status)));
                return;
            }
            // Data was encrypted successfully, so we free() what we peek()ed earlier
            writeBuffer.free(plaintext.length());

            // trailer has been observed to change size, so resize them all (when needed) to be safe
            header = header.left(int(inputBuffers[0].cbBuffer));
            plaintext = plaintext.left(int(inputBuffers[1].cbBuffer));
            trailer = trailer.left(int(inputBuffers[2].cbBuffer));
            const qint64 bytesWritten = plainSocket->write(header // @future[1/1]: ...because they need to be merged
                                                           + plaintext
                                                           + trailer);
#ifdef QSSLSOCKET_DEBUG
            qCDebug(lcSsl) << "Wrote" << bytesWritten << "of total"
                           << header.length() + plaintext.length() + trailer.length() << "bytes";
#endif
            if (bytesWritten >= 0) {
                totalBytesWritten += bytesWritten;
            } else {
                setErrorAndEmit(plainSocket->error(), plainSocket->errorString());
                return;
            }
        }

        if (totalBytesWritten > 0) {
            // Don't emit bytesWritten() recursively.
            if (!emittedBytesWritten) {
                emittedBytesWritten = true;
                emit q->bytesWritten(totalBytesWritten);
                emittedBytesWritten = false;
            }
            emit q->channelBytesWritten(0, totalBytesWritten);
        }
    }

    if (connectionEncrypted) { // Decrypt data from remote
        int totalRead = 0;
        bool hadIncompleteData = false;
        while (!readBufferMaxSize || buffer.size() < readBufferMaxSize) {
            QByteArray ciphertext;
            if (intermediateBuffer.length()) {
#ifdef QSSLSOCKET_DEBUG
                qCDebug(lcSsl) << "Restoring data from intermediateBuffer:"
                               << intermediateBuffer.length() << "bytes";
#endif
                ciphertext.swap(intermediateBuffer);
            }
            int initialLength = ciphertext.length();
            ciphertext += plainSocket->read(16384);
#ifdef QSSLSOCKET_DEBUG
            qCDebug(lcSsl) << "Read" << ciphertext.length() - initialLength
                           << "encrypted bytes from the socket";
#endif
            if (ciphertext.length() == 0 || (hadIncompleteData && initialLength == ciphertext.length())) {
#ifdef QSSLSOCKET_DEBUG
                qCDebug(lcSsl) << (hadIncompleteData ? "No new data received, leaving loop!"
                                                     : "Nothing to decrypt, leaving loop!");
#endif
                if (ciphertext.length()) // We have data, it came from intermediateBuffer, swap back
                    intermediateBuffer.swap(ciphertext);
                break;
            }
            hadIncompleteData = false;
#ifdef QSSLSOCKET_DEBUG
            qCDebug(lcSsl) << "Total amount of bytes to decrypt:" << ciphertext.length();
#endif

            SecBuffer dataBuffer[4]{
                createSecBuffer(ciphertext, SECBUFFER_DATA),
                createSecBuffer(nullptr, 0, SECBUFFER_EMPTY),
                createSecBuffer(nullptr, 0, SECBUFFER_EMPTY),
                createSecBuffer(nullptr, 0, SECBUFFER_EMPTY)
            };
            SecBufferDesc message{
                SECBUFFER_VERSION,
                ARRAYSIZE(dataBuffer),
                dataBuffer
            };
            auto status = DecryptMessage(&contextHandle, &message, 0, nullptr);
            if (status == SEC_E_OK || status == SEC_I_RENEGOTIATE || status == SEC_I_CONTEXT_EXPIRED) {
                // There can still be 0 output even if it succeeds, this is fine
                if (dataBuffer[1].cbBuffer > 0) {
                    // It is always decrypted in-place.
                    // But [0] is the STREAM_HEADER, [1] is the DATA and [2] is the STREAM_TRAILER.
                    // The pointers in all of those still point into the 'ciphertext' byte array.
                    buffer.append(static_cast<char *>(dataBuffer[1].pvBuffer),
                                  dataBuffer[1].cbBuffer);
                    totalRead += dataBuffer[1].cbBuffer;
#ifdef QSSLSOCKET_DEBUG
                    qCDebug(lcSsl) << "Decrypted" << dataBuffer[1].cbBuffer
                                   << "bytes. New read buffer size:" << buffer.size();
#endif
                }
                if (dataBuffer[3].BufferType == SECBUFFER_EXTRA) {
                    // https://docs.microsoft.com/en-us/windows/desktop/secauthn/extra-buffers-returned-by-schannel
                    // dataBuffer[3].cbBuffer indicates the amount of bytes _NOT_ processed,
                    // the rest need to be stored.
#ifdef QSSLSOCKET_DEBUG
                    qCDebug(lcSsl) << "We've got excess data, moving it to the intermediate buffer:"
                                   << dataBuffer[3].cbBuffer << "bytes";
#endif
                    intermediateBuffer = ciphertext.right(int(dataBuffer[3].cbBuffer));
                }
            } else if (status == SEC_E_INCOMPLETE_MESSAGE) {
                // Need more data before we can decrypt.. to the buffer it goes!
#ifdef QSSLSOCKET_DEBUG
                qCDebug(lcSsl, "We didn't have enough data to decrypt anything, will try again!");
#endif
                Q_ASSERT(intermediateBuffer.isEmpty());
                intermediateBuffer.swap(ciphertext);
                // We try again, but if we don't get any more data then we leave
                hadIncompleteData = true;
            } else if (status == SEC_E_INVALID_HANDLE) {
                // I don't think this should happen, if it does we're done...
                qCWarning(lcSsl, "The internal SSPI handle is invalid!");
                Q_UNREACHABLE();
            } else if (status == SEC_E_INVALID_TOKEN) {
                qCWarning(lcSsl, "Got SEC_E_INVALID_TOKEN!");
                Q_UNREACHABLE(); // Happened once due to a bug, but shouldn't generally happen(?)
            } else if (status == SEC_E_MESSAGE_ALTERED) {
                // The message has been altered, disconnect now.
                // According to the Internet it also triggers for messages that are out of order.
                // https://microsoft.public.platformsdk.security.narkive.com/4JAvlMvD/help-please-schannel-security-contexts-and-decryptmessage
                shutdown = true; // skips sending the shutdown alert
                disconnectFromHost();
                setErrorAndEmit(QAbstractSocket::SslInternalError,
                                QSslSocket::tr("The message was tampered with, damaged or out of sequence."));
                break;
            } else if (status == SEC_E_OUT_OF_SEQUENCE) {
                // @todo: I don't know if this one is actually "fatal"..
                // This path might never be hit as it seems this is for connection-oriented connections,
                // while SEC_E_MESSAGE_ALTERED is for stream-oriented ones (what we use).
                shutdown = true; // skips sending the shutdown alert
                disconnectFromHost();
                setErrorAndEmit(QAbstractSocket::SslInternalError,
                                QSslSocket::tr("A message was received out of sequence."));
                break;
            } else if (status == SEC_I_CONTEXT_EXPIRED) {
                // 'remote' has initiated a shutdown
                disconnectFromHost();
                setErrorAndEmit(QAbstractSocket::RemoteHostClosedError,
                                QSslSocket::tr("The TLS/SSL connection has been closed"));
                break;
            } else if (status == SEC_I_RENEGOTIATE) {
                // 'remote' wants to renegotiate
#ifdef QSSLSOCKET_DEBUG
                qCDebug(lcSsl, "The peer wants to renegotiate.");
#endif
                schannelState = SchannelState::Renegotiate;
                renegotiating = true;
                // We need to call 'continueHandshake' or else there's no guarantee it ever gets called
                continueHandshake();
                break;
            }
        }

        if (totalRead) {
            if (readyReadEmittedPointer)
                *readyReadEmittedPointer = true;
            emit q->readyRead();
            emit q->channelReadyRead(0);
        }
    }
}

void QSslSocketBackendPrivate::sendShutdown()
{
    const bool isClient = mode == QSslSocket::SslClientMode;
    DWORD shutdownToken = SCHANNEL_SHUTDOWN;
    SecBuffer buffer = createSecBuffer(&shutdownToken, sizeof(SCHANNEL_SHUTDOWN), SECBUFFER_TOKEN);
    SecBufferDesc token{
        SECBUFFER_VERSION,
        1,
        &buffer
    };
    auto status = ApplyControlToken(&contextHandle, &token);

    if (status != SEC_E_OK) {
#ifdef QSSLSOCKET_DEBUG
        qCDebug(lcSsl) << "Failed to apply shutdown control token:" << schannelError(status);
#endif
        return;
    }

    SecBuffer outBuffers[3];
    outBuffers[0] = createSecBuffer(nullptr, 0, SECBUFFER_TOKEN);
    outBuffers[1] = createSecBuffer(nullptr, 0, SECBUFFER_ALERT);
    outBuffers[2] = createSecBuffer(nullptr, 0, SECBUFFER_EMPTY);
    auto freeBuffers = qScopeGuard([&outBuffers]() {
        for (auto i = 0ull; i < ARRAYSIZE(outBuffers); i++) {
            if (outBuffers[i].pvBuffer)
                FreeContextBuffer(outBuffers[i].pvBuffer);
        }
    });
    SecBufferDesc outputBufferDesc{
        SECBUFFER_VERSION,
        ARRAYSIZE(outBuffers),
        outBuffers
    };

    ULONG contextReq = getContextRequirements();
    TimeStamp expiry;
    if (isClient) {
        status = InitializeSecurityContext(&credentialHandle, // phCredential
                                           &contextHandle, // phContext
                                           CONST_REINTERPRET_CAST(SEC_WCHAR *, targetName().utf16()), // pszTargetName
                                           contextReq, // fContextReq
                                           0, // Reserved1
                                           0, // TargetDataRep (unused)
                                           nullptr, // pInput
                                           0, // Reserved2
                                           nullptr, // phNewContext (we already have one)
                                           &outputBufferDesc, // pOutput
                                           &contextAttributes, // pfContextAttr
                                           &expiry // ptsExpiry
        );
    } else {
        status = AcceptSecurityContext(
                &credentialHandle, // phCredential
                &contextHandle, // phContext
                nullptr, // pInput
                contextReq, // fContextReq
                0, // TargetDataRep (unused)
                nullptr, // phNewContext
                &outputBufferDesc, // pOutput
                &contextAttributes, // pfContextAttr,
                &expiry // ptsTimeStamp
        );
    }
    if (status == SEC_E_OK || status == SEC_I_CONTEXT_EXPIRED) {
        if (!sendToken(outBuffers[0].pvBuffer, outBuffers[0].cbBuffer, false)) {
            // We failed to send the shutdown message, but it's not that important since we're
            // shutting down anyway.
            return;
        }

        schannelState = SchannelState::PerformHandshake;
        continueHandshake();
    } else {
#ifdef QSSLSOCKET_DEBUG
        qCDebug(lcSsl) << "Failed to initialize shutdown:" << schannelError(status);
#endif
    }
}

void QSslSocketBackendPrivate::disconnectFromHost()
{
    if (SecIsValidHandle(&contextHandle)) {
        if (!shutdown) {
            shutdown = true;
            if (plainSocket->state() != QAbstractSocket::UnconnectedState) {
                if (connectionEncrypted) {
                    // Read as much as possible because this is likely our last chance
                    qint64 tempMax = readBufferMaxSize;
                    readBufferMaxSize = 0;
                    transmit();
                    readBufferMaxSize = tempMax;
                }

                sendShutdown();
            }
        }
    }
    if (plainSocket->state() != QAbstractSocket::UnconnectedState)
        plainSocket->disconnectFromHost();
}

void QSslSocketBackendPrivate::disconnected()
{
    if (plainSocket->bytesAvailable() <= 0) {
        shutdown = true;
        connectionEncrypted = false;
        deallocateContext();
        freeCredentialsHandle();
    }
}

QSslCipher QSslSocketBackendPrivate::sessionCipher() const
{
    if (!connectionEncrypted)
        return QSslCipher();
    return QSslCipher(QStringLiteral("Schannel"), sessionProtocol());
}

QSsl::SslProtocol QSslSocketBackendPrivate::sessionProtocol() const
{
    if (!connectionEncrypted)
        return QSsl::SslProtocol::UnknownProtocol;
    return toQtSslProtocol(connectionInfo.dwProtocol);
}

void QSslSocketBackendPrivate::continueHandshake()
{
    Q_Q(QSslSocket);
    const bool isServer = mode == QSslSocket::SslServerMode;
    switch (schannelState) {
    case SchannelState::InitializeHandshake:
        if (!SecIsValidHandle(&credentialHandle) && !acquireCredentialsHandle()) {
            disconnectFromHost();
            return;
        }
        if (!SecIsValidHandle(&credentialHandle)) // Needed to support tst_QSslSocket::setEmptyKey
            return;
        if (!SecIsValidHandle(&contextHandle) && !(isServer ? acceptContext() : createContext())) {
            disconnectFromHost();
            return;
        }
        if (schannelState != SchannelState::PerformHandshake)
            break;
        Q_FALLTHROUGH();
    case SchannelState::PerformHandshake:
        if (!performHandshake()) {
            disconnectFromHost();
            return;
        }
        if (schannelState != SchannelState::VerifyHandshake)
            break;
        Q_FALLTHROUGH();
    case SchannelState::VerifyHandshake:
        // if we're in shutdown or renegotiating then we might not need to verify
        // (since we already did)
        if (!peerCertVerified && !verifyHandshake()) {
            shutdown = true; // Skip sending shutdown alert
            q->abort(); // We don't want to send buffered data
            disconnectFromHost();
            return;
        }
        if (schannelState != SchannelState::Done)
            break;
        Q_FALLTHROUGH();
    case SchannelState::Done:
        // connectionEncrypted is already true if we come here from a renegotiation
        if (!connectionEncrypted) {
            connectionEncrypted = true; // all is done
            emit q->encrypted();
        }
        renegotiating = false;
        if (pendingClose) {
            pendingClose = false;
            disconnectFromHost();
        } else {
            transmit();
        }
        break;
    case SchannelState::Renegotiate:
        if (!renegotiate()) {
            disconnectFromHost();
            return;
        }
        break;
    }
}

QList<QSslCipher> QSslSocketBackendPrivate::defaultCiphers()
{
    QList<QSslCipher> ciphers;
    // @temp (I hope), stolen from qsslsocket_winrt.cpp
    const QString protocolStrings[] = { QStringLiteral("TLSv1"), QStringLiteral("TLSv1.1"),
                                        QStringLiteral("TLSv1.2"), QStringLiteral("TLSv1.3") };
    const QSsl::SslProtocol protocols[] = { QSsl::TlsV1_0, QSsl::TlsV1_1,
                                            QSsl::TlsV1_2, QSsl::TlsV1_3 };
    const int size = ARRAYSIZE(protocols);
    Q_STATIC_ASSERT(size == ARRAYSIZE(protocolStrings));
    ciphers.reserve(size);
    for (int i = 0; i < size; ++i) {
        QSslCipher cipher;
        cipher.d->isNull = false;
        cipher.d->name = QStringLiteral("Schannel");
        cipher.d->protocol = protocols[i];
        cipher.d->protocolString = protocolStrings[i];
        ciphers.append(cipher);
    }

    return ciphers;
}

QList<QSslError> QSslSocketBackendPrivate::verify(const QList<QSslCertificate> &certificateChain,
                                                  const QString &hostName)
{
    Q_UNUSED(certificateChain);
    Q_UNUSED(hostName);

    Q_UNIMPLEMENTED();
    return {}; // @future implement(?)
}

bool QSslSocketBackendPrivate::importPkcs12(QIODevice *device, QSslKey *key, QSslCertificate *cert,
                                            QList<QSslCertificate> *caCertificates,
                                            const QByteArray &passPhrase)
{
    Q_UNUSED(device);
    Q_UNUSED(key);
    Q_UNUSED(cert);
    Q_UNUSED(caCertificates);
    Q_UNUSED(passPhrase);
    // @future: can load into its own certificate store (encountered problems extracting key).
    Q_UNIMPLEMENTED();
    return false;
}

/*
    Copied from qsslsocket_mac.cpp, which was copied from qsslsocket_openssl.cpp
*/
bool QSslSocketBackendPrivate::checkSslErrors()
{
    if (sslErrors.isEmpty())
        return true;
    Q_Q(QSslSocket);

    emit q->sslErrors(sslErrors);

    const bool doVerifyPeer = configuration.peerVerifyMode == QSslSocket::VerifyPeer
            || (configuration.peerVerifyMode == QSslSocket::AutoVerifyPeer
                && mode == QSslSocket::SslClientMode);
    const bool doEmitSslError = !verifyErrorsHaveBeenIgnored();
    // check whether we need to emit an SSL handshake error
    if (doVerifyPeer && doEmitSslError) {
        if (q->pauseMode() & QAbstractSocket::PauseOnSslErrors) {
            pauseSocketNotifiers(q);
            paused = true;
        } else {
            setErrorAndEmit(QAbstractSocket::SslHandshakeFailedError,
                            sslErrors.constFirst().errorString());
            plainSocket->disconnectFromHost();
        }
        return false;
    }

    return true;
}

void QSslSocketBackendPrivate::initializeCertificateStores()
{
    //// helper function which turns a chain into a certificate store
    auto createStoreFromCertificateChain = [](const QList<QSslCertificate> certChain, const QSslKey &privateKey) {
        const wchar_t *passphrase = L"";
        // Need to embed the private key in the certificate
        QByteArray pkcs12 = _q_makePkcs12(certChain,
                                          privateKey,
                                          QString::fromWCharArray(passphrase, 0));
        CRYPT_DATA_BLOB pfxBlob;
        pfxBlob.cbData = DWORD(pkcs12.length());
        pfxBlob.pbData = reinterpret_cast<unsigned char *>(pkcs12.data());
        return PFXImportCertStore(&pfxBlob, passphrase, 0); // returns HCERTSTORE
    };

    if (!configuration.localCertificateChain.isEmpty()) {
        if (configuration.privateKey.isNull()) {
            setErrorAndEmit(QAbstractSocket::SslInvalidUserDataError,
                            QSslSocket::tr("Cannot provide a certificate with no key"));
            return;
        }
        if (localCertificateStore == nullptr) {
            localCertificateStore = createStoreFromCertificateChain(configuration.localCertificateChain,
                                                                    configuration.privateKey);
            if (localCertificateStore == nullptr)
                qCWarning(lcSsl, "Failed to load certificate chain!");
        }
    }

    if (!configuration.caCertificates.isEmpty() && !caCertificateStore) {
        caCertificateStore = createStoreFromCertificateChain(configuration.caCertificates,
                                                             {}); // No private key for the CA certs
    }
}

bool QSslSocketBackendPrivate::verifyCertContext(CERT_CONTEXT *certContext)
{
    Q_ASSERT(certContext);
    Q_Q(QSslSocket);

    const bool isClient = mode == QSslSocket::SslClientMode;

    // Create a collection of stores so we can pass in multiple stores as additional locations to
    // search for the certificate chain
    HCERTSTORE tempCertCollection = CertOpenStore(CERT_STORE_PROV_COLLECTION,
                                                  X509_ASN_ENCODING,
                                                  0,
                                                  CERT_STORE_CREATE_NEW_FLAG,
                                                  nullptr);

    if (rootCertOnDemandLoadingAllowed()) {
        // @future(maybe): following the OpenSSL backend these certificates should be added into
        // the Ca list, not just included during verification.
        // That being said, it's not trivial to add the root certificates (if and only if they
        // came from the system root store). And I don't see this mentioned in our documentation.
        HCERTSTORE rootStore = CertOpenSystemStore(0, L"ROOT");
        if (rootStore) {
            CertAddStoreToCollection(tempCertCollection, rootStore, 0, 1);
            CertCloseStore(rootStore, 0);
        }
    }
    if (caCertificateStore)
        CertAddStoreToCollection(tempCertCollection, caCertificateStore, 0, 1);
    CertAddStoreToCollection(tempCertCollection, certContext->hCertStore, 0, 0);

    CERT_CHAIN_PARA parameters;
    ZeroMemory(&parameters, sizeof(parameters));
    parameters.cbSize = sizeof(CERT_CHAIN_PARA);
    parameters.RequestedUsage.dwType = USAGE_MATCH_TYPE_AND;
    parameters.RequestedUsage.Usage.cUsageIdentifier = 1;
    LPSTR oid = LPSTR(isClient ? szOID_PKIX_KP_SERVER_AUTH
                               : szOID_PKIX_KP_CLIENT_AUTH);
    parameters.RequestedUsage.Usage.rgpszUsageIdentifier = &oid;

    configuration.peerCertificate.clear();
    configuration.peerCertificateChain.clear();
    const CERT_CHAIN_CONTEXT *chainContext = nullptr;
    auto freeCertChain = qScopeGuard([&chainContext]() {
        if (chainContext)
            CertFreeCertificateChain(chainContext);
    });
    BOOL status = CertGetCertificateChain(nullptr, // hChainEngine, default
                                          certContext, // pCertContext
                                          nullptr, // pTime, 'now'
                                          tempCertCollection, // hAdditionalStore, additional cert store
                                          &parameters, // pChainPara
                                          CERT_CHAIN_REVOCATION_CHECK_CHAIN_EXCLUDE_ROOT, // dwFlags
                                          nullptr, // reserved
                                          &chainContext // ppChainContext
    );
    CertCloseStore(tempCertCollection, 0);
    if (status == FALSE || !chainContext || chainContext->cChain == 0) {
        QSslError error(QSslError::UnableToVerifyFirstCertificate);
        sslErrors += error;
        emit q->peerVerifyError(error);
        return q->state() == QAbstractSocket::ConnectedState;
    }

    // Helper-function to get a QSslCertificate given a CERT_CHAIN_ELEMENT
    static auto getCertificateFromChainElement = [](CERT_CHAIN_ELEMENT *element) {
        if (!element)
            return QSslCertificate();

        const CERT_CONTEXT *certContext = element->pCertContext;
        return QSslCertificatePrivate::QSslCertificate_from_CERT_CONTEXT(certContext);
    };

    // Pick a chain to use as the certificate chain, if multiple are available:
    // According to https://docs.microsoft.com/en-gb/windows/desktop/api/wincrypt/ns-wincrypt-_cert_chain_context
    // this seems to be the best way to get a trusted chain.
    CERT_SIMPLE_CHAIN *chain = chainContext->rgpChain[chainContext->cChain - 1];

    if (chain->TrustStatus.dwErrorStatus & CERT_TRUST_IS_PARTIAL_CHAIN) {
        auto error = QSslError(QSslError::SslError::UnableToGetIssuerCertificate,
                               getCertificateFromChainElement(chain->rgpElement[chain->cElement - 1]));
        sslErrors += error;
        emit q->peerVerifyError(error);
        if (q->state() != QAbstractSocket::ConnectedState)
            return false;
    }
    if (chain->TrustStatus.dwErrorStatus & CERT_TRUST_INVALID_BASIC_CONSTRAINTS) {
        // @Note: This is actually one of two errors:
        // "either the certificate cannot be used to issue other certificates, or the chain path length has been exceeded."
        // But here we are checking the chain's status, so we assume the "issuing" error cannot occur here.
        auto error = QSslError(QSslError::PathLengthExceeded);
        sslErrors += error;
        emit q->peerVerifyError(error);
        if (q->state() != QAbstractSocket::ConnectedState)
            return false;
    }
    static const DWORD leftoverCertChainErrorMask = CERT_TRUST_IS_CYCLIC | CERT_TRUST_INVALID_EXTENSION
            | CERT_TRUST_INVALID_POLICY_CONSTRAINTS | CERT_TRUST_INVALID_NAME_CONSTRAINTS
            | CERT_TRUST_CTL_IS_NOT_TIME_VALID | CERT_TRUST_CTL_IS_NOT_SIGNATURE_VALID
            | CERT_TRUST_CTL_IS_NOT_VALID_FOR_USAGE;
    if (chain->TrustStatus.dwErrorStatus & leftoverCertChainErrorMask) {
        auto error = QSslError(QSslError::SslError::UnspecifiedError);
        sslErrors += error;
        emit q->peerVerifyError(error);
        if (q->state() != QAbstractSocket::ConnectedState)
            return false;
    }

    DWORD verifyDepth = chain->cElement;
    if (configuration.peerVerifyDepth > 0 && DWORD(configuration.peerVerifyDepth) < verifyDepth)
        verifyDepth = DWORD(configuration.peerVerifyDepth);

    for (DWORD i = 0; i < verifyDepth; i++) {
        CERT_CHAIN_ELEMENT *element = chain->rgpElement[i];
        QSslCertificate certificate = getCertificateFromChainElement(element);
        const QList<QSslCertificateExtension> extensions = certificate.extensions();

#ifdef QSSLSOCKET_DEBUG
        qCDebug(lcSsl) << "issuer:" << certificate.issuerDisplayName()
                       << "\nsubject:" << certificate.subjectDisplayName()
                       << "\nQSslCertificate info:" << certificate
                       << "\nextended error info:" << element->pwszExtendedErrorInfo
                       << "\nerror status:" << element->TrustStatus.dwErrorStatus;
#endif

        ////// @todo @note This is where *all* certificates are added, read at the top for "discussion".
        configuration.peerCertificateChain.append(certificate);

        if (certificate.isBlacklisted()) {
            const auto error = QSslError(QSslError::CertificateBlacklisted, certificate);
            sslErrors += error;
            emit q->peerVerifyError(error);
            if (q->state() != QAbstractSocket::ConnectedState)
                return false;
        }

        LONG result = CertVerifyTimeValidity(nullptr /*== now */, element->pCertContext->pCertInfo);
        if (result != 0) {
            auto error = QSslError(result == -1 ? QSslError::CertificateNotYetValid
                                                : QSslError::CertificateExpired,
                                   certificate);
            sslErrors += error;
            emit q->peerVerifyError(error);
            if (q->state() != QAbstractSocket::ConnectedState)
                return false;
        }

        //// Errors
        if (element->TrustStatus.dwErrorStatus & CERT_TRUST_IS_NOT_TIME_VALID) {
            // handled right above
            Q_ASSERT(!sslErrors.isEmpty());
        }
        if (element->TrustStatus.dwErrorStatus & CERT_TRUST_IS_REVOKED) {
            auto error = QSslError(QSslError::CertificateRevoked, certificate);
            sslErrors += error;
            emit q->peerVerifyError(error);
            if (q->state() != QAbstractSocket::ConnectedState)
                return false;
        }
        if (element->TrustStatus.dwErrorStatus & CERT_TRUST_IS_NOT_SIGNATURE_VALID) {
            auto error = QSslError(QSslError::CertificateSignatureFailed, certificate);
            sslErrors += error;
            emit q->peerVerifyError(error);
            if (q->state() != QAbstractSocket::ConnectedState)
                return false;
        }

        // While netscape shouldn't be relevant now it defined an extension which is
        // still in use. Schannel does not check this automatically, so we do it here.
        // It is used to differentiate between client and server certificates.
        if (netscapeWrongCertType(extensions, isClient))
            element->TrustStatus.dwErrorStatus |= CERT_TRUST_IS_NOT_VALID_FOR_USAGE;

        if (element->TrustStatus.dwErrorStatus & CERT_TRUST_IS_NOT_VALID_FOR_USAGE) {
            auto error = QSslError(QSslError::InvalidPurpose, certificate);
            sslErrors += error;
            emit q->peerVerifyError(error);
            if (q->state() != QAbstractSocket::ConnectedState)
                return false;
        }
        if (element->TrustStatus.dwErrorStatus & CERT_TRUST_IS_UNTRUSTED_ROOT) {
            // Override this error if we have the certificate inside our trusted CAs list.
            const bool isTrustedRoot = configuration.caCertificates.contains(certificate);
            if (!isTrustedRoot) {
                auto error = QSslError(QSslError::CertificateUntrusted, certificate);
                sslErrors += error;
                emit q->peerVerifyError(error);
                if (q->state() != QAbstractSocket::ConnectedState)
                    return false;
            }
        }
        static const DWORD certRevocationCheckUnavailableError = CERT_TRUST_IS_OFFLINE_REVOCATION
                | CERT_TRUST_REVOCATION_STATUS_UNKNOWN;
        if (element->TrustStatus.dwErrorStatus & certRevocationCheckUnavailableError) {
            // @future(maybe): Do something with this
        }

        // Dumping ground of errors that don't fit our specific errors
        static const DWORD leftoverCertErrorMask = CERT_TRUST_IS_CYCLIC
                | CERT_TRUST_INVALID_EXTENSION | CERT_TRUST_INVALID_NAME_CONSTRAINTS
                | CERT_TRUST_INVALID_POLICY_CONSTRAINTS
                | CERT_TRUST_HAS_EXCLUDED_NAME_CONSTRAINT
                | CERT_TRUST_HAS_NOT_SUPPORTED_CRITICAL_EXT
                | CERT_TRUST_HAS_NOT_DEFINED_NAME_CONSTRAINT
                | CERT_TRUST_HAS_NOT_PERMITTED_NAME_CONSTRAINT
                | CERT_TRUST_HAS_NOT_SUPPORTED_NAME_CONSTRAINT;
        if (element->TrustStatus.dwErrorStatus & leftoverCertErrorMask) {
            auto error = QSslError(QSslError::UnspecifiedError, certificate);
            sslErrors += error;
            emit q->peerVerifyError(error);
            if (q->state() != QAbstractSocket::ConnectedState)
                return false;
        }
        if (element->TrustStatus.dwErrorStatus & CERT_TRUST_INVALID_BASIC_CONSTRAINTS) {
            auto it = std::find_if(extensions.cbegin(), extensions.cend(),
                                   [](const QSslCertificateExtension &extension) {
                                       return extension.name() == QLatin1String("basicConstraints");
                                   });
            if (it != extensions.cend()) {
                // @Note: This is actually one of two errors:
                // "either the certificate cannot be used to issue other certificates,
                // or the chain path length has been exceeded."
                QVariantMap basicConstraints = it->value().toMap();
                QSslError error;
                if (i > 0 && !basicConstraints.value(QLatin1String("ca"), false).toBool())
                    error = QSslError(QSslError::InvalidPurpose, certificate);
                else
                    error = QSslError(QSslError::PathLengthExceeded, certificate);
                sslErrors += error;
                emit q->peerVerifyError(error);
                if (q->state() != QAbstractSocket::ConnectedState)
                    return false;
            }
        }
        if (element->TrustStatus.dwErrorStatus & CERT_TRUST_IS_EXPLICIT_DISTRUST) {
            auto error = QSslError(QSslError::CertificateBlacklisted, certificate);
            sslErrors += error;
            emit q->peerVerifyError(error);
            if (q->state() != QAbstractSocket::ConnectedState)
                return false;
        }

        if (element->TrustStatus.dwInfoStatus & CERT_TRUST_IS_SELF_SIGNED) {
            // If it's self-signed *and* a CA then we can assume it's a root CA certificate
            // and we can ignore the "self-signed" note:
            // We check the basicConstraints certificate extension when possible, but this didn't
            // exist for version 1, so we can only guess in that case
            const bool isRootCertificateAuthority = isCertificateAuthority(extensions)
                    || certificate.version() == "1";

            // Root certificate tends to be signed by themselves, so ignore self-signed status.
            if (!isRootCertificateAuthority) {
                auto error = QSslError(QSslError::SelfSignedCertificate, certificate);
                sslErrors += error;
                emit q->peerVerifyError(error);
                if (q->state() != QAbstractSocket::ConnectedState)
                    return false;
            }
        }
    }

    if (!configuration.peerCertificateChain.isEmpty())
        configuration.peerCertificate = configuration.peerCertificateChain.first();

    // @Note: Somewhat copied from qsslsocket_mac.cpp
    const bool doVerifyPeer = configuration.peerVerifyMode == QSslSocket::VerifyPeer
            || (configuration.peerVerifyMode == QSslSocket::AutoVerifyPeer
                && mode == QSslSocket::SslClientMode);
    // Check the peer certificate itself. First try the subject's common name
    // (CN) as a wildcard, then try all alternate subject name DNS entries the
    // same way.
    if (!configuration.peerCertificate.isNull()) {
        // but only if we're a client connecting to a server
        // if we're the server, don't check CN
        if (mode == QSslSocket::SslClientMode) {
            const QString peerName(verificationPeerName.isEmpty() ? q->peerName() : verificationPeerName);
            if (!isMatchingHostname(configuration.peerCertificate, peerName)) {
                // No matches in common names or alternate names.
                const QSslError error(QSslError::HostNameMismatch, configuration.peerCertificate);
                sslErrors += error;
                emit q->peerVerifyError(error);
                if (q->state() != QAbstractSocket::ConnectedState)
                    return false;
            }
        }
    } else if (doVerifyPeer) {
        // No peer certificate presented. Report as error if the socket
        // expected one.
        const QSslError error(QSslError::NoPeerCertificate);
        sslErrors += error;
        emit q->peerVerifyError(error);
        if (q->state() != QAbstractSocket::ConnectedState)
            return false;
    }

    return true;
}

bool QSslSocketBackendPrivate::rootCertOnDemandLoadingAllowed()
{
    return allowRootCertOnDemandLoading && s_loadRootCertsOnDemand;
}

QT_END_NAMESPACE
