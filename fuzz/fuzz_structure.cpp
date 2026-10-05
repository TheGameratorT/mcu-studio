// SPDX-License-Identifier: GPL-3.0-only
// Part of MCU Studio. See LICENSE and THIRD-PARTY-NOTICES.md.
#include <QByteArray>

#include "DonorHeader.h"
#include "JpegRepair.h"
#include "JpegStructure.h"

extern "C" int LLVMFuzzerTestOneInput(const unsigned char *data, size_t size)
{
    const QByteArray bytes(reinterpret_cast<const char *>(data), qsizetype(size));
    const jpegfile::Structure s = jpegfile::walk(bytes);
    jpegfile::findTrailer(bytes, s.imageEnd);
    jpegfile::findTrailer(bytes, -1);
    jpegfile::findStopDjvuFooter(bytes);
    if (size < 65536) {
        jpegfile::embeddedJpegs(bytes);
        jpegfile::carve(bytes, 16, 16);
    }
    jr::salvageScan(bytes, jr::SalvageMode::Truncate);
    jr::salvageScan(bytes, jr::SalvageMode::ReadThrough);
    const donor::Layout layout = donor::scan(bytes);
    donor::splicePoints(bytes, layout, 4);
    donor::describe(layout);
    return 0;
}
