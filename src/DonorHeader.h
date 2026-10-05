// SPDX-License-Identifier: GPL-3.0-only
// Part of MCU Studio. See LICENSE and THIRD-PARTY-NOTICES.md.

// Opening a JPEG whose header is gone, by borrowing one from another shot.
//
// Ransomware, a truncated copy, and a bad sector at the front of a file all
// destroy the same thing: the markers before the scan. Those markers -- the
// quantization and Huffman tables, the frame header, the scan header -- are
// what a decoder needs to make sense of the entropy-coded data, and none of
// them can be inferred from the data itself. Without them libjpeg cannot open
// the file at all, so none of this tool's repairs can even be attempted.
//
// They are also nearly identical across a camera roll. A camera writes the
// same tables for every frame it shoots at the same settings, so a sibling
// photograph -- same camera, same quality, same resolution -- carries a header
// that fits. This module splices that donor header onto the damaged file's
// surviving entropy data and hands back a JPEG that decodes.
//
// The result is not the original file: the picture will usually be shifted,
// because the number of MCUs lost with the header is unknown. That is what the
// rest of the tool is for -- insert and delete blocks to slide the stream back
// into place, cdelta to pull the color back. This module's job is only to get
// the door open.
#pragma once

#include <QByteArray>
#include <QString>
#include <QStringList>
#include <QVector>

#include <optional>

namespace donor {

// What a walk of a file's marker segments found, stopping at the first scan.
struct Layout {
    bool hasSoi = false;
    qsizetype sofOffset = -1;
    qsizetype sosOffset = -1;
    // First byte after the scan header: where the entropy-coded data begins.
    qsizetype entropyStart = -1;
    int width = 0, height = 0;
    int numComponents = 0;
    int restartInterval = 0; // 0 when the encoder wrote no restart markers
    bool progressive = false;
    // Why the walk stopped short, in a sentence that completes "This file ...".
    // Empty when it reached the scan header.
    QString problem;

    bool usable() const { return hasSoi && sofOffset >= 0 && entropyStart > 0; }
};

Layout scan(const QByteArray &jpeg);

// One line for the UI: either the problem, or what the header says.
QString describe(const Layout &layout);

// Why `layout` cannot serve as a donor, or an empty string if it can.
QString donorProblem(const Layout &layout);

// A place the damaged file's entropy-coded data might resume.
//
// The offsets are guesses, and meant to be judged by eye -- splicing is cheap
// and the preview tells you at once whether one was right. The first is always
// byte 0, which drops nothing and assumes nothing; the rest give up a prefix
// in exchange for a guess about where the damage ended.
struct SplicePoint {
    qsizetype offset = 0;
    QString reason; // shown in the chooser
    // Filled in when this resume point does not exist in this file. The entry
    // is still listed, grayed out and carrying the explanation, because a
    // choice that silently disappears reads as a tool with fewer ideas than it
    // has -- "there is no scan header left to start after" is the useful thing
    // to know.
    QString unavailable;

    bool isAvailable() const { return unavailable.isEmpty(); }
};

// Candidates for where `broken`'s surviving data starts, byte 0 first.
// `restartInterval` comes from the donor, and only decides whether restart
// markers are worth hunting for.
QVector<SplicePoint> splicePoints(const QByteArray &broken, const Layout &brokenLayout,
                                  int restartInterval);

// How the header is assembled.
struct SpliceOptions {
    // Where the damaged file's own header survives in part, keep its
    // quantization and Huffman tables (table by table), frame header, restart
    // interval and scan header, and borrow from the donor only what is
    // missing. Its own tables are the ones its data was coded with.
    bool keepOwnTables = true;
    // Renumber the restart markers in the spliced data so the first one is
    // RST0. libjpeg expects RST0 first and treats any other as a lost or
    // repeated interval -- inserting or skipping a whole interval of the
    // picture -- so data cut in at RST3 would land an interval off.
    bool renumberRestarts = true;
    // Frame size to write instead of the donor's, 0 to keep it. A portrait
    // shot and a landscape one from the same camera share everything but this.
    int width = 0, height = 0;
    // Restart interval to declare instead of the header's: -1 keeps it, 0
    // declares none.
    int restartInterval = -1;

    bool operator==(const SpliceOptions &o) const
    {
        return keepOwnTables == o.keepOwnTables && renumberRestarts == o.renumberRestarts
            && width == o.width && height == o.height && restartInterval == o.restartInterval;
    }
};

struct Splice {
    QByteArray bytes;
    qsizetype headerSize = 0; // bytes of assembled header
    bool carriedExif = false; // the damaged file's own Exif survived and was kept
    // What came from the damaged file's own header rather than the donor's,
    // for the dialog to say ("DQT 0, DQT 1, SOF"). Empty when it was all the
    // donor's.
    QStringList keptOwn;
    int renumberedRestarts = 0;
    // Bytes of ransomware footer dropped from the end of the damaged file.
    qsizetype droppedFooter = 0;
};

// Donor header + `broken` from `offset` on. Fails if the donor is unusable or
// the offset leaves nothing behind.
std::optional<Splice> splice(const QByteArray &donorBytes, const Layout &donorLayout,
                             const QByteArray &broken, qsizetype offset, QString *error,
                             const SpliceOptions &options = {});

} // namespace donor
