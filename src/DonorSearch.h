// SPDX-License-Identifier: GPL-3.0-only
// Part of MCU Studio. See LICENSE and THIRD-PARTY-NOTICES.md.

// Choosing a donor header, and the frame size behind it, by measurement.
//
// A donor whose Huffman tables are not the ones the damaged data was coded
// with turns the data into a storm of invalid codes within a few hundred
// bytes; one whose tables match decodes it with hardly an error once past the
// splice point. That difference is large, cheap to measure and needs no eye,
// so a folder of candidate donors can be ranked before anyone looks at a
// preview. The frame width is found the same way from the other side: with
// the wrong width every MCU row lands shifted against the one above it, and
// with the right one the rows continue each other.
#pragma once

#include <QByteArray>
#include <QString>
#include <QStringList>
#include <QVector>

#include <functional>

#include "DonorHeader.h"

namespace donorsearch {

struct Ranked {
    QString path;
    QString camera;      // from the donor's Exif, when it has one
    int width = 0, height = 0;
    QString problem;     // why it cannot be a donor at all, empty if it can
    int decodedMcus = 0; // MCUs the splice's data covered in the sample
    int anomalies = 0;   // of those, how many failed to decode cleanly
    double score = 0;    // 0..1, higher is better

    bool usable() const { return problem.isEmpty(); }
};

// Splices each of `paths` onto `broken` at `offset` and ranks them by how
// cleanly the first part of the data decodes. `cancelled`, when given, is
// polled between candidates.
QVector<Ranked> rank(const QByteArray &broken, qsizetype offset, const QStringList &paths,
                     const donor::SpliceOptions &options = {},
                     const std::function<bool()> &cancelled = {});

struct WidthCandidate {
    int width = 0;
    int height = 0;
    double cost = 0; // lower is better
};

// Tries frame widths in [minWidth, maxWidth] in steps of the donor's MCU
// width (and the donor's own width and height, swapped), decoding the first
// part of the data under each, and ranks them by how well MCU rows continue
// each other. The height is scaled to keep the pixel count.
QVector<WidthCandidate> detectWidth(const QByteArray &donorBytes, const QByteArray &broken,
                                    qsizetype offset, const donor::SpliceOptions &options,
                                    int minWidth, int maxWidth,
                                    const std::function<bool()> &cancelled = {});

} // namespace donorsearch
