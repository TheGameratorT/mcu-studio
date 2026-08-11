// SPDX-License-Identifier: GPL-3.0-only
// Part of MCU Studio. See LICENSE and THIRD-PARTY-NOTICES.md.

#include "ColorMath.h"

#include <algorithm>
#include <cmath>

namespace colormath {

double pixelShiftForCdelta(int dcQuant, int cdelta)
{
    const int q = dcQuant > 0 ? dcQuant : 1;
    return double(cdelta) * q / 8.0;
}

int cdeltaForPixelShift(int dcQuant, double pixelShift)
{
    const int q = dcQuant > 0 ? dcQuant : 1;
    const double raw = std::round(pixelShift * 8.0 / q);
    return int(std::clamp(raw, double(-kCdeltaLimit), double(kCdeltaLimit)));
}

BlockStats measure(const jr::Samples &samples, QRect pixelRect)
{
    BlockStats stats;
    if (!samples.isValid())
        return stats;

    const QRect r = pixelRect.intersected(QRect(0, 0, samples.width, samples.height));
    if (r.isEmpty())
        return stats;

    double sum[3] = {0, 0, 0};
    qint64 clipped[3] = {0, 0, 0};

    for (int y = r.top(); y <= r.bottom(); ++y) {
        const quint8 *row = samples.pixel(r.left(), y);
        for (int x = 0; x < r.width(); ++x) {
            for (int c = 0; c < 3; ++c) {
                const quint8 v = row[x * 3 + c];
                sum[c] += v;
                if (v == 0 || v == 255)
                    ++clipped[c];
            }
        }
    }

    stats.sampleCount = qint64(r.width()) * r.height();
    for (int c = 0; c < 3; ++c) {
        stats.mean[c] = sum[c] / double(stats.sampleCount);
        stats.clipped[c] = double(clipped[c]) / double(stats.sampleCount);
    }
    return stats;
}

QRect mcuRect(const jr::Info &info, int row, int col)
{
    if (!info.isValid())
        return QRect();
    const QRect full(col * info.mcuWidth, row * info.mcuHeight, info.mcuWidth, info.mcuHeight);
    return full.intersected(QRect(0, 0, info.width, info.height));
}

const char *const channelNames[3] = {"Y", "Cb", "Cr"};

QString formatTriple(const double v[3], int decimals)
{
    return QStringLiteral("Y %1   Cb %2   Cr %3")
        .arg(v[0], 0, 'f', decimals)
        .arg(v[1], 0, 'f', decimals)
        .arg(v[2], 0, 'f', decimals);
}

QColor rgbForYCbCr(const double ycbcr[3])
{
    const double y = ycbcr[0];
    const double cb = ycbcr[1] - 128.0;
    const double cr = ycbcr[2] - 128.0;
    const auto channel = [](double v) {
        return int(std::lround(std::clamp(v, 0.0, 255.0)));
    };
    return QColor(channel(y + 1.402 * cr),
                  channel(y - 0.344136 * cb - 0.714136 * cr),
                  channel(y + 1.772 * cb));
}

jr::Samples ycbcrFromRgb(const jr::Samples &rgb)
{
    if (!rgb.isValid())
        return jr::Samples();

    jr::Samples out = rgb;
    quint8 *p = reinterpret_cast<quint8 *>(out.data.data());
    const qsizetype n = qsizetype(out.width) * out.height;
    for (qsizetype i = 0; i < n; ++i) {
        const double r = p[i * 3 + 0];
        const double g = p[i * 3 + 1];
        const double b = p[i * 3 + 2];
        const auto sample = [](double v) {
            return quint8(std::lround(std::clamp(v, 0.0, 255.0)));
        };
        p[i * 3 + 0] = sample(0.299 * r + 0.587 * g + 0.114 * b);
        p[i * 3 + 1] = sample(128.0 - 0.168736 * r - 0.331264 * g + 0.5 * b);
        p[i * 3 + 2] = sample(128.0 + 0.5 * r - 0.418688 * g - 0.081312 * b);
    }
    return out;
}

} // namespace colormath