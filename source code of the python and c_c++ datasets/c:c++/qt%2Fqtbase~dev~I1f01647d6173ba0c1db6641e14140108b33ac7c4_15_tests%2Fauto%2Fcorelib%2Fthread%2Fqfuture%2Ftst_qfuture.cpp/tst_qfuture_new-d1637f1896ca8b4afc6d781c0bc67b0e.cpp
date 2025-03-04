/****************************************************************************
**
** Copyright (C) 2016 The Qt Company Ltd.
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
#include <QCoreApplication>
#include <QDebug>

#define QFUTURE_TEST

#include <QtTest/QtTest>
#include <qfuture.h>
#include <qfuturewatcher.h>
#include <qresultstore.h>
#include <qthreadpool.h>
#include <qexception.h>
#include <qrandom.h>
#include <QtConcurrent/qtconcurrentrun.h>
#include <private/qfutureinterface_p.h>

#include <vector>
#include <memory>

// COM interface macro.
#if defined(Q_OS_WIN) && defined(interface)
#  undef interface
#endif

struct ResultStoreInt : QtPrivate::ResultStoreBase
{
    ~ResultStoreInt() { clear<int>(); }
};

class SenderObject : public QObject
{
    Q_OBJECT

public:
    void emitNoArg() { emit noArgSignal(); }
    void emitIntArg(int value) { emit intArgSignal(value); }
    void emitConstRefArg(const QString &value) { emit constRefArg(value); }
    void emitMultipleArgs(int value1, double value2, const QString &value3)
    {
        emit multipleArgs(value1, value2, value3);
    }

signals:
    void noArgSignal();
    void intArgSignal(int value);
    void constRefArg(const QString &value);
    void multipleArgs(int value1, double value2, const QString &value3);
};

class LambdaThread : public QThread
{
public:
    LambdaThread(std::function<void ()> fn)
    :m_fn(fn)
    {

    }

    void run() override
    {
        m_fn();
    }

private:
    std::function<void ()> m_fn;
};

using UniquePtr = std::unique_ptr<int>;

class tst_QFuture: public QObject
{
    Q_OBJECT
private slots:
    void resultStore();
    void future();
    void futureInterface();
    void refcounting();
    void cancel();
    void statePropagation();
    void multipleResults();
    void indexedResults();
    void progress();
    void progressText();
    void resultsAfterFinished();
    void resultsAsList();
    void implicitConversions();
    void iterators();
    void iteratorsThread();
    void pause();
    void throttling();
    void voidConversions();
#ifndef QT_NO_EXCEPTIONS
    void exceptions();
    void nestedExceptions();
#endif
    void nonGlobalThreadPool();

    void then();
    void thenForMoveOnlyTypes();
    void thenOnCanceledFuture();
#ifndef QT_NO_EXCEPTIONS
    void thenOnExceptionFuture();
    void thenThrows();
    void onFailed();
    void onFailedTestCallables();
    void onFailedForMoveOnlyTypes();
#endif
    void onCanceled();
    void takeResults();
    void takeResult();
    void runAndTake();
    void resultsReadyAt_data();
    void resultsReadyAt();
    void takeResultWorksForTypesWithoutDefaultCtor();
    void canceledFutureIsNotValid();
    void signalConnect();

private:
    using size_type = std::vector<int>::size_type;

    static void testSingleResult(const UniquePtr &p);
    static void testSingleResult(const std::vector<int> &v);
    template<class T>
    static void testSingleResult(const T &unknown);
    template<class T>
    static void testFutureTaken(QFuture<T> &noMoreFuture);
    template<class T>
    static  void testTakeResults(QFuture<T> future, size_type resultCount);
};

void tst_QFuture::resultStore()
{
    int int0 = 0;
    int int1 = 1;
    int int2 = 2;

    {
        ResultStoreInt store;
        QCOMPARE(store.begin(), store.end());
        QCOMPARE(store.resultAt(0), store.end());
        QCOMPARE(store.resultAt(1), store.end());
    }


    {
        ResultStoreInt store;
        store.addResult(-1, &int0);
        store.addResult(1, &int1);
        QtPrivate::ResultIteratorBase it = store.begin();
        QCOMPARE(it.resultIndex(), 0);
        QVERIFY(it == store.begin());
        QVERIFY(it != store.end());

        ++it;
        QCOMPARE(it.resultIndex(), 1);
        QVERIFY(it != store.begin());
        QVERIFY(it != store.end());

        ++it;
        QVERIFY(it != store.begin());
        QVERIFY(it == store.end());
    }

    QVector<int> vec0 = QVector<int>() << 2 << 3;
    QVector<int> vec1 = QVector<int>() << 4 << 5;

    {
        ResultStoreInt store;
        store.addResults(-1, &vec0, 2);
        store.addResults(-1, &vec1, 2);
        QtPrivate::ResultIteratorBase it = store.begin();
        QCOMPARE(it.resultIndex(), 0);
        QCOMPARE(it, store.begin());
        QVERIFY(it != store.end());

        ++it;
        QCOMPARE(it.resultIndex(), 1);
        QVERIFY(it != store.begin());
        QVERIFY(it != store.end());

        ++it;
        QCOMPARE(it.resultIndex(), 2);

        ++it;
        QCOMPARE(it.resultIndex(), 3);

        ++it;
        QCOMPARE(it, store.end());
    }
    {
        ResultStoreInt store;
        store.addResult(-1, &int0);
        store.addResults(-1, &vec1, 2);
        store.addResult(-1, &int1);

        QtPrivate::ResultIteratorBase it = store.begin();
        QCOMPARE(it.resultIndex(), 0);
        QVERIFY(it == store.begin());
        QVERIFY(it != store.end());

        ++it;
        QCOMPARE(it.resultIndex(), 1);
        QVERIFY(it != store.begin());
        QVERIFY(it != store.end());

        ++it;
        QCOMPARE(it.resultIndex(), 2);
        QVERIFY(it != store.end());
        ++it;
        QCOMPARE(it.resultIndex(), 3);
        QVERIFY(it != store.end());
        ++it;
        QVERIFY(it == store.end());

        QCOMPARE(store.resultAt(0).resultIndex(), 0);
        QCOMPARE(store.resultAt(1).resultIndex(), 1);
        QCOMPARE(store.resultAt(2).resultIndex(), 2);
        QCOMPARE(store.resultAt(3).resultIndex(), 3);
        QCOMPARE(store.resultAt(4), store.end());
    }
    {
        ResultStoreInt store;
        store.addResult(-1, &int0);
        store.addResults(-1, &vec0);
        store.addResult(-1, &int1);

        QtPrivate::ResultIteratorBase it = store.begin();
        QCOMPARE(it.resultIndex(), 0);
        QVERIFY(it == store.begin());
        QVERIFY(it != store.end());

        ++it;
        QCOMPARE(it.resultIndex(), 1);
        QVERIFY(it != store.begin());
        QVERIFY(it != store.end());

        ++it;
        QCOMPARE(it.resultIndex(), 2);
        QVERIFY(it != store.end());
        ++it;
        QCOMPARE(it.resultIndex(), 3);
        QVERIFY(it != store.end());
        ++it;
        QVERIFY(it == store.end());

        QCOMPARE(store.resultAt(0).value<int>(), int0);
        QCOMPARE(store.resultAt(1).value<int>(), vec0[0]);
        QCOMPARE(store.resultAt(2).value<int>(), vec0[1]);
        QCOMPARE(store.resultAt(3).value<int>(), int1);
    }
    {
        ResultStoreInt store;
        store.addResult(-1, &int0);
        store.addResults(-1, &vec0);
        store.addResult(200, &int1);

        QCOMPARE(store.resultAt(0).value<int>(), int0);
        QCOMPARE(store.resultAt(1).value<int>(), vec0[0]);
        QCOMPARE(store.resultAt(2).value<int>(), vec0[1]);
        QCOMPARE(store.resultAt(200).value<int>(), int1);
    }

    {
        ResultStoreInt store;
        store.addResult(1, &int1);
        store.addResult(0, &int0);
        store.addResult(-1, &int2);

        QCOMPARE(store.resultAt(0).value<int>(), int0);
        QCOMPARE(store.resultAt(1).value<int>(), int1);
        QCOMPARE(store.resultAt(2).value<int>(), int2);
    }

    {
        ResultStoreInt store;
        QCOMPARE(store.contains(0), false);
        QCOMPARE(store.contains(1), false);
        QCOMPARE(store.contains(INT_MAX), false);
    }

    {
        // Test filter mode, where "gaps" in the result array aren't allowed.
        ResultStoreInt store;
        store.setFilterMode(true);

        store.addResult(0, &int0);
        QCOMPARE(store.contains(0), true);

        store.addResult(2, &int2); // add result at index 2
        QCOMPARE(store.contains(2), false); // but 1 is missing, so this 2 won't be reported yet.

        store.addResult(1, &int1);
        QCOMPARE(store.contains(1), true);
        QCOMPARE(store.contains(2), true); // 2 should be visible now.

        store.addResult(4, &int0);
        store.addResult(5, &int0);
        store.addResult(7, &int0);
        QCOMPARE(store.contains(4), false);
        QCOMPARE(store.contains(5), false);
        QCOMPARE(store.contains(7), false);

        store.addResult(3, &int0);  // adding 3 makes 4 and 5 visible
        QCOMPARE(store.contains(4), true);
        QCOMPARE(store.contains(5), true);
        QCOMPARE(store.contains(7), false);

        store.addResult(6, &int0);  // adding 6 makes 7 visible

        QCOMPARE(store.contains(6), true);
        QCOMPARE(store.contains(7), true);
        QCOMPARE(store.contains(8), false);
    }

    {
        // test canceled results
        ResultStoreInt store;
        store.setFilterMode(true);

        store.addResult(0, &int0);
        QCOMPARE(store.contains(0), true);

        store.addResult(2, &int0);
        QCOMPARE(store.contains(2), false);

        store.addCanceledResult(1); // report no result at 1

        QCOMPARE(store.contains(0), true);
        QCOMPARE(store.contains(1), true); // 2 gets renamed to 1
        QCOMPARE(store.contains(2), false);

        store.addResult(3, &int0);
        QCOMPARE(store.contains(2), true); //3 gets renamed to 2

        store.addResult(6, &int0);
        store.addResult(7, &int0);
        QCOMPARE(store.contains(3), false);

        store.addCanceledResult(4);
        store.addCanceledResult(5);

        QCOMPARE(store.contains(3), true); //6 gets renamed to 3
        QCOMPARE(store.contains(4), true); //7 gets renamed to 4

        store.addResult(8, &int0);
        QCOMPARE(store.contains(5), true); //8 gets renamed to 4

        QCOMPARE(store.contains(6), false);
        QCOMPARE(store.contains(7), false);
    }

    {
        // test addResult return value
        ResultStoreInt store;
        store.setFilterMode(true);

        store.addResult(0, &int0);
        QCOMPARE(store.count(), 1); // result 0 becomes available
        QCOMPARE(store.contains(0), true);

        store.addResult(2, &int0);
        QCOMPARE(store.count(), 1);
        QCOMPARE(store.contains(2), false);

        store.addCanceledResult(1);
        QCOMPARE(store.count(), 2); // result 2 is renamed to 1 and becomes available

        QCOMPARE(store.contains(0), true);
        QCOMPARE(store.contains(1), true);
        QCOMPARE(store.contains(2), false);

        store.addResult(3, &int0);
        QCOMPARE(store.count(), 3);
        QCOMPARE(store.contains(2), true);

        store.addResult(6, &int0);
        QCOMPARE(store.count(), 3);
        store.addResult(7, &int0);
        QCOMPARE(store.count(), 3);
        QCOMPARE(store.contains(3), false);

        store.addCanceledResult(4);
        store.addCanceledResult(5);
        QCOMPARE(store.count(), 5); // 6 and 7 is renamed to 3 and 4 and becomes available

        QCOMPARE(store.contains(3), true);
        QCOMPARE(store.contains(4), true);

        store.addResult(8, &int0);
        QCOMPARE(store.contains(5), true);
        QCOMPARE(store.count(), 6);

        QCOMPARE(store.contains(6), false);
        QCOMPARE(store.contains(7), false);
    }

    {
        // test resultCount in non-filtered mode. It should always be possible
        // to iterate through the results 0 to resultCount.
        ResultStoreInt store;
        store.addResult(0, &int0);

        QCOMPARE(store.count(), 1);

        store.addResult(2, &int0);

        QCOMPARE(store.count(), 1);

        store.addResult(1, &int0);
        QCOMPARE(store.count(), 3);
    }

    {
        ResultStoreInt store;
        store.addResult(2, &int0);
        QCOMPARE(store.count(), 0);

        store.addResult(1, &int0);
        QCOMPARE(store.count(), 0);

        store.addResult(0, &int0);
        QCOMPARE(store.count(), 3);
    }

    {
        ResultStoreInt store;
        store.addResults(2, &vec1);
        QCOMPARE(store.count(), 0);

        store.addResult(1, &int0);
        QCOMPARE(store.count(), 0);

        store.addResult(0, &int0);
        QCOMPARE(store.count(), 4);
    }

    {
        ResultStoreInt store;
        store.addResults(2, &vec1);
        QCOMPARE(store.count(), 0);

        store.addResults(0, &vec0);
        QCOMPARE(store.count(), 4);
    }
    {
        ResultStoreInt store;
        store.addResults(3, &vec1);
        QCOMPARE(store.count(), 0);

        store.addResults(0, &vec0);
        QCOMPARE(store.count(), 2);

        store.addResult(2, &int0);
        QCOMPARE(store.count(), 5);
    }

    {
        ResultStoreInt store;
        store.setFilterMode(true);
        store.addResults(3, &vec1);
        QCOMPARE(store.count(), 0);

        store.addResults(0, &vec0);
        QCOMPARE(store.count(), 2);

        store.addCanceledResult(2);
        QCOMPARE(store.count(), 4);
    }

    {
        ResultStoreInt store;
        store.setFilterMode(true);
        store.addResults(3, &vec1);
        QCOMPARE(store.count(), 0);

        store.addCanceledResults<int>(0, 3);
        QCOMPARE(store.count(), 2);
    }

    {
        ResultStoreInt store;
        store.setFilterMode(true);
        store.addResults(3, &vec1);
        QCOMPARE(store.count(), 0);

        store.addCanceledResults<int>(0, 3);
        QCOMPARE(store.count(), 2);  // results at 3 and 4 become available at index 0, 1

        store.addResult(5, &int0);
        QCOMPARE(store.count(), 3);// result 5 becomes available at index 2
    }

    {
        ResultStoreInt store;
        store.addResult(1, &int0);
        store.addResult(3, &int0);
        store.addResults(6, &vec0);
        QCOMPARE(store.contains(0), false);
        QCOMPARE(store.contains(1), true);
        QCOMPARE(store.contains(2), false);
        QCOMPARE(store.contains(3), true);
        QCOMPARE(store.contains(4), false);
        QCOMPARE(store.contains(5), false);
        QCOMPARE(store.contains(6), true);
        QCOMPARE(store.contains(7), true);
    }

    {
        ResultStoreInt store;
        store.setFilterMode(true);
        store.addResult(1, &int0);
        store.addResult(3, &int0);
        store.addResults(6, &vec0);
        QCOMPARE(store.contains(0), false);
        QCOMPARE(store.contains(1), false);
        QCOMPARE(store.contains(2), false);
        QCOMPARE(store.contains(3), false);
        QCOMPARE(store.contains(4), false);
        QCOMPARE(store.contains(5), false);
        QCOMPARE(store.contains(6), false);
        QCOMPARE(store.contains(7), false);

        store.addCanceledResult(0);
        store.addCanceledResult(2);
        store.addCanceledResults<int>(4, 2);

        QCOMPARE(store.contains(0), true);
        QCOMPARE(store.contains(1), true);
        QCOMPARE(store.contains(2), true);
        QCOMPARE(store.contains(3), true);
        QCOMPARE(store.contains(4), false);
        QCOMPARE(store.contains(5), false);
        QCOMPARE(store.contains(6), false);
        QCOMPARE(store.contains(7), false);
    }
    {
        ResultStoreInt store;
        store.setFilterMode(true);
        store.addCanceledResult(0);
        QCOMPARE(store.contains(0), false);

        store.addResult(1, &int0);
        QCOMPARE(store.contains(0), true);
        QCOMPARE(store.contains(1), false);
    }
}

void tst_QFuture::future()
{
    // default constructors
    QFuture<int> intFuture;
    intFuture.waitForFinished();
    QFuture<QString> stringFuture;
    stringFuture.waitForFinished();
    QFuture<void> voidFuture;
    voidFuture.waitForFinished();
    QFuture<void> defaultVoidFuture;
    defaultVoidFuture.waitForFinished();

    // copy constructor
    QFuture<int> intFuture2(intFuture);
    QFuture<void> voidFuture2(defaultVoidFuture);

    // assigmnent operator
    intFuture2 = QFuture<int>();
    voidFuture2 = QFuture<void>();

    // state
    QCOMPARE(intFuture2.isStarted(), true);
    QCOMPARE(intFuture2.isFinished(), true);
}

class IntResult : public QFutureInterface<int>
{
public:
    QFuture<int> run()
    {
        this->reportStarted();
        QFuture<int> future = QFuture<int>(this);

        int res = 10;
        reportFinished(&res);
        return future;
    }
};

int value = 10;

class VoidResult : public QFutureInterfaceBase
{
public:
    QFuture<void> run()
    {
        this->reportStarted();
        QFuture<void> future = QFuture<void>(this);
        reportFinished();
        return future;
    }
};

void tst_QFuture::futureInterface()
{
    {
        QFuture<void> future;
        {
            QFutureInterface<void> i;
            i.reportStarted();
            future = i.future();
            i.reportFinished();
        }
    }
    {
        QFuture<int> future;
        {
            QFutureInterface<int> i;
            i.reportStarted();
            i.reportResult(10);
            future = i.future();
            i.reportFinished();
        }
        QCOMPARE(future.resultAt(0), 10);
    }

    {
        QFuture<int> intFuture;

        QCOMPARE(intFuture.isStarted(), true);
        QCOMPARE(intFuture.isFinished(), true);

        IntResult result;

        result.reportStarted();
        intFuture = result.future();

        QCOMPARE(intFuture.isStarted(), true);
        QCOMPARE(intFuture.isFinished(), false);

        result.reportFinished(&value);

        QCOMPARE(intFuture.isStarted(), true);
        QCOMPARE(intFuture.isFinished(), true);

        int e = intFuture.result();

        QCOMPARE(intFuture.isStarted(), true);
        QCOMPARE(intFuture.isFinished(), true);
        QCOMPARE(intFuture.isCanceled(), false);

        QCOMPARE(e, value);
        intFuture.waitForFinished();

        IntResult intAlgo;
        intFuture = intAlgo.run();
        QFuture<int> intFuture2(intFuture);
        QCOMPARE(intFuture.result(), value);
        QCOMPARE(intFuture2.result(), value);
        intFuture.waitForFinished();

        VoidResult a;
        a.run().waitForFinished();
    }
}

template <typename T>
void testRefCounting()
{
    QFutureInterface<T> interface;
    QCOMPARE(interface.d->refCount.load(), 1);

    {
        interface.reportStarted();

        QFuture<T> f = interface.future();
        QCOMPARE(interface.d->refCount.load(), 2);

        QFuture<T> f2(f);
        QCOMPARE(interface.d->refCount.load(), 3);

        QFuture<T> f3;
        f3 = f2;
        QCOMPARE(interface.d->refCount.load(), 4);

        interface.reportFinished(0);
        QCOMPARE(interface.d->refCount.load(), 4);
    }

    QCOMPARE(interface.d->refCount.load(), 1);
}

void tst_QFuture::refcounting()
{
    testRefCounting<int>();
}

void tst_QFuture::cancel()
{
    {
        QFuture<void> f;
        QFutureInterface<void> result;

        result.reportStarted();
        f = result.future();
        QVERIFY(!f.isCanceled());
        result.reportCanceled();
        QVERIFY(f.isCanceled());
        result.reportFinished();
        QVERIFY(f.isCanceled());
        f.waitForFinished();
        QVERIFY(f.isCanceled());
    }

    // Cancel from the QFuture side and test if the result
    // interface detects it.
    {
        QFutureInterface<void> result;

        QFuture<void> f;
        QVERIFY(f.isStarted());

        result.reportStarted();
        f = result.future();

        QVERIFY(f.isStarted());

        QVERIFY(!result.isCanceled());
        f.cancel();

        QVERIFY(result.isCanceled());

        result.reportFinished();
    }

    // Test that finished futures can be canceled.
    {
        QFutureInterface<void> result;

        QFuture<void> f;
        QVERIFY(f.isStarted());

        result.reportStarted();
        f = result.future();

        QVERIFY(f.isStarted());

        result.reportFinished();

        f.cancel();

        QVERIFY(result.isCanceled());
        QVERIFY(f.isCanceled());
    }

    // Results reported after canceled is called should not be propagated.
    {

        QFutureInterface<int> futureInterface;
        futureInterface.reportStarted();
        QFuture<int> f = futureInterface.future();

        int result = 0;
        futureInterface.reportResult(&result);
        result = 1;
        futureInterface.reportResult(&result);
        f.cancel();
        result = 2;
        futureInterface.reportResult(&result);
        result = 3;
        futureInterface.reportResult(&result);
        futureInterface.reportFinished();
        QCOMPARE(f.results(), QList<int>());
    }
}

void tst_QFuture::statePropagation()
{
    QFuture<void> f1;
    QFuture<void> f2;

    QCOMPARE(f1.isStarted(), true);

    QFutureInterface<void> result;
    result.reportStarted();
    f1 = result.future();

    f2 = f1;

    QCOMPARE(f2.isStarted(), true);

    result.reportCanceled();

    QCOMPARE(f2.isStarted(), true);
    QCOMPARE(f2.isCanceled(), true);

    QFuture<void> f3 = f2;

    QCOMPARE(f3.isStarted(), true);
    QCOMPARE(f3.isCanceled(), true);

    result.reportFinished();

    QCOMPARE(f2.isStarted(), true);
    QCOMPARE(f2.isCanceled(), true);

    QCOMPARE(f3.isStarted(), true);
    QCOMPARE(f3.isCanceled(), true);
}

/*
    Tests that a QFuture can return multiple results.
*/
void tst_QFuture::multipleResults()
{
    IntResult a;
    a.reportStarted();
    QFuture<int> f = a.future();

    QFuture<int> copy = f;
    int result;

    result = 1;
    a.reportResult(&result);
    QCOMPARE(f.resultAt(0), 1);

    result = 2;
    a.reportResult(&result);
    QCOMPARE(f.resultAt(1), 2);

    result = 3;
    a.reportResult(&result);

    result = 4;
    a.reportFinished(&result);

    QCOMPARE(f.results(), QList<int>() << 1 << 2 << 3 << 4);

    // test foreach
    QList<int> fasit = QList<int>() << 1 << 2 << 3 << 4;
    {
        QList<int> results;
        foreach(int result, f)
            results.append(result);
        QCOMPARE(results, fasit);
    }
    {
        QList<int> results;
        foreach(int result, copy)
            results.append(result);
        QCOMPARE(results, fasit);
    }
}

