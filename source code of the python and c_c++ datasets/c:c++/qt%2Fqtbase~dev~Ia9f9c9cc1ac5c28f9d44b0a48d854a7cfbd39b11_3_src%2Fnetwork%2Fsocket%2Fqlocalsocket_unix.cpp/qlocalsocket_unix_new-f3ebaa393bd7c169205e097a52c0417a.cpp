/****************************************************************************
**
** Copyright (C) 2016 The Qt Company Ltd.
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

#include "qlocalsocket.h"
#include "qlocalsocket_p.h"
#include "qnet_unix_p.h"

#include <sys/types.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>

#include <qdir.h>
#include <qdebug.h>
#include <qelapsedtimer.h>

#ifdef Q_OS_VXWORKS
#  include <selectLib.h>
#endif

#define QT_CONNECT_TIMEOUT 30000

namespace {

#if defined(Q_OS_LINUX)
//Connecting to abstract socket possible states
enum class AbstractSocketState
{
    Undefined,
    Connecting,
    Ok,
    Retry,
    Error
};
AbstractSocketState abstractConnectStatus = AbstractSocketState::Undefined;
#endif

// determine the full server path
QString GetPathNameForConnection(const QString connectingName)
{
    auto connectingPathName = connectingName;
    if (!connectingName.startsWith(QLatin1Char('/'))) {
        connectingPathName = QDir::tempPath();
        connectingPathName += QLatin1Char('/') + connectingName;
    }
    return connectingPathName;
}

}

QT_BEGIN_NAMESPACE

QLocalSocketPrivate::QLocalSocketPrivate() : QIODevicePrivate(),
        delayConnect(nullptr),
        connectTimer(nullptr),
        connectingSocket(-1),
        state(QLocalSocket::UnconnectedState)
{
}

void QLocalSocketPrivate::init()
{
    Q_Q(QLocalSocket);
    // QIODevice signals
    q->connect(&unixSocket, SIGNAL(aboutToClose()), q, SIGNAL(aboutToClose()));
    q->connect(&unixSocket, SIGNAL(bytesWritten(qint64)),
               q, SIGNAL(bytesWritten(qint64)));
    q->connect(&unixSocket, SIGNAL(readyRead()), q, SIGNAL(readyRead()));
    // QAbstractSocket signals
    q->connect(&unixSocket, SIGNAL(connected()), q, SIGNAL(connected()));
    q->connect(&unixSocket, SIGNAL(disconnected()), q, SIGNAL(disconnected()));
    q->connect(&unixSocket, SIGNAL(stateChanged(QAbstractSocket::SocketState)),
               q, SLOT(_q_stateChanged(QAbstractSocket::SocketState)));
    q->connect(&unixSocket, SIGNAL(errorOccurred(QAbstractSocket::SocketError)),
               q, SLOT(_q_errorOccurred(QAbstractSocket::SocketError)));
    q->connect(&unixSocket, SIGNAL(readChannelFinished()), q, SIGNAL(readChannelFinished()));
    unixSocket.setParent(q);
}

void QLocalSocketPrivate::_q_errorOccurred(QAbstractSocket::SocketError socketError)
{
    Q_Q(QLocalSocket);
    QString function = QLatin1String("QLocalSocket");
    QLocalSocket::LocalSocketError error = (QLocalSocket::LocalSocketError)socketError;
    QString errorString = generateErrorString(error, function);
    q->setErrorString(errorString);
    emit q->errorOccurred(error);
}

void QLocalSocketPrivate::_q_stateChanged(QAbstractSocket::SocketState newState)
{
    Q_Q(QLocalSocket);
    QLocalSocket::LocalSocketState currentState = state;
    switch(newState) {
    case QAbstractSocket::UnconnectedState:
        state = QLocalSocket::UnconnectedState;
        serverName.clear();
        fullServerName.clear();
        break;
    case QAbstractSocket::ConnectingState:
        state = QLocalSocket::ConnectingState;
        break;
    case QAbstractSocket::ConnectedState:
        state = QLocalSocket::ConnectedState;
        break;
    case QAbstractSocket::ClosingState:
        state = QLocalSocket::ClosingState;
        break;
    default:
#if defined QLOCALSOCKET_DEBUG
        qWarning() << "QLocalSocket::Unhandled socket state change:" << newState;
#endif
        return;
    }
    if (currentState != state)
        emit q->stateChanged(state);
}

QString QLocalSocketPrivate::generateErrorString(QLocalSocket::LocalSocketError error, const QString &function) const
{
    QString errorString;
    switch (error) {
    case QLocalSocket::ConnectionRefusedError:
        errorString = QLocalSocket::tr("%1: Connection refused").arg(function);
        break;
    case QLocalSocket::PeerClosedError:
        errorString = QLocalSocket::tr("%1: Remote closed").arg(function);
        break;
    case QLocalSocket::ServerNotFoundError:
        errorString = QLocalSocket::tr("%1: Invalid name").arg(function);
        break;
    case QLocalSocket::SocketAccessError:
        errorString = QLocalSocket::tr("%1: Socket access error").arg(function);
        break;
    case QLocalSocket::SocketResourceError:
        errorString = QLocalSocket::tr("%1: Socket resource error").arg(function);
        break;
    case QLocalSocket::SocketTimeoutError:
        errorString = QLocalSocket::tr("%1: Socket operation timed out").arg(function);
        break;
    case QLocalSocket::DatagramTooLargeError:
        errorString = QLocalSocket::tr("%1: Datagram too large").arg(function);
        break;
    case QLocalSocket::ConnectionError:
        errorString = QLocalSocket::tr("%1: Connection error").arg(function);
        break;
    case QLocalSocket::UnsupportedSocketOperationError:
        errorString = QLocalSocket::tr("%1: The socket operation is not supported").arg(function);
        break;
    case QLocalSocket::OperationError:
        errorString = QLocalSocket::tr("%1: Operation not permitted when socket is in this state").arg(function);
        break;
    case QLocalSocket::UnknownSocketError:
    default:
        errorString = QLocalSocket::tr("%1: Unknown error %2").arg(function).arg(errno);
    }
    return errorString;
}

void QLocalSocketPrivate::setErrorAndEmit(QLocalSocket::LocalSocketError error, const QString &function)
{
    Q_Q(QLocalSocket);
    switch (error) {
    case QLocalSocket::ConnectionRefusedError:
        unixSocket.setSocketError(QAbstractSocket::ConnectionRefusedError);
        break;
    case QLocalSocket::PeerClosedError:
        unixSocket.setSocketError(QAbstractSocket::RemoteHostClosedError);
        break;
    case QLocalSocket::ServerNotFoundError:
        unixSocket.setSocketError(QAbstractSocket::HostNotFoundError);
        break;
    case QLocalSocket::SocketAccessError:
        unixSocket.setSocketError(QAbstractSocket::SocketAccessError);
        break;
    case QLocalSocket::SocketResourceError:
        unixSocket.setSocketError(QAbstractSocket::SocketResourceError);
        break;
    case QLocalSocket::SocketTimeoutError:
        unixSocket.setSocketError(QAbstractSocket::SocketTimeoutError);
        break;
    case QLocalSocket::DatagramTooLargeError:
        unixSocket.setSocketError(QAbstractSocket::DatagramTooLargeError);
        break;
    case QLocalSocket::ConnectionError:
        unixSocket.setSocketError(QAbstractSocket::NetworkError);
        break;
    case QLocalSocket::UnsupportedSocketOperationError:
        unixSocket.setSocketError(QAbstractSocket::UnsupportedSocketOperationError);
        break;
    case QLocalSocket::UnknownSocketError:
    default:
        unixSocket.setSocketError(QAbstractSocket::UnknownSocketError);
    }

    QString errorString = generateErrorString(error, function);
    q->setErrorString(errorString);
    emit q->errorOccurred(error);

    // errors cause a disconnect
    unixSocket.setSocketState(QAbstractSocket::UnconnectedState);
    bool stateChanged = (state != QLocalSocket::UnconnectedState);
    state = QLocalSocket::UnconnectedState;
    q->close();
    if (stateChanged)
        q->emit stateChanged(state);
}

void QLocalSocket::connectToServer(OpenMode openMode)
{
    Q_D(QLocalSocket);
    if (state() == ConnectedState || state() == ConnectingState) {
        QString errorString = d->generateErrorString(QLocalSocket::OperationError, QLatin1String("QLocalSocket::connectToserver"));
        setErrorString(errorString);
        emit errorOccurred(QLocalSocket::OperationError);
        return;
    }

    d->errorString.clear();
    d->unixSocket.setSocketState(QAbstractSocket::ConnectingState);
    d->state = ConnectingState;
    emit stateChanged(d->state);

    if (d->serverName.isEmpty()) {
        d->setErrorAndEmit(ServerNotFoundError,
                           QLatin1String("QLocalSocket::connectToServer"));
        return;
    }

    // create the socket
    if (-1 == (d->connectingSocket = qt_safe_socket(PF_UNIX, SOCK_STREAM, 0, O_NONBLOCK))) {
        d->setErrorAndEmit(UnsupportedSocketOperationError,
                           QLatin1String("QLocalSocket::connectToServer"));
        return;
    }

    // _q_connectToSocket does the actual connecting
    d->connectingName = d->serverName;
    d->connectingOpenMode = openMode;
    d->_q_connectToSocket();
    return;
}

/*!
    \internal

    Tries to connect connectingName and connectingOpenMode
    In case of Linux or Android it tries to connect to an
    abstract socket, and if this fails connects to a regular one

    \sa connectToServer(), waitForConnected()
  */

