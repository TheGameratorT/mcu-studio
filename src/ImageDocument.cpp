// SPDX-License-Identifier: GPL-3.0-only
// Part of MCU Studio. See LICENSE and THIRD-PARTY-NOTICES.md.

#include "ImageDocument.h"

#include <QCoreApplication>
#include <QFile>
#include <QFileInfo>
#include <QSaveFile>

#include "AutoColor.h"

namespace {
QString tr(const char *text)
{
    return QCoreApplication::translate("ImageDocument", text);
}
} // namespace

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

    if (!adopt(path, data, QString(), error, mode))
        return false;

    m_originalCreated = created;
    m_originalModified = modified;
    m_originalAccessed = accessed;
    return true;
}

bool ImageDocument::loadReconstructed(const QString &path, const QByteArray &bytes,
                                      const QString &donorPath, QString *error)
{
    // The dates come from the damaged file, not from the donor: this is still
    // the photograph that was taken on the day the damaged file says. They are
    // read after the fact here rather than before, since a transplant only
    // happens once the file has already been read and failed to open; on any
    // ordinary filesystem the modification and creation times -- the ones that
    // matter for sorting a rescued folder -- are untouched by reading.
    const QFileInfo sourceInfo(path);
    const QDateTime created = sourceInfo.birthTime();
    const QDateTime modified = sourceInfo.lastModified();
    const QDateTime accessed = sourceInfo.lastRead();

    if (!adopt(path, bytes, donorPath, error))
        return false;

    m_originalCreated = created;
    m_originalModified = modified;
    m_originalAccessed = accessed;
    // Nothing about this reconstruction is on disk yet.
    m_unsavedReconstruction = true;
    return true;
}

bool ImageDocument::adopt(const QString &path, const QByteArray &data, const QString &donorPath,
                          QString *error, jr::SalvageMode mode)
{
    if (data.isEmpty()) {
        if (error)
            *error = tr("%1 is empty.").arg(path);
        return false;
    }

    // A scan that stops being a JPEG partway through is the normal case here,
    // not a reason to turn the file away: cut it back to what libjpeg will read
    // through, so the picture that survived can be worked on.
    const jr::Salvage salvaged = jr::salvageScan(data, mode);
    const QByteArray &usable = salvaged.data;

    QString probeError;
    const auto info = jr::probe(usable, &probeError);
    if (!info || !info->isValid()) {
        if (error)
            *error = tr("%1 is not a JPEG this tool can read: %2").arg(path, probeError);
        return false;
    }
    if (info->numComponents != 3) {
        if (error) {
            *error = tr("%1 has %2 color component(s); the MCU repair tools assume the "
                        "usual three (Y, Cb, Cr).")
                         .arg(path)
                         .arg(info->numComponents);
        }
        return false;
    }

    // Decode before swapping anything in, so a file that probes but will not
    // decode leaves any previously loaded document untouched.
    auto decoded = jr::decodeRgb(usable, error);
    if (!decoded)
        return false;

    m_original = usable;
    m_rendered = usable;
    m_scanTrim = salvaged.damaged()
            ? std::optional<ScanTrim>{{salvaged.mode, salvaged.damageAt, salvaged.droppedBytes,
                                       salvaged.defusedMarkers, salvaged.scanStart,
                                       data.size() - salvaged.damageAt, salvaged.restartInterval}}
            : std::nullopt;
    m_rgb = *decoded;
    m_ycbcr.reset();
    m_info = *info;
    m_steps.clear();
    m_history = {QVector<RepairStep>{}};
    m_index = 0;
    m_savedIndex = 0;

    m_filePath = path;
    m_donorPath = donorPath;
    // A trimmed scan is work that exists only in memory, the same as a donor
    // reconstruction: the file on disk is still the one libjpeg refuses.
    m_unsavedReconstruction = m_scanTrim.has_value();
    m_originalCreated = QDateTime();
    m_originalModified = QDateTime();
    m_originalAccessed = QDateTime();
    return true;
}

