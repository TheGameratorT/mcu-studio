/*
 * jr_render.c -- decoding jr_coefs straight to pixels.
 *
 * The arithmetic is the Independent JPEG Group's, as libjpeg-turbo ships it:
 * the accurate integer IDCT (jidctint.c), the "fancy" triangle-filter chroma
 * upsamplers (jdsample.c), the edge handling of the main buffer controller
 * (jdmainct.c) and the JFIF YCbCr->RGB tables (jdcolor.c). Reimplemented here
 * against coefficients held in memory, so a preview can decode only the MCU
 * rows an edit touched, without running the entropy coder in either
 * direction, and still come out sample-for-sample identical to what
 * libjpeg-turbo produces from the exported file. See README.ijg for the IJG
 * license this code is distributed under.
 */

#include "jpegrepair_core.h"

#include <stdlib.h>
#include <string.h>

/* ------------------------------------------------------------------ */
/* Sample range limiting (jdmaster.c prepare_range_limit_table)        */
/* ------------------------------------------------------------------ */

/* Samples past black or white saturate. libjpeg-turbo's C IDCT instead masks
   to 10 bits and wraps the far part of that range, so an overdriven block
   comes out inverted -- but its SIMD IDCTs, which are what djpeg, browsers
   and image viewers actually run on x86 and ARM, saturate. In range the two
   agree exactly; out of range this follows what people will see. */
static uint8_t idct_limit(int x) {
  return (uint8_t)(x < -128 ? 0 : (x > 127 ? 255 : x + 128));
}

static uint8_t clamp255(int v) { return (uint8_t)(v < 0 ? 0 : (v > 255 ? 255 : v)); }

/* ------------------------------------------------------------------ */
/* jidctint.c: jpeg_idct_islow                                         */
/* ------------------------------------------------------------------ */

#define CONST_BITS 13
#define PASS1_BITS 2
#define FIX_0_298631336 ((int64_t)2446)
#define FIX_0_390180644 ((int64_t)3196)
#define FIX_0_541196100 ((int64_t)4433)
#define FIX_0_765366865 ((int64_t)6270)
#define FIX_0_899976223 ((int64_t)7373)
#define FIX_1_175875602 ((int64_t)9633)
#define FIX_1_501321110 ((int64_t)12299)
#define FIX_1_847759065 ((int64_t)15137)
#define FIX_1_961570560 ((int64_t)16069)
#define FIX_2_053119869 ((int64_t)16819)
#define FIX_2_562915447 ((int64_t)20995)
#define FIX_3_072711026 ((int64_t)25172)
#define DESCALE(x, n) (((x) + ((int64_t)1 << ((n)-1))) >> (n))

