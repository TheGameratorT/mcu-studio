// SPDX-License-Identifier: GPL-3.0-only
// Part of MCU Studio. See LICENSE and THIRD-PARTY-NOTICES.md.
#include <QByteArray>

#include "Exif.h"

extern "C" int LLVMFuzzerTestOneInput(const unsigned char *data, size_t size)
{
    const QByteArray bytes(reinterpret_cast<const char *>(data), qsizetype(size));
    exif::thumbnail(bytes);
    exif::withThumbnail(bytes, QByteArray(100, '\x42'));
    exif::withThumbnail(bytes, QByteArray(70000, '\x42'));
    exif::withPixelDimensions(bytes, 4000, 3000);
    exif::cameraModel(bytes);
    exif::orientation(bytes);
    return 0;
}
