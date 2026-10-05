// SPDX-License-Identifier: GPL-3.0-only
// Part of MCU Studio. See LICENSE and THIRD-PARTY-NOTICES.md.

#include "JpegRepair.h"

#include <QCoreApplication>
#include <QtConcurrent>

#include <cmath>
#include <cstdlib>
#include <cstring>

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
    out.restartInterval = in.restart_interval;
    out.scanCount = in.scan_count;
    out.colorSpace = in.color_space;
    for (int i = 0; i < JR_MAX_COMPONENTS; ++i) {
        out.dcQuant[i] = in.dc_quant[i];
        out.hSamp[i] = in.h_samp[i] > 0 ? in.h_samp[i] : 1;
        out.vSamp[i] = in.v_samp[i] > 0 ? in.v_samp[i] : 1;
    }
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
// Info
// ---------------------------------------------------------------------------

QString Info::samplingName() const
{
    if (numComponents == 1)
        return QStringLiteral("grayscale");
    if (numComponents != 3)
        return QStringLiteral("%1 components").arg(numComponents);
    if (hSamp[1] != hSamp[2] || vSamp[1] != vSamp[2] || hSamp[1] != 1 || vSamp[1] != 1)
        return QStringLiteral("%1x%2, %3x%4, %5x%6")
            .arg(hSamp[0]).arg(vSamp[0]).arg(hSamp[1]).arg(vSamp[1]).arg(hSamp[2]).arg(vSamp[2]);
    const int h = hSamp[0], v = vSamp[0];
    if (h == 1 && v == 1)
        return QStringLiteral("4:4:4");
    if (h == 2 && v == 1)
        return QStringLiteral("4:2:2");
    if (h == 2 && v == 2)
        return QStringLiteral("4:2:0");
    if (h == 1 && v == 2)
        return QStringLiteral("4:4:0");
    if (h == 4 && v == 1)
        return QStringLiteral("4:1:1");
    return QStringLiteral("%1x%2").arg(h).arg(v);
}

QString Info::unitName(int unit) const
{
    static const char *const names[] = {"Y", "Cb", "Cr", "K"};
    int base = 0;
    for (int c = 0; c < numComponents && c < JR_MAX_COMPONENTS; ++c) {
        const int n = hSamp[c] * vSamp[c];
        if (unit < base + n) {
            const QString name = QString::fromLatin1(numComponents == 3 || c < 1 ? names[c]
                                                                                  : "C");
            return n > 1 ? QStringLiteral("%1%2").arg(name).arg(unit - base + 1) : name;
        }
        base += n;
    }
    return QStringLiteral("?");
}

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

Op Op::insertMcus(int count, Scope scope)
{
    return Op{JR_OP_INSERT, std::move(scope), count, 0, {}};
}

Op Op::deleteMcus(int count, Scope scope)
{
    return Op{JR_OP_DELETE, std::move(scope), count, 0, {}};
}

Op Op::insertUnits(int count, int row, int col, int unit)
{
    return Op{JR_OP_UNIT_INSERT, Scope::runFrom(row, col), count, unit, {}};
}

Op Op::deleteUnits(int count, int row, int col, int unit)
{
    return Op{JR_OP_UNIT_DELETE, Scope::runFrom(row, col), count, unit, {}};
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
    if (!isValid() || !info.isValid() || blocksPerMcu != info.blocksPerMcu
        || numComponents != info.numComponents)
        return false;
    for (int c = 0; c < numComponents && c < JR_MAX_COMPONENTS; ++c) {
        if (hSamp[c] != info.hSamp[c] || vSamp[c] != info.vSamp[c])
            return false;
    }
    return true;
}

bool Clipboard::sameQuantization(const QVector<quint16> &quantTables) const
{
    return quant.isEmpty() || quant == quantTables;
}

Clipboard Clipboard::requantized(const QVector<quint16> &quantTables) const
{
    Clipboard out = *this;
    if (quant.isEmpty() || quantTables.size() < numComponents * 64 || quant.size() < numComponents * 64)
        return out;
    out.quant = quantTables;
    qint16 *p = reinterpret_cast<qint16 *>(out.coefs.data());
    for (int m = 0; m < mcuCount; ++m) {
        for (int c = 0; c < numComponents; ++c) {
            const int blocks = hSamp[c] * vSamp[c];
            const quint16 *qs = quant.constData() + c * 64;
            const quint16 *qd = quantTables.constData() + c * 64;
            for (int b = 0; b < blocks; ++b, p += 64) {
                for (int i = 0; i < 64; ++i) {
                    if (p[i] == 0 || qd[i] == 0)
                        continue;
                    // Only the store's own int16 bounds: what a JPEG can hold
                    // is applied on the way out (jr_coefs_refresh_dc).
                    const long v = std::lround(double(p[i]) * qs[i] / qd[i]);
                    p[i] = qint16(qBound(-32767L, v, 32767L));
                }
            }
        }
    }
    return out;
}

