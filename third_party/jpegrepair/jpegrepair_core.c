/*
 * jpegrepair_core.c -- coefficient-domain JPEG repair, as a library.
 *
 * Derived from jpegrepair.c, Copyright (c) 2017, Don Mahurin.
 * https://github.com/openrightorg/jpegrepair  (BSD 3-Clause)
 * See LICENSE.jpegrepair for the original notice and disclaimer.
 *
 * The repair operations below are upstream's, with these changes:
 *   - block selection moved behind jr_scope, adding rect and mask scopes;
 *   - coefficients are held in memory, MCU-major, between edits (jr_coefs), so
 *     the operations work on whole MCUs and whole 8x8 blocks in coding order,
 *     and libjpeg's virtual arrays are only touched, row group by row group,
 *     when reading a file in and writing one out;
 *   - INSERT and DELETE shift whole MCUs in scan order. Upstream walked each
 *     component's block grid and wrapped at width_in_blocks, which differs from
 *     the coded width when the image is not a whole number of MCUs wide and
 *     slid the luma of the wrapped columns against its own chroma;
 *   - UNIT_INSERT and UNIT_DELETE were added, shifting single 8x8 blocks in
 *     the order the entropy coder writes them, for damage that loses part of
 *     an MCU;
 *   - the CDELTA case no longer walks all 64 coefficients to touch one;
 *   - failures unwind to the caller instead of calling exit();
 *   - jr_read_mcus and JR_OP_PASTE were added, giving callers a clipboard of
 *     whole MCUs. Upstream could only copy from a relative offset, which
 *     moves under any edit applied before it;
 *   - jr_quantize_patch was added, which turns pixels from anywhere into
 *     blocks in a given file's own quantization, so that content can come
 *     from a picture that was never a JPEG.
 */

#include "jpegrepair_core.h"

#include <setjmp.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <jpeglib.h>

#include "transupp.h"

/* ------------------------------------------------------------------ */
/* Error plumbing                                                      */
/* ------------------------------------------------------------------ */

struct jr_error_mgr {
  struct jpeg_error_mgr pub;
  jmp_buf setjmp_buffer;
  char message[JMSG_LENGTH_MAX];
};

static void jr_error_exit(j_common_ptr cinfo) {
  struct jr_error_mgr *e = (struct jr_error_mgr *)cinfo->err;
  (*cinfo->err->format_message)(cinfo, e->message);
  longjmp(e->setjmp_buffer, 1);
}

static void jr_emit_message(j_common_ptr cinfo, int msg_level) {
  (void)cinfo;
  (void)msg_level; /* stay quiet: warnings are expected on damaged files */
}

static void jr_set_err(char *err, size_t err_len, const char *fmt, ...) {
  va_list ap;
  if (!err || err_len == 0) return;
  va_start(ap, fmt);
  vsnprintf(err, err_len, fmt, ap);
  va_end(ap);
}

static void jr_init_err(struct jr_error_mgr *jerr) {
  jpeg_std_error(&jerr->pub);
  jerr->pub.error_exit = jr_error_exit;
  jerr->pub.emit_message = jr_emit_message;
  jerr->message[0] = '\0';
}

void jr_free(void *p) { free(p); }

/* ------------------------------------------------------------------ */
/* Geometry                                                            */
/* ------------------------------------------------------------------ */

static int ceil_div(int a, int b) { return b > 0 ? (a + b - 1) / b : 0; }

/* Counts SOS segments by walking the markers and skipping entropy-coded data.
   libjpeg only tells you about a scan once it has decoded up to it, and the
   callers that want this number want it before deciding whether to. */
static int count_scans(const uint8_t *d, size_t n) {
  size_t p = 2;
  int scans = 0;
  if (n < 4 || d[0] != 0xFF || d[1] != 0xD8) return 0;
  while (p + 3 < n) {
    uint8_t m;
    size_t len;
    if (d[p] != 0xFF) return scans;
    while (p < n && d[p] == 0xFF) p++;
    if (p >= n) break;
    m = d[p++];
    if (m == 0xD9) break;
    if (m == 0x01 || (m >= 0xD0 && m <= 0xD7)) continue;
    if (p + 1 >= n) break;
    len = ((size_t)d[p] << 8) | d[p + 1];
    if (len < 2 || p + len > n) break;
    p += len;
    if (m != 0xDA) continue;
    scans++;
    /* Entropy-coded data runs to the first FF that is not stuffing or RSTn. */
    while (p + 1 < n) {
      if (d[p] == 0xFF && d[p + 1] != 0x00 && !(d[p + 1] >= 0xD0 && d[p + 1] <= 0xD7) &&
          d[p + 1] != 0xFF)
        break;
      p++;
    }
  }
  return scans;
}

static void fill_info(const struct jpeg_decompress_struct *cinfo, jr_info *info) {
  int ci;
  memset(info, 0, sizeof(*info));
  info->width = (int)cinfo->image_width;
  info->height = (int)cinfo->image_height;
  info->num_components = cinfo->num_components;
  info->progressive = cinfo->progressive_mode;
  info->restart_interval = (int)cinfo->restart_interval;
  info->color_space = (int)cinfo->jpeg_color_space;
  info->data_precision = cinfo->data_precision;
  info->arith_code = cinfo->arith_code;

  info->max_h_samp = 1;
  info->max_v_samp = 1;
  for (ci = 0; ci < cinfo->num_components && ci < JR_MAX_COMPONENTS; ci++) {
    int h = cinfo->comp_info[ci].h_samp_factor;
    int v = cinfo->comp_info[ci].v_samp_factor;
    info->h_samp[ci] = h;
    info->v_samp[ci] = v;
    if (h > info->max_h_samp) info->max_h_samp = h;
    if (v > info->max_v_samp) info->max_v_samp = v;

    {
      int tbl = cinfo->comp_info[ci].quant_tbl_no;
      JQUANT_TBL *q = (tbl >= 0 && tbl < NUM_QUANT_TBLS) ? cinfo->quant_tbl_ptrs[tbl] : NULL;
      /* Entry 0 is the DC term in zigzag order as well as natural order. A
         missing or zero entry falls back to 1 so callers dividing by it
         cannot trap on a malformed DQT. */
      info->dc_quant[ci] = (q && q->quantval[0]) ? (int)q->quantval[0] : 1;
    }
  }

  info->mcu_width = 8 * info->max_h_samp;
  info->mcu_height = 8 * info->max_v_samp;
  info->mcus_x = ceil_div(info->width, info->mcu_width);
  info->mcus_y = ceil_div(info->height, info->mcu_height);

  info->blocks_per_mcu = 0;
  for (ci = 0; ci < info->num_components && ci < JR_MAX_COMPONENTS; ci++)
    info->blocks_per_mcu += info->h_samp[ci] * info->v_samp[ci];
}

