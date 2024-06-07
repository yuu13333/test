/****************************************************************************
**
** Copyright (C) 2021 The Qt Company Ltd.
** Contact: https://www.qt.io/licensing/
**
** This file is part of the QtCore module of the Qt Toolkit.
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

#include "qwindowspipereader_p.h"
#include "qiodevice_p.h"
#include <qelapsedtimer.h>
#include <qdeadlinetimer.h>
#include <qscopedvaluerollback.h>
#include <qcoreapplication.h>
#include <QMutexLocker>

QT_BEGIN_NAMESPACE

static const DWORD minReadBufferSize = 4096;

QWindowsPipeReader::QWindowsPipeReader(QObject *parent)
    : QObject(parent),
      handle(INVALID_HANDLE_VALUE),
      eventHandle(CreateEvent(NULL, FALSE, FALSE, NULL)),
      syncHandle(CreateEvent(NULL, TRUE, FALSE, NULL)),
      waitObject(NULL),
      readBufferMaxSize(0),
      actualReadBufferSize(0),
      pendingReadBytes(0),
      lastError(ERROR_SUCCESS),
      state(Stopped),
      readSequenceStarted(false),
      pipeBroken(false),
      readyReadPending(false),
      winEventActPosted(false),
      inReadyRead(false)
{
    ZeroMemory(&overlapped, sizeof(OVERLAPPED));
    overlapped.hEvent = eventHandle;
    waitObject = CreateThreadpoolWait(waitCallback, this, NULL);
    if (waitObject == NULL)
        qErrnoWarning("QWindowsPipeReader: CreateThreadpollWait failed.");
}

QWindowsPipeReader::~QWindowsPipeReader()
{
    stop();

    // Wait for thread pool callback to complete. It can be still
    // executing some completion code.
    WaitForThreadpoolWaitCallbacks(waitObject, FALSE);
    CloseThreadpoolWait(waitObject);
    CloseHandle(eventHandle);
    CloseHandle(syncHandle);
}

/*!
    Sets the handle to read from. The handle must be valid.
    Do not call this function while the pipe is running.
 */
void QWindowsPipeReader::setHandle(HANDLE hPipeReadEnd)
{
    readBuffer.clear();
    actualReadBufferSize = 0;
    readyReadPending = false;
    pendingReadBytes = 0;
    handle = hPipeReadEnd;
    pipeBroken = false;
    lastError = ERROR_SUCCESS;
}

/*!
    Stops the asynchronous read sequence.
    If the read sequence is running then the I/O operation is canceled.
 */
void QWindowsPipeReader::stop()
{
    cancelAsyncRead(Stopped);
}

/*!
    Stops the asynchronous read sequence.
    Reads all pending bytes into the internal buffer.
 */
void QWindowsPipeReader::drainAndStop()
{
    cancelAsyncRead(Draining);

    // Note that signals are not emitted in the call below, as the caller
    // is expected to do that synchronously.
    consumePendingResults(false);
}

/*!
    Stops the asynchronous read sequence.
 */
void QWindowsPipeReader::cancelAsyncRead(State newState)
{
    if (state != Running)
        return;

    QMutexLocker locker(&mutex);
    state = newState;
    if (readSequenceStarted) {
        consumePending(false);
        // This can legitimately fail due to the GetOverlappedResult()
        // in the callback not being locked. We ignore ERROR_NOT_FOUND
        // in this case.
        if (!CancelIoEx(handle, &overlapped)) {
            const DWORD dwError = GetLastError();
            if (dwError != ERROR_NOT_FOUND) {
                qErrnoWarning(dwError, "QWindowsPipeReader: CancelIoEx on handle %p failed.",
                              handle);
            }
        }
        locker.unlock();
        waitForNotification(-1);
    }
}

/*!
    Sets the size of internal read buffer.
 */
void QWindowsPipeReader::setMaxReadBufferSize(qint64 size)
{
    QMutexLocker locker(&mutex);
    readBufferMaxSize = size;
}

/*!
    Returns the number of bytes we've read so far.
 */
qint64 QWindowsPipeReader::bytesAvailable() const
{
    return actualReadBufferSize;
}

/*!
    Copies at most \c{maxlen} bytes from the internal read buffer to \c{data}.
 */
qint64 QWindowsPipeReader::read(char *data, qint64 maxlen)
{
    if (pipeBroken && actualReadBufferSize == 0)
        return 0;  // signal EOF

    mutex.lock();
    qint64 readSoFar;
    // If startAsyncRead() has read data, copy it to its destination.
    if (maxlen == 1 && actualReadBufferSize > 0) {
        *data = readBuffer.getChar();
        actualReadBufferSize--;
        readSoFar = 1;
    } else {
        readSoFar = readBuffer.read(data, qMin(actualReadBufferSize, maxlen));
        actualReadBufferSize -= readSoFar;
    }
    mutex.unlock();

    if (!pipeBroken) {
        if (state == Running)
            startAsyncRead();
        if (readSoFar == 0)
            return -2;      // signal EWOULDBLOCK
    }

    return readSoFar;
}

/*!
    Returns \c true if a complete line of data can be read from the buffer.
 */
