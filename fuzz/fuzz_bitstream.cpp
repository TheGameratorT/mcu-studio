// SPDX-License-Identifier: GPL-3.0-only
// Part of MCU Studio. See LICENSE and THIRD-PARTY-NOTICES.md.
#include <QByteArray>

#include "Bitstream.h"

extern "C" int LLVMFuzzerTestOneInput(const unsigned char *data, size_t size)
{
    const QByteArray bytes(reinterpret_cast<const char *>(data), qsizetype(size));
    const bitstream::Map map = bitstream::map(bytes);
    if (!map.isValid() || map.mcus.size() > 20000)
        return 0;
    const qint64 at = map.mcus[map.mcus.size() / 2].bitPos;
    bitstream::deleteBits(bytes, at + 3, 5);
    bitstream::insertBits(bytes, at + 1, 9, 1);
    bitstream::flipBit(bytes, at);
    bitstream::searchResync(bytes, at, 3, 2, 12);
    return 0;
}
