// SPDX-License-Identifier: GPL-3.0-only
// Part of MCU Studio. See LICENSE and THIRD-PARTY-NOTICES.md.

#include "Exif.h"

#include <cstring>

namespace exif {
namespace {

// A TIFF block inside an APP1 segment, read with bounds checks everywhere:
// the Exif in a damaged file is as likely to be damaged as anything else.
struct Block {
    qsizetype segment = -1; // the APP1's FF
    qsizetype tiff = -1;    // TIFF header, absolute
    qsizetype size = 0;     // bytes of TIFF data (payload minus "Exif\0\0")
    bool little = false;

    bool valid() const { return tiff >= 0; }
};

quint16 be16(const char *p)
{
    return quint16(quint16(quint8(p[0])) << 8 | quint8(p[1]));
}

Block findBlock(const QByteArray &jpeg)
{
    Block b;
    const qsizetype n = jpeg.size();
    if (n < 4 || quint8(jpeg[0]) != 0xFF || quint8(jpeg[1]) != 0xD8)
        return b;
    qsizetype p = 2;
    while (p + 4 <= n && quint8(jpeg[p]) == 0xFF) {
        const quint8 m = quint8(jpeg[p + 1]);
        if (m == 0xDA || m == 0xD9)
            break;
        const int len = be16(jpeg.constData() + p + 2);
        if (len < 2 || p + 2 + len > n)
            break;
        if (m == 0xE1 && len >= 8 + 8 && std::memcmp(jpeg.constData() + p + 4, "Exif\0\0", 6) == 0) {
            const char *t = jpeg.constData() + p + 10;
            if ((t[0] == 'I' && t[1] == 'I') || (t[0] == 'M' && t[1] == 'M')) {
                b.segment = p;
                b.tiff = p + 10;
                b.size = len - 8;
                b.little = t[0] == 'I';
            }
            return b;
        }
        p += 2 + len;
    }
    return b;
}

struct Reader {
    const QByteArray &bytes;
    const Block &b;

