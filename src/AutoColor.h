// SPDX-License-Identifier: GPL-3.0-only
// Part of MCU Studio. See LICENSE and THIRD-PARTY-NOTICES.md.

// Two automatic corrections, ported from the Python tool's
// photodemon_autocorrect_image(): a per-channel levels stretch followed by a
// global midtone-contrast curve. (PhotoDemon calls the curve "clarity"; in
// Lightroom and PhotoDemon's own UI, clarity means *local* contrast, which this
// is not, so it goes by what it does here.)
//
// Unlike everything else here this is a pixel-domain edit -- it cannot be
// expressed as a coefficient delta, so applying it re-quantizes every block
// (with the file's own tables; see ImageDocument::autoColorPayload).
#pragma once

#include "JpegRepair.h"

namespace autocolor {

// Auto levels per channel: stretch each channel so the given percentile tails
// land on 0 and 255. Stretching the channels independently neutralizes a
// color cast, which is why it is often called white balance, but it also moves
// the black point, which white balance proper does not. 0.05 matches
// PhotoDemon's default.
jr::Samples autoLevels(const jr::Samples &rgb, double clipPercent = 0.05);

// A global midtone-contrast curve, strongest around mid-gray and tapering to
// nothing at both ends so highlights and shadows keep their detail.
jr::Samples midtoneContrast(const jr::Samples &rgb, double strength = 0.4);

// autoLevels() then midtoneContrast(), which is what the "Auto color" button
// does.
jr::Samples autoCorrect(const jr::Samples &rgb);

} // namespace autocolor