/* Refuses headers this library cannot hold safely, before libjpeg allocates
   anything on their say-so. */
static int check_header(const struct jpeg_decompress_struct *cinfo, char *err, size_t err_len) {
  if (cinfo->num_components < 1 || cinfo->num_components > JR_MAX_COMPONENTS) {
    jr_set_err(err, err_len, "This file declares %d color components; at most %d are supported.",
               cinfo->num_components, JR_MAX_COMPONENTS);
    return -1;
  }
  if ((long long)cinfo->image_width * (long long)cinfo->image_height > JR_MAX_PIXELS) {
    jr_set_err(err, err_len,
               "The frame header claims %u x %u pixels, more than this tool will allocate. "
               "A header that size is usually damaged or borrowed from the wrong file.",
               cinfo->image_width, cinfo->image_height);
    return -1;
  }
  if (cinfo->data_precision != 8) {
    jr_set_err(err, err_len, "This file stores %d-bit samples; only 8-bit JPEGs are supported.",
               cinfo->data_precision);
    return -1;
  }
  return 0;
}

/* ------------------------------------------------------------------ */
/* Block selection                                                     */
/* ------------------------------------------------------------------ */

static int scope_covers(const jr_scope *s, int mcu_row, int mcu_col, int mcus_x) {
  switch (s->kind) {
    case JR_SCOPE_RECT:
      if (mcu_row < s->row) return 0;
      if (s->h > 0 && mcu_row >= s->row + s->h) return 0;
      if (mcu_col < s->col) return 0;
      if (s->w > 0 && mcu_col >= s->col + s->w) return 0;
      return 1;

    case JR_SCOPE_MASK:
      if (!s->mask) return 0;
      if (mcu_row < 0 || mcu_row >= s->mask_rows) return 0;
      if (mcu_col < 0 || mcu_col >= s->mask_cols) return 0;
      return s->mask[(size_t)mcu_row * (size_t)s->mask_cols + (size_t)mcu_col] != 0;

    case JR_SCOPE_RUN:
    default: {
      long idx = (long)mcu_row * mcus_x + mcu_col;
      long start = (long)s->row * mcus_x + s->col;
      if (idx < start) return 0;
      /* `w` <= 0 means "to the end of the image". A positive count is
         exclusive; upstream's BN was inclusive, an off-by-one not worth
         reproducing now that nothing parses a command line. */
      if (s->w > 0 && idx >= start + s->w) return 0;
      return 1;
    }
  }
}

/* ------------------------------------------------------------------ */
/* The operations, on any MCU-major store                              */
/* ------------------------------------------------------------------ */

/* The operations only ever move whole blocks, so the same code drives both
   the coefficients (128 bytes a block) and jr_trace's provenance map (4 bytes
   a block). */
typedef struct {
  uint8_t *base;
  size_t unit_size; /* bytes per block */
  size_t units;     /* total blocks */
  int bpm;          /* blocks per MCU */
  long mcus;
  int mcus_x;
} store;

static uint8_t *unit_at(const store *s, size_t u) { return s->base + u * s->unit_size; }
static uint8_t *mcu_at(const store *s, long m) {
  return s->base + (size_t)m * (size_t)s->bpm * s->unit_size;
}

static void copy_mcu(const store *s, long dst, long src) {
  memcpy(mcu_at(s, dst), mcu_at(s, src), (size_t)s->bpm * s->unit_size);
}

static int run_scope_to_end(const jr_scope *sc) {
  return sc->kind == JR_SCOPE_RUN && sc->w <= 0;
}

static long run_start(const jr_scope *sc, int mcus_x) {
  long start = (long)sc->row * mcus_x + sc->col;
  return start < 0 ? 0 : start;
}

/* INSERT: every covered MCU m takes what was at m - n, so the run from the
   origin slides n MCUs later and the n MCUs just before the origin are
   duplicated into the gap. Walked backwards so every source is read before
   anything writes over it. */
static void op_insert(const store *s, const jr_scope *sc, int n) {
  long m;
  if (n <= 0) return;
  if (run_scope_to_end(sc)) {
    long start = run_start(sc, s->mcus_x);
    long first = start > n ? start : n; /* first MCU with a source */
    if (first >= s->mcus) return;
    memmove(mcu_at(s, first), mcu_at(s, first - n),
            (size_t)(s->mcus - first) * (size_t)s->bpm * s->unit_size);
    return;
  }
  for (m = s->mcus - 1; m >= 0; m--) {
    if (!scope_covers(sc, (int)(m / s->mcus_x), (int)(m % s->mcus_x), s->mcus_x)) continue;
    if (m - n >= 0) copy_mcu(s, m, m - n);
  }
}

/* DELETE: every covered MCU m takes what was at m + n. Past the end there is
   nothing to take, so the last n MCUs keep what they had. */
static void op_delete(const store *s, const jr_scope *sc, int n) {
  long m;
  if (n <= 0) return;
  if (run_scope_to_end(sc)) {
    long start = run_start(sc, s->mcus_x);
    if (start + n >= s->mcus) return;
    memmove(mcu_at(s, start), mcu_at(s, start + n),
            (size_t)(s->mcus - start - n) * (size_t)s->bpm * s->unit_size);
    return;
  }
  for (m = 0; m < s->mcus; m++) {
    if (!scope_covers(sc, (int)(m / s->mcus_x), (int)(m % s->mcus_x), s->mcus_x)) continue;
    if (m + n < s->mcus) copy_mcu(s, m, m + n);
  }
}

/* The same two shifts, by single blocks in coding order: a stream that lost
   half an MCU's worth of data puts every later block that much out of step,
   luma against chroma, and no whole-MCU shift can put that back. */
static void op_unit_shift(const store *s, const jr_scope *sc, int n, int unit_offset, int insert) {
  long start_mcu = run_start(sc, s->mcus_x);
  size_t start, count;
  if (n <= 0 || unit_offset < 0 || unit_offset >= s->bpm) return;
  start = (size_t)start_mcu * (size_t)s->bpm + (size_t)unit_offset;
  if (start >= s->units) return;
  if (insert) {
    size_t first = start > (size_t)n ? start : (size_t)n;
    if (first >= s->units) return;
    count = s->units - first;
    memmove(unit_at(s, first), unit_at(s, first - (size_t)n), count * s->unit_size);
  } else {
    if (start + (size_t)n >= s->units) return;
    count = s->units - start - (size_t)n;
    memmove(unit_at(s, start), unit_at(s, start + (size_t)n), count * s->unit_size);
  }
}