void QLocalSocketPrivate::_q_connectToSocket()
{

#if defined(Q_OS_LINUX)
    abstractConnectStatus = ::AbstractSocketState::Connecting;
    _q_connectToAbstractSocket();
    if (abstractConnectStatus == ::AbstractSocketState::Error)
        _q_connectToRegularSocket();
#else
    _q_connectToRegularSocket();
#endif
}

#if defined(Q_OS_LINUX)
/*!
    \internal

    Tries to connect connectingName and connectingOpenMode. The socket will be
    an abstract one. This is specific to Linux and Android sockets.

    \sa connectToServer(), waitForConnected()
*/

void QLocalSocketPrivate::_q_connectToAbstractSocket()
{
    Q_Q(QLocalSocket);
    // determine the full server path
    const auto connectingPathName = ::GetPathNameForConnection(connectingName);
    const QByteArray encodedConnectingPathName = QFile::encodeName(connectingPathName);
    struct ::sockaddr_un addr = {PF_UNIX, {}};

    // add 2 to length, to take into account trailing AND leading null
    if (sizeof(addr.sun_path) < (uint)encodedConnectingPathName.size() + 2) {
        QString function = QLatin1String("QLocalSocket::connectToServer");
        setErrorAndEmit(QLocalSocket::ServerNotFoundError, function);
        return;
    }
    ::memcpy(addr.sun_path + 1, encodedConnectingPathName.constData(),
             encodedConnectingPathName.size() + 1);

    const int addrSize = strlen(&addr.sun_path[1]) + sizeof(sa_family_t) + 1;

    int errorCode = 0;
    if (-1 == qt_safe_connect(connectingSocket, (struct sockaddr *)&addr, addrSize)) {
        QString function = QLatin1String("QLocalSocket::connectToServer");
        errorCode = errno;
        if (errorCode == EAGAIN) {
            if (!delayConnect) {
                delayConnect = new QSocketNotifier(connectingSocket, QSocketNotifier::Write, q);
                q->connect(delayConnect, SIGNAL(activated(QSocketDescriptor)), q, SLOT(_q_connectToAbstractSocket()));
            }
            if (!connectTimer) {
                connectTimer = new QTimer(q);
                q->connect(connectTimer, SIGNAL(timeout()),
                           q, SLOT(_q_abortConnectionAttempt()),
                           Qt::DirectConnection);
                connectTimer->start(QT_CONNECT_TIMEOUT);
            }
            delayConnect->setEnabled(true);
            abstractConnectStatus = ::AbstractSocketState::Retry;
        } else {
            abstractConnectStatus = ::AbstractSocketState::Error;
        }
        return;
    }

    // connected!
    cancelDelayedConnect();

    serverName = connectingName;
    fullServerName = connectingPathName;
    if (unixSocket.setSocketDescriptor(connectingSocket,
        QAbstractSocket::ConnectedState, connectingOpenMode)) {
        q->QIODevice::open(connectingOpenMode | QIODevice::Unbuffered);
        q->emit connected();
        abstractConnectStatus = ::AbstractSocketState::Ok;
    } else {
        QString function = QLatin1String("QLocalSocket::connectToServer");
        abstractConnectStatus = ::AbstractSocketState::Undefined;
        setErrorAndEmit(QLocalSocket::UnknownSocketError, function);
    }
    connectingSocket = -1;
    connectingName.clear();
    connectingOpenMode = { };
}

