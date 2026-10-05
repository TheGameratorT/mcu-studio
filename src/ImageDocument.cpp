// SPDX-License-Identifier: GPL-3.0-only
// Part of MCU Studio. See LICENSE and THIRD-PARTY-NOTICES.md.

#include "ImageDocument.h"

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QFile>
#include <QFileInfo>
#include <QImage>
#include <QSaveFile>

#include "AutoColor.h"
#include "Bitstream.h"
#include "Exif.h"

namespace {

QString tr(const char *text, const char *disambiguation = nullptr, int n = -1)
{
    return QCoreApplication::translate("ImageDocument", text, disambiguation, n);
}

// Longest edge of the Exif thumbnail written on export, as cameras write it.
constexpr int kThumbnailEdge = 160;
constexpr int kThumbnailQuality = 85;
constexpr int kAutoColorCacheEntries = 4;

} // namespace

// ---------------------------------------------------------------------------
// Byte edits
// ---------------------------------------------------------------------------

QString ByteEdit::describe() const
{
    switch (kind) {
    case Kind::DeleteBytes:
        return tr("delete %n byte(s) at %1", nullptr, int(count)).arg(offset);
    case Kind::InsertBytes:
        return tr("insert %n byte(s) at %1", nullptr, int(data.size())).arg(offset);
    case Kind::FlipBit:
        return tr("flip bit %1 of byte %2").arg(bit).arg(offset);
    case Kind::DeleteBits:
        return tr("delete %n bit(s) at byte %1 bit %2", nullptr, int(count)).arg(offset).arg(bit);
    case Kind::InsertBits:
        return tr("insert %n bit(s) at byte %1 bit %2", nullptr, int(count)).arg(offset).arg(bit);
    case Kind::TruncateAt:
        return tr("cut the file at byte %1").arg(offset);
    }
    return QString();
}

std::optional<QByteArray> applyByteEdit(const QByteArray &bytes, const ByteEdit &e, QString *error)
{
    const auto outside = [&](qint64 at) {
        if (error)
            *error = tr("Byte %1 is outside the file (%2 bytes).").arg(at).arg(bytes.size());
        return std::nullopt;
    };
    switch (e.kind) {
    case ByteEdit::Kind::DeleteBytes:
        if (e.offset < 0 || e.count < 0 || e.offset + e.count > bytes.size())
            return outside(e.offset + e.count);
        return QByteArray(bytes).remove(e.offset, e.count);
    case ByteEdit::Kind::InsertBytes:
        if (e.offset < 0 || e.offset > bytes.size())
            return outside(e.offset);
        return QByteArray(bytes).insert(e.offset, e.data);
    case ByteEdit::Kind::FlipBit:
        return bitstream::flipBit(bytes, e.bitPos(), error);
    case ByteEdit::Kind::DeleteBits:
        return bitstream::deleteBits(bytes, e.bitPos(), int(e.count), error);
    case ByteEdit::Kind::InsertBits:
        return bitstream::insertBits(bytes, e.bitPos(), int(e.count), 0, error);
    case ByteEdit::Kind::TruncateAt:
        if (e.offset < 2 || e.offset > bytes.size())
            return outside(e.offset);
        return bytes.left(e.offset) + QByteArray("\xFF\xD9", 2);
    }
    return bytes;
}

bool operator==(const RepairStep &a, const RepairStep &b)
{
    return a.kind == b.kind && a.enabled == b.enabled && a.description == b.description
        && a.ops == b.ops && a.edits == b.edits;
}

// ---------------------------------------------------------------------------
// Opening
// ---------------------------------------------------------------------------

bool ImageDocument::load(const QString &path, QString *error, jr::SalvageMode mode)
{
    // Read the dates before the file itself: opening it updates the access
    // time, and the repaired copy should carry the dates the original had
    // when the user found it.
    const QFileInfo sourceInfo(path);
    const QDateTime created = sourceInfo.birthTime();
    const QDateTime modified = sourceInfo.lastModified();
    const QDateTime accessed = sourceInfo.lastRead();

    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        if (error)
            *error = tr("Could not open %1: %2").arg(path, file.errorString());
        return false;
    }
    const QByteArray data = file.readAll();
    file.close();

    if (!open(path, data, error, mode))
        return false;
    m_sourceSha256 = QCryptographicHash::hash(data, QCryptographicHash::Sha256);
    m_donorPath.clear();
    m_donorOffset = 0;
    m_isReconstruction = false;
    m_originalCreated = created;
    m_originalModified = modified;
    m_originalAccessed = accessed;
    return true;
}