bool ImageDocument::saveAs(const QString &path, QString *error)
{
    if (!isOpen()) {
        if (error)
            *error = tr("There is no image to save.");
        return false;
    }

    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly)) {
        if (error)
            *error = tr("Could not write %1: %2").arg(path, file.errorString());
        return false;
    }
    if (file.write(m_rendered) != m_rendered.size() || !file.commit()) {
        if (error)
            *error = tr("Could not write %1: %2").arg(path, file.errorString());
        return false;
    }

    m_filePath = path;
    m_savedIndex = m_index;
    m_unsavedReconstruction = false; // the reconstruction exists on disk now
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
    // settable on every platform, and a repair that wrote its pixels correctly
    // should not be reported as a failure over a file date.
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

const jr::Samples &ImageDocument::ycbcr() const
{
    if (!m_ycbcr) {
        auto decoded = jr::decodeYCbCr(m_rendered);
        m_ycbcr = decoded ? *decoded : jr::Samples{};
    }
    return *m_ycbcr;
}

// ---------------------------------------------------------------------------
// Rendering the recipe
// ---------------------------------------------------------------------------

std::optional<QByteArray> ImageDocument::render(const QVector<RepairStep> &steps,
                                                QString *error) const
{
    QByteArray bytes = m_original;
    QVector<jr::Op> pending;

    const auto flush = [&bytes, &pending, error]() -> bool {
        if (pending.isEmpty())
            return true;
        auto result = jr::apply(bytes, pending, error);
        if (!result)
            return false;
        bytes = *result;
        pending.clear();
        return true;
    };

    for (const RepairStep &step : steps) {
        if (!step.enabled)
            continue;

        if (step.kind == RepairStep::Kind::Ops) {
            pending += step.ops;
            continue;
        }

        // Auto color reads pixels, so everything queued has to be real bytes
        // before it runs. This is the one place the chain breaks, and it is
        // also the one step that already costs a generation.
        if (!flush())
            return std::nullopt;
        const jr::Samples decoded = jr::decodeRgb(bytes, error).value_or(jr::Samples{});
        if (!decoded.isValid())
            return std::nullopt;
        auto encoded = jr::encodeRgb(autocolor::autoCorrect(decoded), autocolor::kQuality, bytes,
                                     error);
        if (!encoded)
            return std::nullopt;
        bytes = *encoded;
    }

    if (!flush())
        return std::nullopt;
    return bytes;
}

bool ImageDocument::adoptRendered(const QByteArray &jpeg, QString *error)
{
    auto decoded = jr::decodeRgb(jpeg, error);
    if (!decoded)
        return false;
    m_rendered = jpeg;
    m_rgb = *decoded;
    m_ycbcr.reset();
    return true;
}

bool ImageDocument::commitSteps(const QVector<RepairStep> &next, QString *error)
{
    if (!isOpen()) {
        if (error)
            *error = tr("There is no image to repair.");
        return false;
    }

    auto rendered = render(next, error);
    if (!rendered)
        return false;
    if (!adoptRendered(*rendered, error))
        return false;

    // A new edit discards anything that was redoable.
    m_history.resize(m_index + 1);
    m_history.append(next);
    m_index = m_history.size() - 1;
    m_steps = next;
    return true;
}

// ---------------------------------------------------------------------------
// Editing the recipe
// ---------------------------------------------------------------------------

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

bool ImageDocument::undo()
{
    if (!canUndo())
        return false;
    const int target = m_index - 1;
    auto rendered = render(m_history.at(target), nullptr);
    if (!rendered || !adoptRendered(*rendered, nullptr))
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
    auto rendered = render(m_history.at(target), nullptr);
    if (!rendered || !adoptRendered(*rendered, nullptr))
        return false;
    m_index = target;
    m_steps = m_history.at(target);
    return true;
}

void ImageDocument::clear()
{
    m_filePath.clear();
    m_donorPath.clear();
    m_unsavedReconstruction = false;
    m_original.clear();
    m_scanTrim.reset();
    m_steps.clear();
    m_history.clear();
    m_index = -1;
    m_savedIndex = -1;
    m_originalCreated = QDateTime();
    m_originalModified = QDateTime();
    m_originalAccessed = QDateTime();
    m_info = jr::Info{};
    m_rendered.clear();
    m_rgb = jr::Samples{};
    m_ycbcr.reset();
}
