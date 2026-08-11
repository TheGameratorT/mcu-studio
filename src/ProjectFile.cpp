// SPDX-License-Identifier: GPL-3.0-only
// Part of MCU Studio. See LICENSE and THIRD-PARTY-NOTICES.md.

#include "ProjectFile.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>

namespace project {
namespace {

QString tr(const char *text)
{
    return QCoreApplication::translate("ProjectFile", text);
}

constexpr int kFormatVersion = 1;
constexpr auto kMagic = "mcu-studio";

// Paths go in relative to the project's own folder where they can -- the
// project sits beside its image, and a rescued folder gets moved, copied off
// the card, or renamed wholesale often enough that recording where things were
// on the machine that opened them first is the less useful of the two. An
// absolute path is written when nothing relative can reach (another drive on
// Windows), and read back either way.
QString storePath(const QDir &base, const QString &path)
{
    if (path.isEmpty())
        return QString();
    // Anchor it first. A path can arrive relative -- the window opens whatever
    // the command line names -- and QDir::relativeFilePath reads a relative
    // argument as relative to the directory rather than to where the program
    // was run, which would quietly write down a path that leads nowhere.
    const QString absolute = QFileInfo(path).absoluteFilePath();
    const QString relative = base.relativeFilePath(absolute);
    return QFileInfo(relative).isAbsolute() ? QDir::toNativeSeparators(absolute) : relative;
}

QString resolvePath(const QDir &base, const QString &stored)
{
    if (stored.isEmpty())
        return QString();
    const QString cleaned = QDir::fromNativeSeparators(stored);
    return QFileInfo(cleaned).isAbsolute() ? QDir::cleanPath(cleaned)
                                           : QDir::cleanPath(base.absoluteFilePath(cleaned));
}

// --- ops ------------------------------------------------------------------

QString scopeKindName(jr_scope_kind kind)
{
    switch (kind) {
    case JR_SCOPE_RECT:
        return QStringLiteral("rect");
    case JR_SCOPE_MASK:
        return QStringLiteral("mask");
    case JR_SCOPE_RUN:
        break;
    }
    return QStringLiteral("run");
}

QJsonObject scopeToJson(const jr::Scope &scope)
{
    QJsonObject json;
    json.insert(QStringLiteral("kind"), scopeKindName(scope.kind()));
    switch (scope.kind()) {
    case JR_SCOPE_RUN:
        json.insert(QStringLiteral("row"), scope.row());
        json.insert(QStringLiteral("col"), scope.col());
        // A run's length rides in the width slot; 0 means "to the end".
        json.insert(QStringLiteral("count"), scope.width());
        break;
    case JR_SCOPE_RECT:
        json.insert(QStringLiteral("row"), scope.row());
        json.insert(QStringLiteral("col"), scope.col());
        json.insert(QStringLiteral("height"), scope.height());
        json.insert(QStringLiteral("width"), scope.width());
        break;
    case JR_SCOPE_MASK:
        json.insert(QStringLiteral("rows"), scope.maskRows());
        json.insert(QStringLiteral("cols"), scope.maskCols());
        json.insert(QStringLiteral("mask"),
                    QString::fromLatin1(scope.maskBytes().toBase64()));
        break;
    }
    return json;
}

std::optional<jr::Scope> scopeFromJson(const QJsonObject &json, QString *error)
{
    const QString kind = json.value(QStringLiteral("kind")).toString();
    if (kind == QLatin1String("run")) {
        return jr::Scope::runFrom(json.value(QStringLiteral("row")).toInt(),
                                  json.value(QStringLiteral("col")).toInt(),
                                  json.value(QStringLiteral("count")).toInt());
    }
    if (kind == QLatin1String("rect")) {
        return jr::Scope::rect(json.value(QStringLiteral("row")).toInt(),
                               json.value(QStringLiteral("col")).toInt(),
                               json.value(QStringLiteral("height")).toInt(),
                               json.value(QStringLiteral("width")).toInt());
    }
    if (kind == QLatin1String("mask")) {
        const int rows = json.value(QStringLiteral("rows")).toInt();
        const int cols = json.value(QStringLiteral("cols")).toInt();
        const QByteArray mask = QByteArray::fromBase64(
            json.value(QStringLiteral("mask")).toString().toLatin1());
        if (rows <= 0 || cols <= 0 || mask.size() != qsizetype(rows) * cols) {
            if (error)
                *error = tr("a selection mask does not match the %1x%2 grid it claims")
                             .arg(rows)
                             .arg(cols);
            return std::nullopt;
        }
        return jr::Scope::mask(mask, rows, cols);
    }
    if (error)
        *error = tr("unknown selection kind \"%1\"").arg(kind);
    return std::nullopt;
}

QString opTypeName(jr_op_type type)
{
    switch (type) {
    case JR_OP_CDELTA:
        return QStringLiteral("cdelta");
    case JR_OP_COPY:
        return QStringLiteral("copy");
    case JR_OP_INSERT:
        return QStringLiteral("insert");
    case JR_OP_DELETE:
        return QStringLiteral("delete");
    case JR_OP_PASTE:
        break;
    }
    return QStringLiteral("paste");
}

std::optional<jr_op_type> opTypeFromName(const QString &name)
{
    if (name == QLatin1String("cdelta"))
        return JR_OP_CDELTA;
    if (name == QLatin1String("copy"))
        return JR_OP_COPY;
    if (name == QLatin1String("insert"))
        return JR_OP_INSERT;
    if (name == QLatin1String("delete"))
        return JR_OP_DELETE;
    if (name == QLatin1String("paste"))
        return JR_OP_PASTE;
    return std::nullopt;
}

QJsonObject opToJson(const jr::Op &op)
{
    QJsonObject json;
    json.insert(QStringLiteral("type"), opTypeName(op.type));
    json.insert(QStringLiteral("a"), op.a);
    json.insert(QStringLiteral("b"), op.b);
    json.insert(QStringLiteral("scope"), scopeToJson(op.scope));
    // Only a paste carries one, and it is the one thing here that cannot be
    // re-derived from anything else: the blocks it writes were lifted from a
    // state of the image that no longer exists by the time it replays. Pasting
    // a large selection makes for a large project file, and that is the price
    // of the paste surviving the session.
    if (!op.coefs.isEmpty())
        json.insert(QStringLiteral("coefs"), QString::fromLatin1(op.coefs.toBase64()));
    return json;
}

std::optional<jr::Op> opFromJson(const QJsonObject &json, QString *error)
{
    const auto type = opTypeFromName(json.value(QStringLiteral("type")).toString());
    if (!type) {
        if (error) {
            *error = tr("unknown repair operation \"%1\"")
                         .arg(json.value(QStringLiteral("type")).toString());
        }
        return std::nullopt;
    }

    auto scope = scopeFromJson(json.value(QStringLiteral("scope")).toObject(), error);
    if (!scope)
        return std::nullopt;

    jr::Op op;
    op.type = *type;
    op.scope = std::move(*scope);
    op.a = json.value(QStringLiteral("a")).toInt();
    op.b = json.value(QStringLiteral("b")).toInt();
    op.coefs = QByteArray::fromBase64(json.value(QStringLiteral("coefs")).toString().toLatin1());
    if (op.type == JR_OP_PASTE && op.coefs.isEmpty()) {
        if (error)
            *error = tr("a paste step has no block data");
        return std::nullopt;
    }
    return op;
}

// --- steps ----------------------------------------------------------------

QJsonObject stepToJson(const RepairStep &step)
{
    QJsonObject json;
    json.insert(QStringLiteral("kind"),
                step.kind == RepairStep::Kind::AutoColor ? QStringLiteral("autoColor")
                                                         : QStringLiteral("ops"));
    json.insert(QStringLiteral("enabled"), step.enabled);
    json.insert(QStringLiteral("description"), step.description);
    if (step.kind == RepairStep::Kind::Ops) {
        QJsonArray ops;
        for (const jr::Op &op : step.ops)
            ops.append(opToJson(op));
        json.insert(QStringLiteral("ops"), ops);
    }
    return json;
}

std::optional<RepairStep> stepFromJson(const QJsonObject &json, QString *error)
{
    RepairStep step;
    const QString kind = json.value(QStringLiteral("kind")).toString();
    if (kind == QLatin1String("autoColor")) {
        step.kind = RepairStep::Kind::AutoColor;
    } else if (kind == QLatin1String("ops")) {
        step.kind = RepairStep::Kind::Ops;
    } else {
        if (error)
            *error = tr("unknown step kind \"%1\"").arg(kind);
        return std::nullopt;
    }

    step.enabled = json.value(QStringLiteral("enabled")).toBool(true);
    step.description = json.value(QStringLiteral("description")).toString();

    if (step.kind == RepairStep::Kind::Ops) {
        const QJsonArray ops = json.value(QStringLiteral("ops")).toArray();
        for (const QJsonValue &value : ops) {
            auto op = opFromJson(value.toObject(), error);
            if (!op)
                return std::nullopt;
            step.ops.append(*op);
        }
        if (step.ops.isEmpty()) {
            if (error)
                *error = tr("a repair step has no operations");
            return std::nullopt;
        }
    }
    return step;
}

} // namespace

QString suffix()
{
    return QStringLiteral(".mcup");
}

QString pathForSource(const QString &sourcePath)
{
    return sourcePath.isEmpty() ? QString() : sourcePath + suffix();
}

bool isProjectPath(const QString &path)
{
    return path.endsWith(suffix(), Qt::CaseInsensitive);
}

bool write(const QString &projectPath, const Project &project, QString *error)
{
    const QDir base = QFileInfo(projectPath).absoluteDir();

    QJsonObject json;
    json.insert(QStringLiteral("app"), QLatin1String(kMagic));
    json.insert(QStringLiteral("formatVersion"), kFormatVersion);
    json.insert(QStringLiteral("writtenBy"), QCoreApplication::applicationVersion());
    json.insert(QStringLiteral("source"), storePath(base, project.sourcePath));
    json.insert(QStringLiteral("sourceBytes"), project.sourceBytes);
    json.insert(QStringLiteral("salvageMode"),
                project.salvageMode == jr::SalvageMode::ReadThrough
                    ? QStringLiteral("readThrough")
                    : QStringLiteral("truncate"));
    if (project.donor) {
        QJsonObject donor;
        donor.insert(QStringLiteral("source"), storePath(base, project.donor->path));
        donor.insert(QStringLiteral("spliceOffset"), qint64(project.donor->spliceOffset));
        json.insert(QStringLiteral("donor"), donor);
    }
    if (!project.exportPath.isEmpty())
        json.insert(QStringLiteral("export"), storePath(base, project.exportPath));

    QJsonArray steps;
    for (const RepairStep &step : project.steps)
        steps.append(stepToJson(step));
    json.insert(QStringLiteral("steps"), steps);

    QSaveFile file(projectPath);
    if (!file.open(QIODevice::WriteOnly)) {
        if (error)
            *error = tr("Could not write %1: %2").arg(projectPath, file.errorString());
        return false;
    }
    const QByteArray encoded = QJsonDocument(json).toJson(QJsonDocument::Indented);
    if (file.write(encoded) != encoded.size() || !file.commit()) {
        if (error)
            *error = tr("Could not write %1: %2").arg(projectPath, file.errorString());
        return false;
    }
    return true;
}

std::optional<Project> read(const QString &projectPath, QString *error)
{
    QFile file(projectPath);
    if (!file.open(QIODevice::ReadOnly)) {
        if (error)
            *error = tr("Could not read %1: %2").arg(projectPath, file.errorString());
        return std::nullopt;
    }
    const QByteArray encoded = file.readAll();
    file.close();

    QJsonParseError parseError;
    const QJsonDocument document = QJsonDocument::fromJson(encoded, &parseError);
    if (document.isNull() || !document.isObject()) {
        if (error)
            *error = tr("%1 is not a readable project file: %2")
                         .arg(projectPath, parseError.errorString());
        return std::nullopt;
    }

    const QJsonObject json = document.object();
    if (json.value(QStringLiteral("app")).toString() != QLatin1String(kMagic)) {
        if (error)
            *error = tr("%1 is not an MCU Studio project.").arg(projectPath);
        return std::nullopt;
    }
    const int version = json.value(QStringLiteral("formatVersion")).toInt();
    if (version > kFormatVersion) {
        if (error) {
            *error = tr("%1 was written by a newer version of MCU Studio (project format %2, "
                        "this build reads %3).")
                         .arg(projectPath)
                         .arg(version)
                         .arg(kFormatVersion);
        }
        return std::nullopt;
    }

    const QDir base = QFileInfo(projectPath).absoluteDir();
    Project result;
    result.sourcePath = resolvePath(base, json.value(QStringLiteral("source")).toString());
    if (result.sourcePath.isEmpty()) {
        if (error)
            *error = tr("%1 does not say which image it repairs.").arg(projectPath);
        return std::nullopt;
    }
    result.sourceBytes = json.value(QStringLiteral("sourceBytes")).toInteger();
    result.salvageMode = json.value(QStringLiteral("salvageMode")).toString()
                    == QLatin1String("readThrough")
            ? jr::SalvageMode::ReadThrough
            : jr::SalvageMode::Truncate;

    if (json.contains(QStringLiteral("donor"))) {
        const QJsonObject donor = json.value(QStringLiteral("donor")).toObject();
        Donor parsed;
        parsed.path = resolvePath(base, donor.value(QStringLiteral("source")).toString());
        parsed.spliceOffset = donor.value(QStringLiteral("spliceOffset")).toInteger();
        if (parsed.path.isEmpty()) {
            if (error)
                *error = tr("%1 borrows a header but does not say from which file.")
                             .arg(projectPath);
            return std::nullopt;
        }
        result.donor = parsed;
    }
    result.exportPath = resolvePath(base, json.value(QStringLiteral("export")).toString());

    const QJsonArray steps = json.value(QStringLiteral("steps")).toArray();
    for (const QJsonValue &value : steps) {
        QString stepError;
        auto step = stepFromJson(value.toObject(), &stepError);
        if (!step) {
            if (error) {
                *error = tr("%1 could not be read: step %2 is damaged -- %3.")
                             .arg(projectPath)
                             .arg(result.steps.size() + 1)
                             .arg(stepError);
            }
            return std::nullopt;
        }
        result.steps.append(*step);
    }
    return result;
}

} // namespace project
