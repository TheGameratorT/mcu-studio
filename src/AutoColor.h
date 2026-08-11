// SPDX-License-Identifier: GPL-3.0-only
// Part of MCU Studio. See LICENSE and THIRD-PARTY-NOTICES.md.

// PhotoDemon's two automatic corrections, ported from the Python tool's
// photodemon_autocorrect_image(): a per-channel histogram stretch followed by
// a midtone-contrast ("clarity") curve.
//
// Unlike everything else here this is a pixel-domain edit -- it cannot be
// expressed as a coefficient delta, so applying it re-encodes the image.
#pragma once

#include "JpegRepair.h"

namespace autocolor {

// Quality the re-encode uses. High enough that the one lossy step in the tool
// costs little, low enough that it does not bloat the file.
constexpr int kQuality = 95;

// White balance: stretch each channel so the given percentile tails land on
// 0 and 255. 0.05 matches PhotoDemon's default.
jr::Samples whiteBalance(const jr::Samples &rgb, double clipPercent = 0.05);

// Midtone contrast, strongest around mid-gray and tapering to nothing at both
// ends so highlights and shadows keep their detail.
jr::Samples clarity(const jr::Samples &rgb, double strength = 0.4);

// whiteBalance() then clarity(), which is what the old "Auto Color" button did.
jr::Samples autoCorrect(const jr::Samples &rgb);

} // namespace autocolor