/* COPY: each covered MCU takes the one (a, b) MCUs away. The walk runs
   backwards when the source lies behind in scan order, so that a source is
   always read before it is overwritten -- the order upstream's block walk
   used, kept so that saved repairs replay the same. */
static void op_copy(const store *s, const jr_scope *sc, int dr, int dc) {
  const int rows = (int)(s->mcus / s->mcus_x);
  const int reverse = (dr < 0 && dc <= 0) || (dc < 0 && dr <= 0);
  long i;
  for (i = 0; i < s->mcus; i++) {
    const long m = reverse ? s->mcus - 1 - i : i;
    const int r = (int)(m / s->mcus_x), c = (int)(m % s->mcus_x);
    const int sr = r + dr, scol = c + dc;
    if (!scope_covers(sc, r, c, s->mcus_x)) continue;
    if (sr < 0 || scol < 0 || sr >= rows || scol >= s->mcus_x) continue;
    copy_mcu(s, m, (long)sr * s->mcus_x + scol);
  }
}

/* PASTE: see jr_op. `write` copies payload MCU k to position m; jr_trace
   swaps it for one that marks the position as pasted. */
typedef void (*paste_fn)(const store *s, long m, const jr_op *op, size_t k, void *ctx);

static void op_paste(const store *s, const jr_op *op, paste_fn write, void *ctx) {
  const size_t per_mcu = (size_t)s->bpm * 64;
  long m;
  int k;
  if (!op->coefs || op->a <= 0 || per_mcu == 0) return;

  if (op->scope.kind == JR_SCOPE_RUN) {
    const long start = (long)op->scope.row * s->mcus_x + op->scope.col;
    if (start < 0) return;
    for (k = 0; k < op->a; k++) {
      m = start + k;
      if (m >= s->mcus) break; /* ran off the bottom of the image */
      if ((size_t)(k + 1) * per_mcu > op->coef_count) break;
      write(s, m, op, (size_t)k, ctx);
    }
    return;
  }

  for (m = 0, k = 0; m < s->mcus && k < op->a; m++) {
    if (!scope_covers(&op->scope, (int)(m / s->mcus_x), (int)(m % s->mcus_x), s->mcus_x))
      continue;
    if ((size_t)(k + 1) * per_mcu > op->coef_count) break;
    write(s, m, op, (size_t)k, ctx);
    k++;
  }
}

static int validate_op(const jr_info *info, const jr_op *op, char *err, size_t err_len) {
  switch (op->type) {
    case JR_OP_CDELTA:
      if (op->a < 0 || op->a >= info->num_components) {
        jr_set_err(err, err_len, "cdelta component %d is out of range (0..%d).", op->a,
                   info->num_components - 1);
        return -1;
      }
      return 0;
    case JR_OP_PASTE: {
      /* A short buffer means the clipboard and this image disagree about MCU
         shape -- pasting it would shear the components apart. */
      const size_t need = (size_t)(op->a > 0 ? op->a : 0) * (size_t)info->blocks_per_mcu * 64;
      if (!op->coefs || op->a <= 0) {
        jr_set_err(err, err_len, "Paste was given no blocks to write.");
        return -1;
      }
      if (op->coef_count < need) {
        jr_set_err(err, err_len, "Paste needs %lu coefficients for %d MCU(s) but was given %lu.",
                   (unsigned long)need, op->a, (unsigned long)op->coef_count);
        return -1;
      }
      return 0;
    }
    case JR_OP_UNIT_INSERT:
    case JR_OP_UNIT_DELETE:
      if (op->b < 0 || op->b >= info->blocks_per_mcu) {
        jr_set_err(err, err_len, "Block %d is outside an MCU of %d blocks.", op->b,
                   info->blocks_per_mcu);
        return -1;
      }
      return 0;
    case JR_OP_COPY:
    case JR_OP_INSERT:
    case JR_OP_DELETE:
      return 0;
  }
  jr_set_err(err, err_len, "Unknown repair operation %d.", (int)op->type);
  return -1;
}

static void run_geometric_op(const store *s, const jr_op *op) {
  switch (op->type) {
    case JR_OP_COPY: op_copy(s, &op->scope, op->a, op->b); break;
    case JR_OP_INSERT: op_insert(s, &op->scope, op->a); break;
    case JR_OP_DELETE: op_delete(s, &op->scope, op->a); break;
    case JR_OP_UNIT_INSERT: op_unit_shift(s, &op->scope, op->a, op->b, 1); break;
    case JR_OP_UNIT_DELETE: op_unit_shift(s, &op->scope, op->a, op->b, 0); break;
    default: break;
  }
}

/* ------------------------------------------------------------------ */
/* jr_coefs                                                            */
/* ------------------------------------------------------------------ */

static void build_color_tables(jr_coefs *c) {
  /* jdcolor.c's build_ycc_rgb_table, kept identical so jr_coefs_render_rows
     agrees with libjpeg sample for sample. */
  const int scalebits = 16;
  const int32_t one_half = (int32_t)1 << (scalebits - 1);
#define JR_FIX(x) ((int32_t)((x) * (1L << scalebits) + 0.5))
  int i, x;
  for (i = 0, x = -128; i <= 255; i++, x++) {
    c->cr_r[i] = (int)((JR_FIX(1.40200) * x + one_half) >> scalebits);
    c->cb_b[i] = (int)((JR_FIX(1.77200) * x + one_half) >> scalebits);
    c->cr_g[i] = (-JR_FIX(0.71414)) * x;
    c->cb_g[i] = (-JR_FIX(0.34414)) * x + one_half;
  }
#undef JR_FIX
}

static jr_coefs *coefs_alloc(const jr_info *info) {
  jr_coefs *c = (jr_coefs *)calloc(1, sizeof(jr_coefs));
  int ci, off = 0;
  if (!c) return NULL;
  c->info = *info;
  for (ci = 0; ci < info->num_components; ci++) {
    c->comp_offset[ci] = off;
    off += info->h_samp[ci] * info->v_samp[ci];
    c->ds_width[ci] = ceil_div(info->width * info->h_samp[ci], info->max_h_samp);
    c->ds_height[ci] = ceil_div(info->height * info->v_samp[ci], info->max_v_samp);
  }
  c->unit_count = (size_t)info->mcus_x * (size_t)info->mcus_y * (size_t)info->blocks_per_mcu;
  c->data = (int16_t *)calloc(c->unit_count ? c->unit_count * 64 : 1, sizeof(int16_t));
  if (!c->data) {
    free(c);
    return NULL;
  }
  build_color_tables(c);
  return c;
}

