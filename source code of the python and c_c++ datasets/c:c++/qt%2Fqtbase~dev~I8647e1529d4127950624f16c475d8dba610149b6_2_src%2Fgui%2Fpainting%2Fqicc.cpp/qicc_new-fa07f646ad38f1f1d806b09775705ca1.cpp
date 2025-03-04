/****************************************************************************
**
** Copyright (C) 2016 The Qt Company Ltd.
** Contact: https://www.qt.io/licensing/
**
** This file is part of the QtGui module of the Qt Toolkit.
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

#include "qicc_p.h"

#include <qbuffer.h>
#include <qbytearray.h>
#include <qdatastream.h>
#include <qendian.h>
#include <qloggingcategory.h>
#include <qstring.h>

#include "qcolorspace_p.h"
#include "qcolortrc_p.h"

QT_BEGIN_NAMESPACE
Q_LOGGING_CATEGORY(lcIcc, "qt.gui.icc")

struct ICCProfileHeader
{
    quint32_be profileSize;

    quint32_be preferredCmmType;

    quint32_be profileVersion;
    quint32_be profileClass;
    quint32_be inputColorSpace;
    quint32_be pcs;
    quint32_be datetime[3];
    quint32_be signature;
    quint32_be platformSignature;
    quint32_be flags;
    quint32_be deviceManufacturer;
    quint32_be deviceModel;
    quint32_be deviceAttributes[2];

    quint32_be renderingIntent;
    qint32_be  illuminantXyz[3];

    quint32_be creatorSignature;
    quint32_be profileId[4];

    quint32_be reserved[7];

// Technically after the header, but easier to include here:
    quint32_be tagCount;
};

constexpr quint32 IccTag(uchar a, uchar b, uchar c, uchar d)
{
    return (a << 24) | (b << 16) | (c << 8) | d;
}

enum class ProfileClass : quint32 {
    Input       = IccTag('s', 'c', 'r', 'n'),
    Display     = IccTag('m', 'n', 't', 'r'),
    // Not supported:
    Output      = IccTag('p', 'r', 't', 'r'),
    ColorSpace  = IccTag('s', 'p', 'a', 'c'),
};

enum class Tag : quint32 {
    acsp = IccTag('a', 'c', 's', 'p'),
    RGB_ = IccTag('R', 'G', 'B', ' '),
    XYZ_ = IccTag('X', 'Y', 'Z', ' '),
    rXYZ = IccTag('r', 'X', 'Y', 'Z'),
    gXYZ = IccTag('g', 'X', 'Y', 'Z'),
    bXYZ = IccTag('b', 'X', 'Y', 'Z'),
    rTRC = IccTag('r', 'T', 'R', 'C'),
    gTRC = IccTag('g', 'T', 'R', 'C'),
    bTRC = IccTag('b', 'T', 'R', 'C'),
    A2B0 = IccTag('A', '2', 'B', '0'),
    A2B1 = IccTag('A', '2', 'B', '1'),
    B2A0 = IccTag('B', '2', 'A', '0'),
    B2A1 = IccTag('B', '2', 'A', '1'),
    desc = IccTag('d', 'e', 's', 'c'),
    text = IccTag('t', 'e', 'x', 't'),
    cprt = IccTag('c', 'p', 'r', 't'),
    curv = IccTag('c', 'u', 'r', 'v'),
    para = IccTag('p', 'a', 'r', 'a'),
    wtpt = IccTag('w', 't', 'p', 't'),
    bkpt = IccTag('b', 'k', 'p', 't'),
    mft1 = IccTag('m', 'f', 't', '1'),
    mft2 = IccTag('m', 'f', 't', '2'),
    mluc = IccTag('m', 'l', 'u', 'c'),
    mAB_ = IccTag('m', 'A', 'B', ' '),
    mBA_ = IccTag('m', 'B', 'A', ' '),
    chad = IccTag('c', 'h', 'a', 'd'),
    sf32 = IccTag('s', 'f', '3', '2'),

    // Apple extensions for ICCv2:
    aarg = IccTag('a', 'a', 'r', 'g'),
    aagg = IccTag('a', 'a', 'g', 'g'),
    aabg = IccTag('a', 'a', 'b', 'g'),
};

inline uint qHash(const Tag &key, uint seed = 0)
{
    return qHash(quint32(key), seed);
}

namespace QIcc {

struct TagTableEntry
{
    quint32_be signature;
    quint32_be offset;
    quint32_be size;
};

struct GenericTagData {
    quint32_be type;
    quint32_be null;
};

struct XYZTagData : GenericTagData {
    qint32_be fixedX;
    qint32_be fixedY;
    qint32_be fixedZ;
};

struct CurvTagData : GenericTagData {
    quint32_be valueCount;
    quint16_be value[1];
};

struct ParaTagData : GenericTagData {
    quint16_be curveType;
    quint16_be null2;
    quint32_be parameter[1];
};

struct MlucTagRecord {
    quint16_be languageCode;
    quint16_be countryCode;
    quint32_be size;
    quint32_be offset;
};

struct MlucTagData : GenericTagData {
    quint32_be recordCount;
    quint32_be recordSize; // = sizeof(MlucTagRecord)
    MlucTagRecord records[1];
};

// For both mAB and mBA
struct mABTagData : GenericTagData {
    quint8 inputChannels;
    quint8 outputChannels;
    quint8 padding[2];
    quint32_be bCurvesOffset;
    quint32_be matrixOffset;
    quint32_be mCurvesOffset;
    quint32_be clutOffset;
    quint32_be aCurvesOffset;
};

struct Sf32TagData : GenericTagData {
    quint32_be value[1];
};

static int toFixedS1516(float x)
{
    return int(x * 65536.0f + 0.5f);
}

static float fromFixedS1516(int x)
{
    return x * (1.0f / 65536.0f);
}

QColorVector fromXyzData(const XYZTagData *xyz)
{
    const float x = fromFixedS1516(xyz->fixedX);
    const float y = fromFixedS1516(xyz->fixedY);
    const float z = fromFixedS1516(xyz->fixedZ);
    qCDebug(lcIcc) << "XYZ_ " << x << y << z;

    return QColorVector(x, y, z);
}

static bool isValidIccProfile(const ICCProfileHeader &header)
{
    if (header.signature != uint(Tag::acsp)) {
        qCWarning(lcIcc, "Failed ICC signature test");
        return false;
    }
    if (header.profileSize < (sizeof(ICCProfileHeader) + header.tagCount * sizeof(TagTableEntry))) {
        qCWarning(lcIcc, "Failed basic size sanity");
        return false;
    }

    if (header.profileClass != uint(ProfileClass::Input)
        && header.profileClass != uint(ProfileClass::Display)) {
        qCWarning(lcIcc, "Unsupported ICC profile class %x", quint32(header.profileClass));
        return false;
    }
    if (header.inputColorSpace != 0x52474220 /* 'RGB '*/) {
        qCWarning(lcIcc, "Unsupported ICC input color space %x", quint32(header.inputColorSpace));
        return false;
    }
    if (header.pcs != 0x58595a20 /* 'XYZ '*/) {
        // ### support PCSLAB
        qCWarning(lcIcc, "Unsupported ICC profile connection space %x", quint32(header.pcs));
        return false;
    }

    QColorVector illuminant;
    illuminant.x = fromFixedS1516(header.illuminantXyz[0]);
    illuminant.y = fromFixedS1516(header.illuminantXyz[1]);
    illuminant.z = fromFixedS1516(header.illuminantXyz[2]);
    if (illuminant != QColorVector::D50()) {
        qCWarning(lcIcc, "Invalid ICC illuminant");
        return false;
    }

    return true;
}