/*
    Test out-of-order result reporting using indexes
*/
void tst_QFuture::indexedResults()
{
    {
        QFutureInterface<QChar> Interface;
        QFuture<QChar> f;
        QVERIFY(f.isStarted());

        Interface.reportStarted();
        f = Interface.future();

        QVERIFY(f.isStarted());

        QChar result;

        result = 'B';
        Interface.reportResult(&result, 1);

        QCOMPARE(f.resultAt(1), result);

        result = 'A';
        Interface.reportResult(&result, 0);
        QCOMPARE(f.resultAt(0), result);

        result = 'C';
        Interface.reportResult(&result); // no index
        QCOMPARE(f.resultAt(2), result);

        Interface.reportFinished();

        QCOMPARE(f.results(), QList<QChar>() << 'A' << 'B' << 'C');
    }

    {
        // Test result reporting with a missing result in the middle
        QFutureInterface<int> Interface;
        Interface.reportStarted();
        QFuture<int> f = Interface.future();
        int result;

        result = 0;
        Interface.reportResult(&result, 0);
        QVERIFY(f.isResultReadyAt(0));
        QCOMPARE(f.resultAt(0), 0);

        result = 3;
        Interface.reportResult(&result, 3);
        QVERIFY(f.isResultReadyAt(3));
        QCOMPARE(f.resultAt(3), 3);

        result = 2;
        Interface.reportResult(&result, 2);
        QVERIFY(f.isResultReadyAt(2));
        QCOMPARE(f.resultAt(2), 2);

        result = 4;
        Interface.reportResult(&result); // no index
        QVERIFY(f.isResultReadyAt(4));
        QCOMPARE(f.resultAt(4), 4);

        Interface.reportFinished();

        QCOMPARE(f.results(), QList<int>() << 0 << 2 << 3 << 4);
    }
}

