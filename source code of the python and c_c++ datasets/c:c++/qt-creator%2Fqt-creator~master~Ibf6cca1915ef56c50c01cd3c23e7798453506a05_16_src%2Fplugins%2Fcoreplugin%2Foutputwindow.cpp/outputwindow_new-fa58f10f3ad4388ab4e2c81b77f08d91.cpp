/****************************************************************************
**
** Copyright (C) 2016 The Qt Company Ltd.
** Contact: https://www.qt.io/licensing/
**
** This file is part of Qt Creator.
**
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
****************************************************************************/

#include "outputwindow.h"

#include "actionmanager/actionmanager.h"
#include "coreconstants.h"
#include "icore.h"

#include <utils/outputformatter.h>
#include <utils/synchronousprocess.h>

#include <QAction>
#include <QRegularExpression>
#include <QScrollBar>
#include <QTextBlock>

using namespace Utils;

namespace Core {

namespace Internal {

class OutputWindowPrivate
{
public:
    OutputWindowPrivate(QTextDocument *document)
        : cursor(document)
    {
    }

    ~OutputWindowPrivate()
    {
        ICore::removeContextObject(outputWindowContext);
        delete outputWindowContext;
    }

    IContext *outputWindowContext = nullptr;
    Utils::OutputFormatter *formatter = nullptr;

    bool enforceNewline = false;
    bool scrollToBottom = true;
    bool linksActive = true;
    bool m_zoomEnabled = false;
    float m_originalFontSize = 0.;
    int maxCharCount = Core::Constants::DEFAULT_MAX_CHAR_COUNT;
    Qt::MouseButton mouseButtonPressed = Qt::NoButton;
    QTextCursor cursor;
    QString filterText;
    QTextBlock lastFilteredBlock;
    OutputWindow::FilterModeFlags filterMode = OutputWindow::FilterModeFlag::Default;
};

} // namespace Internal

/*******************/

OutputWindow::OutputWindow(Context context, QWidget *parent)
    : QPlainTextEdit(parent)
    , d(new Internal::OutputWindowPrivate(document()))
{
    setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOn);
    //setCenterOnScroll(false);
    setFrameShape(QFrame::NoFrame);
    setMouseTracking(true);
    setUndoRedoEnabled(false);

    d->outputWindowContext = new IContext;
    d->outputWindowContext->setContext(context);
    d->outputWindowContext->setWidget(this);
    ICore::addContextObject(d->outputWindowContext);

    auto undoAction = new QAction(this);
    auto redoAction = new QAction(this);
    auto cutAction = new QAction(this);
    auto copyAction = new QAction(this);
    auto pasteAction = new QAction(this);
    auto selectAllAction = new QAction(this);

    ActionManager::registerAction(undoAction, Constants::UNDO, context);
    ActionManager::registerAction(redoAction, Constants::REDO, context);
    ActionManager::registerAction(cutAction, Constants::CUT, context);
    ActionManager::registerAction(copyAction, Constants::COPY, context);
    ActionManager::registerAction(pasteAction, Constants::PASTE, context);
    ActionManager::registerAction(selectAllAction, Constants::SELECTALL, context);

    connect(undoAction, &QAction::triggered, this, &QPlainTextEdit::undo);
    connect(redoAction, &QAction::triggered, this, &QPlainTextEdit::redo);
    connect(cutAction, &QAction::triggered, this, &QPlainTextEdit::cut);
    connect(copyAction, &QAction::triggered, this, &QPlainTextEdit::copy);
    connect(pasteAction, &QAction::triggered, this, &QPlainTextEdit::paste);
    connect(selectAllAction, &QAction::triggered, this, &QPlainTextEdit::selectAll);

    connect(this, &QPlainTextEdit::undoAvailable, undoAction, &QAction::setEnabled);
    connect(this, &QPlainTextEdit::redoAvailable, redoAction, &QAction::setEnabled);
    connect(this, &QPlainTextEdit::copyAvailable, cutAction, &QAction::setEnabled);  // OutputWindow never read-only
    connect(this, &QPlainTextEdit::copyAvailable, copyAction, &QAction::setEnabled);

    undoAction->setEnabled(false);
    redoAction->setEnabled(false);
    cutAction->setEnabled(false);
    copyAction->setEnabled(false);

    m_scrollTimer.setInterval(10);
    m_scrollTimer.setSingleShot(true);
    connect(&m_scrollTimer, &QTimer::timeout,
            this, &OutputWindow::scrollToBottom);
    m_lastMessage.start();

    d->m_originalFontSize = font().pointSizeF();
}

OutputWindow::~OutputWindow()
{
    delete d;
}