static int writeColorTrc(QDataStream &stream, const QColorTrc &trc)
{
    if (trc.isLinear()) {
        stream << uint(Tag::curv) << uint(0);
        stream << uint(0);
        return 12;
    }

    if (trc.m_type == QColorTrc::Type::Function) {
        const QColorTransferFunction &fun = trc.m_fun;
        stream << uint(Tag::para) << uint(0);
        if (fun.isGamma()) {
            stream << ushort(0) << ushort(0);
            stream << toFixedS1516(fun.m_g);
            return 12 + 4;
        }
        bool type3 = qFuzzyIsNull(fun.m_e) && qFuzzyIsNull(fun.m_f);
        stream << ushort(type3 ? 3 : 4) << ushort(0);
        stream << toFixedS1516(fun.m_g);
        stream << toFixedS1516(fun.m_a);
        stream << toFixedS1516(fun.m_b);
        stream << toFixedS1516(fun.m_c);
        stream << toFixedS1516(fun.m_d);
        if (type3)
            return 12 + 5 * 4;
        stream << toFixedS1516(fun.m_e);
        stream << toFixedS1516(fun.m_f);
        return 12 + 7 * 4;
    }

    Q_ASSERT(trc.m_type == QColorTrc::Type::Table);
    stream << uint(Tag::curv) << uint(0);
    stream << uint(trc.m_table.m_tableSize);
    if (!trc.m_table.m_table16.isEmpty()) {
        for (uint i = 0; i < trc.m_table.m_tableSize; ++i) {
            stream << ushort(trc.m_table.m_table16[i]);
        }
    } else {
        for (uint i = 0; i < trc.m_table.m_tableSize; ++i) {
            stream << ushort(trc.m_table.m_table8[i] * 257U);
        }
    }
    return 12 + 2 * trc.m_table.m_tableSize;
}

