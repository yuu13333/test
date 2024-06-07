/****************************************************************************
**
** Copyright (C) 2013 by Southwest Research Institute (R)
**
** This file is part of the QtCore module of the Qt Toolkit.
**
** $QT_BEGIN_LICENSE:LGPL$
** Commercial License Usage
** Licensees holding valid commercial Qt licenses may use this file in
** accordance with the commercial license agreement provided with the
** Software or, alternatively, in accordance with the terms contained in
** a written agreement between you and Digia.  For licensing terms and
** conditions see http://qt.digia.com/licensing.  For further information
** use the contact form at http://qt.digia.com/contact-us.
**
** GNU Lesser General Public License Usage
** Alternatively, this file may be used under the terms of the GNU Lesser
** General Public License version 2.1 as published by the Free Software
** Foundation and appearing in the file LICENSE.LGPL included in the
** packaging of this file.  Please review the following information to
** ensure the GNU Lesser General Public License version 2.1 requirements
** will be met: http://www.gnu.org/licenses/old-licenses/lgpl-2.1.html.
**
** In addition, as a special exception, Digia gives you certain additional
** rights.  These rights are described in the Digia Qt LGPL Exception
** version 1.1, included in the file LGPL_EXCEPTION.txt in this package.
**
** GNU General Public License Usage
** Alternatively, this file may be used under the terms of the GNU
** General Public License version 3.0 as published by the Free Software
** Foundation and appearing in the file LICENSE.GPL included in the
** packaging of this file.  Please review the following information to
** ensure the GNU General Public License version 3.0 requirements will be
** met: http://www.gnu.org/copyleft/gpl.html.
**
**
** $QT_END_LICENSE$
**
****************************************************************************/

#ifndef QFLOAT16_H
#define QFLOAT16_H

#include <QtCore/qglobal.h>

QT_BEGIN_NAMESPACE

class qfloat16
{
public:
#ifndef Q_QDOC
    Q_DECL_CONSTEXPR inline qfloat16() : b16(0) { }
    inline qfloat16(float f);
    inline operator float() const;
#endif

private:
    quint16 b16;

    Q_CORE_EXPORT static const quint32 mantissatable[];
    Q_CORE_EXPORT static const quint32 exponenttable[];
    Q_CORE_EXPORT static const quint32 offsettable[];
    Q_CORE_EXPORT static const quint32 basetable[];
    Q_CORE_EXPORT static const quint32 shifttable[];

    inline bool isPositive() const
    { return (b16 & quint16(0x8000)) == 0; }

    friend bool qIsNull(qfloat16 f);
    friend qfloat16 operator-(qfloat16 a);
    friend bool operator>(qfloat16 a, qfloat16 b);
    friend bool operator<(qfloat16 a, qfloat16 b);
    friend bool operator>=(qfloat16 a, qfloat16 b);
    friend bool operator<=(qfloat16 a, qfloat16 b);
    friend bool operator==(qfloat16 a, qfloat16 b);
};

Q_CORE_EXPORT bool qIsInf(qfloat16 f);
Q_CORE_EXPORT bool qIsNaN(qfloat16 f);
Q_CORE_EXPORT bool qIsFinite(qfloat16 f);

inline int qRound(qfloat16 d)
{ return qRound(float(d)); }

inline qint64 qRound64(qfloat16 d)
{ return qRound64(float(d)); }

static inline bool qFuzzyCompare(qfloat16 p1, qfloat16 p2)
{
    float f1 = float(p1);
    float f2 = float(p2);
    // The significand precision for IEEE754 half precision is
    // 11 bits (10 explicitly stored), or approximately 3 decimal
    // digits.  In selecting the fuzzy comparison factor of 102.5f
    // (that is, (2^10+1)/10) below, we effectively select a
    // window of about 1 (least significant) decimal digit about
    // which the two operands can vary and still return true.
    return (qAbs(f1-f2) * 102.5f <= qMin(qAbs(f1), qAbs(f2)));
}

inline bool qIsNull(qfloat16 f)
{
    return (f.b16 & quint16(0x7fff)) == 0;
}

inline int qIntCast(qfloat16 f) { return int(float(f)); }

inline qfloat16::qfloat16(float f)
{
    quint32 u;
    memcpy(&u,&f,sizeof(quint32));
    b16 = basetable[(u>>23) & 0x1ff]
        + ((u & 0x007fffff) >> shifttable[(u>>23) & 0x1ff]);
}

inline qfloat16::operator float() const
{
    quint32 u = mantissatable[offsettable[b16>>10] + (b16&0x3ff)]
              + exponenttable[b16>>10];
    float f;
    memcpy(&f,&u,sizeof(quint32));
    return f;
}

inline qfloat16 operator-(qfloat16 a)
{
    qfloat16 f;
    f.b16 = a.b16 ^ quint16(0x8000);
    return f;
}

