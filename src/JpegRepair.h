// SPDX-License-Identifier: GPL-3.0-only
// Part of MCU Studio. See LICENSE and THIRD-PARTY-NOTICES.md.

// Thin C++ face on the vendored jpegrepair core: owning buffers, Qt types,
// and errors as strings instead of return codes.
#pragma once

#include <QByteArray>
#include <QImage>
#include <QString>
#include <QVector>

#include <memory>
#include <optional>

#include "jpegrepair_core.h"

namespace jr {

struct Info {
    int width = 0;
    int height = 0;
    int numComponents = 0;
    int mcuWidth = 0;
    int mcuHeight = 0;
    int mcusX = 0;
    int mcusY = 0;
    int maxHSamp = 1;
    int maxVSamp = 1;
    int dcQuant[JR_MAX_COMPONENTS] = {1, 1, 1, 1};
    int blocksPerMcu = 0;
    int hSamp[JR_MAX_COMPONENTS] = {1, 1, 1, 1};
    int vSamp[JR_MAX_COMPONENTS] = {1, 1, 1, 1};
    bool progressive = false;
    int restartInterval = 0; // MCUs between restart markers, 0 for none
    int scanCount = 0;
    int colorSpace = 0;      // libjpeg's J_COLOR_SPACE

    int mcuCount() const { return mcusX * mcusY; }
    // Whether MCU scan order is the order the file's data was written in, which
    // is what makes insert and delete line up with the way damage spreads.
    // False for progressive files and for sequential files written one
    // component per scan.
    bool interleavedSingleScan() const { return !progressive && scanCount <= 1; }
    // "4:2:0" and friends, for the sampling layouts that have a name.
    QString samplingName() const;
    // The 8x8 block at position `unit` (0 .. blocksPerMcu-1) of an MCU, as
    // "Y2" or "Cb", for the block-level shift controls.
    QString unitName(int unit) const;
    bool isValid() const { return width > 0 && height > 0 && mcusX > 0 && mcusY > 0; }
};

// A run of MCUs lifted out of an image, in the order the entropy coder writes
// them. Kept as raw coefficients rather than pixels, so putting them back
// costs nothing in quality -- the blocks return exactly as they left.
//
// Unlike Op::copyBlocks, which names its source by offset and so reads
// whatever has landed there by the time it runs, this is a snapshot: later
// edits in the same batch cannot move it.
struct Clipboard {
    QByteArray coefs; // int16 payload, mcuCount * blocksPerMcu * 64 entries
    int mcuCount = 0;
    int blocksPerMcu = 0;
    int numComponents = 0;
    int hSamp[JR_MAX_COMPONENTS] = {0, 0, 0, 0};
    int vSamp[JR_MAX_COMPONENTS] = {0, 0, 0, 0};
    // The quantization tables the coefficients are denominated in, natural
    // order, 64 per component.
    QVector<quint16> quant;
    int srcRow = -1, srcCol = -1; // provenance, for the log

    bool isValid() const;
    // Whether this run can go into `info` as-is. MCU layout depends on the
    // components' sampling factors -- not just how many blocks an MCU holds:
    // 4:2:2 and 4:4:0 both have four, laid out differently -- so a snapshot
    // from a differently sampled image would shear the channels apart.
    bool fits(const Info &info) const;
    // Whether `quantTables` (64 per component) are the ones these coefficients
    // were quantized with. If not, the same numbers mean different amounts.
    bool sameQuantization(const QVector<quint16> &quantTables) const;
    // The blocks re-expressed in `quantTables`: each coefficient becomes
    // round(c * Qsrc / Qdst). Lossy only where the destination's step is
    // coarser than the source's, and far closer than pasting the numbers raw.
    Clipboard requantized(const QVector<quint16> &quantTables) const;
};

// Which MCUs an operation touches.
class Scope
{
public:
    static Scope wholeImage();
    static Scope runFrom(int row, int col, int count = 0);
    static Scope rect(int row, int col, int h, int w);
    // `mask` is rows*cols bytes, row-major, non-zero meaning selected. The
    // Scope keeps its own copy, so callers can let theirs go.
    static Scope mask(QByteArray mask, int rows, int cols);

    jr_scope_kind kind() const { return m_kind; }
    bool isEmpty() const;
    jr_scope toC() const;

    // The parts a Scope was built from, so a project file can write one down
    // and hand it back to the factory it came from. Which of them mean
    // anything depends on kind(): a run reads row, col and width-as-count; a
    // rect reads all four; a mask reads only its own bytes and dimensions.
    int row() const { return m_row; }
    int col() const { return m_col; }
    int height() const { return m_h; }
    int width() const { return m_w; }
    const QByteArray &maskBytes() const { return m_mask; }
    int maskRows() const { return m_maskRows; }
    int maskCols() const { return m_maskCols; }