void jr_coefs_free(jr_coefs *c) {
  if (!c) return;
  free(c->data);
  free(c);
}

jr_coefs *jr_coefs_clone(const jr_coefs *src) {
  jr_coefs *c;
  if (!src) return NULL;
  c = (jr_coefs *)malloc(sizeof(jr_coefs));
  if (!c) return NULL;
  *c = *src;
  c->data = (int16_t *)malloc(src->unit_count ? src->unit_count * 64 * sizeof(int16_t) : 1);
  if (!c->data) {
    free(c);
    return NULL;
  }
  memcpy(c->data, src->data, src->unit_count * 64 * sizeof(int16_t));
  return c;
}

int jr_coefs_copy_into(jr_coefs *dst, const jr_coefs *src) {
  if (!dst || !src || dst->unit_count != src->unit_count) return -1;
  memcpy(dst->data, src->data, src->unit_count * 64 * sizeof(int16_t));
  return 0;
}

int16_t *jr_coefs_block(jr_coefs *c, int comp, int by, int bx) {
  int h, v;
  long mcu;
  if (!c || comp < 0 || comp >= c->info.num_components) return NULL;
  h = c->info.h_samp[comp];
  v = c->info.v_samp[comp];
  if (by < 0 || bx < 0 || by >= c->info.mcus_y * v || bx >= c->info.mcus_x * h) return NULL;
  mcu = (long)(by / v) * c->info.mcus_x + bx / h;
  return c->data + ((size_t)mcu * (size_t)c->info.blocks_per_mcu + (size_t)c->comp_offset[comp] +
                    (size_t)((by % v) * h + bx % h)) * 64;
}

int jr_coefs_load(const uint8_t *in, size_t in_len, jr_coefs **out, char *err, size_t err_len) {
  struct jpeg_decompress_struct cinfo;
  struct jr_error_mgr jerr;
  jvirt_barray_ptr *arrays;
  jr_info info;
  jr_coefs *volatile c = NULL;
  int ci;

  if (!in || in_len == 0 || !out) {
    jr_set_err(err, err_len, "No image data to read.");
    return -1;
  }
  *out = NULL;

  cinfo.err = &jerr.pub;
  jr_init_err(&jerr);
  if (setjmp(jerr.setjmp_buffer)) {
    jr_set_err(err, err_len, "libjpeg: %s", jerr.message);
    jpeg_destroy_decompress(&cinfo);
    jr_coefs_free(c);
    return -1;
  }

  jpeg_create_decompress(&cinfo);
  jpeg_mem_src(&cinfo, in, (unsigned long)in_len);
  jpeg_read_header(&cinfo, TRUE);
  if (check_header(&cinfo, err, err_len) != 0) {
    jpeg_destroy_decompress(&cinfo);
    return -1;
  }
  fill_info(&cinfo, &info);
  info.scan_count = count_scans(in, in_len);

  c = coefs_alloc(&info);
  if (!c) {
    jr_set_err(err, err_len, "Out of memory holding a %d x %d image.", info.width, info.height);
    jpeg_destroy_decompress(&cinfo);
    return -1;
  }

  arrays = jpeg_read_coefficients(&cinfo);

  /* The quantization tables each component was actually decoded with. */
  for (ci = 0; ci < info.num_components; ci++) {
    const JQUANT_TBL *q = cinfo.comp_info[ci].quant_table;
    int i;
    if (!q) {
      int tbl = cinfo.comp_info[ci].quant_tbl_no;
      q = (tbl >= 0 && tbl < NUM_QUANT_TBLS) ? cinfo.quant_tbl_ptrs[tbl] : NULL;
    }
    for (i = 0; i < 64; i++) c->quant[ci][i] = q ? q->quantval[i] : 1;
  }

  /* Row group by row group, the way the virtual array manager is meant to be
     used: it may keep the array anywhere, including on disk. */
  for (ci = 0; ci < info.num_components; ci++) {
    const int h = info.h_samp[ci], v = info.v_samp[ci];
    const int bw = info.mcus_x * h;
    int mrow;
    for (mrow = 0; mrow < info.mcus_y; mrow++) {
      JBLOCKARRAY rows = (cinfo.mem->access_virt_barray)(
          (j_common_ptr)&cinfo, arrays[ci], (JDIMENSION)(mrow * v), (JDIMENSION)v, FALSE);
      int yy, bx;
      for (yy = 0; yy < v; yy++) {
        for (bx = 0; bx < bw; bx++) {
          int16_t *dst = jr_coefs_block(c, ci, mrow * v + yy, bx);
          memcpy(dst, rows[yy][bx], 64 * sizeof(int16_t));
        }
      }
    }
  }

  jpeg_finish_decompress(&cinfo);
  jpeg_destroy_decompress(&cinfo);
  /* Once, here: every later edit stays in range by construction (CDELTA
     clamps, pastes come from libjpeg's own encoder or from these blocks). */
  jr_coefs_sanitize(c);
  *out = c;
  return 0;
}

static void paste_coefs(const store *s, long m, const jr_op *op, size_t k, void *ctx) {
  (void)ctx;
  memcpy(mcu_at(s, m), op->coefs + k * (size_t)s->bpm * 64, (size_t)s->bpm * 64 * sizeof(int16_t));
}

