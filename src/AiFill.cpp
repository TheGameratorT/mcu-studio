// SPDX-License-Identifier: GPL-3.0-only
// Part of MCU Studio. See LICENSE and THIRD-PARTY-NOTICES.md.

#include "AiFill.h"

#include <QCoreApplication>
#include <QDir>
#include <QSettings>
#include <QStandardPaths>
#include <QTransform>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>

namespace aifill {

const char *const kLamaModelSha256 =
    "1faef5301d78db7dda502fe59966957ec4b79dd64e16f03ed96913c7a4eb68d6";
const char *const kDefaultCloudBaseUrl = "https://api.openai.com/v1";
const char *const kDefaultCloudModel = "gpt-image-1";
const char *const kLamaModelUrl =
    "https://huggingface.co/Carve/LaMa-ONNX/resolve/main/lama_fp32.onnx";

namespace {

QString tr(const char *text)
{
    return QCoreApplication::translate("aifill", text);
}

// How far around the hole the fit of a redrawn window looks. Close in, because
// a model that redraws a window drifts differently in different parts of it,
// and the part that has to match is the part beside the hole.
constexpr int kFitBand = 64;
// How far a redrawn window is searched for, each way, in pixels.
constexpr int kMaxDrift = 12;

// How far past the selected MCUs a window's mask reaches. A damaged MCU does
// not keep to itself on screen: decoding spreads its chroma a pixel or two
// into its neighbors. A model shown that fringe as picture continues its
// color straight through the gap. No further than that, though: what the
// model draws out there is thrown away, and the further its own content starts
// from the real picture, the less exactly it meets it at the MCU's edge.
int fringeFor(const jr::Info &info)
{
    return std::max({2, info.maxHSamp, info.maxVSamp});
}

// `mask` with every set pixel grown by `by` pixels in each direction.
QImage dilated(const QImage &mask, int by)
{
    const int w = mask.width(), h = mask.height();
    QImage across(w, h, QImage::Format_Grayscale8), out(w, h, QImage::Format_Grayscale8);
    for (int y = 0; y < h; ++y) {
        const uchar *in = mask.constScanLine(y);
        uchar *to = across.scanLine(y);
        int reach = -1; // the last column a set pixel still covers
        for (int x = -by; x < w; ++x) {
            if (x + by < w && in[x + by])
                reach = x + 2 * by;
            if (x >= 0)
                to[x] = x <= reach ? 255 : 0;
        }
    }
    for (int x = 0; x < w; ++x) {
        int reach = -1;
        for (int y = -by; y < h; ++y) {
            if (y + by < h && across.constScanLine(y + by)[x])
                reach = y + 2 * by;
            if (y >= 0)
                out.scanLine(y)[x] = y <= reach ? 255 : 0;
        }
    }
    return out;
}

// The MCU's rectangle in picture pixels, cut off at the picture's edge.
QRect mcuPixels(const jr::Info &info, int index)
{
    const int row = index / info.mcusX, col = index % info.mcusX;
    return QRect(col * info.mcuWidth, row * info.mcuHeight, info.mcuWidth, info.mcuHeight)
        .intersected(QRect(0, 0, info.width, info.height));
}

bool maskMatches(const QByteArray &mcuMask, const jr::Info &info)
{
    return info.isValid() && mcuMask.size() == qsizetype(info.mcusY) * info.mcusX;
}

// The part of `window` far enough from its edges to be worth filling from it.
// An edge the window shares with the picture gives nothing up: there is no
// more context to be had on that side whichever window is used.
QRect innerOf(const QRect &window, const QSize &picture, int marginX, int marginY)
{
    QRect inner = window.adjusted(marginX, marginY, -marginX, -marginY);
    if (window.left() <= 0)
        inner.setLeft(0);
    if (window.top() <= 0)
        inner.setTop(0);
    if (window.right() >= picture.width() - 1)
        inner.setRight(picture.width() - 1);
    if (window.bottom() >= picture.height() - 1)
        inner.setBottom(picture.height() - 1);
    return inner;
}

struct Luma {
    int width = 0, height = 0;
    std::vector<int> value;
    int at(int x, int y) const { return value[size_t(y) * width + x]; }
};

Luma lumaOf(const QImage &rgb)
{
    Luma out;
    out.width = rgb.width();
    out.height = rgb.height();
    out.value.resize(size_t(out.width) * out.height);
    for (int y = 0; y < out.height; ++y) {
        const uchar *line = rgb.constScanLine(y);
        for (int x = 0; x < out.width; ++x) {
            const uchar *p = line + qsizetype(x) * 3;
            out.value[size_t(y) * out.width + x] = (p[0] * 77 + p[1] * 150 + p[2] * 29) >> 8;
        }
    }
    return out;
}

Luma halved(const Luma &in)
{
    Luma out;
    out.width = in.width / 2;
    out.height = in.height / 2;
    out.value.resize(size_t(out.width) * out.height);
    for (int y = 0; y < out.height; ++y) {
        for (int x = 0; x < out.width; ++x) {
            out.value[size_t(y) * out.width + x] = (in.at(2 * x, 2 * y) + in.at(2 * x + 1, 2 * y)
                                                    + in.at(2 * x, 2 * y + 1)
                                                    + in.at(2 * x + 1, 2 * y + 1))
                / 4;
        }
    }
    return out;
}

// Spread of the difference between the two pictures over `where`, with the
// answer read (dx, dy) away. The spread rather than the size of the
// difference, so a tint or a brightness change, which is corrected afterward,
// does not pull the search toward the wrong place.
double driftCost(const Luma &original, const Luma &answer, const std::vector<QPoint> &where,
                 int dx, int dy)
{
    double sum = 0, sumSq = 0;
    qint64 n = 0;
    for (const QPoint &p : where) {
        const int ax = p.x() + dx, ay = p.y() + dy;
        if (ax < 0 || ay < 0 || ax >= answer.width || ay >= answer.height)
            continue;
        const double d = original.at(p.x(), p.y()) - answer.at(ax, ay);
        sum += d;
        sumSq += d * d;
        ++n;
    }
    if (n < 16)
        return std::numeric_limits<double>::max();
    const double mean = sum / double(n);
    return sumSq / double(n) - mean * mean;
}

} // namespace

// ---------------------------------------------------------------------------
// Planning
// ---------------------------------------------------------------------------

QVector<Tile> planTiles(const QByteArray &mcuMask, const jr::Info &info, int tileSize)
{
    QVector<Tile> tiles;
    if (!maskMatches(mcuMask, info) || tileSize <= 0)
        return tiles;

    const QSize picture(info.width, info.height);
    const int tileW = std::min(tileSize, info.width);
    const int tileH = std::min(tileSize, info.height);
    // A quarter of the window on every side is context. Less when the picture
    // is too small for a window to hold an MCU and that much around it.
    const int marginX = std::clamp((tileW - info.mcuWidth) / 2, 0, tileSize / 4);
    const int marginY = std::clamp((tileH - info.mcuHeight) / 2, 0, tileSize / 4);

    const auto place = [&](int left, int top) {
        return QRect(std::clamp(left, 0, info.width - tileW),
                     std::clamp(top, 0, info.height - tileH), tileW, tileH);
    };

    QByteArray todo = mcuMask;
    const auto covered = [&](const QRect &window) {
        const QRect inner = innerOf(window, picture, marginX, marginY);
        QVector<int> mcus;
        const int firstRow = std::max(0, inner.top() / info.mcuHeight);
        const int lastRow = std::min(info.mcusY - 1, inner.bottom() / info.mcuHeight);
        const int firstCol = std::max(0, inner.left() / info.mcuWidth);
        const int lastCol = std::min(info.mcusX - 1, inner.right() / info.mcuWidth);
        for (int r = firstRow; r <= lastRow; ++r) {
            for (int c = firstCol; c <= lastCol; ++c) {
                const int index = r * info.mcusX + c;
                if (todo.at(index) && inner.contains(mcuPixels(info, index)))
                    mcus.append(index);
            }
        }
        return mcus;
    };

    for (qsizetype first = todo.indexOf('\1'); first >= 0; first = todo.indexOf('\1', first)) {
        const QRect start = mcuPixels(info, int(first));
        // First with this MCU in the window's top left, which is where the
        // first MCU in scan order of anything usually is...
        QRect window = place(start.left() - marginX, start.top() - marginY);
        QVector<int> mcus = covered(window);

        // ...then moved to put what it covers in the middle, so a thin strip
        // has as much picture above it as below.
        QRect bounds;
        for (int index : mcus)
            bounds = bounds.united(mcuPixels(info, index));
        if (!bounds.isEmpty()) {
            const QRect centered = place(bounds.center().x() - tileW / 2,
                                         bounds.center().y() - tileH / 2);
            const QVector<int> again = covered(centered);
            if (again.contains(int(first)) && again.size() >= mcus.size()) {
                window = centered;
                mcus = again;
            }
        }

        if (!mcus.contains(int(first)))
            mcus.append(int(first)); // an MCU larger than the window; take it regardless

        for (int index : mcus)
            todo[index] = '\0';
        tiles.append(Tile{window, mcus, window.size()});
    }
    return tiles;
}

namespace {

struct Region {
    QRect bounds;       // in MCUs
    QVector<int> mcus;  // scan-order indices
};

QVector<Region> labelRegions(const QByteArray &mcuMask, const jr::Info &info)
{
    QVector<Region> found;
    if (!maskMatches(mcuMask, info))
        return found;

    QByteArray todo = mcuMask;
    QVector<int> stack;
    for (qsizetype start = 0; start < todo.size(); ++start) {
        if (!todo.at(start))
            continue;
        Region region;
        todo[start] = '\0';
        stack.append(int(start));
        while (!stack.isEmpty()) {
            const int index = stack.takeLast();
            const int row = index / info.mcusX, col = index % info.mcusX;
            region.bounds = region.bounds.united(QRect(col, row, 1, 1));
            region.mcus.append(index);
            for (int dr = -1; dr <= 1; ++dr) {
                for (int dc = -1; dc <= 1; ++dc) {
                    const int r = row + dr, c = col + dc;
                    if (r < 0 || c < 0 || r >= info.mcusY || c >= info.mcusX)
                        continue;
                    const int next = r * info.mcusX + c;
                    if (todo.at(next)) {
                        todo[next] = '\0';
                        stack.append(next);
                    }
                }
            }
        }
        found.append(region);
    }
    return found;
}

} // namespace

QVector<QRect> regions(const QByteArray &mcuMask, const jr::Info &info)
{
    QVector<QRect> found;
    for (const Region &region : labelRegions(mcuMask, info))
        found.append(region.bounds);
    return found;
}

QVector<Tile> planJob(const QByteArray &mcuMask, const jr::Info &info, int tileSize, bool magnify)
{
    if (!magnify)
        return planTiles(mcuMask, info, tileSize);

    QVector<Tile> tiles;
    for (const Region &region : labelRegions(mcuMask, info)) {
        // The region should span a fair part of what the model sees: a
        // sixteenth of the window is the least it is asked to work with.
        const int thickness = std::min(region.bounds.width() * info.mcuWidth,
                                       region.bounds.height() * info.mcuHeight);
        int side = std::clamp(thickness * 16, tileSize * 3 / 8, tileSize);
        side = std::max(8, side / 8 * 8);

        QByteArray only(mcuMask.size(), '\0');
        for (int index : region.mcus)
            only[index] = '\1';
        for (Tile tile : planTiles(only, info, side)) {
            if (side < tileSize) {
                // Enlarged by the same factor both ways, to whole latent cells.
                const double zoom = double(tileSize) / side;
                tile.modelSize = QSize(std::max(8, int(std::lround(tile.window.width() * zoom)) / 8 * 8),
                                       std::max(8, int(std::lround(tile.window.height() * zoom)) / 8 * 8));
            }
            tiles.append(tile);
        }
    }
    return tiles;
}

// ---------------------------------------------------------------------------
// Fitting a redrawn window back over the original
// ---------------------------------------------------------------------------

QImage registerAnswer(const QImage &originalIn, const QImage &answerIn, const QImage &maskIn,
                      Registration *fit)
{
    Registration found;
    const QImage original = originalIn.convertToFormat(QImage::Format_RGB888);
    QImage answer = answerIn.convertToFormat(QImage::Format_RGB888);
    const QImage mask = maskIn.convertToFormat(QImage::Format_Grayscale8);
    if (fit)
        *fit = found;
    if (original.isNull() || answer.size() != original.size() || mask.size() != original.size())
        return answer;

    const int w = original.width(), h = original.height();

    // The pixels the fit is judged on: real picture, near the hole.
    QRect hole;
    for (int y = 0; y < h; ++y) {
        const uchar *line = mask.constScanLine(y);
        for (int x = 0; x < w; ++x) {
            if (line[x])
                hole = hole.united(QRect(x, y, 1, 1));
        }
    }
    if (hole.isEmpty())
        return answer;
    const QRect band = hole.adjusted(-kFitBand, -kFitBand, kFitBand, kFitBand)
                           .intersected(QRect(0, 0, w, h));

    std::vector<QPoint> full, half;
    for (int y = band.top(); y <= band.bottom(); ++y) {
        const uchar *line = mask.constScanLine(y);
        for (int x = band.left(); x <= band.right(); ++x) {
            if (!line[x])
                full.emplace_back(x, y);
        }
    }
    for (int y = band.top() / 2; y <= band.bottom() / 2 && y < h / 2; ++y) {
        for (int x = band.left() / 2; x <= band.right() / 2 && x < w / 2; ++x) {
            if (!mask.constScanLine(2 * y)[2 * x] && !mask.constScanLine(2 * y)[2 * x + 1]
                && !mask.constScanLine(2 * y + 1)[2 * x] && !mask.constScanLine(2 * y + 1)[2 * x + 1])
                half.emplace_back(x, y);
        }
    }
    if (full.size() < 64)
        return answer;

    // Where it drifted to: coarsely at half size, then to the pixel.
    const Luma lumaOriginal = lumaOf(original), lumaAnswer = lumaOf(answer);
    int bestX = 0, bestY = 0;
    if (half.size() >= 64) {
        const Luma halfOriginal = halved(lumaOriginal), halfAnswer = halved(lumaAnswer);
        double best = driftCost(halfOriginal, halfAnswer, half, 0, 0);
        const int reach = kMaxDrift / 2;
        for (int dy = -reach; dy <= reach; ++dy) {
            for (int dx = -reach; dx <= reach; ++dx) {
                const double cost = driftCost(halfOriginal, halfAnswer, half, dx, dy);
                // A tie goes to the smaller move.
                if (cost < best * 0.98) {
                    best = cost;
                    bestX = dx;
                    bestY = dy;
                }
            }
        }
        bestX *= 2;
        bestY *= 2;
    }
    {
        const int centerX = bestX, centerY = bestY;
        double best = driftCost(lumaOriginal, lumaAnswer, full, centerX, centerY);
        for (int dy = centerY - 1; dy <= centerY + 1; ++dy) {
            for (int dx = centerX - 1; dx <= centerX + 1; ++dx) {
                const double cost = driftCost(lumaOriginal, lumaAnswer, full, dx, dy);
                if (cost < best) {
                    best = cost;
                    bestX = dx;
                    bestY = dy;
                }
            }
        }
    }
    found.dx = bestX;
    found.dy = bestY;

    // What it did to the colors: a straight line per channel.
    const auto shifted = [&](int x, int y) {
        return answer.constScanLine(std::clamp(y + found.dy, 0, h - 1))
            + qsizetype(std::clamp(x + found.dx, 0, w - 1)) * 3;
    };
    for (int c = 0; c < 3; ++c) {
        double sumA = 0, sumO = 0, sumAA = 0, sumAO = 0;
        for (const QPoint &p : full) {
            const double a = shifted(p.x(), p.y())[c];
            const double o = original.constScanLine(p.y())[qsizetype(p.x()) * 3 + c];
            sumA += a;
            sumO += o;
            sumAA += a * a;
            sumAO += a * o;
        }
        const double n = double(full.size());
        const double var = sumAA / n - (sumA / n) * (sumA / n);
        const double cov = sumAO / n - (sumA / n) * (sumO / n);
        // Flat surroundings say nothing about contrast; leave it alone then.
        double gain = var > 4.0 ? cov / var : 1.0;
        gain = std::clamp(gain, 0.5, 2.0);
        found.gain[c] = gain;
        found.offset[c] = sumO / n - gain * (sumA / n);
    }

    QImage out(w, h, QImage::Format_RGB888);
    for (int y = 0; y < h; ++y) {
        uchar *line = out.scanLine(y);
        for (int x = 0; x < w; ++x) {
            const uchar *a = shifted(x, y);
            for (int c = 0; c < 3; ++c) {
                line[qsizetype(x) * 3 + c] = uchar(
                    std::clamp(int(std::lround(found.gain[c] * a[c] + found.offset[c])), 0, 255));
            }
        }
    }

    double left = 0;
    for (const QPoint &p : full) {
        const uchar *o = original.constScanLine(p.y()) + qsizetype(p.x()) * 3;
        const uchar *a = out.constScanLine(p.y()) + qsizetype(p.x()) * 3;
        left += std::abs(o[0] - a[0]) + std::abs(o[1] - a[1]) + std::abs(o[2] - a[2]);
    }
    found.residual = left / (3.0 * double(full.size()));

    if (fit)
        *fit = found;
    return out;
}

QImage blendSeams(const QImage &originalIn, const QImage &answerIn, const QImage &maskIn)
{
    const QImage original = originalIn.convertToFormat(QImage::Format_RGB888);
    QImage answer = answerIn.convertToFormat(QImage::Format_RGB888);
    const QImage mask = maskIn.convertToFormat(QImage::Format_Grayscale8);
    if (original.isNull() || answer.size() != original.size() || mask.size() != original.size())
        return answer;

    // One level of a pyramid: the difference per channel, and how much real
    // picture stands behind it.
    struct Level {
        int w = 0, h = 0;
        std::vector<float> value; // 3 per pixel
        std::vector<float> weight;
    };
    std::vector<Level> levels(1);
    Level &base = levels[0];
    base.w = original.width();
    base.h = original.height();
    base.value.assign(size_t(base.w) * base.h * 3, 0.0f);
    base.weight.assign(size_t(base.w) * base.h, 0.0f);
    bool anyHole = false, anyKnown = false;
    for (int y = 0; y < base.h; ++y) {
        const uchar *o = original.constScanLine(y), *a = answer.constScanLine(y);
        const uchar *m = mask.constScanLine(y);
        for (int x = 0; x < base.w; ++x) {
            if (m[x]) {
                anyHole = true;
                continue;
            }
            anyKnown = true;
            const size_t at = size_t(y) * base.w + x;
            base.weight[at] = 1.0f;
            for (int c = 0; c < 3; ++c)
                base.value[at * 3 + c] = float(o[x * 3 + c]) - float(a[x * 3 + c]);
        }
    }
    if (!anyHole || !anyKnown)
        return answer;

    // Pull: each coarser level averages what is known beneath it, so far from
    // the edge the hole is described by more and more of its surroundings.
    while (levels.back().w > 1 || levels.back().h > 1) {
        const Level &fine = levels.back();
        Level coarse;
        coarse.w = (fine.w + 1) / 2;
        coarse.h = (fine.h + 1) / 2;
        coarse.value.assign(size_t(coarse.w) * coarse.h * 3, 0.0f);
        coarse.weight.assign(size_t(coarse.w) * coarse.h, 0.0f);
        for (int y = 0; y < fine.h; ++y) {
            for (int x = 0; x < fine.w; ++x) {
                const size_t from = size_t(y) * fine.w + x;
                const size_t to = size_t(y / 2) * coarse.w + x / 2;
                const float wgt = fine.weight[from];
                coarse.weight[to] += wgt;
                for (int c = 0; c < 3; ++c)
                    coarse.value[to * 3 + c] += fine.value[from * 3 + c] * wgt;
            }
        }
        for (size_t i = 0; i < coarse.weight.size(); ++i) {
            if (coarse.weight[i] > 0) {
                for (int c = 0; c < 3; ++c)
                    coarse.value[i * 3 + c] /= coarse.weight[i];
                coarse.weight[i] = std::min(1.0f, coarse.weight[i]);
            }
        }
        levels.push_back(std::move(coarse));
    }

    // Push: what a level does not know it takes, smoothly, from the one above.
    // The finest level is skipped as a source for itself (its hole takes
    // everything from above), which keeps single-pixel noise at the edge from
    // being drawn into the hole as streaks.
    for (int k = int(levels.size()) - 2; k >= 0; --k) {
        Level &fine = levels[size_t(k)];
        const Level &coarse = levels[size_t(k) + 1];
        for (int y = 0; y < fine.h; ++y) {
            const float fy = std::clamp((y + 0.5f) / 2.0f - 0.5f, 0.0f, float(coarse.h - 1));
            const int y0 = int(fy), y1 = std::min(coarse.h - 1, y0 + 1);
            const float ty = fy - y0;
            for (int x = 0; x < fine.w; ++x) {
                const size_t at = size_t(y) * fine.w + x;
                const float own = fine.weight[at];
                if (own >= 1.0f)
                    continue;
                const float fx = std::clamp((x + 0.5f) / 2.0f - 0.5f, 0.0f, float(coarse.w - 1));
                const int x0 = int(fx), x1 = std::min(coarse.w - 1, x0 + 1);
                const float tx = fx - x0;
                for (int c = 0; c < 3; ++c) {
                    const auto v = [&](int cx, int cy) {
                        return coarse.value[(size_t(cy) * coarse.w + cx) * 3 + c];
                    };
                    const float above = (v(x0, y0) * (1 - tx) + v(x1, y0) * tx) * (1 - ty)
                        + (v(x0, y1) * (1 - tx) + v(x1, y1) * tx) * ty;
                    fine.value[at * 3 + c] = fine.value[at * 3 + c] * own + above * (1 - own);
                }
                fine.weight[at] = 1.0f;
            }
        }
    }

    for (int y = 0; y < base.h; ++y) {
        const uchar *m = mask.constScanLine(y);
        uchar *a = answer.scanLine(y);
        for (int x = 0; x < base.w; ++x) {
            if (!m[x])
                continue;
            const size_t at = size_t(y) * base.w + x;
            for (int c = 0; c < 3; ++c) {
                a[x * 3 + c] = uchar(std::clamp(
                    int(std::lround(a[x * 3 + c] + base.value[at * 3 + c])), 0, 255));
            }
        }
    }
    return answer;
}

// ---------------------------------------------------------------------------
// Running
// ---------------------------------------------------------------------------

std::optional<Result> run(Provider &provider, const jr::Samples &rgb, const QByteArray &mcuMask,
                          const jr::Info &info, const QString &prompt, int quarterTurns,
                          const Progress &progress, const std::function<bool()> &cancelled,
                          QString *error)
{
    const int turns = ((quarterTurns % 4) + 4) % 4;
    const auto turned = [](const QImage &image, int by, QImage::Format format) {
        if (by == 0)
            return image;
        return image.transformed(QTransform().rotate(90.0 * by)).convertToFormat(format);
    };

    const auto fail = [error](const QString &message) {
        if (error)
            *error = message;
        return std::nullopt;
    };

    if (!maskMatches(mcuMask, info) || !rgb.isValid() || rgb.width < info.width
        || rgb.height < info.height)
        return fail(tr("The selection does not match the picture."));

    const QVector<Tile> tiles =
        planJob(mcuMask, info, provider.tileSize(), provider.regeneratesContext());
    if (tiles.isEmpty())
        return fail(tr("No MCUs are selected to fill."));

    // The picture as it stands, filled in as the windows come back, so each
    // window sees what the ones before it made of their part.
    QImage working(info.width, info.height, QImage::Format_RGB888);
    for (int y = 0; y < info.height; ++y)
        std::memcpy(working.scanLine(y), rgb.pixel(0, y), size_t(info.width) * 3);

    // What is still waiting for content. A window that reaches into another
    // one's MCUs has to say they are not picture either.
    QImage waiting(info.width, info.height, QImage::Format_Grayscale8);
    waiting.fill(0);
    for (qsizetype i = 0; i < mcuMask.size(); ++i) {
        if (!mcuMask.at(i))
            continue;
        const QRect r = mcuPixels(info, int(i));
        for (int y = r.top(); y <= r.bottom(); ++y)
            std::memset(waiting.scanLine(y) + r.left(), 255, size_t(r.width()));
    }

    Result result;
    result.tiles = int(tiles.size());
    if (progress)
        progress(0, result.tiles);

    for (int t = 0; t < tiles.size(); ++t) {
        if (cancelled && cancelled())
            return fail(tr("Cancelled."));
        const Tile &tile = tiles.at(t);

        Request request;
        request.image = working.copy(tile.window);
        request.mask = dilated(waiting.copy(tile.window), fringeFor(info));
        request.prompt = prompt;
        if (tile.modelSize != tile.window.size()) {
            request.image = request.image
                                .scaled(tile.modelSize, Qt::IgnoreAspectRatio, Qt::SmoothTransformation)
                                .convertToFormat(QImage::Format_RGB888);
            request.mask = request.mask
                               .scaled(tile.modelSize, Qt::IgnoreAspectRatio, Qt::FastTransformation)
                               .convertToFormat(QImage::Format_Grayscale8);
        }
        request.image = turned(request.image, turns, QImage::Format_RGB888);
        request.mask = turned(request.mask, turns, QImage::Format_Grayscale8);

        QString why;
        const std::optional<QImage> answered = provider.inpaint(request, cancelled, &why);
        if (cancelled && cancelled())
            return fail(tr("Cancelled."));
        if (!answered || answered->isNull())
            return fail(why.isEmpty() ? tr("The model returned nothing.") : why);

        QImage answer = answered->convertToFormat(QImage::Format_RGB888);
        if (answer.size() != request.image.size()) {
            answer = answer.scaled(request.image.size(), Qt::IgnoreAspectRatio,
                                   Qt::SmoothTransformation)
                         .convertToFormat(QImage::Format_RGB888);
        }
        if (provider.regeneratesContext()) {
            Registration fit;
            answer = registerAnswer(request.image, answer, request.mask, &fit);
            result.worstResidual = std::max(result.worstResidual, fit.residual);
            answer = blendSeams(request.image, answer, request.mask);
        }
        answer = turned(answer, (4 - turns) % 4, QImage::Format_RGB888);
        if (answer.size() != tile.window.size()) {
            answer = answer.scaled(tile.window.size(), Qt::IgnoreAspectRatio,
                                   Qt::SmoothTransformation)
                         .convertToFormat(QImage::Format_RGB888);
        }

        // Only this window's MCUs are taken from it, and nothing else of it.
        for (int index : tile.mcus) {
            const QRect r = mcuPixels(info, index);
            const QPoint local = r.topLeft() - tile.window.topLeft();
            for (int y = 0; y < r.height(); ++y) {
                const int ay = local.y() + y;
                if (ay < 0 || ay >= answer.height())
                    continue;
                const int x0 = std::max(0, local.x());
                const int x1 = std::min(answer.width(), local.x() + r.width());
                if (x1 <= x0)
                    continue;
                const int skip = x0 - local.x();
                std::memcpy(working.scanLine(r.top() + y) + qsizetype(r.left() + skip) * 3,
                            answer.constScanLine(ay) + qsizetype(x0) * 3, size_t(x1 - x0) * 3);
                std::memset(waiting.scanLine(r.top() + y) + r.left() + skip, 0, size_t(x1 - x0));
            }
        }

        if (progress)
            progress(t + 1, result.tiles);
    }

    result.image = working;
    return result;
}

// ---------------------------------------------------------------------------
// Settings
// ---------------------------------------------------------------------------

Settings Settings::load()
{
    QSettings store;
    Settings s;
    s.provider = store.value(QStringLiteral("aifill/provider")).toString();
    s.lamaModelPath = store.value(QStringLiteral("aifill/lamaModel")).toString();
    s.lamaUseGpu = store.value(QStringLiteral("aifill/lamaUseGpu"), false).toBool();
    s.cloudBaseUrl = store.value(QStringLiteral("aifill/cloudBaseUrl")).toString();
    s.cloudModel = store.value(QStringLiteral("aifill/cloudModel")).toString();
    s.cloudApiKey = store.value(QStringLiteral("aifill/cloudApiKey")).toString();
    s.confirmedHosts = store.value(QStringLiteral("aifill/confirmedHosts")).toStringList();
    return s;
}

void Settings::save() const
{
    QSettings store;
    store.setValue(QStringLiteral("aifill/provider"), provider);
    store.setValue(QStringLiteral("aifill/lamaModel"), lamaModelPath);
    store.setValue(QStringLiteral("aifill/lamaUseGpu"), lamaUseGpu);
    store.setValue(QStringLiteral("aifill/cloudBaseUrl"), cloudBaseUrl);
    store.setValue(QStringLiteral("aifill/cloudModel"), cloudModel);
    store.setValue(QStringLiteral("aifill/cloudApiKey"), cloudApiKey);
    store.setValue(QStringLiteral("aifill/confirmedHosts"), confirmedHosts);
}

QString Settings::effectiveBaseUrl() const
{
    QString url = cloudBaseUrl.trimmed();
    if (url.isEmpty())
        url = QString::fromLatin1(kDefaultCloudBaseUrl);
    while (url.endsWith(QLatin1Char('/')))
        url.chop(1);
    return url;
}

QString Settings::effectiveModel() const
{
    const QString model = cloudModel.trimmed();
    return model.isEmpty() ? QString::fromLatin1(kDefaultCloudModel) : model;
}

QString Settings::effectiveApiKey() const
{
    const QString key = cloudApiKey.trimmed();
    return key.isEmpty() ? qEnvironmentVariable("OPENAI_API_KEY").trimmed() : key;
}

QString defaultLamaModelPath()
{
    return QDir(QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation))
        .filePath(QStringLiteral("models/lama_fp32.onnx"));
}

QString Settings::effectiveLamaModelPath() const
{
    const QString path = lamaModelPath.trimmed();
    return path.isEmpty() ? defaultLamaModelPath() : path;
}

std::vector<std::unique_ptr<Provider>> providers(const Settings &settings)
{
    std::vector<std::unique_ptr<Provider>> list;
    list.push_back(makeLamaProvider(settings));
    list.push_back(makeCloudProvider(settings));
    return list;
}

} // namespace aifill