//
#endif

/*!
    \internal

    Tries to connect connectingName and connectingOpenMode. The socket will be
    a non-abstract.

    \sa connectToServer(), waitForConnected()
  */
void QLocalSocketPrivate::_q_connectToRegularSocket()
{
    Q_Q(QLocalSocket);
    const auto connectingPathName = ::GetPathNameForConnection(connectingName);
    const QByteArray encodedConnectingPathName = QFile::encodeName(connectingPathName);
    struct ::sockaddr_un addr = {PF_UNIX, {}};

    if (sizeof(addr.sun_path) < (uint)encodedConnectingPathName.size() + 1) {
        QString function = QLatin1String("QLocalSocket::connectToServer");
        setErrorAndEmit(QLocalSocket::ServerNotFoundError, function);
        return;
    }
    ::memcpy(addr.sun_path, encodedConnectingPathName.constData(),
             encodedConnectingPathName.size() + 1);
    if (-1 == qt_safe_connect(connectingSocket, (struct sockaddr *)&addr, sizeof(addr))) {
        QString function = QLatin1String("QLocalSocket::connectToServer");
        switch (errno)
        {
        case EINVAL:
        case ECONNREFUSED:
            setErrorAndEmit(QLocalSocket::ConnectionRefusedError, function);
            break;
        case ENOENT:
            setErrorAndEmit(QLocalSocket::ServerNotFoundError, function);
            break;
        case EACCES:
        case EPERM:
            setErrorAndEmit(QLocalSocket::SocketAccessError, function);
            break;
        case ETIMEDOUT:
            setErrorAndEmit(QLocalSocket::SocketTimeoutError, function);
            break;
        case EAGAIN:
            // Try again later, all of the sockets listening are full
            if (!delayConnect) {
                delayConnect = new QSocketNotifier(connectingSocket, QSocketNotifier::Write, q);
                q->connect(delayConnect, SIGNAL(activated(QSocketDescriptor)), q, SLOT(_q_connectToRegularSocket()));
            }
            if (!connectTimer) {
                connectTimer = new QTimer(q);
                q->connect(connectTimer, SIGNAL(timeout()),
                                 q, SLOT(_q_abortConnectionAttempt()),
                                 Qt::DirectConnection);
                connectTimer->start(QT_CONNECT_TIMEOUT);
            }
            delayConnect->setEnabled(true);
            break;
        default:
            setErrorAndEmit(QLocalSocket::UnknownSocketError, function);
        }
        return;
    }

    // connected!
    cancelDelayedConnect();

    serverName = connectingName;
    fullServerName = connectingPathName;
    if (unixSocket.setSocketDescriptor(connectingSocket,
        QAbstractSocket::ConnectedState, connectingOpenMode)) {
        q->QIODevice::open(connectingOpenMode | QIODevice::Unbuffered);
        q->emit connected();
    } else {
        QString function = QLatin1String("QLocalSocket::connectToServer");
        setErrorAndEmit(QLocalSocket::UnknownSocketError, function);
    }
    connectingSocket = -1;
    connectingName.clear();
    connectingOpenMode = { };
}

