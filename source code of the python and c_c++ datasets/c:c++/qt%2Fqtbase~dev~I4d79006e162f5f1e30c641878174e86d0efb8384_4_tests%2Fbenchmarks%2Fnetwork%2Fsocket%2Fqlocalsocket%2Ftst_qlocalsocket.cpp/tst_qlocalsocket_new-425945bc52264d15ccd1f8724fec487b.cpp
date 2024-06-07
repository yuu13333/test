/****************************************************************************
**
** Copyright (C) 2020 The Qt Company Ltd.
** Contact: https://www.qt.io/licensing/
**
** This file is part of the test suite of the Qt Toolkit.
**
** $QT_BEGIN_LICENSE:GPL-EXCEPT$
** Commercial License Usage
** Licensees holding valid commercial Qt licenses may use this file in
** accordance with the commercial license agreement provided with the
** Software or, alternatively, in accordance with the terms contained in
** a written agreement between you and The Qt Company. For licensing terms
** and conditions see https://www.qt.io/terms-conditions. For further
** information use the contact form at https://www.qt.io/contact-us.
**
** GNU General Public License Usage
** Alternatively, this file may be used under the terms of the GNU
** General Public License version 3 as published by the Free Software
** Foundation with exceptions as appearing in the file LICENSE.GPL3-EXCEPT
** included in the packaging of this file. Please review the following
** information to ensure the GNU General Public License requirements will
** be met: https://www.gnu.org/licenses/gpl-3.0.html.
**
** $QT_END_LICENSE$
**
****************************************************************************/

#include <QtTest/QtTest>
#include <QtCore/qglobal.h>
#include <QtCore/qthread.h>
#include <QtCore/qbytearray.h>
#include <QtCore/qeventloop.h>
#include <QtCore/qvector.h>
#include <QtCore/qelapsedtimer.h>
#include <QtNetwork/qlocalsocket.h>
#include <QtNetwork/qlocalserver.h>

class tst_QLocalSocket : public QObject
{
    Q_OBJECT

private slots:
    void pingPong_data();
    void pingPong();
    void dataExchange_data();
    void dataExchange();
};

class ServerThread : public QThread
{
public:
    explicit ServerThread(int chunkSize)
    {
        buffer.resize(chunkSize);
    }

    void run() override
    {
        QLocalServer *server = new QLocalServer();

        connect(server, &QLocalServer::newConnection, [this, server]() {
            auto socket = server->nextPendingConnection();

            connect(socket, &QLocalSocket::readyRead, [this, socket]() {
                const qint64 bytesAvailable = socket->bytesAvailable();

                QCOMPARE(socket->read(this->buffer.data(), bytesAvailable), bytesAvailable);
                QCOMPARE(socket->write(this->buffer.data(), bytesAvailable), bytesAvailable);
            });
        });

        server->listen("foo");
        exec();
        delete server;
    }

protected:
    QByteArray buffer;
};

class SocketFactory : public QObject
{
    Q_OBJECT

public:
    explicit SocketFactory(int chunkSize, int connections, qint64 bytesToTransfer)
        : numberOfSockets(connections)
    {
        buffer.resize(chunkSize);
        bytesToRead.fill(qMax(1, bytesToTransfer / chunkSize) * chunkSize, connections);
        for (int i = 0; i < connections; ++i) {
            QLocalSocket *socket = new QLocalSocket(this);
            Q_CHECK_PTR(socket);

            connect(this, &SocketFactory::start, [this, socket]() {
               QCOMPARE(socket->write(this->buffer.data(), this->buffer.size()),
                                      this->buffer.size());
            });

            connect(socket, &QLocalSocket::readyRead, [i, this, socket]() {
                const qint64 bytesAvailable = socket->bytesAvailable();

                QCOMPARE(socket->read(this->buffer.data(), bytesAvailable), bytesAvailable);
                this->bytesToRead[i] -= bytesAvailable;
                if (this->bytesToRead[i] == 0) {
                    if (--this->numberOfSockets == 0)
                        this->eventLoop.quit();
                } else {
                    QCOMPARE(socket->write(this->buffer.data(), this->buffer.size()),
                                           this->buffer.size());
                }
            });

            while (socket->state() != QLocalSocket::ConnectedState)
                socket->connectToServer("foo");
        }
    }

    void run()
    {
        emit start();
        eventLoop.exec();
    }

signals:
    void start();

protected:
    QByteArray buffer;
    QEventLoop eventLoop;
    QVector<qint64> bytesToRead;
    int numberOfSockets;
};

void tst_QLocalSocket::pingPong_data()
{
    QTest::addColumn<int>("connections");
    for (int value : {10, 50, 100, 1000, 10000})
        QTest::addRow("connections: %d", value) << value;
}

void tst_QLocalSocket::pingPong()
{
    QFETCH(int, connections);

    const int iterations = 100000;

    ServerThread serverThread(1);
    serverThread.start();
    SocketFactory factory(1, connections, qMax(1, iterations / connections));

    QElapsedTimer timer;
    timer.start();

    factory.run();

    qDebug("Elapsed time: %.1f s", timer.elapsed() / 1000.0);
    serverThread.quit();
    serverThread.wait();
}

void tst_QLocalSocket::dataExchange_data()
{
    QTest::addColumn<int>("connections");
    QTest::addColumn<int>("chunkSize");
    for (int connections : {1, 5, 10}) {
        for (int chunkSize : {100, 1000, 10000, 100000 }) {
            QTest::addRow("connections: %d, chunk size: %d",
                          connections, chunkSize) << connections << chunkSize;
        }
    }
}

void tst_QLocalSocket::dataExchange()
{
    QFETCH(int, connections);
    QFETCH(int, chunkSize);

    const qint64 iterations = 50000;

    ServerThread serverThread(chunkSize);
    serverThread.start();
    SocketFactory factory(chunkSize, connections, iterations * chunkSize / connections);

    QElapsedTimer timer;
    timer.start();

    factory.run();

    qDebug("Elapsed time: %.1f s", timer.elapsed() / 1000.0);
    serverThread.quit();
    serverThread.wait();
}

QTEST_MAIN(tst_QLocalSocket)

#include "tst_qlocalsocket.moc"