QByteArray toIccProfile(const QColorSpace &space)
{
    if (!space.isValid())
        return QByteArray();

    const QColorSpacePrivate *spaceDPtr = QColorSpacePrivate::get(space);

    constexpr int tagCount = 9;
    constexpr uint profileDataOffset = 128 + 4 + 12 * tagCount;
    constexpr uint variableTagTableOffsets = 128 + 4 + 12 * 5;
    uint currentOffset = 0;
    uint rTrcOffset, gTrcOffset, bTrcOffset;
    uint rTrcSize, gTrcSize, bTrcSize;
    uint descOffset, descSize;

    QBuffer buffer;
    buffer.open(QIODevice::WriteOnly);
    QDataStream stream(&buffer);

    // Profile header:
    stream << uint(0); // Size, we will update this later
    stream << uint(0);
    stream << uint(0x02400000); // Version 2.4 (note we use 'para' from version 4)
    stream << uint(ProfileClass::Display);
    stream << uint(Tag::RGB_);
    stream << uint(Tag::XYZ_);
    stream << uint(0) << uint(0) << uint(0);
    stream << uint(Tag::acsp);
    stream << uint(0) << uint(0) << uint(0);
    stream << uint(0) << uint(0) << uint(0);
    stream << uint(1); // Rendering intent
    stream << uint(0x0000f6d6); // D50 X
    stream << uint(0x00010000); // D50 Y
    stream << uint(0x0000d32d); // D50 Z
    stream << IccTag('Q','t', QT_VERSION_MAJOR, QT_VERSION_MINOR);
    stream << uint(0) << uint(0) << uint(0) << uint(0);
    stream << uint(0) << uint(0) << uint(0) << uint(0) << uint(0) << uint(0) << uint(0);

    // Tag table:
    stream << uint(tagCount);
    stream << uint(Tag::rXYZ) << uint(profileDataOffset + 00) << uint(20);
    stream << uint(Tag::gXYZ) << uint(profileDataOffset + 20) << uint(20);
    stream << uint(Tag::bXYZ) << uint(profileDataOffset + 40) << uint(20);
    stream << uint(Tag::wtpt) << uint(profileDataOffset + 60) << uint(20);
    stream << uint(Tag::cprt) << uint(profileDataOffset + 80) << uint(12);
    // From here the offset and size will be updated later:
    stream << uint(Tag::rTRC) << uint(0) << uint(0);
    stream << uint(Tag::gTRC) << uint(0) << uint(0);
    stream << uint(Tag::bTRC) << uint(0) << uint(0);
    stream << uint(Tag::desc) << uint(0) << uint(0);
    // TODO: consider adding 'chad' tag (required in ICC >=4 when we have non-D50 whitepoint)
    currentOffset = profileDataOffset;

    // Tag data:
    stream << uint(Tag::XYZ_) << uint(0);
    stream << toFixedS1516(spaceDPtr->toXyz.r.x);
    stream << toFixedS1516(spaceDPtr->toXyz.r.y);
    stream << toFixedS1516(spaceDPtr->toXyz.r.z);
    stream << uint(Tag::XYZ_) << uint(0);
    stream << toFixedS1516(spaceDPtr->toXyz.g.x);
    stream << toFixedS1516(spaceDPtr->toXyz.g.y);
    stream << toFixedS1516(spaceDPtr->toXyz.g.z);
    stream << uint(Tag::XYZ_) << uint(0);
    stream << toFixedS1516(spaceDPtr->toXyz.b.x);
    stream << toFixedS1516(spaceDPtr->toXyz.b.y);
    stream << toFixedS1516(spaceDPtr->toXyz.b.z);
    stream << uint(Tag::XYZ_) << uint(0);
    stream << toFixedS1516(spaceDPtr->whitePoint.x);
    stream << toFixedS1516(spaceDPtr->whitePoint.y);
    stream << toFixedS1516(spaceDPtr->whitePoint.z);
    stream << uint(Tag::text) << uint(0);
    stream << uint(IccTag('N', '/', 'A', '\0'));
    currentOffset += 92;

    // From now on the data is variable sized:
    rTrcOffset = currentOffset;
    rTrcSize = writeColorTrc(stream, spaceDPtr->trc[0]);
    currentOffset += rTrcSize;
    if (spaceDPtr->trc[0] == spaceDPtr->trc[1]) {
        gTrcOffset = rTrcOffset;
        gTrcSize = rTrcSize;
    } else {
        gTrcOffset = currentOffset;
        gTrcSize = writeColorTrc(stream, spaceDPtr->trc[1]);
        currentOffset += gTrcSize;
    }
    if (spaceDPtr->trc[0] == spaceDPtr->trc[2]) {
        bTrcOffset = rTrcOffset;
        bTrcSize = rTrcSize;
    } else {
        bTrcOffset = currentOffset;
        bTrcSize = writeColorTrc(stream, spaceDPtr->trc[2]);
        currentOffset += bTrcSize;
    }

    descOffset = currentOffset;
    QByteArray description = spaceDPtr->description.toUtf8();
    stream << uint(Tag::desc) << uint(0);
    stream << uint(description.size() + 1);
    stream.writeRawData(description.constData(), description.size() + 1);
    stream << uint(0) << uint(0);
    stream << ushort(0) << uchar(0);
    QByteArray macdesc(67, '\0');
    stream.writeRawData(macdesc.constData(), 67);
    descSize = 90 + description.size() + 1;
    currentOffset += descSize;

    buffer.close();
    QByteArray iccProfile = buffer.buffer();
    // Now write final size
    *(quint32_be *)iccProfile.data() = iccProfile.size();
    // And the final indices and sizes of variable size tags:
    *(quint32_be *)(iccProfile.data() + variableTagTableOffsets + 4) = rTrcOffset;
    *(quint32_be *)(iccProfile.data() + variableTagTableOffsets + 8) = rTrcSize;
    *(quint32_be *)(iccProfile.data() + variableTagTableOffsets + 12 + 4) = gTrcOffset;
    *(quint32_be *)(iccProfile.data() + variableTagTableOffsets + 12 + 8) = gTrcSize;
    *(quint32_be *)(iccProfile.data() + variableTagTableOffsets + 2 * 12 + 4) = bTrcOffset;
    *(quint32_be *)(iccProfile.data() + variableTagTableOffsets + 2 * 12 + 8) = bTrcSize;
    *(quint32_be *)(iccProfile.data() + variableTagTableOffsets + 3 * 12 + 4) = descOffset;
    *(quint32_be *)(iccProfile.data() + variableTagTableOffsets + 3 * 12 + 8) = descSize;

#if !defined(QT_NO_DEBUG) || defined(QT_FORCE_ASSERTS)
    const ICCProfileHeader *iccHeader = (const ICCProfileHeader *)iccProfile.constData();
    Q_ASSERT(qsizetype(iccHeader->profileSize) == qsizetype(iccProfile.size()));
    Q_ASSERT(isValidIccProfile(*iccHeader));
#endif

    return iccProfile;
}

