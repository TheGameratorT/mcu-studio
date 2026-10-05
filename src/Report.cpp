// SPDX-License-Identifier: GPL-3.0-only
// Part of MCU Studio. See LICENSE and THIRD-PARTY-NOTICES.md.

#include "Report.h"

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDateTime>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QSaveFile>

#include "ImageDocument.h"

namespace report {
namespace {

QString tr(const char *text)
{
    return QCoreApplication::translate("Report", text);
}

QString hex(const QByteArray &bytes)
{
    return QString::fromLatin1(bytes.toHex());
}

QString kindName(RepairStep::Kind k)
{
    switch (k) {
    case RepairStep::Kind::Ops:
        return QStringLiteral("coefficients");
    case RepairStep::Kind::AutoColor:
        return QStringLiteral("autoColor");
    case RepairStep::Kind::Bytes:
        return QStringLiteral("bytes");
    }
    return QString();
}

} // namespace

QJsonObject build(const ImageDocument &doc, const QString &exportPath, const QByteArray &exportedBytes)
{
    QJsonObject r;
    r.insert(QStringLiteral("tool"), QStringLiteral("MCU Studio %1").arg(QStringLiteral(MCU_STUDIO_VERSION)));
    r.insert(QStringLiteral("created"), QDateTime::currentDateTimeUtc().toString(Qt::ISODate));

    QJsonObject source;
    source.insert(QStringLiteral("path"), QFileInfo(doc.filePath()).absoluteFilePath());
    source.insert(QStringLiteral("bytes"), QFileInfo(doc.filePath()).size());
    source.insert(QStringLiteral("sha256"), hex(doc.sourceSha256()));
    if (!doc.donorPath().isEmpty()) {
        QJsonObject donor;
        donor.insert(QStringLiteral("path"), QFileInfo(doc.donorPath()).absoluteFilePath());
        donor.insert(QStringLiteral("spliceOffset"), qint64(doc.donorSpliceOffset()));
        source.insert(QStringLiteral("donorHeader"), donor);
    }
    if (const auto &trim = doc.scanTrim()) {
        QJsonObject t;
        t.insert(QStringLiteral("readableUntil"), qint64(trim->at));
        t.insert(QStringLiteral("unreadableBytes"), qint64(trim->damagedBytes));
        t.insert(QStringLiteral("mode"), trim->mode == jr::SalvageMode::ReadThrough
                                             ? QStringLiteral("readThrough")
                                             : QStringLiteral("truncate"));
        source.insert(QStringLiteral("salvage"), t);
    }
    if (doc.trailer().present())
        source.insert(QStringLiteral("trailer"), doc.trailer().describe());
    r.insert(QStringLiteral("source"), source);

    QJsonArray steps;
    for (const RepairStep &s : doc.steps()) {
        QJsonObject step;
        step.insert(QStringLiteral("kind"), kindName(s.kind));
        step.insert(QStringLiteral("enabled"), s.enabled);
        step.insert(QStringLiteral("description"), s.description);
        steps.append(step);
    }
    r.insert(QStringLiteral("steps"), steps);

    // Where every MCU of the output came from.
    const QByteArray prov = doc.provenance();
    qint64 moved = 0, dc = 0, synthesized = 0, noData = 0, reencoded = 0, untouched = 0;
    for (char c : prov) {
        const quint8 f = quint8(c);
        moved += (f & JR_TRACE_MOVED) ? 1 : 0;
        dc += (f & JR_TRACE_DC) ? 1 : 0;
        synthesized += (f & JR_TRACE_PASTED) ? 1 : 0;
        noData += (f & ImageDocument::kNoData) ? 1 : 0;
        reencoded += (f & ImageDocument::kReencoded) ? 1 : 0;
        untouched += f == 0 ? 1 : 0;
    }
    QJsonObject mcus;
    mcus.insert(QStringLiteral("total"), prov.size());
    mcus.insert(QStringLiteral("untouched"), untouched);
    mcus.insert(QStringLiteral("moved"), moved);
    mcus.insert(QStringLiteral("dcAdjusted"), dc);
    mcus.insert(QStringLiteral("pastedOrSynthesized"), synthesized);
    mcus.insert(QStringLiteral("noRecoveredData"), noData);
    mcus.insert(QStringLiteral("reencoded"), reencoded);
    r.insert(QStringLiteral("mcus"), mcus);

    QJsonObject out;
    out.insert(QStringLiteral("path"), QFileInfo(exportPath).absoluteFilePath());
    out.insert(QStringLiteral("bytes"), exportedBytes.size());
    out.insert(QStringLiteral("sha256"),
               hex(QCryptographicHash::hash(exportedBytes, QCryptographicHash::Sha256)));
    const jr::Info &info = doc.info();
    out.insert(QStringLiteral("width"), info.width);
    out.insert(QStringLiteral("height"), info.height);
    r.insert(QStringLiteral("output"), out);
    return r;
}

QString toText(const QJsonObject &r)
{
    const QJsonObject src = r.value(QStringLiteral("source")).toObject();
    const QJsonObject out = r.value(QStringLiteral("output")).toObject();
    const QJsonObject mcus = r.value(QStringLiteral("mcus")).toObject();
    const double total = qMax(1.0, mcus.value(QStringLiteral("total")).toDouble());
    const auto pct = [&](const char *key) {
        return QStringLiteral("%1 (%2%)")
            .arg(mcus.value(QLatin1String(key)).toInteger())
            .arg(100.0 * mcus.value(QLatin1String(key)).toDouble() / total, 0, 'f', 1);
    };
    QStringList lines;
    lines << tr("Recovery report, %1").arg(r.value(QStringLiteral("tool")).toString());
    lines << tr("Created %1").arg(r.value(QStringLiteral("created")).toString());
    lines << QString();
    lines << tr("Source: %1").arg(src.value(QStringLiteral("path")).toString());
    lines << tr("  SHA-256 %1, %2 bytes")
                 .arg(src.value(QStringLiteral("sha256")).toString())
                 .arg(src.value(QStringLiteral("bytes")).toInteger());
    const QJsonObject donor = src.value(QStringLiteral("donorHeader")).toObject();
    if (!donor.isEmpty())
        lines << tr("  Header borrowed from %1, data from byte %2")
                     .arg(donor.value(QStringLiteral("path")).toString())
                     .arg(donor.value(QStringLiteral("spliceOffset")).toInteger());
    if (src.contains(QStringLiteral("trailer")))
        lines << tr("  After the image: %1").arg(src.value(QStringLiteral("trailer")).toString());
    lines << QString();
    lines << tr("Steps:");
    for (const QJsonValue &v : r.value(QStringLiteral("steps")).toArray()) {
        const QJsonObject s = v.toObject();
        lines << QStringLiteral("  [%1] %2 (%3)")
                     .arg(s.value(QStringLiteral("enabled")).toBool() ? QStringLiteral("x") : QStringLiteral(" "),
                          s.value(QStringLiteral("description")).toString(),
                          s.value(QStringLiteral("kind")).toString());
    }
    lines << QString();
    lines << tr("MCUs: %1 total").arg(mcus.value(QStringLiteral("total")).toInteger());
    lines << tr("  untouched:              %1").arg(pct("untouched"));
    lines << tr("  moved into place:       %1").arg(pct("moved"));
    lines << tr("  DC adjusted:            %1").arg(pct("dcAdjusted"));
    lines << tr("  pasted or synthesized:  %1").arg(pct("pastedOrSynthesized"));
    lines << tr("  no recovered data:      %1").arg(pct("noRecoveredData"));
    lines << tr("  re-encoded (auto color): %1").arg(pct("reencoded"));
    lines << QString();
    lines << tr("Output: %1").arg(out.value(QStringLiteral("path")).toString());
    lines << tr("  SHA-256 %1, %2 bytes, %3 x %4")
                 .arg(out.value(QStringLiteral("sha256")).toString())
                 .arg(out.value(QStringLiteral("bytes")).toInteger())
                 .arg(out.value(QStringLiteral("width")).toInt())
                 .arg(out.value(QStringLiteral("height")).toInt());
    return lines.join(QLatin1Char('\n')) + QLatin1Char('\n');
}

bool writeBeside(const QString &exportPath, const QJsonObject &report, QString *error)
{
    QSaveFile file(exportPath + QStringLiteral(".report.json"));
    if (!file.open(QIODevice::WriteOnly)) {
        if (error)
            *error = file.errorString();
        return false;
    }
    const QByteArray bytes = QJsonDocument(report).toJson(QJsonDocument::Indented);
    if (file.write(bytes) != bytes.size() || !file.commit()) {
        if (error)
            *error = file.errorString();
        return false;
    }
    return true;
}

} // namespace report