void tst_QFuture::progress()
{
    QFutureInterface<QChar> result;
    QFuture<QChar> f;

    QCOMPARE (f.progressValue(), 0);

    result.reportStarted();
    f = result.future();

    QCOMPARE (f.progressValue(), 0);

    result.setProgressValue(50);

    QCOMPARE (f.progressValue(), 50);

    result.reportFinished();

    QCOMPARE (f.progressValue(), 50);
}

void tst_QFuture::progressText()
{
    QFutureInterface<void> i;
    i.reportStarted();
    QFuture<void> f = i.future();

    QCOMPARE(f.progressText(), QLatin1String(""));
    i.setProgressValueAndText(1, QLatin1String("foo"));
    QCOMPARE(f.progressText(), QLatin1String("foo"));
    i.reportFinished();
}

/*
    Test that results reported after finished are ignored.
*/
void tst_QFuture::resultsAfterFinished()
{
    {
        IntResult a;
        a.reportStarted();
        QFuture<int> f =  a.future();
        int result;

        QCOMPARE(f.resultCount(), 0);

        result = 1;
        a.reportResult(&result);
        QCOMPARE(f.resultAt(0), 1);

        a.reportFinished();

        QCOMPARE(f.resultAt(0), 1);
        QCOMPARE(f.resultCount(), 1);
        result = 2;
        a.reportResult(&result);
        QCOMPARE(f.resultCount(), 1);
    }
    // cancel it
    {
        IntResult a;
        a.reportStarted();
        QFuture<int> f =  a.future();
        int result;

        QCOMPARE(f.resultCount(), 0);

        result = 1;
        a.reportResult(&result);
        QCOMPARE(f.resultAt(0), 1);
        QCOMPARE(f.resultCount(), 1);

        a.reportCanceled();

        QCOMPARE(f.resultAt(0), 1);
        QCOMPARE(f.resultCount(), 1);

        result = 2;
        a.reportResult(&result);
        a.reportFinished();
    }
}

void tst_QFuture::resultsAsList()
{
    IntResult a;
    a.reportStarted();
    QFuture<int> f = a.future();

    int result;
    result = 1;
    a.reportResult(&result);
    result = 2;
    a.reportResult(&result);

    a.reportFinished();

    QList<int> results = f.results();
    QCOMPARE(results, QList<int>() << 1 << 2);
}

/*
    Test that QFuture<T> can be implicitly converted to T
*/
void tst_QFuture::implicitConversions()
{
    QFutureInterface<QString> iface;
    iface.reportStarted();

    QFuture<QString> f(&iface);

    const QString input("FooBar 2000");
    iface.reportFinished(&input);

    const QString result = f;
    QCOMPARE(result, input);
    QCOMPARE(QString(f), input);
    QCOMPARE(static_cast<QString>(f), input);
}