bool parseTRC(const GenericTagData *trcData, QColorTrc &gamma)
{
    if (trcData->type == quint32(Tag::curv)) {
        const CurvTagData *curv = reinterpret_cast<const CurvTagData *>(trcData);
        qCDebug(lcIcc) << "curv" << uint(curv->valueCount);
        if (curv->valueCount == 0) {
            gamma.m_type = QColorTrc::Type::Function;
            gamma.m_fun = QColorTransferFunction(); // Linear
        } else if (curv->valueCount == 1) {
            float g = curv->value[0] * (1.0f / 256.0f);
            qCDebug(lcIcc) << g;
            gamma.m_type = QColorTrc::Type::Function;
            gamma.m_fun = QColorTransferFunction::fromGamma(g);
        } else {
            QVector<quint16> tabl;
            tabl.resize(curv->valueCount);
            for (uint i = 0; i < curv->valueCount; ++i)
                tabl[i] = curv->value[i];
            QColorTransferTable table = QColorTransferTable(curv->valueCount, std::move(tabl));
            QColorTransferFunction curve;
            if (!table.asColorTransferFunction(&curve)) {
                gamma.m_type = QColorTrc::Type::Table;
                gamma.m_table = table;
            } else {
                qCDebug(lcIcc) << "Detected curv table as function";
                gamma.m_type = QColorTrc::Type::Function;
                gamma.m_fun = curve;
            }
        }
        return true;
    }
    if (trcData->type == quint32(Tag::para)) {
        const ParaTagData *para = reinterpret_cast<const ParaTagData *>(trcData);
        qCDebug(lcIcc) << "para" << uint(para->curveType);
        switch (para->curveType) {
        case 0: {
            float g = fromFixedS1516(para->parameter[0]);
            qCDebug(lcIcc) << g;
            gamma.m_type = QColorTrc::Type::Function;
            gamma.m_fun = QColorTransferFunction::fromGamma(g);
            break;
        }
        case 1: {
            float g = fromFixedS1516(para->parameter[0]);
            float a = fromFixedS1516(para->parameter[1]);
            float b = fromFixedS1516(para->parameter[2]);
            float d = -b / a;
            qCDebug(lcIcc) << g << a << b;
            gamma.m_type = QColorTrc::Type::Function;
            gamma.m_fun = QColorTransferFunction(a, b, 0.0f, d, 0.0f, 0.0f, g);
            break;
        }
        case 2: {
            float g = fromFixedS1516(para->parameter[0]);
            float a = fromFixedS1516(para->parameter[1]);
            float b = fromFixedS1516(para->parameter[2]);
            float c = fromFixedS1516(para->parameter[3]);
            float d = -b / a;
            qCDebug(lcIcc) << g << a << b << c;
            gamma.m_type = QColorTrc::Type::Function;
            gamma.m_fun = QColorTransferFunction(a, b, 0.0f, d, c, c, g);
            break;
        }
        case 3: {
            float g = fromFixedS1516(para->parameter[0]);
            float a = fromFixedS1516(para->parameter[1]);
            float b = fromFixedS1516(para->parameter[2]);
            float c = fromFixedS1516(para->parameter[3]);
            float d = fromFixedS1516(para->parameter[4]);
            qCDebug(lcIcc) << g << a << b << c << d;
            gamma.m_type = QColorTrc::Type::Function;
            gamma.m_fun = QColorTransferFunction(a, b, c, d, 0.0f, 0.0f, g);
            break;
        }
        case 4: {
            float g = fromFixedS1516(para->parameter[0]);
            float a = fromFixedS1516(para->parameter[1]);
            float b = fromFixedS1516(para->parameter[2]);
            float c = fromFixedS1516(para->parameter[3]);
            float d = fromFixedS1516(para->parameter[4]);
            float e = fromFixedS1516(para->parameter[5]);
            float f = fromFixedS1516(para->parameter[6]);
            qCDebug(lcIcc) << g << a << b << c << d << e << f;
            gamma.m_type = QColorTrc::Type::Function;
            gamma.m_fun = QColorTransferFunction(a, b, c, d, e, f, g);
            break;
        }
        default:
            qCWarning(lcIcc)  << "Unknown para type" << uint(para->curveType);
            return false;
        }
        return true;
    }
    qCWarning(lcIcc) << "Invalid TRC data type";
    return false;
}

