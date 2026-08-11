// SPDX-License-Identifier: GPL-3.0-only
// Part of MCU Studio. See LICENSE and THIRD-PARTY-NOTICES.md.

// Filling damaged blocks with content from another picture.
//
// Every other repair in this tool moves coefficients that are already in the
// file: a shift, a copy from elsewhere in the same scan, a constant added to a
// DC term. That works while the picture data survives somewhere. When it does
// not -- an overwritten stretch, a hole a truncation left, blocks that decode
// to nothing recognizable -- there is nothing inside the file to move.
//
// A second copy of the photograph answers it, and unlike the color reference
// this one has to be a copy of the *content*, not just of the colors: a
// re-render, an export, a backup, a frame pulled out of a video, a PNG a
// previous recovery attempt produced. Those pixels have never been through a
// DCT, so they cannot be transplanted the way another JPEG's MCUs can. They
// have to be compressed to enter the file at all.
//
// What this module does is confine that compression to the blocks being
// replaced. The patch is quantized with the damaged file's own tables and
// sampling factors (see jr::quantizePatch), so the blocks handed over are the
// ones the file would have held had it been shot with this content, and they
// are written into the coefficient array beside their neighbors -- which are
// not read, not transformed, and not re-encoded. One generation, on the new
// data only.
#pragma once

#include <QByteArray>
#include <QImage>
#include <QPoint>
#include <QRect>
#include <QSize>
#include <QString>

#include <optional>

#include "JpegRepair.h"

namespace fill {

// The reference laid over the image being repaired.
//
// Scaled to the damaged file's dimensions when it does not already match --
// color survives resampling, and so does enough detail to be worth having,
// but a copy at the original size is always the better one -- and then
// extended into the padding that the last MCU row and column cover past the
// picture's edge, which the encoder needs and the picture does not have.
struct Aligned {
    QImage image;     // exactly mcusX * mcuWidth by mcusY * mcuHeight, RGB888
    QSize sourceSize; // what the reference measured before any scaling
    bool scaled = false;
    // The two are not the same shape, so scaling to fit stretched one axis
    // against the other. Worth saying: it usually means the copy was cropped,
    // and cropped content will not line up block for block.
    bool aspectChanged = false;

    bool isValid() const { return !image.isNull(); }
};

std::optional<Aligned> align(const QImage &reference, const jr::Info &dest,
                             QString *error = nullptr);

// Reference pixels for one MCU-aligned rectangle of the image being repaired,
// read `mcuOffset` MCUs away from where that rectangle sits. Reads outside the
// reference replicate its edge rather than failing, so an offset that hangs off
// the side still produces a patch -- a visibly wrong one, which is the right
// way for it to be wrong.
jr::Samples patchFor(const Aligned &reference, const jr::Info &dest, QRect mcuRect,
                     QPoint mcuOffset);

// What a fill is going to touch, worked out before anything is compressed so
// the dialog can describe it and refuse an empty one.
struct Plan {
    QRect mcuRect;    // bounding box of the selected MCUs
    int mcuCount = 0; // how many of them are actually selected

    bool isValid() const { return mcuCount > 0 && !mcuRect.isEmpty(); }
};

// `mask` is dest.mcusY * dest.mcusX bytes, row-major, non-zero where selected.
Plan planFor(const QByteArray &mask, const jr::Info &dest);

// Compresses the reference content under `plan` and returns the step that
// writes it into exactly the masked blocks.
//
// `destJpeg` is the image being repaired as it currently stands; only its
// header is read, for the quantization tables the patch has to be written in.
std::optional<jr::Op> build(const QByteArray &destJpeg, const jr::Info &dest,
                            const Aligned &reference, const QByteArray &mask, const Plan &plan,
                            QPoint mcuOffset, QString *error = nullptr);

} // namespace fill
