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

#undef Q_UNIMPLEMENTED // @temp: remove, just want better output
#define Q_UNIMPLEMENTED() qWarning() << __func__ << "is not implemented"

/*
    @todo list:

    High(?) priority:
    - Send certificate chain/intermediate certificates!!
        - No documentation on how to send the chain
        - A stackoverflow question on this from 3 years ago implies schannel only sends intermediate
            certificates if it's "in the system or user certificate store".
                - https://stackoverflow.com/q/30156584/2493610
                - Need to test if this actually works
                    - Tested: It works. But pollutes the system store.
        - I'd like to avoid 'polluting' the system/user store, even if temporarily (temporarily
            storing certificates in the system store relies on a clean exit to remove the certificates).

    Medium priority:
    - Setting cipher-suites (or ALG_ID)
        - People have survived without it in WinRT
    - peerCertificateChain() is different in Schannel in that it includes the entire chain, not just
        the certificates that the peer sent. This means the root certificate is found in this chain.
        Which I would argue makes sense for a list whose purpose is displaying the chain.
        But this should be up for discussion. Relevant (support-reported) bug: QTBUG-20119

    Low priority:
    - Possibly make RAII wrappers for SecBuffer (which I commonly create QScopeGuards for)
    - Perform the '@future' optimization in "transmit()"

    @future!:

    - PSK support
        - Was added in Windows 10 (it seems), documentation at time of writing is sparse/non-existent.
            - Specifically about how to supply credentials when they're requested.
            - Or how to recognize that they're requested in the first place.
        - Skip certificate verification.
        - Check if "PSK-only" is still required to do PSK _at all_ (all-around bad solution).
        - Check if SEC_I_INCOMPLETE_CREDENTIALS is still returned for both "missing certificate" and
            "missing PSK" when calling InitializeSecurityContext in "doStep2".

    *** Things that need to be documented ***

    About PeerVerifyMode (copied from somewhere in this code):
    QueryPeer can (currently) not work in Schannel since Schannel itself doesn't have a way to
    ask for a certificate and then still be OK if it's not received.
    To work around this we don't request a certificate at all for QueryPeer.
    For servers AutoVerifyPeer is supposed to be treated the same as QueryPeer. This means that
    servers using Schannel will only request client certificate for "VerifyPeer".

    About PSK:
    Not supported (at least for now).
*/

QT_BEGIN_NAMESPACE

namespace {
SecBuffer createSecBuffer(void *ptr, unsigned long length, unsigned long bufferType)
{
    return SecBuffer{
        length,
        bufferType,
        ptr
    };
}

SecBuffer createSecBuffer(QByteArray &buffer, unsigned long bufferType)
{
    return createSecBuffer(buffer.data(), static_cast<unsigned long>(buffer.length()), bufferType);
}

void schannelError(qint32 status)
{
    // @todo: make this less debugging-related
    switch (status) {
    case SEC_E_INSUFFICIENT_MEMORY:
    case SEC_E_INTERNAL_ERROR:
        qDebug() << "silly error";
        break;
    case SEC_E_INVALID_HANDLE:
        qDebug() << "invalid handle";
        break;

    case SEC_E_INVALID_TOKEN:
        qDebug() << "invalid token";
        break;
    case SEC_E_LOGON_DENIED:
    case SEC_E_NO_AUTHENTICATING_AUTHORITY:
    case SEC_E_NO_CREDENTIALS:
        qDebug() << "auth errors";
        break;
    case SEC_E_TARGET_UNKNOWN:
        qDebug() << "target unknown";
        break;
    case SEC_E_UNSUPPORTED_FUNCTION:
        qDebug() << "unsupported function";
        break;
    case SEC_E_WRONG_PRINCIPAL:
        qDebug() << "wrong principal";
        break;
#ifdef Q_CC_MSVC // @todo: define missing on mingw
    case SEC_E_APPLICATION_PROTOCOL_MISMATCH:
        qDebug() << "protocol mismatch";
        break;
#endif
    case SEC_E_ILLEGAL_MESSAGE:
        qDebug() << "unexpected or badly formatted message received";
        break;
    case SEC_E_ENCRYPT_FAILURE:
        qDebug() << "The data could not be encrypted!";
        break;
    case SEC_E_ALGORITHM_MISMATCH:
        qDebug() << "Algorithm mismatch";
        break;
    case SEC_E_UNKNOWN_CREDENTIALS:
        qDebug() << "unknown credentials";
        break;
    default:
        qDebug() << "unknown status:" << status;
        break;
    }
}

DWORD fromQtSslProtocol(QSsl::SslProtocol protocol)
{
    DWORD protocols = 0;
    switch (protocol) {
    case QSsl::UnknownProtocol:
        return 0; // no
    case QSsl::AnyProtocol:
        protocols = SP_PROT_SSL2 | SP_PROT_SSL3 | SP_PROT_TLS1_0
                | SP_PROT_TLS1_1 | SP_PROT_TLS1_2; // @future: TlsV1_3
        break;
    case QSsl::SslV2:
        protocols = SP_PROT_SSL2;
        break;
    case QSsl::SslV3:
        protocols = SP_PROT_SSL3;
        break;
    case QSsl::TlsV1SslV3:
        protocols = (SP_PROT_SSL3 | SP_PROT_TLS1_0);
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
    // @future: TlsV1_3
    case QSsl::SecureProtocols: // TLS v1.0 and later is currently considered secure (@todo: can also be 0; system defaults)
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
        break; // @future: fallthrough to TlsV1_3OrLater when it is added
    }
    return protocols;
}

QSsl::SslProtocol toQtSslProtocol(DWORD protocol)
{
    if (protocol & SP_PROT_SSL2)
        return QSsl::SslV2;
    if (protocol & SP_PROT_SSL3)
        return QSsl::SslV3;
    if (protocol & SP_PROT_TLS1_0)
        return QSsl::TlsV1_0;
    if (protocol & SP_PROT_TLS1_1)
        return QSsl::TlsV1_1;
    if (protocol & SP_PROT_TLS1_2)
        return QSsl::TlsV1_2;
    // @future: TlsV1_3
    Q_UNREACHABLE();
    return QSsl::UnknownProtocol;
}

void printSecBufferDescInfo(const SecBufferDesc &desc) // @temp ?
{
#ifndef QSSLSOCKET_DEBUG
    Q_UNUSED(desc)
#else
#define Print(value) #value << ":" << value
    for (unsigned i = 0; i < desc.cBuffers; i++) {
        auto buffer = desc.pBuffers[i];
        qDebug() << i << Print(buffer.BufferType) << Print(buffer.cbBuffer);
    }
#undef Print
#endif
}

Q_DECL_UNUSED QVector<ALG_ID> QSslCipherToALG_ID(QList<QSslCipher> ciphers)
{
    if (ciphers.isEmpty())
        return {};
    Q_UNIMPLEMENTED();
    return {}; // @todo: implement
}
} // anonymous namespace

// bool QSslSocketPrivate::s_libraryLoaded = true;
bool QSslSocketPrivate::s_loadRootCertsOnDemand = true;
// bool QSslSocketPrivate::s_loadedCiphersAndCerts = false;

