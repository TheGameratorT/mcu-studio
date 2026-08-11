// SPDX-License-Identifier: GPL-3.0-only
// Part of MCU Studio. See LICENSE and THIRD-PARTY-NOTICES.md.

#include "DonorHeader.h"

#include <QCoreApplication>

#include <cstring>

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
// colour transform flag, and the ICC profile -- which for a camera roll is the
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

// Offset of the FF in the last pair that cannot occur inside a scan, or -1.
qsizetype lastIllegalFf(const QByteArray &jpeg)
{
    const auto *d = reinterpret_cast<const quint8 *>(jpeg.constData());
    for (qsizetype i = jpeg.size() - 2; i >= 0; --i) {
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

// Prefix lengths that ransomware families are commonly reported to encrypt
// before leaving the rest of a file alone -- STOP/DJVU and its relatives. When
// one of them is the size of the damage, the first intact byte sits exactly on
// the boundary.
constexpr qsizetype kKnownEncryptedPrefixes[] = {0x25800, 0x9C000}; // 150 KiB, 624 KiB

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

    const qsizetype illegal = lastIllegalFf(broken);
    const qsizetype boundary = illegal >= 0 ? illegal + 2 : 0;

    // Restarts are the ideal place to come in: the encoder flushed to a byte
    // boundary and reset its DC predictors there, so data that follows one
    // decodes correctly without knowing anything about what came before.
    const bool restartsExpected = restartInterval > 0 || brokenLayout.restartInterval > 0;
    bool foundRestart = false;
    if (restartsExpected) {
        qsizetype rst = firstRestart(broken, boundary);
        for (int i = 0; i < 3 && rst >= 0; ++i) {
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
                tr("unavailable: every FF in this file is followed by a byte that scan data is "
                   "allowed to contain, so there is no boundary to find"));
    }

    for (qsizetype prefix : kKnownEncryptedPrefixes) {
        if (prefix >= size)
            continue;
        const qsizetype rst = restartInterval > 0 ? firstRestart(broken, prefix) : -1;
        if (rst >= 0) {
            add(rst + 2, tr("At the first restart marker after %1 KiB, a size ransomware often "
                            "encrypts (offset %2)")
                             .arg(prefix / 1024)
                             .arg(rst));
        }
        add(prefix, tr("At %1 KiB, a size ransomware often encrypts").arg(prefix / 1024));
    }

    if (out.size() > kMaxSplicePoints)
        out.resize(kMaxSplicePoints);
    return out;
}

// ---------------------------------------------------------------------------
// Building the transplant
// ---------------------------------------------------------------------------

std::optional<Splice> splice(const QByteArray &donorBytes, const Layout &donorLayout,
                             const QByteArray &broken, qsizetype offset, QString *error)
{
    const QString problem = donorProblem(donorLayout);
    if (!problem.isEmpty()) {
        if (error)
            *error = tr("The donor %1").arg(problem);
        return std::nullopt;
    }
    if (offset < 0 || offset >= broken.size()) {
        if (error) {
            *error = tr("A splice point of %1 leaves none of the damaged file's data behind.")
                         .arg(offset);
        }
        return std::nullopt;
    }

    QByteArray header;
    header.reserve(donorLayout.entropyStart + 64);
    header.append("\xFF\xD8", 2);

    const QByteArray ownExif = findOwnExif(broken, offset);
    header.append(ownExif); // Exif belongs first, and the SOI is all that precedes it

    walkMarkers(donorBytes, [&](const Segment &seg) {
        if (seg.marker == 0xD8) // already written
            return;
        if (isIdentityMarker(donorBytes, seg))
            return;
        header.append(donorBytes.constData() + seg.start, seg.length);
    });

    Splice out;
    out.headerSize = header.size();
    out.carriedExif = !ownExif.isEmpty();
    out.bytes = std::move(header);
    out.bytes.append(broken.constData() + offset, broken.size() - offset);
    // A stream that just stops makes libjpeg complain about a premature end
    // and fill the rest grey, which is fine, but it should still be told where
    // the data ran out.
    if (!out.bytes.endsWith(QByteArray("\xFF\xD9", 2)))
        out.bytes.append("\xFF\xD9", 2);
    return out;
}

} // namespace donor