void tst_QFuture::iterators()
{
    {
        QFutureInterface<int> e;
        e.reportStarted();
        QFuture<int> f = e.future();

        int result;
        result = 1;
        e.reportResult(&result);
        result = 2;
        e.reportResult(&result);
        result = 3;
        e.reportResult(&result);
        e.reportFinished();

        QList<int> results;
        QFutureIterator<int> i(f);
        while (i.hasNext()) {
            results.append(i.next());
        }

        QCOMPARE(results, f.results());

        QFuture<int>::const_iterator i1 = f.begin(), i2 = i1 + 1;
        QFuture<int>::const_iterator c1 = i1, c2 = c1 + 1;

        QCOMPARE(i1, i1);
        QCOMPARE(i1, c1);
        QCOMPARE(c1, i1);
        QCOMPARE(c1, c1);
        QCOMPARE(i2, i2);
        QCOMPARE(i2, c2);
        QCOMPARE(c2, i2);
        QCOMPARE(c2, c2);
        QCOMPARE(1 + i1, i1 + 1);
        QCOMPARE(1 + c1, c1 + 1);

        QVERIFY(i1 != i2);
        QVERIFY(i1 != c2);
        QVERIFY(c1 != i2);
        QVERIFY(c1 != c2);
        QVERIFY(i2 != i1);
        QVERIFY(i2 != c1);
        QVERIFY(c2 != i1);
        QVERIFY(c2 != c1);

        int x1 = *i1;
        Q_UNUSED(x1);
        int x2 = *i2;
        Q_UNUSED(x2);
        int y1 = *c1;
        Q_UNUSED(y1);
        int y2 = *c2;
        Q_UNUSED(y2);
    }

    {
        QFutureInterface<QString> e;
        e.reportStarted();
        QFuture<QString> f =  e.future();

        e.reportResult(QString("one"));
        e.reportResult(QString("two"));
        e.reportResult(QString("three"));
        e.reportFinished();

        QList<QString> results;
        QFutureIterator<QString> i(f);
        while (i.hasNext()) {
            results.append(i.next());
        }

        QCOMPARE(results, f.results());

        QFuture<QString>::const_iterator i1 = f.begin(), i2 = i1 + 1;
        QFuture<QString>::const_iterator c1 = i1, c2 = c1 + 1;

        QCOMPARE(i1, i1);
        QCOMPARE(i1, c1);
        QCOMPARE(c1, i1);
        QCOMPARE(c1, c1);
        QCOMPARE(i2, i2);
        QCOMPARE(i2, c2);
        QCOMPARE(c2, i2);
        QCOMPARE(c2, c2);
        QCOMPARE(1 + i1, i1 + 1);
        QCOMPARE(1 + c1, c1 + 1);

        QVERIFY(i1 != i2);
        QVERIFY(i1 != c2);
        QVERIFY(c1 != i2);
        QVERIFY(c1 != c2);
        QVERIFY(i2 != i1);
        QVERIFY(i2 != c1);
        QVERIFY(c2 != i1);
        QVERIFY(c2 != c1);

        QString x1 = *i1;
        QString x2 = *i2;
        QString y1 = *c1;
        QString y2 = *c2;

        QCOMPARE(x1, y1);
        QCOMPARE(x2, y2);

        int i1Size = i1->size();
        int i2Size = i2->size();
        int c1Size = c1->size();
        int c2Size = c2->size();

        QCOMPARE(i1Size, c1Size);
        QCOMPARE(i2Size, c2Size);
    }

    {
        const int resultCount = 20;

        QFutureInterface<int> e;
        e.reportStarted();
        QFuture<int> f =  e.future();

        for (int i = 0; i < resultCount; ++i) {
            e.reportResult(i);
        }

        e.reportFinished();

        {
            QFutureIterator<int> it(f);
            QFutureIterator<int> it2(it);
        }

        {
            QFutureIterator<int> it(f);

            for (int i = 0; i < resultCount - 1; ++i) {
                QVERIFY(it.hasNext());
                QCOMPARE(it.peekNext(), i);
                QCOMPARE(it.next(), i);
            }

            QVERIFY(it.hasNext());
            QCOMPARE(it.peekNext(), resultCount - 1);
            QCOMPARE(it.next(), resultCount - 1);
            QVERIFY(!it.hasNext());
        }

        {
            QFutureIterator<int> it(f);
            QVERIFY(it.hasNext());
            it.toBack();
            QVERIFY(!it.hasNext());
            it.toFront();
            QVERIFY(it.hasNext());
        }
    }
}
void tst_QFuture::iteratorsThread()
{
    const int expectedResultCount = 10;
    QFutureInterface<int> futureInterface;

    // Create result producer thread. The results are
    // produced with delays in order to make the consumer
    // wait.
    QSemaphore sem;
    LambdaThread thread = {[=, &futureInterface, &sem](){
        for (int i = 1; i <= expectedResultCount; i += 2) {
            int result = i;
            futureInterface.reportResult(&result);
            result = i + 1;
            futureInterface.reportResult(&result);
        }

        sem.acquire(2);
        futureInterface.reportFinished();
    }};

    futureInterface.reportStarted();
    QFuture<int> future = futureInterface.future();

    // Iterate over results while the thread is producing them.
    thread.start();
    int resultCount = 0;
    int resultSum = 0;
    for (int result : future) {
        sem.release();
        ++resultCount;
        resultSum += result;
    }
    thread.wait();

    QCOMPARE(resultCount, expectedResultCount);
    QCOMPARE(resultSum, expectedResultCount * (expectedResultCount + 1) / 2);

    // Reverse iterate
    resultSum = 0;
    QFutureIterator<int> it(future);
    it.toBack();
    while (it.hasPrevious())
        resultSum += it.previous();

    QCOMPARE(resultSum, expectedResultCount * (expectedResultCount + 1) / 2);
}

class SignalSlotObject : public QObject
{
Q_OBJECT
public:
    SignalSlotObject()
    : finishedCalled(false),
      canceledCalled(false),
      rangeBegin(0),
      rangeEnd(0) { }

public slots:
    void finished()
    {
        finishedCalled = true;
    }

    void canceled()
    {
        canceledCalled = true;
    }

    void resultReady(int index)
    {
        results.insert(index);
    }

    void progressRange(int begin, int end)
    {
        rangeBegin = begin;
        rangeEnd = end;
    }

    void progress(int progress)
    {
        reportedProgress.insert(progress);
    }
public:
    bool finishedCalled;
    bool canceledCalled;
    QSet<int> results;
    int rangeBegin;
    int rangeEnd;
    QSet<int> reportedProgress;
};

void tst_QFuture::pause()
{
    QFutureInterface<void> Interface;

    Interface.reportStarted();
    QFuture<void> f = Interface.future();

    QVERIFY(!Interface.isPaused());
    f.pause();
    QVERIFY(Interface.isPaused());
    f.resume();
    QVERIFY(!Interface.isPaused());
    f.togglePaused();
    QVERIFY(Interface.isPaused());
    f.togglePaused();
    QVERIFY(!Interface.isPaused());

    Interface.reportFinished();
}

class ResultObject : public QObject
{
Q_OBJECT
public slots:
    void resultReady(int)
    {

    }
public:
};

// Test that that the isPaused() on future result interface returns true
// if we report a lot of results that are not handled.
void tst_QFuture::throttling()
{
    {
        QFutureInterface<void> i;

        i.reportStarted();
        QFuture<void> f = i.future();

        QVERIFY(!i.isThrottled());

        i.setThrottled(true);
        QVERIFY(i.isThrottled());

        i.setThrottled(false);
        QVERIFY(!i.isThrottled());

        i.setThrottled(true);
        QVERIFY(i.isThrottled());

        i.reportFinished();
    }
}

void tst_QFuture::voidConversions()
{
    {
        QFutureInterface<int> iface;
        iface.reportStarted();

        QFuture<int> intFuture(&iface);
        int value = 10;
        iface.reportFinished(&value);

        QFuture<void> voidFuture(intFuture);
        voidFuture = intFuture;

        QVERIFY(voidFuture == intFuture);
    }

    {
        QFuture<void> voidFuture;
        {
            QFutureInterface<QList<int> > iface;
            iface.reportStarted();

            QFuture<QList<int> > listFuture(&iface);
            iface.reportResult(QList<int>() << 1 << 2 << 3);
            voidFuture = listFuture;
        }
        QCOMPARE(voidFuture.resultCount(), 0);
    }
}


#ifndef QT_NO_EXCEPTIONS

QFuture<void> createExceptionFuture()
{
    QFutureInterface<void> i;
    i.reportStarted();
    QFuture<void> f = i.future();

    QException e;
    i.reportException(e);
    i.reportFinished();
    return f;
}

QFuture<int> createExceptionResultFuture()
{
    QFutureInterface<int> i;
    i.reportStarted();
    QFuture<int> f = i.future();
    int r = 0;
    i.reportResult(r);

    QException e;
    i.reportException(e);
    i.reportFinished();
    return f;
}

class DerivedException : public QException
{
public:
    void raise() const override { throw *this; }
    DerivedException *clone() const override { return new DerivedException(*this); }
};

QFuture<void> createDerivedExceptionFuture()
{
    QFutureInterface<void> i;
    i.reportStarted();
    QFuture<void> f = i.future();

    DerivedException e;
    i.reportException(e);
    i.reportFinished();
    return f;
}

struct TestException
{
};

QFuture<int> createCustomExceptionFuture()
{
    QFutureInterface<int> i;
    i.reportStarted();
    QFuture<int> f = i.future();
    int r = 0;
    i.reportResult(r);
    auto exception = std::make_exception_ptr(TestException());
    i.reportException(exception);
    i.reportFinished();
    return f;
}