static void idct_islow(const int16_t *in, const uint16_t *q, uint8_t *out, size_t stride) {
  int ws[64];
  int ctr;
  int64_t tmp0, tmp1, tmp2, tmp3, tmp10, tmp11, tmp12, tmp13, z1, z2, z3, z4, z5;

  /* Pass 1: columns from input, into the work array. */
  for (ctr = 0; ctr < 8; ctr++) {
    const int16_t *ip = in + ctr;
    const uint16_t *qp = q + ctr;
    int *wp = ws + ctr;
    if (ip[8] == 0 && ip[16] == 0 && ip[24] == 0 && ip[32] == 0 && ip[40] == 0 && ip[48] == 0 &&
        ip[56] == 0) {
      const int dcval = (int)(((int64_t)ip[0] * qp[0]) * (1 << PASS1_BITS));
      wp[0] = wp[8] = wp[16] = wp[24] = wp[32] = wp[40] = wp[48] = wp[56] = dcval;
      continue;
    }
    z2 = (int64_t)ip[16] * qp[16];
    z3 = (int64_t)ip[48] * qp[48];
    z1 = (z2 + z3) * FIX_0_541196100;
    tmp2 = z1 + z3 * (-FIX_1_847759065);
    tmp3 = z1 + z2 * FIX_0_765366865;
    z2 = (int64_t)ip[0] * qp[0];
    z3 = (int64_t)ip[32] * qp[32];
    tmp0 = (z2 + z3) * ((int64_t)1 << CONST_BITS);
    tmp1 = (z2 - z3) * ((int64_t)1 << CONST_BITS);
    tmp10 = tmp0 + tmp3;
    tmp13 = tmp0 - tmp3;
    tmp11 = tmp1 + tmp2;
    tmp12 = tmp1 - tmp2;

    tmp0 = (int64_t)ip[56] * qp[56];
    tmp1 = (int64_t)ip[40] * qp[40];
    tmp2 = (int64_t)ip[24] * qp[24];
    tmp3 = (int64_t)ip[8] * qp[8];
    z1 = tmp0 + tmp3;
    z2 = tmp1 + tmp2;
    z3 = tmp0 + tmp2;
    z4 = tmp1 + tmp3;
    z5 = (z3 + z4) * FIX_1_175875602;
    tmp0 = tmp0 * FIX_0_298631336;
    tmp1 = tmp1 * FIX_2_053119869;
    tmp2 = tmp2 * FIX_3_072711026;
    tmp3 = tmp3 * FIX_1_501321110;
    z1 = z1 * (-FIX_0_899976223);
    z2 = z2 * (-FIX_2_562915447);
    z3 = z3 * (-FIX_1_961570560);
    z4 = z4 * (-FIX_0_390180644);
    z3 += z5;
    z4 += z5;
    tmp0 += z1 + z3;
    tmp1 += z2 + z4;
    tmp2 += z2 + z3;
    tmp3 += z1 + z4;

    wp[0] = (int)DESCALE(tmp10 + tmp3, CONST_BITS - PASS1_BITS);
    wp[56] = (int)DESCALE(tmp10 - tmp3, CONST_BITS - PASS1_BITS);
    wp[8] = (int)DESCALE(tmp11 + tmp2, CONST_BITS - PASS1_BITS);
    wp[48] = (int)DESCALE(tmp11 - tmp2, CONST_BITS - PASS1_BITS);
    wp[16] = (int)DESCALE(tmp12 + tmp1, CONST_BITS - PASS1_BITS);
    wp[40] = (int)DESCALE(tmp12 - tmp1, CONST_BITS - PASS1_BITS);
    wp[24] = (int)DESCALE(tmp13 + tmp0, CONST_BITS - PASS1_BITS);
    wp[32] = (int)DESCALE(tmp13 - tmp0, CONST_BITS - PASS1_BITS);
  }

  /* Pass 2: rows from the work array, into the output. */
  for (ctr = 0; ctr < 8; ctr++) {
    const int *wp = ws + ctr * 8;
    uint8_t *op = out + (size_t)ctr * stride;
    if (wp[1] == 0 && wp[2] == 0 && wp[3] == 0 && wp[4] == 0 && wp[5] == 0 && wp[6] == 0 &&
        wp[7] == 0) {
      const uint8_t dc = idct_limit((int)DESCALE((int64_t)wp[0], PASS1_BITS + 3));
      memset(op, dc, 8);
      continue;
    }
    z2 = wp[2];
    z3 = wp[6];
    z1 = (z2 + z3) * FIX_0_541196100;
    tmp2 = z1 + z3 * (-FIX_1_847759065);
    tmp3 = z1 + z2 * FIX_0_765366865;
    tmp0 = ((int64_t)wp[0] + wp[4]) * ((int64_t)1 << CONST_BITS);
    tmp1 = ((int64_t)wp[0] - wp[4]) * ((int64_t)1 << CONST_BITS);
    tmp10 = tmp0 + tmp3;
    tmp13 = tmp0 - tmp3;
    tmp11 = tmp1 + tmp2;
    tmp12 = tmp1 - tmp2;

    tmp0 = wp[7];
    tmp1 = wp[5];
    tmp2 = wp[3];
    tmp3 = wp[1];
    z1 = tmp0 + tmp3;
    z2 = tmp1 + tmp2;
    z3 = tmp0 + tmp2;
    z4 = tmp1 + tmp3;
    z5 = (z3 + z4) * FIX_1_175875602;
    tmp0 = tmp0 * FIX_0_298631336;
    tmp1 = tmp1 * FIX_2_053119869;
    tmp2 = tmp2 * FIX_3_072711026;
    tmp3 = tmp3 * FIX_1_501321110;
    z1 = z1 * (-FIX_0_899976223);
    z2 = z2 * (-FIX_2_562915447);
    z3 = z3 * (-FIX_1_961570560);
    z4 = z4 * (-FIX_0_390180644);
    z3 += z5;
    z4 += z5;
    tmp0 += z1 + z3;
    tmp1 += z2 + z4;
    tmp2 += z2 + z3;
    tmp3 += z1 + z4;

    op[0] = idct_limit((int)DESCALE(tmp10 + tmp3, CONST_BITS + PASS1_BITS + 3));
    op[7] = idct_limit((int)DESCALE(tmp10 - tmp3, CONST_BITS + PASS1_BITS + 3));
    op[1] = idct_limit((int)DESCALE(tmp11 + tmp2, CONST_BITS + PASS1_BITS + 3));
    op[6] = idct_limit((int)DESCALE(tmp11 - tmp2, CONST_BITS + PASS1_BITS + 3));
    op[2] = idct_limit((int)DESCALE(tmp12 + tmp1, CONST_BITS + PASS1_BITS + 3));
    op[5] = idct_limit((int)DESCALE(tmp12 - tmp1, CONST_BITS + PASS1_BITS + 3));
    op[3] = idct_limit((int)DESCALE(tmp13 + tmp0, CONST_BITS + PASS1_BITS + 3));
    op[4] = idct_limit((int)DESCALE(tmp13 - tmp0, CONST_BITS + PASS1_BITS + 3));
  }
}