int jr_coefs_apply(jr_coefs *c, const jr_op *ops, size_t n_ops, char *err, size_t err_len) {
  store s;
  size_t i;
  if (!c) {
    jr_set_err(err, err_len, "No coefficients to edit.");
    return -1;
  }
  for (i = 0; i < n_ops; i++)
    if (validate_op(&c->info, &ops[i], err, err_len) != 0) return -1;

  s.base = (uint8_t *)c->data;
  s.unit_size = 64 * sizeof(int16_t);
  s.units = c->unit_count;
  s.bpm = c->info.blocks_per_mcu;
  s.mcus = (long)c->info.mcus_x * c->info.mcus_y;
  s.mcus_x = c->info.mcus_x;

  for (i = 0; i < n_ops; i++) {
    const jr_op *op = &ops[i];
    if (op->type == JR_OP_CDELTA) {
      const int comp = op->a;
      const int nb = c->info.h_samp[comp] * c->info.v_samp[comp];
      const int delta = op->b;
      long m;
      int k;
      for (m = 0; m < s.mcus; m++) {
        int16_t *blk;
        if (!scope_covers(&op->scope, (int)(m / s.mcus_x), (int)(m % s.mcus_x), s.mcus_x))
          continue;
        blk = c->data + ((size_t)m * (size_t)s.bpm + (size_t)c->comp_offset[comp]) * 64;
        for (k = 0; k < nb; k++) {
          /* Clamped to what the Huffman coder can write, as jr_coefs_sanitize
             would: see there. */
          int v = blk[(size_t)k * 64] + delta;
          blk[(size_t)k * 64] = (int16_t)(v < -1024 ? -1024 : (v > 1023 ? 1023 : v));
        }
      }
    } else if (op->type == JR_OP_PASTE) {
      op_paste(&s, op, paste_coefs, NULL);
    } else {
      run_geometric_op(&s, op);
    }
  }
  return 0;
}

long jr_coefs_sanitize(jr_coefs *c) {
  size_t u;
  long changed = 0;
  if (!c) return 0;
  for (u = 0; u < c->unit_count; u++) {
    int16_t *b = c->data + u * 64;
    int i, hit;
    /* Branch-free over the AC terms so the common, clean block costs a few
       vector instructions rather than 63 predictions. */
    int dc = b[0];
    int nd = dc < -1024 ? -1024 : (dc > 1023 ? 1023 : dc);
    hit = nd != dc;
    b[0] = (int16_t)nd;
    for (i = 1; i < 64; i++) {
      const int v = b[i];
      const int nv = v < -1023 ? -1023 : (v > 1023 ? 1023 : v);
      hit |= nv != v;
      b[i] = (int16_t)nv;
    }
    changed += hit;
  }
  return changed;
}

int jr_coefs_write(const jr_coefs *c, const uint8_t *src, size_t src_len,
                   uint8_t **out, size_t *out_len, char *err, size_t err_len) {
  struct jpeg_decompress_struct srcinfo;
  struct jpeg_compress_struct dstinfo;
  struct jr_error_mgr jerr;
  jvirt_barray_ptr arrays[JR_MAX_COMPONENTS];
  unsigned char *volatile outbuf = NULL;
  unsigned long outsize = 0;
  volatile int src_live = 0, dst_live = 0;
  int ci;

  if (!c || !src || src_len == 0 || !out || !out_len) {
    jr_set_err(err, err_len, "No image data to write.");
    return -1;
  }
  *out = NULL;
  *out_len = 0;

  srcinfo.err = &jerr.pub;
  dstinfo.err = &jerr.pub;
  jr_init_err(&jerr);
  if (setjmp(jerr.setjmp_buffer)) {
    jr_set_err(err, err_len, "libjpeg: %s", jerr.message);
    if (dst_live) jpeg_destroy_compress(&dstinfo);
    if (src_live) jpeg_destroy_decompress(&srcinfo);
    free(outbuf);
    return -1;
  }

  jpeg_create_decompress(&srcinfo);
  src_live = 1;
  jpeg_create_compress(&dstinfo);
  dst_live = 1;

  jpeg_mem_src(&srcinfo, src, (unsigned long)src_len);
  jcopy_markers_setup(&srcinfo, JCOPYOPT_ALL);
  jpeg_read_header(&srcinfo, TRUE);
  if ((int)srcinfo.image_width != c->info.width || (int)srcinfo.image_height != c->info.height ||
      srcinfo.num_components != c->info.num_components) {
    jr_set_err(err, err_len, "The header source does not describe these coefficients.");
    jpeg_destroy_compress(&dstinfo);
    jpeg_destroy_decompress(&srcinfo);
    return -1;
  }

  /* With the libjpeg 7+ API, jpeg_copy_critical_parameters takes the frame
     size from the output dimensions, which nothing has computed yet because
     the source's entropy-coded data is never read here. */
  jpeg_calc_output_dimensions(&srcinfo);
  jpeg_copy_critical_parameters(&srcinfo, &dstinfo);
  /* The tables the coefficients were quantized with are the ones they have to
     be written with, whatever slot the header put them in. */
  for (ci = 0; ci < c->info.num_components; ci++) {
    int i;
    if (!dstinfo.quant_tbl_ptrs[ci]) dstinfo.quant_tbl_ptrs[ci] = jpeg_alloc_quant_table((j_common_ptr)&dstinfo);
    for (i = 0; i < 64; i++) dstinfo.quant_tbl_ptrs[ci]->quantval[i] = c->quant[ci][i];
    dstinfo.quant_tbl_ptrs[ci]->sent_table = FALSE;
    dstinfo.comp_info[ci].quant_tbl_no = ci;
  }
  dstinfo.optimize_coding = TRUE;
#ifdef C_ARITH_CODING_SUPPORTED
  if (srcinfo.arith_code) {
    dstinfo.arith_code = TRUE;
    dstinfo.optimize_coding = FALSE;
  }
#endif

  for (ci = 0; ci < c->info.num_components; ci++) {
    arrays[ci] = (dstinfo.mem->request_virt_barray)(
        (j_common_ptr)&dstinfo, JPOOL_IMAGE, FALSE,
        (JDIMENSION)(c->info.mcus_x * c->info.h_samp[ci]),
        (JDIMENSION)(c->info.mcus_y * c->info.v_samp[ci]), (JDIMENSION)c->info.v_samp[ci]);
  }

  jpeg_mem_dest(&dstinfo, (unsigned char **)&outbuf, &outsize);
  jpeg_write_coefficients(&dstinfo, arrays); /* realizes the arrays */

  for (ci = 0; ci < c->info.num_components; ci++) {
    const int h = c->info.h_samp[ci], v = c->info.v_samp[ci];
    const int bw = c->info.mcus_x * h;
    int mrow;
    for (mrow = 0; mrow < c->info.mcus_y; mrow++) {
      JBLOCKARRAY rows = (dstinfo.mem->access_virt_barray)(
          (j_common_ptr)&dstinfo, arrays[ci], (JDIMENSION)(mrow * v), (JDIMENSION)v, TRUE);
      int yy, bx;
      for (yy = 0; yy < v; yy++)
        for (bx = 0; bx < bw; bx++)
          memcpy(rows[yy][bx], jr_coefs_block((jr_coefs *)c, ci, mrow * v + yy, bx),
                 64 * sizeof(int16_t));
    }
  }

  jcopy_markers_execute(&srcinfo, &dstinfo, JCOPYOPT_ALL);
  jpeg_finish_compress(&dstinfo);
  jpeg_destroy_compress(&dstinfo);
  dst_live = 0;
  jpeg_destroy_decompress(&srcinfo);
  src_live = 0;

  *out = outbuf;
  *out_len = (size_t)outsize;
  return 0;
}

