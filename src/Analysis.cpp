// SPDX-License-Identifier: GPL-3.0-only
// Part of MCU Studio. See LICENSE and THIRD-PARTY-NOTICES.md.

#include "Analysis.h"

#include <QCoreApplication>

#include <algorithm>
#include <cmath>

#include "DctEdge.h"
#include "JpegStructure.h"

namespace analysis {
namespace {

// The first and last pixel rows (and columns) of every MCU, clipped to the
// picture. Taken from the render, so chroma is the upsampled chroma the
// viewer sees.
struct Edges {
    int mcusX = 0, mcusY = 0, mw = 0, mh = 0, width = 0, height = 0;
    const jr::Samples *img = nullptr;

    int validWidth(int col) const { return std::min(mw, width - col * mw); }
    int validHeight(int row) const { return std::min(mh, height - row * mh); }
    // Sample `c` at pixel (x, y) of the picture.
    int at(int x, int y, int c) const { return img->pixel(x, y)[c]; }
};

Edges edgesFor(const jr::Info &info, const jr::Samples &ycbcr)
{
    Edges e;
    e.mcusX = info.mcusX;
    e.mcusY = info.mcusY;
    e.mw = info.mcuWidth;
    e.mh = info.mcuHeight;
    e.width = ycbcr.width;
    e.height = ycbcr.height;
    e.img = &ycbcr;
    return e;
}

double median(QVector<double> v)
{
    if (v.isEmpty())
        return 0;
    const auto mid = v.begin() + v.size() / 2;
    std::nth_element(v.begin(), mid, v.end());
    return *mid;
}

} // namespace

DamageMap damage(const jr::Info &info, const jr::Samples &ycbcr, const bitstream::Map *map,
                 const QVector<qint32> *unitSrc)
{
    DamageMap out;
    if (!info.isValid() || !ycbcr.isValid() || ycbcr.width != info.width)
        return out;
    const Edges e = edgesFor(info, ycbcr);
    out.mcusX = info.mcusX;
    out.mcusY = info.mcusY;
    out.score.fill(0.f, info.mcuCount());

    for (int row = 0; row < info.mcusY; ++row) {
        for (int col = 0; col < info.mcusX; ++col) {
            const int m = row * info.mcusX + col;
            const int x0 = col * e.mw, y0 = row * e.mh;
            const int w = e.validWidth(col), h = e.validHeight(row);
            if (w <= 0 || h <= 0)
                continue;

            // The picture's own detail inside the MCU, row to row and column
            // to column: what an edge between two good MCUs looks like here.
            double inner = 0;
            int innerN = 0;
            for (int y = y0 + 1; y < y0 + h; y += std::max(1, h / 4))
                for (int x = x0; x < x0 + w; ++x, ++innerN)
                    inner += std::abs(e.at(x, y, 0) - e.at(x, y - 1, 0));
            for (int x = x0 + 1; x < x0 + w; x += std::max(1, w / 4))
                for (int y = y0; y < y0 + h; ++y, ++innerN)
                    inner += std::abs(e.at(x, y, 0) - e.at(x - 1, y, 0));
            inner = innerN ? inner / innerN : 0;

            // Across the top and left edges, luma and chroma.
            double seam = 0, chromaSeam = 0;
            int seamN = 0;
            if (row > 0) {
                for (int x = x0; x < x0 + w; ++x, ++seamN) {
                    seam += std::abs(e.at(x, y0, 0) - e.at(x, y0 - 1, 0));
                    chromaSeam += std::abs(e.at(x, y0, 1) - e.at(x, y0 - 1, 1))
                        + std::abs(e.at(x, y0, 2) - e.at(x, y0 - 1, 2));
                }
            }
            if (col > 0) {
                for (int y = y0; y < y0 + h; ++y, ++seamN) {
                    seam += std::abs(e.at(x0, y, 0) - e.at(x0 - 1, y, 0));
                    chromaSeam += std::abs(e.at(x0, y, 1) - e.at(x0 - 1, y, 1))
                        + std::abs(e.at(x0, y, 2) - e.at(x0 - 1, y, 2));
                }
            }
            float s = 0;
            if (seamN) {
                seam /= seamN;
                chromaSeam /= seamN;
                const double ratio = (seam + 0.5 * chromaSeam) / (inner + 4.0);
                s = float(std::clamp((ratio - 2.0) / 6.0, 0.0, 1.0));
            }
            out.score[m] = s;
        }
    }

    if (map && map->isValid() && map->mcusX == info.mcusX && map->mcusY == info.mcusY) {
        const int bpm = info.blocksPerMcu;
        for (int m = 0; m < info.mcuCount(); ++m) {
            int from = m;
            if (unitSrc) {
                const qint32 u = unitSrc->value(qsizetype(m) * bpm, -1);
                if (u < 0)
                    continue; // written by a paste: the map says nothing about it
                from = u / bpm;
            }
            if (from < map->mcus.size() && map->mcus.at(from).anomalies)
                out.score[m] = 1.f;
        }
    }

    for (int m = 0; m < out.score.size(); ++m) {
        if (out.score[m] >= 0.5f) {
            out.firstDamaged = m;
            break;
        }
    }
    return out;
}

int nextDamaged(const DamageMap &map, int from, float threshold)
{
    for (int m = std::max(0, from + 1); m < map.score.size(); ++m) {
        if (map.score[m] >= threshold)
            return m;
    }
    return -1;
}

QVector<AlignCandidate> autoAlign(const jr::Info &info, const jr::Samples &ycbcr, int start,
                                  int maxShift)
{
    QVector<AlignCandidate> out;
    const int W = info.mcusX, total = info.mcuCount();
    if (!ycbcr.isValid() || start < W || start >= total)
        return out;
    const Edges e = edgesFor(info, ycbcr);

    // After a shift by k, the MCU at position p (p >= start) shows what was at
    // p - k. The seam that changes is the one between the last unshifted row
    // of content and the first shifted one: position p in [start, start + W)
    // under position p - W, which lies before `start` and so does not move.
    for (int k = -maxShift; k <= maxShift; ++k) {
        double sum[3] = {0, 0, 0}, sumSq[3] = {0, 0, 0};
        long n = 0;
        for (int p = start; p < start + W && p < total; ++p) {
            const int src = p - k;
            if (src < 0 || src >= total)
                continue;
            const int above = p - W;
            const int pc = p % W, ar = above / W;
            const int sr = src / W, sc = src % W;
            const int w = std::min(e.validWidth(pc), e.validWidth(sc));
            if (w <= 0 || e.validHeight(sr) <= 0)
                continue;
            const int yAbove = ar * e.mh + e.validHeight(ar) - 1;
            const int ySrc = sr * e.mh;
            for (int x = 0; x < w; ++x) {
                for (int c = 0; c < 3; ++c) {
                    const double d = e.at(sc * e.mw + x, ySrc, c) - e.at(pc * e.mw + x, yAbove, c);
                    sum[c] += d;
                    sumSq[c] += d * d;
                }
                ++n;
            }
        }
        if (n < 8)
            continue;
        AlignCandidate cand;
        cand.shift = k;
        double cost = 0;
        for (int c = 0; c < 3; ++c) {
            const double mean = sum[c] / n;
            cand.offset[c] = mean;
            const double var = std::max(0.0, sumSq[c] / n - mean * mean);
            cost += (c == 0 ? 1.0 : 0.5) * std::sqrt(var);
        }
        cand.cost = cost;
        out.append(cand);
    }
    std::stable_sort(out.begin(), out.end(), [](const AlignCandidate &a, const AlignCandidate &b) {
        if (a.cost != b.cost)
            return a.cost < b.cost;
        return std::abs(a.shift) < std::abs(b.shift);
    });
    return out;
}

DcEstimate autoDc(const jr::Coefs &coefs, const QByteArray &mask)
{
    DcEstimate out;
    const jr::Info info = coefs.info();
    const int W = info.mcusX, total = info.mcuCount();
    if (mask.size() != total || info.numComponents < 1)
        return out;
    const jr_coefs *c = coefs.raw();
    const QVector<quint16> q = coefs.quantTables();
    QVector<double> diffs[3];
    const auto in = [&mask](int m) { return mask.at(m) != 0; };
    double inside[8], outside[8];

    for (int m = 0; m < total; ++m) {
        if (!in(m))
            continue;
        const int row = m / W, col = m % W;
        for (int k = 0; k < info.numComponents && k < 3; ++k) {
            const int h = info.hSamp[k], v = info.vSamp[k];
            const quint16 *qk = q.constData() + 64 * k;
            // Real (not padding) blocks only: past the picture's edge the
            // encoder made the content up.
            const int compBlocksX = (c->ds_width[k] + 7) / 8, compBlocksY = (c->ds_height[k] + 7) / 8;
            if (row > 0 && !in(m - W)) {
                for (int bx = 0; bx < h; ++bx) {
                    const int bcol = col * h + bx;
                    if (bcol >= compBlocksX || row * v >= compBlocksY)
                        continue;
                    const int16_t *below = jr_coefs_block(const_cast<jr_coefs *>(c), k, row * v, bcol);
                    const int16_t *above = jr_coefs_block(const_cast<jr_coefs *>(c), k, row * v - 1, bcol);
                    dctedge::samples(below, qk, dctedge::Top, inside);
                    dctedge::samples(above, qk, dctedge::Bottom, outside);
                    for (int i = 0; i < 8; ++i)
                        diffs[k].append(outside[i] - inside[i]);
                }
            }
            if (col > 0 && !in(m - 1)) {
                for (int by = 0; by < v; ++by) {
                    const int brow = row * v + by;
                    if (brow >= compBlocksY || col * h >= compBlocksX)
                        continue;
                    const int16_t *right = jr_coefs_block(const_cast<jr_coefs *>(c), k, brow, col * h);
                    const int16_t *left = jr_coefs_block(const_cast<jr_coefs *>(c), k, brow, col * h - 1);
                    dctedge::samples(right, qk, dctedge::Left, inside);
                    dctedge::samples(left, qk, dctedge::Right, outside);
                    for (int i = 0; i < 8; ++i)
                        diffs[k].append(outside[i] - inside[i]);
                }
            }
        }
    }
    out.boundaryPixels = diffs[0].size();
    if (out.boundaryPixels < 8)
        return out;

    double spread = 0;
    for (int k = 0; k < 3 && k < info.numComponents; ++k) {
        const double shift = median(diffs[k]);
        out.pixelShift[k] = shift;
        out.cdelta[k] = int(std::lround(shift * 8.0 / std::max(1, info.dcQuant[k])));
        if (k == 0) {
            QVector<double> dev;
            dev.reserve(diffs[0].size());
            for (double d : diffs[0])
                dev.append(std::abs(d - shift));
            spread = median(dev);
        }
    }
    out.confidence = std::clamp(1.0 - spread / 24.0, 0.0, 1.0);
    out.valid = true;
    return out;
}

double rowContinuity(const jr::Info &info, const jr::Samples &ycbcr, int rows)
{
    if (!ycbcr.isValid())
        return 1e9;
    const Edges e = edgesFor(info, ycbcr);
    rows = std::min(rows, info.mcusY);
    double seam = 0, inner = 0;
    long seamN = 0, innerN = 0;
    for (int r = 1; r < rows; ++r) {
        const int y = r * e.mh;
        if (y >= e.height)
            break;
        for (int x = 0; x < e.width; ++x) {
            seam += std::abs(e.at(x, y, 0) - e.at(x, y - 1, 0));
            ++seamN;
            if (y + 1 < e.height) {
                inner += std::abs(e.at(x, y + 1, 0) - e.at(x, y, 0));
                ++innerN;
            }
        }
    }
    if (!seamN || !innerN)
        return 1e9;
    return (seam / seamN) / (inner / innerN + 1.0);
}

QString verdictName(Triage::Verdict v)
{
    const char *ctx = "Triage";
    switch (v) {
    case Triage::Verdict::Healthy: return QCoreApplication::translate(ctx, "healthy");
    case Triage::Verdict::Trailer: return QCoreApplication::translate(ctx, "healthy, data after image");
    case Triage::Verdict::Ransomware: return QCoreApplication::translate(ctx, "ransomware-encrypted header");
    case Triage::Verdict::HeaderDamaged: return QCoreApplication::translate(ctx, "header damaged");
    case Triage::Verdict::Truncated: return QCoreApplication::translate(ctx, "truncated");
    case Triage::Verdict::DecodeErrors: return QCoreApplication::translate(ctx, "decode errors");
    case Triage::Verdict::NotJpeg: return QCoreApplication::translate(ctx, "not a JPEG");
    }
    return QString();
}

Triage triage(const QByteArray &bytes)
{
    const char *ctx = "Triage";
    Triage t;
    const auto footer = jpegfile::findStopDjvuFooter(bytes);
    const jpegfile::Structure s = jpegfile::walk(bytes);
    const jpegfile::Trailer trailer = jpegfile::findTrailer(bytes, s.imageEnd);
    const QByteArray image = trailer.present() ? bytes.left(trailer.offset) : bytes;
    t.width = s.width;
    t.height = s.height;

    const auto info = jr::probe(image);
    if (footer) {
        t.verdict = Triage::Verdict::Ransomware;
        t.summary = QCoreApplication::translate(ctx, "STOP/Djvu footer, personal ID %1%2")
                        .arg(footer->personalId.isEmpty() ? QStringLiteral("?") : footer->personalId,
                             footer->offlineIdLikely
                                 ? QCoreApplication::translate(ctx, " (offline ID: try a decryptor first)")
                                 : QString());
        return t;
    }
    if (!info) {
        const bool anyMarkers = bytes.contains(QByteArray("\xFF\xDA", 2)) || bytes.contains(QByteArray("\xFF\xC4", 2));
        t.verdict = s.problem.isEmpty() || anyMarkers || bytes.startsWith(QByteArray("\xFF\xD8", 2))
            ? Triage::Verdict::HeaderDamaged
            : Triage::Verdict::NotJpeg;
        t.summary = s.problem.isEmpty() ? QCoreApplication::translate(ctx, "unreadable header")
                                        : QCoreApplication::translate(ctx, "this file %1").arg(s.problem);
        return t;
    }
    t.width = info->width;
    t.height = info->height;
    t.mcus = info->mcuCount();

    const jr::Salvage salvaged = jr::salvageScan(image);
    const bitstream::Map map = bitstream::map(salvaged.data);
    int truncated = 0;
    if (map.isValid()) {
        for (const bitstream::McuRecord &m : map.mcus) {
            if (m.anomalies == bitstream::Truncated)
                ++truncated;
            else if (m.anomalies)
                ++t.decodeErrors;
            if (m.anomalies && m.anomalies != bitstream::Truncated && t.firstError < 0)
                t.firstError = int(&m - map.mcus.constData());
        }
        t.readable = map.mcus.isEmpty() ? 0 : 1.0 - double(truncated) / map.mcus.size();
    } else {
        t.readable = salvaged.damaged() ? 0.5 : 1.0;
    }

    if (t.decodeErrors > 0) {
        t.verdict = Triage::Verdict::DecodeErrors;
        t.summary = QCoreApplication::translate(ctx, "%1 MCU(s) with decode errors, first at MCU %2")
                        .arg(t.decodeErrors)
                        .arg(t.firstError);
    } else if (truncated > 0 || salvaged.damaged()) {
        t.verdict = Triage::Verdict::Truncated;
        t.summary = QCoreApplication::translate(ctx, "%1% of the picture has data")
                        .arg(int(t.readable * 100));
    } else if (trailer.present() && trailer.kind != jpegfile::TrailerKind::Unknown) {
        t.verdict = Triage::Verdict::Trailer;
        t.summary = trailer.describe();
    } else {
        t.verdict = Triage::Verdict::Healthy;
        t.summary = map.isValid() ? QString() : map.unsupported;
    }
    return t;
}

} // namespace analysis