bool QLocalSocket::setSocketDescriptor(qintptr socketDescriptor,
        LocalSocketState socketState, OpenMode openMode)
{
    Q_D(QLocalSocket);
    QAbstractSocket::SocketState newSocketState = QAbstractSocket::UnconnectedState;
    switch (socketState) {
    case ConnectingState:
        newSocketState = QAbstractSocket::ConnectingState;
        break;
    case ConnectedState:
        newSocketState = QAbstractSocket::ConnectedState;
        break;
    case ClosingState:
        newSocketState = QAbstractSocket::ClosingState;
        break;
    case UnconnectedState:
        newSocketState = QAbstractSocket::UnconnectedState;
        break;
    }
    QIODevice::open(openMode);
    d->state = socketState;
    return d->unixSocket.setSocketDescriptor(socketDescriptor,
                                             newSocketState, openMode);
}

void QLocalSocketPrivate::_q_abortConnectionAttempt()
{
    Q_Q(QLocalSocket);
    q->close();
}

void QLocalSocketPrivate::cancelDelayedConnect()
{
    if (delayConnect) {
        delayConnect->setEnabled(false);
        delete delayConnect;
        delayConnect = nullptr;
        connectTimer->stop();
        delete connectTimer;
        connectTimer = nullptr;
    }
}

qintptr QLocalSocket::socketDescriptor() const
{
    Q_D(const QLocalSocket);
    return d->unixSocket.socketDescriptor();
}

qint64 QLocalSocket::readData(char *data, qint64 c)
{
    Q_D(QLocalSocket);
    return d->unixSocket.read(data, c);
}

qint64 QLocalSocket::skipData(qint64 maxSize)
{
    return d_func()->unixSocket.skip(maxSize);
}

qint64 QLocalSocket::writeData(const char *data, qint64 c)
{
    Q_D(QLocalSocket);
    return d->unixSocket.writeData(data, c);
}

void QLocalSocket::abort()
{
    Q_D(QLocalSocket);
    d->unixSocket.abort();
}

qint64 QLocalSocket::bytesAvailable() const
{
    Q_D(const QLocalSocket);
    return QIODevice::bytesAvailable() + d->unixSocket.bytesAvailable();
}