void OutputWindow::mousePressEvent(QMouseEvent * e)
{
    d->mouseButtonPressed = e->button();
    QPlainTextEdit::mousePressEvent(e);
}

void OutputWindow::mouseReleaseEvent(QMouseEvent *e)
{
    if (d->linksActive && d->mouseButtonPressed == Qt::LeftButton) {
        const QString href = anchorAt(e->pos());
        if (d->formatter)
            d->formatter->handleLink(href);
    }

    // Mouse was released, activate links again
    d->linksActive = true;
    d->mouseButtonPressed = Qt::NoButton;

    QPlainTextEdit::mouseReleaseEvent(e);
}

void OutputWindow::mouseMoveEvent(QMouseEvent *e)
{
    // Cursor was dragged to make a selection, deactivate links
    if (d->mouseButtonPressed != Qt::NoButton && textCursor().hasSelection())
        d->linksActive = false;

    if (!d->linksActive || anchorAt(e->pos()).isEmpty())
        viewport()->setCursor(Qt::IBeamCursor);
    else
        viewport()->setCursor(Qt::PointingHandCursor);
    QPlainTextEdit::mouseMoveEvent(e);
}

void OutputWindow::resizeEvent(QResizeEvent *e)
{
    //Keep scrollbar at bottom of window while resizing, to ensure we keep scrolling
    //This can happen if window is resized while building, or if the horizontal scrollbar appears
    bool atBottom = isScrollbarAtBottom();
    QPlainTextEdit::resizeEvent(e);
    if (atBottom)
        scrollToBottom();
}

void OutputWindow::keyPressEvent(QKeyEvent *ev)
{
    QPlainTextEdit::keyPressEvent(ev);

    //Ensure we scroll also on Ctrl+Home or Ctrl+End
    if (ev->matches(QKeySequence::MoveToStartOfDocument))
        verticalScrollBar()->triggerAction(QAbstractSlider::SliderToMinimum);
    else if (ev->matches(QKeySequence::MoveToEndOfDocument))
        verticalScrollBar()->triggerAction(QAbstractSlider::SliderToMaximum);
}

OutputFormatter *OutputWindow::formatter() const
{
    return d->formatter;
}

void OutputWindow::setFormatter(OutputFormatter *formatter)
{
    d->formatter = formatter;
    if (d->formatter) {
        d->formatter->setPlainTextEdit(this);
        connect(d->formatter, &OutputFormatter::contentChanged, this, &OutputWindow::filterNewContent);
    }
}

void OutputWindow::showEvent(QShowEvent *e)
{
    QPlainTextEdit::showEvent(e);
    if (d->scrollToBottom)
        verticalScrollBar()->setValue(verticalScrollBar()->maximum());
    d->scrollToBottom = false;
}

void OutputWindow::wheelEvent(QWheelEvent *e)
{
    if (d->m_zoomEnabled) {
        if (e->modifiers() & Qt::ControlModifier) {
            float delta = e->angleDelta().y() / 120.f;
            zoomInF(delta);
            emit wheelZoom();
            return;
        }
    }
    QAbstractScrollArea::wheelEvent(e);
    updateMicroFocus();
}

void OutputWindow::setBaseFont(const QFont &newFont)
{
    float zoom = fontZoom();
    d->m_originalFontSize = newFont.pointSizeF();
    QFont tmp = newFont;
    float newZoom = qMax(d->m_originalFontSize + zoom, 4.0f);
    tmp.setPointSizeF(newZoom);
    setFont(tmp);
}

float OutputWindow::fontZoom() const
{
    return font().pointSizeF() - d->m_originalFontSize;
}

void OutputWindow::setFontZoom(float zoom)
{
    QFont f = font();
    if (f.pointSizeF() == d->m_originalFontSize + zoom)
        return;
    float newZoom = qMax(d->m_originalFontSize + zoom, 4.0f);
    f.setPointSizeF(newZoom);
    setFont(f);
}

void OutputWindow::setWheelZoomEnabled(bool enabled)
{
    d->m_zoomEnabled = enabled;
}

void OutputWindow::setHighlightBgColor(const QColor &bgColor)
{
    m_highlightBgColor = bgColor;
}

void OutputWindow::setHighlightTextColor(const QColor &textColor)
{
    m_highlightTextColor = textColor;
}

QString OutputWindow::filterText() const
{
    return d->filterText;
}