bool parseDesc(const QByteArray &data, quint32 offset, QString &descName)
{
    const GenericTagData *tag = (const GenericTagData *)(data.constData() + offset);

    // Either 'desc' (ICCv2) or 'mluc' (ICCv4)
    if (tag->type == quint32(Tag::desc)) {
        quint32 len = *(quint32_be *)(data.constData() + offset + 8);
        if (len < 1)
            return false;
        const char *asciiName = data.constData() + offset + 12;
        if (offset + 12 + len > quint32(data.length()))
            return false;
        if (asciiName[len - 1] != '\0')
            return false;
        descName = QString::fromLatin1(asciiName, len - 1);
        return true;
    }
    if (tag->type != quint32(Tag::mluc))
        return false;

    const MlucTagData *mluc = (const MlucTagData *)(data.constData() + offset);
    if (mluc->recordCount < 1)
        return false;
    if (mluc->recordSize < 12)
        return false;
    // We just use the primary record regardless of language or country.
    const quint32 stringOffset = mluc->records[0].offset;
    const quint32 stringSize = mluc->records[0].size;
    if (offset + stringOffset + stringSize > quint32(data.size()))
        return false;
    if (stringSize & 1)
        return false;
    quint32 stringLen = stringSize / 2;
    const ushort *unicodeString = (const ushort *)(data.constData() + offset + stringOffset);
    // The given length shouldn't include 0-termination, but might.
    if (stringLen > 1 && unicodeString[stringLen - 1] == 0)
        --stringLen;
    QVarLengthArray<quint16> utf16hostendian(stringLen);
    qFromBigEndian<ushort>(unicodeString, stringLen, utf16hostendian.data());
    descName = QString::fromUtf16(utf16hostendian.data(), stringLen);
    return true;
}

