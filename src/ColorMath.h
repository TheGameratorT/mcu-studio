// SPDX-License-Identifier: GPL-3.0-only
// Part of MCU Studio. See LICENSE and THIRD-PARTY-NOTICES.md.

// Conversions between what the user sees (a shift in 0-255 sample terms) and
// what jpegrepair takes (a delta on the quantized DC coefficient).
#pragma once

#include <QColor>
#include <QRect>
#include <QString>

#include "JpegRepair.h"

namespace colormath {

// jpegrepair's `cdelta COMP dC` adds dC to the *quantized* DC coefficient, so
// the dequantized DC moves by dC * Q00. In the JPEG DCT normalization the DC
// term is 8x the block's mean sample, so every sample in the block moves by
// dC * Q00 / 8. Q00 comes from the image's own DQT and differs per component
// and per quality -- a flat dC/8 is only right for the hypothetical Q00 == 1.
double pixelShiftForCdelta(int dcQuant, int cdelta);

// The inverse, rounded to the nearest whole coefficient step and clamped to
// the range a JCOEF can carry.
int cdeltaForPixelShift(int dcQuant, double pixelShift);

constexpr int kCdeltaLimit = 2047;

struct BlockStats {
    double mean[3] = {0, 0, 0};
    // Fraction of samples pinned at 0 or 255. The decoder clips out-of-range
    // samples, so their real values are unknown and only known to lie further
    // out; a mean over clipped samples is pulled toward the middle and any
    // correction derived from it falls short.
    double clipped[3] = {0, 0, 0};
    qint64 sampleCount = 0;

    bool isValid() const { return sampleCount > 0; }
};

// Mean and clipping over a pixel rectangle, intersected with the image.
BlockStats measure(const jr::Samples &samples, QRect pixelRect);

// The pixel rectangle covered by an MCU, clipped to the image bounds.
QRect mcuRect(const jr::Info &info, int row, int col);

// The colour a measured YCbCr mean stands for, so a patch's average can be
// shown as a swatch instead of three numbers. This is JFIF's full-range
// conversion, the one libjpeg's decoder applies.
QColor rgbForYCbCr(const double ycbcr[3]);

// The forward conversion, for a reference picture that did not come out of a
// JPEG decoder: Qt hands back RGB whatever the file format was, while the match
// arithmetic works in the samples a JPEG actually stores.
jr::Samples ycbcrFromRgb(const jr::Samples &rgb);

// The channels, in the order every triple here is written in.
extern const char *const channelNames[3];

// "Y 128.0   Cb 127.4   Cr 130.2" -- how every measured triple is shown, so
// that two of them can be read against each other at a glance.
QString formatTriple(const double v[3], int decimals = 1);

} // namespace colormath