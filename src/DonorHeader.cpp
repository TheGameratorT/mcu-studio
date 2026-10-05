// SPDX-License-Identifier: GPL-3.0-only
// Part of MCU Studio. See LICENSE and THIRD-PARTY-NOTICES.md.

#include "DonorHeader.h"

#include <QCoreApplication>
#include <QMap>

#include <cstring>

#include "JpegStructure.h"

namespace donor {
namespace {

QString tr(const char *text)
{
    return QCoreApplication::translate("DonorHeader", text);
}

quint16 be16(const quint8 *p)
{
    return quint16(quint16(p[0]) << 8 | p[1]);
}

// Markers that stand alone: no length, no payload.
bool isStandalone(quint8 marker)
{
    return marker == 0x01 || (marker >= 0xD0 && marker <= 0xD9);
}

// Frame headers. C4 is DHT, C8 is reserved, CC is DAC -- all of them sit in
// the same numeric range without being frames.
bool isSof(quint8 marker)
{
    return marker >= 0xC0 && marker <= 0xCF && marker != 0xC4 && marker != 0xC8
        && marker != 0xCC;
}

bool isProgressiveSof(quint8 marker)
{
    return marker == 0xC2 || marker == 0xC6 || marker == 0xCA;
}

struct Segment {
    quint8 marker = 0;
    qsizetype start = 0;      // the segment's first FF
    qsizetype length = 0;     // through the end of the payload
    qsizetype dataStart = -1; // first payload byte, past the length field
    qsizetype dataLength = 0;
};

// Walks the marker segments from the SOI up to and including the first SOS,
// handing each to `visit` and filling in what the header says along the way.
// Both readers of a JPEG's front matter go through here, so a file that scans
// cleanly is exactly a file whose header can be copied.
template <typename Visit>
Layout walkMarkers(const QByteArray &jpeg, Visit visit)
{
    Layout out;
    const auto *d = reinterpret_cast<const quint8 *>(jpeg.constData());
    const qsizetype n = jpeg.size();

    if (n < 4 || d[0] != 0xFF || d[1] != 0xD8) {
        out.problem = tr("does not begin with a JPEG header (FF D8).");
        return out;
    }
    out.hasSoi = true;
    visit(Segment{0xD8, 0, 2, -1, 0});

    qsizetype p = 2;
    while (true) {
        if (p >= n) {
            out.problem = tr("ends before its scan header.");
            break;
        }
        if (d[p] != 0xFF) {
            out.problem = tr("has something other than a marker at offset %1, where the next "
                             "header segment should start.")
                              .arg(p);
            break;
        }
        // Fill bytes: any number of FFs may pad the gap before a marker.
        qsizetype m = p;
        while (m < n && d[m] == 0xFF)
            ++m;
        if (m >= n) {
            out.problem = tr("ends in padding, before its scan header.");
            break;
        }

        const quint8 marker = d[m];
        const qsizetype afterMarker = m + 1;

        if (marker == 0xD9) {
            out.problem = tr("ends at its EOI marker without ever starting a scan.");
            break;
        }
        if (isStandalone(marker)) {
            visit(Segment{marker, p, afterMarker - p, -1, 0});
            p = afterMarker;
            continue;
        }
        if (afterMarker + 1 >= n) {
            out.problem = tr("stops in the middle of a marker segment.");
            break;
        }
        const int length = be16(d + afterMarker);
        if (length < 2 || afterMarker + length > n) {
            out.problem = tr("has a marker segment at offset %1 whose length runs past the end "
                             "of the file.")
                              .arg(m - 1);
            break;
        }

        Segment seg;
        seg.marker = marker;
        seg.start = p;
        seg.length = afterMarker + length - p;
        seg.dataStart = afterMarker + 2;
        seg.dataLength = length - 2;

        if (isSof(marker)) {
            if (seg.dataLength < 6) {
                out.problem = tr("has a frame header too short to read.");
                break;
            }
            if (out.sofOffset < 0) {
                out.sofOffset = m - 1;
                out.height = be16(d + seg.dataStart + 1);
                out.width = be16(d + seg.dataStart + 3);
                out.numComponents = d[seg.dataStart + 5];
                out.progressive = isProgressiveSof(marker);
            }
        } else if (marker == 0xDD && seg.dataLength >= 2) {
            out.restartInterval = be16(d + seg.dataStart);
        }

        visit(seg);

        if (marker == 0xDA) {
            out.sosOffset = m - 1;
            out.entropyStart = seg.start + seg.length;
            break;
        }
        p = seg.start + seg.length;
    }
    return out;
}

bool payloadIs(const QByteArray &jpeg, const Segment &seg, const char *signature, qsizetype n)
{
    if (seg.dataStart < 0 || seg.dataLength < n)
        return false;
    return std::memcmp(jpeg.constData() + seg.dataStart, signature, size_t(n)) == 0;
}

// APPn and COM segments that describe *this photograph* rather than how to
// decode it. The donor's Exif would stamp another shot's date, lens and GPS
// onto the rescued file, and its thumbnail would show the wrong picture, so
// those go. The ones that change how the data decodes stay, because for the
// donor's tables the donor's are the right ones: JFIF's pixel density, Adobe's
// color transform flag, and the ICC profile -- which for a camera roll is the
// same profile the damaged file had.
bool isIdentityMarker(const QByteArray &jpeg, const Segment &seg)
{
    const quint8 m = seg.marker;
    if (m == 0xFE) // COM
        return true;
    if (m < 0xE0 || m > 0xEF)
        return false;
    if (m == 0xE0 && (payloadIs(jpeg, seg, "JFIF\0", 5) || payloadIs(jpeg, seg, "JFXX\0", 5)))
        return false;
    if (m == 0xE2 && payloadIs(jpeg, seg, "ICC_PROFILE\0", 12))
        return false;
    if (m == 0xEE && payloadIs(jpeg, seg, "Adobe", 5))
        return false;
    return true;
}

// The damaged file's own Exif, if it is still there ahead of the splice point.
// Worth the look: partial damage often spares it, and it carries the date the
// photograph was taken -- which is most of why anyone is rescuing the file.
// The TIFF byte-order magic is checked as well as the signature, because a
// stray FF E1 in encrypted bytes is otherwise easy to believe.
QByteArray findOwnExif(const QByteArray &jpeg, qsizetype limit)
{
    const auto *d = reinterpret_cast<const quint8 *>(jpeg.constData());
    const qsizetype n = qMin(limit, jpeg.size());
    for (qsizetype i = 0; i + 12 < n; ++i) {
        if (d[i] != 0xFF || d[i + 1] != 0xE1)
            continue;
        const int length = be16(d + i + 2);
        if (length < 14 || i + 2 + length > n)
            continue;
        const quint8 *payload = d + i + 4;
        if (std::memcmp(payload, "Exif\0\0", 6) != 0)
            continue;
        if (std::memcmp(payload + 6, "II*\0", 4) != 0
            && std::memcmp(payload + 6, "MM\0*", 4) != 0)
            continue;
        return jpeg.mid(i, 2 + length);
    }
    return QByteArray();
}

// In entropy-coded data every FF is either byte-stuffed (FF 00), a restart
// marker, padding, or the EOI that ends the scan. Any other pair cannot occur
// in a valid stream -- and turns up roughly every 250 bytes in encrypted or
// random data, where all but a handful of the 256 possible following bytes are
// illegal. So the last such pair is a tight upper bound on where damage ends.
bool legalAfterFf(quint8 next)
{
    return next == 0x00 || next == 0xFF || next == 0xD9 || (next >= 0xD0 && next <= 0xD7);
}

// Offset of the FF in the last pair before `end` that cannot occur inside a
// scan, or -1. `end` is where the image's own bytes stop: past it sit
// trailers -- an MPF preview, a motion photo's video, a ransomware footer --
// that are full of marker bytes and say nothing about where the damage ends.
qsizetype lastIllegalFf(const QByteArray &jpeg, qsizetype end)
{
    const auto *d = reinterpret_cast<const quint8 *>(jpeg.constData());
    for (qsizetype i = qMin(end, jpeg.size()) - 2; i >= 0; --i) {
        if (d[i] == 0xFF && !legalAfterFf(d[i + 1]))
            return i;
    }
    return -1;
}

// Offset of the first restart marker at or after `from`, or -1.
qsizetype firstRestart(const QByteArray &jpeg, qsizetype from)
{
    const auto *d = reinterpret_cast<const quint8 *>(jpeg.constData());
    const qsizetype n = jpeg.size();
    for (qsizetype i = qMax(qsizetype(0), from); i + 1 < n; ++i) {
        if (d[i] == 0xFF && d[i + 1] >= 0xD0 && d[i + 1] <= 0xD7)
            return i;
    }
    return -1;
}

// The prefix STOP/Djvu encrypts before leaving the rest of a file alone:
// 150 KiB. When it is the size of the damage, the first intact byte sits
// exactly on the boundary.
constexpr qsizetype kStopDjvuPrefix = 0x25800;

// Where the damaged file's own bytes end: before a trailer that its own
// signatures identify, or the whole file.
qsizetype imageBytesEnd(const QByteArray &broken)
{
    const jpegfile::Trailer t = jpegfile::findTrailer(broken, -1);
    return t.present() ? t.offset : broken.size();
}

constexpr int kMaxSplicePoints = 12;

} // namespace

// ---------------------------------------------------------------------------
// Reading a file's front matter
// ---------------------------------------------------------------------------

Layout scan(const QByteArray &jpeg)
{
    return walkMarkers(jpeg, [](const Segment &) {});
}

QString describe(const Layout &layout)
{
    if (!layout.problem.isEmpty())
        return tr("This file %1").arg(layout.problem);
    return tr("%1 × %2, %3 component(s), %4%5.")
        .arg(layout.width)
        .arg(layout.height)
        .arg(layout.numComponents)
        .arg(layout.progressive ? tr("progressive") : tr("baseline"),
             layout.restartInterval > 0
                 ? tr(", restart marker every %1 MCUs").arg(layout.restartInterval)
                 : tr(", no restart markers"));
}

QString donorProblem(const Layout &layout)
{
    if (!layout.usable())
        return layout.problem.isEmpty() ? tr("has no usable header.") : layout.problem;
    if (layout.progressive) {
        // A progressive file spreads its data over several scans, each behind
        // its own scan header, and those headers sit *between* the runs of
        // data. There is no single front-of-file header to lift.
        return tr("is progressive. A donor has to be a baseline JPEG, because a progressive "
                  "file's scan headers are interleaved with its data rather than sitting in "
                  "front of it.");
    }
    if (layout.numComponents != 3) {
        return tr("has %1 color component(s); this tool works on the usual three "
                  "(Y, Cb, Cr).")
            .arg(layout.numComponents);
    }
    return QString();
}

// ---------------------------------------------------------------------------
// Where the surviving data starts
// ---------------------------------------------------------------------------

QVector<SplicePoint> splicePoints(const QByteArray &broken, const Layout &brokenLayout,
                                  int restartInterval)
{
    QVector<SplicePoint> out;
    const qsizetype size = broken.size();

    const auto add = [&out, size](qsizetype offset, const QString &reason) {
        if (offset < 0 || offset >= size)
            return false;
        for (const SplicePoint &existing : out) {
            if (existing.offset == offset)
                return false;
        }
        out.append(SplicePoint{offset, reason, QString()});
        return true;
    };
    // Lists a resume point this file does not have, along with what is missing.
    const auto explain = [&out](const QString &reason, const QString &why) {
        out.append(SplicePoint{-1, reason, why});
    };

    // Keeping every byte is the honest first thing to try: it assumes nothing
    // about where the damage ends, and for a file that only lost its markers
    // -- a zeroed or overwritten front, a carved fragment -- it is the right
    // answer. The guesses below drop data, so they come after it.
    add(0, tr("At the very start of the file (nothing dropped)"));

    // For partial damage: the file still has marker segments of its own, but
    // something in them is beyond libjpeg -- a mangled table, a frame header
    // that disagrees with itself. Its data still starts where its own scan
    // header ends, so the donor supplies the tables and not a byte of the
    // picture is dropped. This is the one candidate that differs from byte 0
    // only when part of the header survived.
    if (brokenLayout.entropyStart > 0) {
        add(brokenLayout.entropyStart, tr("Right after this file's own scan header"));
    } else {
        explain(tr("Right after this file's own scan header"),
                brokenLayout.problem.isEmpty()
                    ? tr("for partial damage, where a file keeps its own header; unavailable "
                         "because no scan header survives here")
                    : tr("for partial damage, where a file keeps its own header; unavailable "
                         "because this file %1")
                          .arg(brokenLayout.problem));
    }

    const qsizetype end = imageBytesEnd(broken);
    qsizetype illegal = lastIllegalFf(broken, end);
    // An "illegal" pair inside the file's own surviving header is just one of
    // its markers.
    if (brokenLayout.entropyStart > 0 && illegal >= 0 && illegal < brokenLayout.entropyStart)
        illegal = -1;
    const qsizetype boundary = illegal >= 0 ? illegal + 2 : 0;

    // Restarts are the ideal place to come in: the encoder flushed to a byte
    // boundary and reset its DC predictors there, so data that follows one
    // decodes correctly without knowing anything about what came before. Which
    // interval it is cannot be known from the marker alone (they count modulo
    // 8), so the picture may still need shifting by whole intervals.
    const bool restartsExpected = restartInterval > 0 || brokenLayout.restartInterval > 0;
    bool foundRestart = false;
    if (restartsExpected) {
        qsizetype rst = firstRestart(broken, boundary);
        for (int i = 0; i < 3 && rst >= 0 && rst < end; ++i) {
            foundRestart = true;
            add(rst + 2, i == 0 ? tr("At the first restart marker past the damage (offset %1)")
                                      .arg(rst)
                                : tr("At the following restart marker (offset %1)").arg(rst));
            rst = firstRestart(broken, rst + 2);
        }
    }
    if (!foundRestart) {
        explain(tr("At a restart marker past the damage"),
                restartsExpected
                    ? tr("unavailable: no restart marker survives past the damage")
                    : tr("unavailable: this camera wrote no restart markers, so the stream has "
                         "no point where it resynchronizes on its own"));
    }

    if (illegal >= 0) {
        add(boundary, tr("Just past the last byte pair no valid JPEG could contain (offset %1)")
                          .arg(illegal));
    } else {
        explain(tr("Just past the last byte pair no valid JPEG could contain"),
                tr("unavailable: every FF in this file's picture data is followed by a byte "
                   "scan data is allowed to contain, so there is no boundary to find"));
    }

    const bool djvu = jpegfile::findStopDjvuFooter(broken).has_value();
    if (kStopDjvuPrefix < end) {
        const qsizetype rst = restartInterval > 0 ? firstRestart(broken, kStopDjvuPrefix) : -1;
        if (rst >= 0 && rst < end) {
            add(rst + 2, tr("At the first restart marker after 150 KiB, where STOP/Djvu stops "
                            "encrypting (offset %1)")
                             .arg(rst));
        }
        add(kStopDjvuPrefix, djvu ? tr("At 150 KiB: this file carries the STOP/Djvu footer, and "
                                       "STOP/Djvu encrypts exactly the first 150 KiB")
                                  : tr("At 150 KiB, the prefix STOP/Djvu ransomware encrypts"));
    }

    if (out.size() > kMaxSplicePoints)
        out.resize(kMaxSplicePoints);
    return out;
}

// ---------------------------------------------------------------------------
// Building the transplant
// ---------------------------------------------------------------------------

namespace {

// One table out of a DQT or DHT segment, keyed by what identifies it (the
// table slot, and for DHT its class), with the bytes that define it.
using Tables = QMap<int, QByteArray>;

void collectTables(const QByteArray &jpeg, const Segment &seg, Tables *dqt, Tables *dht)
{
    const auto *p = reinterpret_cast<const quint8 *>(jpeg.constData()) + seg.dataStart;
    qsizetype i = 0;
    if (seg.marker == 0xDB) {
        while (i < seg.dataLength) {
            const int precision = p[i] >> 4, id = p[i] & 15;
            const qsizetype size = 1 + (precision ? 128 : 64);
            if (id > 3 || i + size > seg.dataLength)
                return;
            dqt->insert(id, QByteArray(reinterpret_cast<const char *>(p + i), size));
            i += size;
        }
    } else if (seg.marker == 0xC4) {
        while (i + 17 <= seg.dataLength) {
            int count = 0;
            for (int l = 1; l <= 16; ++l)
                count += p[i + l];
            const qsizetype size = 17 + count;
            if ((p[i] & 15) > 3 || (p[i] >> 4) > 1 || count > 256 || i + size > seg.dataLength)
                return;
            dht->insert(p[i], QByteArray(reinterpret_cast<const char *>(p + i), size));
            i += size;
        }
    }
}

QByteArray segmentOf(quint8 marker, const QByteArray &payload)
{
    QByteArray out;
    out.append(char(0xFF));
    out.append(char(marker));
    const int length = int(payload.size()) + 2;
    out.append(char(length >> 8));
    out.append(char(length & 0xFF));
    out.append(payload);
    return out;
}

// The header parts a file carries, as far as it can be read.
struct HeaderParts {
    QByteArrayList appSegments; // JFIF, ICC, Adobe: kept from the donor
    Tables dqt, dht;
    QByteArray sof, dri, sos;   // whole segments
};

HeaderParts partsOf(const QByteArray &jpeg, bool donorSide)
{
    HeaderParts parts;
    walkMarkers(jpeg, [&](const Segment &seg) {
        if (seg.marker == 0xD8)
            return;
        const QByteArray whole = jpeg.mid(seg.start, seg.length);
        if (seg.marker == 0xDB || seg.marker == 0xC4) {
            collectTables(jpeg, seg, &parts.dqt, &parts.dht);
        } else if (isSof(seg.marker)) {
            if (parts.sof.isEmpty())
                parts.sof = whole;
        } else if (seg.marker == 0xDD) {
            parts.dri = whole;
        } else if (seg.marker == 0xDA) {
            parts.sos = whole;
        } else if (donorSide && !isIdentityMarker(jpeg, seg) && seg.marker >= 0xE0 && seg.marker <= 0xEF) {
            parts.appSegments.append(whole);
        }
    });
    return parts;
}

// Component ids a frame header declares, and the ones a scan header names.
QVector<int> sofComponents(const QByteArray &sof)
{
    QVector<int> ids;
    if (sof.size() < 10)
        return ids;
    const int n = quint8(sof[9]);
    for (int c = 0; c < n && 10 + 3 * c < sof.size(); ++c)
        ids.append(quint8(sof[10 + 3 * c]));
    return ids;
}

QVector<int> sosComponents(const QByteArray &sos)
{
    QVector<int> ids;
    if (sos.size() < 5)
        return ids;
    const int n = quint8(sos[4]);
    for (int c = 0; c < n && 5 + 2 * c < sos.size(); ++c)
        ids.append(quint8(sos[5 + 2 * c]));
    return ids;
}

// Renumbers RSTn markers in `data` (entropy-coded bytes) so the first is
// RST0, keeping the gaps between them. Returns how many it rewrote.
int renumberRestarts(QByteArray &data, qsizetype from, qsizetype to)
{
    auto *d = reinterpret_cast<quint8 *>(data.data());
    int first = -1, changed = 0;
    for (qsizetype i = from; i + 1 < to; ++i) {
        if (d[i] != 0xFF)
            continue;
        const quint8 next = d[i + 1];
        if (next == 0xFF)
            continue; // fill byte; the marker, if any, follows
        if (next >= 0xD0 && next <= 0xD7) {
            if (first < 0)
                first = next - 0xD0;
            const quint8 renumbered = quint8(0xD0 + ((next - 0xD0 - first + 8) & 7));
            if (renumbered != next) {
                d[i + 1] = renumbered;
                ++changed;
            }
        }
        ++i;
    }
    return changed;
}

} // namespace

std::optional<Splice> splice(const QByteArray &donorBytes, const Layout &donorLayout,
                             const QByteArray &broken, qsizetype offset, QString *error,
                             const SpliceOptions &options)
{
    const QString problem = donorProblem(donorLayout);
    if (!problem.isEmpty()) {
        if (error)
            *error = tr("The donor %1").arg(problem);
        return std::nullopt;
    }

    // A ransomware footer is not picture data, and left in place it would be
    // decoded as some.
    qsizetype end = broken.size();
    qsizetype droppedFooter = 0;
    if (const auto footer = jpegfile::findStopDjvuFooter(broken)) {
        end = footer->offset;
        droppedFooter = footer->length;
    }
    if (offset < 0 || offset >= end) {
        if (error) {
            *error = tr("A splice point of %1 leaves none of the damaged file's data behind.")
                         .arg(offset);
        }
        return std::nullopt;
    }

    Splice out;
    out.droppedFooter = droppedFooter;
    const HeaderParts donorParts = partsOf(donorBytes, true);
    HeaderParts own;
    if (options.keepOwnTables)
        own = partsOf(broken.left(offset > 0 ? offset : 0), false);

    // Tables: the file's own where it still has them, slot by slot.
    Tables dqt = donorParts.dqt, dht = donorParts.dht;
    for (auto it = own.dqt.constBegin(); it != own.dqt.constEnd(); ++it) {
        dqt.insert(it.key(), it.value());
        out.keptOwn << QStringLiteral("DQT %1").arg(it.key());
    }
    for (auto it = own.dht.constBegin(); it != own.dht.constEnd(); ++it) {
        dht.insert(it.key(), it.value());
        out.keptOwn << QStringLiteral("DHT %1%2").arg(it.key() >> 4 ? "AC" : "DC").arg(it.key() & 15);
    }

    // Frame and scan header travel as a pair: the scan names the frame's
    // components. Take the file's own only if both survived and agree.
    QByteArray sof = donorParts.sof, sos = donorParts.sos, dri = donorParts.dri;
    if (!own.sof.isEmpty() && !own.sos.isEmpty() && sofComponents(own.sof).size() == 3) {
        const QVector<int> frameIds = sofComponents(own.sof);
        bool agree = true;
        for (int id : sosComponents(own.sos))
            agree = agree && frameIds.contains(id);
        if (agree) {
            sof = own.sof;
            sos = own.sos;
            out.keptOwn << QStringLiteral("SOF") << QStringLiteral("SOS");
        }
    }
    if (!own.dri.isEmpty()) {
        dri = own.dri;
        out.keptOwn << QStringLiteral("DRI");
    }
    if (options.restartInterval >= 0) {
        dri = options.restartInterval > 0
            ? segmentOf(0xDD, QByteArray::fromRawData("\0\0", 2))
            : QByteArray();
        if (!dri.isEmpty()) {
            dri[4] = char(options.restartInterval >> 8);
            dri[5] = char(options.restartInterval & 0xFF);
        }
    }
    if (sof.size() >= 9) {
        if (options.height > 0) {
            sof[5] = char(options.height >> 8);
            sof[6] = char(options.height & 0xFF);
        }
        if (options.width > 0) {
            sof[7] = char(options.width >> 8);
            sof[8] = char(options.width & 0xFF);
        }
    }

    QByteArray header;
    header.reserve(donorLayout.entropyStart + 64);
    header.append("\xFF\xD8", 2);
    const QByteArray ownExif = findOwnExif(broken, offset);
    header.append(ownExif); // Exif belongs first, and the SOI is all that precedes it
    for (const QByteArray &app : donorParts.appSegments)
        header.append(app);
    QByteArray q;
    for (const QByteArray &t : std::as_const(dqt))
        q.append(t);
    if (!q.isEmpty())
        header.append(segmentOf(0xDB, q));
    header.append(sof);
    QByteArray h;
    for (const QByteArray &t : std::as_const(dht))
        h.append(t);
    if (!h.isEmpty())
        header.append(segmentOf(0xC4, h));
    header.append(dri);
    header.append(sos);

    out.headerSize = header.size();
    out.carriedExif = !ownExif.isEmpty();
    out.bytes = std::move(header);
    out.bytes.append(broken.constData() + offset, end - offset);
    if (options.renumberRestarts)
        out.renumberedRestarts = renumberRestarts(out.bytes, out.headerSize, out.bytes.size());
    // A stream that just stops makes libjpeg complain about a premature end
    // and fill the rest gray, which is fine, but it should still be told where
    // the data ran out.
    if (!out.bytes.endsWith(QByteArray("\xFF\xD9", 2)))
        out.bytes.append("\xFF\xD9", 2);
    return out;
}

} // namespace donor
