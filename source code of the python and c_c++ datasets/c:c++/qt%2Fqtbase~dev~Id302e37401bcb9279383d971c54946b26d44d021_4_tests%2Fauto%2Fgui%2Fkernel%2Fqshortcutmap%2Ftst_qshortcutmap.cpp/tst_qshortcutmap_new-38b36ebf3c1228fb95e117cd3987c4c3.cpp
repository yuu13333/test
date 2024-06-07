/****************************************************************************
**
** Copyright (C) 2021 The Qt Company Ltd.
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

#include <QTest>
#include <QtGui/qguiapplication.h>
#include <QtGui/qshortcut.h>
#include <QtGui/qpainter.h>
#include <QtGui/qrasterwindow.h>
#include <QtGui/qscreen.h>
#include <QtGui/qwindow.h>
#include <private/qshortcutmap_p.h>
#include <private/qguiapplication_p.h>

class tst_QShortcutMap : public QObject
{
    Q_OBJECT
public:

private slots:
    void ownerDeleted_QTBUG_96551();
};

class ColoredWindow : public QRasterWindow {
public:
    ColoredWindow(QColor c) : m_color(c) {}

protected:
    void paintEvent(QPaintEvent *event) override;

private:
    const QColor m_color;
};

void ColoredWindow::paintEvent(QPaintEvent *)
{
    QPainter p(this);
    p.fillRect(QRect(QPoint(), size()), m_color);
}

static void sendKey(QWindow *target, Qt::Key k, char c, Qt::KeyboardModifiers modifiers)
{
    QTest::sendKeyEvent(QTest::Press, target, k, c, modifiers);
    QTest::sendKeyEvent(QTest::Release, target, k, c, modifiers);
}

static bool simpleContextMatcher(QObject *obj, Qt::ShortcutContext context)
{
    return obj != nullptr;
}

void tst_QShortcutMap::ownerDeleted_QTBUG_96551()
{
    ColoredWindow w(Qt::yellow);
    w.setTitle(QTest::currentTestFunction());
    w.resize(QGuiApplication::primaryScreen()->size() / 4);
    w.show();
    QVERIFY(QTest::qWaitForWindowExposed(&w));
    QTRY_VERIFY(QGuiApplication::applicationState() == Qt::ApplicationActive);

    // Represents incorrect use of the QShortcutMap API, where the
    // owner is destroyed without first removing the shortcut.
    {
        QObject *badOwner = new QObject();
        QGuiApplicationPrivate::instance()->shortcutMap.addShortcut(
                    badOwner, QKeySequence(QKeySequence::StandardKey::Delete),
                    Qt::ShortcutContext::WindowShortcut, simpleContextMatcher);
        delete badOwner;
    }

    // Success if no crash
    sendKey(&w, Qt::Key_Delete, 0, Qt::NoModifier);

    QVERIFY(true);
}

QTEST_MAIN(tst_QShortcutMap)
#include "tst_qshortcutmap.moc"
