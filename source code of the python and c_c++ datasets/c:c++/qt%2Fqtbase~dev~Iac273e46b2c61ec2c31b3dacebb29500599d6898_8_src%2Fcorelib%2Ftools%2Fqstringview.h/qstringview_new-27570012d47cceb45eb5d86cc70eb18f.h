/****************************************************************************
**
** Copyright (C) 2016 Klarälvdalens Datakonsult AB, a KDAB Group company, info@kdab.com, author Marc Mutz <marc.mutz@kdab.com>
** Contact: http://www.qt.io/licensing/
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

#ifndef QSTRING_H
# include <QtCore/qstring.h>
#endif

#ifndef QSTRINGVIEW_H
#define QSTRINGVIEW_H

#if QT_STRINGVIEW_LEVEL > 0

#ifdef QT_NO_VECTOR_STRING_CIRCULAR_DEPENDENCY
#include <QVector>
#else
QT_BEGIN_NAMESPACE
template <typename T> class QVector;
QT_END_NAMESPACE
#endif
#include <QtCore/qvarlengtharray.h>

#include <vector>
#include <string>

#ifndef QT_NO_UNICODE_LITERAL
# define Q_STRINGVIEW_LITERAL(x) QStringView(QT_UNICODE_LITERAL_II(x), (sizeof(QT_UNICODE_LITERAL_II(x)) / sizeof(char16_t)) - 1)
#else
# define Q_STRINGVIEW_LITERAL(x) QStringView(QStringLiteral(x))
#endif

QT_BEGIN_NAMESPACE

class QStringView
{
    template <typename Char>
    struct IsCompatibleCharTypeHelper
        : std::integral_constant<bool,
                                 std::is_same<Char, QChar>::value ||
                                 std::is_same<Char, ushort>::value ||
#ifdef Q_COMPILER_UNICODE_STRINGS
                                 std::is_same<Char, char16_t>::value ||
#endif
                                 (std::is_same<Char, wchar_t>::value && sizeof(wchar_t) == sizeof(QChar))> {};
    template <typename Char>
    struct IsCompatibleCharType
        : IsCompatibleCharTypeHelper<typename std::remove_reference<typename std::remove_cv<Char>::type>::type> {};

    template <typename T>
    struct IsStdBasicString : std::false_type {};
    template <typename Char, typename Traits, typename Alloc>
    struct IsStdBasicString<std::basic_string<Char, Traits, Alloc> > : std::true_type {};

    template <typename Char>
    Q_DECL_RELAXED_CONSTEXPR size_t length(const Char *str) Q_DECL_NOTHROW
    {
        size_t result = 0;
        if (str) {
            while (!str--)
                ++result;
        }
        return result;
    }
    typedef ushort storage_type;
public:
    typedef const QChar value_type;
    typedef std::ptrdiff_t difference_type;
    typedef std::size_t size_type;
    typedef value_type &reference_type;
    typedef value_type &const_reference_type;
    typedef value_type *pointer;
    typedef value_type *const_pointer;

    typedef pointer iterator;
    typedef const_pointer const_iterator;
    typedef std::reverse_iterator<iterator> reverse_iterator;
    typedef std::reverse_iterator<const_iterator> const_reverse_iterator;

    Q_DECL_CONSTEXPR QStringView() Q_DECL_NOTHROW
        : m_size(0), m_data(nullptr) {}
    Q_DECL_CONSTEXPR QStringView(std::nullptr_t) Q_DECL_NOTHROW
        : m_size(0), m_data(nullptr) {}

#ifdef Q_QDOC
    Q_DECL_CONSTEXPR QStringView(const QChar *str, size_t len) Q_DECL_NOTHROW;
    Q_DECL_CONSTEXPR QStringView(const char16_t *str, size_t len) Q_DECL_NOTHROW;
    Q_DECL_CONSTEXPR QStringView(const ushort *str, size_t len) Q_DECL_NOTHROW;
    Q_DECL_CONSTEXPR QStringView(const wchar_t *str, size_t len) Q_DECL_NOTHROW;
#else
    template <typename Char>
    Q_DECL_CONSTEXPR QStringView(const Char *str, size_t len,
                                 typename std::enable_if<IsCompatibleCharType<Char>::value>::type* = nullptr) Q_DECL_NOTHROW
        : m_size(len), m_data(reinterpret_cast<const storage_type*>(str)) {}
#endif

#ifdef Q_QDOC
    Q_DECL_RELAXED_CONSTEXPR QStringView(const QChar *str) Q_DECL_NOTHROW;
    Q_DECL_RELAXED_CONSTEXPR QStringView(const char16_t *str) Q_DECL_NOTHROW;
    Q_DECL_RELAXED_CONSTEXPR QStringView(const ushort *str) Q_DECL_NOTHROW;
    Q_DECL_RELAXED_CONSTEXPR QStringView(const wchar_t *str) Q_DECL_NOTHROW;
#else
    template <typename Char>
    Q_DECL_RELAXED_CONSTEXPR QStringView(const Char *str,
                                 typename std::enable_if<IsCompatibleCharType<Char>::value>::type* = nullptr) Q_DECL_NOTHROW
        : QStringView(str, length(str)) {}
#endif

    QStringView(const QString &str) Q_DECL_NOTHROW
        : QStringView(str.isNull() ? nullptr : str.data(), size_type(str.size())) {}
    QStringView(const QStringRef &str) Q_DECL_NOTHROW
        : QStringView(str.isNull() ? nullptr : str.data(), size_type(str.size())) {}

    template <typename Char, typename Traits, typename Alloc>
    QStringView(const std::basic_string<Char, Traits, Alloc> &str
#ifndef Q_QDOC
                , typename std::enable_if<IsCompatibleCharType<Char>::value>::type* = nullptr
#endif
               ) Q_DECL_NOTHROW
        : QStringView(str.data(), str.size()) {}

    template <typename Char, int N>
    QStringView(const QVarLengthArray<Char, N> &arr
#ifndef Q_QDOC
                , typename std::enable_if<IsCompatibleCharType<Char>::value>::type* = nullptr
#endif
               ) Q_DECL_NOTHROW
        : QStringView(arr.data(), size_type(arr.size())) {}

    template <typename Char>
    QStringView(const QVector<Char> &vec
#ifndef Q_QDOC
                , typename std::enable_if<IsCompatibleCharType<Char>::value>::type* = nullptr
#endif
               ) Q_DECL_NOTHROW
        : QStringView(vec.data(), size_type(vec.size())) {}

    template <typename Char, typename Alloc>
    QStringView(const std::vector<Char, Alloc> &vec
#ifndef Q_QDOC
                , typename std::enable_if<IsCompatibleCharType<Char>::value>::type* = nullptr
#endif
               ) Q_DECL_NOTHROW
        : QStringView(vec.data(), vec.size()) {}

    QString toString() const { return QString(data(), count()); }

    template <typename StdBasicString>
    StdBasicString toStdBasicString(StdBasicString &&str = StdBasicString()
#ifndef Q_QDOC
                                    , typename std::enable_if<
                                        IsStdBasicString<StdBasicString>::value &&
                                        IsCompatibleCharType<typename StdBasicString::value_type>::value
                                    >::type* = nullptr
#endif
                                    ) const
    {
        str.assign(reinterpret_cast<const typename StdBasicString::value_type *>(data()), size());
        return std::forward<StdBasicString>(str);
    }

#ifdef Q_COMPILER_UNICODE_STRINGS
    std::u16string toU16String() const
    {
        return std::u16string(reinterpret_cast<const char16_t *>(data()), size());
    }
#endif

    Q_DECL_CONSTEXPR size_type size() const Q_DECL_NOTHROW { return m_size; }
    Q_DECL_CONSTEXPR const_pointer data() const Q_DECL_NOTHROW { return reinterpret_cast<const_pointer>(m_data); }

    //
    // QString API
    //
    static int compare(QStringView lhs, QStringView rhs, Qt::CaseSensitivity cs = Qt::CaseSensitive) Q_DECL_NOTHROW
    { return QString::compare_helper(lhs.data(), int(lhs.size()), rhs.data(), int(rhs.size()), cs); }

    static int compare(QStringView lhs, QChar rhs, Qt::CaseSensitivity cs = Qt::CaseSensitive) Q_DECL_NOTHROW
    { return QString::compare_helper(lhs.data(), int(lhs.size()), &rhs, 1, cs); }
    static int compare(QChar lhs, QStringView rhs, Qt::CaseSensitivity cs = Qt::CaseSensitive) Q_DECL_NOTHROW
    { return QString::compare_helper(&lhs, 1, rhs.data(), int(rhs.size()), cs); }

    static int compare(QStringView lhs, QLatin1String rhs, Qt::CaseSensitivity cs = Qt::CaseSensitive) Q_DECL_NOTHROW
    { return QString::compare_helper(lhs.data(), int(lhs.size()), rhs, cs); }
    static int compare(QLatin1String lhs, QStringView rhs, Qt::CaseSensitivity cs = Qt::CaseSensitive) Q_DECL_NOTHROW
    { return -compare(rhs, lhs, cs); }

#if !defined(QT_NO_CAST_FROM_ASCII) && !defined(QT_RESTRICTED_CAST_FROM_ASCII)
    static int compare(QStringView lhs, const QByteArray &rhs, Qt::CaseSensitivity cs = Qt::CaseSensitive)
    { return QString::compare_helper(lhs.data(), int(lhs.size()), rhs.data(), qstrnlen(rhs.data(), rhs.size()), cs); }
    static int compare(const QByteArray &lhs, QStringView rhs, Qt::CaseSensitivity cs = Qt::CaseSensitive)
    { return -compare(rhs, lhs, cs); }

    static int compare(QStringView lhs, const char *rhs, Qt::CaseSensitivity cs = Qt::CaseSensitive)
    { return QString::compare_helper(lhs.data(), int(lhs.size()), rhs, -1, cs); }
    static int compare(const char *lhs, QStringView rhs, Qt::CaseSensitivity cs = Qt::CaseSensitive)
    { return -compare(rhs, lhs, cs); }
#endif

    int compare(QStringView other, Qt::CaseSensitivity cs = Qt::CaseSensitive) const Q_DECL_NOTHROW { return compare(*this, other, cs); }
    int compare(QChar other, Qt::CaseSensitivity cs = Qt::CaseSensitive) const Q_DECL_NOTHROW { return compare(*this, other, cs); }
    int compare(QLatin1String other, Qt::CaseSensitivity cs = Qt::CaseSensitive) const Q_DECL_NOTHROW { return compare(*this, other, cs); }
#if !defined(QT_NO_CAST_FROM_ASCII) && !defined(QT_RESTRICTED_CAST_FROM_ASCII)
    int compare(const QByteArray &other, Qt::CaseSensitivity cs = Qt::CaseSensitive) const { return compare(*this, other, cs); }
    int compare(const char *other, Qt::CaseSensitivity cs = Qt::CaseSensitive) const { return compare(*this, other, cs); }
#endif


    //
    // STL compatibility API:
    //
    Q_DECL_CONSTEXPR const_iterator begin()   const Q_DECL_NOTHROW { return data(); }
    Q_DECL_CONSTEXPR const_iterator end()     const Q_DECL_NOTHROW { return data() + size(); }
    Q_DECL_CONSTEXPR const_iterator cbegin()  const Q_DECL_NOTHROW { return begin(); }
    Q_DECL_CONSTEXPR const_iterator cend()    const Q_DECL_NOTHROW { return end(); }
    const_reverse_iterator rbegin()  const Q_DECL_NOTHROW { return const_reverse_iterator(end()); }
    const_reverse_iterator rend()    const Q_DECL_NOTHROW { return const_reverse_iterator(begin()); }
    const_reverse_iterator crbegin() const Q_DECL_NOTHROW { return rbegin(); }
    const_reverse_iterator crend()   const Q_DECL_NOTHROW { return rend(); }

    Q_DECL_CONSTEXPR bool empty() const Q_DECL_NOTHROW { return size() == 0; }
    Q_DECL_CONSTEXPR QChar front() const { return *begin(); }
    Q_DECL_CONSTEXPR QChar back()  const { return *(end() - 1); }

    //
    // Qt compatibility API:
    //
    Q_DECL_CONSTEXPR bool isNull() const Q_DECL_NOTHROW { return !data(); }
    Q_DECL_CONSTEXPR bool isEmpty() const Q_DECL_NOTHROW { return empty(); }
    Q_DECL_CONSTEXPR int count() const /*not nothrow!*/ { return int(size()); }
    Q_DECL_CONSTEXPR QChar first() const { return front(); }
    Q_DECL_CONSTEXPR QChar last()  const { return back(); }