inline double operator+(qfloat16 a, int b) { return double(float(a)) + b; }
inline double operator-(qfloat16 a, int b) { return double(float(a)) - b; }
inline double operator*(qfloat16 a, int b) { return double(float(a)) * b; }
inline double operator/(qfloat16 a, int b) { return double(float(a)) / b; }
inline double operator+(int a, qfloat16 b) { return a + double(float(b)); }
inline double operator-(int a, qfloat16 b) { return a - double(float(b)); }
inline double operator*(int a, qfloat16 b) { return a * double(float(b)); }
inline double operator/(int a, qfloat16 b) { return a / double(float(b)); }

inline long double operator+(qfloat16 a, long double b) { return (long double)(float(a)) + b; }
inline long double operator-(qfloat16 a, long double b) { return (long double)(float(a)) - b; }
inline long double operator*(qfloat16 a, long double b) { return (long double)(float(a)) * b; }
inline long double operator/(qfloat16 a, long double b) { return (long double)(float(a)) / b; }
inline long double operator+(long double a, qfloat16 b) { return a + (long double)(float(b)); }
inline long double operator-(long double a, qfloat16 b) { return a - (long double)(float(b)); }
inline long double operator*(long double a, qfloat16 b) { return a * (long double)(float(b)); }
inline long double operator/(long double a, qfloat16 b) { return a / (long double)(float(b)); }

inline double operator+(qfloat16 a, double b) { return double(float(a)) + b; }
inline double operator-(qfloat16 a, double b) { return double(float(a)) - b; }
inline double operator*(qfloat16 a, double b) { return double(float(a)) * b; }
inline double operator/(qfloat16 a, double b) { return double(float(a)) / b; }
inline double operator+(double a, qfloat16 b) { return a + double(float(b)); }
inline double operator-(double a, qfloat16 b) { return a - double(float(b)); }
inline double operator*(double a, qfloat16 b) { return a * double(float(b)); }
inline double operator/(double a, qfloat16 b) { return a / double(float(b)); }

inline float operator+(qfloat16 a, float b) { return float(a) + b; }
inline float operator-(qfloat16 a, float b) { return float(a) - b; }
inline float operator*(qfloat16 a, float b) { return float(a) * b; }
inline float operator/(qfloat16 a, float b) { return float(a) / b; }
inline float operator+(float a, qfloat16 b) { return a + float(b); }
inline float operator-(float a, qfloat16 b) { return a - float(b); }
inline float operator*(float a, qfloat16 b) { return a * float(b); }
inline float operator/(float a, qfloat16 b) { return a / float(b); }

inline qfloat16 operator+(qfloat16 a, qfloat16 b) { return qfloat16(float(a) + float(b)); }
inline qfloat16 operator-(qfloat16 a, qfloat16 b) { return qfloat16(float(a) - float(b)); }
inline qfloat16 operator*(qfloat16 a, qfloat16 b) { return qfloat16(float(a) * float(b)); }
inline qfloat16 operator/(qfloat16 a, qfloat16 b) { return qfloat16(float(a) / float(b)); }

inline qfloat16& operator+=(qfloat16& a, int b) { a = qfloat16(float(a + b)); return a; }
inline qfloat16& operator-=(qfloat16& a, int b) { a = qfloat16(float(a - b)); return a; }
inline qfloat16& operator*=(qfloat16& a, int b) { a = qfloat16(float(a * b)); return a; }
inline qfloat16& operator/=(qfloat16& a, int b) { a = qfloat16(float(a / b)); return a; }

inline qfloat16& operator+=(qfloat16& a, long double b) { a = qfloat16(float(a + b)); return a; }
inline qfloat16& operator-=(qfloat16& a, long double b) { a = qfloat16(float(a - b)); return a; }
inline qfloat16& operator*=(qfloat16& a, long double b) { a = qfloat16(float(a * b)); return a; }
inline qfloat16& operator/=(qfloat16& a, long double b) { a = qfloat16(float(a / b)); return a; }

inline qfloat16& operator+=(qfloat16& a, double b) { a = qfloat16(float(a + b)); return a; }
inline qfloat16& operator-=(qfloat16& a, double b) { a = qfloat16(float(a - b)); return a; }
inline qfloat16& operator*=(qfloat16& a, double b) { a = qfloat16(float(a * b)); return a; }
inline qfloat16& operator/=(qfloat16& a, double b) { a = qfloat16(float(a / b)); return a; }

inline qfloat16& operator+=(qfloat16& a, float b) { a = qfloat16(a + b); return a; }
inline qfloat16& operator-=(qfloat16& a, float b) { a = qfloat16(a - b); return a; }
inline qfloat16& operator*=(qfloat16& a, float b) { a = qfloat16(a * b); return a; }
inline qfloat16& operator/=(qfloat16& a, float b) { a = qfloat16(a / b); return a; }

inline qfloat16& operator+=(qfloat16& a, qfloat16 b) { a = a + b; return a; }
inline qfloat16& operator-=(qfloat16& a, qfloat16 b) { a = a - b; return a; }
inline qfloat16& operator*=(qfloat16& a, qfloat16 b) { a = a * b; return a; }
inline qfloat16& operator/=(qfloat16& a, qfloat16 b) { a = a / b; return a; }

