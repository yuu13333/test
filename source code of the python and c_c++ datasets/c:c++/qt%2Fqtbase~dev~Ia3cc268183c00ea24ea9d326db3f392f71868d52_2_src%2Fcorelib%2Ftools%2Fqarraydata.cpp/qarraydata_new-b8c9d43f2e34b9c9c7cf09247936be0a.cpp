/****************************************************************************
**
** Copyright (C) 2020 The Qt Company Ltd.
** Copyright (C) 2016 Intel Corporation.
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

#include <QtCore/qarraydata.h>
#include <QtCore/private/qnumeric_p.h>
#include <QtCore/private/qtools_p.h>
#include <QtCore/qmath.h>

#include <QtCore/qbytearray.h>  // QBA::value_type
#include <QtCore/qstring.h>  // QString::value_type

#include <stdlib.h>
#include <algorithm>  // constexpr std::max

QT_BEGIN_NAMESPACE

/*
 * This pair of functions is declared in qtools_p.h and is used by the Qt
 * containers to allocate memory and grow the memory block during append
 * operations.
 *
 * They take qsizetype parameters and return qsizetype so they will change sizes
 * according to the pointer width. However, knowing Qt containers store the
 * container size and element indexes in ints, these functions never return a
 * size larger than INT_MAX. This is done by casting the element count and
 * memory block size to int in several comparisons: the check for negative is
 * very fast on most platforms as the code only needs to check the sign bit.
 *
 * These functions return SIZE_MAX on overflow, which can be passed to malloc()
 * and will surely cause a NULL return (there's no way you can allocate a
 * memory block the size of your entire VM space).
 */

/*!
    \internal
    \since 5.7

    Returns the memory block size for a container containing \a elementCount
    elements, each of \a elementSize bytes, plus a header of \a headerSize
    bytes. That is, this function returns \c
      {elementCount * elementSize + headerSize}

    but unlike the simple calculation, it checks for overflows during the
    multiplication and the addition.

    Both \a elementCount and \a headerSize can be zero, but \a elementSize
    cannot.

    This function returns -1 on overflow or if the memory block size
    would not fit a qsizetype.
*/
qsizetype qCalculateBlockSize(qsizetype elementCount, qsizetype elementSize, qsizetype headerSize) noexcept
{
    Q_ASSERT(elementSize);

    size_t bytes;
    if (Q_UNLIKELY(mul_overflow(size_t(elementSize), size_t(elementCount), &bytes)) ||
            Q_UNLIKELY(add_overflow(bytes, size_t(headerSize), &bytes)))
        return -1;
    if (Q_UNLIKELY(qsizetype(bytes) < 0))
        return -1;

    return qsizetype(bytes);
}

/*!
    \internal
    \since 5.7

    Returns the memory block size and the number of elements that will fit in
    that block for a container containing \a elementCount elements, each of \a
    elementSize bytes, plus a header of \a headerSize bytes. This function
    assumes the container will grow and pre-allocates a growth factor.

    Both \a elementCount and \a headerSize can be zero, but \a elementSize
    cannot.

    This function returns -1 on overflow or if the memory block size
    would not fit a qsizetype.

    \note The memory block may contain up to \a elementSize - 1 bytes more than
    needed.
*/
CalculateGrowingBlockSizeResult
qCalculateGrowingBlockSize(qsizetype elementCount, qsizetype elementSize, qsizetype headerSize) noexcept
{
    CalculateGrowingBlockSizeResult result = {
        qsizetype(-1), qsizetype(-1)
    };

    qsizetype bytes = qCalculateBlockSize(elementCount, elementSize, headerSize);
    if (bytes < 0)
        return result;

    size_t morebytes = static_cast<size_t>(qNextPowerOfTwo(quint64(bytes)));
    if (Q_UNLIKELY(qsizetype(morebytes) < 0)) {
        // grow by half the difference between bytes and morebytes
        // this slows the growth and avoids trying to allocate exactly
        // 2G of memory (on 32bit), something that many OSes can't deliver
        bytes += (morebytes - bytes) / 2;
    } else {
        bytes = qsizetype(morebytes);
    }

    result.elementCount = (bytes - headerSize) / elementSize;
    result.size = result.elementCount * elementSize + headerSize;
    return result;
}