// ---------------------------------------------------------------------------
// Coefs
// ---------------------------------------------------------------------------

Coefs::~Coefs()
{
    jr_coefs_free(m_c);
}

std::shared_ptr<Coefs> Coefs::load(const QByteArray &jpeg, QString *error)
{
    if (jpeg.isEmpty()) {
        if (error)
            *error = QCoreApplication::translate("jr", "There is no image data to read.");
        return nullptr;
    }
    ErrBuf err;
    jr_coefs *c = nullptr;
    if (jr_coefs_load(reinterpret_cast<const uint8_t *>(jpeg.constData()), size_t(jpeg.size()), &c,
                      err.data(), err.size())
        != 0) {
        err.flushTo(error);
        return nullptr;
    }
    std::shared_ptr<Coefs> out(new Coefs);
    out->m_c = c;
    return out;
}

std::shared_ptr<Coefs> Coefs::clone() const
{
    jr_coefs *c = jr_coefs_clone(m_c);
    if (!c)
        return nullptr;
    std::shared_ptr<Coefs> out(new Coefs);
    out->m_c = c;
    return out;
}

namespace {

QVector<jr_op> toC(const QVector<Op> &ops)
{
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
    return cOps;
}

} // namespace

bool Coefs::apply(const QVector<Op> &ops, QString *error)
{
    if (ops.isEmpty())
        return true;
    const QVector<jr_op> cOps = toC(ops);
    ErrBuf err;
    if (jr_coefs_apply(m_c, cOps.constData(), size_t(cOps.size()), err.data(), err.size()) != 0) {
        err.flushTo(error);
        return false;
    }
    return true;
}

std::optional<QByteArray> Coefs::write(const QByteArray &headerSource, QString *error) const
{
    CBuffer out;
    size_t outLen = 0;
    ErrBuf err;
    if (jr_coefs_write(m_c, reinterpret_cast<const uint8_t *>(headerSource.constData()),
                       size_t(headerSource.size()), reinterpret_cast<uint8_t **>(&out.p), &outLen,
                       err.data(), err.size())
        != 0) {
        err.flushTo(error);
        return std::nullopt;
    }
    return QByteArray(static_cast<const char *>(out.p), qsizetype(outLen));
}

Info Coefs::info() const
{
    return fromC(m_c->info);
}

QVector<quint16> Coefs::quantTables() const
{
    QVector<quint16> out;
    out.reserve(m_c->info.num_components * 64);
    for (int c = 0; c < m_c->info.num_components; ++c)
        for (int i = 0; i < 64; ++i)
            out.append(m_c->quant[c][i]);
    return out;
}

const qint16 *Coefs::mcu(int index) const
{
    return reinterpret_cast<const qint16 *>(m_c->data)
        + qsizetype(index) * m_c->info.blocks_per_mcu * 64;
}

std::optional<Clipboard> Coefs::readMcus(int row, int col, int count, QString *error) const
{
    CBuffer coefs;
    size_t n = 0;
    ErrBuf err;
    if (jr_coefs_read_mcus(m_c, row, col, count, reinterpret_cast<int16_t **>(&coefs.p), &n,
                           err.data(), err.size())
        != 0) {
        err.flushTo(error);
        return std::nullopt;
    }
    const Info inf = info();
    Clipboard clip;
    clip.blocksPerMcu = inf.blocksPerMcu;
    clip.numComponents = inf.numComponents;
    for (int c = 0; c < JR_MAX_COMPONENTS; ++c) {
        clip.hSamp[c] = c < inf.numComponents ? inf.hSamp[c] : 0;
        clip.vSamp[c] = c < inf.numComponents ? inf.vSamp[c] : 0;
    }
    clip.quant = quantTables();
    clip.srcRow = row;
    clip.srcCol = col;
    clip.mcuCount = clip.blocksPerMcu > 0 ? int(n / (size_t(clip.blocksPerMcu) * 64)) : 0;
    clip.coefs = QByteArray(static_cast<const char *>(coefs.p), qsizetype(n * sizeof(qint16)));
    if (!clip.isValid()) {
        if (error)
            *error = QCoreApplication::translate("jr", "Could not read those MCUs.");
        return std::nullopt;
    }
    return clip;
}

