// SPDX-License-Identifier: GPL-3.0-only
// Part of MCU Studio. See LICENSE and THIRD-PARTY-NOTICES.md.

#include "ReferenceImage.h"

#include <QCoreApplication>
#include <QFile>
#include <QFileInfo>

#include <algorithm>
#include <cmath>
#include <cstring>

#include "ColorMath.h"

namespace refimage {
namespace {

bool looksLikeJpeg(const QByteArray &bytes)
{
    return bytes.size() >= 2 && quint8(bytes.at(0)) == 0xFF && quint8(bytes.at(1)) == 0xD8;
}

// Whether the picture has any colour to lend.
//
// Read off the samples rather than the component count, because the component
// count only catches half of it: a one-component JPEG decodes to Cb = Cr = 128,
// but so does a perfectly ordinary three-component JPEG that happens to store a
// greyscale picture -- which is what a "black and white" export from most tools
// is. Either way there is no chroma in the file, so a match would drag the
// damaged image's colour to neutral instead of restoring it.
//
// A real photograph, however drab, has some samples off neutral; the tolerance
// is there for rounding in the conversion, not for grey-ish pictures.
bool neutralChroma(const jr::Samples &ycbcr)
{
    constexpr int kTolerance = 1;
    const quint8 *p = reinterpret_cast<const quint8 *>(ycbcr.data.constData());
    const qsizetype n = qsizetype(ycbcr.width) * ycbcr.height;
    for (qsizetype i = 0; i < n; ++i) {
        if (std::abs(int(p[i * 3 + 1]) - 128) > kTolerance
            || std::abs(int(p[i * 3 + 2]) - 128) > kTolerance)
            return false;
    }
    return true;
}

// The reference is only ever measured, so a file that will not decode is a
// dead end rather than something to repair -- say so plainly and let the user
// pick another copy.
std::optional<Loaded> loadJpeg(const QByteArray &bytes, QString *error)
{
    const auto ycbcr = jr::decodeYCbCr(bytes, error);
    if (!ycbcr || !ycbcr->isValid())
        return std::nullopt;
    const auto rgb = jr::decodeRgb(bytes, error);
    if (!rgb || !rgb->isValid())
        return std::nullopt;

    Loaded out;
    out.ycbcr = *ycbcr;
    out.preview = rgb->toImage();
    out.monochrome = neutralChroma(out.ycbcr);
    return out;
}

std::optional<Loaded> loadViaQt(const QByteArray &bytes, QString *error)
{
    QImage image;
    if (!image.loadFromData(bytes)) {
        if (error) {
            *error = QCoreApplication::translate(
                "refimage", "This is not a JPEG, and Qt could not read it either.");
        }
        return std::nullopt;
    }

    const QImage rgb888 = image.convertToFormat(QImage::Format_RGB888);
    jr::Samples rgb;
    rgb.width = rgb888.width();
    rgb.height = rgb888.height();
    rgb.data.resize(qsizetype(rgb.width) * rgb.height * 3);
    // QImage rows are padded to a four-byte boundary, so copy row by row rather
    // than trusting the buffer to be contiguous.
    for (int y = 0; y < rgb.height; ++y) {
        std::memcpy(rgb.data.data() + qsizetype(y) * rgb.width * 3, rgb888.constScanLine(y),
                    size_t(rgb.width) * 3);
    }

    Loaded out;
    out.ycbcr = colormath::ycbcrFromRgb(rgb);
    out.preview = rgb888;
    out.monochrome = out.ycbcr.isValid() && neutralChroma(out.ycbcr);
    if (!out.isValid()) {
        if (error)
            *error = QCoreApplication::translate("refimage", "That image is empty.");
        return std::nullopt;
    }
    return out;
}

} // namespace

std::optional<Loaded> load(const QString &path, QString *error)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        if (error) {
            *error = QCoreApplication::translate("refimage", "Could not read %1: %2")
                         .arg(QFileInfo(path).fileName(), file.errorString());
        }
        return std::nullopt;
    }
    const QByteArray bytes = file.readAll();
    file.close();

    if (bytes.isEmpty()) {
        if (error) {
            *error = QCoreApplication::translate("refimage", "%1 is empty.")
                         .arg(QFileInfo(path).fileName());
        }
        return std::nullopt;
    }

    // A JPEG that libjpeg turns down is still worth handing to Qt: a truncated
    // small copy often has enough of a picture in it to measure, and Qt's
    // plugin salvages the scanlines it got.
    if (looksLikeJpeg(bytes)) {
        QString jpegError;
        if (auto loaded = loadJpeg(bytes, &jpegError))
            return loaded;
        if (auto loaded = loadViaQt(bytes, nullptr))
            return loaded;
        if (error)
            *error = jpegError;
        return std::nullopt;
    }
    return loadViaQt(bytes, error);
}

QRect centredIn(QRect rect, QPoint centre, QSize bounds)
{
    if (rect.isEmpty() || bounds.isEmpty())
        return QRect();

    const int w = std::min(rect.width(), bounds.width());
    const int h = std::min(rect.height(), bounds.height());
    const int x = std::clamp(centre.x() - w / 2, 0, bounds.width() - w);
    const int y = std::clamp(centre.y() - h / 2, 0, bounds.height() - h);
    return QRect(x, y, w, h);
}

QRect correspondingRect(QRect rect, QSize subjectSize, QSize referenceSize, int minEdge)
{
    if (rect.isEmpty() || subjectSize.isEmpty() || referenceSize.isEmpty())
        return QRect();

    const double sx = double(referenceSize.width()) / subjectSize.width();
    const double sy = double(referenceSize.height()) / subjectSize.height();

    const int w = std::max(minEdge, int(std::lround(rect.width() * sx)));
    const int h = std::max(minEdge, int(std::lround(rect.height() * sy)));
    const QPoint centre(int(std::lround((rect.x() + rect.width() / 2.0) * sx)),
                        int(std::lround((rect.y() + rect.height() / 2.0) * sy)));
    return centredIn(QRect(0, 0, w, h), centre, referenceSize);
}

} // namespace refimage
