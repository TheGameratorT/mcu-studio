// SPDX-License-Identifier: GPL-3.0-only
// Part of MCU Studio. See LICENSE and THIRD-PARTY-NOTICES.md.
#include <QFile>
#include <QTemporaryDir>

#include "ProjectFile.h"

extern "C" int LLVMFuzzerTestOneInput(const unsigned char *data, size_t size)
{
    static QTemporaryDir dir;
    const QString path = dir.filePath(QStringLiteral("fuzz.mcup"));
    {
        QFile f(path);
        if (!f.open(QIODevice::WriteOnly))
            return 0;
        f.write(reinterpret_cast<const char *>(data), qint64(size));
    }
    QString error;
    if (const auto p = project::read(path, &error))
        project::write(path, *p, &error);
    return 0;
}
