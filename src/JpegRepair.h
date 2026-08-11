// SPDX-License-Identifier: GPL-3.0-only
// Part of MCU Studio. See LICENSE and THIRD-PARTY-NOTICES.md.

// Thin C++ face on the vendored jpegrepair core: owning buffers, Qt types,
// and errors as strings instead of return codes.
#pragma once

#include <QByteArray>
#include <QImage>
#include <QString>
#include <QVector>

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
    bool progressive = false;

    int mcuCount() const { return mcusX * mcusY; }
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
    int srcRow = -1, srcCol = -1; // provenance, for the log

    bool isValid() const;
    // Whether this run can go into `info` as-is. MCU layout depends on the
    // components' sampling factors, so a snapshot from a differently sampled
    // image would shear the channels apart.
    bool fits(const Info &info) const;
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
    static Op insertBlocks(int count, Scope scope);
    static Op deleteBlocks(int count, Scope scope);
    // Writes `clip` back starting at (row, col) and running in scan order,
    // overwriting what is there. Stops at the end of the image.
    static Op paste(int row, int col, const Clipboard &clip);
    // Writes one MCU of `coefs` into each MCU `scope` covers, in scan order.
    // `coefs` has to have been assembled by walking the same scope the same
    // way -- see Patch and fill::build.
    static Op fillScope(Scope scope, QByteArray coefs, int mcuCount);
};

// Pixels turned into the blocks a particular JPEG would have stored for them.
//
// The one bridge into a JPEG from a picture that is not one. Nothing about it
// is lossless -- pixels with no coefficients of their own have to be colour
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
// was decoded. Kept as raw samples rather than a QImage because the colour
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
std::optional<QByteArray> apply(const QByteArray &jpeg, const QVector<Op> &ops,
                                QString *error = nullptr);

std::optional<Samples> decodeRgb(const QByteArray &jpeg, QString *error = nullptr);
// The JPEG's own YCbCr samples, with no clipping to the RGB gamut -- see the
// note in jpegrepair_core.h on why that matters for measuring colour.
std::optional<Samples> decodeYCbCr(const QByteArray &jpeg, QString *error = nullptr);

// markerSource is the JPEG the pixels came from; its Exif and other metadata
// markers are carried into the re-encoded output. Pass an empty array to write
// a bare JPEG.
std::optional<QByteArray> encodeRgb(const Samples &rgb, int quality, const QByteArray &markerSource,
                                    QString *error = nullptr);

} // namespace jr