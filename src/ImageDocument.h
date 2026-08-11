// SPDX-License-Identifier: GPL-3.0-only
// Part of MCU Studio. See LICENSE and THIRD-PARTY-NOTICES.md.

// The image being repaired, held as a recipe rather than as a result.
//
// The document is the original file plus an ordered list of steps, and the
// image on screen is what you get by replaying the enabled ones. Nothing is
// ever applied on top of an earlier apply's output, which matters more than it
// sounds: libjpeg's compressor fills the dummy blocks that pad the last MCU
// column with a DC-only copy of their neighbor, so every re-encode silently
// flattens one 8-pixel block column per MCU row. Those blocks are outside the
// visible image, but insert and delete shift the stream *through* them, and a
// second pass drags the flattened blocks into view as a stripe of detail-less
// squares. Replaying from the original keeps the whole session to a single
// encode and the hole never opens.
//
// It also makes a step something you can switch off. Repairing damage is a
// search -- try 17 blocks, look, try 18 -- so steps are toggleable and
// reorderable, and the picture follows.
#pragma once

#include <QByteArray>
#include <QDateTime>
#include <QString>
#include <QVector>

#include <optional>

#include "JpegRepair.h"

// One entry in the repair recipe.
struct RepairStep {
    enum class Kind {
        Ops,       // coefficient-domain: cdelta, copy, insert, delete, paste
        AutoColor // pixel-domain, and so a re-encode -- see ImageDocument::render
    };

    Kind kind = Kind::Ops;
    QVector<jr::Op> ops; // empty for AutoColor
    QString description;
    bool enabled = true;
};

class ImageDocument
{
public:
    // How much of a damaged scan had to be dropped for libjpeg to read the file
    // at all. See jr::salvageScan.
    struct ScanTrim {
        jr::SalvageMode mode = jr::SalvageMode::Truncate;
        qsizetype at = 0;         // where the scan stopped being readable
        qsizetype dropped = 0;    // bytes cut off the end, Truncate only
        qsizetype defused = 0;    // stray markers neutralized, ReadThrough only
        qsizetype scanStart = -1; // where the picture data began, -1 if unknown
        qsizetype damagedBytes = 0; // how much of the scan is past `at`
        int restartInterval = 0;    // what the file declares, 0 for none

        // What fraction of the picture data the damage covers, 0 if there is no
        // sound basis for saying. Measured against the scan rather than the
        // file, so an undamaged header does not flatter the number.
        double lostFraction() const
        {
            const qsizetype scanBytes = at + damagedBytes - scanStart;
            return (scanStart >= 0 && scanBytes > 0) ? double(damagedBytes) / double(scanBytes)
                                                     : 0.0;
        }
    };

    // `mode` decides what to do with a scan that stops being readable partway
    // through; see jr::SalvageMode. Reopening the same path in ReadThrough is
    // how the window acts on the offer it makes when Truncate finds damage.
    bool load(const QString &path, QString *error,
              jr::SalvageMode mode = jr::SalvageMode::Truncate);
    // Opens `bytes` as though they had been read from `path`, for a file that
    // cannot be decoded as it stands: the donor header transplant hands over a
    // reconstruction that libjpeg will accept, while the document goes on
    // belonging to the damaged file -- its name, its dates, and never its
    // bytes as the thing to save over. `donorPath` is where the header came
    // from and `spliceOffset` where the damaged file's own data was taken to
    // resume; both are remembered so the window can say so and a project file
    // can rebuild the same reconstruction.
    bool loadReconstructed(const QString &path, const QByteArray &bytes, const QString &donorPath,
                           qsizetype spliceOffset, QString *error);
    // Writes the rendered image out. This is the only thing the session ever
    // asks a path for: the repair itself lives in a project file the window
    // keeps written for it.
    bool exportTo(const QString &path, QString *error);