void tst_QFuture::exceptions()
{
    // test throwing from waitForFinished
    {
        QFuture<void> f = createExceptionFuture();
        bool caught = false;
        try {
            f.waitForFinished();
        } catch (QException &) {
            caught = true;
        }
        QVERIFY(caught);
    }

    // test result()
    {
        QFuture<int> f = createExceptionResultFuture();
        bool caught = false;
        try {
            f.result();
        } catch (QException &) {
            caught = true;
        }
        QVERIFY(caught);
    }

    // test result() and destroy
    {
        bool caught = false;
        try {
            createExceptionResultFuture().result();
        } catch (QException &) {
            caught = true;
        }
        QVERIFY(caught);
    }

    // test results()
    {
        QFuture<int> f = createExceptionResultFuture();
        bool caught = false;
        try {
            f.results();
        } catch (QException &) {
            caught = true;
        }
        QVERIFY(caught);
    }

    // test foreach
    {
        QFuture<int> f = createExceptionResultFuture();
        bool caught = false;
        try {
            foreach (int e, f.results()) {
                Q_UNUSED(e);
                QFAIL("did not get exception");
            }
        } catch (QException &) {
            caught = true;
        }
        QVERIFY(caught);
    }

    // catch derived exceptions
    {
        bool caught = false;
        try {
            createDerivedExceptionFuture().waitForFinished();
        } catch (QException &) {
            caught = true;
        }
        QVERIFY(caught);
    }

    {
        bool caught = false;
        try {
            createDerivedExceptionFuture().waitForFinished();
        } catch (DerivedException &) {
            caught = true;
        }
        QVERIFY(caught);
    }

    // Custom exceptions
    {
        QFuture<int> f = createCustomExceptionFuture();
        bool caught = false;
        try {
            f.result();
        } catch (const TestException &) {
            caught = true;
        }
        QVERIFY(caught);
    }
}

class MyClass
{
public:
    ~MyClass()
    {
        QFuture<void> f = createExceptionFuture();
        try {
            f.waitForFinished();
        } catch (QException &) {
            caught = true;
        }
    }
    static bool caught;
};

bool MyClass::caught = false;

// This is a regression test for QTBUG-18149. where QFuture did not throw
// exceptions if called from destructors when the stack was already unwinding
// due to an exception having been thrown.
void tst_QFuture::nestedExceptions()
{
    try {
        MyClass m;
        Q_UNUSED(m);
        throw 0;
    } catch (int) {}

    QVERIFY(MyClass::caught);
}

#endif // QT_NO_EXCEPTIONS

void tst_QFuture::nonGlobalThreadPool()
{
    static Q_CONSTEXPR int Answer = 42;

    struct UselessTask : QRunnable, QFutureInterface<int>
    {
        QFuture<int> start(QThreadPool *pool)
        {
            setRunnable(this);
            setThreadPool(pool);
            reportStarted();
            QFuture<int> f = future();
            pool->start(this);
            return f;
        }

        void run() override
        {
            const int ms = 100 + (QRandomGenerator::global()->bounded(100) - 100/2);
            QThread::msleep(ms);
            reportResult(Answer);
            reportFinished();
        }
    };

    QThreadPool pool;

    const int numTasks = QThread::idealThreadCount();

    QVector<QFuture<int> > futures;
    futures.reserve(numTasks);

    for (int i = 0; i < numTasks; ++i)
        futures.push_back((new UselessTask)->start(&pool));

    QVERIFY(!pool.waitForDone(0)); // pool is busy (meaning our tasks did end up executing there)

    QVERIFY(pool.waitForDone(10000)); // max sleep time in UselessTask::run is 150ms, so 10s should be enough
                                      // (and the call returns as soon as all tasks finished anyway, so the
                                      // maximum wait time only matters when the test fails)

    Q_FOREACH (const QFuture<int> &future, futures) {
        QVERIFY(future.isFinished());
        QCOMPARE(future.result(), Answer);
    }
}

void tst_QFuture::then()
{
    {
        struct Add
        {

            static int addTwo(int arg) { return arg + 2; }

            int operator()(int arg) const { return arg + 3; }
        };

        QFutureInterface<int> promise;
        QFuture<int> then = promise.future()
                                    .then([](int res) { return res + 1; }) // lambda
                                    .then(Add::addTwo) // function
                                    .then(Add()); // functor

        promise.reportStarted();
        QVERIFY(!then.isStarted());
        QVERIFY(!then.isFinished());

        const int result = 0;
        promise.reportResult(result);
        promise.reportFinished();

        then.waitForFinished();

        QVERIFY(then.isStarted());
        QVERIFY(then.isFinished());
        QCOMPARE(then.result(), result + 6);
    }

    // then() on a ready future
    {
        QFutureInterface<int> promise;
        promise.reportStarted();

        const int result = 0;
        promise.reportResult(result);
        promise.reportFinished();

        QFuture<int> then = promise.future()
                                    .then([](int res1) { return res1 + 1; })
                                    .then([](int res2) { return res2 + 2; })
                                    .then([](int res3) { return res3 + 3; });

        then.waitForFinished();

        QVERIFY(then.isStarted());
        QVERIFY(then.isFinished());
        QCOMPARE(then.result(), result + 6);
    }

    // Continuation of QFuture<void>
    {
        int result = 0;
        QFutureInterface<void> promise;
        QFuture<void> then = promise.future()
                                     .then([&]() { result += 1; })
                                     .then([&]() { result += 2; })
                                     .then([&]() { result += 3; });

        promise.reportStarted();
        QVERIFY(!then.isStarted());
        QVERIFY(!then.isFinished());
        promise.reportFinished();

        then.waitForFinished();

        QVERIFY(then.isStarted());
        QVERIFY(then.isFinished());
        QCOMPARE(result, 6);
    }

    // Continuation returns QFuture<void>
    {
        QFutureInterface<int> promise;
        int value;
        QFuture<void> then =
                promise.future().then([](int res) { return res * 2; }).then([&](int prevResult) {
                    value = prevResult;
                });

        promise.reportStarted();
        QVERIFY(!then.isStarted());
        QVERIFY(!then.isFinished());

        const int result = 5;
        promise.reportResult(result);
        promise.reportFinished();

        then.waitForFinished();

        QVERIFY(then.isStarted());
        QVERIFY(then.isFinished());
        QCOMPARE(value, result * 2);
    }

    // Continuations taking a QFuture argument.
    {
        int value = 0;
        QFutureInterface<int> promise;
        QFuture<void> then = promise.future()
                                     .then([](QFuture<int> f1) { return f1.result() + 1; })
                                     .then([&](QFuture<int> f2) { value = f2.result() + 2; })
                                     .then([&](QFuture<void> f3) {
                                         QVERIFY(f3.isFinished());
                                         value += 3;
                                     });

        promise.reportStarted();
        QVERIFY(!then.isStarted());
        QVERIFY(!then.isFinished());

        const int result = 0;
        promise.reportResult(result);
        promise.reportFinished();

        then.waitForFinished();

        QVERIFY(then.isStarted());
        QVERIFY(then.isFinished());
        QCOMPARE(value, 6);
    }

    // Continuations use a new thread
    {
        Qt::HANDLE threadId1 = nullptr;
        Qt::HANDLE threadId2 = nullptr;
        QFutureInterface<void> promise;
        QFuture<void> then = promise.future()
                                     .then(QtFuture::Launch::Async,
                                           [&]() { threadId1 = QThread::currentThreadId(); })
                                     .then([&]() { threadId2 = QThread::currentThreadId(); });

        promise.reportStarted();
        QVERIFY(!then.isStarted());
        QVERIFY(!then.isFinished());

        promise.reportFinished();

        then.waitForFinished();

        QVERIFY(then.isStarted());
        QVERIFY(then.isFinished());
        QVERIFY(threadId1 != QThread::currentThreadId());
        QVERIFY(threadId2 != QThread::currentThreadId());
        QVERIFY(threadId1 == threadId2);
    }

    // Continuation inherits the launch policy of its parent (QtFuture::Launch::Sync)
    {
        Qt::HANDLE threadId1 = nullptr;
        Qt::HANDLE threadId2 = nullptr;
        QFutureInterface<void> promise;
        QFuture<void> then = promise.future()
                                     .then(QtFuture::Launch::Sync,
                                           [&]() { threadId1 = QThread::currentThreadId(); })
                                     .then(QtFuture::Launch::Inherit,
                                           [&]() { threadId2 = QThread::currentThreadId(); });

        promise.reportStarted();
        QVERIFY(!then.isStarted());
        QVERIFY(!then.isFinished());

        promise.reportFinished();

        then.waitForFinished();

        QVERIFY(then.isStarted());
        QVERIFY(then.isFinished());
        QVERIFY(threadId1 == QThread::currentThreadId());
        QVERIFY(threadId2 == QThread::currentThreadId());
        QVERIFY(threadId1 == threadId2);
    }

    // Continuation inherits the launch policy of its parent (QtFuture::Launch::Async)
    {
        Qt::HANDLE threadId1 = nullptr;
        Qt::HANDLE threadId2 = nullptr;
        QFutureInterface<void> promise;
        QFuture<void> then = promise.future()
                                     .then(QtFuture::Launch::Async,
                                           [&]() { threadId1 = QThread::currentThreadId(); })
                                     .then(QtFuture::Launch::Inherit,
                                           [&]() { threadId2 = QThread::currentThreadId(); });

        promise.reportStarted();
        QVERIFY(!then.isStarted());
        QVERIFY(!then.isFinished());

        promise.reportFinished();

        then.waitForFinished();

        QVERIFY(then.isStarted());
        QVERIFY(then.isFinished());
        QVERIFY(threadId1 != QThread::currentThreadId());
        QVERIFY(threadId2 != QThread::currentThreadId());
    }

    // Continuations use a custom thread pool
    {
        QFutureInterface<void> promise;
        QThreadPool pool;
        QVERIFY(pool.waitForDone(0)); // pool is not busy yet
        QSemaphore semaphore;
        QFuture<void> then = promise.future().then(&pool, [&]() { semaphore.acquire(); });

        promise.reportStarted();
        promise.reportFinished();

        // Make sure the custom thread pool is busy on running the continuation
        QVERIFY(!pool.waitForDone(0));
        semaphore.release();
        then.waitForFinished();

        QVERIFY(then.isStarted());
        QVERIFY(then.isFinished());
        QCOMPARE(then.d.threadPool(), &pool);
    }

    // Continuation inherits parent's thread pool
    {
        Qt::HANDLE threadId1 = nullptr;
        Qt::HANDLE threadId2 = nullptr;
        QFutureInterface<void> promise;

        QThreadPool pool;
        QFuture<void> then1 = promise.future().then(&pool, [&]() {
            threadId1 = QThread::currentThreadId();
        });

        promise.reportStarted();
        promise.reportFinished();

        then1.waitForFinished();
        QVERIFY(pool.waitForDone()); // The pool is not busy after the first continuation is done

        QSemaphore semaphore;
        QFuture<void> then2 = then1.then(QtFuture::Launch::Inherit, [&]() {
            semaphore.acquire();
            threadId2 = QThread::currentThreadId();
        });

        QVERIFY(!pool.waitForDone(0)); // The pool is busy running the 2nd continuation

        semaphore.release();
        then2.waitForFinished();

        QVERIFY(then2.isStarted());
        QVERIFY(then2.isFinished());
        QCOMPARE(then1.d.threadPool(), then2.d.threadPool());
        QCOMPARE(then2.d.threadPool(), &pool);
        QVERIFY(threadId1 != QThread::currentThreadId());
        QVERIFY(threadId2 != QThread::currentThreadId());
    }
}