inline bool operator>(qfloat16 a, int b)  { return float(a) > b; }
inline bool operator<(qfloat16 a, int b)  { return float(a) < b; }
inline bool operator>=(qfloat16 a, int b) { return float(a) >= b; }
inline bool operator<=(qfloat16 a, int b) { return float(a) <= b; }
inline bool operator==(qfloat16 a, int b) { return float(a) == b; }
inline bool operator>(int a, qfloat16 b)  { return a > float(b); }
inline bool operator<(int a, qfloat16 b)  { return a < float(b); }
inline bool operator>=(int a, qfloat16 b) { return a >= float(b); }
inline bool operator<=(int a, qfloat16 b) { return a <= float(b); }
inline bool operator==(int a, qfloat16 b) { return a == float(b); }

inline bool operator>(qfloat16 a, long double b)  { return float(a) > b; }
inline bool operator<(qfloat16 a, long double b)  { return float(a) < b; }
inline bool operator>=(qfloat16 a, long double b) { return float(a) >= b; }
inline bool operator<=(qfloat16 a, long double b) { return float(a) <= b; }
inline bool operator==(qfloat16 a, long double b) { return float(a) == b; }
inline bool operator>(long double a, qfloat16 b)  { return a > float(b); }
inline bool operator<(long double a, qfloat16 b)  { return a < float(b); }
inline bool operator>=(long double a, qfloat16 b) { return a >= float(b); }
inline bool operator<=(long double a, qfloat16 b) { return a <= float(b); }
inline bool operator==(long double a, qfloat16 b) { return a == float(b); }

inline bool operator>(qfloat16 a, double b)  { return float(a) > b; }
inline bool operator<(qfloat16 a, double b)  { return float(a) < b; }
inline bool operator>=(qfloat16 a, double b) { return float(a) >= b; }
inline bool operator<=(qfloat16 a, double b) { return float(a) <= b; }
inline bool operator==(qfloat16 a, double b) { return float(a) == b; }
inline bool operator>(double a, qfloat16 b)  { return a > float(b); }
inline bool operator<(double a, qfloat16 b)  { return a < float(b); }
inline bool operator>=(double a, qfloat16 b) { return a >= float(b); }
inline bool operator<=(double a, qfloat16 b) { return a <= float(b); }
inline bool operator==(double a, qfloat16 b) { return a == float(b); }

inline bool operator>(qfloat16 a, float b)  { return float(a) > b; }
inline bool operator<(qfloat16 a, float b)  { return float(a) < b; }
inline bool operator>=(qfloat16 a, float b) { return float(a) >= b; }
inline bool operator<=(qfloat16 a, float b) { return float(a) <= b; }
inline bool operator==(qfloat16 a, float b) { return float(a) == b; }
inline bool operator>(float a, qfloat16 b)  { return a > float(b); }
inline bool operator<(float a, qfloat16 b)  { return a < float(b); }
inline bool operator>=(float a, qfloat16 b) { return a >= float(b); }
inline bool operator<=(float a, qfloat16 b) { return a <= float(b); }
inline bool operator==(float a, qfloat16 b) { return a == float(b); }

inline bool operator>(qfloat16 a, qfloat16 b)
{
    if ((qIsFinite(a) == false) || (qIsFinite(b) == false ))
        return float(a) > float(b);

    if (qIsNull(a) && qIsNull(b))
        return false;

    if (a.isPositive() || b.isPositive())
        return qint16(a.b16) > qint16(b.b16);

    return a.b16 < b.b16;
}

inline bool operator<(qfloat16 a, qfloat16 b)
{
    if ((qIsFinite(a) == false) || (qIsFinite(b) == false ))
        return float(a) < float(b);

    if (qIsNull(a) && qIsNull(b))
        return false;

    if (a.isPositive() || a.isPositive())
        return qint16(a.b16) < qint16(b.b16);

    return a.b16 > b.b16;
}

inline bool operator>=(qfloat16 a, qfloat16 b)
{
    if ((qIsFinite(a) == false) || (qIsFinite(b) == false ))
        return float(a) >= float(b);

    if (qIsNull(a) && qIsNull(b))
        return true;

    if (a.isPositive() || b.isPositive())
        return qint16(a.b16) >= qint16(b.b16);

    return a.b16 <= b.b16;
}

Q_DECL_CONSTEXPR inline bool operator<=(qfloat16 a, qfloat16 b)
{
    if ((qIsFinite(a) == false) || (qIsFinite(b) == false ))
        return float(a) <= float(b);

    if (qIsNull(a) && qIsNull(b))
        return true;

    if (a.isPositive() || b.isPositive())
        return qint16(a.b16) <= qint16(b.b16);

    return a.b16 >= b.b16;
}

Q_DECL_CONSTEXPR inline bool operator==(qfloat16 a, qfloat16 b)
{
    if ((qIsFinite(a) == false) || (qIsFinite(b) == false ))
        return float(a) == float(b);

    if (qIsNull(a) && qIsNull(b))
        return true;

    return a.b16 == b.b16;
}

Q_DECL_CONSTEXPR inline bool qFuzzyIsNull(qfloat16 f)
{
    return qAbs(f) <= 0.001f;
}

QT_END_NAMESPACE

#endif