bool QWindowsPipeReader::canReadLine() const
{
    QMutexLocker locker(&mutex);
    return readBuffer.indexOf('\n', actualReadBufferSize) >= 0;
}

/*!
    Starts an asynchronous read sequence on the pipe.
 */
void QWindowsPipeReader::startAsyncRead()
{
    QMutexLocker locker(&mutex);

    if (readSequenceStarted || lastError != ERROR_SUCCESS)
        return;

    state = Running;
    startAsyncReadLocked();

    // Do not post the event, if the read operation will be completed asynchronously.
    if (!readyReadPending && lastError == ERROR_SUCCESS)
        return;

    if (!winEventActPosted) {
        winEventActPosted = true;
        locker.unlock();
        QCoreApplication::postEvent(this, new QEvent(QEvent::WinEventAct));
    } else {
        locker.unlock();
    }

    SetEvent(syncHandle);
}

/*!
    Starts a new read sequence. Thread-safety should be ensured
    by the caller.
 */
void QWindowsPipeReader::startAsyncReadLocked()
{
    // Determine the number of bytes to read.
    qint64 bytesToRead = qMax(checkPipeState(), state == Running ? minReadBufferSize : 0);

    // This can happen only while draining; just do nothing in this case.
    if (bytesToRead == 0)
        return;

    while (lastError == ERROR_SUCCESS) {
        if (readBufferMaxSize && bytesToRead > (readBufferMaxSize - readBuffer.size())) {
            bytesToRead = readBufferMaxSize - readBuffer.size();
            if (bytesToRead <= 0) {
                // Buffer is full. User must read data from the buffer
                // before we can read more from the pipe.
                return;
            }
        }

        char *ptr = readBuffer.reserve(bytesToRead);

        // ReadFile() returns true, if the read operation completes synchronously.
        // We don't need to call GetOverlappedResult() additionally, because
        // 'numberOfBytesRead' is valid in this case.
        DWORD numberOfBytesRead;
        DWORD errorCode = ERROR_SUCCESS;
        if (!ReadFile(handle, ptr, bytesToRead, &numberOfBytesRead, &overlapped)) {
            errorCode = GetLastError();
            if (errorCode == ERROR_IO_PENDING) {
                Q_ASSERT(state == Running);
                // Operation has been queued and will complete in the future.
                readSequenceStarted = true;
                SetThreadpoolWait(waitObject, eventHandle, NULL);
                return;
            }
        }

        if (!readCompleted(errorCode, numberOfBytesRead))
            return;

        // In the 'Draining' state, we have to get all the data with one call
        // to ReadFile(). Note that message mode pipes are not supported here.
        if (state == Draining) {
            Q_ASSERT(bytesToRead == qint64(numberOfBytesRead));
            return;
        }

        // We need to loop until all pending data has been read and an
        // operation is queued for asynchronous completion.
        // If the pipe is configured to work in message mode, we read
        // the data in chunks.
        bytesToRead = qMax(checkPipeState(), minReadBufferSize);
    }
}

/*!
    Thread pool callback procedure.
 */
void QWindowsPipeReader::waitCallback(PTP_CALLBACK_INSTANCE instance, PVOID context,
                                      PTP_WAIT wait, TP_WAIT_RESULT waitResult)
{
    Q_UNUSED(instance);
    Q_UNUSED(wait);
    Q_UNUSED(waitResult);
    QWindowsPipeReader *pipeReader = reinterpret_cast<QWindowsPipeReader *>(context);

    // Get the result of the asynchronous operation.
    DWORD numberOfBytesTransfered = 0;
    DWORD errorCode = ERROR_SUCCESS;
    if (!GetOverlappedResult(pipeReader->handle, &pipeReader->overlapped,
                             &numberOfBytesTransfered, FALSE))
        errorCode = GetLastError();

    QMutexLocker locker(&pipeReader->mutex);

    pipeReader->readSequenceStarted = false;

    // Do not overwrite error code, if error has been detected by
    // checkPipeState() in waitForPipeClosed().
    if (pipeReader->lastError != ERROR_SUCCESS
            // After the reader was stopped, the only reason why this function can
            // be called is the completion of a cancellation. No signals should be
            // emitted, and no new read sequence should be started in this case.
            || pipeReader->state == Stopped) {
        SetEvent(pipeReader->syncHandle);
        return;
    }

    // Ignore ERROR_OPERATION_ABORTED. We have canceled the I/O operation
    // specifically for flushing the pipe.
    if (pipeReader->state == Draining && errorCode == ERROR_OPERATION_ABORTED)
        errorCode = ERROR_SUCCESS;

    const bool wasPendingBytes = pipeReader->readyReadPending;
    if (pipeReader->readCompleted(errorCode, numberOfBytesTransfered))
        pipeReader->startAsyncReadLocked();

    if (pipeReader->state == Running && !pipeReader->winEventActPosted) {
        pipeReader->winEventActPosted = true;
        locker.unlock();
        QCoreApplication::postEvent(pipeReader, new QEvent(QEvent::WinEventAct));
    } else {
        locker.unlock();
    }

    // To avoid a race condition with waitForReadyRead(), skip setting
    // the event if it has already been set.
    if (!wasPendingBytes)
        SetEvent(pipeReader->syncHandle);
}