bool ImageDocument::loadReconstructed(const QString &path, const QByteArray &bytes,
                                      const QString &donorPath, qsizetype spliceOffset,
                                      QString *error)
{
    // The dates come from the damaged file, not from the donor: this is still
    // the photograph that was taken on the day the damaged file says.
    const QFileInfo sourceInfo(path);
    const QDateTime created = sourceInfo.birthTime();
    const QDateTime modified = sourceInfo.lastModified();
    const QDateTime accessed = sourceInfo.lastRead();

    QByteArray sha;
    QFile file(path);
    if (file.open(QIODevice::ReadOnly))
        sha = QCryptographicHash::hash(file.readAll(), QCryptographicHash::Sha256);

    if (!open(path, bytes, error, jr::SalvageMode::Truncate))
        return false;
    m_sourceSha256 = sha;
    m_donorPath = donorPath;
    m_donorOffset = spliceOffset;
    m_isReconstruction = true;
    m_originalCreated = created;
    m_originalModified = modified;
    m_originalAccessed = accessed;
    return true;
}

bool ImageDocument::open(const QString &path, const QByteArray &raw, QString *error,
                         jr::SalvageMode mode)
{
    if (raw.isEmpty()) {
        if (error)
            *error = tr("%1 is empty.").arg(path);
        return false;
    }

    // What follows the image is set aside before anything reads the image:
    // it is not picture data, and it is not damage either.
    const jpegfile::Structure structure = jpegfile::walk(raw);
    const jpegfile::Trailer trailer = jpegfile::findTrailer(raw, structure.imageEnd);
    const QByteArray source = trailer.present() ? raw.left(trailer.offset) : raw;

    // Swap in only once everything has worked, so a file that will not open
    // leaves any open document alone.
    const QByteArray previousSource = m_source;
    const jr::SalvageMode previousMode = m_salvageMode;
    m_source = source;
    m_salvageMode = mode;
    QString why;
    auto base = buildBase({}, &why);
    if (!base) {
        m_source = previousSource;
        m_salvageMode = previousMode;
        if (error)
            *error = tr("%1 is not a JPEG this tool can read: %2").arg(path, why);
        return false;
    }

    m_raw = raw;
    m_trailer = trailer;
    m_filePath = path;
    m_steps.clear();
    m_history = {QVector<RepairStep>{}};
    m_index = 0;
    m_exportedIndex = -1;
    m_autoColorCache.clear();
    m_originalRgb = jr::Samples{};
    m_state.reset();
    m_originalCreated = QDateTime();
    m_originalModified = QDateTime();
    m_originalAccessed = QDateTime();
    adoptState(*base, base->coefs);
    return true;
}

std::optional<ImageDocument::Base> ImageDocument::buildBase(const QVector<ByteEdit> &edits,
                                                            QString *error) const
{
    QByteArray bytes = m_source;
    for (const ByteEdit &e : edits) {
        auto next = applyByteEdit(bytes, e, error);
        if (!next)
            return std::nullopt;
        bytes = *next;
    }

    // A scan that stops being a JPEG partway through is the normal case here,
    // not a reason to turn the file away: cut it back to what libjpeg will read
    // through, so the picture that survived can be worked on.
    const jr::Salvage salvaged = jr::salvageScan(bytes, m_salvageMode);

    auto coefs = jr::Coefs::load(salvaged.data, error);
    if (!coefs)
        return std::nullopt;
    const jr::Info info = coefs->info();
    if (!info.isValid()) {
        if (error)
            *error = tr("the picture has no size.");
        return std::nullopt;
    }
    if (info.numComponents != 3 || (info.colorSpace != 3 /* YCbCr */ && info.colorSpace != 2)) {
        if (error) {
            *error = tr("it stores %1 color component(s); the MCU repair tools work on the usual "
                        "three (Y, Cb, Cr).")
                         .arg(info.numComponents);
        }
        return std::nullopt;
    }

    Base base;
    base.edits = edits;
    base.stream = bytes;
    base.bytes = salvaged.data;
    base.coefs = coefs;
    base.info = info;
    if (salvaged.damaged()) {
        base.trim = ScanTrim{salvaged.mode,         salvaged.damageAt,
                             salvaged.droppedBytes, salvaged.defusedMarkers,
                             salvaged.scanStart,    bytes.size() - salvaged.damageAt,
                             salvaged.restartInterval};
    }
    return base;
}