void QSslSocketPrivate::ensureInitialized()
{
    static QMutex initMutex;
    static bool initialized = false;

    if (initialized)
        return;
    QMutexLocker locker(&initMutex);
    if (initialized)
        return;
    initialized = true;

    setDefaultCaCertificates(systemCaCertificates());
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
    // Changes: function pointers changed to direct usage. (@todo: update original)
    QList<QSslCertificate> systemCerts;
    HCERTSTORE hSystemStore = CertOpenSystemStore(0, L"ROOT");
    if (hSystemStore) {
        PCCERT_CONTEXT pc = nullptr;
        while (1) {
            pc = CertFindCertificateInStore(hSystemStore, X509_ASN_ENCODING, 0, CERT_FIND_ANY, nullptr, pc);
            if (!pc)
                break;
            QByteArray der(reinterpret_cast<const char *>(pc->pbCertEncoded),
                           static_cast<int>(pc->cbCertEncoded));
            QSslCertificate cert(der, QSsl::Der);
            systemCerts.append(cert);
        }
        CertCloseStore(hSystemStore, 0);
    }
    return systemCerts;
}

long QSslSocketPrivate::sslLibraryVersionNumber()
{
    return QOperatingSystemVersion::current().majorVersion(); // @todo should be more accurate than this..
}

QString QSslSocketPrivate::sslLibraryVersionString()
{
    auto os = QOperatingSystemVersion::current();
    return QString::fromLatin1("Secure Channel, %1 %2.%3.%4")
            .arg(os.name(),
                 QString::number(os.majorVersion()),
                 QString::number(os.minorVersion()),
                 QString::number(os.microVersion()));
}

long QSslSocketPrivate::sslLibraryBuildVersionNumber()
{
    return QOperatingSystemVersion::current().majorVersion(); // @todo not accurate at all
}

QString QSslSocketPrivate::sslLibraryBuildVersionString()
{
    auto os = QOperatingSystemVersion::current();
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

bool QSslSocketBackendPrivate::sendToken(void *token, unsigned long tokenLength)
{
    int written = plainSocket->write(static_cast<const char *>(token), tokenLength);
    if (written >= 0 && written != int(tokenLength)) {
        // failed to write everything
        setErrorAndEmit(QAbstractSocket::SslInternalError, QStringLiteral("@todo: couldn't write all the data"));
        return false;
    }
    return true;
}

QString QSslSocketBackendPrivate::targetName() const
{
    Q_Q(const QSslSocket);
    // Used for SNI extension
    return verificationPeerName.isEmpty() ? q->peerName() : verificationPeerName;
}

ULONG QSslSocketBackendPrivate::getContextRequirements()
{
    bool isServer = mode == QSslSocket::SslServerMode;
    ULONG req = 0;

    req |= ISC_REQ_ALLOCATE_MEMORY; // allocate memory for buffers automatically
    req |= ISC_REQ_CONFIDENTIALITY; // encrypt messages
    req |= ISC_REQ_REPLAY_DETECT; // detect replayed messages
    req |= ISC_REQ_SEQUENCE_DETECT; // detect out of sequence messages
    if (!isServer) // @todo ??? why'd I put this here, forgot (but it's required somehow)
        req |= ISC_REQ_MANUAL_CRED_VALIDATION; // Manually validate certificate
    req |= ISC_REQ_STREAM; // Support a stream-oriented connection

    if (isServer
        && configuration.peerVerifyMode != QSslSocket::PeerVerifyMode::VerifyNone
        // There doesn't seem to be a way to ask for client cert without _requiring_ it.
        && configuration.peerVerifyMode != QSslSocket::PeerVerifyMode::AutoVerifyPeer
        && configuration.peerVerifyMode != QSslSocket::PeerVerifyMode::QueryPeer) {
        req |= ASC_REQ_MUTUAL_AUTH;
    }
    if (certificateRequested) { // @todo (sorta experimental?)
        Q_ASSERT(!isServer);
        certificateRequested = false;
        req |= ISC_REQ_USE_SUPPLIED_CREDS;
    }

    return req;
}

bool QSslSocketBackendPrivate::acquireCredentialsHandle()
{
    const bool isClient = mode == QSslSocket::SslClientMode;
    qDebug() << __func__ << (isClient ? "client" : "server");
    const DWORD protocols = fromQtSslProtocol(configuration.protocol);

    const CERT_CHAIN_CONTEXT *chainContext = nullptr;
    const CERT_CONTEXT *localCert = nullptr;
    auto freeCertChain = qScopeGuard([&chainContext]() {
        if (chainContext)
            CertFreeCertificateChain(chainContext);
    });

    DWORD certsCount = 0;
    // Set up our certificate stores before trying to use one...
    initializeCertificateStores();
    if (localCertificateStore != nullptr) {
        if (configuration.privateKey.isNull()) {
            setErrorAndEmit(QAbstractSocket::SslInvalidUserDataError,
                            QSslSocket::tr("Cannot provide a certificate with no key"));
            return true; // Needed to support tst_QSslSocket::setEmptyKey
        }
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
            qDebug() << "no certificate chain fits our purpose"; // @temp
            return false; // @todo The user provided a certificate, but if we continue it will not be used
        } else {
            // @temp debug message
            qDebug() << "local chain numbers:" << chainContext->cChain << chainContext->rgpChain[0]->cElement;
            localCert = chainContext->rgpChain[0]->rgpElement[0]->pCertContext;
            certsCount = 1;

            ///////////////// @temp
            {
                // @temp: this works, but leaves the certificate in the system store
                HCERTSTORE myStore = CertOpenStore(CERT_STORE_PROV_SYSTEM, 0, 0, CERT_SYSTEM_STORE_CURRENT_USER | CERT_STORE_OPEN_EXISTING_FLAG, L"My");
                qDebug() << "myStore:" << myStore;
                if (myStore) {
                    for (DWORD i = 1; i < chainContext->rgpChain[0]->cElement; i++)
                        qDebug() << !!CertAddCertificateContextToStore(myStore, chainContext->rgpChain[0]->rgpElement[i]->pCertContext, CERT_STORE_ADD_NEWER, nullptr);
                    CertCloseStore(myStore, 0);
                }
            }
            //////////////// @temp
        }
    }

    SCHANNEL_CRED cred{
        SCHANNEL_CRED_VERSION, // dwVersion;
        certsCount, // cCreds
        &localCert, // paCred (certificate(s) containing a private key for authentication)
        nullptr, // hRootStore

        0, // cMappers (reserved)
        nullptr, // aphMappers (reserved)

        0, // cSupportedAlgs
        nullptr, // palgSupportedAlgs (nullptr = system default) @todo

        protocols, // grbitEnabledProtocols
        0, // dwMinimumCipherStrength (0 = system default)
        0, // dwMaximumCipherStrength (0 = system default)
        0, // dwSessionLifespan (0 = schannel default, 10 hours)
        SCH_CRED_REVOCATION_CHECK_CHAIN_EXCLUDE_ROOT
                | SCH_CRED_NO_DEFAULT_CREDS, // dwFlags
        0 // dwCredFormat (must be 0)
    };

    TimeStamp expiration{ 0, 0 };
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
        qDebug() << "failed to acquire credentials handle:";
        qDebug() << "protocol:" << configuration.protocol << "schannel:" << protocols;
        schannelError(status);
        // @todo: handle failure
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
        CertCloseStore(localCertificateStore, CERT_CLOSE_STORE_FORCE_FLAG);
        localCertificateStore = nullptr;
    }
    if (peerCertificateStore) {
        CertCloseStore(peerCertificateStore, CERT_CLOSE_STORE_FORCE_FLAG);
        peerCertificateStore = nullptr;
    }
    if (caCertificateStore) {
        CertCloseStore(caCertificateStore, CERT_CLOSE_STORE_FORCE_FLAG);
        caCertificateStore = nullptr;
    }
}