/*!
    Will be called whenever the read operation completes. Returns \c true if
    no error occurred; otherwise returns \c false.
 */
bool QWindowsPipeReader::readCompleted(DWORD errorCode, DWORD numberOfBytesRead)
{
    // ERROR_MORE_DATA is not an error. We're connected to a message mode
    // pipe and the message didn't fit into the pipe's system
    // buffer. We will read the remaining data in the next call.
    if (errorCode == ERROR_SUCCESS || errorCode == ERROR_MORE_DATA) {
        readyReadPending = true;
        pendingReadBytes += numberOfBytesRead;
        readBuffer.truncate(actualReadBufferSize + pendingReadBytes);
        return true;
    }

    lastError = errorCode;
    return false;
}

/*!
    Receives notification that the read operation has completed.
 */
bool QWindowsPipeReader::event(QEvent *e)
{
    if (e->type() == QEvent::WinEventAct) {
        consumePendingResults(true);
        return true;
    }
    return QObject::event(e);
}

/*!
    Updates the read buffer size and emits pending signals in the main thread.
    Returns \c true, if readyRead() was emitted.
 */
bool QWindowsPipeReader::consumePendingResults(bool allowWinActPosting)
{
    mutex.lock();

    const bool emitReadyRead = consumePending(allowWinActPosting);
    const DWORD dwError = lastError;

    mutex.unlock();

    // Disable any further processing, if the pipe was stopped.
    // We are not allowed to emit signals in either 'Stopped'
    // or 'Draining' state.
    if (state != Running)
        return false;

    if (emitReadyRead && !inReadyRead) {
        QScopedValueRollback<bool> guard(inReadyRead, true);
        emit readyRead();
    }

    // Trigger 'pipeBroken' only once.
    if (dwError != ERROR_SUCCESS && !pipeBroken) {
        pipeBroken = true;
        if (dwError != ERROR_BROKEN_PIPE && dwError != ERROR_PIPE_NOT_CONNECTED)
            emit winError(dwError, QLatin1String("QWindowsPipeReader::consumePendingResults"));
        emit pipeClosed();
    }

    return emitReadyRead;
}

/*!
    Updates the read buffer size. Returns \c true, if readyRead()
    should be emitted. Thread-safety should be ensured by the caller.
 */
bool QWindowsPipeReader::consumePending(bool allowWinActPosting)
{
    // Confirm that the I/O operation is complete.
    ResetEvent(syncHandle);

    // Enable QEvent::WinEventAct posting.
    if (allowWinActPosting)
        winEventActPosted = false;

    if (readyReadPending) {
        readyReadPending = false;
        actualReadBufferSize += pendingReadBytes;
        pendingReadBytes = 0;
        return true;
    }

    return false;
}

/*!
    Returns the number of available bytes in the pipe.
 */
DWORD QWindowsPipeReader::checkPipeState()
{
    DWORD bytes;
    if (PeekNamedPipe(handle, nullptr, 0, nullptr, &bytes, nullptr))
        return bytes;

    lastError = GetLastError();
    return 0;
}

bool QWindowsPipeReader::waitForNotification(int timeout)
{
    QElapsedTimer t;
    t.start();
    int msecs = timeout;
    do {
        DWORD waitRet = WaitForSingleObjectEx(syncHandle,
                                              msecs == -1 ? INFINITE : msecs, TRUE);
        if (waitRet == WAIT_OBJECT_0)
            return true;

        if (waitRet != WAIT_IO_COMPLETION)
            return false;

        // Some I/O completion routine was called. Wait some more.
        msecs = qt_subtract_from_timeout(timeout, t.elapsed());
    } while (msecs != 0);

    return false;
}

/*!
    Waits for the completion of the asynchronous read operation.
    Returns \c true, if we've emitted the readyRead signal (non-recursive case)
    or readyRead will be emitted by the event loop (recursive case).
 */
bool QWindowsPipeReader::waitForReadyRead(int msecs)
{
    if (readBufferMaxSize && actualReadBufferSize >= readBufferMaxSize)
        return false;

    // Make sure that 'syncHandle' was triggered by the thread pool callback.
    return !pipeBroken && waitForNotification(msecs)
           && consumePendingResults(false);
}

/*!
    Waits until the pipe is closed.
 */
bool QWindowsPipeReader::waitForPipeClosed(int msecs)
{
    const int sleepTime = 10;
    QDeadlineTimer timer(msecs);

    while (waitForReadyRead(timer.remainingTime())) {}
    if (pipeBroken)
        return true;

    if (timer.hasExpired())
        return false;

    // When the read buffer is full, the read sequence is not running,
    // so we need to peek the pipe to detect disconnection.
    forever {
        mutex.lock();
        checkPipeState();
        mutex.unlock();
        consumePendingResults(false);
        if (pipeBroken)
            return true;

        if (timer.hasExpired())
            return false;

        Sleep(sleepTime);
    }
}

QT_END_NAMESPACE