    bool in(qsizetype off, qsizetype len) const { return off >= 0 && len >= 0 && off + len <= b.size; }
    quint16 u16(qsizetype off) const
    {
        const auto *p = reinterpret_cast<const quint8 *>(bytes.constData() + b.tiff + off);
        return b.little ? quint16(p[0] | p[1] << 8) : quint16(p[1] | p[0] << 8);
    }
    quint32 u32(qsizetype off) const
    {
        const auto *p = reinterpret_cast<const quint8 *>(bytes.constData() + b.tiff + off);
        return b.little ? quint32(p[0]) | quint32(p[1]) << 8 | quint32(p[2]) << 16 | quint32(p[3]) << 24
                        : quint32(p[3]) | quint32(p[2]) << 8 | quint32(p[1]) << 16 | quint32(p[0]) << 24;
    }
    // Offset of `tag`'s 12-byte entry in the IFD at `ifd`, or -1.
    qsizetype entry(qsizetype ifd, quint16 tag) const
    {
        if (!in(ifd, 2))
            return -1;
        const int count = u16(ifd);
        for (int i = 0; i < count; ++i) {
            const qsizetype e = ifd + 2 + 12 * i;
            if (!in(e, 12))
                return -1;
            if (u16(e) == tag)
                return e;
        }
        return -1;
    }
    qsizetype nextIfdPointer(qsizetype ifd) const
    {
        if (!in(ifd, 2))
            return -1;
        const qsizetype at = ifd + 2 + 12 * qsizetype(u16(ifd));
        return in(at, 4) ? at : -1;
    }
    // A SHORT or LONG value with count 1.
    std::optional<quint32> scalar(qsizetype e) const
    {
        if (e < 0)
            return std::nullopt;
        const quint16 type = u16(e + 2);
        if (u32(e + 4) != 1)
            return std::nullopt;
        if (type == 3)
            return u16(e + 8);
        if (type == 4)
            return u32(e + 8);
        return std::nullopt;
    }
    QString ascii(qsizetype e) const
    {
        if (e < 0 || u16(e + 2) != 2)
            return QString();
        const quint32 count = u32(e + 4);
        const qsizetype off = count <= 4 ? e + 8 : qsizetype(u32(e + 8));
        if (!in(off, count))
            return QString();
        return QString::fromLatin1(bytes.constData() + b.tiff + off, int(count)).trimmed().remove(QChar(0));
    }
};

void put32(QByteArray &bytes, qsizetype at, quint32 v, bool little)
{
    auto *p = reinterpret_cast<quint8 *>(bytes.data() + at);
    if (little) {
        p[0] = quint8(v); p[1] = quint8(v >> 8); p[2] = quint8(v >> 16); p[3] = quint8(v >> 24);
    } else {
        p[3] = quint8(v); p[2] = quint8(v >> 8); p[1] = quint8(v >> 16); p[0] = quint8(v >> 24);
    }
}

void put16(QByteArray &bytes, qsizetype at, quint16 v, bool little)
{
    auto *p = reinterpret_cast<quint8 *>(bytes.data() + at);
    if (little) {
        p[0] = quint8(v); p[1] = quint8(v >> 8);
    } else {
        p[1] = quint8(v); p[0] = quint8(v >> 8);
    }
}

struct ThumbRef {
    qsizetype ifd1 = -1;
    qsizetype offsetEntry = -1, lengthEntry = -1;
    quint32 offset = 0, length = 0;
};

std::optional<ThumbRef> thumbRef(const Reader &r)
{
    if (!r.in(4, 4))
        return std::nullopt;
    const qsizetype ifd0 = r.u32(4);
    const qsizetype nextAt = r.nextIfdPointer(ifd0);
    if (nextAt < 0)
        return std::nullopt;
    ThumbRef t;
    t.ifd1 = r.u32(nextAt);
    if (t.ifd1 <= 0)
        return std::nullopt;
    t.offsetEntry = r.entry(t.ifd1, 0x0201);
    t.lengthEntry = r.entry(t.ifd1, 0x0202);
    const auto off = r.scalar(t.offsetEntry);
    const auto len = r.scalar(t.lengthEntry);
    if (!off || !len || !r.in(*off, *len) || *len < 4)
        return std::nullopt;
    t.offset = *off;
    t.length = *len;
    return t;
}

} // namespace

std::optional<QByteArray> thumbnail(const QByteArray &jpeg)
{
    const Block b = findBlock(jpeg);
    if (!b.valid())
        return std::nullopt;
    const Reader r{jpeg, b};
    const auto t = thumbRef(r);
    if (!t)
        return std::nullopt;
    return jpeg.mid(b.tiff + t->offset, t->length);
}

QByteArray withThumbnail(const QByteArray &jpeg, const QByteArray &newThumbnail)
{
    const Block b = findBlock(jpeg);
    if (!b.valid() || newThumbnail.isEmpty())
        return jpeg;
    const Reader r{jpeg, b};
    const auto t = thumbRef(r);
    if (!t)
        return jpeg;

    QByteArray out = jpeg;
    const bool lengthIsLong = r.u16(t->lengthEntry + 2) == 4;
    const auto setLength = [&](quint32 v) {
        if (lengthIsLong)
            put32(out, b.tiff + t->lengthEntry + 8, v, b.little);
        else
            put16(out, b.tiff + t->lengthEntry + 8, quint16(v), b.little);
    };

    // At the end of the block: resize the APP1 around the new thumbnail.
    if (qsizetype(t->offset) + t->length == b.size) {
        const qsizetype newSize = qsizetype(t->offset) + newThumbnail.size();
        const qsizetype segLength = 8 + newSize; // length field counts itself and "Exif\0\0"
        if (segLength <= 0xFFFF && (lengthIsLong || newThumbnail.size() <= 0xFFFF)) {
            setLength(quint32(newThumbnail.size()));
            out.replace(b.tiff + t->offset, t->length, newThumbnail);
            out[b.segment + 2] = char(segLength >> 8);
            out[b.segment + 3] = char(segLength & 0xFF);
            return out;
        }
    }
    // Elsewhere, but the new one fits in the old one's space.
    if (quint32(newThumbnail.size()) <= t->length) {
        out.replace(b.tiff + t->offset, newThumbnail.size(), newThumbnail);
        setLength(quint32(newThumbnail.size()));
        return out;
    }
    // No room: unlink IFD1, so no viewer shows the stale picture.
    const qsizetype ifd0 = r.u32(4);
    const qsizetype nextAt = r.nextIfdPointer(ifd0);
    if (nextAt >= 0)
        put32(out, b.tiff + nextAt, 0, b.little);
    return out;
}

QByteArray withPixelDimensions(const QByteArray &jpeg, int width, int height)
{
    const Block b = findBlock(jpeg);
    if (!b.valid() || width <= 0 || height <= 0)
        return jpeg;
    const Reader r{jpeg, b};
    if (!r.in(4, 4))
        return jpeg;
    const qsizetype ifd0 = r.u32(4);
    const auto exifIfd = r.scalar(r.entry(ifd0, 0x8769));
    if (!exifIfd)
        return jpeg;
    QByteArray out = jpeg;
    for (const auto &[tag, value] : {std::pair<quint16, int>{0xA002, width}, {0xA003, height}}) {
        const qsizetype e = r.entry(*exifIfd, tag);
        if (e < 0 || r.u32(e + 4) != 1)
            continue;
        const quint16 type = r.u16(e + 2);
        if (type == 3 && value <= 0xFFFF)
            put16(out, b.tiff + e + 8, quint16(value), b.little);
        else if (type == 4)
            put32(out, b.tiff + e + 8, quint32(value), b.little);
    }
    return out;
}

QString cameraModel(const QByteArray &jpeg)
{
    const Block b = findBlock(jpeg);
    if (!b.valid())
        return QString();
    const Reader r{jpeg, b};
    if (!r.in(4, 4))
        return QString();
    const qsizetype ifd0 = r.u32(4);
    const QString make = r.ascii(r.entry(ifd0, 0x010F));
    const QString model = r.ascii(r.entry(ifd0, 0x0110));
    if (model.startsWith(make, Qt::CaseInsensitive) || make.isEmpty())
        return model;
    return make + QLatin1Char(' ') + model;
}

int orientation(const QByteArray &jpeg)
{
    const Block b = findBlock(jpeg);
    if (!b.valid())
        return 1;
    const Reader r{jpeg, b};
    if (!r.in(4, 4))
        return 1;
    const auto v = r.scalar(r.entry(r.u32(4), 0x0112));
    return v && *v >= 1 && *v <= 8 ? int(*v) : 1;
}

} // namespace exif