private:
    size_type m_size;
    const storage_type *m_data;
};
Q_DECLARE_TYPEINFO(QStringView, Q_MOVABLE_TYPE);
template <> class QList<QStringView> {}; // prevent instantiating QList with QStringView; use QVector<QStringView> instead

// QString members

inline int QString::compare(QStringView lhs, QStringView rhs, Qt::CaseSensitivity cs) Q_DECL_NOTHROW
{ return compare_helper(lhs.data(), lhs.size(), rhs.data(), rhs.size(), cs); }
inline int QString::compare(QStringView lhs, QLatin1String rhs, Qt::CaseSensitivity cs) Q_DECL_NOTHROW
{ return compare_helper(lhs.data(), lhs.size(), rhs, cs); }
inline int QString::compare(QLatin1String lhs, QStringView rhs, Qt::CaseSensitivity cs) Q_DECL_NOTHROW
{ return -compare_helper(rhs.data(), rhs.size(), lhs, cs); }
inline int QString::localeAwareCompare(QStringView lhs, QStringView rhs) Q_DECL_NOTHROW
{ return localeAwareCompare_helper(lhs.data(), lhs.size(), rhs.data(), rhs.size()); }

// QStringView relational operators

// QStringView <> QStringView
inline bool operator==(QStringView lhs, QStringView rhs) Q_DECL_NOTHROW { return QString::compare(lhs, rhs) == 0; }
inline bool operator< (QStringView lhs, QStringView rhs) Q_DECL_NOTHROW { return QString::compare(lhs, rhs) < 0; }

