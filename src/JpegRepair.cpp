// SPDX-License-Identifier: GPL-3.0-only
// Part of MCU Studio. See LICENSE and THIRD-PARTY-NOTICES.md.

#include "JpegRepair.h"

#include <QCoreApplication>

#include <cstdlib>

namespace jr {
namespace {

// The core writes its reason into a fixed buffer; this turns that into the
// QString the caller asked for, or drops it if they did not.
class ErrBuf
{
public:
    char *data() { return m_buf; }
    size_t size() const { return sizeof(m_buf); }
    void flushTo(QString *error) const
    {
        if (error)
            *error = QString::fromUtf8(m_buf);
    }

private:
    char m_buf[JR_ERR_LEN] = {0};
};

// Frees a buffer the core handed back, whatever path we leave by.
struct CBuffer {
    void *p = nullptr;
    ~CBuffer() { jr_free(p); }
    CBuffer() = default;
    CBuffer(const CBuffer &) = delete;
    CBuffer &operator=(const CBuffer &) = delete;
};

Info fromC(const jr_info &in)
{
    Info out;
    out.width = in.width;
    out.height = in.height;
    out.numComponents = in.num_components;
    out.mcuWidth = in.mcu_width;
    out.mcuHeight = in.mcu_height;
    out.mcusX = in.mcus_x;
    out.mcusY = in.mcus_y;
    out.maxHSamp = in.max_h_samp;
    out.maxVSamp = in.max_v_samp;
    out.progressive = in.progressive != 0;
    out.blocksPerMcu = in.blocks_per_mcu;
    for (int i = 0; i < JR_MAX_COMPONENTS; ++i)
        out.dcQuant[i] = in.dc_quant[i];
    return out;
}

std::optional<Samples> decode(const QByteArray &jpeg, bool ycbcr, QString *error)
{
    if (jpeg.isEmpty()) {
        if (error)
            *error = QCoreApplication::translate("jr", "There is no image data to decode.");
        return std::nullopt;
    }

    CBuffer pixels;
    int w = 0, h = 0;
    ErrBuf err;
    if (jr_decode(reinterpret_cast<const uint8_t *>(jpeg.constData()), size_t(jpeg.size()),
                  ycbcr ? 1 : 0, reinterpret_cast<uint8_t **>(&pixels.p), &w, &h, err.data(),
                  err.size())
        != 0) {
        err.flushTo(error);
        return std::nullopt;
    }

    Samples out;
    out.width = w;
    out.height = h;
    out.data = QByteArray(static_cast<const char *>(pixels.p), qsizetype(w) * h * 3);
    return out;
}

} // namespace

// ---------------------------------------------------------------------------
// Scope
// ---------------------------------------------------------------------------

Scope Scope::wholeImage()
{
    Scope s;
    s.m_kind = JR_SCOPE_RUN;
    return s;
}

Scope Scope::runFrom(int row, int col, int count)
{
    Scope s;
    s.m_kind = JR_SCOPE_RUN;
    s.m_row = row;
    s.m_col = col;
    s.m_w = count;
    return s;
}

Scope Scope::rect(int row, int col, int h, int w)
{
    Scope s;
    s.m_kind = JR_SCOPE_RECT;
    s.m_row = row;
    s.m_col = col;
    s.m_h = h;
    s.m_w = w;
    return s;
}

Scope Scope::mask(QByteArray mask, int rows, int cols)
{
    Scope s;
    s.m_kind = JR_SCOPE_MASK;
    s.m_mask = std::move(mask);
    s.m_maskRows = rows;
    s.m_maskCols = cols;
    return s;
}

bool Scope::isEmpty() const
{
    if (m_kind != JR_SCOPE_MASK)
        return false;
    if (m_mask.size() != qsizetype(m_maskRows) * m_maskCols)
        return true;
    for (char c : m_mask) {
        if (c != 0)
            return false;
    }
    return true;
}

jr_scope Scope::toC() const
{
    jr_scope s;
    s.kind = m_kind;
    s.row = m_row;
    s.col = m_col;
    s.h = m_h;
    s.w = m_w;
    s.mask = m_kind == JR_SCOPE_MASK ? reinterpret_cast<const uint8_t *>(m_mask.constData())
                                     : nullptr;
    s.mask_rows = m_maskRows;
    s.mask_cols = m_maskCols;
    return s;
}

// ---------------------------------------------------------------------------
// Op
// ---------------------------------------------------------------------------

Op Op::cdelta(int component, int delta, Scope scope)
{
    return Op{JR_OP_CDELTA, std::move(scope), component, delta, {}};
}

Op Op::copyBlocks(int dRow, int dCol, Scope scope)
{
    return Op{JR_OP_COPY, std::move(scope), dRow, dCol, {}};
}

Op Op::insertBlocks(int count, Scope scope)
{
    return Op{JR_OP_INSERT, std::move(scope), count, 0, {}};
}

Op Op::deleteBlocks(int count, Scope scope)
{
    return Op{JR_OP_DELETE, std::move(scope), count, 0, {}};
}

Op Op::paste(int row, int col, const Clipboard &clip)
{
    return Op{JR_OP_PASTE, Scope::runFrom(row, col), clip.mcuCount, 0, clip.coefs};
}

Op Op::fillScope(Scope scope, QByteArray coefs, int mcuCount)
{
    return Op{JR_OP_PASTE, std::move(scope), mcuCount, 0, std::move(coefs)};
}

// ---------------------------------------------------------------------------
// Clipboard
// ---------------------------------------------------------------------------

bool Clipboard::isValid() const
{
    if (mcuCount <= 0 || blocksPerMcu <= 0)
        return false;
    return coefs.size() == qsizetype(mcuCount) * blocksPerMcu * 64 * qsizetype(sizeof(qint16));
}

bool Clipboard::fits(const Info &info) const
{
    return isValid() && info.isValid() && blocksPerMcu == info.blocksPerMcu
        && numComponents == info.numComponents;
}

// ---------------------------------------------------------------------------
// Patch
// ---------------------------------------------------------------------------

bool Patch::isValid() const
{
    if (mcusX <= 0 || mcusY <= 0 || blocksPerMcu <= 0)
        return false;
    return coefs.size()
        == qsizetype(mcusX) * mcusY * coefsPerMcu() * qsizetype(sizeof(qint16));
}

const qint16 *Patch::mcu(int row, int col) const
{
    if (!isValid() || row < 0 || col < 0 || row >= mcusY || col >= mcusX)
        return nullptr;
    return reinterpret_cast<const qint16 *>(coefs.constData())
        + (qsizetype(row) * mcusX + col) * coefsPerMcu();
}

// ---------------------------------------------------------------------------
// Samples
// ---------------------------------------------------------------------------

QImage Samples::toImage() const
{
    if (!isValid())
        return QImage();
    QImage img(reinterpret_cast<const uchar *>(data.constData()), width, height, width * 3,
               QImage::Format_RGB888);
    return img.copy(); // detach before `data` can go away underneath it
}

namespace {

// Markers that can legally follow a scan. Anything else at a segment boundary
// is foreign data wearing an FF, and is where the stream stops being a JPEG.
// A second SOI counts as foreign: one file holds one image, and libjpeg says so
// in as many words.
bool continuesStream(quint8 marker)
{
    switch (marker) {
    case 0xC4: // DHT
    case 0xCC: // DAC
    case 0xD9: // EOI
    case 0xDA: // SOS
    case 0xDB: // DQT
    case 0xDC: // DNL
    case 0xDD: // DRI
    case 0xDE: // DHP
    case 0xDF: // EXP
    case 0xFE: // COM
        return true;
    default:
        // SOFn, minus the two reserved slots in the run (C8 is JPG, CC is DAC
        // and already taken above).
        if (marker >= 0xC0 && marker <= 0xCF)
            return marker != 0xC8;
        if (marker >= 0xE0 && marker <= 0xEF) // APPn
            return true;
        return false;
    }
}

} // namespace

// ---------------------------------------------------------------------------
// Entry points
// ---------------------------------------------------------------------------

Salvage salvageScan(const QByteArray &jpeg, SalvageMode mode)
{
    Salvage out;
    out.data = jpeg;
    out.mode = mode;

    const auto *d = reinterpret_cast<const quint8 *>(jpeg.constData());
    const qsizetype n = jpeg.size();
    if (n < 4 || d[0] != 0xFF || d[1] != 0xD8)
        return out;

    // Offset of the first byte to drop, once we find one.
    qsizetype cut = -1;
    qsizetype p = 2;

    while (cut < 0) {
        if (p >= n) { // ran off the end mid-header
            cut = p;
            break;
        }
        if (d[p] != 0xFF) {
            cut = p;
            break;
        }
        // Fill bytes: any number of FFs may pad the gap before a marker.
        qsizetype m = p;
        while (m < n && d[m] == 0xFF)
            ++m;
        if (m >= n) {
            cut = p;
            break;
        }

        const quint8 marker = d[m];
        if (!continuesStream(marker)) {
            cut = p;
            break;
        }
        if (marker == 0xD9) { // a clean end: nothing to salvage
            out.data = jpeg.left(m + 1);
            out.droppedBytes = n - out.data.size();
            out.damageAt = out.droppedBytes > 0 ? out.data.size() : -1;
            return out;
        }

        if (m + 2 >= n) {
            cut = p;
            break;
        }
        const qsizetype length = (qsizetype(d[m + 1]) << 8) | d[m + 2];
        if (length < 2 || m + 1 + length > n) {
            cut = p;
            break;
        }
        // DRI carries the restart interval in the first two payload bytes.
        if (marker == 0xDD && length >= 4)
            out.restartInterval = (int(d[m + 3]) << 8) | d[m + 4];
        p = m + 1 + length;

        if (marker != 0xDA)
            continue;
        if (out.scanStart < 0)
            out.scanStart = p;

        // Entropy-coded data: FF 00 is a stuffed literal and FF D0-D7 a restart,
        // both of which belong to the scan. The first FF that is neither ends
        // it, and the marker there decides whether the file goes on.
        while (p < n) {
            if (d[p] != 0xFF) {
                ++p;
                continue;
            }
            qsizetype q = p;
            while (q < n && d[q] == 0xFF)
                ++q;
            if (q >= n) // trailing padding, harmless
                break;
            const quint8 next = d[q];
            if (next == 0x00 || (next >= 0xD0 && next <= 0xD7)) {
                p = q + 1;
                continue;
            }
            break; // p is at the FF that ends the scan; the outer loop judges it
        }
        if (p >= n) { // the scan runs to EOF with no EOI to close it
            cut = n;
            break;
        }
    }

    if (cut < 0)
        return out;

    out.damageAt = cut;

    if (mode == SalvageMode::Truncate) {
        out.data = jpeg.left(cut);
        out.data.append("\xFF\xD9", 2);
        out.droppedBytes = n - cut;
        return out;
    }

    // Read-through: from the damage to the end, every FF that is not already a
    // stuffed literal becomes one. That keeps the byte count (and so the rough
    // position of anything readable further down) while leaving nothing for
    // libjpeg to read as a marker, since the first marker it meets makes it
    // abandon the rest of the image.
    //
    // Restart markers survive only if the file asked for them: with a restart
    // interval set they are genuine resynchronization points that libjpeg can
    // use to recover, and without one they are just noise that would stop it.
    const bool keepRestarts = out.restartInterval > 0;
    auto *w = reinterpret_cast<quint8 *>(out.data.data());
    for (qsizetype i = cut; i + 1 < n; ++i) {
        if (w[i] != 0xFF)
            continue;
        const quint8 next = w[i + 1];
        if (next == 0x00 || (keepRestarts && next >= 0xD0 && next <= 0xD7)) {
            ++i; // already harmless, and its partner is not a marker either
            continue;
        }
        w[i + 1] = 0x00;
        ++out.defusedMarkers;
        ++i;
    }
    // The stream has to end somewhere libjpeg accepts, and every marker in the
    // damaged stretch is gone -- including whatever EOI used to be there.
    if (n >= 2) {
        w[n - 2] = 0xFF;
        w[n - 1] = 0xD9;
    }
    return out;
}

std::optional<Info> probe(const QByteArray &jpeg, QString *error)
{
    if (jpeg.isEmpty()) {
        if (error)
            *error = QCoreApplication::translate("jr", "There is no image data to read.");
        return std::nullopt;
    }

    jr_info info;
    ErrBuf err;
    if (jr_probe(reinterpret_cast<const uint8_t *>(jpeg.constData()), size_t(jpeg.size()), &info,
                 err.data(), err.size())
        != 0) {
        err.flushTo(error);
        return std::nullopt;
    }
    return fromC(info);
}

std::optional<Clipboard> readMcus(const QByteArray &jpeg, int row, int col, int count,
                                  QString *error)
{
    const std::optional<Info> info = probe(jpeg, error);
    if (!info)
        return std::nullopt;

    CBuffer coefs;
    size_t n = 0;
    ErrBuf err;
    if (jr_read_mcus(reinterpret_cast<const uint8_t *>(jpeg.constData()), size_t(jpeg.size()), row,
                     col, count, reinterpret_cast<int16_t **>(&coefs.p), &n, err.data(), err.size())
        != 0) {
        err.flushTo(error);
        return std::nullopt;
    }

    Clipboard clip;
    clip.blocksPerMcu = info->blocksPerMcu;
    clip.numComponents = info->numComponents;
    clip.srcRow = row;
    clip.srcCol = col;
    // The core clamps a run that reaches past the last MCU, so the count comes
    // back from the payload rather than from what was asked for.
    clip.mcuCount = clip.blocksPerMcu > 0 ? int(n / (size_t(clip.blocksPerMcu) * 64)) : 0;
    clip.coefs = QByteArray(static_cast<const char *>(coefs.p), qsizetype(n * sizeof(qint16)));
    if (!clip.isValid()) {
        if (error)
            *error = QCoreApplication::translate("jr", "Could not read those MCUs.");
        return std::nullopt;
    }
    return clip;
}

std::optional<Patch> quantizePatch(const QByteArray &destJpeg, const Samples &rgb, QString *error)
{
    if (destJpeg.isEmpty() || !rgb.isValid()) {
        if (error)
            *error = QCoreApplication::translate("jr", "There are no pixels to quantize.");
        return std::nullopt;
    }

    CBuffer coefs;
    size_t n = 0;
    int mcusX = 0, mcusY = 0;
    ErrBuf err;
    if (jr_quantize_patch(reinterpret_cast<const uint8_t *>(destJpeg.constData()),
                          size_t(destJpeg.size()),
                          reinterpret_cast<const uint8_t *>(rgb.data.constData()), rgb.width,
                          rgb.height, reinterpret_cast<int16_t **>(&coefs.p), &n, &mcusX, &mcusY,
                          err.data(), err.size())
        != 0) {
        err.flushTo(error);
        return std::nullopt;
    }

    Patch patch;
    patch.mcusX = mcusX;
    patch.mcusY = mcusY;
    const qsizetype mcus = qsizetype(mcusX) * mcusY;
    patch.blocksPerMcu = mcus > 0 ? int(n / (size_t(mcus) * 64)) : 0;
    patch.coefs = QByteArray(static_cast<const char *>(coefs.p), qsizetype(n * sizeof(qint16)));
    if (!patch.isValid()) {
        if (error)
            *error = QCoreApplication::translate("jr", "Could not quantize that patch.");
        return std::nullopt;
    }
    return patch;
}

std::optional<QByteArray> apply(const QByteArray &jpeg, const QVector<Op> &ops, QString *error)
{
    if (jpeg.isEmpty()) {
        if (error)
            *error = QCoreApplication::translate("jr", "There is no image data to transform.");
        return std::nullopt;
    }

    // The C ops borrow each Scope's mask and each paste payload, so the Ops
    // must outlive the call -- they do, since `ops` is the caller's.
    QVector<jr_op> cOps;
    cOps.reserve(ops.size());
    for (const Op &op : ops) {
        jr_op c;
        c.type = op.type;
        c.scope = op.scope.toC();
        c.a = op.a;
        c.b = op.b;
        c.coefs = op.coefs.isEmpty() ? nullptr
                                     : reinterpret_cast<const int16_t *>(op.coefs.constData());
        c.coef_count = size_t(op.coefs.size()) / sizeof(qint16);
        cOps.push_back(c);
    }

    CBuffer out;
    size_t outLen = 0;
    ErrBuf err;
    if (jr_apply(reinterpret_cast<const uint8_t *>(jpeg.constData()), size_t(jpeg.size()),
                 cOps.constData(), size_t(cOps.size()), reinterpret_cast<uint8_t **>(&out.p),
                 &outLen, err.data(), err.size())
        != 0) {
        err.flushTo(error);
        return std::nullopt;
    }
    return QByteArray(static_cast<const char *>(out.p), qsizetype(outLen));
}

std::optional<Samples> decodeRgb(const QByteArray &jpeg, QString *error)
{
    return decode(jpeg, false, error);
}

std::optional<Samples> decodeYCbCr(const QByteArray &jpeg, QString *error)
{
    return decode(jpeg, true, error);
}

std::optional<QByteArray> encodeRgb(const Samples &rgb, int quality, const QByteArray &markerSource,
                                    QString *error)
{
    if (!rgb.isValid()) {
        if (error)
            *error = QCoreApplication::translate("jr", "There are no pixels to encode.");
        return std::nullopt;
    }

    CBuffer out;
    size_t outLen = 0;
    ErrBuf err;
    if (jr_encode_rgb(reinterpret_cast<const uint8_t *>(rgb.data.constData()), rgb.width, rgb.height,
                      quality,
                      markerSource.isEmpty()
                          ? nullptr
                          : reinterpret_cast<const uint8_t *>(markerSource.constData()),
                      size_t(markerSource.size()), reinterpret_cast<uint8_t **>(&out.p), &outLen,
                      err.data(), err.size())
        != 0) {
        err.flushTo(error);
        return std::nullopt;
    }
    return QByteArray(static_cast<const char *>(out.p), qsizetype(outLen));
}

} // namespace jr