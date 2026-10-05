// SPDX-License-Identifier: GPL-3.0-only
// Part of MCU Studio. See LICENSE and THIRD-PARTY-NOTICES.md.

// What a repair did, written down for someone who was not there.
//
// A repaired photograph looks like a photograph. Where it is going to be
// relied on -- an insurance claim, a court, an archive -- the person relying
// on it needs to know which parts are the file's own data, which were moved
// into place, which had their color adjusted, and which were synthesized from
// another picture or never recovered at all. The report says that, with
// hashes tying it to the exact input and output bytes.
#pragma once

#include <QByteArray>
#include <QJsonObject>
#include <QString>

class ImageDocument;

namespace report {

QJsonObject build(const ImageDocument &doc, const QString &exportPath,
                  const QByteArray &exportedBytes);
// The same, as readable text.
QString toText(const QJsonObject &report);

// Writes `<exportPath>.report.json` beside the export.
bool writeBeside(const QString &exportPath, const QJsonObject &report, QString *error = nullptr);

} // namespace report