QVector<ByteEdit> ImageDocument::enabledEdits(const QVector<RepairStep> &steps)
{
    QVector<ByteEdit> out;
    for (const RepairStep &s : steps) {
        if (s.enabled && s.kind == RepairStep::Kind::Bytes)
            out += s.edits;
    }
    return out;
}

// ---------------------------------------------------------------------------
// Rendering
// ---------------------------------------------------------------------------

std::optional<QByteArray> ImageDocument::autoColorPayload(const jr::Coefs &state,
                                                          const QByteArray &header,
                                                          QString *error) const
{
    const jr_coefs *c = state.raw();
    const quint64 key = qHashBits(c->data, c->unit_count * 64 * sizeof(int16_t), 0x5eed);
    if (auto it = m_autoColorCache.constFind(key); it != m_autoColorCache.constEnd())
        return *it;

    const jr::Info info = state.info();
    const jr::Samples corrected = autocolor::autoCorrect(state.render(false));

    // The encoder has to be handed whole MCUs, so the picture is extended into
    // the padding by repeating its last row and column -- what a camera's
    // encoder does with the same padding.
    jr::Samples padded;
    padded.width = info.mcusX * info.mcuWidth;
    padded.height = info.mcusY * info.mcuHeight;
    padded.data = QByteArray(qsizetype(padded.width) * padded.height * 3, Qt::Uninitialized);
    for (int y = 0; y < padded.height; ++y) {
        const quint8 *src = corrected.pixel(0, qMin(y, corrected.height - 1));
        quint8 *dst = padded.pixel(0, y);
        std::memcpy(dst, src, size_t(corrected.width) * 3);
        for (int x = corrected.width; x < padded.width; ++x)
            std::memcpy(dst + x * 3, src + (corrected.width - 1) * 3, 3);
    }
    const auto patch = jr::quantizePatch(header, padded, error);
    if (!patch)
        return std::nullopt;
    if (m_autoColorCache.size() >= kAutoColorCacheEntries)
        m_autoColorCache.clear();
    m_autoColorCache.insert(key, patch->coefs);
    return patch->coefs;
}

std::optional<std::pair<ImageDocument::Base, std::shared_ptr<jr::Coefs>>>
ImageDocument::render(const QVector<RepairStep> &steps, QString *error) const
{
    const QVector<ByteEdit> edits = enabledEdits(steps);
    Base base;
    if (m_state && edits == m_base.edits) {
        base = m_base;
    } else {
        auto rebuilt = buildBase(edits, error);
        if (!rebuilt)
            return std::nullopt;
        base = *rebuilt;
    }

    std::shared_ptr<jr::Coefs> work = base.coefs->clone();
    if (!work) {
        if (error)
            *error = tr("Out of memory.");
        return std::nullopt;
    }
    for (const RepairStep &step : steps) {
        if (!step.enabled || step.kind == RepairStep::Kind::Bytes)
            continue;
        if (step.kind == RepairStep::Kind::Ops) {
            if (!work->apply(step.ops, error))
                return std::nullopt;
            continue;
        }
        // AutoColor: decoded, corrected, and quantized back with this file's
        // own tables and sampling, so the grid every later step addresses
        // does not move and nothing is written out in between.
        const auto payload = autoColorPayload(*work, base.bytes, error);
        if (!payload)
            return std::nullopt;
        const int mcus = base.info.mcuCount();
        if (!work->apply({jr::Op::fillScope(jr::Scope::wholeImage(), *payload, mcus)}, error))
            return std::nullopt;
    }
    return std::make_pair(base, work);
}

void ImageDocument::adoptState(Base base, std::shared_ptr<const jr::Coefs> state)
{
    const bool sameGeometry = m_state && m_rgb.isValid() && m_base.info.width == base.info.width
        && m_base.info.height == base.info.height && m_base.info.mcusX == base.info.mcusX
        && m_base.info.mcusY == base.info.mcusY && m_base.info.blocksPerMcu == base.info.blocksPerMcu;
    if (sameGeometry) {
        const QVector<int> rows = state->changedRows(*m_state);
        state->renderRows(m_rgb, rows, false);
        if (m_ycbcrValid)
            state->renderRows(m_ycbcr, rows, true);
    } else {
        m_rgb = state->render(false);
        m_ycbcr = jr::Samples{};
        m_ycbcrValid = false;
    }
    const bool baseChanged = !m_base.coefs || m_base.coefs != base.coefs;
    m_base = std::move(base);
    m_state = std::move(state);
    if (baseChanged)
        m_originalRgb = jr::Samples{};
}

