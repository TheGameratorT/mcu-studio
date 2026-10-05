// SPDX-License-Identifier: GPL-3.0-only
// Part of MCU Studio. See LICENSE and THIRD-PARTY-NOTICES.md.

// The image being repaired, held as a recipe rather than as a result.
//
// The document is the original file plus an ordered list of steps, and the
// image on screen is what you get by replaying the enabled ones. Nothing is
// ever applied on top of an earlier apply's output, which matters more than it
// sounds: libjpeg's compressor fills the dummy blocks that pad the last MCU
// column (and, when the height is not a whole number of MCUs, the last MCU
// row) with DC-only copies of their neighbors, so every re-encode silently
// flattens those blocks. They are outside the visible image, but insert and
// delete shift the stream *through* them, and a second pass would drag the
// flattened blocks into view as a stripe of detail-less squares. Replaying
// from the original keeps the whole session to a single write -- the export
// -- and the hole never opens.
//
// Replaying is cheap because the original's coefficients are decoded once and
// kept in memory: a replay is a copy and a handful of memmoves, and the screen
// is brought up to date by decoding only the MCU rows whose coefficients
// changed.
//
// It also makes a step something you can switch off. Repairing damage is a
// search -- try 17 MCUs, look, try 18 -- so steps are toggleable and
// reorderable, and the picture follows.
#pragma once

#include <QByteArray>
#include <QDateTime>
#include <QHash>
#include <QString>
#include <QVector>

#include <memory>
#include <optional>

#include "JpegRepair.h"
#include "JpegStructure.h"

// An edit to the file's bytes, made before anything is decoded.
//
// Coefficient steps work on what libjpeg made of the stream. These work on
// the stream itself, which is where most damage happened: a bit flipped, a
// run of bytes lost or doubled. Offsets are into the file as the previous
// byte edits left it.
struct ByteEdit {
    enum class Kind {
        DeleteBytes, // `count` bytes at `offset`
        InsertBytes, // `data` at `offset`
        FlipBit,     // bit `bit` of byte `offset`
        DeleteBits,  // `count` bits of entropy-coded data at (`offset`, `bit`)
        InsertBits,  // `count` zero bits of entropy-coded data at (`offset`, `bit`)
        TruncateAt,  // drop everything from `offset` on and close with an EOI
    };

    Kind kind = Kind::DeleteBytes;
    qint64 offset = 0;
    int bit = 0; // 0 is the most significant bit of the byte
    qint64 count = 0;
    QByteArray data;

    qint64 bitPos() const { return offset * 8 + bit; }
    QString describe() const;
    bool operator==(const ByteEdit &o) const
    {
        return kind == o.kind && offset == o.offset && bit == o.bit && count == o.count
            && data == o.data;
    }
};

std::optional<QByteArray> applyByteEdit(const QByteArray &bytes, const ByteEdit &edit,
                                        QString *error = nullptr);

// One entry in the repair recipe.
struct RepairStep {
    enum class Kind {
        Ops,       // coefficient-domain: cdelta, copy, insert, delete, paste
        AutoColor, // pixel-domain, re-quantized into the file's own tables
        Bytes,     // edits to the stream itself, applied before decoding
    };

    Kind kind = Kind::Ops;
    QVector<jr::Op> ops;        // Kind::Ops
    QVector<ByteEdit> edits;    // Kind::Bytes
    QString description;
    bool enabled = true;
};

// Export options that do not belong to the recipe.
struct DocumentExportOptions {
    // Replace the Exif thumbnail, which shows the picture as it was before
    // the repair, with one rendered from the repaired picture.
    bool refreshThumbnail = true;
    // Append whatever followed the image in the original (an MPF preview,
    // a motion photo's video), fixing the MPF index to match. A STOP/Djvu
    // footer is never carried.
    bool keepTrailer = true;
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

    using ExportOptions = DocumentExportOptions;