int jr_coefs_read_mcus(const jr_coefs *c, int row, int col, int count,
                       int16_t **out, size_t *out_count, char *err, size_t err_len) {
  long start, total;
  size_t per_mcu;
  int16_t *flat;

  if (!c || !out || !out_count) {
    jr_set_err(err, err_len, "No image data to read.");
    return -1;
  }
  *out = NULL;
  *out_count = 0;
  if (count <= 0 || row < 0 || col < 0) {
    jr_set_err(err, err_len, "No MCUs to read.");
    return -1;
  }
  total = (long)c->info.mcus_x * c->info.mcus_y;
  start = (long)row * c->info.mcus_x + col;
  if (col >= c->info.mcus_x || start >= total) {
    jr_set_err(err, err_len, "MCU (%d, %d) is outside a %d x %d grid.", row, col,
               c->info.mcus_y, c->info.mcus_x);
    return -1;
  }
  /* Clamp rather than refuse: a selection running to the end of the image is
     the normal case, not a mistake. */
  if (start + count > total) count = (int)(total - start);

  per_mcu = (size_t)c->info.blocks_per_mcu * 64;
  flat = (int16_t *)malloc((size_t)count * per_mcu * sizeof(int16_t));
  if (!flat) {
    jr_set_err(err, err_len, "Out of memory reading %d MCU(s).", count);
    return -1;
  }
  memcpy(flat, c->data + (size_t)start * per_mcu, (size_t)count * per_mcu * sizeof(int16_t));
  *out = flat;
  *out_count = (size_t)count * per_mcu;
  return 0;
}

/* ------------------------------------------------------------------ */
/* Provenance                                                          */
/* ------------------------------------------------------------------ */

typedef struct {
  uint8_t *flags;
} trace_ctx;

static void paste_trace(const store *s, long m, const jr_op *op, size_t k, void *ctx) {
  int32_t *u = (int32_t *)mcu_at(s, m);
  int i;
  (void)op;
  (void)k;
  for (i = 0; i < s->bpm; i++) u[i] = -1;
  if (((trace_ctx *)ctx)->flags) ((trace_ctx *)ctx)->flags[m] |= JR_TRACE_PASTED;
}

int jr_trace(const jr_info *info, const jr_op *ops, size_t n_ops,
             int32_t *unit_src, uint8_t *mcu_flags, char *err, size_t err_len) {
  store s;
  size_t i, u;
  long m;
  trace_ctx ctx;
  int32_t *before = NULL;

  if (!info || !unit_src) {
    jr_set_err(err, err_len, "Nothing to trace.");
    return -1;
  }
  for (i = 0; i < n_ops; i++)
    if (validate_op(info, &ops[i], err, err_len) != 0) return -1;

  s.base = (uint8_t *)unit_src;
  s.unit_size = sizeof(int32_t);
  s.bpm = info->blocks_per_mcu;
  s.mcus = (long)info->mcus_x * info->mcus_y;
  s.mcus_x = info->mcus_x;
  s.units = (size_t)s.mcus * (size_t)s.bpm;
  for (u = 0; u < s.units; u++) unit_src[u] = (int32_t)u;
  if (mcu_flags) memset(mcu_flags, 0, (size_t)s.mcus);
  ctx.flags = mcu_flags;

  if (mcu_flags) {
    before = (int32_t *)malloc(s.units * sizeof(int32_t));
    if (!before) {
      jr_set_err(err, err_len, "Out of memory tracing the repair.");
      return -1;
    }
  }

  for (i = 0; i < n_ops; i++) {
    const jr_op *op = &ops[i];
    if (op->type == JR_OP_CDELTA) {
      if (!mcu_flags) continue;
      for (m = 0; m < s.mcus; m++)
        if (scope_covers(&op->scope, (int)(m / s.mcus_x), (int)(m % s.mcus_x), s.mcus_x))
          mcu_flags[m] |= JR_TRACE_DC;
      continue;
    }
    if (op->type == JR_OP_PASTE) {
      op_paste(&s, op, paste_trace, &ctx);
      continue;
    }
    if (before) memcpy(before, unit_src, s.units * sizeof(int32_t));
    run_geometric_op(&s, op);
    if (before) {
      /* The DC and paste flags belong to positions; a move carries the flags
         of what moved along with it, so recompute them from scratch is not
         possible here -- instead mark every MCU whose content changed. */
      for (m = 0; m < s.mcus; m++) {
        const size_t base = (size_t)m * (size_t)s.bpm;
        if (memcmp(before + base, unit_src + base, (size_t)s.bpm * sizeof(int32_t)) != 0)
          mcu_flags[m] |= JR_TRACE_MOVED;
      }
    }
  }
  free(before);
  return 0;
}

/* ------------------------------------------------------------------ */
/* One-shot entry points                                               */
/* ------------------------------------------------------------------ */

int jr_probe(const uint8_t *in, size_t in_len, jr_info *out_info,
             char *err, size_t err_len) {
  struct jpeg_decompress_struct cinfo;
  struct jr_error_mgr jerr;

  if (!in || in_len == 0 || !out_info) {
    jr_set_err(err, err_len, "No image data to probe.");
    return -1;
  }

  cinfo.err = &jerr.pub;
  jr_init_err(&jerr);
  if (setjmp(jerr.setjmp_buffer)) {
    jr_set_err(err, err_len, "libjpeg: %s", jerr.message);
    jpeg_destroy_decompress(&cinfo);
    return -1;
  }

  jpeg_create_decompress(&cinfo);
  jpeg_mem_src(&cinfo, in, (unsigned long)in_len);
  jpeg_read_header(&cinfo, TRUE);
  if (check_header(&cinfo, err, err_len) != 0) {
    jpeg_destroy_decompress(&cinfo);
    return -1;
  }

  fill_info(&cinfo, out_info);
  out_info->scan_count = count_scans(in, in_len);
  jpeg_destroy_decompress(&cinfo);
  return 0;
}

