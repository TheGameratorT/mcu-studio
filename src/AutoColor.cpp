// SPDX-License-Identifier: GPL-3.0-only
// Part of MCU Studio. See LICENSE and THIRD-PARTY-NOTICES.md.

#include "AutoColor.h"

#include <algorithm>
#include <array>
#include <cmath>

namespace autocolor {
namespace {

using Histogram = std::array<qint64, 256>;

Histogram histogram(const jr::Samples &rgb, int channel)
{
    Histogram h{};
    const quint8 *p = reinterpret_cast<const quint8 *>(rgb.data.constData());
    const qsizetype n = qsizetype(rgb.width) * rgb.height;
    for (qsizetype i = 0; i < n; ++i)
        ++h[p[i * 3 + channel]];
    return h;
}

// The k-th smallest sample (0-based) read off a cumulative histogram.
int orderStatistic(const Histogram &cumulative, qint64 k)
{
    k = std::clamp<qint64>(k, 0, cumulative[255] - 1);
    // Values are 0..255, so a linear walk is 256 steps at worst -- cheaper
    // than a binary search once cache behavior is accounted for.
    for (int v = 0; v < 256; ++v) {
        if (cumulative[v] > k)
            return v;
    }
    return 255;
}

// Matches numpy's default percentile: linear interpolation between the two
// order statistics straddling q/100 * (N - 1).
double percentile(const Histogram &histogram, double q)
{
    Histogram cumulative{};
    qint64 running = 0;
    for (int v = 0; v < 256; ++v) {
        running += histogram[v];
        cumulative[v] = running;
    }
    const qint64 n = cumulative[255];
    if (n <= 0)
        return 0.0;

    const double pos = std::clamp(q, 0.0, 100.0) / 100.0 * double(n - 1);
    const qint64 lo = qint64(std::floor(pos));
    const double frac = pos - double(lo);
    const int a = orderStatistic(cumulative, lo);
    if (frac <= 0.0)
        return a;
    const int b = orderStatistic(cumulative, lo + 1);
    return a + frac * (b - a);
}

// PhotoDemon's midtone curve: push each level away from mid-gray in
// proportion to how far it already is, scaled down again by how close it is
// to either end of the range, so 0 and 255 stay put.
std::array<quint8, 256> midtoneTable(double strength)
{
    std::array<quint8, 256> lut{};
    for (int x = 0; x < 256; ++x) {
        const double xf = double(x);
        const double diff = xf - 127.0;
        const double taper = (x < 127 ? xf : 255.0 - xf) / 127.0;
        const double pushed = xf + taper * (diff / 2.0) * strength;
        lut[x] = quint8(std::lround(std::clamp(pushed, 0.0, 255.0)));
    }
    return lut;
}

} // namespace

jr::Samples autoLevels(const jr::Samples &rgb, double clipPercent)
{
    if (!rgb.isValid())
        return rgb;

    jr::Samples out = rgb;
    quint8 *p = reinterpret_cast<quint8 *>(out.data.data());
    const qsizetype n = qsizetype(out.width) * out.height;

    for (int c = 0; c < 3; ++c) {
        const Histogram h = histogram(rgb, c);
        const double lo = percentile(h, clipPercent);
        const double hi = percentile(h, 100.0 - clipPercent);
        if (hi <= lo)
            continue; // flat channel: stretching it would only amplify noise

        // Precompute the whole mapping -- 256 entries beats a multiply and a
        // clamp per sample on any image worth opening.
        std::array<quint8, 256> lut{};
        const double scale = 255.0 / (hi - lo);
        for (int v = 0; v < 256; ++v)
            lut[v] = quint8(std::lround(std::clamp((double(v) - lo) * scale, 0.0, 255.0)));

        for (qsizetype i = 0; i < n; ++i)
            p[i * 3 + c] = lut[p[i * 3 + c]];
    }
    return out;
}

jr::Samples midtoneContrast(const jr::Samples &rgb, double strength)
{
    if (!rgb.isValid())
        return rgb;

    jr::Samples out = rgb;
    const std::array<quint8, 256> lut = midtoneTable(strength);
    quint8 *p = reinterpret_cast<quint8 *>(out.data.data());
    const qsizetype n = qsizetype(out.width) * out.height * 3;
    for (qsizetype i = 0; i < n; ++i)
        p[i] = lut[p[i]];
    return out;
}

jr::Samples autoCorrect(const jr::Samples &rgb)
{
    return midtoneContrast(autoLevels(rgb));
}

} // namespace autocolor