/* ------------------------------------------------------------------ */
/* One component's samples for a band of rows                          */
/* ------------------------------------------------------------------ */

typedef struct {
  uint8_t *pix;   /* rows [row0, row0 + rows) of the padded sample plane */
  int row0, rows;
  size_t stride;  /* padded width: mcus_x * h * 8 */
} plane;

/* IDCTs the block rows covering sample rows [r0, r1) of component `ci`. */
static int plane_build(const jr_coefs *c, int ci, int r0, int r1, plane *p) {
  const int h = c->info.h_samp[ci];
  const int bw = c->info.mcus_x * h;
  const int br0 = r0 / 8, br1 = (r1 + 7) / 8;
  int by, bx;
  p->stride = (size_t)bw * 8;
  p->row0 = br0 * 8;
  p->rows = (br1 - br0) * 8;
  p->pix = (uint8_t *)malloc(p->stride * (size_t)p->rows);
  if (!p->pix) return -1;
  for (by = br0; by < br1; by++)
    for (bx = 0; bx < bw; bx++) {
      int16_t blk[64];
      jr_coefs_output_block(c, jr_coefs_block((jr_coefs *)c, ci, by, bx), blk);
      idct_islow(blk, c->quant[ci], p->pix + (size_t)(by - br0) * 8 * p->stride + (size_t)bx * 8,
                 p->stride);
    }
  return 0;
}

/* Sample row `r` of a component, clamped to its real rows the way
   jdmainct.c's context pointers duplicate the first and last real row. */
static const uint8_t *plane_row(const jr_coefs *c, int ci, const plane *p, int r) {
  if (r < 0) r = 0;
  if (r >= c->ds_height[ci]) r = c->ds_height[ci] - 1;
  return p->pix + (size_t)(r - p->row0) * p->stride;
}

/* jdsample.c's upsamplers, writing one output row of `width` samples. */
static void upsample_row(const jr_coefs *c, int ci, const plane *p, int y, uint8_t *out,
                         int width) {
  const int hx = c->info.max_h_samp / c->info.h_samp[ci];
  const int vx = c->info.max_v_samp / c->info.v_samp[ci];
  const int dsw = c->ds_width[ci];
  int x;

  if (hx == 1 && vx == 1) {
    memcpy(out, plane_row(c, ci, p, y), (size_t)width);
    return;
  }

  if (hx == 2 && vx == 1) {
    const uint8_t *in = plane_row(c, ci, p, y);
    if (dsw > 2) {
      /* h2v1_fancy_upsample */
      int col, o = 0;
      int v = in[0];
#define PUT(val) do { if (o < width) out[o] = (uint8_t)(val); o++; } while (0)
      PUT(v);
      PUT((v * 3 + in[1] + 2) >> 2);
      for (col = 1; col < dsw - 1; col++) {
        v = in[col] * 3;
        PUT((v + in[col - 1] + 1) >> 2);
        PUT((v + in[col + 1] + 2) >> 2);
      }
      v = in[dsw - 1];
      PUT((v * 3 + in[dsw - 2] + 1) >> 2);
      PUT(v);
#undef PUT
    } else {
      for (x = 0; x < width; x++) out[x] = in[x / 2];
    }
    return;
  }

  if (hx == 1 && vx == 2) {
    /* h1v2_fancy_upsample */
    const int sr = y / 2;
    const uint8_t *in0 = plane_row(c, ci, p, sr);
    const uint8_t *in1 = plane_row(c, ci, p, (y & 1) ? sr + 1 : sr - 1);
    const int bias = (y & 1) ? 2 : 1;
    for (x = 0; x < width; x++) out[x] = (uint8_t)((in0[x] * 3 + in1[x] + bias) >> 2);
    return;
  }

  if (hx == 2 && vx == 2) {
    const int sr = y / 2;
    const uint8_t *in0 = plane_row(c, ci, p, sr);
    if (dsw > 2) {
      /* h2v2_fancy_upsample */
      const uint8_t *in1 = plane_row(c, ci, p, (y & 1) ? sr + 1 : sr - 1);
      int col, o = 0;
      int this_sum, last_sum, next_sum;
#define PUT(val) do { if (o < width) out[o] = (uint8_t)(val); o++; } while (0)
      this_sum = in0[0] * 3 + in1[0];
      next_sum = in0[1] * 3 + in1[1];
      PUT((this_sum * 4 + 8) >> 4);
      PUT((this_sum * 3 + next_sum + 7) >> 4);
      last_sum = this_sum;
      this_sum = next_sum;
      for (col = 2; col < dsw; col++) {
        next_sum = in0[col] * 3 + in1[col];
        PUT((this_sum * 3 + last_sum + 8) >> 4);
        PUT((this_sum * 3 + next_sum + 7) >> 4);
        last_sum = this_sum;
        this_sum = next_sum;
      }
      PUT((this_sum * 3 + last_sum + 8) >> 4);
      PUT((this_sum * 4 + 7) >> 4);
#undef PUT
    } else {
      for (x = 0; x < width; x++) out[x] = in0[x / 2];
    }
    return;
  }

  /* int_upsample: plain replication for any other integral ratio. */
  {
    const uint8_t *in = plane_row(c, ci, p, y / vx);
    for (x = 0; x < width; x++) out[x] = in[x / hx];
  }
}

