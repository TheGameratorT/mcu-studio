// SPDX-License-Identifier: GPL-3.0-only
// Part of MCU Studio. See LICENSE and THIRD-PARTY-NOTICES.md.

// Synthetic JPEGs for the tests: generated, so every property a test relies on
// (sampling, restart interval, progressive, size, content) is chosen rather
// than hoped for, and nothing binary has to live in the repository.
#pragma once

#include <QByteArray>
#include <QtGlobal>

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include <jpeglib.h>

namespace testutil {

struct Spec {
    int width = 320;
    int height = 240;
    int h0 = 2, v0 = 2; // luma sampling; chroma is 1x1
    int quality = 85;
    int restartInterval = 0;
    bool progressive = false;
    bool optimize = false;
    int seed = 1;
};

// Smooth, photograph-like content: broad gradients, a few blobs, mild
// texture. Continuous across MCU edges, which is what the alignment and DC
// estimators measure.
inline QByteArray pixels(const Spec &s)
{
    QByteArray px(qsizetype(s.width) * s.height * 3, Qt::Uninitialized);
    unsigned rs = unsigned(s.seed) * 2654435761u + 1;
    const auto rnd = [&rs] {
        rs = rs * 1103515245u + 12345u;
        return int((rs >> 16) & 0x7fff);
    };
    const double phase = s.seed * 0.7;
    for (int y = 0; y < s.height; ++y) {
        for (int x = 0; x < s.width; ++x) {
            const double fx = double(x) / s.width, fy = double(y) / s.height;
            const double blob = std::exp(-((fx - 0.35) * (fx - 0.35) + (fy - 0.4) * (fy - 0.4)) * 18.0);
            const double wave = std::sin(fx * 9.0 + phase) * std::cos(fy * 7.0 - phase);
            const int n = rnd() % 7 - 3;
            auto *p = reinterpret_cast<unsigned char *>(px.data()) + (qsizetype(y) * s.width + x) * 3;
            p[0] = (unsigned char)qBound(0, int(60 + 120 * fx + 60 * blob + 20 * wave) + n, 255);
            p[1] = (unsigned char)qBound(0, int(40 + 150 * fy + 30 * wave) + n, 255);
            p[2] = (unsigned char)qBound(0, int(200 - 120 * fx * fy + 50 * blob) + n, 255);
        }
    }
    return px;
}

inline QByteArray encode(const QByteArray &rgb, const Spec &s)
{
    jpeg_compress_struct c;
    jpeg_error_mgr e;
    c.err = jpeg_std_error(&e);
    jpeg_create_compress(&c);
    unsigned char *out = nullptr;
    unsigned long len = 0;
    jpeg_mem_dest(&c, &out, &len);
    c.image_width = JDIMENSION(s.width);
    c.image_height = JDIMENSION(s.height);
    c.input_components = 3;
    c.in_color_space = JCS_RGB;
    jpeg_set_defaults(&c);
    jpeg_set_quality(&c, s.quality, TRUE);
    c.comp_info[0].h_samp_factor = s.h0;
    c.comp_info[0].v_samp_factor = s.v0;
    c.comp_info[1].h_samp_factor = c.comp_info[1].v_samp_factor = 1;
    c.comp_info[2].h_samp_factor = c.comp_info[2].v_samp_factor = 1;
    c.restart_interval = unsigned(s.restartInterval);
    c.optimize_coding = s.optimize ? TRUE : FALSE;
    if (s.progressive)
        jpeg_simple_progression(&c);
    jpeg_start_compress(&c, TRUE);
    while (c.next_scanline < c.image_height) {
        JSAMPROW row = reinterpret_cast<JSAMPROW>(const_cast<char *>(rgb.constData()))
            + size_t(c.next_scanline) * size_t(s.width) * 3;
        jpeg_write_scanlines(&c, &row, 1);
    }
    jpeg_finish_compress(&c);
    jpeg_destroy_compress(&c);
    QByteArray result(reinterpret_cast<const char *>(out), qsizetype(len));
    free(out);
    return result;
}

inline QByteArray make(const Spec &s)
{
    return encode(pixels(s), s);
}

// A minimal Exif APP1 with an IFD1 thumbnail at the end, inserted after the
// SOI. `thumb` is the thumbnail's JPEG bytes.
inline QByteArray withExifThumbnail(const QByteArray &jpeg, const QByteArray &thumb)
{
    QByteArray tiff;
    const auto u16 = [&tiff](int v) { tiff.append(char(v & 0xFF)); tiff.append(char(v >> 8)); };
    const auto u32 = [&tiff](quint32 v) {
        for (int i = 0; i < 4; ++i)
            tiff.append(char((v >> (8 * i)) & 0xFF));
    };
    tiff.append("II*\0", 4);
    u32(8);              // IFD0 at 8
    u16(1);              // one entry
    u16(0x0112); u16(3); u32(1); u16(1); u16(0); // Orientation = 1
    u32(26);             // next IFD (IFD1) at 26
    // IFD1 at 26: two entries
    u16(2);
    const quint32 thumbOffset = 26 + 2 + 2 * 12 + 4;
    u16(0x0201); u16(4); u32(1); u32(thumbOffset);
    u16(0x0202); u16(4); u32(1); u32(quint32(thumb.size()));
    u32(0);
    tiff.append(thumb);
    QByteArray app1("\xFF\xE1", 2);
    const int len = 2 + 6 + int(tiff.size());
    app1.append(char(len >> 8));
    app1.append(char(len & 0xFF));
    app1.append("Exif\0\0", 6);
    app1.append(tiff);
    return jpeg.left(2) + app1 + jpeg.mid(2);
}

// An MPF primary: `primary` with an APP2 MPF index, followed by `secondary`.
inline QByteArray withMpf(const QByteArray &primary, const QByteArray &secondary)
{
    // Built twice: the second pass knows the final length of the primary.
    QByteArray app2;
    for (int pass = 0; pass < 2; ++pass) {
        const qsizetype headerAt = 2 + 4 + 4; // SOI, APP2 marker+length, "MPF\0"
        const qsizetype primaryLen = primary.size() + (pass ? app2.size() : 0);
        QByteArray t;
        const auto u16 = [&t](int v) { t.append(char(v & 0xFF)); t.append(char(v >> 8)); };
        const auto u32 = [&t](quint32 v) {
            for (int i = 0; i < 4; ++i)
                t.append(char((v >> (8 * i)) & 0xFF));
        };
        t.append("II*\0", 4);
        u32(8);
        u16(3);
        u16(0xB000); u16(7); u32(4); t.append("0100", 4);
        u16(0xB001); u16(4); u32(1); u32(2);
        const quint32 entriesAt = 8 + 2 + 3 * 12 + 4;
        u16(0xB002); u16(7); u32(32); u32(entriesAt);
        u32(0);
        // entry 1: primary, offset 0
        u32(0x030000); u32(quint32(primaryLen)); u32(0); u16(0); u16(0);
        // entry 2: offset from the MP header to the secondary
        u32(0x010001); u32(quint32(secondary.size())); u32(quint32(primaryLen - headerAt)); u16(0); u16(0);
        app2 = QByteArray("\xFF\xE2", 2);
        const int len = 2 + 4 + int(t.size());
        app2.append(char(len >> 8));
        app2.append(char(len & 0xFF));
        app2.append("MPF\0", 4);
        app2.append(t);
    }
    return primary.left(2) + app2 + primary.mid(2) + secondary;
}

} // namespace testutil
