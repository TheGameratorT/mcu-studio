// SPDX-License-Identifier: GPL-3.0-only
// Part of MCU Studio. See LICENSE and THIRD-PARTY-NOTICES.md.

#include "Bitstream.h"

#include <QCoreApplication>
#include <QStringList>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstring>

#include <jpeglib.h>

#include "DctEdge.h"
#include "JpegStructure.h"

namespace bitstream {
namespace {

QString tr(const char *text)
{
    return QCoreApplication::translate("Bitstream", text);
}

// ---------------------------------------------------------------------------
// Huffman tables
// ---------------------------------------------------------------------------

struct Huff {
    bool present = false;
    // The classic decoding tables (JPEG Annex F.2.2.3): for each code length,
    // the largest code of that length and where its values start.
    std::array<int, 18> maxcode{};
    std::array<int, 17> valptr{};
    std::array<int, 17> mincode{};
    std::array<quint8, 256> vals{};

    void build(const quint8 bits[17], const quint8 *values, int count)
    {
        present = true;
        int code = 0, k = 0;
        for (int l = 1; l <= 16; ++l) {
            valptr[l] = k;
            mincode[l] = code;
            code += bits[l];
            k += bits[l];
            maxcode[l] = bits[l] ? code - 1 : -1;
            code <<= 1;
        }
        maxcode[17] = 0x7FFFFFFF;
        for (int i = 0; i < count && i < 256; ++i)
            vals[i] = values[i];
    }
};

struct Component {
    int id = 0, h = 1, v = 1;
    int dc = 0, ac = 0; // table slots, from the scan header
    int tq = 0;         // quantization table slot, from the frame header
};

// Zigzag position -> natural (row-major) index.
constexpr int kNatural[64] = {
    0,  1,  8,  16, 9,  2,  3,  10, 17, 24, 32, 25, 18, 11, 4,  5,
    12, 19, 26, 33, 40, 48, 41, 34, 27, 20, 13, 6,  7,  14, 21, 28,
    35, 42, 49, 56, 57, 50, 43, 36, 29, 22, 15, 23, 30, 37, 44, 51,
    58, 59, 52, 45, 38, 31, 39, 46, 53, 60, 61, 54, 47, 55, 62, 63};

struct Setup {
    QVector<Component> comps; // in scan order
    int blocksPerMcu = 0;
    int mcusX = 0, mcusY = 0;
    int restartInterval = 0;
    Huff dc[4], ac[4];
    int quant[4][64] = {}; // natural order
    jpegfile::Scan scan;
    QString unsupported;
};

void standardTables(Huff dc[4], Huff ac[4])
{
    // libjpeg's own copy of Annex K's tables, which is what a decoder falls
    // back on when a file (Motion JPEG, mostly) carries none.
    jpeg_compress_struct c;
    jpeg_error_mgr e;
    c.err = jpeg_std_error(&e);
    jpeg_create_compress(&c);
    c.in_color_space = JCS_RGB;
    c.input_components = 3;
    jpeg_set_defaults(&c);
    for (int i = 0; i < 2; ++i) {
        if (const JHUFF_TBL *t = c.dc_huff_tbl_ptrs[i]) {
            int n = 0;
            for (int l = 1; l <= 16; ++l)
                n += t->bits[l];
            dc[i].build(t->bits, t->huffval, n);
        }
        if (const JHUFF_TBL *t = c.ac_huff_tbl_ptrs[i]) {
            int n = 0;
            for (int l = 1; l <= 16; ++l)
                n += t->bits[l];
            ac[i].build(t->bits, t->huffval, n);
        }
    }
    jpeg_destroy_compress(&c);
}

int ceilDiv(int a, int b)
{
    return b > 0 ? (a + b - 1) / b : 0;
}

Setup setup(const QByteArray &jpeg, const jpegfile::Structure &s)
{
    Setup out;
    const auto *d = reinterpret_cast<const quint8 *>(jpeg.constData());
    if (s.scans.isEmpty()) {
        out.unsupported = tr("The file has no picture data to map.");
        return out;
    }
    if (s.frameMarker != 0xC0 && s.frameMarker != 0xC1) {
        out.unsupported = s.progressive()
            ? tr("Progressive files spread each block over several scans, so there is no single "
                 "position where an MCU starts.")
            : tr("Only Huffman-coded sequential files can be mapped.");
        return out;
    }
    standardTables(out.dc, out.ac);

    QVector<Component> frame;
    int maxH = 1, maxV = 1;
    const jpegfile::Scan &scan = s.scans.first();
    out.scan = scan;
    for (const jpegfile::Segment &seg : s.segments) {
        if (seg.offset >= scan.sosOffset)
            break;
        const quint8 *p = d + seg.dataOffset;
        if (seg.marker == 0xDB) {
            qsizetype i = 0;
            while (i < seg.dataLength) {
                const int pq = p[i] >> 4, tq = p[i] & 3;
                const qsizetype size = 1 + (pq ? 128 : 64);
                if (i + size > seg.dataLength)
                    break;
                for (int k = 0; k < 64; ++k)
                    out.quant[tq][kNatural[k]] = pq ? (p[i + 1 + 2 * k] << 8 | p[i + 2 + 2 * k]) : p[i + 1 + k];
                i += size;
            }
        } else if (seg.marker == 0xC4) {
            qsizetype i = 0;
            while (i + 17 <= seg.dataLength) {
                const int cls = p[i] >> 4, id = p[i] & 15;
                quint8 bits[17] = {0};
                int count = 0;
                for (int l = 1; l <= 16; ++l) {
                    bits[l] = p[i + l];
                    count += bits[l];
                }
                if (id > 3 || cls > 1 || i + 17 + count > seg.dataLength || count > 256)
                    break;
                (cls == 0 ? out.dc[id] : out.ac[id]).build(bits, p + i + 17, count);
                i += 17 + count;
            }
        } else if (seg.offset == s.frameOffset && seg.dataLength >= 6) {
            const int n = p[5];
            for (int c = 0; c < n && 6 + 3 * c + 2 < seg.dataLength; ++c) {
                Component comp;
                comp.id = p[6 + 3 * c];
                comp.h = qMax(1, p[7 + 3 * c] >> 4);
                comp.v = qMax(1, p[7 + 3 * c] & 15);
                comp.tq = p[8 + 3 * c] & 3;
                maxH = qMax(maxH, comp.h);
                maxV = qMax(maxV, comp.v);
                frame.append(comp);
            }
        }
    }
    out.restartInterval = s.restartInterval;

    const qsizetype sos = scan.sosOffset + 4;
    const int n = d[sos];
    if (n < 1 || n > 4) {
        out.unsupported = tr("The scan header is damaged.");
        return out;
    }
    for (int i = 0; i < n; ++i) {
        const int id = d[sos + 1 + 2 * i];
        const int tables = d[sos + 2 + 2 * i];
        auto it = std::find_if(frame.begin(), frame.end(), [id](const Component &c) { return c.id == id; });
        if (it == frame.end()) {
            out.unsupported = tr("The scan names a component the frame does not have.");
            return out;
        }
        Component c = *it;
        c.dc = (tables >> 4) & 3;
        c.ac = tables & 3;
        if (!out.dc[c.dc].present || !out.ac[c.ac].present) {
            out.unsupported = tr("The scan uses a Huffman table the file never defines.");
            return out;
        }
        out.comps.append(c);
    }
    if (n == 1) {
        // A single-component scan codes that component's real blocks one by
        // one, with no padding to whole MCUs.
        Component &c = out.comps.first();
        const int compW = ceilDiv(s.width * c.h, maxH);
        const int compH = ceilDiv(s.height * c.v, maxV);
        out.mcusX = ceilDiv(compW, 8);
        out.mcusY = ceilDiv(compH, 8);
        c.h = c.v = 1;
        out.blocksPerMcu = 1;
        if (frame.size() > 1) {
            out.unsupported = tr("This file stores its components in separate scans, so MCU "
                                 "positions do not line up with the picture grid.");
            return out;
        }
    } else {
        if (n != frame.size()) {
            out.unsupported = tr("The first scan carries only some of the components.");
            return out;
        }
        out.mcusX = ceilDiv(s.width, 8 * maxH);
        out.mcusY = ceilDiv(s.height, 8 * maxV);
        for (const Component &c : out.comps)
            out.blocksPerMcu += c.h * c.v;
    }
    return out;
}

// ---------------------------------------------------------------------------
// The unstuffed stream
// ---------------------------------------------------------------------------

struct Rst {
    qsizetype raw; // raw byte index the marker sits in front of
    int number;
};

struct Raw {
    qsizetype stoppedAt = 0; // file offset where reading stopped
    QByteArray bytes;
    QVector<qsizetype> fileOffset; // per raw byte
    QVector<Rst> rsts;