void OutputWindow::setFilterText(const QString &filterText)
{
    if (d->filterText != filterText) {
        d->lastFilteredBlock = {};
        d->filterText = filterText;

        // Update textedit's background color
        if (filterText.isEmpty()) {
            d->formatter->plainTextEdit()->setPalette({});
        } else {
            QPalette pal;
            pal.setColor(QPalette::Active, QPalette::Base, m_highlightBgColor);
            pal.setColor(QPalette::Inactive, QPalette::Base, m_highlightBgColor.darker(120));
            pal.setColor(QPalette::Active, QPalette::Text, m_highlightTextColor);
            pal.setColor(QPalette::Inactive, QPalette::Text, m_highlightTextColor.darker(120));
            d->formatter->plainTextEdit()->setPalette(pal);
        }

        setReadOnly(!filterText.isEmpty());
        filterNewContent();
    }
}

OutputWindow::FilterModeFlags OutputWindow::filterMode() const
{
    return d->filterMode;
}

void OutputWindow::setFilterMode(OutputWindow::FilterModeFlag filterMode, bool enabled)
{
    if (d->filterMode.testFlag(filterMode) != enabled) {
        d->filterMode.setFlag(filterMode, enabled);
        d->lastFilteredBlock = {};
        filterNewContent();
    }
}

void OutputWindow::filterNewContent()
{
    bool atBottom = isScrollbarAtBottom();
    QPlainTextEdit *textEdit = d->formatter->plainTextEdit();
    if (!textEdit)
        return;

    QTextDocument *document = textEdit->document();

    auto &lastBlock = d->lastFilteredBlock;

    if (!lastBlock.isValid())
        lastBlock = document->begin();

    if (d->filterMode.testFlag(OutputWindow::FilterModeFlag::RegExp)) {
        QRegularExpression regExp(d->filterText);
        if (!d->filterMode.testFlag(OutputWindow::FilterModeFlag::CaseSensitive))
            regExp.setPatternOptions(QRegularExpression::CaseInsensitiveOption);

        for (; lastBlock != document->end(); lastBlock = lastBlock.next())
            lastBlock.setVisible(d->filterText.isEmpty()
                                 || regExp.match(lastBlock.text()).hasMatch());
    } else {
        if (d->filterMode.testFlag(OutputWindow::FilterModeFlag::CaseSensitive)) {
            for (; lastBlock != document->end(); lastBlock = lastBlock.next())
                lastBlock.setVisible(d->filterText.isEmpty()
                                     || lastBlock.text().contains(d->filterText));
        } else {
            for (; lastBlock != document->end(); lastBlock = lastBlock.next())
                lastBlock.setVisible(d->filterText.isEmpty()
                                     || lastBlock.text().toLower().contains(d->filterText.toLower()));
        }
    }

    lastBlock = document->lastBlock();
    textEdit->setDocument(document);

    if (atBottom)
        scrollToBottom();
}

QString OutputWindow::doNewlineEnforcement(const QString &out)
{
    d->scrollToBottom = true;
    QString s = out;
    if (d->enforceNewline) {
        s.prepend(QLatin1Char('\n'));
        d->enforceNewline = false;
    }

    if (s.endsWith(QLatin1Char('\n'))) {
        d->enforceNewline = true; // make appendOutputInline put in a newline next time
        s.chop(1);
    }

    return s;
}

void OutputWindow::setMaxCharCount(int count)
{
    d->maxCharCount = count;
    setMaximumBlockCount(count / 100);
}

int OutputWindow::maxCharCount() const
{
    return d->maxCharCount;
}

bool OutputWindow::isReadOnly() const
{
    if (d->formatter)
        return d->formatter->plainTextEdit()->isReadOnly();
    return false;
}

void OutputWindow::setReadOnly(bool readOnly)
{
    if (d->formatter)
        d->formatter->plainTextEdit()->setReadOnly(readOnly);
}

