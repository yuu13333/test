/****************************************************************************
**
** Copyright (C) 2017 The Qt Company Ltd.
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

#include "private/qnativesocketengine_p.h"

#include "qsslsocket_openssl_symbols_p.h"
#include "qdtlsconnection_openssl_p.h"
#include "qsslcontext_openssl_p.h"
#include "qdtlsconnection.h"
#include "qsslsocket.h"
#include "qssl_p.h"

#include "qmessageauthenticationcode.h"
#include "qcryptographichash.h"

#include "qnetworkdatagram_p.h"

#include "private/qthread_p.h"

#include "qdebug.h"


// DTLSTODO: Windows ....
#include <netinet/in.h>

#include <cstring>
#include <cstddef>

QT_BEGIN_NAMESPACE

namespace {

#if QT_CONFIG(opensslv11)

// Not here yet ...

#else // opensslv11

QByteArray qt_get_cookie_material_for_connection(SSL *ssl)
{
    Q_ASSERT(ssl);

    // SSL_get_rbio does not increment the reference count:
    BIO *readChannel = q_SSL_get_rbio(ssl);
    if (!readChannel) {
        qCWarning(lcSsl) << "failed to read peer's address";
        return {};
    }

    qt_sockaddr peer;
    if (q_BIO_dgram_get_peer(readChannel, &peer) <= 0) {
        qCWarning(lcSsl) << "BIO_dgram_get_peer failed";
        return {};
    }

    QByteArray peerData;
    char *dst = nullptr;
    switch (peer.a.sa_family) {
    case AF_INET:
        peerData.resize(int(sizeof(in_addr) + sizeof peer.a4.sin_port));
        dst = peerData.data();
        std::memcpy(dst, &peer.a4.sin_port, sizeof peer.a4.sin_port);
        dst += sizeof peer.a4.sin_port;
        std::memcpy(dst, &peer.a4.sin_addr, sizeof(in_addr));
        break;
    case AF_INET6:
        peerData.resize(int(sizeof(in6_addr) + sizeof peer.a6.sin6_port));
        dst = peerData.data();
        std::memcpy(dst, &peer.a6.sin6_port, sizeof peer.a6.sin6_port);
        dst += sizeof peer.a6.sin6_port;
        std::memcpy(dst, &peer.a6.sin6_addr, sizeof(in6_addr));
        break;
    default:;
    }

    return peerData;
}

void qt_set_BIO_connected(BIO *bio, const QHostAddress &peerAddress, quint16 peerPort)
{
    Q_ASSERT(bio);

    // qt_sockaddr is something similar to what OpenSSL 1.1 has in bio_addr_st.
    qt_sockaddr peer;
    if (peerAddress.protocol() == QAbstractSocket::IPv6Protocol) {
        memset(&peer.a, 0, sizeof(sockaddr_in6));
        peer.a6.sin6_family = AF_INET6;
        peer.a6.sin6_port = htons(peerPort);
        Q_IPV6ADDR tmp = peerAddress.toIPv6Address();
        memcpy(&peer.a6.sin6_addr, &tmp, sizeof(tmp));
        SetSALen::set(&peer.a6, sizeof(sockaddr_in6));
    } else if (peerAddress.protocol() == QAbstractSocket::IPv4Protocol) {
        memset(&peer.a, 0, sizeof(sockaddr_in));
        peer.a4.sin_family = AF_INET;
        peer.a4.sin_port = htons(peerPort);
        peer.a4.sin_addr.s_addr = htonl(peerAddress.toIPv4Address());
        SetSALen::set(&peer.a, sizeof(sockaddr_in));
    } else {
        Q_UNREACHABLE();
    }

    q_BIO_ctrl(bio, BIO_CTRL_DGRAM_SET_CONNECTED, 0, &peer);
}

#endif // !opensslv11

struct CookieSecret
{
    CookieSecret()
    {
        // DTLSTODO :where did I find this 16? Why is it hardcoded?
        key.resize(16);
        const int status = q_RAND_bytes(reinterpret_cast<unsigned char *>(key.data()),
                                        key.size());
        if (status <= 0)
            key.clear();
    }

    QByteArray key;

    Q_DISABLE_COPY(CookieSecret)
};

} // unnamed namespace

void SslConnectionGuard::SslDeleter::cleanup(SSL *ssl)
{
    if (ssl)
        q_SSL_free(ssl);
}

QDtlsConnectionOpenSSL::~QDtlsConnectionOpenSSL()
{
}

void QDtlsConnectionOpenSSL::readNotification()
{
    Q_Q(QDtlsConnection);

    if (encryptionState == QDtlsConnection::InHandshake) {
        continueHandshake();
        return;
    } else if (socketEngine->hasPendingDatagrams()) {
        if (encryptionState == QDtlsConnection::Encrypted) {
            emit q->readyRead();
        } else if (side == QDtlsConnection::Server) {
            if (!initTls()) {
                // DTLSTODO: set error/description, emit.
                qCWarning(lcSsl) << "cannot send a HelloVerifyRequest";
                return;
            }

            verifyClientHello();
        } // else - we ignore.
    }
}

void QDtlsConnectionOpenSSL::writeNotification()
{
    if (encryptionState == QDtlsConnection::InHandshake)
        continueHandshake();
}

void QDtlsConnectionOpenSSL::closeNotification()
{
    Q_UNIMPLEMENTED();
}

void QDtlsConnectionOpenSSL::exceptionNotification()
{
    Q_UNIMPLEMENTED();
}

void QDtlsConnectionOpenSSL::connectionNotification()
{
    Q_UNIMPLEMENTED();
}

void QDtlsConnectionOpenSSL::proxyAuthenticationRequired(const QNetworkProxy &proxy,
                                                         QAuthenticator *authenticator)
{
    Q_UNUSED(proxy) Q_UNUSED(authenticator)
    Q_UNIMPLEMENTED();
}

bool QDtlsConnectionOpenSSL::initTls()
{
    if (sslContext)
        return true;

    if (!QSslSocket::supportsSsl())
        return false;

    // create a deep copy of our configuration
    QSslConfigurationPrivate *configurationCopy = new QSslConfigurationPrivate(configuration);
    configurationCopy->ref.store(0); // the QSslConfiguration constructor refs up

    // DTLSTODO: check we do not set something DTLS-incompatible there ...
    SslContextGuard newContext(QSslContext::sharedFromConfiguration(sideToMode(), configurationCopy, true));
    if (!newContext) {
        qCWarning(lcSsl) << "QSslContext::sharedFromConfiguration failed";
        return false;
    }

    SslConnectionGuard newConnection(newContext.createSsl());
    if (!newConnection) {
        qCWarning(lcSsl) << "SSL_new failed";
        // DTLSTODO: be more specific why ...
        return false;
    }

    Q_ASSERT(socketEngine.data());
    BIO *bio = q_BIO_new_dgram(socketEngine->socketDescriptor(), BIO_NOCLOSE);
    if (!bio) {
        qCWarning(lcSsl) << "BIO_new_dgram failed";
        return false;
    }

    q_SSL_set_bio(newConnection, bio, bio);

    if (side == QDtlsConnection::Server) {
        q_SSL_CTX_set_cookie_generate_cb(newContext, generateCookieCallback);
        q_SSL_CTX_set_cookie_verify_cb(newContext, (pre11VerifyCallbackType)verifyCookieCallback);
        q_SSL_set_options(newConnection, SSL_OP_COOKIE_EXCHANGE);
    } else {
        Q_ASSERT(side == QDtlsConnection::Client);
        Q_ASSERT(socketState == QAbstractSocket::ConnectedState);
        qt_set_BIO_connected(q_SSL_get_rbio(newConnection), peerAddress, peerPort);
        // DTLSTODO: this is only temporarily!!! I need it for tests and
        // a proper solution ...
        SSL_set_options(newConnection, SSL_OP_NO_QUERY_MTU);
        DTLS_set_link_mtu(newConnection, 1024);
    }

    sslContext.swap(newContext);
    sslConnection.swap(newConnection);

    return true;
}

bool QDtlsConnectionOpenSSL::connectToHost(const QHostAddress &address, quint16 port)
{
    if (!QDtlsConnectionPrivate::connectToHost(address, port))
        return false;

    // Check for consistency!
    if (side == QDtlsConnection::Server) {
        Q_ASSERT(sslConnection);
        // SSL_get_rbio does not ref-up a counter.
        auto bio = q_SSL_get_rbio(sslConnection);
        Q_ASSERT(bio);
        qt_set_BIO_connected(bio, address, port);
    }

    return true;
}

bool QDtlsConnectionOpenSSL::startHandshake()
{
    Q_Q(QDtlsConnection);

    if (socketState != QAbstractSocket::ConnectedState) {
        qCWarning(lcSsl) << "cannot start handshake, must be in ConnectedState";
        return false;
    }

    if (encryptionState == QDtlsConnection::Encrypted) {
        qCWarning(lcSsl) << "cannot start handshake, already encrypted";
        return false;
    }

    if (!initTls()) {
        // initTls already reported an error.
        return false;
    }

    Q_ASSERT(sslConnection);

    encryptionState = QDtlsConnection::InHandshake;
    const bool isClient = side == QDtlsConnection::Client;
    const int result = isClient ? q_SSL_connect(sslConnection) : q_SSL_accept(sslConnection);

    if (result > 0) {
        encryptionState = QDtlsConnection::Encrypted;
        emit q->encrypted();
    } else {
        switch (q_SSL_get_error(sslConnection, result)) {
        case SSL_ERROR_WANT_READ:
        case SSL_ERROR_WANT_WRITE:
            // The handshake is not yet complete.
            break;
        default: {
            }
        }
    }

    return true;
}

bool QDtlsConnectionOpenSSL::continueHandshake()
{
    return startHandshake();
}

QSslSocket::SslMode QDtlsConnectionOpenSSL::sideToMode() const
{
    return side == QDtlsConnection::Server ? QSslSocket::SslServerMode
                                           : QSslSocket::SslClientMode;
}

#if QT_CONFIG(opensslv11)
void QDtlsConnectionOpenSSL::verifyClientHello()
{
}
#else // opensslv11

void QDtlsConnectionOpenSSL::verifyClientHello()
{
    Q_Q(QDtlsConnection);

    Q_ASSERT(sslConnection);

    qt_sockaddr peer;
    if (q_DTLSv1_listen(sslConnection, &peer) > 0) {
        QHostAddress address(&peer.a);
        if (address.isNull()) {
            // We somehow failed to extract/create address, nothing to connect
            // to, ignore.
            return;
        }

        quint16 port = 0;
        if (peer.a.sa_family == AF_INET) {
            port = peer.a4.sin_port;
        } else if (peer.a.sa_family == AF_INET6) {
            port = peer.a6.sin6_port;
        } else {
            // Ignore this datagram, that's something we don't accept/expect
            // or some error.
            return;
        }

        peerAddress = address;
        peerPort = ntohs(port);

        clientVerified = true;
        socketEngine->setReadNotificationEnabled(false);
        emit q->newConnection(peerAddress, peerPort);
    } // DTLSTODO els ...
}

#endif // !opensslv11

qint64 QDtlsConnectionOpenSSL::writeDatagram(const QByteArray &datagram)
{
    Q_ASSERT(encryptionState == QDtlsConnection::Encrypted);

    q_SSL_write(sslConnection, datagram.constData(), datagram.size());
    // DTLSTODO: q_SSL_write requires error handling.
    return datagram.size();
}

qint64 QDtlsConnectionOpenSSL::readDatagram(QByteArray *datagram)
{
    Q_ASSERT(datagram);
    Q_ASSERT(encryptionState == QDtlsConnection::Encrypted);

    datagram->resize(socketEngine->pendingDatagramSize());
    const int read = q_SSL_read(sslConnection, datagram->data(), datagram->size());
    if (read >= 0)
        datagram->resize(read);
    else
        datagram->clear();
    // DTLSTODO: Error handling here!
    return read;
}

int QDtlsConnectionOpenSSL::generateCookieCallback(SSL *ssl, unsigned char *dst,
                                                   unsigned *cookieLength)
{
    if (!ssl || !dst || !cookieLength) {
        qCWarning(lcSsl)
             << "failed to generate cookie - invalid (nullptr) parameter(s)";
        return 0;
    }

    *cookieLength = 0;

    static CookieSecret secret;
    if (!secret.key.size())
        return 0;

    const QByteArray peerData = qt_get_cookie_material_for_connection(ssl);
    if (!peerData.size())
        return 0;

    QMessageAuthenticationCode hmac(QCryptographicHash::Sha1, secret.key);
    hmac.addData(peerData);
    const QByteArray cookie = hmac.result();
    Q_ASSERT(cookie.size() >= 0);
    // OpenSSL docs don't say what's the max possible size, but dtls1_state_st
    // has a data-member 'cookie' which has this size - DTLS1_COOKIE_LENGTH and
    // &cookie[0] is what they pass into our callback. That's also what we
    // do when verifying a cookie.
    if (Q_UNLIKELY(!cookie.size() || cookie.size() > DTLS1_COOKIE_LENGTH)) {
        qCWarning(lcSsl) << "HMAC does not fit in DTLS1_COOKIE_LENGTH";
        return 0;
    }

    std::memcpy(dst, cookie.constData(), cookie.size());
    *cookieLength = cookie.size();

    return 1;
}

int QDtlsConnectionOpenSSL::verifyCookieCallback(SSL *ssl, const unsigned char *cookie,
                                                 unsigned cookieLength)
{
    if (!ssl || !cookie || !cookieLength) {
        qCWarning(lcSsl)
            << "could not verify cookie, one of input parameters is not valid";
        return 0;
    }

    unsigned char newCookie[DTLS1_COOKIE_LENGTH] = {};
    unsigned newCookieLength = 0;
    if (generateCookieCallback(ssl, newCookie, &newCookieLength) != 1)
        return 0;

    if (newCookieLength == cookieLength
        && !std::memcmp(cookie, newCookie, cookieLength))
        return 1;

    return 0;
}

QT_END_NAMESPACE
