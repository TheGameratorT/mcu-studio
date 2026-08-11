// SPDX-License-Identifier: GPL-3.0-only
// Part of MCU Studio. See LICENSE and THIRD-PARTY-NOTICES.md.

// Borrowing a color reference from another copy of the same picture.
//
// Color matching needs one block of known-good color to measure the damage
// against, and until now that block had to come from the image being repaired.
// A donor header takes that away: the transplant splices the surviving entropy
// data onto a header that never described it, so the DC predictors start out
// wrong and the very first MCU can already be the wrong color. There is then
// no good block anywhere in the file to point at.
//
// A surviving small copy answers it. Camera roll backups, an Exif thumbnail
// pulled out elsewhere, a phone gallery cache, a messaging app's downscaled
// copy -- any of them shows what the colors were, and color is exactly what
// survives downscaling. The picture is measured, never spliced: nothing from
// this file reaches the output, only three numbers.
//
// Samples are held as YCbCr because that is what the match arithmetic and the
// DC coefficients work in, and because YCbCr means are absolute -- comparing
// them across two files needs no shared quantization, resolution or quality.
#pragma once

#include <QImage>
#include <QRect>
#include <QSize>
#include <QString>

#include <optional>

#include "JpegRepair.h"

namespace refimage {

struct Loaded {
    jr::Samples ycbcr; // what the match arithmetic reads
    QImage preview;    // what the picker draws
    // The picture carries no chroma of its own, so matching against it would
    // not correct the damaged file's color but flatten it to neutral. Worth
    // saying out loud, because a grayscale copy of a color photograph looks
    // like a perfectly good reference.
    bool monochrome = false;

    bool isValid() const { return ycbcr.isValid() && !preview.isNull(); }
    QSize size() const { return QSize(ycbcr.width, ycbcr.height); }
};

// Reads `path`. JPEGs go through libjpeg, so their samples arrive exactly as
// the file stored them, with no clipping to the RGB gamut; any other format Qt
// can read is loaded as RGB and converted.
std::optional<Loaded> load(const QString &path, QString *error = nullptr);

// Where `rect` -- a region of a `subjectSize` picture -- falls in a
// `referenceSize` copy of the same picture.
//
// Only a starting guess, and offered as one: after a header transplant the
// stream is shifted by an unknown number of MCUs, so the block at a given
// position is showing content from somewhere else entirely until insert and
// delete have put it back. The patch is meant to be dragged onto matching
// content by eye; this just saves the first drag when nothing has shifted.
//
// Never returns an edge shorter than `minEdge`: a thumbnail can shrink an MCU
// to less than a pixel, and a mean over no samples is no reference at all.
QRect correspondingRect(QRect rect, QSize subjectSize, QSize referenceSize, int minEdge = 2);

// `rect` moved so that it is centerd on `center` and lies inside `bounds`,
// keeping its size. Shared by the picker's click handling and by
// correspondingRect().
QRect centerdIn(QRect rect, QPoint center, QSize bounds);

} // namespace refimage