int jr_apply(const uint8_t *in, size_t in_len,
             const jr_op *ops, size_t n_ops,
             uint8_t **out, size_t *out_len,
             char *err, size_t err_len) {
  jr_coefs *c = NULL;
  int rc;
  if (!out || !out_len) {
    jr_set_err(err, err_len, "No image data to transform.");
    return -1;
  }
  *out = NULL;
  *out_len = 0;
  if (jr_coefs_load(in, in_len, &c, err, err_len) != 0) return -1;
  if (jr_coefs_apply(c, ops, n_ops, err, err_len) != 0) {
    jr_coefs_free(c);
    return -1;
  }
  rc = jr_coefs_write(c, in, in_len, out, out_len, err, err_len);
  jr_coefs_free(c);
  return rc;
}

int jr_read_mcus(const uint8_t *in, size_t in_len, int row, int col, int count,
                 int16_t **out, size_t *out_count, char *err, size_t err_len) {
  jr_coefs *c = NULL;
  int rc;
  if (!out || !out_count) {
    jr_set_err(err, err_len, "No image data to read.");
    return -1;
  }
  *out = NULL;
  *out_count = 0;
  if (count <= 0 || row < 0 || col < 0) {
    jr_set_err(err, err_len, "No MCUs to read.");
    return -1;
  }
  if (jr_coefs_load(in, in_len, &c, err, err_len) != 0) return -1;
  rc = jr_coefs_read_mcus(c, row, col, count, out, out_count, err, err_len);
  jr_coefs_free(c);
  return rc;
}

int jr_quantize_patch(const uint8_t *ref, size_t ref_len,
                      const uint8_t *rgb, int width, int height,
                      int16_t **out, size_t *out_count,
                      int *out_mcus_x, int *out_mcus_y,
                      char *err, size_t err_len) {
  struct jpeg_decompress_struct refinfo;
  struct jpeg_compress_struct dstinfo;
  struct jr_error_mgr jerr;
  unsigned char *volatile tmpbuf = NULL;
  unsigned long tmpsize = 0;
  jr_info info;
  jr_coefs *tmp = NULL;
  volatile int ref_live = 0, dst_live = 0;
  size_t n;

  if (!ref || ref_len == 0 || !rgb || width <= 0 || height <= 0 || !out || !out_count) {
    jr_set_err(err, err_len, "No patch to quantize.");
    return -1;
  }
  *out = NULL;
  *out_count = 0;

  refinfo.err = &jerr.pub;
  dstinfo.err = &jerr.pub;
  jr_init_err(&jerr);
  if (setjmp(jerr.setjmp_buffer)) {
    jr_set_err(err, err_len, "libjpeg: %s", jerr.message);
    if (dst_live) jpeg_destroy_compress(&dstinfo);
    if (ref_live) jpeg_destroy_decompress(&refinfo);
    free(tmpbuf);
    return -1;
  }

  jpeg_create_decompress(&refinfo);
  ref_live = 1;
  jpeg_mem_src(&refinfo, ref, (unsigned long)ref_len);
  jpeg_read_header(&refinfo, TRUE);
  fill_info(&refinfo, &info);

  /* Everything the patch has to agree with is decided here, before any work:
     it is written in the destination's own currency or not at all. */
  if (refinfo.num_components != 3 || refinfo.jpeg_color_space != JCS_YCbCr) {
    jr_set_err(err, err_len,
               "This file does not store the usual three YCbCr components, so "
               "pixels cannot be quantized into it.");
    jpeg_destroy_decompress(&refinfo);
    return -1;
  }
  if (refinfo.data_precision != BITS_IN_JSAMPLE) {
    jr_set_err(err, err_len, "This file stores %d-bit samples; this build handles %d.",
               refinfo.data_precision, BITS_IN_JSAMPLE);
    jpeg_destroy_decompress(&refinfo);
    return -1;
  }
  if (info.mcu_width <= 0 || info.mcu_height <= 0 ||
      width % info.mcu_width != 0 || height % info.mcu_height != 0) {
    jr_set_err(err, err_len,
               "A patch has to measure whole MCUs: %dx%d does not divide by %dx%d.",
               width, height, info.mcu_width, info.mcu_height);
    jpeg_destroy_decompress(&refinfo);
    return -1;
  }

  /* jpeg_copy_critical_parameters brings across the quantization tables, the
     sampling factors and the color space in one go -- which is the whole
     point: the patch has to be quantized by the tables the destination
     already carries, or its blocks would mean something else once written
     there. Only the dimensions and the input format are ours. */
  jpeg_create_compress(&dstinfo);
  dst_live = 1;
  jpeg_copy_critical_parameters(&refinfo, &dstinfo);
  dstinfo.image_width = (JDIMENSION)width;
  dstinfo.image_height = (JDIMENSION)height;
  dstinfo.input_components = 3;
  dstinfo.in_color_space = JCS_RGB;
  /* The entropy coding is thrown away below; asking for optimized tables only
     makes sure every coefficient this patch produces has a code at all. */
  dstinfo.optimize_coding = TRUE;
  dstinfo.arith_code = FALSE;

  jpeg_mem_dest(&dstinfo, (unsigned char **)&tmpbuf, &tmpsize);
  jpeg_start_compress(&dstinfo, TRUE);
  while (dstinfo.next_scanline < dstinfo.image_height) {
    JSAMPROW row = (JSAMPROW)(rgb + (size_t)dstinfo.next_scanline * (size_t)width * 3);
    jpeg_write_scanlines(&dstinfo, &row, 1);
  }
  jpeg_finish_compress(&dstinfo);
  jpeg_destroy_compress(&dstinfo);
  dst_live = 0;
  jpeg_destroy_decompress(&refinfo);
  ref_live = 0;

  /* Read the quantized blocks straight back out. libjpeg keeps them nowhere a
     caller can reach during compression, and a coefficient-only decode of a
     patch-sized JPEG costs nothing next to the encode that just happened. */
  if (jr_coefs_load(tmpbuf, tmpsize, &tmp, err, err_len) != 0) {
    free(tmpbuf);
    return -1;
  }
  free(tmpbuf);

  n = tmp->unit_count * 64;
  *out = (int16_t *)malloc(n * sizeof(int16_t));
  if (!*out) {
    jr_set_err(err, err_len, "Out of memory quantizing a %dx%d patch.", width, height);
    jr_coefs_free(tmp);
    return -1;
  }
  memcpy(*out, tmp->data, n * sizeof(int16_t));
  *out_count = n;
  if (out_mcus_x) *out_mcus_x = tmp->info.mcus_x;
  if (out_mcus_y) *out_mcus_y = tmp->info.mcus_y;
  jr_coefs_free(tmp);
  return 0;
}