template<class Type, class Callable>
bool runThenForMoveOnly(Callable &&callable)
{
    QFutureInterface<Type> promise;
    auto future = promise.future();

    auto then = future.then(std::forward<Callable>(callable));

    promise.reportStarted();
    if constexpr (!std::is_same_v<Type, void>)
        promise.reportAndMoveResult(std::make_unique<int>(42));
    promise.reportFinished();
    then.waitForFinished();

    bool success = true;
    if constexpr (!std::is_same_v<decltype(then), QFuture<void>>)
        success &= *then.takeResult() == 42;

    if constexpr (!std::is_same_v<Type, void>)
        success &= !future.isValid();

    return success;
}

void tst_QFuture::thenForMoveOnlyTypes()
{
    QVERIFY(runThenForMoveOnly<UniquePtr>([](UniquePtr res) { return res; }));
    QVERIFY(runThenForMoveOnly<UniquePtr>([](UniquePtr res) { Q_UNUSED(res); }));
    QVERIFY(runThenForMoveOnly<UniquePtr>([](QFuture<UniquePtr> res) { return res.takeResult(); }));
    QVERIFY(runThenForMoveOnly<void>([] { return std::make_unique<int>(42); }));
}

template<class T>
QFuture<T> createCanceledFuture()
{
    QFutureInterface<T> promise;
    promise.reportStarted();
    promise.reportCanceled();
    promise.reportFinished();
    return promise.future();
}

void tst_QFuture::thenOnCanceledFuture()
{
    // Continuations on a canceled future
    {
        int thenResult = 0;
        QFuture<void> then = createCanceledFuture<void>().then([&]() { ++thenResult; }).then([&]() {
            ++thenResult;
        });

        QVERIFY(then.isCanceled());
        QCOMPARE(thenResult, 0);
    }

    // QFuture gets canceled after continuations are set
    {
        QFutureInterface<void> promise;

        int thenResult = 0;
        QFuture<void> then =
                promise.future().then([&]() { ++thenResult; }).then([&]() { ++thenResult; });

        promise.reportStarted();
        promise.reportCanceled();
        promise.reportFinished();

        QVERIFY(then.isCanceled());
        QCOMPARE(thenResult, 0);
    }

    // Same with QtFuture::Launch::Async

    // Continuations on a canceled future
    {
        int thenResult = 0;
        QFuture<void> then = createCanceledFuture<void>()
                                     .then(QtFuture::Launch::Async, [&]() { ++thenResult; })
                                     .then([&]() { ++thenResult; });

        QVERIFY(then.isCanceled());
        QCOMPARE(thenResult, 0);
    }

    // QFuture gets canceled after continuations are set
    {
        QFutureInterface<void> promise;

        int thenResult = 0;
        QFuture<void> then =
                promise.future().then(QtFuture::Launch::Async, [&]() { ++thenResult; }).then([&]() {
                    ++thenResult;
                });

        promise.reportStarted();
        promise.reportCanceled();
        promise.reportFinished();

        QVERIFY(then.isCanceled());
        QCOMPARE(thenResult, 0);
    }
}

#ifndef QT_NO_EXCEPTIONS
void tst_QFuture::thenOnExceptionFuture()
{
    {
        QFutureInterface<int> promise;

        int thenResult = 0;
        QFuture<void> then = promise.future().then([&](int res) { thenResult = res; });

        promise.reportStarted();
        QException e;
        promise.reportException(e);
        promise.reportFinished();

        bool caught = false;
        try {
            then.waitForFinished();
        } catch (QException &) {
            caught = true;
        }
        QVERIFY(caught);
        QCOMPARE(thenResult, 0);
    }

    // Exception handled inside the continuation
    {
        QFutureInterface<int> promise;

        bool caught = false;
        bool caughtByContinuation = false;
        bool success = false;
        int thenResult = 0;
        QFuture<void> then = promise.future()
                                     .then([&](QFuture<int> res) {
                                         try {
                                             thenResult = res.result();
                                         } catch (QException &) {
                                             caughtByContinuation = true;
                                         }
                                     })
                                     .then([&]() { success = true; });

        promise.reportStarted();
        QException e;
        promise.reportException(e);
        promise.reportFinished();

        try {
            then.waitForFinished();
        } catch (QException &) {
            caught = true;
        }

        QCOMPARE(thenResult, 0);
        QVERIFY(!caught);
        QVERIFY(caughtByContinuation);
        QVERIFY(success);
    }

    // Exception future
    {
        QFutureInterface<int> promise;
        promise.reportStarted();
        QException e;
        promise.reportException(e);
        promise.reportFinished();

        int thenResult = 0;
        QFuture<void> then = promise.future().then([&](int res) { thenResult = res; });

        bool caught = false;
        try {
            then.waitForFinished();
        } catch (QException &) {
            caught = true;
        }
        QVERIFY(caught);
        QCOMPARE(thenResult, 0);
    }

    // Same with QtFuture::Launch::Async
    {
        QFutureInterface<int> promise;

        int thenResult = 0;
        QFuture<void> then =
                promise.future().then(QtFuture::Launch::Async, [&](int res) { thenResult = res; });

        promise.reportStarted();
        QException e;
        promise.reportException(e);
        promise.reportFinished();

        bool caught = false;
        try {
            then.waitForFinished();
        } catch (QException &) {
            caught = true;
        }
        QVERIFY(caught);
        QCOMPARE(thenResult, 0);
    }

    // Exception future
    {
        QFutureInterface<int> promise;
        promise.reportStarted();
        QException e;
        promise.reportException(e);
        promise.reportFinished();

        int thenResult = 0;
        QFuture<void> then =
                promise.future().then(QtFuture::Launch::Async, [&](int res) { thenResult = res; });

        bool caught = false;
        try {
            then.waitForFinished();
        } catch (QException &) {
            caught = true;
        }
        QVERIFY(caught);
        QCOMPARE(thenResult, 0);
    }
}

template<class Exception, bool hasTestMsg = false>
QFuture<void> createExceptionContinuation(QtFuture::Launch policy = QtFuture::Launch::Sync)
{
    QFutureInterface<void> promise;

    auto then = promise.future().then(policy, [] {
        if constexpr (hasTestMsg)
            throw Exception("TEST");
        else
            throw Exception();
    });

    promise.reportStarted();
    promise.reportFinished();

    return then;
}

void tst_QFuture::thenThrows()
{
    // Continuation throws a QException
    {
        auto future = createExceptionContinuation<QException>();

        bool caught = false;
        try {
            future.waitForFinished();
        } catch (const QException &) {
            caught = true;
        }
        QVERIFY(caught);
    }

    // Continuation throws an exception derived from QException
    {
        auto future = createExceptionContinuation<DerivedException>();

        bool caught = false;
        try {
            future.waitForFinished();
        } catch (const QException &) {
            caught = true;
        } catch (const std::exception &) {
            QFAIL("The exception should be caught by the above catch block.");
        }

        QVERIFY(caught);
    }

    // Continuation throws std::exception
    {
        auto future = createExceptionContinuation<std::runtime_error, true>();

        bool caught = false;
        try {
            future.waitForFinished();
        } catch (const QException &) {
            QFAIL("The exception should be caught by the below catch block.");
        } catch (const std::exception &e) {
            QCOMPARE(e.what(), "TEST");
            caught = true;
        }

        QVERIFY(caught);
    }

    // Same with QtFuture::Launch::Async
    {
        auto future = createExceptionContinuation<QException>(QtFuture::Launch::Async);

        bool caught = false;
        try {
            future.waitForFinished();
        } catch (const QException &) {
            caught = true;
        }
        QVERIFY(caught);
    }
}