namespace {

// Bands small enough to spread over every core, large enough that the
// per-band chroma context (a block row above and below) stays cheap.
constexpr int kBandMcuRows = 4;

struct Band {
    int row0, row1;
};

QVector<Band> bandsFor(const QVector<int> &rows)
{
    QVector<Band> out;
    for (int r : rows) {
        if (!out.isEmpty() && out.last().row1 == r && out.last().row1 - out.last().row0 < kBandMcuRows) {
            out.last().row1 = r + 1;
            continue;
        }
        out.append(Band{r, r + 1});
    }
    return out;
}

} // namespace

Samples Coefs::render(bool ycbcr) const
{
    Samples out;
    out.width = m_c->info.width;
    out.height = m_c->info.height;
    out.data = QByteArray(qsizetype(out.width) * out.height * 3, Qt::Uninitialized);
    QVector<int> rows(m_c->info.mcus_y);
    for (int r = 0; r < rows.size(); ++r)
        rows[r] = r;
    renderRows(out, rows, ycbcr);
    return out;
}

void Coefs::renderRows(Samples &target, const QVector<int> &mcuRows, bool ycbcr) const
{
    if (target.width != m_c->info.width || target.height != m_c->info.height
        || target.data.size() != qsizetype(target.width) * target.height * 3)
        return;
    QVector<int> rows = mcuRows;
    std::sort(rows.begin(), rows.end());
    rows.erase(std::unique(rows.begin(), rows.end()), rows.end());
    const QVector<Band> bands = bandsFor(rows);
    uint8_t *base = reinterpret_cast<uint8_t *>(target.data.data());
    const size_t stride = size_t(target.width) * 3;
    const jr_coefs *c = m_c;
    if (bands.size() <= 1) {
        for (const Band &b : bands)
            jr_coefs_render_rows(c, b.row0, b.row1, ycbcr ? 1 : 0, base, stride);
        return;
    }
    QtConcurrent::blockingMap(bands, [c, base, stride, ycbcr](const Band &b) {
        jr_coefs_render_rows(c, b.row0, b.row1, ycbcr ? 1 : 0, base, stride);
    });
}

QVector<int> Coefs::changedRows(const Coefs &other) const
{
    QVector<int> out;
    const jr_coefs *a = m_c, *b = other.m_c;
    if (a->info.mcus_x != b->info.mcus_x || a->info.mcus_y != b->info.mcus_y
        || a->info.blocks_per_mcu != b->info.blocks_per_mcu
        || std::memcmp(a->quant, b->quant, sizeof(a->quant)) != 0) {
        for (int r = 0; r < a->info.mcus_y; ++r)
            out.append(r);
        return out;
    }
    const size_t rowBytes = size_t(a->info.mcus_x) * size_t(a->info.blocks_per_mcu) * 64 * sizeof(int16_t);
    const char *pa = reinterpret_cast<const char *>(a->data);
    const char *pb = reinterpret_cast<const char *>(b->data);
    // The output DCs too: an edit up the image can move where a DC chain that
    // ran out of range gets pulled back in, rows away from the edit itself.
    const size_t dcRowBytes = size_t(a->info.mcus_x) * size_t(a->info.blocks_per_mcu) * sizeof(int16_t);
    const char *da = reinterpret_cast<const char *>(a->out_dc);
    const char *db = reinterpret_cast<const char *>(b->out_dc);
    QVector<char> dirty(a->info.mcus_y, 0);
    for (int r = 0; r < a->info.mcus_y; ++r) {
        if (std::memcmp(pa + size_t(r) * rowBytes, pb + size_t(r) * rowBytes, rowBytes) != 0
            || std::memcmp(da + size_t(r) * dcRowBytes, db + size_t(r) * dcRowBytes, dcRowBytes) != 0) {
            for (int d = -1; d <= 1; ++d) {
                if (r + d >= 0 && r + d < a->info.mcus_y)
                    dirty[r + d] = 1;
            }
        }
    }
    for (int r = 0; r < dirty.size(); ++r) {
        if (dirty[r])
            out.append(r);
    }
    return out;
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
            if (out.data.size() < n)
                out.trailerAt = out.data.size();
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
    const auto coefs = Coefs::load(jpeg, error);
    if (!coefs)
        return std::nullopt;
    return coefs->readMcus(row, col, count, error);
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

    const QVector<jr_op> cOps = toC(ops);

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