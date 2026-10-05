// SPDX-License-Identifier: GPL-3.0-only
// Part of MCU Studio. See LICENSE and THIRD-PARTY-NOTICES.md.

#include "DonorSearch.h"

#include <QFile>
#include <QFileInfo>

#include <algorithm>

#include "Analysis.h"
#include "Bitstream.h"
#include "Exif.h"
#include "JpegRepair.h"

namespace donorsearch {
namespace {

// How much of the damaged file's data each candidate decodes. Enough to get
// well past the splice point's own garbage, small enough that a folder of
// hundreds is ranked in seconds.
constexpr qsizetype kSampleBytes = 384 * 1024;

QByteArray sample(const QByteArray &spliced, qsizetype headerSize)
{
    if (spliced.size() <= headerSize + kSampleBytes)
        return spliced;
    QByteArray out = spliced.left(headerSize + kSampleBytes);
    // Do not leave the cut in the middle of a stuffed FF 00 or a marker.
    while (!out.isEmpty() && quint8(out.back()) == 0xFF)
        out.chop(1);
    out.append("\xFF\xD9", 2);
    return out;
}

} // namespace

QVector<Ranked> rank(const QByteArray &broken, qsizetype offset, const QStringList &paths,
                     const donor::SpliceOptions &options, const std::function<bool()> &cancelled)
{
    QVector<Ranked> out;
    for (const QString &path : paths) {
        if (cancelled && cancelled())
            break;
        Ranked r;
        r.path = path;
        QFile file(path);
        if (!file.open(QIODevice::ReadOnly)) {
            r.problem = file.errorString();
            out.append(r);
            continue;
        }
        const QByteArray bytes = file.readAll();
        const donor::Layout layout = donor::scan(bytes);
        r.camera = exif::cameraModel(bytes);
        r.width = layout.width;
        r.height = layout.height;
        r.problem = donor::donorProblem(layout);
        if (!r.usable()) {
            out.append(r);
            continue;
        }
        QString error;
        const auto spliced = donor::splice(bytes, layout, broken, offset, &error, options);
        if (!spliced) {
            r.problem = error;
            out.append(r);
            continue;
        }
        const bitstream::Map map = bitstream::map(sample(spliced->bytes, spliced->headerSize));
        if (!map.isValid()) {
            r.problem = map.unsupported;
            out.append(r);
            continue;
        }
        // Count what the sample actually covered: past it everything reads as
        // truncated, which says nothing about the donor.
        for (const bitstream::McuRecord &m : map.mcus) {
            if (m.anomalies == bitstream::Truncated)
                break;
            ++r.decodedMcus;
            if (m.anomalies)
                ++r.anomalies;
        }
        r.score = r.decodedMcus > 0 ? 1.0 - double(r.anomalies) / r.decodedMcus : 0.0;
        // A donor that decodes only a sliver before stopping is no better than
        // one that fails: weigh by how far it got, up to a few hundred MCUs.
        r.score *= std::min(1.0, r.decodedMcus / 200.0);
        out.append(r);
    }
    std::stable_sort(out.begin(), out.end(), [](const Ranked &a, const Ranked &b) {
        if (a.usable() != b.usable())
            return a.usable();
        return a.score > b.score;
    });
    return out;
}

QVector<WidthCandidate> detectWidth(const QByteArray &donorBytes, const QByteArray &broken,
                                    qsizetype offset, const donor::SpliceOptions &options,
                                    int minWidth, int maxWidth, const std::function<bool()> &cancelled)
{
    QVector<WidthCandidate> out;
    const donor::Layout layout = donor::scan(donorBytes);
    if (!donor::donorProblem(layout).isEmpty() || layout.width <= 0 || layout.height <= 0)
        return out;

    // The MCU width, from a probe of the donor itself.
    const auto info = jr::probe(donorBytes);
    const int step = info ? info->mcuWidth : 16;
    const qint64 pixels = qint64(layout.width) * layout.height;

    QVector<int> widths;
    for (int w = std::max(step, minWidth - minWidth % step); w <= maxWidth; w += step)
        widths.append(w);
    widths.append(layout.width);
    widths.append(layout.height);
    std::sort(widths.begin(), widths.end());
    widths.erase(std::unique(widths.begin(), widths.end()), widths.end());

    for (int w : widths) {
        if (cancelled && cancelled())
            break;
        if (w <= 0 || w > 65535)
            continue;
        donor::SpliceOptions o = options;
        o.width = w;
        o.height = int(std::clamp<qint64>(pixels / w, 8, 65535));
        QString error;
        const auto spliced = donor::splice(donorBytes, layout, broken, offset, &error, o);
        if (!spliced)
            continue;
        const auto coefs = jr::Coefs::load(sample(spliced->bytes, spliced->headerSize));
        if (!coefs)
            continue;
        const jr::Info inf = coefs->info();
        // Only the rows the sample reached carry data -- past it libjpeg
        // fills with zeros, which would look perfectly continuous. Judge
        // those, and nothing narrower than a few rows.
        int rows = 0;
        const int per = inf.mcusX * inf.blocksPerMcu * 64;
        while (rows < std::min(inf.mcusY, 24)) {
            const qint16 *p = coefs->mcu(rows * inf.mcusX);
            if (std::all_of(p, p + per, [](qint16 v) { return v == 0; }))
                break;
            ++rows;
        }
        if (rows < 3)
            continue;
        jr::Samples img;
        img.width = inf.width;
        img.height = inf.height;
        img.data = QByteArray(qsizetype(img.width) * img.height * 3, '\0');
        QVector<int> wanted;
        for (int r = 0; r < rows; ++r)
            wanted.append(r);
        coefs->renderRows(img, wanted, true);
        WidthCandidate c;
        c.width = w;
        c.height = o.height;
        c.cost = analysis::rowContinuity(inf, img, rows);
        out.append(c);
    }
    std::stable_sort(out.begin(), out.end(),
                     [](const WidthCandidate &a, const WidthCandidate &b) { return a.cost < b.cost; });
    return out;
}

} // namespace donorsearch