/* Sample rows of component `ci` that output rows [y0, y1) read from. */
static void rows_needed(const jr_coefs *c, int ci, int y0, int y1, int *r0, int *r1) {
  const int vx = c->info.max_v_samp / c->info.v_samp[ci];
  int lo = y0 / vx, hi = (y1 - 1) / vx + 1;
  if (vx == 2) { /* fancy vertical filters read one row either side */
    lo -= 1;
    hi += 1;
  }
  if (lo < 0) lo = 0;
  if (hi > c->ds_height[ci]) hi = c->ds_height[ci];
  if (hi <= lo) hi = lo + 1;
  *r0 = lo;
  *r1 = hi;
}

void jr_coefs_render_rows(const jr_coefs *c, int mcu_row0, int mcu_row1, int ycbcr,
                          uint8_t *out, size_t stride) {
  plane planes[JR_MAX_COMPONENTS];
  uint8_t *rowbuf = NULL;
  const int W = c->info.width;
  int y0, y1, y, ci, x, nc;

  if (!c || !out || mcu_row0 >= mcu_row1) return;
  if (mcu_row0 < 0) mcu_row0 = 0;
  if (mcu_row1 > c->info.mcus_y) mcu_row1 = c->info.mcus_y;
  y0 = mcu_row0 * c->info.mcu_height;
  y1 = mcu_row1 * c->info.mcu_height;
  if (y1 > c->info.height) y1 = c->info.height;
  if (y0 >= y1) return;

  nc = c->info.num_components;
  if (nc > 3) nc = 3; /* CMYK and friends are refused before they get here */
  memset(planes, 0, sizeof(planes));
  for (ci = 0; ci < nc; ci++) {
    int r0, r1;
    rows_needed(c, ci, y0, y1, &r0, &r1);
    if (plane_build(c, ci, r0, r1, &planes[ci]) != 0) goto done;
  }
  rowbuf = (uint8_t *)malloc((size_t)W * 3 + 32);
  if (!rowbuf) goto done;

  for (y = y0; y < y1; y++) {
    uint8_t *dst = out + (size_t)y * stride;
    uint8_t *yrow = rowbuf, *cbrow = rowbuf + W + 8, *crrow = rowbuf + 2 * (W + 8);
    upsample_row(c, 0, &planes[0], y, yrow, W);
    if (nc == 1) {
      for (x = 0; x < W; x++) {
        dst[x * 3 + 0] = yrow[x];
        dst[x * 3 + 1] = ycbcr ? 128 : yrow[x];
        dst[x * 3 + 2] = ycbcr ? 128 : yrow[x];
      }
      continue;
    }
    upsample_row(c, 1, &planes[1], y, cbrow, W);
    upsample_row(c, 2, &planes[2], y, crrow, W);
    if (ycbcr || c->info.color_space == 2 /* JCS_RGB */) {
      for (x = 0; x < W; x++) {
        dst[x * 3 + 0] = yrow[x];
        dst[x * 3 + 1] = cbrow[x];
        dst[x * 3 + 2] = crrow[x];
      }
      continue;
    }
    for (x = 0; x < W; x++) {
      const int yy = yrow[x], cb = cbrow[x], cr = crrow[x];
      dst[x * 3 + 0] = clamp255(yy + c->cr_r[cr]);
      dst[x * 3 + 1] = clamp255(yy + (int)((c->cb_g[cb] + c->cr_g[cr]) >> 16));
      dst[x * 3 + 2] = clamp255(yy + c->cb_b[cb]);
    }
  }

done:
  free(rowbuf);
  for (ci = 0; ci < JR_MAX_COMPONENTS; ci++) free(planes[ci].pix);
}