    bool load(const QString &path, QString *error,
              jr::SalvageMode mode = jr::SalvageMode::Truncate);
    // Opens `bytes` as though they had been read from `path`, for a file that
    // cannot be decoded as it stands: the donor header transplant hands over a
    // reconstruction that libjpeg will accept, while the document goes on
    // belonging to the damaged file -- its name, its dates, and never its
    // bytes as the thing to save over.
    bool loadReconstructed(const QString &path, const QByteArray &bytes, const QString &donorPath,
                           qsizetype spliceOffset, QString *error);
    bool exportTo(const QString &path, QString *error, const ExportOptions &options = {});
    // The bytes exportTo would write.
    std::optional<QByteArray> exportBytes(QString *error, const ExportOptions &options = {}) const;

    bool isOpen() const { return m_state != nullptr; }
    QString filePath() const { return m_filePath; }
    QString fileName() const;
    QString donorPath() const { return m_donorPath; }
    qsizetype donorSpliceOffset() const { return m_donorOffset; }
    jr::SalvageMode salvageMode() const { return m_salvageMode; }
    const std::optional<ScanTrim> &scanTrim() const { return m_base.trim; }
    bool isReconstruction() const { return m_isReconstruction || m_base.trim.has_value(); }
    bool hasUnexportedChanges() const
    {
        return (isReconstruction() || !m_steps.isEmpty()) && m_index != m_exportedIndex;
    }
    // What came after the image in the file as opened.
    const jpegfile::Trailer &trailer() const { return m_trailer; }
    // SHA-256 of the file as read from disk (before any reconstruction).
    QByteArray sourceSha256() const { return m_sourceSha256; }

    const jr::Info &info() const { return m_base.info; }
    // The stream every coefficient step is applied to: the file with its byte
    // edits made and its scan salvaged. Its header is the one exports carry.
    const QByteArray &baseBytes() const { return m_base.bytes; }
    // The file with its byte edits made but before the scan is salvaged: the
    // bytes a new byte edit's offsets refer to.
    const QByteArray &streamBytes() const { return m_base.stream; }
    // The coefficients of the base, before any coefficient step.
    std::shared_ptr<const jr::Coefs> baseCoefs() const { return m_base.coefs; }
    // The coefficients of the current render.
    std::shared_ptr<const jr::Coefs> coefs() const { return m_state; }
    // Decoded RGB of the current render, kept because the view needs it on
    // every repaint. Brought up to date row by row as steps change.
    const jr::Samples &rgb() const { return m_rgb; }
    const jr::Samples &ycbcr() const;
    // The original as opened, decoded, for side-by-side comparison.
    const jr::Samples &originalRgb() const;

    const QVector<RepairStep> &steps() const { return m_steps; }
    int stepCount() const { return m_steps.size(); }
    // Every coefficient op of the enabled steps, in order. AutoColor steps are
    // left out: they rewrite every block, and are reported separately.
    QVector<jr::Op> enabledOps() const;

    // Every mutator renders the new recipe before committing it, so a step
    // that libjpeg refuses leaves the document exactly as it was.
    bool addOps(const QVector<jr::Op> &ops, const QString &description, QString *error);
    bool addAutoColor(const QString &description, QString *error);
    // Byte edits always run before coefficient steps, so the step goes in
    // after the last byte-edit step rather than at the end.
    bool addByteEdits(const QVector<ByteEdit> &edits, const QString &description, QString *error);
    bool setStepEnabled(int index, bool enabled, QString *error);
    bool removeStep(int index, QString *error);
    bool moveStep(int from, int to, QString *error);
    bool clearSteps(QString *error);
    // Takes `steps` as the whole recipe, as when a project file is reopened,
    // along with the undo history that led there when the project kept one.
    bool adoptSteps(const QVector<RepairStep> &steps, QString *error,
                    const QVector<QVector<RepairStep>> &history = {}, int historyIndex = -1);

    bool canUndo() const { return m_index > 0; }
    bool canRedo() const { return m_index + 1 < m_history.size(); }
    bool undo();
    bool redo();
    const QVector<QVector<RepairStep>> &history() const { return m_history; }
    int historyIndex() const { return m_index; }