bool QSslSocketBackendPrivate::createContext()
{
    qDebug() << __func__;
    Q_ASSERT(mode == QSslSocket::SslClientMode);
    ULONG contextReq = getContextRequirements();

    SecBuffer outBuffers[3];
    outBuffers[0] = createSecBuffer(nullptr, 0, SECBUFFER_TOKEN);
    outBuffers[1] = createSecBuffer(nullptr, 0, SECBUFFER_ALERT);
    outBuffers[2] = createSecBuffer(nullptr, 0, SECBUFFER_EMPTY);
    auto freeBuffers = qScopeGuard([&outBuffers]() {
        for (int i = 0; i < 3; i++) {
            if (outBuffers[i].pvBuffer)
                FreeContextBuffer(outBuffers[i].pvBuffer);
        }
    });
    SecBufferDesc outputBufferDesc{
        SECBUFFER_VERSION,
        3,
        outBuffers
    };

    TimeStamp expiry;

    auto status = InitializeSecurityContext(&credentialHandle, // phCredential
                                            nullptr, // phContext
                                            targetName().toStdWString().data(), // pszTargetName
                                            contextReq, // fContextReq
                                            0, // Reserved1
                                            0, // TargetDataRep (unused)
                                            nullptr, // pInput (no input at the moment @todo: alpn)
                                            0, // Reserved2
                                            &contextHandle, // phNewContext
                                            &outputBufferDesc, // pOutput
                                            &contextAttributes, // pfContextAttr
                                            &expiry // ptsExpiry
    );

    if (status != SEC_I_CONTINUE_NEEDED) // @todo: other possible non-error return values here?
    {
        // @todo Some error occurred, handle it
        if (status == SEC_E_WRONG_PRINCIPAL) {
            // @todo: SNI error...
            setErrorAndEmit(QAbstractSocket::SslHandshakeFailedError, QStringLiteral("@todo: some SNI error"));
        } else if (status == SEC_E_INTERNAL_ERROR) {
            setErrorAndEmit(QAbstractSocket::SslInternalError, QStringLiteral("@todo: internal error when creating security context"));
        } else {
            // Not very descriptive...
            setErrorAndEmit(QAbstractSocket::SslHandshakeFailedError, QStringLiteral("Handshake failed."));
        }
        qDebug() << "create context: ssl dead";
        schannelError(status); // @temp: for debugging
        return false;
    }

    printSecBufferDescInfo(outputBufferDesc); // @temp
    if (!sendToken(outBuffers[0].pvBuffer, outBuffers[0].cbBuffer))
        return false;
    schannelState = SchannelState::Step2;
    return true;
}

