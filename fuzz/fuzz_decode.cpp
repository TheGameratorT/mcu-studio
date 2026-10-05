// SPDX-License-Identifier: GPL-3.0-only
// Part of MCU Studio. See LICENSE and THIRD-PARTY-NOTICES.md.
#include <QByteArray>

#include "JpegRepair.h"

extern "C" int LLVMFuzzerTestOneInput(const unsigned char *data, size_t size)
{
    const QByteArray bytes(reinterpret_cast<const char *>(data), qsizetype(size));
    const auto info = jr::probe(bytes);
    if (!info || qint64(info->width) * info->height > 4 * 1000 * 1000)
        return 0;
    auto coefs = jr::Coefs::load(bytes);
    if (!coefs)
        return 0;
    const jr::Info i = coefs->info();
    const int row = i.mcusY / 2, col = i.mcusX / 2;
    coefs->apply({jr::Op::cdelta(0, 7, jr::Scope::runFrom(row, col)),
                  jr::Op::insertMcus(3, jr::Scope::runFrom(row, col)),
                  jr::Op::deleteUnits(2, row, 0, i.blocksPerMcu - 1),
                  jr::Op::copyBlocks(-1, 1, jr::Scope::rect(row, col, 2, 2))});
    if (i.numComponents <= 3) {
        coefs->render(false);
        coefs->render(true);
    }
    coefs->write(bytes);
    if (auto clip = coefs->readMcus(0, 0, 4))
        clip->requantized(coefs->quantTables());
    return 0;
}