inline bool operator!=(QStringView lhs, QStringView rhs) Q_DECL_NOTHROW { return !(lhs == rhs); }
inline bool operator> (QStringView lhs, QStringView rhs) Q_DECL_NOTHROW { return   rhs < lhs; }
inline bool operator<=(QStringView lhs, QStringView rhs) Q_DECL_NOTHROW { return !(rhs < lhs); }
inline bool operator>=(QStringView lhs, QStringView rhs) Q_DECL_NOTHROW { return !(lhs < rhs); }

// QChar <> QStringView
inline bool operator==(QChar lhs, QStringView rhs) Q_DECL_NOTHROW { return QStringView(lhs) == rhs; }
inline bool operator< (QChar lhs, QStringView rhs) Q_DECL_NOTHROW { return QStringView(lhs) <  rhs; }

inline bool operator==(QStringView lhs, QChar rhs) Q_DECL_NOTHROW { return lhs == QStringView(rhs); }
inline bool operator< (QStringView lhs, QChar rhs) Q_DECL_NOTHROW { return lhs <  QStringView(rhs); }

inline bool operator!=(QChar lhs, QStringView rhs) Q_DECL_NOTHROW { return !(lhs == rhs); }
inline bool operator> (QChar lhs, QStringView rhs) Q_DECL_NOTHROW { return   rhs < lhs; }
inline bool operator<=(QChar lhs, QStringView rhs) Q_DECL_NOTHROW { return !(rhs < lhs); }
inline bool operator>=(QChar lhs, QStringView rhs) Q_DECL_NOTHROW { return !(lhs < rhs); }