    bool operator==(const Scope &o) const
    {
        return m_kind == o.m_kind && m_row == o.m_row && m_col == o.m_col && m_h == o.m_h
            && m_w == o.m_w && m_mask == o.m_mask && m_maskRows == o.m_maskRows
            && m_maskCols == o.m_maskCols;
    }

private:
    jr_scope_kind m_kind = JR_SCOPE_RUN;
    int m_row = 0, m_col = 0, m_h = 0, m_w = 0;
    QByteArray m_mask;
    int m_maskRows = 0, m_maskCols = 0;
};

struct Op {
    jr_op_type type = JR_OP_CDELTA;
    Scope scope;
    int a = 0;
    int b = 0;
    // PASTE payload. The Op owns it, the way a Scope owns its mask, so the
    // borrowed pointer the C API takes stays alive for the call.
    QByteArray coefs;

    static Op cdelta(int component, int delta, Scope scope);
    static Op copyBlocks(int dRow, int dCol, Scope scope);
    // Whole MCUs, from the scope's origin to the end of the image.
    static Op insertMcus(int count, Scope scope);
    static Op deleteMcus(int count, Scope scope);
    // Single 8x8 blocks in coding order, starting at block `unit` of the
    // MCU at (row, col).
    static Op insertUnits(int count, int row, int col, int unit);
    static Op deleteUnits(int count, int row, int col, int unit);
    // Writes `clip` back starting at (row, col) and running in scan order,
    // overwriting what is there. Stops at the end of the image.
    static Op paste(int row, int col, const Clipboard &clip);
    // Writes one MCU of `coefs` into each MCU `scope` covers, in scan order.
    // `coefs` has to have been assembled by walking the same scope the same
    // way -- see Patch and fill::build.
    static Op fillScope(Scope scope, QByteArray coefs, int mcuCount);

    bool operator==(const Op &o) const
    {
        return type == o.type && scope == o.scope && a == o.a && b == o.b && coefs == o.coefs;
    }
};

// Pixels turned into the blocks a particular JPEG would have stored for them.
//
// The one bridge into a JPEG from a picture that is not one. Nothing about it
// is lossless -- pixels with no coefficients of their own have to be color
// converted, downsampled, transformed and quantized to acquire any -- but the
// loss is confined to the patch and denominated in the destination's own
// quantization, and every block outside it keeps the coefficients it had.
struct Patch {
    QByteArray coefs; // int16, MCU-major, mcusX * mcusY * blocksPerMcu * 64
    int mcusX = 0;
    int mcusY = 0;
    int blocksPerMcu = 0;

    int coefsPerMcu() const { return blocksPerMcu * 64; }
    bool isValid() const;
    // One MCU of the patch, as a view into `coefs`, or nullptr if it is
    // outside the patch.
    const qint16 *mcu(int row, int col) const;
};

// Three interleaved 8-bit channels, either RGB or YCbCr depending on how it
// was decoded. Kept as raw samples rather than a QImage because the color
// maths wants YCbCr, which QImage has no format for.
struct Samples {
    QByteArray data;
    int width = 0;
    int height = 0;

    bool isValid() const
    {
        return width > 0 && height > 0 && data.size() == qsizetype(width) * height * 3;
    }
    const quint8 *pixel(int x, int y) const
    {
        return reinterpret_cast<const quint8 *>(data.constData()) + (qsizetype(y) * width + x) * 3;
    }
    quint8 *pixel(int x, int y)
    {
        return reinterpret_cast<quint8 *>(data.data()) + (qsizetype(y) * width + x) * 3;
    }
    QImage toImage() const; // interprets the samples as RGB
};

// A scan trimmed back to the part that still parses.
//
// libjpeg reads a JPEG to the end before it will hand over coefficients, so one
// stray byte pair in the entropy stream fails the whole file -- and a stream
// overwritten with foreign data grows plenty of them, including the FF D8 that
// libjpeg reports as "two SOI markers". A viewer never notices, because it
// paints scanlines as they arrive and stops when they run out. This tool has to
// read further than that, so it cuts the stream at the first byte libjpeg would
// refuse and closes it with an EOI. The dropped tail decodes to nothing on its
// own; what it costs is the picture data past the cut, which was unreachable
// regardless.
enum class SalvageMode {
    // Cut the stream at the first unreadable byte. The default, because what
    // follows the cut is usually another file's bytes, and decoding those
    // produces colored noise that looks like a result without being one.
    Truncate,
    // Keep the damaged stretch and defuse the stray markers inside it, so the
    // decoder runs to the end of the file instead of stopping dead. One stray
    // marker byte is enough to make libjpeg abandon every MCU after it, so a
    // region holding real (if displaced) picture data can hide behind a single
    // bad byte. This mode is how you find out, at the price of rewriting those
    // bytes and of noise wherever there was nothing to find.
    ReadThrough,
};

// A damaged scan made readable, one way or the other.
struct Salvage {
    QByteArray data; // the stream libjpeg can read through
    SalvageMode mode = SalvageMode::Truncate;
    // Where the scan stops being readable, -1 if it never does.
    qsizetype damageAt = -1;
    qsizetype droppedBytes = 0;   // Truncate: bytes cut from the end
    qsizetype defusedMarkers = 0; // ReadThrough: stray markers neutralized
    // Where data after a clean End Of Image marker begins, -1 if there is
    // none. That data is a trailer (an MPF preview, a motion photo's video, a
    // ransomware footer), not damage: it is cut from `data` but does not count
    // toward damageAt or droppedBytes.
    qsizetype trailerAt = -1;
    // First entropy-coded byte of the first scan, -1 if the walk never reached
    // one. Lets a caller weigh the loss against the picture data rather than
    // against the file, which counts a header the damage never touched.
    qsizetype scanStart = -1;
    // Restart interval the file declares, 0 for none. Restart markers inside a
    // damaged stretch are real resynchronization points when the file asked for
    // them, and noise when it did not, so ReadThrough keeps them only in the
    // first case.
    int restartInterval = 0;

