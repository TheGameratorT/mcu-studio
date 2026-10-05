// SPDX-License-Identifier: GPL-3.0-only
// Part of MCU Studio. See LICENSE and THIRD-PARTY-NOTICES.md.

// What a JPEG file holds, byte by byte, without decoding any of it.
//
// libjpeg reads a file from the front and stops at the first End Of Image
// marker, which is the right behavior for a viewer and hides most of what a
// repair tool needs to know: how many scans there are and where each one's
// data runs, where the primary image ends, and what sits after it. Plenty of
// healthy files carry data past that EOI -- a second, larger preview indexed
// by an MPF segment, a Motion Photo's video, an editor's trailer -- and a
// ransomware-encrypted file carries the ransomware's own footer there. Treating
// any of those as picture data, or as damage, gives wrong answers; this module
// tells them apart.
#pragma once

#include <QByteArray>
#include <QString>
#include <QVector>

#include <optional>

namespace jpegfile {

// One marker segment, from its FF to the end of its payload.
struct Segment {
    quint8 marker = 0;
    qsizetype offset = 0;     // the marker's FF
    qsizetype length = 0;     // marker + length field + payload
    qsizetype dataOffset = -1; // first payload byte, -1 for a standalone marker
    qsizetype dataLength = 0;
};

// One scan: its header and the entropy-coded data behind it.
struct Scan {
    qsizetype sosOffset = 0;
    qsizetype dataStart = 0;
    qsizetype dataEnd = 0; // the FF of whatever marker ends the scan
    int componentCount = 0;
    int componentIds[4] = {0, 0, 0, 0};
    int ss = 0, se = 63, ah = 0, al = 0; // spectral selection, successive approximation
    int restartMarkers = 0;
    // Where the data stops being something an encoder could have written, -1
    // if it never does. The walk stops there.
    qsizetype firstIllegal = -1;

    bool damaged() const { return firstIllegal >= 0; }
    qsizetype dataLength() const { return dataEnd - dataStart; }
    // "DC, all components", "AC 1-5 Y, refining bit 0", ...
    QString describe() const;
};

struct Structure {
    QVector<Segment> segments; // everything but entropy-coded data, in file order
    QVector<Scan> scans;
    qsizetype frameOffset = -1; // the SOFn segment
    quint8 frameMarker = 0;
    int width = 0, height = 0, components = 0;
    int restartInterval = 0;
    // Just past the EOI that ends the primary image; -1 when the walk never got
    // there, which is the normal state of a damaged file.
    qsizetype imageEnd = -1;
    // Why the walk stopped short of an EOI, empty if it did not.
    QString problem;

    bool progressive() const { return frameMarker == 0xC2 || frameMarker == 0xC6 || frameMarker == 0xCA; }
    const Segment *find(quint8 marker, const char *signature = nullptr, int signatureLength = 0,
                        const QByteArray *bytes = nullptr) const;
};

// Walks every marker segment and scan of the primary image.
Structure walk(const QByteArray &jpeg);

// ---------------------------------------------------------------------------
// After the image
// ---------------------------------------------------------------------------

enum class TrailerKind {
    None,
    Mpf,          // further images indexed by the primary's MPF (APP2) segment
    MotionPhoto,  // an MP4 appended by a phone camera's motion photo mode
    EmbeddedJpeg, // another JPEG, not indexed by anything this tool reads
    StopDjvu,     // the footer STOP/Djvu ransomware appends to what it encrypts
    Unknown,
};

struct Trailer {
    TrailerKind kind = TrailerKind::None;
    qsizetype offset = -1; // first byte past the primary image
    qsizetype length = 0;
    // STOP/Djvu only: the victim's personal ID as the ransomware wrote it.
    QString personalId;
    // STOP/Djvu only: whether the ID has the shape of an offline ID, the kind
    // encrypted with a key the ransomware carries rather than one fetched from
    // its server. Offline keys have been recovered for many variants, so this
    // is the case where a decryptor may restore the file outright.
    bool offlineIdLikely = false;

    bool present() const { return kind != TrailerKind::None && length > 0; }
    // Whether exporting a repair should carry the trailer along. Everything
    // that belongs to the photograph is kept; the ransomware's footer is not.
    bool keepOnExport() const { return present() && kind != TrailerKind::StopDjvu; }
    QString describe() const;
};

// What follows the primary image. `imageEnd` comes from walk(); pass -1 for
// a file too damaged to walk, and the trailer is found by its own signatures
// instead (an EOI followed directly by another SOI or an MP4 box, or the
// STOP/Djvu footer).
Trailer findTrailer(const QByteArray &bytes, qsizetype imageEnd);

// The STOP/Djvu footer at the end of `bytes`, if there is one.
std::optional<Trailer> findStopDjvuFooter(const QByteArray &bytes);

// Rewrites the MPF index in `mainImage` -- a primary image about to be written
// with a trailer appended directly after it -- so that its entries still
// point at the images inside that trailer. `original` is the file the trailer
// was taken from and `originalTrailerStart` where it began there. Returns
// false, leaving `mainImage` alone, when there is no MPF index to fix.
bool fixMpfOffsets(QByteArray &mainImage, const QByteArray &original, qsizetype originalTrailerStart);

// ---------------------------------------------------------------------------
// Other pictures inside a file
// ---------------------------------------------------------------------------

struct Embedded {
    qsizetype offset = 0;
    qsizetype length = 0;
    QString origin; // "Exif thumbnail", "MPF image 2", "embedded JPEG", ...
    int width = 0, height = 0;
};

// Every other complete JPEG inside `bytes`: the Exif thumbnail, MPF images,
// a camera's preview in its maker notes, a JPEG appended after the EOI. The
// primary image itself is not listed. Each candidate is checked by walking it.
QVector<Embedded> embeddedJpegs(const QByteArray &bytes);

// Every JPEG that can be walked from an SOI to an EOI anywhere in `bytes`,
// largest first: for disk images, RAW files (whose full-size preview is
// usually a JPEG), and anything else that carries JPEGs without indexing them.
// Candidates nested inside a larger one (its thumbnail) are skipped.
QVector<Embedded> carve(const QByteArray &bytes, int maxCount = 1000, qsizetype minLength = 1024);

// Width and height from a JPEG's frame header, or 0x0.
void frameSize(const QByteArray &jpeg, int *width, int *height);

} // namespace jpegfile