void OutputWindow::appendMessage(const QString &output, OutputFormat format)
{
    QString out = SynchronousProcess::normalizeNewlines(output);

    if (out.size() > d->maxCharCount) {
        // Current line alone exceeds limit, we need to cut it.
        out.truncate(d->maxCharCount);
        out.append("[...]");
        setMaximumBlockCount(1);
    } else {
        int plannedChars = document()->characterCount() + out.size();
        if (plannedChars > d->maxCharCount) {
            int plannedBlockCount = document()->blockCount();
            QTextBlock tb = document()->firstBlock();
            while (tb.isValid() && plannedChars > d->maxCharCount && plannedBlockCount > 1) {
                plannedChars -= tb.length();
                plannedBlockCount -= 1;
                tb = tb.next();
            }
            setMaximumBlockCount(plannedBlockCount);
        } else {
            setMaximumBlockCount(-1);
        }
    }

    const bool atBottom = isScrollbarAtBottom() || m_scrollTimer.isActive();

    if (format == ErrorMessageFormat || format == NormalMessageFormat) {
        if (d->formatter)
            d->formatter->appendMessage(doNewlineEnforcement(out), format);
    } else {

        bool sameLine = format == StdOutFormatSameLine
                     || format == StdErrFormatSameLine;

        if (sameLine) {
            d->scrollToBottom = true;

            int newline = -1;
            bool enforceNewline = d->enforceNewline;
            d->enforceNewline = false;

            if (enforceNewline) {
                out.prepend('\n');
            } else {
                newline = out.indexOf(QLatin1Char('\n'));
                moveCursor(QTextCursor::End);
                if (newline != -1 && d->formatter)
                    d->formatter->appendMessage(out.left(newline), format);// doesn't enforce new paragraph like appendPlainText
            }

            QString s = out.mid(newline+1);
            if (s.isEmpty()) {
                d->enforceNewline = true;
            } else {
                if (s.endsWith(QLatin1Char('\n'))) {
                    d->enforceNewline = true;
                    s.chop(1);
                }
                if (d->formatter)
                    d->formatter->appendMessage(s, format);
            }
        } else {
            if (d->formatter)
                d->formatter->appendMessage(doNewlineEnforcement(out), format);
        }
    }

    if (atBottom) {
        if (m_lastMessage.elapsed() < 5) {
            m_scrollTimer.start();
        } else {
            m_scrollTimer.stop();
            scrollToBottom();
        }
    }

    m_lastMessage.start();
    enableUndoRedo();
}

// TODO rename
void OutputWindow::appendText(const QString &textIn, const QTextCharFormat &format)
{
    const QString text = SynchronousProcess::normalizeNewlines(textIn);
    if (d->maxCharCount > 0 && document()->characterCount() >= d->maxCharCount)
        return;
    const bool atBottom = isScrollbarAtBottom();
    if (!d->cursor.atEnd())
        d->cursor.movePosition(QTextCursor::End);
    d->cursor.beginEditBlock();
    d->cursor.insertText(doNewlineEnforcement(text), format);

    if (d->maxCharCount > 0 && document()->characterCount() >= d->maxCharCount) {
        QTextCharFormat tmp;
        tmp.setFontWeight(QFont::Bold);
        d->cursor.insertText(doNewlineEnforcement(tr("Additional output omitted. You can increase "
                                                     "the limit in the \"Build & Run\" settings.")
                                                  + QLatin1Char('\n')), tmp);
    }

    d->cursor.endEditBlock();
    if (atBottom)
        scrollToBottom();
}

bool OutputWindow::isScrollbarAtBottom() const
{
    return verticalScrollBar()->value() == verticalScrollBar()->maximum();
}

void OutputWindow::clear()
{
    d->enforceNewline = false;
    QPlainTextEdit::clear();
    if (d->formatter)
        d->formatter->clear();
}

void OutputWindow::scrollToBottom()
{
    verticalScrollBar()->setValue(verticalScrollBar()->maximum());
    // QPlainTextEdit destroys the first calls value in case of multiline
    // text, so make sure that the scroll bar actually gets the value set.
    // Is a noop if the first call succeeded.
    verticalScrollBar()->setValue(verticalScrollBar()->maximum());
}

void OutputWindow::grayOutOldContent()
{
    if (!d->cursor.atEnd())
        d->cursor.movePosition(QTextCursor::End);
    QTextCharFormat endFormat = d->cursor.charFormat();

    d->cursor.select(QTextCursor::Document);

    QTextCharFormat format;
    const QColor bkgColor = palette().base().color();
    const QColor fgdColor = palette().text().color();
    double bkgFactor = 0.50;
    double fgdFactor = 1.-bkgFactor;
    format.setForeground(QColor((bkgFactor * bkgColor.red() + fgdFactor * fgdColor.red()),
                             (bkgFactor * bkgColor.green() + fgdFactor * fgdColor.green()),
                             (bkgFactor * bkgColor.blue() + fgdFactor * fgdColor.blue()) ));
    d->cursor.mergeCharFormat(format);

    d->cursor.movePosition(QTextCursor::End);
    d->cursor.setCharFormat(endFormat);
    d->cursor.insertBlock(QTextBlockFormat());
}

void OutputWindow::enableUndoRedo()
{
    setMaximumBlockCount(0);
    setUndoRedoEnabled(true);
}

void OutputWindow::setWordWrapEnabled(bool wrap)
{
    if (wrap)
        setWordWrapMode(QTextOption::WrapAtWordBoundaryOrAnywhere);
    else
        setWordWrapMode(QTextOption::NoWrap);
}

} // namespace Core
