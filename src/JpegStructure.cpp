// SPDX-License-Identifier: GPL-3.0-only
// Part of MCU Studio. See LICENSE and THIRD-PARTY-NOTICES.md.

#include "JpegStructure.h"

#include <QCoreApplication>

#include <algorithm>
#include <cstring>

namespace jpegfile {
namespace {

QString tr(const char *text)
{
    return QCoreApplication::translate("JpegStructure", text);
}

quint16 be16(const quint8 *p)
{
    return quint16(quint16(p[0]) << 8 | p[1]);
}

bool isSof(quint8 m)
{
    return m >= 0xC0 && m <= 0xCF && m != 0xC4 && m != 0xC8 && m != 0xCC;
}

// Markers that can follow a scan in a well-formed file.
bool continuesStream(quint8 m)
{
    if (m >= 0xC0 && m <= 0xCF)
        return m != 0xC8;
    if (m >= 0xE0 && m <= 0xEF)
        return true;
    switch (m) {
    case 0xD9: case 0xDA: case 0xDB: case 0xDC: case 0xDD: case 0xDE: case 0xDF: case 0xFE:
        return true;
    default:
        return false;
    }
}

// The STOP/Djvu file marker: the GUID the ransomware writes at the very end of
// every file it encrypts, directly after the victim's personal ID.
constexpr char kDjvuMarker[] = "{36A698B9-D67C-4E07-BE82-0EC5B14B4DF5}";

bool isIdChar(char c)
{
    return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
}

} // namespace

QString Scan::describe() const
{
    QString comps;
    if (componentCount >= 3)
        comps = tr("all components");
    else {
        QStringList names;
        for (int i = 0; i < componentCount; ++i) {
            const int id = componentIds[i];
            names << (id == 1 ? QStringLiteral("Y")
                              : id == 2 ? QStringLiteral("Cb")
                                        : id == 3 ? QStringLiteral("Cr")
                                                  : QStringLiteral("#%1").arg(id));
        }
        comps = names.join(QStringLiteral(", "));
    }
    QString band = (ss == 0 && se == 63) ? tr("full") : (ss == 0 ? tr("DC") : tr("AC %1-%2").arg(ss).arg(se));
    if (ah > 0)
        return tr("%1, %2, refining bit %3").arg(band, comps).arg(al);
    if (al > 0)
        return tr("%1, %2, first pass at bit %3").arg(band, comps).arg(al);
    return tr("%1, %2").arg(band, comps);
}

const Segment *Structure::find(quint8 marker, const char *signature, int signatureLength,
                               const QByteArray *bytes) const
{
    for (const Segment &s : segments) {
        if (s.marker != marker)
            continue;
        if (!signature)
            return &s;
        if (bytes && s.dataOffset >= 0 && s.dataLength >= signatureLength
            && std::memcmp(bytes->constData() + s.dataOffset, signature, size_t(signatureLength)) == 0)
            return &s;
    }
    return nullptr;
}

Structure walk(const QByteArray &jpeg)
{
    Structure out;
    const auto *d = reinterpret_cast<const quint8 *>(jpeg.constData());
    const qsizetype n = jpeg.size();
    if (n < 4 || d[0] != 0xFF || d[1] != 0xD8) {
        out.problem = tr("does not begin with a JPEG header (FF D8).");
        return out;
    }
    out.segments.append(Segment{0xD8, 0, 2, -1, 0});

    qsizetype p = 2;
    while (true) {
        if (p >= n) {
            out.problem = tr("ends before its End Of Image marker.");
            return out;
        }
        if (d[p] != 0xFF) {
            out.problem = tr("has something other than a marker at offset %1.").arg(p);
            return out;
        }
        qsizetype m = p;
        while (m < n && d[m] == 0xFF)
            ++m;
        if (m >= n) {
            out.problem = tr("ends in padding.");
            return out;
        }
        const quint8 marker = d[m];
        if (marker == 0xD9) {
            out.segments.append(Segment{0xD9, m - 1, 2, -1, 0});
            out.imageEnd = m + 1;
            return out;
        }
        if (marker == 0x01 || (marker >= 0xD0 && marker <= 0xD7)) {
            out.segments.append(Segment{marker, m - 1, 2, -1, 0});
            p = m + 1;
            continue;
        }
        if (!out.scans.isEmpty() && !continuesStream(marker)) {
            out.scans.last().firstIllegal = m - 1;
            out.problem = tr("has a byte pair at offset %1 that no JPEG encoder writes (FF %2).")
                              .arg(m - 1)
                              .arg(marker, 2, 16, QLatin1Char('0'));
            return out;
        }
        if (m + 2 >= n) {
            out.problem = tr("stops in the middle of a marker segment.");
            return out;
        }
        const int length = be16(d + m + 1);
        if (length < 2 || m + 1 + length > n) {
            out.problem = tr("has a marker segment at offset %1 whose length runs past the end of "
                             "the file.")
                              .arg(m - 1);
            return out;
        }
        Segment seg{marker, m - 1, length + 2, m + 3, length - 2};
        out.segments.append(seg);
        const quint8 *payload = d + seg.dataOffset;

        if (isSof(marker) && out.frameOffset < 0 && seg.dataLength >= 6) {
            out.frameOffset = seg.offset;
            out.frameMarker = marker;
            out.height = be16(payload + 1);
            out.width = be16(payload + 3);
            out.components = payload[5];
        } else if (marker == 0xDD && seg.dataLength >= 2) {
            out.restartInterval = be16(payload);
        }
        p = m + 1 + length;
        if (marker != 0xDA)
            continue;

        Scan scan;
        scan.sosOffset = seg.offset;
        scan.dataStart = p;
        if (seg.dataLength >= 1) {
            scan.componentCount = qMin<int>(payload[0], 4);
            for (int i = 0; i < scan.componentCount && 1 + 2 * i < seg.dataLength; ++i)
                scan.componentIds[i] = payload[1 + 2 * i];
            const qsizetype tail = 1 + 2 * qsizetype(payload[0]);
            if (tail + 2 < seg.dataLength) {
                scan.ss = payload[tail];
                scan.se = payload[tail + 1];
                scan.ah = payload[tail + 2] >> 4;
                scan.al = payload[tail + 2] & 15;
            }
        }
        while (p < n) {
            if (d[p] != 0xFF) {
                ++p;
                continue;
            }
            qsizetype q = p;
            while (q < n && d[q] == 0xFF)
                ++q;
            if (q >= n)
                break;
            const quint8 next = d[q];
            if (next == 0x00) {
                p = q + 1;
                continue;
            }
            if (next >= 0xD0 && next <= 0xD7) {
                ++scan.restartMarkers;
                p = q + 1;
                continue;
            }
            break;
        }
        scan.dataEnd = qMin(p, n);
        out.scans.append(scan);
        if (p >= n) {
            out.problem = tr("ends inside its picture data, with no End Of Image marker.");
            return out;
        }
    }
}

// ---------------------------------------------------------------------------
// Trailers
// ---------------------------------------------------------------------------

QString Trailer::describe() const
{
    switch (kind) {
    case TrailerKind::None:
        return QString();
    case TrailerKind::Mpf:
        return tr("further images indexed by the file's MPF segment (%1 bytes)").arg(length);
    case TrailerKind::MotionPhoto:
        return tr("a motion photo video (%1 bytes)").arg(length);
    case TrailerKind::EmbeddedJpeg:
        return tr("another JPEG appended after the image (%1 bytes)").arg(length);
    case TrailerKind::StopDjvu:
        return tr("the STOP/Djvu ransomware footer (%1 bytes, personal ID %2)")
            .arg(length)
            .arg(personalId.isEmpty() ? tr("not found") : personalId);
    case TrailerKind::Unknown:
        return tr("%1 bytes of data after the image").arg(length);
    }
    return QString();
}

std::optional<Trailer> findStopDjvuFooter(const QByteArray &bytes)
{
    const qsizetype window = qMin<qsizetype>(bytes.size(), 4096);
    const qsizetype from = bytes.size() - window;
    const qsizetype at = bytes.lastIndexOf(QByteArray(kDjvuMarker));
    if (at < from || at < 0)
        return std::nullopt;

    Trailer t;
    t.kind = TrailerKind::StopDjvu;
    // The personal ID is the run of letters and digits just before the marker.
    qsizetype idStart = at;
    while (idStart > 0 && at - idStart < 64 && isIdChar(bytes.at(idStart - 1)))
        --idStart;
    if (at - idStart >= 16) {
        t.personalId = QString::fromLatin1(bytes.mid(idStart, at - idStart));
        t.offlineIdLikely = t.personalId.endsWith(QLatin1String("t1"));
    }
    // The footer starts after the original file's last byte, which for a JPEG
    // was its EOI. Look for it close in front of the ID; failing that, take
    // the ID itself as the start, which leaves at worst a few bytes of the
    // footer's binary prefix behind the image, where decoders ignore them.
    qsizetype start = idStart;
    const qsizetype searchFrom = qMax<qsizetype>(0, idStart - 1024);
    for (qsizetype i = idStart - 2; i >= searchFrom; --i) {
        if (quint8(bytes.at(i)) == 0xFF && quint8(bytes.at(i + 1)) == 0xD9) {
            start = i + 2;
            break;
        }
    }
    t.offset = start;
    t.length = bytes.size() - start;
    return t;
}

Trailer findTrailer(const QByteArray &bytes, qsizetype imageEnd)
{
    Trailer t;
    const auto *d = reinterpret_cast<const quint8 *>(bytes.constData());
    const qsizetype n = bytes.size();

    if (auto djvu = findStopDjvuFooter(bytes)) {
        if (imageEnd < 0 || djvu->offset >= imageEnd - 2) {
            if (imageEnd >= 0 && imageEnd < djvu->offset) {
                // Whatever sits between the image and the footer belonged to the
                // original file; keep it in the footer's count so it is not read
                // as picture data, but only the footer is the ransomware's.
                djvu->length += djvu->offset - imageEnd;
                djvu->offset = imageEnd;
            }
            return *djvu;
        }
    }

    if (imageEnd < 0) {
        // Too damaged to walk: find the end of the image by what follows it.
        for (qsizetype i = 0; i + 4 < n; ++i) {
            if (d[i] != 0xFF || d[i + 1] != 0xD9)
                continue;
            const bool soiFollows = i + 4 < n && d[i + 2] == 0xFF && d[i + 3] == 0xD8 && d[i + 4] == 0xFF;
            const bool mp4Follows = i + 10 <= n && std::memcmp(d + i + 6, "ftyp", 4) == 0;
            if (soiFollows || mp4Follows) {
                imageEnd = i + 2;
                break;
            }
        }
        if (imageEnd < 0)
            return t;
    }
    if (imageEnd >= n)
        return t;

    t.offset = imageEnd;
    t.length = n - imageEnd;
    // Padding some writers leave after the EOI is not worth a word.
    bool allPadding = true;
    for (qsizetype i = imageEnd; i < n && allPadding; ++i)
        allPadding = d[i] == 0x00 || d[i] == 0xFF;
    if (allPadding && t.length <= 4096) {
        t.kind = TrailerKind::Unknown;
        return t;
    }

    const Structure s = walk(bytes.left(imageEnd));
    if (s.find(0xE2, "MPF\0", 4, &bytes)) {
        t.kind = TrailerKind::Mpf;
        return t;
    }
    if (t.length >= 12 && std::memcmp(d + imageEnd + 4, "ftyp", 4) == 0) {
        t.kind = TrailerKind::MotionPhoto;
        return t;
    }
    if (t.length >= 3 && d[imageEnd] == 0xFF && d[imageEnd + 1] == 0xD8 && d[imageEnd + 2] == 0xFF) {
        t.kind = TrailerKind::EmbeddedJpeg;
        return t;
    }
    // Some phones put the motion video after padding or a vendor block.
    const qsizetype ftyp = bytes.indexOf("ftyp", imageEnd);
    if (ftyp >= 0 && ftyp - imageEnd < 65536) {
        t.kind = TrailerKind::MotionPhoto;
        return t;
    }
    t.kind = TrailerKind::Unknown;
    return t;
}

namespace {

struct Tiff {
    bool little = false;
    quint32 u32(const quint8 *p) const
    {
        return little ? quint32(p[0]) | quint32(p[1]) << 8 | quint32(p[2]) << 16 | quint32(p[3]) << 24
                      : quint32(p[3]) | quint32(p[2]) << 8 | quint32(p[1]) << 16 | quint32(p[0]) << 24;
    }
    quint16 u16(const quint8 *p) const
    {
        return little ? quint16(p[0] | p[1] << 8) : quint16(p[1] | p[0] << 8);
    }
    void put32(quint8 *p, quint32 v) const
    {
        if (little) {
            p[0] = quint8(v); p[1] = quint8(v >> 8); p[2] = quint8(v >> 16); p[3] = quint8(v >> 24);
        } else {
            p[3] = quint8(v); p[2] = quint8(v >> 8); p[1] = quint8(v >> 16); p[0] = quint8(v >> 24);
        }
    }
};

// The MP Entry table of an MPF segment: where it lives in `bytes`, where the
// MP header (the offsets' origin) is, and its byte order.
struct MpIndex {
    qsizetype header = -1;  // the TIFF header after "MPF\0"
    qsizetype entries = -1; // first 16-byte MP entry
    int count = 0;
    Tiff tiff;
};

std::optional<MpIndex> findMpIndex(const QByteArray &bytes)
{
    const Structure s = walk(bytes);
    const Segment *seg = s.find(0xE2, "MPF\0", 4, &bytes);
    if (!seg || seg->dataLength < 4 + 8 + 2)
        return std::nullopt;
    const auto *d = reinterpret_cast<const quint8 *>(bytes.constData());
    MpIndex idx;
    idx.header = seg->dataOffset + 4;
    const quint8 *h = d + idx.header;
    const qsizetype limit = seg->dataOffset + seg->dataLength; // absolute end of payload
    if (h[0] == 'I' && h[1] == 'I')
        idx.tiff.little = true;
    else if (!(h[0] == 'M' && h[1] == 'M'))
        return std::nullopt;
    const quint32 ifd = idx.tiff.u32(h + 4);
    if (idx.header + qsizetype(ifd) + 2 > limit)
        return std::nullopt;
    const quint8 *ip = h + ifd;
    const int entries = idx.tiff.u16(ip);
    for (int i = 0; i < entries; ++i) {
        const quint8 *e = ip + 2 + 12 * i;
        if (e + 12 - d > limit)
            return std::nullopt;
        if (idx.tiff.u16(e) != 0xB002) // MPEntry
            continue;
        const quint32 count = idx.tiff.u32(e + 4);
        const quint32 off = idx.tiff.u32(e + 8);
        if (count % 16 != 0 || idx.header + qsizetype(off) + qsizetype(count) > limit)
            return std::nullopt;
        idx.entries = idx.header + off;
        idx.count = int(count / 16);
        return idx;
    }
    return std::nullopt;
}

} // namespace

bool fixMpfOffsets(QByteArray &mainImage, const QByteArray &original, qsizetype originalTrailerStart)
{
    const auto before = findMpIndex(original);
    const auto after = findMpIndex(mainImage);
    if (!before || !after || before->count != after->count)
        return false;
    const qsizetype newTrailerStart = mainImage.size();
    auto *d = reinterpret_cast<quint8 *>(mainImage.data());
    for (int i = 0; i < after->count; ++i) {
        quint8 *e = d + after->entries + 16 * i;
        const quint32 offset = after->tiff.u32(e + 8);
        if (i == 0 || offset == 0) {
            // The primary image: offset 0 by definition, and its size is the
            // file the encoder just wrote rather than the one it replaced.
            after->tiff.put32(e + 4, quint32(mainImage.size()));
            continue;
        }
        const qint64 moved = qint64(offset) + qint64(before->header) - originalTrailerStart
            + newTrailerStart - qint64(after->header);
        if (moved <= 0 || moved > 0xFFFFFFFFLL)
            return false;
        after->tiff.put32(e + 8, quint32(moved));
    }
    return true;
}

// ---------------------------------------------------------------------------
// Embedded pictures
// ---------------------------------------------------------------------------

void frameSize(const QByteArray &jpeg, int *width, int *height)
{
    const Structure s = walk(jpeg);
    if (width)
        *width = s.width;
    if (height)
        *height = s.height;
}

namespace {

// A complete JPEG starting at `at`, measured by walking it.
std::optional<Embedded> jpegAt(const QByteArray &bytes, qsizetype at, qsizetype limit)
{
    const QByteArray candidate = bytes.mid(at, limit - at);
    const Structure s = walk(candidate);
    if (s.imageEnd < 0 || s.width <= 0 || s.height <= 0 || s.scans.isEmpty())
        return std::nullopt;
    Embedded e;
    e.offset = at;
    e.length = s.imageEnd;
    e.width = s.width;
    e.height = s.height;
    return e;
}

} // namespace

QVector<Embedded> carve(const QByteArray &bytes, int maxCount, qsizetype minLength)
{
    QVector<Embedded> found;
    const auto *d = reinterpret_cast<const quint8 *>(bytes.constData());
    const qsizetype n = bytes.size();
    qsizetype coveredUntil = -1;
    for (qsizetype i = 0; i + 3 < n && found.size() < maxCount; ++i) {
        if (d[i] != 0xFF || d[i + 1] != 0xD8 || d[i + 2] != 0xFF)
            continue;
        const quint8 m = d[i + 3];
        if (!(m >= 0xC0 && m != 0xFF))
            continue;
        if (i < coveredUntil)
            continue; // inside an image already found: its thumbnail
        auto e = jpegAt(bytes, i, n);
        if (!e || e->length < minLength)
            continue;
        e->origin = QCoreApplication::translate("JpegStructure", "JPEG at offset %1").arg(i);
        found.append(*e);
        coveredUntil = i + e->length;
        i = coveredUntil - 1;
    }
    std::sort(found.begin(), found.end(),
              [](const Embedded &a, const Embedded &b) { return a.length > b.length; });
    return found;
}

QVector<Embedded> embeddedJpegs(const QByteArray &bytes)
{
    QVector<Embedded> out;
    const auto *d = reinterpret_cast<const quint8 *>(bytes.constData());
    const qsizetype n = bytes.size();
    const auto seen = [&out](qsizetype off) {
        for (const Embedded &e : out)
            if (e.offset == off)
                return true;
        return false;
    };

    // MPF entries name their images exactly.
    if (const auto idx = findMpIndex(bytes)) {
        for (int i = 1; i < idx->count; ++i) {
            const quint8 *e = d + idx->entries + 16 * i;
            const qsizetype off = idx->header + qsizetype(idx->tiff.u32(e + 8));
            const qsizetype size = idx->tiff.u32(e + 4);
            if (off <= 0 || off >= n || size <= 0 || off + size > n)
                continue;
            if (auto img = jpegAt(bytes, off, off + size)) {
                img->origin = tr("MPF image %1").arg(i + 1);
                out.append(*img);
            }
        }
    }

    // Anything else: every SOI that walks to an EOI, past the primary's own.
    const Structure head = walk(bytes);
    for (qsizetype i = 1; i + 3 < n; ++i) {
        if (d[i] != 0xFF || d[i + 1] != 0xD8 || d[i + 2] != 0xFF)
            continue;
        if (seen(i))
            continue;
        auto img = jpegAt(bytes, i, n);
        if (!img)
            continue;
        // A thumbnail inside APP1 is the Exif one; anything else is labeled
        // by where it sits.
        QString origin = tr("embedded JPEG");
        for (const Segment &s : head.segments) {
            if (i > s.offset && i < s.offset + s.length) {
                origin = s.marker == 0xE1 ? tr("Exif thumbnail")
                                          : tr("preview in APP%1 segment").arg(s.marker - 0xE0);
                break;
            }
        }
        if (head.imageEnd >= 0 && i >= head.imageEnd)
            origin = tr("JPEG after the image");
        img->origin = origin;
        out.append(*img);
        i += img->length - 1;
    }
    return out;
}

} // namespace jpegfile