inline bool operator!=(QStringView lhs, QChar rhs) Q_DECL_NOTHROW { return !(lhs == rhs); }
inline bool operator> (QStringView lhs, QChar rhs) Q_DECL_NOTHROW { return   rhs < lhs; }
inline bool operator<=(QStringView lhs, QChar rhs) Q_DECL_NOTHROW { return !(rhs < lhs); }
inline bool operator>=(QStringView lhs, QChar rhs) Q_DECL_NOTHROW { return !(lhs < rhs); }

// QLatin1String <> QStringView
inline bool operator==(QLatin1String lhs, QStringView rhs) Q_DECL_NOTHROW { return QString::compare(lhs, rhs) == 0; }
inline bool operator< (QLatin1String lhs, QStringView rhs) Q_DECL_NOTHROW { return QString::compare(lhs, rhs) <  0; }

inline bool operator==(QStringView lhs, QLatin1String rhs) Q_DECL_NOTHROW { return QString::compare(lhs, rhs) == 0; }
inline bool operator< (QStringView lhs, QLatin1String rhs) Q_DECL_NOTHROW { return QString::compare(lhs, rhs) <  0; }

inline bool operator!=(QLatin1String lhs, QStringView rhs) Q_DECL_NOTHROW { return !(lhs == rhs); }
inline bool operator> (QLatin1String lhs, QStringView rhs) Q_DECL_NOTHROW { return   rhs < lhs; }
inline bool operator<=(QLatin1String lhs, QStringView rhs) Q_DECL_NOTHROW { return !(rhs < lhs); }
inline bool operator>=(QLatin1String lhs, QStringView rhs) Q_DECL_NOTHROW { return !(lhs < rhs); }