bool ImageDocument::show(const QVector<RepairStep> &steps, QString *error)
{
    auto rendered = render(steps, error);
    if (!rendered)
        return false;
    adoptState(rendered->first, rendered->second);
    return true;
}

std::optional<ImageDocument::Preview>
ImageDocument::preview(const std::shared_ptr<const jr::Coefs> &state, const jr::Samples &rgb,
                       const QVector<jr::Op> &pending, QString *error)
{
    if (!state)
        return std::nullopt;
    std::shared_ptr<jr::Coefs> work = state->clone();
    if (!work || !work->apply(pending, error))
        return std::nullopt;
    Preview out;
    out.rgb = rgb;
    work->renderRows(out.rgb, work->changedRows(*state), false);
    out.coefs = work;
    return out;
}

const jr::Samples &ImageDocument::ycbcr() const
{
    if (!m_ycbcrValid && m_state) {
        m_ycbcr = m_state->render(true);
        m_ycbcrValid = true;
    }
    return m_ycbcr;
}

const jr::Samples &ImageDocument::originalRgb() const
{
    if (!m_originalRgb.isValid() && m_base.coefs)
        m_originalRgb = m_base.coefs->render(false);
    return m_originalRgb;
}

QVector<jr::Op> ImageDocument::enabledOps() const
{
    QVector<jr::Op> out;
    for (const RepairStep &s : m_steps) {
        if (s.enabled && s.kind == RepairStep::Kind::Ops)
            out += s.ops;
    }
    return out;
}

QVector<qint32> ImageDocument::unitSources() const
{
    QVector<qint32> src;
    if (!m_state)
        return src;
    const QVector<jr::Op> ops = enabledOps();
    QVector<jr_op> cOps;
    for (const jr::Op &op : ops) {
        jr_op c;
        c.type = op.type;
        c.scope = op.scope.toC();
        c.a = op.a;
        c.b = op.b;
        c.coefs = op.coefs.isEmpty() ? nullptr : reinterpret_cast<const int16_t *>(op.coefs.constData());
        c.coef_count = size_t(op.coefs.size()) / sizeof(qint16);
        cOps.append(c);
    }
    const jr_info ci = m_base.coefs->raw()->info;
    src.resize(qsizetype(m_base.info.mcuCount()) * m_base.info.blocksPerMcu);
    jr_trace(&ci, cOps.constData(), size_t(cOps.size()), src.data(), nullptr, nullptr, 0);
    return src;
}

QByteArray ImageDocument::provenance() const
{
    if (!m_state)
        return QByteArray();
    const jr::Info &info = m_base.info;
    const int mcus = info.mcuCount();
    QByteArray flags(mcus, '\0');

    const QVector<jr::Op> ops = enabledOps(); // keeps the C ops' borrowed pointers alive
    bool reencoded = false;
    for (const RepairStep &s : m_steps)
        reencoded = reencoded || (s.enabled && s.kind == RepairStep::Kind::AutoColor);
    QVector<jr_op> cOps;
    for (const jr::Op &op : ops) {
        jr_op c;
        c.type = op.type;
        c.scope = op.scope.toC();
        c.a = op.a;
        c.b = op.b;
        c.coefs = op.coefs.isEmpty() ? nullptr : reinterpret_cast<const int16_t *>(op.coefs.constData());
        c.coef_count = size_t(op.coefs.size()) / sizeof(qint16);
        cOps.append(c);
    }
    const jr_info ci = m_base.coefs->raw()->info;
    QVector<qint32> src(qsizetype(mcus) * info.blocksPerMcu);
    jr_trace(&ci, cOps.constData(), size_t(cOps.size()), src.data(),
             reinterpret_cast<uint8_t *>(flags.data()), nullptr, 0);

    // An MCU the damaged stream never reached decodes to all-zero
    // coefficients: libjpeg fills what it cannot read with zeros. Traced back
    // through the moves to wherever it came from.
    const int per = info.blocksPerMcu * 64;
    const qint16 *base = m_base.coefs->mcu(0);
    for (int m = 0; m < mcus; ++m) {
        const qint32 from = src[qsizetype(m) * info.blocksPerMcu];
        if (from >= 0) {
            const qint16 *blk = base + qsizetype(from / info.blocksPerMcu) * per;
            bool empty = true;
            for (int i = 0; i < per && empty; ++i)
                empty = blk[i] == 0;
            if (empty)
                flags[m] = char(quint8(flags[m]) | kNoData);
        }
        if (reencoded)
            flags[m] = char(quint8(flags[m]) | kReencoded);
    }
    return flags;
}