void tst_QFuture::onFailed()
{
    // Ready exception void future
    {
        int checkpoint = 0;
        auto future = createExceptionFuture().then([&] { checkpoint = 1; }).onFailed([&] {
            checkpoint = 2;
        });

        try {
            future.waitForFinished();
        } catch (...) {
            checkpoint = 3;
        }
        QCOMPARE(checkpoint, 2);
    }

    // std::exception handler
    {
        QFutureInterface<int> promise;

        int checkpoint = 0;
        auto then = promise.future()
                            .then([&](int res) {
                                throw std::exception();
                                return res;
                            })
                            .then([&](int res) { return res + 1; })
                            .onFailed([&](const QException &) {
                                checkpoint = 1;
                                return -1;
                            })
                            .onFailed([&](const std::exception &) {
                                checkpoint = 2;
                                return -1;
                            })
                            .onFailed([&] {
                                checkpoint = 3;
                                return -1;
                            });

        promise.reportStarted();
        promise.reportResult(1);
        promise.reportFinished();

        int res = 0;
        try {
            res = then.result();
        } catch (...) {
            checkpoint = 4;
        }
        QCOMPARE(checkpoint, 2);
        QCOMPARE(res, -1);
    }

    // then() throws an exception derived from QException
    {
        QFutureInterface<int> promise;

        int checkpoint = 0;
        auto then = promise.future()
                            .then([&](int res) {
                                throw DerivedException();
                                return res;
                            })
                            .then([&](int res) { return res + 1; })
                            .onFailed([&](const QException &) {
                                checkpoint = 1;
                                return -1;
                            })
                            .onFailed([&](const std::exception &) {
                                checkpoint = 2;
                                return -1;
                            })
                            .onFailed([&] {
                                checkpoint = 3;
                                return -1;
                            });

        promise.reportStarted();
        promise.reportResult(1);
        promise.reportFinished();

        int res = 0;
        try {
            res = then.result();
        } catch (...) {
            checkpoint = 4;
        }
        QCOMPARE(checkpoint, 1);
        QCOMPARE(res, -1);
    }

    // then() throws a custom exception
    {
        QFutureInterface<int> promise;

        int checkpoint = 0;
        auto then = promise.future()
                            .then([&](int res) {
                                throw TestException();
                                return res;
                            })
                            .then([&](int res) { return res + 1; })
                            .onFailed([&](const QException &) {
                                checkpoint = 1;
                                return -1;
                            })
                            .onFailed([&](const std::exception &) {
                                checkpoint = 2;
                                return -1;
                            })
                            .onFailed([&] {
                                checkpoint = 3;
                                return -1;
                            });

        promise.reportStarted();
        promise.reportResult(1);
        promise.reportFinished();

        int res = 0;
        try {
            res = then.result();
        } catch (...) {
            checkpoint = 4;
        }
        QCOMPARE(checkpoint, 3);
        QCOMPARE(res, -1);
    }

    // Custom exception handler
    {
        struct TestException
        {
        };

        QFutureInterface<int> promise;

        int checkpoint = 0;
        auto then = promise.future()
                            .then([&](int res) {
                                throw TestException();
                                return res;
                            })
                            .then([&](int res) { return res + 1; })
                            .onFailed([&](const QException &) {
                                checkpoint = 1;
                                return -1;
                            })
                            .onFailed([&](const TestException &) {
                                checkpoint = 2;
                                return -1;
                            })
                            .onFailed([&] {
                                checkpoint = 3;
                                return -1;
                            });

        promise.reportStarted();
        promise.reportResult(1);
        promise.reportFinished();

        int res = 0;
        try {
            res = then.result();
        } catch (...) {
            checkpoint = 4;
        }
        QCOMPARE(checkpoint, 2);
        QCOMPARE(res, -1);
    }

    // Handle all exceptions
    {
        QFutureInterface<int> promise;

        int checkpoint = 0;
        auto then = promise.future()
                            .then([&](int res) {
                                throw QException();
                                return res;
                            })
                            .then([&](int res) { return res + 1; })
                            .onFailed([&] {
                                checkpoint = 1;
                                return -1;
                            })
                            .onFailed([&](const QException &) {
                                checkpoint = 2;
                                return -1;
                            });

        promise.reportStarted();
        promise.reportResult(1);
        promise.reportFinished();

        int res = 0;
        try {
            res = then.result();
        } catch (...) {
            checkpoint = 3;
        }
        QCOMPARE(checkpoint, 1);
        QCOMPARE(res, -1);
    }

    // Handler throws exception
    {
        QFutureInterface<int> promise;

        int checkpoint = 0;
        auto then = promise.future()
                            .then([&](int res) {
                                throw QException();
                                return res;
                            })
                            .then([&](int res) { return res + 1; })
                            .onFailed([&](const QException &) {
                                checkpoint = 1;
                                throw QException();
                                return -1;
                            })
                            .onFailed([&] {
                                checkpoint = 2;
                                return -1;
                            });

        promise.reportStarted();
        promise.reportResult(1);
        promise.reportFinished();

        int res = 0;
        try {
            res = then.result();
        } catch (...) {
            checkpoint = 3;
        }
        QCOMPARE(checkpoint, 2);
        QCOMPARE(res, -1);
    }

    // No handler for exception
    {
        QFutureInterface<int> promise;

        int checkpoint = 0;
        auto then = promise.future()
                            .then([&](int res) {
                                throw QException();
                                return res;
                            })
                            .then([&](int res) { return res + 1; })
                            .onFailed([&](const std::exception &) {
                                checkpoint = 1;
                                throw std::exception();
                                return -1;
                            })
                            .onFailed([&](QException &) {
                                checkpoint = 2;
                                return -1;
                            });

        promise.reportStarted();
        promise.reportResult(1);
        promise.reportFinished();

        int res = 0;
        try {
            res = then.result();
        } catch (...) {
            checkpoint = 3;
        }
        QCOMPARE(checkpoint, 3);
        QCOMPARE(res, 0);
    }

    // onFailed on a canceled future
    {
        auto future = createCanceledFuture<int>()
                              .then([](int) { return 42; })
                              .onCanceled([] { return -1; })
                              .onFailed([] { return -2; });
        QCOMPARE(future.result(), -1);
    }
}

template<class Callable>
bool runForCallable(Callable &&handler)
{
    QFuture<int> future = createExceptionResultFuture()
                                  .then([&](int) { return 1; })
                                  .onFailed(std::forward<Callable>(handler));

    int res = 0;
    try {
        res = future.result();
    } catch (...) {
        return false;
    }
    return res == -1;
}

int foo()
{
    return -1;
}

void tst_QFuture::onFailedTestCallables()
{
    QVERIFY(runForCallable([&] { return -1; }));
    QVERIFY(runForCallable(foo));
    QVERIFY(runForCallable(&foo));

    std::function<int()> func = foo;
    QVERIFY(runForCallable(func));

    struct Functor1
    {
        int operator()() { return -1; }
        static int foo() { return -1; }
    };
    QVERIFY(runForCallable(Functor1()));
    QVERIFY(runForCallable(Functor1::foo));

    struct Functor2
    {
        int operator()() const { return -1; }
        static int foo() { return -1; }
    };
    QVERIFY(runForCallable(Functor2()));

    struct Functor3
    {
        int operator()() const noexcept { return -1; }
        static int foo() { return -1; }
    };
    QVERIFY(runForCallable(Functor3()));
}

template<class Callable>
bool runOnFailedForMoveOnly(Callable &&callable)
{
    QFutureInterface<UniquePtr> promise;
    auto future = promise.future();

    auto failedFuture = future.onFailed(std::forward<Callable>(callable));

    promise.reportStarted();
    QException e;
    promise.reportException(e);
    promise.reportFinished();

    return *failedFuture.takeResult() == -1;
}

void tst_QFuture::onFailedForMoveOnlyTypes()
{
    QVERIFY(runOnFailedForMoveOnly([](const QException &) { return std::make_unique<int>(-1); }));
    QVERIFY(runOnFailedForMoveOnly([] { return std::make_unique<int>(-1); }));
}

#endif // QT_NO_EXCEPTIONS

void tst_QFuture::onCanceled()
{
    // Canceled int future
    {
        auto future = createCanceledFuture<int>().then([](int) { return 42; }).onCanceled([] {
            return -1;
        });
        QCOMPARE(future.result(), -1);
    }

    // Canceled void future
    {
        int checkpoint = 0;
        auto future = createCanceledFuture<void>().then([&] { checkpoint = 42; }).onCanceled([&] {
            checkpoint = -1;
        });
        QCOMPARE(checkpoint, -1);
    }

    // onCanceled propagates result
    {
        QFutureInterface<int> promise;
        auto future =
                promise.future().then([](int res) { return res; }).onCanceled([] { return -1; });

        promise.reportStarted();
        promise.reportResult(42);
        promise.reportFinished();
        QCOMPARE(future.result(), 42);
    }

    // onCanceled propagates move-only result
    {
        QFutureInterface<UniquePtr> promise;
        auto future = promise.future().then([](UniquePtr res) { return res; }).onCanceled([] {
            return std::make_unique<int>(-1);
        });

        promise.reportStarted();
        promise.reportAndMoveResult(std::make_unique<int>(42));
        promise.reportFinished();
        QCOMPARE(*future.takeResult(), 42);
    }

#ifndef QT_NO_EXCEPTIONS
    // onCanceled propagates exceptions
    {
        QFutureInterface<int> promise;
        auto future = promise.future()
                              .then([](int res) {
                                  throw std::runtime_error("error");
                                  return res;
                              })
                              .onCanceled([] { return 2; })
                              .onFailed([] { return 3; });

        promise.reportStarted();
        promise.reportResult(1);
        promise.reportFinished();
        QCOMPARE(future.result(), 3);
    }

    // onCanceled throws
    {
        auto future = createCanceledFuture<int>()
                              .then([](int) { return 42; })
                              .onCanceled([] {
                                  throw std::runtime_error("error");
                                  return -1;
                              })
                              .onFailed([] { return -2; });

        QCOMPARE(future.result(), -2);
    }

#endif // QT_NO_EXCEPTIONS
}

