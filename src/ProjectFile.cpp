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

#include <iterator>

namespace project {
namespace {

QString tr(const char *text)
{
    return QCoreApplication::translate("ProjectFile", text);
}

constexpr int kFormatVersion = 2;
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
    case JR_OP_UNIT_INSERT:
        return QStringLiteral("unitInsert");
    case JR_OP_UNIT_DELETE:
        return QStringLiteral("unitDelete");
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
    if (name == QLatin1String("unitInsert"))
        return JR_OP_UNIT_INSERT;
    if (name == QLatin1String("unitDelete"))
        return JR_OP_UNIT_DELETE;
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

// --- byte edits -----------------------------------------------------------

const char *const kEditKinds[] = {"deleteBytes", "insertBytes", "flipBit",
                                  "deleteBits",  "insertBits",  "truncate"};

QJsonObject editToJson(const ByteEdit &e)
{
    QJsonObject json;
    json.insert(QStringLiteral("kind"), QLatin1String(kEditKinds[int(e.kind)]));
    json.insert(QStringLiteral("offset"), e.offset);
    if (e.bit)
        json.insert(QStringLiteral("bit"), e.bit);
    if (e.count)
        json.insert(QStringLiteral("count"), e.count);
    if (!e.data.isEmpty())
        json.insert(QStringLiteral("data"), QString::fromLatin1(e.data.toBase64()));
    return json;
}

std::optional<ByteEdit> editFromJson(const QJsonObject &json, QString *error)
{
    const QString kind = json.value(QStringLiteral("kind")).toString();
    ByteEdit e;
    bool found = false;
    for (int i = 0; i < int(std::size(kEditKinds)); ++i) {
        if (kind == QLatin1String(kEditKinds[i])) {
            e.kind = ByteEdit::Kind(i);
            found = true;
        }
    }
    if (!found) {
        if (error)
            *error = tr("unknown byte edit \"%1\"").arg(kind);
        return std::nullopt;
    }
    e.offset = json.value(QStringLiteral("offset")).toInteger();
    e.bit = json.value(QStringLiteral("bit")).toInt();
    e.count = json.value(QStringLiteral("count")).toInteger();
    e.data = QByteArray::fromBase64(json.value(QStringLiteral("data")).toString().toLatin1());
    if (e.offset < 0 || e.bit < 0 || e.bit > 7 || e.count < 0) {
        if (error)
            *error = tr("a byte edit points outside the file");
        return std::nullopt;
    }
    return e;
}

// --- steps ----------------------------------------------------------------

QJsonObject stepToJson(const RepairStep &step)
{
    QJsonObject json;
    json.insert(QStringLiteral("kind"),
                step.kind == RepairStep::Kind::AutoColor ? QStringLiteral("autoColor")
                : step.kind == RepairStep::Kind::Bytes   ? QStringLiteral("bytes")
                                                         : QStringLiteral("ops"));
    json.insert(QStringLiteral("enabled"), step.enabled);
    json.insert(QStringLiteral("description"), step.description);
    if (step.kind == RepairStep::Kind::Ops) {
        QJsonArray ops;
        for (const jr::Op &op : step.ops)
            ops.append(opToJson(op));
        json.insert(QStringLiteral("ops"), ops);
    }
    if (step.kind == RepairStep::Kind::Bytes) {
        QJsonArray edits;
        for (const ByteEdit &e : step.edits)
            edits.append(editToJson(e));
        json.insert(QStringLiteral("edits"), edits);
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
    } else if (kind == QLatin1String("bytes")) {
        step.kind = RepairStep::Kind::Bytes;
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
    if (step.kind == RepairStep::Kind::Bytes) {
        const QJsonArray edits = json.value(QStringLiteral("edits")).toArray();
        for (const QJsonValue &value : edits) {
            auto e = editFromJson(value.toObject(), error);
            if (!e)
                return std::nullopt;
            step.edits.append(*e);
        }
        if (step.edits.isEmpty()) {
            if (error)
                *error = tr("a byte-edit step has no edits");
            return std::nullopt;
        }
    }
    return step;
}

// The undo history, stored as lists of indices into a pool of distinct steps:
// consecutive states share almost all of their steps, and a paste step's
// payload can be large.
void historyToJson(QJsonObject &json, const QVector<QVector<RepairStep>> &history, int index)
{
    QVector<RepairStep> pool;
    QJsonArray states;
    for (const QVector<RepairStep> &state : history) {
        QJsonArray refs;
        for (const RepairStep &step : state) {
            qsizetype at = pool.indexOf(step);
            if (at < 0) {
                pool.append(step);
                at = pool.size() - 1;
            }
            refs.append(int(at));
        }
        states.append(refs);
    }
    QJsonArray poolJson;
    for (const RepairStep &step : pool)
        poolJson.append(stepToJson(step));
    QJsonObject h;
    h.insert(QStringLiteral("pool"), poolJson);
    h.insert(QStringLiteral("states"), states);
    h.insert(QStringLiteral("index"), index);
    json.insert(QStringLiteral("history"), h);
}

void historyFromJson(const QJsonObject &json, Project &project)
{
    const QJsonObject h = json.value(QStringLiteral("history")).toObject();
    if (h.isEmpty())
        return;
    QVector<RepairStep> pool;
    for (const QJsonValue &v : h.value(QStringLiteral("pool")).toArray()) {
        auto step = stepFromJson(v.toObject(), nullptr);
        if (!step)
            return; // history is a convenience: a damaged one is dropped, not fatal
        pool.append(*step);
    }
    QVector<QVector<RepairStep>> history;
    for (const QJsonValue &v : h.value(QStringLiteral("states")).toArray()) {
        QVector<RepairStep> state;
        for (const QJsonValue &ref : v.toArray()) {
            const int i = ref.toInt(-1);
            if (i < 0 || i >= pool.size())
                return;
            state.append(pool.at(i));
        }
        history.append(state);
    }
    project.history = history;
    project.historyIndex = h.value(QStringLiteral("index")).toInt(-1);
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
    if (!project.sourceSha256.isEmpty())
        json.insert(QStringLiteral("sourceSha256"), QString::fromLatin1(project.sourceSha256.toHex()));
    json.insert(QStringLiteral("salvageMode"),
                project.salvageMode == jr::SalvageMode::ReadThrough
                    ? QStringLiteral("readThrough")
                    : QStringLiteral("truncate"));
    if (project.donor) {
        QJsonObject donor;
        donor.insert(QStringLiteral("source"), storePath(base, project.donor->path));
        donor.insert(QStringLiteral("spliceOffset"), qint64(project.donor->spliceOffset));
        const donor::SpliceOptions &o = project.donor->options;
        donor.insert(QStringLiteral("keepOwnTables"), o.keepOwnTables);
        donor.insert(QStringLiteral("renumberRestarts"), o.renumberRestarts);
        if (o.width > 0)
            donor.insert(QStringLiteral("width"), o.width);
        if (o.height > 0)
            donor.insert(QStringLiteral("height"), o.height);
        if (o.restartInterval >= 0)
            donor.insert(QStringLiteral("restartInterval"), o.restartInterval);
        json.insert(QStringLiteral("donor"), donor);
    }
    if (!project.exportPath.isEmpty())
        json.insert(QStringLiteral("export"), storePath(base, project.exportPath));

    QJsonArray steps;
    for (const RepairStep &step : project.steps)
        steps.append(stepToJson(step));
    json.insert(QStringLiteral("steps"), steps);
    if (project.history.size() > 1)
        historyToJson(json, project.history, project.historyIndex);

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
    result.sourceSha256 =
        QByteArray::fromHex(json.value(QStringLiteral("sourceSha256")).toString().toLatin1());
    result.salvageMode = json.value(QStringLiteral("salvageMode")).toString()
                    == QLatin1String("readThrough")
            ? jr::SalvageMode::ReadThrough
            : jr::SalvageMode::Truncate;

    if (json.contains(QStringLiteral("donor"))) {
        const QJsonObject donor = json.value(QStringLiteral("donor")).toObject();
        Donor parsed;
        parsed.path = resolvePath(base, donor.value(QStringLiteral("source")).toString());
        parsed.spliceOffset = donor.value(QStringLiteral("spliceOffset")).toInteger();
        // Projects from before these options existed spliced the donor's
        // header whole and left restart markers alone.
        const bool legacy = !donor.contains(QStringLiteral("keepOwnTables"));
        parsed.options.keepOwnTables = donor.value(QStringLiteral("keepOwnTables")).toBool(!legacy);
        parsed.options.renumberRestarts =
            donor.value(QStringLiteral("renumberRestarts")).toBool(!legacy);
        parsed.options.width = donor.value(QStringLiteral("width")).toInt(0);
        parsed.options.height = donor.value(QStringLiteral("height")).toInt(0);
        parsed.options.restartInterval = donor.value(QStringLiteral("restartInterval")).toInt(-1);
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
    historyFromJson(json, result);
    return result;
}

bool openSource(const Project &project, ImageDocument &doc, QString *error)
{
    if (!project.donor)
        return doc.load(project.sourcePath, error, project.salvageMode);
    QFile broken(project.sourcePath), donorFile(project.donor->path);
    if (!broken.open(QIODevice::ReadOnly)) {
        if (error)
            *error = tr("Could not read %1: %2").arg(project.sourcePath, broken.errorString());
        return false;
    }
    if (!donorFile.open(QIODevice::ReadOnly)) {
        if (error)
            *error = tr("Could not read %1: %2").arg(project.donor->path, donorFile.errorString());
        return false;
    }
    const QByteArray donorBytes = donorFile.readAll();
    const auto spliced = donor::splice(donorBytes, donor::scan(donorBytes), broken.readAll(),
                                       project.donor->spliceOffset, error, project.donor->options);
    if (!spliced)
        return false;
    return doc.loadReconstructed(project.sourcePath, spliced->bytes, project.donor->path,
                                 project.donor->spliceOffset, error);
}

} // namespace project
