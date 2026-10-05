// SPDX-License-Identifier: GPL-3.0-only
// Part of MCU Studio. See LICENSE and THIRD-PARTY-NOTICES.md.
#include <QByteArray>

#include "DonorHeader.h"

// The first input byte says where to cut the rest into a donor and a damaged
// file.
extern "C" int LLVMFuzzerTestOneInput(const unsigned char *data, size_t size)
{
    if (size < 4)
        return 0;
    const size_t cut = 1 + (size_t(data[0]) * (size - 1)) / 256;
    const QByteArray donorBytes(reinterpret_cast<const char *>(data + 1), qsizetype(cut - 1));
    const QByteArray broken(reinterpret_cast<const char *>(data + cut), qsizetype(size - cut));
    const donor::Layout layout = donor::scan(donorBytes);
    for (bool keep : {false, true}) {
        donor::SpliceOptions o;
        o.keepOwnTables = keep;
        o.width = keep ? 64 : 0;
        o.restartInterval = keep ? 2 : -1;
        donor::splice(donorBytes, layout, broken, broken.size() / 3, nullptr, o);
    }
    return 0;
}