void tst_QFuture::testSingleResult(const UniquePtr &p)
{
    QVERIFY(p.get() != nullptr);
}

void tst_QFuture::testSingleResult(const std::vector<int> &v)
{
    QVERIFY(!v.empty());
}

template<class T>
void tst_QFuture::testSingleResult(const T &unknown)
{
    Q_UNUSED(unknown);
}


template<class T>
void tst_QFuture::testFutureTaken(QFuture<T> &noMoreFuture)
{
    QCOMPARE(noMoreFuture.isValid(), false);
    QCOMPARE(noMoreFuture.resultCount(), 0);
    QCOMPARE(noMoreFuture.isStarted(), false);
    QCOMPARE(noMoreFuture.isRunning(), false);
    QCOMPARE(noMoreFuture.isPaused(), false);
    QCOMPARE(noMoreFuture.isFinished(), false);
    QCOMPARE(noMoreFuture.progressValue(), 0);
}

template<class T>
void tst_QFuture::testTakeResults(QFuture<T> future, size_type resultCount)
{
    auto copy = future;
    QVERIFY(future.isFinished());
    QVERIFY(future.isValid());
    QCOMPARE(size_type(future.resultCount()), resultCount);
    QVERIFY(copy.isFinished());
    QVERIFY(copy.isValid());
    QCOMPARE(size_type(copy.resultCount()), resultCount);

    auto vec = future.takeResults();
    QCOMPARE(vec.size(), resultCount);

    for (const auto &r : vec) {
        testSingleResult(r);
        if (QTest::currentTestFailed())
            return;
    }

    testFutureTaken(future);
    if (QTest::currentTestFailed())
        return;
    testFutureTaken(copy);
}

void tst_QFuture::takeResults()
{
    // Test takeResults() for movable types (whether or not copyable).

    // std::unique_ptr<int> supports only move semantics:
    QFutureInterface<UniquePtr> moveIface;
    moveIface.reportStarted();

    // std::vector<int> supports both copy and move:
    QFutureInterface<std::vector<int>> copyIface;
    copyIface.reportStarted();

    const int expectedCount = 10;

    for (int i = 0; i < expectedCount; ++i) {
        moveIface.reportAndMoveResult(UniquePtr{new int(0b101010)}, i);
        copyIface.reportAndMoveResult(std::vector<int>{1,2,3,4,5}, i);
    }

    moveIface.reportFinished();
    copyIface.reportFinished();

    testTakeResults(moveIface.future(), size_type(expectedCount));
    if (QTest::currentTestFailed())
        return;

    testTakeResults(copyIface.future(), size_type(expectedCount));
}

void tst_QFuture::takeResult()
{
    QFutureInterface<UniquePtr> iface;
    iface.reportStarted();
    iface.reportAndMoveResult(UniquePtr{new int(0b101010)}, 0);
    iface.reportFinished();

    auto future = iface.future();
    QVERIFY(future.isFinished());
    QVERIFY(future.isValid());
    QCOMPARE(future.resultCount(), 1);

    auto result = future.takeResult();
    testFutureTaken(future);
    if (QTest::currentTestFailed())
        return;
    testSingleResult(result);
}

void tst_QFuture::runAndTake()
{
    // Test if a 'moving' future can be used by
    // QtConcurrent::run.

    auto rabbit = [](){
        // Let's wait a bit to give the test below some time
        // to sync up with us with its watcher.
        QThread::currentThread()->msleep(100);
        return UniquePtr(new int(10));
    };

    QTestEventLoop loop;
    QFutureWatcher<UniquePtr> watcha;
    connect(&watcha, &QFutureWatcher<UniquePtr>::finished, [&loop](){
        loop.exitLoop();
    });

    auto gotcha = QtConcurrent::run(rabbit);
    watcha.setFuture(gotcha);

    loop.enterLoopMSecs(500);
    if (loop.timeout())
        QSKIP("Failed to run the task, nothing to test");

    gotcha = watcha.future();
    testTakeResults(gotcha, size_type(1));
}

void tst_QFuture::resultsReadyAt_data()
{
    QTest::addColumn<bool>("testMove");

    QTest::addRow("reportResult") << false;
    QTest::addRow("reportAndMoveResult") << true;
}

void tst_QFuture::resultsReadyAt()
{
    QFETCH(const bool, testMove);

    QFutureInterface<int> iface;
    QFutureWatcher<int> watcher;
    watcher.setFuture(iface.future());

    QTestEventLoop eventProcessor;
    connect(&watcher, &QFutureWatcher<int>::finished, &eventProcessor, &QTestEventLoop::exitLoop);

    const int nExpectedResults = 4;
    int reported = 0;
    int taken = 0;
    connect(&watcher, &QFutureWatcher<int>::resultsReadyAt,
            [&iface, &reported, &taken](int begin, int end)
    {
        auto future = iface.future();
        QVERIFY(end - begin > 0);
        for (int i = begin; i < end; ++i, ++reported) {
            QVERIFY(future.isResultReadyAt(i));
            taken |= 1 << i;
        }
    });

    auto report = [&iface, testMove](int index)
    {
        int dummyResult = 0b101010;
        if (testMove)
            iface.reportAndMoveResult(std::move(dummyResult), index);
        else
            iface.reportResult(&dummyResult, index);
    };

    const QSignalSpy readyCounter(&watcher, &QFutureWatcher<int>::resultsReadyAt);
    QTimer::singleShot(0, [&iface, &report]{
        // With filter mode == true, the result may go into the pending results.
        // Reporting it as ready will allow an application to try and access the
        // result, crashing on invalid (store.end()) iterator dereferenced.
        iface.setFilterMode(true);
        iface.reportStarted();
        report(0);
        report(1);
        // This one - should not be reported (it goes into pending):
        report(3);
        // Let's close the 'gap' and make them all ready:
        report(-1);
        iface.reportFinished();
    });

    // Run event loop, QCoreApplication::postEvent is in use
    // in QFutureInterface:
    eventProcessor.enterLoopMSecs(2000);
    QVERIFY(!eventProcessor.timeout());
    if (QTest::currentTestFailed()) // Failed in our lambda observing 'ready at'
        return;

    QCOMPARE(reported, nExpectedResults);
    QCOMPARE(nExpectedResults, iface.future().resultCount());
    QCOMPARE(readyCounter.count(), 3);
    QCOMPARE(taken, 0b1111);
}

template <class T>
auto makeFutureInterface(T &&result)
{
    QFutureInterface<T> f;

    f.reportStarted();
    f.reportResult(std::forward<T>(result));
    f.reportFinished();

    return f;
}

void tst_QFuture::takeResultWorksForTypesWithoutDefaultCtor()
{
    struct Foo
    {
        Foo() = delete;
        explicit Foo(int i) : _i(i) {}

        int _i = -1;
    };

    auto f = makeFutureInterface(Foo(42));

    QCOMPARE(f.takeResult()._i, 42);
}
void tst_QFuture::canceledFutureIsNotValid()
{
    auto f = makeFutureInterface(42);

    f.cancel();

    QVERIFY(!f.isValid());
}

void tst_QFuture::signalConnect()
{
    // No arg
    {
        SenderObject sender;
        auto future =
                QtFuture::connect(&sender, &SenderObject::noArgSignal).then([&] { return true; });
        sender.emitNoArg();
        QCOMPARE(future.result(), true);
    }

    // One arg
    {
        SenderObject sender;
        auto future = QtFuture::connect(&sender, &SenderObject::intArgSignal).then([](int value) {
            return value;
        });
        sender.emitIntArg(42);
        QCOMPARE(future.result(), 42);
    }

    // Const ref arg
    {
        SenderObject sender;
        auto future =
                QtFuture::connect(&sender, &SenderObject::constRefArg).then([](QString value) {
                    return value;
                });
        sender.emitConstRefArg(QString("42"));
        QCOMPARE(future.result(), "42");
    }

    // Multiple args
    {
        SenderObject sender;
        using TupleArgs = std::tuple<int, double, QString>;
        auto future =
                QtFuture::connect(&sender, &SenderObject::multipleArgs).then([](TupleArgs values) {
                    return values;
                });
        sender.emitMultipleArgs(42, 42.5, "42");
        auto result = future.result();
        QCOMPARE(std::get<0>(result), 42);
        QCOMPARE(std::get<1>(result), 42.5);
        QCOMPARE(std::get<2>(result), "42");
    }

    // Sender destroyed
    {
        SenderObject *sender = new SenderObject();

        auto future = QtFuture::connect(sender, &SenderObject::intArgSignal);

        QSignalSpy spy(sender, &QObject::destroyed);
        sender->deleteLater();

        // emit the signal when sender is being destroyed
        QObject::connect(sender, &QObject::destroyed, [sender] { sender->emitIntArg(42); });
        spy.wait();

        QVERIFY(future.isCanceled());
        QVERIFY(!future.isValid());
    }
}

QTEST_MAIN(tst_QFuture)
#include "tst_qfuture.moc"