// ---------------------------------------------------------------------------
// Export
// ---------------------------------------------------------------------------

std::optional<QByteArray> ImageDocument::exportBytes(QString *error, const ExportOptions &options) const
{
    if (!m_state) {
        if (error)
            *error = tr("There is no image to export.");
        return std::nullopt;
    }
    auto bytes = m_state->write(m_base.bytes, error);
    if (!bytes)
        return std::nullopt;

    const bool changed = !m_steps.isEmpty() || isReconstruction();
    if (options.refreshThumbnail && changed && exif::thumbnail(*bytes)) {
        // The old thumbnail shows the picture before the repair.
        const QImage full = m_rgb.toImage();
        const QImage small = full.scaled(kThumbnailEdge, kThumbnailEdge, Qt::KeepAspectRatio,
                                         Qt::SmoothTransformation)
                                 .convertToFormat(QImage::Format_RGB888);
        jr::Samples thumb;
        thumb.width = small.width();
        thumb.height = small.height();
        thumb.data.resize(qsizetype(thumb.width) * thumb.height * 3);
        for (int y = 0; y < thumb.height; ++y)
            std::memcpy(thumb.pixel(0, y), small.constScanLine(y), size_t(thumb.width) * 3);
        if (auto encoded = jr::encodeRgb(thumb, kThumbnailQuality, QByteArray(), nullptr))
            *bytes = exif::withThumbnail(*bytes, *encoded);
    }
    // A transplanted header brings the donor's frame size; the Exif, if the
    // damaged file's own survived, should agree with what was written.
    if (!m_donorPath.isEmpty())
        *bytes = exif::withPixelDimensions(*bytes, m_base.info.width, m_base.info.height);

    if (options.keepTrailer && m_trailer.keepOnExport()) {
        const QByteArray tail = m_raw.mid(m_trailer.offset, m_trailer.length);
        if (m_trailer.kind == jpegfile::TrailerKind::Mpf)
            jpegfile::fixMpfOffsets(*bytes, m_raw, m_trailer.offset);
        *bytes += tail;
    }
    return bytes;
}

bool ImageDocument::exportTo(const QString &path, QString *error, const ExportOptions &options)
{
    const auto bytes = exportBytes(error, options);
    if (!bytes)
        return false;

    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly)) {
        if (error)
            *error = tr("Could not write %1: %2").arg(path, file.errorString());
        return false;
    }
    if (file.write(*bytes) != bytes->size() || !file.commit()) {
        if (error)
            *error = tr("Could not write %1: %2").arg(path, file.errorString());
        return false;
    }

    // The document goes on belonging to the file it was opened from, whatever
    // the export was called.
    m_exportedIndex = m_index;
    applyOriginalTimestamps(path);
    return true;
}

// A repaired image is the same photograph as the one that came in, so it keeps
// the original's dates: sorting a rescued folder by date, or matching a file
// back to the day it was taken, has to keep working after a repair.
void ImageDocument::applyOriginalTimestamps(const QString &path) const
{
    if (!m_originalModified.isValid())
        return;

    QFile file(path);
    if (!file.open(QIODevice::ReadWrite))
        return;
    // Best effort, and deliberately quiet: creation time in particular is not
    // settable on every platform.
    if (m_originalCreated.isValid())
        file.setFileTime(m_originalCreated, QFileDevice::FileBirthTime);
    if (m_originalAccessed.isValid())
        file.setFileTime(m_originalAccessed, QFileDevice::FileAccessTime);
    file.setFileTime(m_originalModified, QFileDevice::FileModificationTime);
}

QString ImageDocument::fileName() const
{
    return m_filePath.isEmpty() ? QString() : QFileInfo(m_filePath).fileName();
}

// ---------------------------------------------------------------------------
// Editing the recipe
// ---------------------------------------------------------------------------

