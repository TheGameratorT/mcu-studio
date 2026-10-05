// SPDX-License-Identifier: GPL-3.0-only
// Part of MCU Studio. See LICENSE and THIRD-PARTY-NOTICES.md.

// Finding damage, and the edits that undo it, by looking at the picture.
//
// Three questions come up again and again in a repair, and each has an answer
// that can be measured instead of guessed:
//
//  - Where does the damage start? Damaged MCUs disagree with their neighbors
//    across the block edge far more than the picture's own detail explains,
//    and the bitstream map knows where decoding itself went wrong.
//  - How far has the stream slid? After an insert or delete of k MCUs at the
//    damage point, the content below the seam either continues the content
//    above it or it does not. Trying every k and measuring the seam finds the
//    one that does -- with the seam's mean difference removed first, so a DC
//    drift that has not been corrected yet does not hide the right answer.
//  - How far has the color drifted? Across the edge of the damaged region,
//    the median difference per channel is the offset; everything else is the
//    picture's own gradient, which the median ignores.
//
// Everything here is advisory: the results are offered as candidates and
// previewed, and nothing is applied without being chosen.
#pragma once

#include <QByteArray>
#include <QVector>

#include "Bitstream.h"
#include "JpegRepair.h"

namespace analysis {

struct DamageMap {
    QVector<float> score; // per MCU, 0 (looks fine) .. 1 (certainly damaged)
    int mcusX = 0, mcusY = 0;
    int firstDamaged = -1;

    bool isValid() const { return !score.isEmpty(); }
};

// Scores every MCU from its edges in `ycbcr` (the current render) and, when
// given, the bitstream map's anomalies -- which describe the base file, so
// pass `unitSrc` from jr_trace to carry them to where the MCUs now sit, or
// nullptr when no coefficient step has moved anything.
DamageMap damage(const jr::Info &info, const jr::Samples &ycbcr, const bitstream::Map *map = nullptr,
                 const QVector<qint32> *unitSrc = nullptr);

// The next MCU after `from` in scan order scoring at least `threshold`, or -1.
int nextDamaged(const DamageMap &map, int from, float threshold = 0.5f);

struct AlignCandidate {
    int shift = 0;     // > 0: insert this many MCUs; < 0: delete -shift
    double cost = 0;   // seam discontinuity after the shift, lower is better
    double offset[3] = {0, 0, 0}; // the mean step across the seam, per channel
};

// Ranks shifts of the stream at MCU `start` by how well the content after it
// continues the content above it. `start` must not be in the first MCU row.
// The candidate list includes 0, so "it is already aligned" can win.
QVector<AlignCandidate> autoAlign(const jr::Info &info, const jr::Samples &ycbcr, int start,
                                  int maxShift);

struct DcEstimate {
    bool valid = false;
    int cdelta[3] = {0, 0, 0};       // to apply to the region, per component
    double pixelShift[3] = {0, 0, 0}; // the same in 0-255 sample terms
    // How much the boundary agrees with itself once the offset is taken out,
    // 0..1. Low means the region and its surroundings do not show continuous
    // content, and the estimate is a guess.
    double confidence = 0;
    int boundaryPixels = 0;
};

// The DC offsets that bring the MCUs in `mask` (one byte per MCU) in line
// with the MCUs bordering them. Measured on block edges computed from the
// coefficients, per component at its own resolution: in the decoded picture
// the chroma upsampler blends the two sides of an edge and halves the step.
DcEstimate autoDc(const jr::Coefs &coefs, const QByteArray &mask);

// What is wrong with a file, at a glance, for sorting a folder of them.
struct Triage {
    enum class Verdict {
        Healthy,       // decodes cleanly to its End Of Image
        Trailer,       // healthy, with data after the image (MPF, motion photo, ...)
        Ransomware,    // carries a ransomware footer: header encrypted, rest intact
        HeaderDamaged, // libjpeg cannot read the header: needs a donor
        Truncated,     // the data stops before the picture does
        DecodeErrors,  // the stream has Huffman errors: desync or corruption
        NotJpeg,       // nothing here looks like a JPEG
    };
    Verdict verdict = Verdict::NotJpeg;
    QString summary;
    int width = 0, height = 0;
    int mcus = 0, decodeErrors = 0;
    int firstError = -1; // MCU index, -1 for none
    // Share of the picture's MCUs with data behind them, 0..1.
    double readable = 0;
};

Triage triage(const QByteArray &bytes);
QString verdictName(Triage::Verdict v);

// How well vertically adjacent MCU rows of `ycbcr` continue each other, for
// ranking frame widths: lower is better. Considers the first `rows` MCU rows.
double rowContinuity(const jr::Info &info, const jr::Samples &ycbcr, int rows);

} // namespace analysis