    bool isOpen() const { return !m_original.isEmpty(); }
    QString filePath() const { return m_filePath; }
    QString fileName() const;
    // Where a transplanted header came from, empty for a file that opened on
    // its own.
    QString donorPath() const { return m_donorPath; }
    // Where the damaged file's own data was taken to resume behind that header.
    qsizetype donorSpliceOffset() const { return m_donorOffset; }
    // What was done to a scan that stopped being readable. Recorded whether or
    // not there was damage, because reopening the file has to make the same
    // choice to arrive at the same bytes.
    jr::SalvageMode salvageMode() const { return m_salvageMode; }
    // Set when the scan had to be cut back to open the file, so the window can
    // say how much of the picture data was unreachable.
    const std::optional<ScanTrim> &scanTrim() const { return m_scanTrim; }
    // Whether the bytes being repaired are something this tool assembled --
    // a donor transplant, or a scan cut back to what libjpeg would read --
    // rather than the file as it sits on disk.
    bool isReconstruction() const { return m_isReconstruction; }
    // Whether there is repair work the exported JPEG does not have. The recipe
    // itself is never at risk -- the project file holds it -- so this asks only
    // about the picture: a reconstruction with no steps still counts, because
    // nothing on disk holds that picture either.
    bool hasUnexportedChanges() const
    {
        return (m_isReconstruction || !m_steps.isEmpty()) && m_index != m_exportedIndex;
    }

    const jr::Info &info() const { return m_info; }
    // The rendered result of the enabled steps.
    const QByteArray &bytes() const { return m_rendered; }
    // Decoded RGB of the rendered state, kept because the view needs it on
    // every repaint and re-decoding a large JPEG per paint is wasteful.
    const jr::Samples &rgb() const { return m_rgb; }
    // Decoded on first use: only the color-matching path needs it.
    const jr::Samples &ycbcr() const;

    const QVector<RepairStep> &steps() const { return m_steps; }
    int stepCount() const { return m_steps.size(); }

    // Every mutator renders the new recipe before committing it, so a step
    // that libjpeg refuses leaves the document exactly as it was.
    bool addOps(const QVector<jr::Op> &ops, const QString &description, QString *error);
    bool addAutoColor(const QString &description, QString *error);
    bool setStepEnabled(int index, bool enabled, QString *error);
    bool removeStep(int index, QString *error);
    // Moves the step at `from` so that it ends up at index `to`.
    bool moveStep(int from, int to, QString *error);
    // Drops every step. Undoable, so an accidental click does not throw away
    // a long repair session.
    bool clearSteps(QString *error);
    // Takes `steps` as the whole recipe, as when a project file is reopened.
    // Unlike every other mutator this starts the history over rather than
    // adding to it: the steps are where the session was left, and there is
    // nothing behind them to undo back into.
    bool adoptSteps(const QVector<RepairStep> &steps, QString *error);

    bool canUndo() const { return m_index > 0; }
    bool canRedo() const { return m_index + 1 < m_history.size(); }
    bool undo();
    bool redo();

    void clear();

private:
    // Replays `steps` from the original. Coefficient steps are accumulated and
    // handed to one jr::apply, so a run of them costs a single decode/encode;
    // an AutoColor step has to flush that run, because it works on pixels.
    std::optional<QByteArray> render(const QVector<RepairStep> &steps, QString *error) const;
    // Checks `data` over and, if it holds up, makes it the document. Nothing
    // is swapped in until every check has passed, so a file that will not
    // decode leaves any open document alone.
    bool adopt(const QString &path, const QByteArray &data, const QString &donorPath,
               QString *error, jr::SalvageMode mode = jr::SalvageMode::Truncate);
    // Renders `next`, and on success makes it the current state and pushes it
    // onto the history.
    bool commitSteps(const QVector<RepairStep> &next, QString *error);
    bool adoptRendered(const QByteArray &jpeg, QString *error);
    void applyOriginalTimestamps(const QString &path) const;

    QString m_filePath;
    QString m_donorPath;   // set when the header was borrowed from another file
    qsizetype m_donorOffset = 0;
    bool m_isReconstruction = false;
    jr::SalvageMode m_salvageMode = jr::SalvageMode::Truncate;
    QByteArray m_original; // the file as opened, never edited
    std::optional<ScanTrim> m_scanTrim;

    QVector<RepairStep> m_steps;              // == m_history[m_index]
    QVector<QVector<RepairStep>> m_history;   // [0] is the empty recipe
    int m_index = -1;
    int m_exportedIndex = -1; // the state the exported JPEG holds, -1 for none

    // Taken when the file is opened, because saving may well overwrite it.
    QDateTime m_originalCreated;
    QDateTime m_originalModified;
    QDateTime m_originalAccessed;

    jr::Info m_info;
    QByteArray m_rendered;
    jr::Samples m_rgb;
    mutable std::optional<jr::Samples> m_ycbcr;
};