bool ImageDocument::commitSteps(const QVector<RepairStep> &next, QString *error)
{
    if (!isOpen()) {
        if (error)
            *error = tr("There is no image to repair.");
        return false;
    }
    if (!show(next, error))
        return false;

    // A new edit discards anything that was redoable.
    m_history.resize(m_index + 1);
    m_history.append(next);
    m_index = m_history.size() - 1;
    m_steps = next;
    return true;
}

bool ImageDocument::addOps(const QVector<jr::Op> &ops, const QString &description, QString *error)
{
    if (ops.isEmpty())
        return true;
    QVector<RepairStep> next = m_steps;
    RepairStep step;
    step.kind = RepairStep::Kind::Ops;
    step.ops = ops;
    step.description = description;
    next.append(step);
    return commitSteps(next, error);
}

bool ImageDocument::addAutoColor(const QString &description, QString *error)
{
    QVector<RepairStep> next = m_steps;
    RepairStep step;
    step.kind = RepairStep::Kind::AutoColor;
    step.description = description;
    next.append(step);
    return commitSteps(next, error);
}

bool ImageDocument::addByteEdits(const QVector<ByteEdit> &edits, const QString &description,
                                 QString *error)
{
    if (edits.isEmpty())
        return true;
    QVector<RepairStep> next = m_steps;
    RepairStep step;
    step.kind = RepairStep::Kind::Bytes;
    step.edits = edits;
    step.description = description;
    int at = 0;
    for (int i = 0; i < next.size(); ++i) {
        if (next[i].kind == RepairStep::Kind::Bytes)
            at = i + 1;
    }
    next.insert(at, step);
    return commitSteps(next, error);
}

bool ImageDocument::setStepEnabled(int index, bool enabled, QString *error)
{
    if (index < 0 || index >= m_steps.size())
        return false;
    if (m_steps.at(index).enabled == enabled)
        return true;
    QVector<RepairStep> next = m_steps;
    next[index].enabled = enabled;
    return commitSteps(next, error);
}

bool ImageDocument::removeStep(int index, QString *error)
{
    if (index < 0 || index >= m_steps.size())
        return false;
    QVector<RepairStep> next = m_steps;
    next.removeAt(index);
    return commitSteps(next, error);
}

bool ImageDocument::moveStep(int from, int to, QString *error)
{
    if (from < 0 || from >= m_steps.size() || to < 0 || to >= m_steps.size() || from == to)
        return false;
    // Byte edits run before everything else whatever the order says, so they
    // only reorder among themselves; letting one sit after a coefficient step
    // would make the list say something the replay does not do.
    const bool bytesFrom = m_steps.at(from).kind == RepairStep::Kind::Bytes;
    const bool bytesTo = m_steps.at(to).kind == RepairStep::Kind::Bytes;
    if (bytesFrom != bytesTo) {
        if (error)
            *error = tr("Byte edits are always applied before coefficient steps, so they stay at "
                        "the top of the list.");
        return false;
    }
    QVector<RepairStep> next = m_steps;
    next.move(from, to);
    return commitSteps(next, error);
}

bool ImageDocument::clearSteps(QString *error)
{
    if (m_steps.isEmpty())
        return false;
    return commitSteps(QVector<RepairStep>{}, error);
}

bool ImageDocument::adoptSteps(const QVector<RepairStep> &steps, QString *error,
                               const QVector<QVector<RepairStep>> &history, int historyIndex)
{
    if (!isOpen()) {
        if (error)
            *error = tr("There is no image to repair.");
        return false;
    }
    if (!show(steps, error))
        return false;

    m_steps = steps;
    if (!history.isEmpty() && historyIndex >= 0 && historyIndex < history.size()
        && history.at(historyIndex) == steps) {
        m_history = history;
        m_index = historyIndex;
    } else {
        m_history = {steps};
        m_index = 0;
    }
    m_exportedIndex = -1;
    return true;
}

bool ImageDocument::undo()
{
    if (!canUndo())
        return false;
    const int target = m_index - 1;
    if (!show(m_history.at(target), nullptr))
        return false;
    m_index = target;
    m_steps = m_history.at(target);
    return true;
}

bool ImageDocument::redo()
{
    if (!canRedo())
        return false;
    const int target = m_index + 1;
    if (!show(m_history.at(target), nullptr))
        return false;
    m_index = target;
    m_steps = m_history.at(target);
    return true;
}

void ImageDocument::clear()
{
    *this = ImageDocument();
}
