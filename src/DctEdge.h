// SPDX-License-Identifier: GPL-3.0-only
// Part of MCU Studio. See LICENSE and THIRD-PARTY-NOTICES.md.

// The samples along one edge of an 8x8 block, straight from its coefficients.
//
// Comparing two blocks across the edge they share needs only those samples,
// and a one-dimensional IDCT of the edge gives them for a fraction of the
// cost of decoding the blocks -- and, for chroma, before upsampling blends
// the two sides of the edge into each other.
#pragma once

#include <QtGlobal>

#include <cmath>

namespace dctedge {

enum Edge { Top = 0, Bottom = 1, Left = 2, Right = 3 };

// `coef` is 64 quantized coefficients in natural order, `q` the 64 steps of
// the table they were quantized with. `out` receives the 8 samples along the
// edge (left to right, or top to bottom), level-shifted: 0 is mid-gray.
template <typename Coef, typename Quant>
void samples(const Coef *coef, const Quant *q, Edge edge, double out[8])
{
    static const struct Table {
        double t[8][8];
        Table()
        {
            for (int k = 0; k < 8; ++k)
                for (int n = 0; n < 8; ++n)
                    t[k][n] = (k == 0 ? std::sqrt(0.5) : 1.0) / 2.0 * std::cos((2 * n + 1) * k * M_PI / 16.0);
        }
    } T;
    const bool horizontal = edge == Top || edge == Bottom;
    const int fixed = (edge == Top || edge == Left) ? 0 : 7;
    double g[8];
    for (int a = 0; a < 8; ++a) {
        double sum = 0;
        for (int b = 0; b < 8; ++b) {
            const int idx = horizontal ? b * 8 + a : a * 8 + b;
            sum += T.t[b][fixed] * double(coef[idx]) * double(q[idx]);
        }
        g[a] = sum;
    }
    for (int n = 0; n < 8; ++n) {
        double sum = 0;
        for (int a = 0; a < 8; ++a)
            sum += T.t[a][n] * g[a];
        out[n] = sum;
    }
}

} // namespace dctedge