bool fromIccProfile(const QByteArray &data, QColorSpace *colorSpace)
{
    if (data.size() < qsizetype(sizeof(ICCProfileHeader))) {
        qCWarning(lcIcc) << "fromIccProfile: failed size sanity 1";
        return false;
    }
    const ICCProfileHeader *header = (const ICCProfileHeader *)data.constData();
    if (!isValidIccProfile(*header)) {
        qCWarning(lcIcc) << "fromIccProfile: failed general sanity check";
        return false;
    }
    if (qsizetype(header->profileSize) > data.size()) {
        qCWarning(lcIcc) << "fromIccProfile: failed size sanity 2";
        return false;
    }

    // Read tag index
    const TagTableEntry *tagTable = (const TagTableEntry *)(data.constData() + sizeof(ICCProfileHeader));
    const qsizetype offsetToData = sizeof(ICCProfileHeader) + header->tagCount * sizeof(TagTableEntry);

    QHash<Tag, quint32> tagIndex;
    for (uint i = 0; i < header->tagCount; ++i) {
        // Sanity check tag sizes and offsets:
        if (qsizetype(tagTable[i].offset) < offsetToData) {
            qCWarning(lcIcc) << "fromIccProfile: failed tag offset sanity 1";
            return false;
        }
        // Checked separately from (+ size) to handle overflow.
        if (tagTable[i].offset > header->profileSize) {
            qCWarning(lcIcc) << "fromIccProfile: failed tag offset sanity 2";
            return false;
        }
        if ((tagTable[i].offset + tagTable[i].size) > header->profileSize) {
            qCWarning(lcIcc) << "fromIccProfile: failed tag offset + size sanity";
            return false;
        }
//        printf("'%4s' %d %d\n", (const char *)&tagTable[i].signature,
//                                quint32(tagTable[i].offset),
//                                quint32(tagTable[i].size));
        tagIndex.insert(Tag(quint32(tagTable[i].signature)), tagTable[i].offset);
    }
    // Check the profile is three-component matrix based (what we currently support):
    if (!tagIndex.contains(Tag::rXYZ) || !tagIndex.contains(Tag::gXYZ) || !tagIndex.contains(Tag::bXYZ) ||
        !tagIndex.contains(Tag::rTRC) || !tagIndex.contains(Tag::gTRC) || !tagIndex.contains(Tag::bTRC) ||
        !tagIndex.contains(Tag::wtpt)) {
        qCWarning(lcIcc) << "fromIccProfile: Unsupported ICC profile - not three component matrix based";
        return false;
    }

    // Parse XYZ tags
    const XYZTagData *rXyz = (const XYZTagData *)(data.constData() + tagIndex[Tag::rXYZ]);
    const XYZTagData *gXyz = (const XYZTagData *)(data.constData() + tagIndex[Tag::gXYZ]);
    const XYZTagData *bXyz = (const XYZTagData *)(data.constData() + tagIndex[Tag::bXYZ]);
    const XYZTagData *wXyz = (const XYZTagData *)(data.constData() + tagIndex[Tag::wtpt]);
    if (rXyz->type != quint32(Tag::XYZ_) || gXyz->type != quint32(Tag::XYZ_) ||
        wXyz->type != quint32(Tag::XYZ_) || wXyz->type != quint32(Tag::XYZ_)) {
        qCWarning(lcIcc) << "fromIccProfile: Bad XYZ data type";
        return false;
    }
    QColorSpacePrivate *colorspaceDPtr = QColorSpacePrivate::getWritable(*colorSpace);

    colorspaceDPtr->toXyz.r = fromXyzData(rXyz);
    colorspaceDPtr->toXyz.g = fromXyzData(gXyz);
    colorspaceDPtr->toXyz.b = fromXyzData(bXyz);
    QColorVector whitePoint = fromXyzData(wXyz);
    colorspaceDPtr->whitePoint = whitePoint;

    colorspaceDPtr->gamut = QColorSpace::Gamut::Custom;
    if (colorspaceDPtr->toXyz == QColorMatrix::toXyzFromSRgb()) {
        qCDebug(lcIcc) << "fromIccProfile: sRGB gamut detected";
        colorspaceDPtr->gamut = QColorSpace::Gamut::SRgb;
    } else if (colorspaceDPtr->toXyz == QColorMatrix::toXyzFromAdobeRgb()) {
        qCDebug(lcIcc) << "fromIccProfile: Adobe RGB gamut detected";
        colorspaceDPtr->gamut = QColorSpace::Gamut::AdobeRgb;
    } else if (colorspaceDPtr->toXyz == QColorMatrix::toXyzFromDciP3D65()) {
        qCDebug(lcIcc) << "fromIccProfile: DCI-P3 D65 gamut detected";
        colorspaceDPtr->gamut = QColorSpace::Gamut::DciP3D65;
    } else if (colorspaceDPtr->toXyz == QColorMatrix::toXyzFromBt2020()) {
        qCDebug(lcIcc) << "fromIccProfile: BT.2020 gamut detected";
        colorspaceDPtr->gamut = QColorSpace::Gamut::Bt2020;
    }
    if (colorspaceDPtr->toXyz == QColorMatrix::toXyzFromProPhotoRgb()) {
        qCDebug(lcIcc) << "fromIccProfile: ProPhoto RGB gamut detected";
        colorspaceDPtr->gamut = QColorSpace::Gamut::ProPhotoRgb;
    }
    // Reset the matrix to our canonical values:
    if (colorspaceDPtr->gamut != QColorSpace::Gamut::Custom)
        colorspaceDPtr->setToXyzMatrix();

    // Parse TRC tags
    const GenericTagData *rTrc;
    const GenericTagData *gTrc;
    const GenericTagData *bTrc;
    if (tagIndex.contains(Tag::aarg) && tagIndex.contains(Tag::aagg) && tagIndex.contains(Tag::aabg)) {
        // Apple extension for parametric version of TRCs in ICCv2:
        rTrc = (const GenericTagData *)(data.constData() + tagIndex[Tag::aarg]);
        gTrc = (const GenericTagData *)(data.constData() + tagIndex[Tag::aagg]);
        bTrc = (const GenericTagData *)(data.constData() + tagIndex[Tag::aabg]);
    } else {
        rTrc = (const GenericTagData *)(data.constData() + tagIndex[Tag::rTRC]);
        gTrc = (const GenericTagData *)(data.constData() + tagIndex[Tag::gTRC]);
        bTrc = (const GenericTagData *)(data.constData() + tagIndex[Tag::bTRC]);
    }

    QColorTrc rCurve;
    QColorTrc gCurve;
    QColorTrc bCurve;
    if (!parseTRC(rTrc, rCurve))
        return false;
    if (!parseTRC(gTrc, gCurve))
        return false;
    if (!parseTRC(bTrc, bCurve))
        return false;
    if (rCurve == gCurve && gCurve == bCurve && rCurve.m_type == QColorTrc::Type::Function) {
        if (rCurve.m_fun.isLinear()) {
            qCDebug(lcIcc) << "fromIccProfile: Linear gamma detected";
            colorspaceDPtr->trc[0] = QColorTransferFunction();
            colorspaceDPtr->transferFunction = QColorSpace::TransferFunction::Linear;
            colorspaceDPtr->gamma = 1.0f;
        } else if (rCurve.m_fun.isGamma()) {
            qCDebug(lcIcc) << "fromIccProfile: Simple gamma detected";
            colorspaceDPtr->trc[0] = QColorTransferFunction::fromGamma(rCurve.m_fun.m_g);
            colorspaceDPtr->transferFunction = QColorSpace::TransferFunction::Gamma;
            colorspaceDPtr->gamma = rCurve.m_fun.m_g;
        } else if (rCurve.m_fun.isSRgb()) {
            qCDebug(lcIcc) << "fromIccProfile: sRGB gamma detected";
            colorspaceDPtr->trc[0] = QColorTransferFunction::fromSRgb();
            colorspaceDPtr->transferFunction = QColorSpace::TransferFunction::SRgb;
        } else {
            colorspaceDPtr->trc[0] = rCurve;
            colorspaceDPtr->transferFunction = QColorSpace::TransferFunction::Custom;
        }

        colorspaceDPtr->trc[1] = colorspaceDPtr->trc[0];
        colorspaceDPtr->trc[2] = colorspaceDPtr->trc[0];
    } else {
        colorspaceDPtr->trc[0] = rCurve;
        colorspaceDPtr->trc[1] = gCurve;
        colorspaceDPtr->trc[2] = bCurve;
        colorspaceDPtr->transferFunction = QColorSpace::TransferFunction::Custom;
    }

    if (tagIndex.contains(Tag::desc)) {
        if (!parseDesc(data, tagIndex[Tag::desc], colorspaceDPtr->description))
            qCWarning(lcIcc) << "fromIccProfile: Failed to parse description";
        else
            qCDebug(lcIcc) << "fromIccProfile: Description" << colorspaceDPtr->description;
    }

    if (!colorspaceDPtr->identifyColorSpace())
        colorspaceDPtr->id = QColorSpace::Unknown;
    else
        qCDebug(lcIcc) << "fromIccProfile: Named colorspace detected: " << colorSpace->colorSpaceId();

    colorspaceDPtr->iccProfile = data;

    return true;
}

} // namespace QIcc

QT_END_NAMESPACE