int jr_decode(const uint8_t *in, size_t in_len, int ycbcr,
              uint8_t **pixels, int *width, int *height,
              char *err, size_t err_len) {
  struct jpeg_decompress_struct cinfo;
  struct jr_error_mgr jerr;
  uint8_t *volatile buffer = NULL;
  JSAMPROW volatile row = NULL;
  int gray = 0;
  size_t stride = 0;

  if (!in || in_len == 0 || !pixels) {
    jr_set_err(err, err_len, "No image data to decode.");
    return -1;
  }
  *pixels = NULL;

  cinfo.err = &jerr.pub;
  jr_init_err(&jerr);
  if (setjmp(jerr.setjmp_buffer)) {
    jr_set_err(err, err_len, "libjpeg: %s", jerr.message);
    jpeg_destroy_decompress(&cinfo);
    free(buffer);
    free(row);
    return -1;
  }

  jpeg_create_decompress(&cinfo);
  jpeg_mem_src(&cinfo, in, (unsigned long)in_len);
  jpeg_read_header(&cinfo, TRUE);
  if ((long long)cinfo.image_width * (long long)cinfo.image_height > JR_MAX_PIXELS) {
    jr_set_err(err, err_len, "The frame header claims %u x %u pixels, more than this tool will "
               "allocate.", cinfo.image_width, cinfo.image_height);
    jpeg_destroy_decompress(&cinfo);
    return -1;
  }

  gray = (cinfo.num_components == 1);
  /* JCS_YCbCr skips color conversion entirely, so samples come back as the
     JPEG stored them -- no clipping to the RGB gamut. */
  cinfo.out_color_space = gray ? JCS_GRAYSCALE : (ycbcr ? JCS_YCbCr : JCS_RGB);

  jpeg_start_decompress(&cinfo);

  stride = (size_t)cinfo.output_width * (size_t)cinfo.output_components;
  buffer = (uint8_t *)malloc((size_t)cinfo.output_height * (size_t)cinfo.output_width * 3);
  row = (JSAMPROW)malloc(stride);
  if (!buffer || !row) {
    jr_set_err(err, err_len, "Out of memory decoding a %ux%u image.",
               cinfo.output_width, cinfo.output_height);
    free(buffer);
    free(row);
    jpeg_abort_decompress(&cinfo);
    jpeg_destroy_decompress(&cinfo);
    return -1;
  }

  while (cinfo.output_scanline < cinfo.output_height) {
    unsigned int y = cinfo.output_scanline;
    uint8_t *dst = buffer + (size_t)y * (size_t)cinfo.output_width * 3;
    JSAMPROW r = row;
    jpeg_read_scanlines(&cinfo, &r, 1);
    if (gray) {
      /* Widen to 3 channels so every caller sees the same layout: a gray
         pixel is (Y, 128, 128) in YCbCr and (v, v, v) in RGB. */
      unsigned int x;
      for (x = 0; x < cinfo.output_width; x++) {
        dst[x * 3 + 0] = row[x];
        dst[x * 3 + 1] = ycbcr ? 128 : row[x];
        dst[x * 3 + 2] = ycbcr ? 128 : row[x];
      }
    } else {
      memcpy(dst, row, stride);
    }
  }
  free(row);

  if (width) *width = (int)cinfo.output_width;
  if (height) *height = (int)cinfo.output_height;

  jpeg_finish_decompress(&cinfo);
  jpeg_destroy_decompress(&cinfo);

  *pixels = buffer;
  return 0;
}

int jr_encode_rgb(const uint8_t *pixels, int width, int height, int quality,
                  const uint8_t *marker_src, size_t marker_src_len,
                  uint8_t **out, size_t *out_len,
                  char *err, size_t err_len) {
  struct jpeg_compress_struct cinfo;
  struct jpeg_decompress_struct srcinfo;
  struct jr_error_mgr jerr;
  unsigned char *volatile outbuf = NULL;
  unsigned long outsize = 0;
  int copy_markers = (marker_src != NULL && marker_src_len > 0);
  volatile int src_live = 0, dst_live = 0;

  if (!pixels || width <= 0 || height <= 0 || !out || !out_len) {
    jr_set_err(err, err_len, "No pixels to encode.");
    return -1;
  }
  *out = NULL;
  *out_len = 0;

  cinfo.err = &jerr.pub;
  srcinfo.err = &jerr.pub;
  jr_init_err(&jerr);
  if (setjmp(jerr.setjmp_buffer)) {
    jr_set_err(err, err_len, "libjpeg: %s", jerr.message);
    if (dst_live) jpeg_destroy_compress(&cinfo);
    if (src_live) jpeg_destroy_decompress(&srcinfo);
    free(outbuf);
    return -1;
  }

  /* The markers have to be read before the destination starts writing and
     stay readable until they are copied, so the source outlives
     jpeg_start_compress. */
  if (copy_markers) {
    jpeg_create_decompress(&srcinfo);
    src_live = 1;
    jpeg_mem_src(&srcinfo, marker_src, (unsigned long)marker_src_len);
    jcopy_markers_setup(&srcinfo, JCOPYOPT_ALL);
    jpeg_read_header(&srcinfo, TRUE);
  }

  jpeg_create_compress(&cinfo);
  dst_live = 1;
  jpeg_mem_dest(&cinfo, (unsigned char **)&outbuf, &outsize);
  cinfo.image_width = (JDIMENSION)width;
  cinfo.image_height = (JDIMENSION)height;
  cinfo.input_components = 3;
  cinfo.in_color_space = JCS_RGB;
  jpeg_set_defaults(&cinfo);
  jpeg_set_quality(&cinfo, quality, TRUE);
  jpeg_start_compress(&cinfo, TRUE);

  if (copy_markers) {
    jcopy_markers_execute(&srcinfo, &cinfo, JCOPYOPT_ALL);
    jpeg_destroy_decompress(&srcinfo);
    src_live = 0;
  }

  while (cinfo.next_scanline < cinfo.image_height) {
    JSAMPROW row = (JSAMPROW)(pixels + (size_t)cinfo.next_scanline * (size_t)width * 3);
    jpeg_write_scanlines(&cinfo, &row, 1);
  }

  jpeg_finish_compress(&cinfo);
  jpeg_destroy_compress(&cinfo);
  dst_live = 0;

  *out = outbuf;
  *out_len = (size_t)outsize;
  return 0;
}
