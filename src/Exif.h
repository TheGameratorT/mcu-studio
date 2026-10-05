// SPDX-License-Identifier: GPL-3.0-only
// Part of MCU Studio. See LICENSE and THIRD-PARTY-NOTICES.md.

// The few Exif edits a repaired photograph needs.
//
// A repair carries the original's Exif across, which is right for the date,
// the camera and the GPS fix, and wrong in two places: the embedded thumbnail
// still shows the picture as it was before the repair (or, for a donor
// transplant, whatever the damaged file's thumbnail held), and the pixel
// dimensions the Exif records can disagree with a frame header that was
// edited. Everything else is left exactly as it was.
#pragma once

#include <QByteArray>
#include <QString>

#include <optional>

namespace exif {

// The Exif thumbnail's JPEG bytes, if the file has one.
std::optional<QByteArray> thumbnail(const QByteArray &jpeg);

// `jpeg` with its Exif thumbnail replaced by `newThumbnail`. When the old one
// sits at the end of the Exif block, as cameras write it, the block is resized
// around the new one; otherwise the new one is written in place if it fits,
// and failing both the thumbnail is unlinked rather than left stale. A file
// without an Exif thumbnail comes back unchanged.
QByteArray withThumbnail(const QByteArray &jpeg, const QByteArray &newThumbnail);

// `jpeg` with the Exif PixelXDimension/PixelYDimension set to `width` and
// `height`, where the file records them. Edited in place: the tags are
// fixed-size, so nothing moves.
QByteArray withPixelDimensions(const QByteArray &jpeg, int width, int height);

// "Canon EOS 80D", from Make and Model, or empty.
QString cameraModel(const QByteArray &jpeg);

// The Orientation tag (1-8), 1 when absent.
int orientation(const QByteArray &jpeg);

} // namespace exif