    qint64 bits() const { return qint64(bytes.size()) * 8; }
    // Raw byte index of file offset `at`, or -1 if `at` is not a data byte.
    qsizetype rawIndexOf(qsizetype at) const
    {
        auto it = std::lower_bound(fileOffset.begin(), fileOffset.end(), at);
        if (it == fileOffset.end() || *it != at)
            return -1;
        return it - fileOffset.begin();
    }
    int rstAt(qsizetype raw) const
    {
        auto it = std::lower_bound(rsts.begin(), rsts.end(), raw,
                                   [](const Rst &r, qsizetype v) { return r.raw < v; });
        return it != rsts.end() && it->raw == raw ? it->number : -1;
    }
    qsizetype nextRst(qsizetype raw) const
    {
        auto it = std::lower_bound(rsts.begin(), rsts.end(), raw,
                                   [](const Rst &r, qsizetype v) { return r.raw < v; });
        return it == rsts.end() ? -1 : it->raw;
    }
};

// Unstuffs a scan's data. `end` is where to stop: the scan's own end for a
// strict reading, or further for a lenient one, which reads on through bytes
// no encoder writes (taking an FF there as a literal) so that an edit or a
// search can reach past a stretch of garbage to the data behind it.
Raw unstuff(const QByteArray &jpeg, qsizetype start, qsizetype end, bool lenient)
{
    Raw out;
    const auto *d = reinterpret_cast<const quint8 *>(jpeg.constData());
    end = qMin(end, jpeg.size());
    out.bytes.reserve(end - start);
    out.fileOffset.reserve(end - start);
    qsizetype p = start;
    for (; p < end; ++p) {
        if (d[p] != 0xFF) {
            out.bytes.append(char(d[p]));
            out.fileOffset.append(p);
            continue;
        }
        if (p + 1 >= end)
            break;
        const quint8 next = d[p + 1];
        if (next == 0x00) {
            out.bytes.append(char(0xFF));
            out.fileOffset.append(p);
            ++p;
        } else if (next >= 0xD0 && next <= 0xD7) {
            out.rsts.append(Rst{out.bytes.size(), next - 0xD0});
            ++p;
        } else if (next == 0xFF) {
            // fill byte: the next FF starts the marker
        } else if (lenient && next != 0xD9) {
            out.bytes.append(char(0xFF));
            out.fileOffset.append(p);
        } else {
            break;
        }
    }
    out.stoppedAt = qMin(p, end);
    return out;
}

Raw unstuff(const QByteArray &jpeg, const jpegfile::Scan &scan)
{
    return unstuff(jpeg, scan.dataStart, scan.dataEnd, false);
}

// Where a lenient reading of the scan starting at `scan` ends: the next scan,
// or the file's last EOI, or the end of the file.
qsizetype lenientEnd(const QByteArray &jpeg, const jpegfile::Structure &s, int scanIndex)
{
    if (scanIndex + 1 < s.scans.size())
        return s.scans[scanIndex + 1].sosOffset;
    const qsizetype eoi = jpeg.lastIndexOf(QByteArray("\xFF\xD9", 2));
    return eoi > s.scans[scanIndex].dataStart ? eoi : jpeg.size();
}

// ---------------------------------------------------------------------------
// Decoding
// ---------------------------------------------------------------------------

// Reads bits from a Raw, optionally as though `del` bits at `editPos` were
// gone or `ins` zero bits were there.
struct Reader {
    const Raw *raw = nullptr;
    qint64 pos = 0; // logical
    qint64 editPos = -1;
    int del = 0, ins = 0;
    bool overran = false;

