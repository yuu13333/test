/****************************************************************************
**
** Copyright (C) 2017 Klarälvdalens Datakonsult AB, a KDAB Group company, info@kdab.com, author Rafael Roquetto <rafael.roquetto@kdab.com>
** Contact: https://www.qt.io/licensing/
**
** This file is part of the tools applications of the Qt Toolkit.
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

#include "provider.h"

#include <qfile.h>
#include <qfileinfo.h>
#include <qtextstream.h>
#include <qregexp.h>

#ifdef TRACEGEN_DEBUG
#include <qdebug.h>

static void dumpTracepoint(const Tracepoint &t)
{
    qDebug() << "=== BEGIN TRACEPOINT ===";
    qDebug() << "name:" << t.name;
    qDebug() << "ARGS\n";

    int j = 0;

    for (auto i = t.args.constBegin(); i != t.args.constEnd(); ++i) {
        qDebug() << "ARG[" << j << "] type:" << i->type;
        qDebug() << "ARG[" << j << "] name:" << i->name;
        qDebug() << "ARG[" << j << "] arrayLen:" << i->arrayLen;
        ++j;
    }

    qDebug() << "\nFIELDS\n";

    j = 0;

    for (auto i = t.fields.constBegin(); i != t.fields.constEnd(); ++i) {
        qDebug() << "FIELD[" << j << "] backend_type" << static_cast<int>(i->backendType);
        qDebug() << "FIELD[" << j << "] param_type" << i->paramType;
        qDebug() << "FIELD[" << j << "] name" << i->name;
        qDebug() << "FIELD[" << j << "] arrayLen" << i->arrayLen;
        qDebug() << "FIELD[" << j << "] seqLen" << i->seqLen;
        ++j;
    }

    qDebug() << "=== END TRACEPOINT ===\n";

}
#endif

static inline int arrayLength(const QString &rawType)
{
    QRegExp rx(QStringLiteral(".*\\[([0-9]+)\\].*"));

    if (!rx.exactMatch(rawType.trimmed()))
        return 0;

    return rx.cap(1).toInt();
}

static inline QString sequenceLength(const QString &rawType)
{
    QRegExp rx(QStringLiteral(".*\\[([A-Za-z_][A-Za-z_0-9]*)\\].*"));

    if (!rx.exactMatch(rawType.trimmed()))
        return QString();

    return rx.cap(1);
}

static QString decayToPointer(QString type)
{
    QRegExp rx(QStringLiteral("\\[(.+)\\]"));
    return type.replace(rx, QStringLiteral("*"));
}

static QString removeBraces(QString type)
{
    QRegExp rx(QStringLiteral("\\[.*\\]"));

    return type.remove(rx);
}

static Tracepoint::Field::BackendType backendType(QString rawType)
{
    static const QHash<QString, Tracepoint::Field::BackendType> typeHash = {
        { QStringLiteral("bool"),                   Tracepoint::Field::Integer },
        { QStringLiteral("short_int"),              Tracepoint::Field::Integer },
        { QStringLiteral("signed_short"),           Tracepoint::Field::Integer },
        { QStringLiteral("signed_short_int"),       Tracepoint::Field::Integer },
        { QStringLiteral("unsigned_short"),         Tracepoint::Field::Integer },
        { QStringLiteral("unsigned_short_int"),     Tracepoint::Field::Integer },
        { QStringLiteral("int"),                    Tracepoint::Field::Integer },
        { QStringLiteral("signed"),                 Tracepoint::Field::Integer },
        { QStringLiteral("signed_int"),             Tracepoint::Field::Integer },
        { QStringLiteral("unsigned"),               Tracepoint::Field::Integer },
        { QStringLiteral("unsigned_int"),           Tracepoint::Field::Integer },
        { QStringLiteral("long"),                   Tracepoint::Field::Integer },
        { QStringLiteral("long_int"),               Tracepoint::Field::Integer },
        { QStringLiteral("signed_long"),            Tracepoint::Field::Integer },
        { QStringLiteral("signed_long_int"),        Tracepoint::Field::Integer },
        { QStringLiteral("unsigned_long"),          Tracepoint::Field::Integer },
        { QStringLiteral("unsigned_long_int"),      Tracepoint::Field::Integer },
        { QStringLiteral("long_long"),              Tracepoint::Field::Integer },
        { QStringLiteral("long_long_int"),          Tracepoint::Field::Integer },
        { QStringLiteral("signed_long_long"),       Tracepoint::Field::Integer },
        { QStringLiteral("signed_long_long_int"),   Tracepoint::Field::Integer },
        { QStringLiteral("unsigned_long_long"),     Tracepoint::Field::Integer },
        { QStringLiteral("char"),                   Tracepoint::Field::Integer },
        { QStringLiteral("float"),                  Tracepoint::Field::Float },
        { QStringLiteral("double"),                 Tracepoint::Field::Float },
        { QStringLiteral("long_double"),            Tracepoint::Field::Float },
        { QStringLiteral("char_ptr"),               Tracepoint::Field::String },
        { QStringLiteral("QString"),                Tracepoint::Field::QtString },
        { QStringLiteral("QByteArray"),             Tracepoint::Field::QtByteArray },
        { QStringLiteral("QUrl"),                   Tracepoint::Field::QtUrl },
        { QStringLiteral("QRect"),                  Tracepoint::Field::QtRect }
    };

    if (arrayLength(rawType) > 0)
        return Tracepoint::Field::Array;

    if (!sequenceLength(rawType).isNull())
        return Tracepoint::Field::Sequence;

    rawType.remove(QRegExp(QStringLiteral("\\s*const\\s*")));
    rawType.remove(QStringLiteral("&"));
    rawType.replace(QRegExp(QStringLiteral("\\s*\\*\\s*")), QStringLiteral("_ptr"));
    rawType = rawType.trimmed();
    rawType.replace(QStringLiteral(" "), QStringLiteral("_"));

    return typeHash.value(rawType.trimmed(), Tracepoint::Field::Unknown);
}

static Tracepoint parseTracepoint(const QString &name, const QStringList &args)
{
    Tracepoint t;
    t.name = name;

    if (args.isEmpty())
        return t;

    auto i = args.constBegin();
    auto end = args.constEnd();
    int argc = 0;

    QRegExp rx(QStringLiteral("(.*)\\b([A-Za-z_][A-Za-z0-9_]*)$"));

    while (i != end) {
        rx.exactMatch(*i);

        const QString type = rx.cap(1).trimmed();

        if (type.isNull())
            qFatal("Missing parameter type for argument %d of %s", argc, qPrintable(name));

        const QString name = rx.cap(2).trimmed();

        if (name.isNull())
            qFatal("Missing parameter name for argument %d of %s", argc, qPrintable(name));

        int arrayLen = arrayLength(type);

        Tracepoint::Argument a;
        a.arrayLen = arrayLen;
        a.name = name;
        a.type = decayToPointer(type);

        t.args << std::move(a);

        Tracepoint::Field f;
        f.backendType = backendType(type);
        f.paramType = removeBraces(type);
        f.name = name;
        f.arrayLen = arrayLen;
        f.seqLen = sequenceLength(type);

        t.fields << std::move(f);

        ++i;
    }

    return t;
}

Provider parseProvider(const QString &filename)
{
    QFile f(filename);

    if (!f.open(QIODevice::ReadOnly | QIODevice::Text))
        qFatal("Cannot open %s: %s", qPrintable(filename), qPrintable(f.errorString()));

    QTextStream s(&f);

    QRegExp tracedef(QStringLiteral("([A-Za-z][A-Za-z0-9_]*)\\((.*)\\)"));

    int lineNumber = 0;

    Provider provider;
    provider.name = QFileInfo(filename).baseName();

    for (;;) {
        QString line = s.readLine().trimmed();

        if (line.isNull())
            break;

        if (line.isEmpty() || line.startsWith(QStringLiteral("#"))) {
            ++lineNumber;
            continue;
        }

        if (tracedef.exactMatch(line)) {
            const QString name = tracedef.cap(1);
            QStringList args = tracedef.cap(2).split(QStringLiteral(","));

            if (args.at(0).isNull())
                args.clear();

            provider.tracepoints << parseTracepoint(name, args);
        } else {
            qFatal("Syntax error whilre processing %s on line %d", qPrintable(filename), lineNumber);
        }

        ++lineNumber;
    }

#ifdef TRACEGEN_DEBUG
    for (auto i = provider.tracepoints.constBegin(); i != provider.tracepoints.constEnd(); ++i)
        dumpTracepoint(*i);
#endif

    return provider;
}