    bool damaged() const { return damageAt >= 0; }
};

// Every coefficient of an image, held in memory between edits.
//
// Loading runs the entropy decoder once. After that an edit is a memmove or a
// loop over DC terms, a preview decodes only the MCU rows an edit changed, and
// the Huffman coder runs again only when a file is written out. Instances are
// treated as immutable once built and shared between the UI and render
// threads through shared_ptr<const Coefs>.
class Coefs
{
public:
    ~Coefs();
    Coefs(const Coefs &) = delete;
    Coefs &operator=(const Coefs &) = delete;

    static std::shared_ptr<Coefs> load(const QByteArray &jpeg, QString *error = nullptr);
    std::shared_ptr<Coefs> clone() const;

    bool apply(const QVector<Op> &ops, QString *error = nullptr);
    // Writes the coefficients out, taking markers and frame parameters from
    // `headerSource`, the file they were loaded from.
    std::optional<QByteArray> write(const QByteArray &headerSource, QString *error = nullptr) const;
    std::optional<Clipboard> readMcus(int row, int col, int count, QString *error = nullptr) const;

    Info info() const;
    // 64 per component, natural order.
    QVector<quint16> quantTables() const;
    const jr_coefs *raw() const { return m_c; }
    jr_coefs *raw() { return m_c; }
    const qint16 *mcu(int index) const;
    qint16 *mcu(int index);

    // Decodes the whole picture, split across the thread pool.
    Samples render(bool ycbcr) const;
    // Re-decodes MCU rows of `target`, which must already hold this image's
    // size, in place. Used with changedRows() so an edit costs only the rows
    // it touched.
    void renderRows(Samples &target, const QVector<int> &mcuRows, bool ycbcr) const;
    // MCU rows whose pixels can differ between this and `other`: the rows
    // whose coefficients differ, widened by one row each way because the
    // chroma upsampler reads a sample row across the boundary. Empty if the
    // two are identical; every row if their geometry differs.
    QVector<int> changedRows(const Coefs &other) const;

private:
    Coefs() = default;
    jr_coefs *m_c = nullptr;
};

// Never fails: a file too broken to walk comes back unchanged, for probe to
// reject with a better-aimed complaint than this could give.
Salvage salvageScan(const QByteArray &jpeg, SalvageMode mode = SalvageMode::Truncate);

std::optional<Info> probe(const QByteArray &jpeg, QString *error = nullptr);

// Lifts `count` MCUs starting at (row, col) in scan order. A run reaching
// past the last MCU is clamped, not refused.
std::optional<Clipboard> readMcus(const QByteArray &jpeg, int row, int col, int count,
                                  QString *error = nullptr);

// Quantizes `rgb` into the blocks `destJpeg` would have held for it, using
// that file's own quantization tables and sampling factors. `rgb` has to
// measure a whole number of the destination's MCUs on both axes -- Info
// carries the size of one -- so that the patch's block boundaries fall on the
// destination's.
std::optional<Patch> quantizePatch(const QByteArray &destJpeg, const Samples &rgb,
                                   QString *error = nullptr);

// Applies every op in order to one coefficient read, then re-emits the
// stream. Blocks no op touched keep their exact coefficients, so they decode
// to the same pixels as before -- repairs never cost a re-encode generation.
// (Coefficients a damaged stream decoded outside the range a JPEG can store
// are clamped into it; see jr_coefs_sanitize.)
std::optional<QByteArray> apply(const QByteArray &jpeg, const QVector<Op> &ops,
                                QString *error = nullptr);

std::optional<Samples> decodeRgb(const QByteArray &jpeg, QString *error = nullptr);
// The JPEG's own YCbCr samples, with no clipping to the RGB gamut -- see the
// note in jpegrepair_core.h on why that matters for measuring color.
std::optional<Samples> decodeYCbCr(const QByteArray &jpeg, QString *error = nullptr);

// markerSource is the JPEG the pixels came from; its Exif and other metadata
// markers are carried into the re-encoded output. Pass an empty array to write
// a bare JPEG. Uses libjpeg's standard tables and 4:2:0 sampling: for anything
// that has to stay in step with an existing file, use quantizePatch instead.
std::optional<QByteArray> encodeRgb(const Samples &rgb, int quality, const QByteArray &markerSource,
                                    QString *error = nullptr);

} // namespace jr