    qint64 physical(qint64 l) const
    {
        if (editPos < 0 || l < editPos)
            return l;
        if (l < editPos + ins)
            return -1; // an inserted zero
        return l - ins + del;
    }
    qint64 logicalOf(qint64 p) const
    {
        if (editPos < 0 || p < editPos)
            return p;
        return p + ins - del;
    }
    int bit()
    {
        const qint64 p = physical(pos++);
        if (p < 0)
            return 0;
        if (p >= raw->bits()) {
            overran = true;
            return 0;
        }
        return (quint8(raw->bytes.at(qsizetype(p >> 3))) >> (7 - (p & 7))) & 1;
    }
    int bits(int n)
    {
        int v = 0;
        for (int i = 0; i < n; ++i)
            v = (v << 1) | bit();
        return v;
    }
    int decode(const Huff &h)
    {
        int code = bit();
        int l = 1;
        while (l <= 16 && (h.maxcode[l] < 0 || code > h.maxcode[l])) {
            code = (code << 1) | bit();
            ++l;
        }
        if (l > 16)
            return -1;
        const int idx = h.valptr[l] + code - h.mincode[l];
        return idx >= 0 && idx < 256 ? h.vals[idx] : -1;
    }
};

int extend(int v, int t)
{
    return t == 0 ? 0 : (v < (1 << (t - 1)) ? v - (1 << t) + 1 : v);
}

// Decodes one MCU, returning its anomaly bits. `pred` (one per scan
// component) carries the DC predictors across MCUs; `dcFirst`, when given,
// receives each component's first DC value.
// `luma`, when given, receives the first component's blocks (h*v of them,
// 64 coefficients each in natural order, quantized).
quint8 decodeMcu(const Setup &s, Reader &r, int *pred, int *dcFirst = nullptr, qint16 *luma = nullptr)
{
    quint8 flags = 0;
    for (int ci = 0; ci < s.comps.size(); ++ci) {
        const Component &c = s.comps[ci];
        for (int b = 0; b < c.h * c.v; ++b) {
            qint16 *out = (ci == 0 && luma) ? luma + 64 * b : nullptr;
            if (out)
                std::fill(out, out + 64, qint16(0));
            const int t = r.decode(s.dc[c.dc]);
            if (t < 0)
                return flags | BadCode;
            if (t > 11)
                flags |= ValueRange;
            pred[ci] += extend(r.bits(t), qMin(t, 16));
            if (b == 0 && dcFirst)
                dcFirst[ci] = pred[ci];
            if (out)
                out[0] = qint16(qBound(-32768, pred[ci], 32767));
            for (int k = 1; k < 64;) {
                const int rs = r.decode(s.ac[c.ac]);
                if (rs < 0)
                    return flags | BadCode;
                const int run = rs >> 4, size = rs & 15;
                if (size) {
                    k += run;
                    if (k > 63)
                        return flags | CoefOverflow;
                    if (size > 10)
                        flags |= ValueRange;
                    const int v = extend(r.bits(size), size);
                    if (out)
                        out[kNatural[k]] = qint16(v);
                    ++k;
                } else if (run == 15) {
                    k += 16;
                    if (k > 64)
                        return flags | CoefOverflow;
                } else {
                    break; // end of block
                }
            }
        }
    }
    if (r.overran)
        flags |= Truncated;
    return flags;
}

// Whether a restart marker sits inside the logical bit range [from, to).
bool rstInside(const Raw &raw, const Reader &r, qint64 from, qint64 to)
{
    const qint64 pf = r.physical(from), pt = r.physical(to);
    if (pf < 0 || pt < 0)
        return false;
    const qsizetype next = raw.nextRst(qsizetype((pf + 7) >> 3));
    return next >= 0 && qint64(next) * 8 < pt && qint64(next) * 8 > pf;
}

qint64 fileBitPos(const Raw &raw, qint64 physicalBit)
{
    const qsizetype byte = qsizetype(physicalBit >> 3);
    if (byte < 0 || byte >= raw.fileOffset.size())
        return raw.fileOffset.isEmpty() ? 0 : qint64(raw.fileOffset.last() + 1) * 8;
    return qint64(raw.fileOffset.at(byte)) * 8 + (physicalBit & 7);
}

} // namespace

int Map::mcuAtByte(qsizetype offset) const
{
    const qint64 bit = qint64(offset) * 8 + 7;
    auto it = std::upper_bound(mcus.begin(), mcus.end(), bit,
                               [](qint64 v, const McuRecord &m) { return v < m.bitPos; });
    if (it == mcus.begin())
        return -1;
    return int(it - mcus.begin()) - 1;
}

QString describe(quint8 a)
{
    QStringList out;
    if (a & BadCode)
        out << tr("invalid Huffman code");
    if (a & CoefOverflow)
        out << tr("coefficients past the end of the block");
    if (a & MarkerInMcu)
        out << tr("marker inside the MCU");
    if (a & RestartOrder)
        out << tr("restart marker out of order");
    if (a & RestartMissing)
        out << tr("restart marker missing");
    if (a & Truncated)
        out << tr("data ran out");
    if (a & ValueRange)
        out << tr("value out of range");
    return out.join(QStringLiteral(", "));
}

Map map(const QByteArray &jpeg)
{
    Map out;
    const jpegfile::Structure s = jpegfile::walk(jpeg);
    const Setup st = setup(jpeg, s);
    if (!st.unsupported.isEmpty()) {
        out.unsupported = st.unsupported;
        return out;
    }
    out.mcusX = st.mcusX;
    out.mcusY = st.mcusY;
    out.restartInterval = st.restartInterval;
    out.scanStart = st.scan.dataStart;
    out.scanEnd = st.scan.dataEnd;
    out.components = st.comps.size();

    const Raw raw = unstuff(jpeg, st.scan);
    Reader r;
    r.raw = &raw;
    const int total = st.mcusX * st.mcusY;
    const int nc = out.components;
    out.mcus.reserve(total);
    out.predIn.fill(0, qsizetype(total) * nc);
    out.dc.fill(0, qsizetype(total) * nc);
    int pred[4] = {0, 0, 0, 0};
    int expectedRst = 0;
    quint8 pending = 0;
    for (int m = 0; m < total; ++m) {
        if (st.restartInterval > 0 && m > 0 && m % st.restartInterval == 0) {
            std::fill(pred, pred + 4, 0);
            r.pos = (r.pos + 7) & ~qint64(7);
            const qsizetype at = qsizetype(r.pos >> 3);
            const int num = raw.rstAt(at);
            if (num < 0) {
                // Where libjpeg would discard data until the next marker.
                const qsizetype next = raw.nextRst(at);
                pending |= RestartMissing;
                if (next >= 0)
                    r.pos = qint64(next) * 8;
            } else if (num != expectedRst) {
                pending |= RestartOrder;
            }
            expectedRst = (expectedRst + 1) & 7;
        }
        McuRecord rec;
        const qint64 start = r.pos;
        rec.bitPos = fileBitPos(raw, start);
        for (int c = 0; c < nc; ++c)
            out.predIn[qsizetype(m) * nc + c] = pred[c];
        rec.anomalies = decodeMcu(st, r, pred, out.dc.data() + qsizetype(m) * nc) | pending;
        pending = 0;
        if (st.restartInterval > 0 && rstInside(raw, r, start, r.pos))
            rec.anomalies |= MarkerInMcu;
        rec.bits = int(qMin<qint64>(r.pos - start, 1 << 30));
        if (rec.anomalies) {
            ++out.anomalyCount;
            if (out.firstAnomaly < 0)
                out.firstAnomaly = m;
        }
        out.mcus.append(rec);
        if (r.overran) {
            // Nothing past here: account for the rest as truncated, once.
            for (int k = m + 1; k < total; ++k) {
                McuRecord t;
                t.bitPos = rec.bitPos + rec.bits;
                t.anomalies = Truncated;
                out.mcus.append(t);
            }
            out.anomalyCount += total - m - 1;
            break;
        }
    }
    return out;
}

// ---------------------------------------------------------------------------
// Editing
// ---------------------------------------------------------------------------

namespace {

struct Located {
    jpegfile::Scan scan;
    Raw raw;
    qsizetype rawByte = -1;
    int bit = 0;
};

std::optional<Located> locate(const QByteArray &jpeg, qint64 bitPos, QString *error)
{
    const jpegfile::Structure s = jpegfile::walk(jpeg);
    const qsizetype byte = qsizetype(bitPos >> 3);
    for (int i = s.scans.size() - 1; i >= 0; --i) {
        const jpegfile::Scan &scan = s.scans[i];
        if (byte < scan.dataStart)
            continue;
        const qsizetype end = lenientEnd(jpeg, s, i);
        if (byte >= end)
            break;
        Located l;
        l.scan = scan;
        l.scan.dataEnd = end;
        l.raw = unstuff(jpeg, scan.dataStart, end, true);
        l.scan.dataEnd = l.raw.stoppedAt;
        l.rawByte = l.raw.rawIndexOf(byte);
        l.bit = int(bitPos & 7);
        if (l.rawByte < 0) {
            if (error)
                *error = tr("Byte %1 is part of a stuffed FF 00 or a marker, not picture data.").arg(byte);
            return std::nullopt;
        }
        return l;
    }
    if (error)
        *error = tr("Byte %1 is not inside any scan's picture data.").arg(byte);
    return std::nullopt;
}

// Rebuilds a scan's stored bytes from a Raw whose bytes were edited between
// raw indices [segStart, oldSegEnd), now [segStart, newSegEnd).
QByteArray restuff(const Raw &raw, const QByteArray &bytes, qsizetype segStart, qsizetype shift)
{
    QByteArray out;
    out.reserve(bytes.size() + bytes.size() / 64 + 16);
    int ri = 0;
    for (qsizetype i = 0; i <= bytes.size(); ++i) {
        // Restart markers at or before this byte, in the raw index space after
        // the edit: those past the edited segment moved by `shift`.
        while (ri < raw.rsts.size()) {
            const qsizetype at = raw.rsts[ri].raw > segStart ? raw.rsts[ri].raw + shift : raw.rsts[ri].raw;
            if (at != i)
                break;
            out.append(char(0xFF));
            out.append(char(0xD0 + raw.rsts[ri].number));
            ++ri;
        }
        if (i == bytes.size())
            break;
        const char c = bytes.at(i);
        out.append(c);
        if (quint8(c) == 0xFF)
            out.append(char(0x00));
    }
    return out;
}

std::optional<QByteArray> editBits(const QByteArray &jpeg, qint64 bitPos, int del, int ins, int bit,
                                   QString *error)
{
    auto l = locate(jpeg, bitPos, error);
    if (!l)
        return std::nullopt;
    const Raw &raw = l->raw;
    // The restart interval the edit lands in: bits do not flow across a
    // restart marker, because the encoder byte-aligned the stream there.
    qsizetype segStart = 0;
    for (const Rst &r : raw.rsts) {
        if (r.raw <= l->rawByte)
            segStart = r.raw;
    }
    qsizetype segEnd = raw.nextRst(l->rawByte + 1);
    if (segEnd < 0)
        segEnd = raw.bytes.size();

    QVector<quint8> bits;
    bits.reserve((segEnd - segStart) * 8 + ins);
    for (qsizetype i = segStart; i < segEnd; ++i)
        for (int b = 7; b >= 0; --b)
            bits.append((quint8(raw.bytes.at(i)) >> b) & 1);
    const qsizetype at = (l->rawByte - segStart) * 8 + l->bit;
    if (del > 0) {
        if (at + del > bits.size()) {
            if (error)
                *error = tr("There are fewer than %1 bits left before the next restart marker.").arg(del);
            return std::nullopt;
        }
        bits.remove(at, del);
    }
    for (int i = 0; i < ins; ++i)
        bits.insert(at, quint8(bit & 1));
    while (bits.size() % 8 != 0)
        bits.append(1); // an encoder pads a segment's last byte with 1-bits

    QByteArray seg;
    seg.reserve(bits.size() / 8);
    for (qsizetype i = 0; i < bits.size(); i += 8) {
        int v = 0;
        for (int b = 0; b < 8; ++b)
            v = (v << 1) | bits[i + b];
        seg.append(char(v));
    }
    QByteArray rawBytes = raw.bytes;
    rawBytes.replace(segStart, segEnd - segStart, seg);
    const qsizetype shift = seg.size() - (segEnd - segStart);

    QByteArray out = jpeg.left(l->scan.dataStart);
    out += restuff(raw, rawBytes, segStart, shift);
    out += jpeg.mid(l->scan.dataEnd);
    return out;
}

} // namespace

std::optional<QByteArray> deleteBits(const QByteArray &jpeg, qint64 bitPos, int count, QString *error)
{
    if (count <= 0)
        return jpeg;
    return editBits(jpeg, bitPos, count, 0, 0, error);
}

std::optional<QByteArray> insertBits(const QByteArray &jpeg, qint64 bitPos, int count, int bit,
                                     QString *error)
{
    if (count <= 0)
        return jpeg;
    return editBits(jpeg, bitPos, 0, count, bit, error);
}

std::optional<QByteArray> flipBit(const QByteArray &jpeg, qint64 bitPos, QString *error)
{
    const qsizetype byte = qsizetype(bitPos >> 3);
    if (byte < 0 || byte >= jpeg.size()) {
        if (error)
            *error = tr("Byte %1 is outside the file.").arg(byte);
        return std::nullopt;
    }
    QByteArray out = jpeg;
    out[byte] = char(quint8(out[byte]) ^ (0x80 >> (bitPos & 7)));
    return out;
}

// ---------------------------------------------------------------------------
// Searching
// ---------------------------------------------------------------------------

namespace {

// One number for how wrong an edit leaves the picture, in tenths of a sample
// level: the DC offset it leaves on everything after it, weighted heavily
// because it spreads across the rest of the image, plus how badly the first
// MCUs' edges meet their neighbors, where garbage left inside them shows.
// Neither alone is enough: at high quality a DC step is a fraction of a
// level and the offset drowns in texture, while flat garbage can meet its
// neighbors more smoothly than real detail does.
int resyncScore(const Candidate &c)
{
    constexpr double kDcWeight = 4.0;
    return qRound((kDcWeight * c.dcStep + c.edgeCost) * 10);
}

void blockEdge(const qint16 *coef, const int *q, int which, double out[8])
{
    dctedge::samples(coef, q, dctedge::Edge(which), out);
}

// One edge of an MCU's luma: its blocks' edges side by side.
QVector<double> mcuEdge(const qint16 *luma, int h, int v, const int *q, int which)
{
    QVector<double> out;
    double e[8];
    if (which < 2) {
        const int row = which == 0 ? 0 : v - 1;
        for (int bx = 0; bx < h; ++bx) {
            blockEdge(luma + 64 * (row * h + bx), q, which, e);
            out.append(QVector<double>(e, e + 8));
        }
    } else {
        const int col = which == 2 ? 0 : h - 1;
        for (int by = 0; by < v; ++by) {
            blockEdge(luma + 64 * (by * h + col), q, which, e);
            out.append(QVector<double>(e, e + 8));
        }
    }
    return out;
}

double seam(const QVector<double> &a, const QVector<double> &b)
{
    double sum = 0;
    for (int i = 0; i < a.size() && i < b.size(); ++i)
        sum += std::abs(a[i] - b[i]);
    return a.isEmpty() ? 0 : sum / a.size();
}

} // namespace

QVector<Candidate> searchResync(const QByteArray &jpeg, qint64 bitPos, int maxBits, int maxBytes,
                                int window)
{
    QVector<Candidate> out;
    const jpegfile::Structure s = jpegfile::walk(jpeg);
    const Setup st = setup(jpeg, s);
    if (!st.unsupported.isEmpty())
        return out;
    auto l = locate(jpeg, bitPos, nullptr);
    if (!l || l->scan.dataStart != st.scan.dataStart)
        return out;
    const Raw &raw = l->raw;
    const qint64 start = qint64(l->rawByte) * 8 + l->bit;
    // A decode that reaches the next restart marker exactly on a byte
    // boundary, after a whole number of MCUs, is the strongest evidence there
    // is that the edit was right.
    const qsizetype nextRst = raw.nextRst(l->rawByte + 1);

    // The MCU the edit point starts, and what the stream before it says:
    // the DC predictors going in, and the DC values of the row above.
    const Map before = map(jpeg);
    const int nc = st.comps.size();
    int m0 = -1;
    for (int i = 0; i < before.mcus.size(); ++i) {
        if (before.mcus[i].bitPos == bitPos) {
            m0 = i;
            break;
        }
    }
    if (m0 < 0)
        m0 = qMax(0, before.mcuAtByte(qsizetype(bitPos >> 3)));
    const int W = st.mcusX;
    const Component &luma0 = st.comps.first();
    const int lumaBlocks = luma0.h * luma0.v;
    const int *lq = st.quant[luma0.tq];
    // The luma of an MCU before the edit point, decoded where the map says it
    // starts.
    const auto lumaOf = [&](int m, QVector<qint16> *out) {
        if (m < 0 || m >= m0 || m >= before.mcus.size())
            return false;
        const qsizetype rawByte = raw.rawIndexOf(qsizetype(before.mcus[m].bitPos >> 3));
        if (rawByte < 0)
            return false;
        Reader r;
        r.raw = &raw;
        r.pos = qint64(rawByte) * 8 + (before.mcus[m].bitPos & 7);
        int pred[4] = {0, 0, 0, 0};
        for (int k = 0; k < nc; ++k)
            pred[k] = before.predIn.value(qsizetype(m) * nc + k);
        out->resize(64 * lumaBlocks);
        return decodeMcu(st, r, pred, nullptr, out->data()) == 0;
    };
    QVector<qint16> leftLuma, aboveLuma, aboveNextLuma;
    const bool haveLeft = (m0 % W) > 0 && lumaOf(m0 - 1, &leftLuma);
    const bool haveAbove = lumaOf(m0 - W, &aboveLuma);
    const bool haveAboveNext = (m0 + 1) % W != 0 && lumaOf(m0 + 1 - W, &aboveNextLuma);
    const QVector<double> leftEdge = haveLeft ? mcuEdge(leftLuma.constData(), luma0.h, luma0.v, lq, 3) : QVector<double>();
    const QVector<double> aboveEdge = haveAbove ? mcuEdge(aboveLuma.constData(), luma0.h, luma0.v, lq, 1) : QVector<double>();
    const QVector<double> aboveNextEdge = haveAboveNext ? mcuEdge(aboveNextLuma.constData(), luma0.h, luma0.v, lq, 1) : QVector<double>();

    // What the DC of MCU `here`, component k, should be, judged from the
    // untouched MCUs before m0: carried on from the two rows above (which
    // follows a gradient as well as a flat area), or from the two MCUs to the
    // left on the first row.
    const auto expected = [&](int here, int k, bool *ok) -> double {
        const auto dcOf = [&](int m) { return double(before.dc.value(qsizetype(m) * nc + k)); };
        *ok = true;
        if (here - 2 * W >= 0 && here - W < m0)
            return 2 * dcOf(here - W) - dcOf(here - 2 * W);
        if (here - W >= 0 && here - W < m0)
            return dcOf(here - W);
        if (here - 2 >= 0 && here - 1 < m0)
            return 2 * dcOf(here - 1) - dcOf(here - 2);
        *ok = false;
        return 0;
    };

    const auto evaluate = [&](int delta, qint64 editAt, int limit) {
        Reader r;
        r.raw = &raw;
        r.pos = start;
        r.editPos = editAt;
        r.del = delta > 0 ? delta : 0;
        r.ins = delta < 0 ? -delta : 0;
        Candidate c;
        c.deltaBits = delta;
        c.bitPos = fileBitPos(raw, editAt);
        int pred[4] = {0, 0, 0, 0};
        for (int k = 0; k < nc && m0 >= 0 && qsizetype(m0) * nc + k < before.predIn.size(); ++k)
            pred[k] = before.predIn[qsizetype(m0) * nc + k];
        QVector<double> steps[4];
        QVector<int> bits;
        QVector<qint16> first(64 * lumaBlocks), second(64 * lumaBlocks);
        for (int m = 0; m < limit; ++m) {
            const qint64 at = r.pos;
            int dc[4] = {0, 0, 0, 0};
            const quint8 a = decodeMcu(st, r, pred, dc, m == 0 ? first.data() : m == 1 ? second.data() : nullptr);
            if (a)
                break;
            bits.append(int(r.pos - at));
            for (int k = 0; k < nc; ++k) {
                bool ok = false;
                const double e = expected(m0 + m, k, &ok);
                if (ok)
                    steps[k].append(dc[k] - e);
            }
            if (nextRst >= 0) {
                const qint64 p = r.physical(r.pos);
                if (p > qint64(nextRst) * 8)
                    break; // ran through the marker
                if (((p + 7) & ~qint64(7)) == qint64(nextRst) * 8) {
                    ++c.cleanMcus;
                    c.reachedEnd = true;
                    break;
                }
            }
            if (r.pos == at)
                break;
            ++c.cleanMcus;
        }
        if (c.cleanMcus >= window)
            c.reachedEnd = true;
        for (int k = 0; k < nc; ++k) {
            if (steps[k].isEmpty())
                continue;
            // The signed median: natural texture scatters either side of the
            // prediction and cancels out, while garbage left in the
            // predictors shifts every value the same way and does not. In
            // sample levels, so components with different steps add up.
            auto mid = steps[k].begin() + steps[k].size() / 2;
            std::nth_element(steps[k].begin(), mid, steps[k].end());
            c.dcStep += std::abs(*mid) * st.quant[st.comps[k].tq][0] / 8.0;
        }
        if (bits.size() >= 4) {
            const int firstBits = bits[0] + bits[1];
            QVector<int> sorted = bits;
            auto mid = sorted.begin() + sorted.size() / 2;
            std::nth_element(sorted.begin(), mid, sorted.end());
            c.excessBits = qMax(0, firstBits - 2 * *mid);
        }
        // The seams of the first two MCUs against what surrounds them: the
        // places garbage left inside them shows.
        if (c.cleanMcus >= 2) {
            double cost = 0;
            if (haveLeft)
                cost += seam(leftEdge, mcuEdge(first.constData(), luma0.h, luma0.v, lq, 2));
            if (haveAbove)
                cost += seam(aboveEdge, mcuEdge(first.constData(), luma0.h, luma0.v, lq, 0));
            if ((m0 + 1) % W != 0)
                cost += seam(mcuEdge(first.constData(), luma0.h, luma0.v, lq, 3),
                             mcuEdge(second.constData(), luma0.h, luma0.v, lq, 2));
            if (haveAboveNext)
                cost += seam(aboveNextEdge, mcuEdge(second.constData(), luma0.h, luma0.v, lq, 0));
            c.edgeCost = cost;
        }
        return c;
    };

    // Screening: a short decode for every candidate, which is all a wrong one
    // ever gets before it fails. The few that survive with the best DC are
    // then decoded over the whole window.
    const int screen = qMin(window, qMax(48, qMin(W, 160)));
    QVector<Candidate> screened;
    const auto consider = [&](int delta, qint64 at) {
        const Candidate c = evaluate(delta, at, screen);
        if (c.cleanMcus >= qMin(4, screen) || delta == 0)
            screened.append(c);
    };
    consider(0, start);
    for (int b = 1; b <= maxBits; ++b) {
        consider(b, start);
        consider(-b, start);
    }
    // Longer deletions, bit by bit. Garbage that landed in the middle of an
    // MCU cannot be cut out exactly from the MCU's start, but deleting the
    // rest of that MCU along with the garbage lands the decoder on the next
    // intact MCU boundary -- after which the stream decodes cleanly again, one
    // or more MCUs short, which a whole-MCU insert then puts back.
    for (qint64 b = qint64(maxBits) + 1; b <= qint64(maxBytes) * 8; ++b)
        consider(int(b), start);
    // Garbage from a bad sector or a botched copy is whole bytes, inserted at
    // a byte boundary somewhere between the MCU's start and the first point
    // the decoder choked. Cutting exactly those bytes restores the stream
    // bit for bit, so try every byte-aligned position in that stretch with
    // every whole-byte length.
    qint64 limit = start + 8 * 64;
    for (int i = qMax(0, m0); i < before.mcus.size(); ++i) {
        if (before.mcus[i].anomalies) {
            const qsizetype raw0 = raw.rawIndexOf(qsizetype(before.mcus[i].bitPos >> 3));
            if (raw0 >= 0)
                limit = qMin(limit, qint64(raw0) * 8 + before.mcus[i].bits + 8);
            break;
        }
    }
    for (qint64 at = (start + 7) & ~qint64(7); at <= limit; at += 8)
        for (int bytes = 1; bytes <= maxBytes; ++bytes)
            consider(bytes * 8, at);

    const auto screenedClean = [screen](const Candidate &c) { return c.cleanMcus >= screen * 9 / 10; };
    std::stable_sort(screened.begin(), screened.end(), [&](const Candidate &a, const Candidate &b) {
        if (screenedClean(a) != screenedClean(b))
            return screenedClean(a);
        return resyncScore(a) < resyncScore(b);
    });
    constexpr int kFinalists = 64;
    for (int i = 0; i < screened.size() && i < kFinalists; ++i) {
        const qint64 raw0 = raw.rawIndexOf(qsizetype(screened[i].bitPos >> 3));
        out.append(evaluate(screened[i].deltaBits, qint64(raw0) * 8 + (screened[i].bitPos & 7), window));
    }

    // Huffman codes resynchronize on their own, so many edits near the right
    // one also decode "cleanly" after a few garbage symbols. What they cannot
    // do is undo the garbage's effect on the DC predictors: every later block
    // inherits it. The right edit is the clean one whose DC continues the
    // untouched row above.
    const auto clean = [window](const Candidate &c) {
        return c.reachedEnd || c.cleanMcus >= window * 9 / 10;
    };
    const auto clean_ = clean;
    std::stable_sort(out.begin(), out.end(), [&](const Candidate &a, const Candidate &b) {
        if (clean_(a) != clean_(b))
            return clean_(a);
        if (!clean_(a) && a.cleanMcus != b.cleanMcus)
            return a.cleanMcus > b.cleanMcus;
        const int sa = resyncScore(a), sb = resyncScore(b);
        if (sa != sb)
            return sa < sb;
        if (a.excessBits != b.excessBits)
            return a.excessBits < b.excessBits;
        return std::abs(a.deltaBits) < std::abs(b.deltaBits);
    });
    return out;
}

} // namespace bitstream