qint64 QLocalSocket::bytesToWrite() const
{
    Q_D(const QLocalSocket);
    return d->unixSocket.bytesToWrite();
}

bool QLocalSocket::canReadLine() const
{
    Q_D(const QLocalSocket);
    return QIODevice::canReadLine() || d->unixSocket.canReadLine();
}

void QLocalSocket::close()
{
    Q_D(QLocalSocket);
    d->unixSocket.close();
    d->cancelDelayedConnect();
    if (d->connectingSocket != -1)
        ::close(d->connectingSocket);
    d->connectingSocket = -1;
    d->connectingName.clear();
    d->connectingOpenMode = { };
    d->serverName.clear();
    d->fullServerName.clear();
    QIODevice::close();
}

bool QLocalSocket::waitForBytesWritten(int msecs)
{
    Q_D(QLocalSocket);
    return d->unixSocket.waitForBytesWritten(msecs);
}

bool QLocalSocket::flush()
{
    Q_D(QLocalSocket);
    return d->unixSocket.flush();
}

void QLocalSocket::disconnectFromServer()
{
    Q_D(QLocalSocket);
    d->unixSocket.disconnectFromHost();
}

QLocalSocket::LocalSocketError QLocalSocket::error() const
{
    Q_D(const QLocalSocket);
    switch (d->unixSocket.error()) {
    case QAbstractSocket::ConnectionRefusedError:
        return QLocalSocket::ConnectionRefusedError;
    case QAbstractSocket::RemoteHostClosedError:
        return QLocalSocket::PeerClosedError;
    case QAbstractSocket::HostNotFoundError:
        return QLocalSocket::ServerNotFoundError;
    case QAbstractSocket::SocketAccessError:
        return QLocalSocket::SocketAccessError;
    case QAbstractSocket::SocketResourceError:
        return QLocalSocket::SocketResourceError;
    case QAbstractSocket::SocketTimeoutError:
        return QLocalSocket::SocketTimeoutError;
    case QAbstractSocket::DatagramTooLargeError:
        return QLocalSocket::DatagramTooLargeError;
    case QAbstractSocket::NetworkError:
        return QLocalSocket::ConnectionError;
    case QAbstractSocket::UnsupportedSocketOperationError:
        return QLocalSocket::UnsupportedSocketOperationError;
    case QAbstractSocket::UnknownSocketError:
        return QLocalSocket::UnknownSocketError;
    default:
#if defined QLOCALSOCKET_DEBUG
        qWarning() << "QLocalSocket error not handled:" << d->unixSocket.error();
#endif
        break;
    }
    return UnknownSocketError;
}

bool QLocalSocket::isValid() const
{
    Q_D(const QLocalSocket);
    return d->unixSocket.isValid();
}

qint64 QLocalSocket::readBufferSize() const
{
    Q_D(const QLocalSocket);
    return d->unixSocket.readBufferSize();
}

void QLocalSocket::setReadBufferSize(qint64 size)
{
    Q_D(QLocalSocket);
    d->unixSocket.setReadBufferSize(size);
}

bool QLocalSocket::waitForConnected(int msec)
{
    Q_D(QLocalSocket);

    if (state() != ConnectingState)
        return (state() == ConnectedState);

    QElapsedTimer timer;
    timer.start();

    pollfd pfd = qt_make_pollfd(d->connectingSocket, POLLIN);

    do {
        const int timeout = (msec > 0) ? qMax(msec - timer.elapsed(), Q_INT64_C(0)) : msec;
        const int result = qt_poll_msecs(&pfd, 1, timeout);

        if (result == -1)
            d->setErrorAndEmit(QLocalSocket::UnknownSocketError,
                               QLatin1String("QLocalSocket::waitForConnected"));
        else if (result > 0)
            d->_q_connectToSocket();
    } while (state() == ConnectingState && !timer.hasExpired(msec));

    return (state() == ConnectedState);
}

bool QLocalSocket::waitForDisconnected(int msecs)
{
    Q_D(QLocalSocket);
    if (state() == UnconnectedState) {
        qWarning("QLocalSocket::waitForDisconnected() is not allowed in UnconnectedState");
        return false;
    }
    return (d->unixSocket.waitForDisconnected(msecs));
}

bool QLocalSocket::waitForReadyRead(int msecs)
{
    Q_D(QLocalSocket);
    if (state() == QLocalSocket::UnconnectedState)
        return false;
    return (d->unixSocket.waitForReadyRead(msecs));
}

QT_END_NAMESPACE