bool QSslSocketBackendPrivate::acceptContext()
{
    qDebug() << __func__; // @temp
    Q_ASSERT(mode == QSslSocket::SslServerMode);
    ULONG contextReq = getContextRequirements();

    intermediateBuffer += plainSocket->readAll();
    qDebug() << "bytes available:" << intermediateBuffer.length();
    if (intermediateBuffer.isEmpty())
        return true; // definitely need more data..

    SecBuffer inBuffers[3];
    inBuffers[0] = createSecBuffer(intermediateBuffer, SECBUFFER_TOKEN);
    inBuffers[1] = createSecBuffer(nullptr, 0, SECBUFFER_EMPTY);
    SecBufferDesc inputBufferDesc{
        SECBUFFER_VERSION,
        2,
        inBuffers
    };

    SecBuffer outBuffers[3];
    outBuffers[0] = createSecBuffer(nullptr, 0, SECBUFFER_TOKEN);
    outBuffers[1] = createSecBuffer(nullptr, 0, SECBUFFER_ALERT);
    outBuffers[2] = createSecBuffer(nullptr, 0, SECBUFFER_EMPTY);
    auto freeBuffers = qScopeGuard([&outBuffers]() {
        for (int i = 0; i < 3; i++) {
            if (outBuffers[i].pvBuffer)
                FreeContextBuffer(outBuffers[i].pvBuffer);
        }
    });
    SecBufferDesc outputBufferDesc{
        SECBUFFER_VERSION,
        3,
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

    if (outBuffers[1].cbBuffer) { // some alert was raised
        qDebug() << "ALERT::" // @todo
                 << "I don't know how to check the alert";
    }

    if (inBuffers[1].BufferType == SECBUFFER_EXTRA) {
        // https://docs.microsoft.com/en-us/windows/desktop/secauthn/extra-buffers-returned-by-schannel
        // inBuffers[1].cbBuffer indicates the amount of bytes _NOT_ processed, the rest need to
        // be stored.
        intermediateBuffer = intermediateBuffer.right(inBuffers[1].cbBuffer);
    } else if (status != SEC_E_INCOMPLETE_MESSAGE) {
        intermediateBuffer.clear();
    }

    if (status != SEC_I_CONTINUE_NEEDED) {
        qDebug() << "accept context: ssl dead";
        schannelError(status); // @temp: for debugging
        setErrorAndEmit(QAbstractSocket::SslHandshakeFailedError, errorString);
        return false;
    }
    qDebug() << "accept context: continue needed";
    if (!sendToken(outBuffers[0].pvBuffer, outBuffers[0].cbBuffer)) {
        setErrorAndEmit(QAbstractSocket::SslHandshakeFailedError, errorString);
        return false;
    }
    schannelState = SchannelState::Step2;
    return true;
}

bool QSslSocketBackendPrivate::doStep2()
{
    bool isClient = mode == QSslSocket::SslClientMode;
    qDebug() << __func__ << (isClient ? "client" : "server");
    if (plainSocket->state() == QAbstractSocket::UnconnectedState) {
        qDebug() << "Remote host disconnected";
        return false;
    }
    // @temp:
    qDebug() << "bytes available from socket:" << plainSocket->bytesAvailable();
    qDebug() << "intermediateBuffer size:" << intermediateBuffer.size();

    intermediateBuffer += plainSocket->readAll();
    if (intermediateBuffer.isEmpty())
        return true; // no data, will fail

    SecBuffer inputBuffers[2];
    inputBuffers[0] = createSecBuffer(intermediateBuffer, SECBUFFER_TOKEN);
    inputBuffers[1] = createSecBuffer(nullptr, 0, SECBUFFER_EMPTY);
    SecBufferDesc inputBufferDesc{
        SECBUFFER_VERSION,
        2,
        inputBuffers
    };

    SecBuffer outBuffers[3];
    outBuffers[0] = createSecBuffer(nullptr, 0, SECBUFFER_TOKEN);
    outBuffers[1] = createSecBuffer(nullptr, 0, SECBUFFER_ALERT);
    outBuffers[2] = createSecBuffer(nullptr, 0, SECBUFFER_EMPTY);
    auto freeBuffers = qScopeGuard([&outBuffers]() {
        for (int i = 0; i < 3; i++) {
            if (outBuffers[i].pvBuffer)
                FreeContextBuffer(outBuffers[i].pvBuffer);
        }
    });
    SecBufferDesc outputBufferDesc{
        SECBUFFER_VERSION,
        3,
        outBuffers
    };
    qDebug() << "input";
    printSecBufferDescInfo(inputBufferDesc);
    qDebug() << "output";
    printSecBufferDescInfo(outputBufferDesc);

    ULONG contextReq = getContextRequirements();
    TimeStamp expiry;
    auto status = InitializeSecurityContext(&credentialHandle, // phCredential
                                            &contextHandle, // phContext
                                            targetName().toStdWString().data(), // pszTargetName
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

    qDebug() << "input";
    printSecBufferDescInfo(inputBufferDesc);
    qDebug() << "output";
    printSecBufferDescInfo(outputBufferDesc);
    if (outBuffers[1].cbBuffer) { // some alert was raised
        qDebug() << "ALERT::" // @todo
                 << "I don't know how to check the alert";
    }

    if (inputBuffers[1].BufferType == SECBUFFER_EXTRA) {
        // https://docs.microsoft.com/en-us/windows/desktop/secauthn/extra-buffers-returned-by-schannel
        // inputBuffers[1].cbBuffer indicates the amount of bytes _NOT_ processed, the rest need to
        // be stored.
        intermediateBuffer = intermediateBuffer.right(inputBuffers[1].cbBuffer);
    } else {
        // Clear the buffer if we weren't asked for more data
        if (status != SEC_E_INCOMPLETE_MESSAGE)
            intermediateBuffer.clear();
    }
    // @todo: clean up
    switch (status) {
    case SEC_E_OK:
        // Need to transmit a final token in the handshake if 'cbBuffer' is non-zero.
        if (!sendToken(outBuffers[0].pvBuffer, outBuffers[0].cbBuffer))
            return false;
        qDebug() << "ssl done";
        schannelState = SchannelState::Step3;
        return true;
    case SEC_I_COMPLETE_AND_CONTINUE:
    case SEC_I_COMPLETE_NEEDED:
        qDebug() << "ssl need complete"; // @note: server-side only!
        SecBufferDesc message; // @todo implement
        status = CompleteAuthToken(&contextHandle, &message);
        return true;
    case SEC_I_CONTINUE_NEEDED:
        if (!sendToken(outBuffers[0].pvBuffer, outBuffers[0].cbBuffer))
            return false;
        // Must call InitializeSecurityContext again later (done through continueHandshake)
        qDebug() << "ssl need continue";
        return true;
    case SEC_I_INCOMPLETE_CREDENTIALS: {
        // @todo: server asked for client credentials, and they didn't match anything server-side
        qDebug() << "ssl need credentials";

        certificateRequested = true;

        // Delete the context and recreate it now that we have set "certificateRequested" to true.
        // This will recreate the context with a new flag which makes schannel send our certificate.
        deallocateContext();
        schannelState = SchannelState::Step1;
        return createContext();
    }
    case SEC_I_CONTEXT_EXPIRED:
        // "The message sender has finished using the connection and has initiated a shutdown."
        if (outBuffers[0].BufferType == SECBUFFER_TOKEN) {
            if (!sendToken(outBuffers[0].pvBuffer, outBuffers[0].cbBuffer))
                return false;
        }
        return true;
    case SEC_E_INCOMPLETE_MESSAGE:
        // Simply incomplete, wait for more data
        qDebug() << "incomplete message";
        return true;
    case SEC_E_ALGORITHM_MISMATCH:
        qDebug() << "algorithm mismatch";
        setErrorAndEmit(QAbstractSocket::SslHandshakeFailedError, QStringLiteral("Protocol version mismatch"));
        return false;
    }

    // Note: We can get here if the connection is using TLS 1.2 and the server certificate uses
    // MD5, which is not allowed in Schannel. This causes an "invalid token" error during handshake.
    // (If you came here investigating an error: md5 is insecure, update your certificate)

    // @todo Some error occurred, handle it. Error message is vague
    setErrorAndEmit(QAbstractSocket::SslHandshakeFailedError, QStringLiteral("Handshake failed"));
    qDebug() << "step 2: ssl dead";
    schannelError(status);
    return false;
}

bool QSslSocketBackendPrivate::doStep3()
{
    bool isClient = mode == QSslSocket::SslClientMode;
    qDebug() << __func__ << (isClient ? "client" : "server");
    // @todo: make this more.. release-version-friendly
#define checkStatus(status)                                   \
    if (status != SEC_E_OK) {                                 \
        qDebug() << __LINE__ << "Couldn't query the context"; \
        schannelError(status); /*@temp: debugging*/           \
        return false;                                         \
    }

    // everything is set up, now make sure there's nothing wrong and query some attributes...
    if (contextAttributes != getContextRequirements()) {
        qWarning() << "Didn't get the requirements we asked for!"; // @todo: emit error

#ifdef QSSLSOCKET_DEBUG
        qDebug() << "Context and requested:\n"
                 << contextAttributes << "xor" << getContextRequirements()
                 << '=' << (contextAttributes ^ getContextRequirements());
        if (!(contextAttributes & ISC_RET_ALLOCATED_MEMORY))
            qDebug() << "alloc mem missing";
        if (!(contextAttributes & ISC_RET_CONFIDENTIALITY)) // encrypt messages
            qDebug() << "encrypt messages missing";
        if (!(contextAttributes & ISC_RET_REPLAY_DETECT)) // detect replayed messages
            qDebug() << "detect replay missing";
        if (!(contextAttributes & ISC_RET_SEQUENCE_DETECT)) // detect out of sequence messages
            qDebug() << "detect out of sequence missing";
        if (!(contextAttributes & ISC_RET_MANUAL_CRED_VALIDATION)) // Manually validate certificate
            qDebug() << "manually validate cert missing";
        if (!(contextAttributes & ISC_RET_STREAM)) // Support stream
            qDebug() << "stream missing";
#endif
        return false;
    }

    // Get stream sizes (to know the max size of a message and the size of the header and trailer)
    auto status = QueryContextAttributes(&contextHandle,
                                         SECPKG_ATTR_STREAM_SIZES,
                                         reinterpret_cast<void *>(&streamSizes));
    checkStatus(status);

    // Get session cipher info
    status = QueryContextAttributes(&contextHandle,
                                    SECPKG_ATTR_CONNECTION_INFO,
                                    reinterpret_cast<void *>(&connectionInfo));
    checkStatus(status);

    // Verify certificate
    CERT_CONTEXT *certificateContext = nullptr;
    auto freeCertificate = qScopeGuard([&certificateContext]() {
        CertFreeCertificateContext(certificateContext);
    });
    status = QueryContextAttributes(&contextHandle,
                                    SECPKG_ATTR_REMOTE_CERT_CONTEXT,
                                    reinterpret_cast<void *>(&certificateContext));

    // QueryPeer can (currently) not work in Schannel since Schannel itself doesn't have a way to
    // ask for a certificate and then still be OK if it's not received.
    // To work around this we don't request a certificate at all for QueryPeer.
    // For servers AutoVerifyPeer is supposed to be treated the same as QueryPeer.
    // This means that servers using Schannel will only request client certificate for "VerifyPeer".
    if ((!isClient && configuration.peerVerifyMode == QSslSocket::PeerVerifyMode::VerifyPeer)
        || (isClient && configuration.peerVerifyMode != QSslSocket::PeerVerifyMode::VerifyNone
            && configuration.peerVerifyMode != QSslSocket::PeerVerifyMode::QueryPeer)) {
        checkStatus(status);
    }

    sslErrors = verifySingleCertificate(certificateContext); // @todo
    qDebug() << sslErrors;
    if (!checkSslErrors() || state != QAbstractSocket::ConnectedState) {
        qDebug() << "doStep3 unsuccessful";
        return paused; // If we're paused then checkSslErrors returned false, but it's not an error
    }
    qDebug() << "doStep3 successful";

    schannelState = SchannelState::Done;
    return true;
#undef checkStatus
}

bool QSslSocketBackendPrivate::renegotiate()
{
    qDebug() << __func__;
    SecBuffer outBuffers[3];
    outBuffers[0] = createSecBuffer(nullptr, 0, SECBUFFER_TOKEN);
    outBuffers[1] = createSecBuffer(nullptr, 0, SECBUFFER_ALERT);
    outBuffers[2] = createSecBuffer(nullptr, 0, SECBUFFER_EMPTY);
    auto freeBuffers = qScopeGuard([&outBuffers]() {
        for (int i = 0; i < 3; i++) {
            if (outBuffers[i].pvBuffer)
                FreeContextBuffer(outBuffers[i].pvBuffer);
        }
    });
    SecBufferDesc outputBufferDesc{
        SECBUFFER_VERSION,
        3,
        outBuffers
    };

    ULONG contextReq = getContextRequirements();
    TimeStamp expiry;
    auto status = InitializeSecurityContext(&credentialHandle, // phCredential
                                            &contextHandle, // phContext
                                            targetName().toStdWString().data(), // pszTargetName
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
    for (int i = 0; i < 3; i++) { // @temp debug
        qDebug() << "type:" << outBuffers[i].BufferType << "bytecount:" << outBuffers[i].cbBuffer;
    }
    if (status == SEC_I_CONTINUE_NEEDED) {
        qDebug() << "reneg needs continue";
        schannelState = SchannelState::Step2;
        if (!sendToken(outBuffers[0].pvBuffer, outBuffers[0].cbBuffer))
            return false;

        return true;
    }
    qDebug() << "reneg failed";
    schannelError(status);
    // @todo: emit some error
    return false;
}

/*!
    \internal
    reset the state in preparation for reuse of socket
*/
void QSslSocketBackendPrivate::reset()
{
    freeCredentialsHandle(); // in case we already had one (@todo: session resumption requires re-use)
    deallocateContext();
    closeCertificateStores(); // certificate stores could've changed

    contextAttributes = 0;
    intermediateBuffer.clear();
    schannelState = SchannelState::Step1;
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
    bool isClient = mode == QSslSocket::SslClientMode;
    qDebug() << __func__ << (isClient ? "client" : "server");
    Q_Q(QSslSocket);

    if (!connectionEncrypted || renegotiating)
        continueHandshake();
    if (renegotiating || shutdown)
        return;

    if (connectionEncrypted) { // encrypt data in writeBuffer and write it to plainSocket
        qint64 totalBytesWritten = 0;
        qint64 nextDataBlockSize;
        while ((nextDataBlockSize = writeBuffer.nextDataBlockSize()) > 0) {
            QByteArray plaintext;
            {
                // Try to read 'cbMaximumMessage' bytes from buffer before encrypting.
                int size = int(std::min(writeBuffer.size(), qint64(streamSizes.cbMaximumMessage)));
                plaintext.resize(size);
                // @temp
                qDebug() << "writeBuffer.size:" << writeBuffer.size() << ", streamSizes.cbMaximumMessage:" << streamSizes.cbMaximumMessage << ", end size:" << size;
                qint64 copied = 0;
                do {
                    int toRead = int(std::min(nextDataBlockSize, qint64(size - copied)));
                    copied += writeBuffer.read(plaintext.data() + copied, toRead);
                } while ((nextDataBlockSize = writeBuffer.nextDataBlockSize()) > 0
                         && copied < size);

                Q_ASSERT(copied <= size);
                plaintext.resize(copied);
            }
            QByteArray header(streamSizes.cbHeader, '\0');
            QByteArray trailer(streamSizes.cbTrailer, '\0');

            SecBuffer inputBuffers[4]{
                // @future[0/1]: optimize by using one container for all fields...
                createSecBuffer(header, SECBUFFER_STREAM_HEADER),
                createSecBuffer(plaintext, SECBUFFER_DATA),
                createSecBuffer(trailer, SECBUFFER_STREAM_TRAILER),
                createSecBuffer(nullptr, 0, SECBUFFER_EMPTY)
            };
            SecBufferDesc message{
                SECBUFFER_VERSION,
                4,
                inputBuffers
            };
            auto status = EncryptMessage(&contextHandle, // phContext
                                         0, // fQOP
                                         &message, // pMessage
                                         0 // sequence number (must be 0)
            );
            if (status != SEC_E_OK) {
                schannelError(status);
                setErrorAndEmit(QAbstractSocket::SslInternalError, QStringLiteral("@todo: encryption failed"));
                // @todo: restore the data somewhere? can't prepend to writebuffer...
                return;
            }
            // trailer has been observed to change size, so resize them all (when needed) to be safe
            header = header.left(inputBuffers[0].cbBuffer);
            plaintext = plaintext.left(inputBuffers[1].cbBuffer);
            trailer = trailer.left(inputBuffers[2].cbBuffer);
            qDebug() << (isClient ? "client" : "server") << "sending" << header.length() + plaintext.length() + trailer.length() << "bytes";
            int bytesWritten = 0;
            bytesWritten += plainSocket->write(header // @future[1/1]: ...because they need to be merged
                                               + plaintext
                                               + trailer);
            if (bytesWritten >= 0) {
                totalBytesWritten += bytesWritten;
            } else {
                // @todo: some error, handle that
            }
        }

        if (totalBytesWritten > 0) {
            Q_Q(QSslSocket);
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
        qDebug() << (isClient ? "client" : "server") << "about to decrypt from remote";
        int totalRead = 0;
        bool hadIncompleteData = false;
        while (!readBufferMaxSize || buffer.size() < readBufferMaxSize) {
            QByteArray ciphertext;
            if (intermediateBuffer.length()) {
                qDebug() << "There's data in intermediateBuffer:" << intermediateBuffer.length();
                ciphertext.swap(intermediateBuffer);
            }
            int initialLength = ciphertext.length();
            ciphertext += plainSocket->read(16384);
            if (ciphertext.length() == 0 || (hadIncompleteData && initialLength == ciphertext.length())) {
                qDebug() << "Nothing to decrypt, leaving loop!";
                if (ciphertext.length()) // Swap back if needed
                    intermediateBuffer.swap(ciphertext);
                break;
            }
            hadIncompleteData = false;
            qDebug() << "ciphertext length:" << ciphertext.length();

            SecBuffer dataBuffer[4]{
                createSecBuffer(ciphertext, SECBUFFER_DATA),
                createSecBuffer(nullptr, 0, SECBUFFER_EMPTY),
                createSecBuffer(nullptr, 0, SECBUFFER_EMPTY),
                createSecBuffer(nullptr, 0, SECBUFFER_EMPTY)
            };
            SecBufferDesc message{
                SECBUFFER_VERSION,
                4,
                dataBuffer
            };
            ULONG qop;
            auto status = DecryptMessage(&contextHandle, // phContext
                                         &message, // pMessage
                                         0, // MessageSeqNo
                                         &qop // pfQOP
            );
            if (status == SEC_E_OK || status == SEC_I_RENEGOTIATE || status == SEC_I_CONTEXT_EXPIRED) {
                // There can still be 0 output even if it succeeds, this is fine
                if (dataBuffer[1].cbBuffer > 0) {
                    // It was decrypted in-place.
                    // But [0] is the STREAM_HEADER, [1] is the DATA and [2] is the STREAM_TRAILER.
                    // The pointers in all of those still point into the 'ciphertext' byte array.
                    buffer.append(static_cast<char *>(dataBuffer[1].pvBuffer),
                                  dataBuffer[1].cbBuffer);
                    qDebug() << "read data, new buffer size:" << buffer.size();
                    qDebug() << "data read:" << ciphertext.mid(dataBuffer[0].cbBuffer, std::min(100ul, dataBuffer[1].cbBuffer));
                    totalRead += dataBuffer[1].cbBuffer;
                }
                if (dataBuffer[3].BufferType == SECBUFFER_EXTRA) {
                    // https://docs.microsoft.com/en-us/windows/desktop/secauthn/extra-buffers-returned-by-schannel
                    // dataBuffer[3].cbBuffer indicates the amount of bytes _NOT_ processed, the rest need to
                    // be stored.
                    qDebug() << "We've got excess data:" << dataBuffer[3].cbBuffer;
                    intermediateBuffer = ciphertext.right(dataBuffer[3].cbBuffer);
                }
                printSecBufferDescInfo(message); // @temp debug
            }
            // @todo: clean up error handling a bit...
            if (status == SEC_E_INCOMPLETE_MESSAGE) {
                // Need more data before we can decrypt.. to the buffer it goes!
                qDebug() << "data was incomplete, need more";
                Q_ASSERT(intermediateBuffer.isEmpty());
                intermediateBuffer.swap(ciphertext);
                // We try again, but if we don't get any more data then we leave
                hadIncompleteData = true;
            } else if (status == SEC_E_INVALID_HANDLE) {
                // I don't think this should happen, if it does we're done...
                qDebug() << "invalid handle";
                Q_UNREACHABLE();
            } else if (status == SEC_E_INVALID_TOKEN) {
                qDebug() << "invalid token";
                Q_UNREACHABLE(); // Happened once due to a bug, but shouldn't generally happen(?)
            } else if (status == SEC_E_MESSAGE_ALTERED) {
                // The message has been altered, disconnect now
                // @todo: emit error?
                qDebug() << "message altered";
                shutdown = true; // skips sending the shutdown alert
                disconnectFromHost();
                break;
            } else if (status == SEC_E_OUT_OF_SEQUENCE) {
                // I don't know if this one is actually "fatal"..
                qDebug() << "out of sequence";
                // @todo
                shutdown = true; // skips sending the shutdown alert
                disconnectFromHost();
                break;
            } else if (status == SEC_I_CONTEXT_EXPIRED) {
                // 'remote' has initiated a shutdown
                qDebug() << "remote shut down";
                shutdown = true; // skips sending the shutdown alert
                disconnectFromHost();
                setErrorAndEmit(QAbstractSocket::RemoteHostClosedError,
                                QSslSocket::tr("The TLS/SSL connection has been closed"));
                break;
            } else if (status == SEC_I_RENEGOTIATE) {
                // 'remote' wants to renegotiate
                qDebug() << "remote wants to renegotiate";
                qDebug() << message.pBuffers[1].cbBuffer
                         << ciphertext.mid(message.pBuffers[0].cbBuffer, message.pBuffers[1].cbBuffer);
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
    qDebug() << __func__ << (mode == QSslSocket::SslClientMode ? "client" : "server");
    DWORD shutdownToken = SCHANNEL_SHUTDOWN;
    SecBuffer buffer = createSecBuffer(&shutdownToken, sizeof(SCHANNEL_SHUTDOWN), SECBUFFER_TOKEN);
    SecBufferDesc token{
        SECBUFFER_VERSION,
        1,
        &buffer
    };
    auto status = ApplyControlToken(&contextHandle, &token);

    if (status == SEC_E_OK) {
        SecBuffer outBuffers[3];
        outBuffers[0] = createSecBuffer(nullptr, 0, SECBUFFER_TOKEN);
        outBuffers[1] = createSecBuffer(nullptr, 0, SECBUFFER_ALERT);
        outBuffers[2] = createSecBuffer(nullptr, 0, SECBUFFER_EMPTY);
        auto freeBuffers = qScopeGuard([&outBuffers]() {
            for (int i = 0; i < 3; i++) {
                if (outBuffers[i].pvBuffer)
                    FreeContextBuffer(outBuffers[i].pvBuffer);
            }
        });
        SecBufferDesc outputBufferDesc{
            SECBUFFER_VERSION,
            3,
            outBuffers
        };

        ULONG contextReq = getContextRequirements();
        TimeStamp expiry;
        auto status = InitializeSecurityContext(&credentialHandle, // phCredential
                                                &contextHandle, // phContext
                                                targetName().toStdWString().data(), // pszTargetName
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
        qDebug() << "output";
        printSecBufferDescInfo(outputBufferDesc);
        if (status == SEC_E_OK || status == SEC_I_CONTEXT_EXPIRED) {
            if (!sendToken(outBuffers[0].pvBuffer, outBuffers[0].cbBuffer)) {
                // @temp for debugging
                qDebug() << "Failed to send the shutdown alert/token";
                return;
            }

            schannelState = SchannelState::Step2;
            continueHandshake();
        } else {
            schannelError(status); // @temp
        }
    } else {
        schannelError(status); // @temp
    }
}

void QSslSocketBackendPrivate::disconnectFromHost()
{
    qDebug() << __func__;
    if (SecIsValidHandle(&contextHandle)) {
        if (!shutdown) {
            qDebug() << plainSocket->state() << connectionEncrypted;
            if (plainSocket->state() != QAbstractSocket::UnconnectedState) {
                if (schannelState >= SchannelState::Step3)
                    sendShutdown();
                if (connectionEncrypted)
                    transmit();
            }
            shutdown = true;
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
    Q_UNIMPLEMENTED();
    auto ciph = QSslCipher(QStringLiteral("Schannel"), sessionProtocol());
    return ciph;
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
    case SchannelState::Step1:
        if (!SecIsValidHandle(&credentialHandle) && !acquireCredentialsHandle()) {
            disconnectFromHost();
            return;
        }
        if (!SecIsValidHandle(&credentialHandle)) // Needed to support tst_QSslSocket::setEmptyKey
            return;
        if (!SecIsValidHandle(&contextHandle) && (isServer ? !acceptContext() : !createContext())) {
            disconnectFromHost();
            return;
        }
        if (schannelState != SchannelState::Step2)
            break;
        Q_FALLTHROUGH();
    case SchannelState::Step2:
        if (!doStep2()) {
            disconnectFromHost();
            return;
        }
        if (schannelState != SchannelState::Step3)
            break;
        Q_FALLTHROUGH();
    case SchannelState::Step3:
        if (!doStep3()) {
            disconnectFromHost();
            return;
        }
        if (schannelState != SchannelState::Done)
            break;
        Q_FALLTHROUGH();
    case SchannelState::Done:
        // connectionEncrypted is already true if we come here from a renegotiation
        if (!renegotiating) {
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
    const QString protocolStrings[] = { QStringLiteral("SSLv3"), QStringLiteral("TLSv1"),
                                        QStringLiteral("TLSv1.1"), QStringLiteral("TLSv1.2") };
    const QSsl::SslProtocol protocols[] = { QSsl::SslV3, QSsl::TlsV1_0, QSsl::TlsV1_1, QSsl::TlsV1_2 };
    const int size = 4;
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
    return {}; // @todo?
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
    // @todo can load into its own certificate store.
    Q_UNIMPLEMENTED();
    return false;
}

/*
    Copied from qsslsocket_mac.cpp, which was copied from qsslsocket_openssl.cpp
*/
bool QSslSocketBackendPrivate::checkSslErrors()
{
    Q_Q(QSslSocket);
    if (sslErrors.isEmpty())
        return true;

    emit q->sslErrors(sslErrors);
    qDebug() << "checking errors";

    const bool doVerifyPeer = configuration.peerVerifyMode == QSslSocket::VerifyPeer
            || (configuration.peerVerifyMode == QSslSocket::AutoVerifyPeer
                && mode == QSslSocket::SslClientMode);
    const bool doEmitSslError = !verifyErrorsHaveBeenIgnored();
    // check whether we need to emit an SSL handshake error
    if (doVerifyPeer && doEmitSslError) {
        if (q->pauseMode() & QAbstractSocket::PauseOnSslErrors) {
            qDebug() << "paused";
            pauseSocketNotifiers(q);
            paused = true;
        } else {
            qDebug() << "disconnecting";
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
        // @todo: move some of this somewhere else so it can be used by importPkcs12?
        const wchar_t *passphrase = L"";
        // Need to embed the private key in the certificate
        QByteArray pkcs12 = _q_makePkcs12(certChain,
                                          privateKey,
                                          QString::fromWCharArray(passphrase, 0));
        CRYPT_DATA_BLOB pfxBlob;
        pfxBlob.cbData = pkcs12.length();
        pfxBlob.pbData = reinterpret_cast<unsigned char *>(pkcs12.data());
        return PFXImportCertStore(&pfxBlob, passphrase, 0); // returns HCERTSTORE
    };

    if (!configuration.localCertificateChain.isEmpty()) {
        if (localCertificateStore == nullptr) {
            localCertificateStore = createStoreFromCertificateChain(configuration.localCertificateChain,
                                                                    configuration.privateKey);
            if (localCertificateStore == nullptr)
                qDebug() << "Failed to load certificate chain"; // @temp
        }
    }

    if (!configuration.caCertificates.isEmpty() && !caCertificateStore) {
        caCertificateStore = createStoreFromCertificateChain(configuration.caCertificates,
                                                             {}); // No private key for the CA certs
    }
}

// @todo: this name doesn't really match, since it grabs and checks a cert chain in the process..
QList<QSslError> QSslSocketBackendPrivate::verifySingleCertificate(CERT_CONTEXT *certContext)
{
    // @todo: I want this to be better, doesn't really match up with the openssl backend.
    Q_Q(QSslSocket);
    QList<QSslError> errors;
    if (certContext == nullptr)
        return errors;

    bool isClient = mode == QSslSocket::SslClientMode;

    // Create a collection of stores so we can pass in multiple stores as additional locations to
    // search for the certificate chain
    HCERTSTORE tempCertCollection = CertOpenStore(CERT_STORE_PROV_COLLECTION,
                                                  X509_ASN_ENCODING,
                                                  0,
                                                  CERT_STORE_CREATE_NEW_FLAG,
                                                  nullptr);
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
    auto status = CertGetCertificateChain(nullptr, // hChainEngine, default
                                          certContext, // pCertContext
                                          nullptr, // pTime, 'now'
                                          tempCertCollection, // hAdditionalStore, additional cert store
                                          &parameters, // pChainPara
                                          CERT_CHAIN_REVOCATION_CHECK_CHAIN_EXCLUDE_ROOT, // dwFlags
                                          nullptr, // reserved
                                          &chainContext // ppChainContext
    );
    CertCloseStore(tempCertCollection, 0);
    if (status == 0) {
        QSslError error(QSslError::UnspecifiedError);
        errors += error;
        emit q->peerVerifyError(error);
        return errors;
    }

    // Helper-function to get a QSslCertificate given a CERT_CHAIN_ELEMENT
    static auto getCertificateFromChainElement = [](CERT_CHAIN_ELEMENT *element) {
        if (!element)
            return QSslCertificate();

        const CERT_CONTEXT *certContext = element->pCertContext;
        QByteArray certificate = QByteArray(const_cast<const char *>(reinterpret_cast<char *>(certContext->pbCertEncoded)),
                                            certContext->cbCertEncoded);
        return QSslCertificate(certificate, QSsl::Der);
    };

    // @todo look into making these mappings into a map (map<error, qt sslerror>) to simplify
    // the loop somewhat
    qDebug() << "chains:" << chainContext->cChain;
    // Pick a chain to use as the certificate chain, if multiple are available:
    CERT_SIMPLE_CHAIN *chain = nullptr;
    if (chainContext->cChain > 0) {
        // According to https://docs.microsoft.com/en-gb/windows/desktop/api/wincrypt/ns-wincrypt-_cert_chain_context
        // this seems to be the best way to get a trusted chain.
        // (@temp Most search results to confirm this leads back to our code, qwindowscarootfetcher.cpp)
        chain = chainContext->rgpChain[chainContext->cChain - 1];
    }

    if (chain) {
        if (chain->TrustStatus.dwErrorStatus & CERT_TRUST_IS_PARTIAL_CHAIN) {
            qDebug() << "we have a partial chain...";
            auto error = QSslError(QSslError::SslError::UnableToGetIssuerCertificate);
            errors += error;
            emit q->peerVerifyError(error);
        }
        if (chain->TrustStatus.dwErrorStatus & CERT_TRUST_INVALID_BASIC_CONSTRAINTS) {
            auto error = QSslError(QSslError::PathLengthExceeded);
            errors += error;
            emit q->peerVerifyError(error);
        }
        if (chain->TrustStatus.dwErrorStatus & CERT_TRUST_IS_CYCLIC
            || chain->TrustStatus.dwErrorStatus & CERT_TRUST_INVALID_EXTENSION
            || chain->TrustStatus.dwErrorStatus & CERT_TRUST_INVALID_POLICY_CONSTRAINTS
            || chain->TrustStatus.dwErrorStatus & CERT_TRUST_INVALID_BASIC_CONSTRAINTS
            || chain->TrustStatus.dwErrorStatus & CERT_TRUST_INVALID_NAME_CONSTRAINTS
            || chain->TrustStatus.dwErrorStatus & CERT_TRUST_CTL_IS_NOT_TIME_VALID
            || chain->TrustStatus.dwErrorStatus & CERT_TRUST_CTL_IS_NOT_SIGNATURE_VALID
            || chain->TrustStatus.dwErrorStatus & CERT_TRUST_CTL_IS_NOT_VALID_FOR_USAGE) {
            auto error = QSslError(QSslError::SslError::UnspecifiedError);
            errors += error;
            emit q->peerVerifyError(error);
        }

        DWORD verifyDepth = (configuration.peerVerifyDepth == 0)
                ? chain->cElement
                : std::min(chain->cElement, DWORD(configuration.peerVerifyDepth));

        qDebug() << "elements:" << chain->cElement << ", verifyDepth:" << verifyDepth;
        for (DWORD j = 0; j < verifyDepth; j++) {
            CERT_CHAIN_ELEMENT *element = chain->rgpElement[j];
            QSslCertificate certificate = getCertificateFromChainElement(element);
            const QList<QSslCertificateExtension> extensions = certificate.extensions();
            qDebug() << "issuer:" << certificate.issuerDisplayName();
            qDebug() << "subject:" << certificate.subjectDisplayName();
            qDebug() << certificate;
            qDebug() << "extended error info:" << element->pwszExtendedErrorInfo;
            qDebug() << "error status:" << element->TrustStatus.dwErrorStatus;

            ////// @todo @note This is where certificates are added, read at the top for "discussion".
            configuration.peerCertificateChain.append(certificate);

            if (certificate.isBlacklisted()) {
                const auto error = QSslError(QSslError::CertificateBlacklisted, certificate);
                errors += error;
                emit q->peerVerifyError(error);
            }

            LONG result = CertVerifyTimeValidity(nullptr /* now */, element->pCertContext->pCertInfo);
            if (result == -1) {
                auto error = QSslError(QSslError::CertificateNotYetValid, certificate);
                errors += error;
                emit q->peerVerifyError(error);
            } else if (result == 1) {
                auto error = QSslError(QSslError::CertificateExpired, certificate);
                errors += error;
                emit q->peerVerifyError(error);
            }

            //// Errors
            if (element->TrustStatus.dwErrorStatus & CERT_TRUST_IS_NOT_TIME_VALID) {
                // handled right above
            }
            if (element->TrustStatus.dwErrorStatus & CERT_TRUST_IS_REVOKED) {
                auto error = QSslError(QSslError::CertificateRevoked, certificate);
                errors += error;
                emit q->peerVerifyError(error);
            }
            if (element->TrustStatus.dwErrorStatus & CERT_TRUST_IS_NOT_SIGNATURE_VALID) {
                auto error = QSslError(QSslError::CertificateSignatureFailed, certificate);
                errors += error;
                emit q->peerVerifyError(error);
            }

            // While netscape shouldn't be relevant now it still defined an extension which is
            // still in use in some cases (*cough*our tests*cough*). Schannel does not check this
            // automatically, so we'll do it manually. It is used to differentiate between client
            // and server certificates.
            auto netscapeIt = std::find_if(extensions.cbegin(), extensions.cend(),
                                           [](const QSslCertificateExtension &extension) {
                                               return extension.oid() == QStringLiteral("2.16.840.1.113730.1.1"); // Netscape cert type
                                           });
            if (netscapeIt != extensions.cend()) {
                qDebug() << "certificate actually has netscape cert type extension";
                const QByteArray netscapeCertTypeByte = netscapeIt->value().toByteArray();
                int netscapeCertType = 0;
                QDataStream(netscapeCertTypeByte) >> netscapeCertType;
                qDebug() << "Netscape cert type value:" << netscapeCertTypeByte << netscapeCertType;
                if ((isClient && (netscapeCertType & NETSCAPE_SSL_SERVER_AUTH_CERT_TYPE) == 0)
                    || (!isClient && (netscapeCertType & NETSCAPE_SSL_CLIENT_AUTH_CERT_TYPE) == 0)) {
                    qDebug() << "Netscape says: wrong usage";
                    element->TrustStatus.dwErrorStatus |= CERT_TRUST_IS_NOT_VALID_FOR_USAGE;
                }
            }
            if (element->TrustStatus.dwErrorStatus & CERT_TRUST_IS_NOT_VALID_FOR_USAGE) {
                auto error = QSslError(QSslError::InvalidPurpose, certificate);
                errors += error;
                emit q->peerVerifyError(error);
            }
            if (element->TrustStatus.dwErrorStatus & CERT_TRUST_IS_UNTRUSTED_ROOT) {
                bool isTrustedRoot = false;
                qDebug() << "cert is untrusted root";
                if (!isTrustedRoot && !configuration.caCertificates.isEmpty()) {
                    // Override this error if we have the certificate inside our trusted CAs list.
                    if (configuration.caCertificates.contains(certificate)) {
                        qDebug() << "It's a trusted CA";
                        isTrustedRoot = true;
                    }
                }
                if (!isTrustedRoot) {
                    auto error = QSslError(QSslError::CertificateUntrusted, certificate);
                    errors += error;
                    emit q->peerVerifyError(error);
                }
            }
            if (element->TrustStatus.dwErrorStatus & CERT_TRUST_IS_OFFLINE_REVOCATION
                || element->TrustStatus.dwErrorStatus & CERT_TRUST_REVOCATION_STATUS_UNKNOWN) {
                // @todo what am I supposed to do about that???
                qDebug() << "Revocation status unknown";
            }
            if (element->TrustStatus.dwErrorStatus & CERT_TRUST_IS_CYCLIC // Dumping ground of errors that don't fit our specific errors
                || element->TrustStatus.dwErrorStatus & CERT_TRUST_INVALID_EXTENSION
                || element->TrustStatus.dwErrorStatus & CERT_TRUST_INVALID_BASIC_CONSTRAINTS
                || element->TrustStatus.dwErrorStatus & CERT_TRUST_INVALID_NAME_CONSTRAINTS
                || element->TrustStatus.dwErrorStatus & CERT_TRUST_INVALID_POLICY_CONSTRAINTS
                || element->TrustStatus.dwErrorStatus & CERT_TRUST_HAS_EXCLUDED_NAME_CONSTRAINT
                || element->TrustStatus.dwErrorStatus & CERT_TRUST_HAS_NOT_SUPPORTED_CRITICAL_EXT
                || element->TrustStatus.dwErrorStatus & CERT_TRUST_HAS_NOT_DEFINED_NAME_CONSTRAINT
                || element->TrustStatus.dwErrorStatus & CERT_TRUST_HAS_NOT_PERMITTED_NAME_CONSTRAINT
                || element->TrustStatus.dwErrorStatus & CERT_TRUST_HAS_NOT_SUPPORTED_NAME_CONSTRAINT) {
                auto error = QSslError(QSslError::UnspecifiedError, certificate);
                errors += error;
                emit q->peerVerifyError(error);
            }
            if (element->TrustStatus.dwErrorStatus & CERT_TRUST_IS_EXPLICIT_DISTRUST) {
                // @todo Rejected, untrusted, blacklisted. They all fit!
                auto error = QSslError(QSslError::CertificateUntrusted, certificate);
                errors += error;
                emit q->peerVerifyError(error);
            }

            if (element->TrustStatus.dwInfoStatus & CERT_TRUST_IS_SELF_SIGNED) {
                qDebug() << "Self-signed certificate";
                bool isRootCertificateAuthority = false;
                auto it = std::find_if(extensions.cbegin(), extensions.cend(),
                                       [](const QSslCertificateExtension &extension) {
                                           return extension.name() == QLatin1String("basicConstraints");
                                       });
                if (it != extensions.cend()) {
                    QVariantMap basicConstraints = it->value().toMap();
                    qDebug() << "Basic constraints:" << basicConstraints;
                    // If it's self-signed *and* a CA then we can assume it's a root CA certificate
                    // and we can ignore the "self-signed" note
                    isRootCertificateAuthority = basicConstraints.value(QLatin1String("ca"), false).toBool();
                    qDebug() << "isRootCertificateAuthority" << isRootCertificateAuthority;
                } else if (certificate.version() == "1") {
                    // In version 1 there was no Basic Constraint... so we can only guess...........
                    // @todo: check spec if "self-signed" is the only way to tell.
                    isRootCertificateAuthority = true;
                    qDebug() << "version 1 certificate";
                    qDebug() << "isRootCertificateAuthority" << isRootCertificateAuthority;
                }

                // Root certificate tends to be signed by themselves, so ignore self-signed status.
                if (!isRootCertificateAuthority) {
                    auto error = QSslError(QSslError::SelfSignedCertificate, certificate);
                    errors += error;
                    emit q->peerVerifyError(error);
                }
            }
        }
    }

    if (!configuration.peerCertificateChain.isEmpty()) {
        configuration.peerCertificate = configuration.peerCertificateChain.first();

        qDebug() << "Leaf certificate's extensions:";
        for (auto extension : configuration.peerCertificate.extensions()) {
            qDebug() << "  " << extension.name() << "-" << extension.value();
        }
    }

    // @Note: Somewhat copied from qsslsocket_mac.cpp
    // Changes: dropped check for "!canIgnoreVerify" at positions: [0], [1]
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
            qDebug() << "peerName:" << peerName;
            if (!isMatchingHostname(configuration.peerCertificate, peerName)) { // [0]: here
                // No matches in common names or alternate names.
                const QSslError error(QSslError::HostNameMismatch, configuration.peerCertificate);
                errors += error;
                emit q->peerVerifyError(error);
            }
        }
    } else if (doVerifyPeer) { // [1]: and here
        // No peer certificate presented. Report as error if the socket
        // expected one.
        const QSslError error(QSslError::NoPeerCertificate);
        errors += error;
        emit q->peerVerifyError(error);
    }

    return errors;
}

QT_END_NAMESPACE