static inline qsizetype calculateBlockSize(qsizetype &capacity, qsizetype objectSize, qsizetype headerSize, uint options)
{
    // Calculate the byte size
    // allocSize = objectSize * capacity + headerSize, but checked for overflow
    // plus padded to grow in size
    if (options & (QArrayData::GrowsForward | QArrayData::GrowsBackwards)) {
        auto r = qCalculateGrowingBlockSize(capacity, objectSize, headerSize);
        capacity = r.elementCount;
        return r.size;
    } else {
        return qCalculateBlockSize(capacity, objectSize, headerSize);
    }
}

static QArrayData *allocateData(qsizetype allocSize, uint options)
{
    QArrayData *header = static_cast<QArrayData *>(::malloc(size_t(allocSize)));
    if (header) {
        header->ref_.storeRelaxed(1);
        header->flags = options;
        header->alloc = 0;
    }
    return header;
}

void *QArrayData::allocate(QArrayData **dptr, qsizetype objectSize, qsizetype alignment,
        qsizetype capacity, ArrayOptions options) noexcept
{
    Q_ASSERT(dptr);
    // Alignment is a power of two
    Q_ASSERT(alignment >= qsizetype(alignof(QArrayData))
            && !(alignment & (alignment - 1)));

    if (capacity == 0) {
        *dptr = nullptr;
        return nullptr;
    }

    qsizetype headerSize = sizeof(QArrayData);
    const qsizetype headerAlignment = alignof(QArrayData);

    if (alignment > headerAlignment) {
        // Allocate extra (alignment - Q_ALIGNOF(QArrayData)) padding bytes so we
        // can properly align the data array. This assumes malloc is able to
        // provide appropriate alignment for the header -- as it should!
        headerSize += alignment - headerAlignment;
    }
    Q_ASSERT(headerSize > 0);

    qsizetype allocSize = calculateBlockSize(capacity, objectSize, headerSize, options);
    constexpr qsizetype reservedExtraBytes = std::max(
        sizeof(QByteArray::value_type), sizeof(QString::value_type));
    QArrayData *header = allocateData(allocSize + reservedExtraBytes, options);
    void *data = nullptr;
    if (header) {
        // find where offset should point to so that data() is aligned to alignment bytes
        data = QTypedArrayData<void>::dataStart(header, alignment);
        header->alloc = qsizetype(capacity);
    }

    *dptr = header;
    return data;
}

QPair<QArrayData *, void *>
QArrayData::reallocateUnaligned(QArrayData *data, void *dataPointer,
                                qsizetype objectSize, qsizetype capacity, ArrayOptions options) noexcept
{
    Q_ASSERT(!data || !data->isShared());

    qsizetype headerSize = sizeof(QArrayData);
    qsizetype allocSize = calculateBlockSize(capacity, objectSize, headerSize, options);
    qptrdiff offset = dataPointer ? reinterpret_cast<char *>(dataPointer) - reinterpret_cast<char *>(data) : headerSize;
    constexpr qsizetype reservedExtraBytes = std::max(
        sizeof(QByteArray::value_type), sizeof(QString::value_type));
    QArrayData *header = static_cast<QArrayData *>(
        ::realloc(data, size_t(allocSize + reservedExtraBytes)));
    if (header) {
        header->flags = options;
        header->alloc = uint(capacity);
        dataPointer = reinterpret_cast<char *>(header) + offset;
    }
    return qMakePair(static_cast<QArrayData *>(header), dataPointer);
}

void QArrayData::deallocate(QArrayData *data, qsizetype objectSize,
        qsizetype alignment) noexcept
{
    // Alignment is a power of two
    Q_ASSERT(alignment >= qsizetype(alignof(QArrayData))
            && !(alignment & (alignment - 1)));
    Q_UNUSED(objectSize);
    Q_UNUSED(alignment);

    ::free(data);
}

QT_END_NAMESPACE
