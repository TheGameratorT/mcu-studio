// SPDX-License-Identifier: GPL-3.0-only
// Part of MCU Studio. See LICENSE and THIRD-PARTY-NOTICES.md.

#include "ReferenceFill.h"

#include <QCoreApplication>

#include <algorithm>
#include <cmath>
#include <cstring>

namespace fill {
namespace {

QString tr(const char *text)
{
    return QCoreApplication::translate("fill", text);
}

// Beyond this the two pictures are not the same crop, whatever else they are.
constexpr double kAspectTolerance = 0.01;

// The pixel grid the coefficient array covers: whole MCUs, which runs past the
// picture's right and bottom edges whenever the dimensions are not a multiple
// of the MCU size.
QSize paddedSize(const jr::Info &dest)
{
    return QSize(dest.mcusX * dest.mcuWidth, dest.mcusY * dest.mcuHeight);
}

// Grows `image` to `size` by repeating its last row and column. That is what
// libjpeg does with the real picture when it encodes the padding, so the
// blocks that straddle the edge come out holding what they would have held.
QImage extendEdges(const QImage &image, QSize size)
{
    if (image.size() == size)
        return image;
    if (image.isNull() || size.width() < image.width() || size.height() < image.height())
        return QImage();

    QImage out(size, QImage::Format_RGB888);
    if (out.isNull())
        return out;

    for (int y = 0; y < image.height(); ++y) {
        const uchar *src = image.constScanLine(y);
        uchar *dst = out.scanLine(y);
        std::memcpy(dst, src, size_t(image.width()) * 3);
        for (int x = image.width(); x < size.width(); ++x)
            std::memcpy(dst + qsizetype(x) * 3, src + qsizetype(image.width() - 1) * 3, 3);
    }
    // Every row below the picture repeats the last one, which by now has been
    // extended sideways too.
    const uchar *last = out.constScanLine(image.height() - 1);
    for (int y = image.height(); y < size.height(); ++y)
        std::memcpy(out.scanLine(y), last, size_t(size.width()) * 3);
    return out;
}

} // namespace

std::optional<Aligned> align(const QImage &reference, const jr::Info &dest, QString *error)
{
    if (reference.isNull()) {
        if (error)
            *error = tr("There is no reference picture to align.");
        return std::nullopt;
    }
    if (!dest.isValid()) {
        if (error)
            *error = tr("There is no image to align it against.");
        return std::nullopt;
    }

    Aligned out;
    out.sourceSize = reference.size();

    const QSize subject(dest.width, dest.height);
    QImage rgb = reference.convertToFormat(QImage::Format_RGB888);
    if (rgb.size() != subject) {
        const double refAspect = double(rgb.width()) / rgb.height();
        const double subjectAspect = double(subject.width()) / subject.height();
        out.aspectChanged = std::abs(refAspect - subjectAspect) / subjectAspect > kAspectTolerance;
        rgb = rgb.scaled(subject, Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
        out.scaled = true;
        if (rgb.format() != QImage::Format_RGB888)
            rgb = rgb.convertToFormat(QImage::Format_RGB888);
    }

    out.image = extendEdges(rgb, paddedSize(dest));
    if (out.image.isNull()) {
        if (error)
            *error = tr("The reference picture could not be scaled to fit.");
        return std::nullopt;
    }
    return out;
}

jr::Samples patchFor(const Aligned &reference, const jr::Info &dest, QRect mcuRect,
                     QPoint mcuOffset)
{
    jr::Samples out;
    if (!reference.isValid() || mcuRect.isEmpty() || !dest.isValid())
        return out;

    out.width = mcuRect.width() * dest.mcuWidth;
    out.height = mcuRect.height() * dest.mcuHeight;
    out.data.resize(qsizetype(out.width) * out.height * 3);

    // Where in the reference the rectangle reads from, once the alignment
    // offset has been applied.
    const int srcX = (mcuRect.x() + mcuOffset.x()) * dest.mcuWidth;
    const int srcY = (mcuRect.y() + mcuOffset.y()) * dest.mcuHeight;
    const int maxX = reference.image.width() - 1;
    const int maxY = reference.image.height() - 1;

    for (int y = 0; y < out.height; ++y) {
        const int sy = std::clamp(srcY + y, 0, maxY);
        const uchar *src = reference.image.constScanLine(sy);
        uchar *dst = reinterpret_cast<uchar *>(out.data.data()) + qsizetype(y) * out.width * 3;
        // The common case is a run of in-bounds pixels with nothing to clamp,
        // so take it a row at a time and only walk pixel by pixel at the edges.
        const int firstInside = std::clamp(-srcX, 0, out.width);
        const int lastInside = std::clamp(maxX - srcX + 1, 0, out.width);
        for (int x = 0; x < firstInside; ++x)
            std::memcpy(dst + qsizetype(x) * 3, src, 3);
        if (lastInside > firstInside) {
            std::memcpy(dst + qsizetype(firstInside) * 3, src + qsizetype(srcX + firstInside) * 3,
                        size_t(lastInside - firstInside) * 3);
        }
        for (int x = lastInside; x < out.width; ++x)
            std::memcpy(dst + qsizetype(x) * 3, src + qsizetype(maxX) * 3, 3);
    }
    return out;
}

Plan planFor(const QByteArray &mask, const jr::Info &dest)
{
    Plan plan;
    if (!dest.isValid() || mask.size() != qsizetype(dest.mcusY) * dest.mcusX)
        return plan;

    int top = dest.mcusY, left = dest.mcusX, bottom = -1, right = -1;
    for (int r = 0; r < dest.mcusY; ++r) {
        for (int c = 0; c < dest.mcusX; ++c) {
            if (!mask.at(qsizetype(r) * dest.mcusX + c))
                continue;
            ++plan.mcuCount;
            top = std::min(top, r);
            left = std::min(left, c);
            bottom = std::max(bottom, r);
            right = std::max(right, c);
        }
    }
    if (plan.mcuCount > 0)
        plan.mcuRect = QRect(QPoint(left, top), QPoint(right, bottom));
    return plan;
}

std::optional<jr::Op> build(const QByteArray &destJpeg, const jr::Info &dest,
                            const Aligned &reference, const QByteArray &mask, const Plan &plan,
                            QPoint mcuOffset, QString *error)
{
    if (!plan.isValid()) {
        if (error)
            *error = tr("No blocks are selected to fill.");
        return std::nullopt;
    }
    if (!reference.isValid()) {
        if (error)
            *error = tr("There is no reference picture to fill from.");
        return std::nullopt;
    }
    if (mask.size() != qsizetype(dest.mcusY) * dest.mcusX) {
        if (error)
            *error = tr("The selection does not match the image's block grid.");
        return std::nullopt;
    }

    // Only the bounding box of the selection is compressed. A selection with
    // holes in it costs those holes, which is cheap next to running the encoder
    // over the whole picture and lets one quantize serve every block.
    const jr::Samples rgb = patchFor(reference, dest, plan.mcuRect, mcuOffset);
    if (!rgb.isValid()) {
        if (error)
            *error = tr("The reference picture yielded no pixels for that selection.");
        return std::nullopt;
    }

    const auto patch = jr::quantizePatch(destJpeg, rgb, error);
    if (!patch)
        return std::nullopt;
    if (patch->mcusX != plan.mcuRect.width() || patch->mcusY != plan.mcuRect.height()) {
        if (error) {
            *error = tr("The quantized patch came back as %1 x %2 blocks instead of %3 x %4.")
                         .arg(patch->mcusX)
                         .arg(patch->mcusY)
                         .arg(plan.mcuRect.width())
                         .arg(plan.mcuRect.height());
        }
        return std::nullopt;
    }

    // The payload is consumed one MCU per covered MCU in scan order, so it has
    // to be assembled by walking the selection the same way the transform will.
    const qsizetype perMcu = patch->coefsPerMcu() * qsizetype(sizeof(qint16));
    QByteArray payload;
    payload.reserve(perMcu * plan.mcuCount);
    for (int r = plan.mcuRect.top(); r <= plan.mcuRect.bottom(); ++r) {
        for (int c = plan.mcuRect.left(); c <= plan.mcuRect.right(); ++c) {
            if (!mask.at(qsizetype(r) * dest.mcusX + c))
                continue;
            const qint16 *src = patch->mcu(r - plan.mcuRect.top(), c - plan.mcuRect.left());
            if (!src) {
                if (error)
                    *error = tr("The quantized patch is missing block row %1, column %2.")
                                 .arg(r)
                                 .arg(c);
                return std::nullopt;
            }
            payload.append(reinterpret_cast<const char *>(src), perMcu);
        }
    }

    return jr::Op::fillScope(jr::Scope::mask(mask, dest.mcusY, dest.mcusX), std::move(payload),
                             plan.mcuCount);
}

} // namespace fill