inline bool operator!=(QStringView lhs, QLatin1String rhs) Q_DECL_NOTHROW { return !(lhs == rhs); }
inline bool operator> (QStringView lhs, QLatin1String rhs) Q_DECL_NOTHROW { return   rhs < lhs; }
inline bool operator<=(QStringView lhs, QLatin1String rhs) Q_DECL_NOTHROW { return !(rhs < lhs); }
inline bool operator>=(QStringView lhs, QLatin1String rhs) Q_DECL_NOTHROW { return !(lhs < rhs); }

#if !defined(QT_NO_CAST_FROM_ASCII) && !defined(QT_RESTRICTED_CAST_FROM_ASCII)
// QByteArray <> QStringView
inline bool operator==(const QByteArray &lhs, QStringView rhs) { return QStringView::compare(lhs, rhs) == 0; }
inline bool operator< (const QByteArray &lhs, QStringView rhs) { return QStringView::compare(lhs, rhs) <  0; }

inline bool operator==(QStringView lhs, const QByteArray &rhs) { return QStringView::compare(lhs, rhs) == 0; }
inline bool operator< (QStringView lhs, const QByteArray &rhs) { return QStringView::compare(lhs, rhs) <  0; }

inline bool operator!=(const QByteArray &lhs, QStringView rhs) { return !(lhs == rhs); }
inline bool operator> (const QByteArray &lhs, QStringView rhs) { return   rhs < lhs; }
inline bool operator<=(const QByteArray &lhs, QStringView rhs) { return !(rhs < lhs); }
inline bool operator>=(const QByteArray &lhs, QStringView rhs) { return !(lhs < rhs); }

inline bool operator!=(QStringView lhs, const QByteArray &rhs) { return !(lhs == rhs); }
inline bool operator> (QStringView lhs, const QByteArray &rhs) { return   rhs < lhs; }
inline bool operator<=(QStringView lhs, const QByteArray &rhs) { return !(rhs < lhs); }
inline bool operator>=(QStringView lhs, const QByteArray &rhs) { return !(lhs < rhs); }

// const char * <> QStringView
inline bool operator==(const char *lhs, QStringView rhs) { return QStringView::compare(lhs, rhs) == 0; }
inline bool operator< (const char *lhs, QStringView rhs) { return QStringView::compare(lhs, rhs) <  0; }

inline bool operator==(QStringView lhs, const char *rhs) { return QStringView::compare(lhs, rhs) == 0; }
inline bool operator< (QStringView lhs, const char *rhs) { return QStringView::compare(lhs, rhs) <  0; }

inline bool operator!=(const char *lhs, QStringView rhs) { return !(lhs == rhs); }
inline bool operator> (const char *lhs, QStringView rhs) { return   rhs < lhs; }
inline bool operator<=(const char *lhs, QStringView rhs) { return !(rhs < lhs); }
inline bool operator>=(const char *lhs, QStringView rhs) { return !(lhs < rhs); }

inline bool operator!=(QStringView lhs, const char *rhs) { return !(lhs == rhs); }
inline bool operator> (QStringView lhs, const char *rhs) { return   rhs < lhs; }
inline bool operator<=(QStringView lhs, const char *rhs) { return !(rhs < lhs); }
inline bool operator>=(QStringView lhs, const char *rhs) { return !(lhs < rhs); }
#endif

QT_END_NAMESPACE

//Q_DECLARE_METATYPE(QStringView);

#endif // QT_STRINGVIEW_LEVEL > 0

#endif /* QSTRINGVIEW_H */