    void clear();

    // Renders `pending` on top of the current state without committing it.
    // Safe to call on another thread with copies of state() and rgb().
    struct Preview {
        std::shared_ptr<const jr::Coefs> coefs;
        jr::Samples rgb;
    };
    static std::optional<Preview> preview(const std::shared_ptr<const jr::Coefs> &state,
                                          const jr::Samples &rgb, const QVector<jr::Op> &pending,
                                          QString *error);
    // The picture with `edits` added as a byte-edit step, without committing
    // it: the stream is re-read, so this costs a decode of the file, but it is
    // how a candidate fix is judged by eye. Fails, without changing anything,
    // when the edited stream does not decode or changes the frame.
    std::optional<jr::Samples> previewByteEdits(const QVector<ByteEdit> &edits, QString *error) const;

    // Per MCU, what the recipe did to it: JR_TRACE_* bits, plus kNoData for an
    // MCU whose coefficients the damaged stream never supplied and kReencoded
    // when an AutoColor step rewrote everything.
    static constexpr quint8 kNoData = 0x40;
    static constexpr quint8 kReencoded = 0x80;
    QByteArray provenance() const;
    // For every block of the current render, the index of the base block it
    // came from (-1 if a paste wrote it). See jr_trace.
    QVector<qint32> unitSources() const;

private:
    struct Base {
        QVector<ByteEdit> edits; // what produced it
        QByteArray stream;       // source + edits, before salvage
        QByteArray bytes;
        std::shared_ptr<const jr::Coefs> coefs;
        jr::Info info;
        std::optional<ScanTrim> trim;
    };

    // The source with `edits` applied and its scan salvaged, then decoded.
    std::optional<Base> buildBase(const QVector<ByteEdit> &edits, QString *error) const;
    static QVector<ByteEdit> enabledEdits(const QVector<RepairStep> &steps);
    // Replays `steps`. Returns the base it needed (which may differ from the
    // current one when byte edits changed) and the resulting coefficients.
    std::optional<std::pair<Base, std::shared_ptr<jr::Coefs>>>
    render(const QVector<RepairStep> &steps, QString *error) const;
    bool open(const QString &path, const QByteArray &raw, QString *error, jr::SalvageMode mode);
    bool commitSteps(const QVector<RepairStep> &next, QString *error);
    bool show(const QVector<RepairStep> &steps, QString *error);
    void adoptState(Base base, std::shared_ptr<const jr::Coefs> state);
    void applyOriginalTimestamps(const QString &path) const;
    // The re-quantized blocks an AutoColor step writes over `state`, cached
    // by the state they were computed from.
    std::optional<QByteArray> autoColorPayload(const jr::Coefs &state, const QByteArray &header,
                                               QString *error) const;

    QString m_filePath;
    QString m_donorPath;
    qsizetype m_donorOffset = 0;
    bool m_isReconstruction = false;
    jr::SalvageMode m_salvageMode = jr::SalvageMode::Truncate;
    QByteArray m_source;  // the image's bytes as opened, trailer removed
    QByteArray m_raw;     // the whole file as opened (or the transplant), trailer and all
    jpegfile::Trailer m_trailer;
    QByteArray m_sourceSha256;

    Base m_base;
    std::shared_ptr<const jr::Coefs> m_state;
    jr::Samples m_rgb;
    mutable jr::Samples m_ycbcr;
    mutable bool m_ycbcrValid = false;
    mutable jr::Samples m_originalRgb;

    QVector<RepairStep> m_steps;              // == m_history[m_index]
    QVector<QVector<RepairStep>> m_history;   // [0] is where the session began
    int m_index = -1;
    int m_exportedIndex = -1;

    mutable QHash<quint64, QByteArray> m_autoColorCache;

    QDateTime m_originalCreated;
    QDateTime m_originalModified;
    QDateTime m_originalAccessed;
};

bool operator==(const RepairStep &a, const RepairStep &b);
