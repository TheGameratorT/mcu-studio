/*
 * jpegrepair_core.c -- coefficient-domain JPEG repair, as a library.
 *
 * Derived from jpegrepair.c, Copyright (c) 2017, Don Mahurin.
 * https://github.com/openrightorg/jpegrepair  (BSD 3-Clause)
 * See LICENSE.jpegrepair for the original notice and disclaimer.
 *
 * The transform loop below is upstream's, with these changes:
 *   - block selection moved behind jr_scope, adding rect and mask scopes;
 *   - the virtual-array accessor is passed the decompress object rather
 *     than its address (upstream passed `&srcinfo` where `srcinfo` was
 *     already a pointer, so libjpeg was handed a j_common_ptr aimed at a
 *     pointer variable -- it only survived because the error path it would
 *     have dereferenced was never taken);
 *   - the CDELTA case no longer walks all 64 coefficients to touch one;
 *   - INSERT and DELETE wrap rows at the MCU-aligned block count rather than
 *     width_in_blocks, which upstream used. The two differ when the image is
 *     not a whole number of MCUs wide, and the mismatch shifted the luma of
 *     the wrapped columns one block against its own chroma on every row;
 *   - failures unwind to the caller instead of calling exit();
 *   - jr_read_mcus and JR_OP_PASTE were added, giving callers a clipboard of
 *     whole MCUs. Upstream could only copy from a relative offset, which
 *     moves under any edit applied before it.
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

void jr_free(void *p) { free(p); }

/* ------------------------------------------------------------------ */
/* Geometry                                                            */
/* ------------------------------------------------------------------ */

static int ceil_div(int a, int b) { return b > 0 ? (a + b - 1) / b : 0; }

static void fill_info(const struct jpeg_decompress_struct *cinfo, jr_info *info) {
  int ci;
  memset(info, 0, sizeof(*info));
  info->width = (int)cinfo->image_width;
  info->height = (int)cinfo->image_height;
  info->num_components = cinfo->num_components;
  info->progressive = cinfo->progressive_mode;

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
/* The transform                                                       */
/* ------------------------------------------------------------------ */

static void transform(struct jpeg_decompress_struct *srcinfo,
                      jvirt_barray_ptr *coef_arrays,
                      const jr_info *info,
                      const jr_op *op) {
  int ci, block_y, block_x, by, bx, i;
  int nx, ny;
  JBLOCKARRAY coef_buffer;
  int reverse_order;

  /* INSERT walks backward so a block is read before the write that would
     overwrite its source; COPY needs the same when it pulls from ahead of
     itself in scan order. */
  reverse_order = (op->type == JR_OP_INSERT ||
                   (op->type == JR_OP_COPY &&
                    ((op->a < 0 && op->b <= 0) || (op->b < 0 && op->a <= 0))))
                      ? 1
                      : 0;

  for (ci = 0; ci < srcinfo->num_components; ci++) {
    int h_samp_factor, v_samp_factor, row_blocks, col_blocks;

    if (op->type == JR_OP_CDELTA && ci != op->a) continue;

    coef_buffer = (srcinfo->mem->access_virt_barray)(
        (j_common_ptr)srcinfo, coef_arrays[ci], 0,
        srcinfo->comp_info[ci].v_samp_factor, TRUE);

    h_samp_factor = srcinfo->comp_info[ci].h_samp_factor;
    v_samp_factor = srcinfo->comp_info[ci].v_samp_factor;
    /* Walk the block grid the entropy coder actually wrote, which is whole
       MCUs: mcus_x * h_samp_factor blocks per row. That is width_in_blocks
       rounded up, and the two differ whenever the image is not a whole
       number of MCUs wide -- 1701px at 2x2 sampling gives 213 luma blocks
       across but 214 coded ones. The extra blocks are edge padding, yet the
       stream still carries them, so a shift has to travel through them like
       any other. Wrapping a row at width_in_blocks instead would land one
       block early on every wrap and slide the luma of the wrapped columns
       sideways against its own chroma. libjpeg sizes the coefficient arrays
       to these same rounded-up extents, so the padding is there to index. */
    row_blocks = info->mcus_x * h_samp_factor;
    col_blocks = info->mcus_y * v_samp_factor;

    for (block_y = 0; block_y < col_blocks; block_y++) {
      by = reverse_order ? (col_blocks - block_y - 1) : block_y;
      for (block_x = 0; block_x < row_blocks; block_x++) {
        bx = reverse_order ? (row_blocks - block_x - 1) : block_x;

        if (!scope_covers(&op->scope, by / v_samp_factor, bx / h_samp_factor,
                          info->mcus_x))
          continue;

        if (op->type == JR_OP_CDELTA) {
          coef_buffer[by][bx][0] += (JCOEF)op->b;
          continue;
        }

        for (i = 0; i < DCTSIZE2; i++) {
          if (op->type == JR_OP_DELETE) {
            nx = bx + op->a * h_samp_factor;
            ny = by + (nx / row_blocks) * v_samp_factor;
            nx = nx % row_blocks;
            if (ny < col_blocks) coef_buffer[by][bx][i] = coef_buffer[ny][nx][i];
          } else if (op->type == JR_OP_INSERT) {
            nx = op->a * h_samp_factor + row_blocks - 1 - bx;
            ny = by - (nx / row_blocks) * v_samp_factor;
            nx = row_blocks - (nx % row_blocks) - 1;
            if (ny >= 0) coef_buffer[by][bx][i] = coef_buffer[ny][nx][i];
          } else if (op->type == JR_OP_COPY) {
            ny = by + v_samp_factor * op->a;
            nx = bx + h_samp_factor * op->b;
            if (ny >= 0 && nx >= 0 && ny < col_blocks && nx < row_blocks)
              coef_buffer[by][bx][i] = coef_buffer[ny][nx][i];
          }
        }
      }
    }
  }
}

/* ------------------------------------------------------------------ */
/* Whole-MCU access, for the clipboard                                 */
/* ------------------------------------------------------------------ */

/* Hands back one JBLOCKARRAY per component, indexable over the component's
   whole padded height.

   The request asks for only v_samp_factor rows because that is the maxaccess
   jpeg_read_coefficients registered, and asking for more trips libjpeg's
   "bogus virtual array access" check. Indexing past them is still sound: the
   coefficient arrays are realized whole in memory, so the returned pointer
   covers every row. That is the same assumption the transform loop above
   makes, and it holds as long as libjpeg has no backing store to swap to --
   true for the jmemnobs manager that libjpeg-turbo builds by default. */
static void access_all(struct jpeg_decompress_struct *cinfo,
                       jvirt_barray_ptr *coef_arrays, const jr_info *info,
                       JBLOCKARRAY *out, boolean writable) {
  int ci;
  (void)info;
  for (ci = 0; ci < cinfo->num_components; ci++)
    out[ci] = (cinfo->mem->access_virt_barray)(
        (j_common_ptr)cinfo, coef_arrays[ci], 0,
        (JDIMENSION)cinfo->comp_info[ci].v_samp_factor, writable);
}

/* One MCU's blocks in the order the entropy coder writes them: component by
   component, and within a component its v_samp*h_samp blocks in raster
   order. `flat` holds blocks_per_mcu * 64 coefficients. */
static void mcu_read(JBLOCKARRAY *buf, const struct jpeg_decompress_struct *cinfo,
                     int mcu_row, int mcu_col, JCOEF *flat) {
  int ci, by, bx, i;
  for (ci = 0; ci < cinfo->num_components; ci++) {
    const int h = cinfo->comp_info[ci].h_samp_factor;
    const int v = cinfo->comp_info[ci].v_samp_factor;
    for (by = 0; by < v; by++)
      for (bx = 0; bx < h; bx++) {
        JCOEFPTR block = buf[ci][mcu_row * v + by][mcu_col * h + bx];
        for (i = 0; i < DCTSIZE2; i++) *flat++ = block[i];
      }
  }
}

static void mcu_write(JBLOCKARRAY *buf, const struct jpeg_decompress_struct *cinfo,
                      int mcu_row, int mcu_col, const JCOEF *flat) {
  int ci, by, bx, i;
  for (ci = 0; ci < cinfo->num_components; ci++) {
    const int h = cinfo->comp_info[ci].h_samp_factor;
    const int v = cinfo->comp_info[ci].v_samp_factor;
    for (by = 0; by < v; by++)
      for (bx = 0; bx < h; bx++) {
        JCOEFPTR block = buf[ci][mcu_row * v + by][mcu_col * h + bx];
        for (i = 0; i < DCTSIZE2; i++) block[i] = *flat++;
      }
  }
}

/* A RUN scope walks scan order from the scope origin, so a run that started
   mid-row comes back down the same way it was read, wrapping at the image
   edge. A RECT or MASK scope walks the whole grid and spends one payload MCU
   on each MCU it covers, in scan order -- the shape of the selection decides
   where the blocks land, and the payload only has to have been assembled in
   that same order. */
static void transform_paste(struct jpeg_decompress_struct *srcinfo,
                            jvirt_barray_ptr *coef_arrays,
                            const jr_info *info, const jr_op *op) {
  JBLOCKARRAY buf[JR_MAX_COMPONENTS];
  const long total = (long)info->mcus_x * info->mcus_y;
  const size_t per_mcu = (size_t)info->blocks_per_mcu * DCTSIZE2;
  long idx;
  int k;

  if (!op->coefs || op->a <= 0 || per_mcu == 0) return;

  access_all(srcinfo, coef_arrays, info, buf, TRUE);

  if (op->scope.kind == JR_SCOPE_RUN) {
    const long start = (long)op->scope.row * info->mcus_x + op->scope.col;
    if (start < 0) return;
    for (k = 0; k < op->a; k++) {
      idx = start + k;
      if (idx >= total) break; /* ran off the bottom of the image */
      if ((size_t)(k + 1) * per_mcu > op->coef_count) break;
      mcu_write(buf, srcinfo, (int)(idx / info->mcus_x), (int)(idx % info->mcus_x),
                (const JCOEF *)op->coefs + (size_t)k * per_mcu);
    }
    return;
  }

  for (idx = 0, k = 0; idx < total && k < op->a; idx++) {
    const int mcu_row = (int)(idx / info->mcus_x);
    const int mcu_col = (int)(idx % info->mcus_x);
    if (!scope_covers(&op->scope, mcu_row, mcu_col, info->mcus_x)) continue;
    if ((size_t)(k + 1) * per_mcu > op->coef_count) break;
    mcu_write(buf, srcinfo, mcu_row, mcu_col,
              (const JCOEF *)op->coefs + (size_t)k * per_mcu);
    k++;
  }
}

/* ------------------------------------------------------------------ */
/* Public entry points                                                 */
/* ------------------------------------------------------------------ */

int jr_probe(const uint8_t *in, size_t in_len, jr_info *out_info,
             char *err, size_t err_len) {
  struct jpeg_decompress_struct cinfo;
  struct jr_error_mgr jerr;

  if (!in || in_len == 0 || !out_info) {
    jr_set_err(err, err_len, "No image data to probe.");
    return -1;
  }

  cinfo.err = jpeg_std_error(&jerr.pub);
  jerr.pub.error_exit = jr_error_exit;
  jerr.pub.emit_message = jr_emit_message;
  jerr.message[0] = '\0';

  if (setjmp(jerr.setjmp_buffer)) {
    jr_set_err(err, err_len, "libjpeg: %s", jerr.message);
    jpeg_destroy_decompress(&cinfo);
    return -1;
  }

  jpeg_create_decompress(&cinfo);
  jpeg_mem_src(&cinfo, in, (unsigned long)in_len);
  jpeg_read_header(&cinfo, TRUE);

  fill_info(&cinfo, out_info);
  jpeg_destroy_decompress(&cinfo);
  return 0;
}

int jr_apply(const uint8_t *in, size_t in_len,
             const jr_op *ops, size_t n_ops,
             uint8_t **out, size_t *out_len,
             char *err, size_t err_len) {
  struct jpeg_decompress_struct srcinfo;
  struct jpeg_compress_struct dstinfo;
  struct jr_error_mgr jerr;
  jvirt_barray_ptr *coef_arrays;
  unsigned char *outbuf = NULL;
  unsigned long outsize = 0;
  jr_info info;
  size_t i;
  int src_live = 0, dst_live = 0;

  if (!in || in_len == 0 || !out || !out_len) {
    jr_set_err(err, err_len, "No image data to transform.");
    return -1;
  }
  *out = NULL;
  *out_len = 0;

  /* One error manager for both objects, as upstream did -- either side
     longjmps to the same recovery point. */
  srcinfo.err = jpeg_std_error(&jerr.pub);
  dstinfo.err = &jerr.pub;
  jerr.pub.error_exit = jr_error_exit;
  jerr.pub.emit_message = jr_emit_message;
  jerr.message[0] = '\0';

  if (setjmp(jerr.setjmp_buffer)) {
    jr_set_err(err, err_len, "libjpeg: %s", jerr.message);
    if (dst_live) jpeg_destroy_compress(&dstinfo);
    if (src_live) jpeg_destroy_decompress(&srcinfo);
    if (outbuf) free(outbuf);
    return -1;
  }

  jpeg_create_decompress(&srcinfo);
  src_live = 1;
  jpeg_create_compress(&dstinfo);
  dst_live = 1;

  jpeg_mem_src(&srcinfo, in, (unsigned long)in_len);
  jcopy_markers_setup(&srcinfo, JCOPYOPT_ALL);
  jpeg_read_header(&srcinfo, TRUE);

  fill_info(&srcinfo, &info);

  coef_arrays = jpeg_read_coefficients(&srcinfo);
  jpeg_copy_critical_parameters(&srcinfo, &dstinfo);

  for (i = 0; i < n_ops; i++) {
    const jr_op *op = &ops[i];
    if (op->type == JR_OP_CDELTA &&
        (op->a < 0 || op->a >= srcinfo.num_components)) {
      jr_set_err(err, err_len, "cdelta component %d is out of range (0..%d).",
                 op->a, srcinfo.num_components - 1);
      jpeg_destroy_compress(&dstinfo);
      jpeg_destroy_decompress(&srcinfo);
      return -1;
    }
    if (op->type == JR_OP_PASTE) {
      /* A short buffer means the clipboard and this image disagree about MCU
         shape -- pasting it would shear the components apart. */
      const size_t need =
          (size_t)(op->a > 0 ? op->a : 0) * info.blocks_per_mcu * DCTSIZE2;
      if (!op->coefs || op->a <= 0 || op->coef_count < need) {
        if (!op->coefs || op->a <= 0)
          jr_set_err(err, err_len, "Paste was given no blocks to write.");
        else
          jr_set_err(err, err_len,
                     "Paste needs %lu coefficients for %d MCU(s) but was given %lu.",
                     (unsigned long)need, op->a, (unsigned long)op->coef_count);
        jpeg_destroy_compress(&dstinfo);
        jpeg_destroy_decompress(&srcinfo);
        return -1;
      }
      transform_paste(&srcinfo, coef_arrays, &info, op);
      continue;
    }
    transform(&srcinfo, coef_arrays, &info, op);
  }

  jpeg_mem_dest(&dstinfo, &outbuf, &outsize);
  jpeg_write_coefficients(&dstinfo, coef_arrays);
  jcopy_markers_execute(&srcinfo, &dstinfo, JCOPYOPT_ALL);
  jpeg_finish_compress(&dstinfo);
  jpeg_destroy_compress(&dstinfo);
  dst_live = 0;
  jpeg_finish_decompress(&srcinfo);
  jpeg_destroy_decompress(&srcinfo);
  src_live = 0;

  *out = outbuf;
  *out_len = (size_t)outsize;
  return 0;
}

int jr_read_mcus(const uint8_t *in, size_t in_len, int row, int col, int count,
                 int16_t **out, size_t *out_count, char *err, size_t err_len) {
  struct jpeg_decompress_struct cinfo;
  struct jr_error_mgr jerr;
  jvirt_barray_ptr *coef_arrays;
  JBLOCKARRAY buf[JR_MAX_COMPONENTS];
  JCOEF *flat = NULL;
  jr_info info;
  long start, total;
  size_t per_mcu;
  int k;

  if (!in || in_len == 0 || !out || !out_count) {
    jr_set_err(err, err_len, "No image data to read.");
    return -1;
  }
  *out = NULL;
  *out_count = 0;
  if (count <= 0 || row < 0 || col < 0) {
    jr_set_err(err, err_len, "No MCUs to read.");
    return -1;
  }

  cinfo.err = jpeg_std_error(&jerr.pub);
  jerr.pub.error_exit = jr_error_exit;
  jerr.pub.emit_message = jr_emit_message;
  jerr.message[0] = '\0';

  if (setjmp(jerr.setjmp_buffer)) {
    jr_set_err(err, err_len, "libjpeg: %s", jerr.message);
    jpeg_destroy_decompress(&cinfo);
    if (flat) free(flat);
    return -1;
  }

  jpeg_create_decompress(&cinfo);
  jpeg_mem_src(&cinfo, in, (unsigned long)in_len);
  jpeg_read_header(&cinfo, TRUE);
  fill_info(&cinfo, &info);

  total = (long)info.mcus_x * info.mcus_y;
  start = (long)row * info.mcus_x + col;
  if (col >= info.mcus_x || start >= total) {
    jr_set_err(err, err_len, "MCU (%d, %d) is outside a %d x %d grid.", row, col,
               info.mcus_y, info.mcus_x);
    jpeg_destroy_decompress(&cinfo);
    return -1;
  }
  /* Clamp rather than refuse: a selection running to the end of the image is
     the normal case, not a mistake. */
  if (start + count > total) count = (int)(total - start);

  per_mcu = (size_t)info.blocks_per_mcu * DCTSIZE2;
  flat = (JCOEF *)malloc((size_t)count * per_mcu * sizeof(JCOEF));
  if (!flat) {
    jr_set_err(err, err_len, "Out of memory reading %d MCU(s).", count);
    jpeg_destroy_decompress(&cinfo);
    return -1;
  }

  coef_arrays = jpeg_read_coefficients(&cinfo);
  access_all(&cinfo, coef_arrays, &info, buf, FALSE);
  for (k = 0; k < count; k++) {
    const long idx = start + k;
    mcu_read(buf, &cinfo, (int)(idx / info.mcus_x), (int)(idx % info.mcus_x),
             flat + (size_t)k * per_mcu);
  }

  jpeg_finish_decompress(&cinfo);
  jpeg_destroy_decompress(&cinfo);

  *out = (int16_t *)flat;
  *out_count = (size_t)count * per_mcu;
  return 0;
}

int jr_quantize_patch(const uint8_t *ref, size_t ref_len,
                      const uint8_t *rgb, int width, int height,
                      int16_t **out, size_t *out_count,
                      int *out_mcus_x, int *out_mcus_y,
                      char *err, size_t err_len) {
  struct jpeg_decompress_struct refinfo, tmpinfo;
  struct jpeg_compress_struct dstinfo;
  struct jr_error_mgr jerr;
  jvirt_barray_ptr *coef_arrays;
  JBLOCKARRAY buf[JR_MAX_COMPONENTS];
  unsigned char *tmpbuf = NULL;
  unsigned long tmpsize = 0;
  JCOEF *flat = NULL;
  jr_info info, tinfo;
  size_t per_mcu;
  long total, idx;
  int ref_live = 0, dst_live = 0, tmp_live = 0;

  if (!ref || ref_len == 0 || !rgb || width <= 0 || height <= 0 || !out || !out_count) {
    jr_set_err(err, err_len, "No patch to quantize.");
    return -1;
  }
  *out = NULL;
  *out_count = 0;

  /* Both decompress objects and the compressor share one error manager, as in
     jr_apply -- whichever fails longjmps to the same recovery point. */
  refinfo.err = jpeg_std_error(&jerr.pub);
  dstinfo.err = &jerr.pub;
  tmpinfo.err = &jerr.pub;
  jerr.pub.error_exit = jr_error_exit;
  jerr.pub.emit_message = jr_emit_message;
  jerr.message[0] = '\0';

  if (setjmp(jerr.setjmp_buffer)) {
    jr_set_err(err, err_len, "libjpeg: %s", jerr.message);
    if (tmp_live) jpeg_destroy_decompress(&tmpinfo);
    if (dst_live) jpeg_destroy_compress(&dstinfo);
    if (ref_live) jpeg_destroy_decompress(&refinfo);
    if (tmpbuf) free(tmpbuf);
    if (flat) free(flat);
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

  jpeg_mem_dest(&dstinfo, &tmpbuf, &tmpsize);
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
  jpeg_create_decompress(&tmpinfo);
  tmp_live = 1;
  jpeg_mem_src(&tmpinfo, tmpbuf, tmpsize);
  jpeg_read_header(&tmpinfo, TRUE);
  fill_info(&tmpinfo, &tinfo);

  per_mcu = (size_t)tinfo.blocks_per_mcu * DCTSIZE2;
  total = (long)tinfo.mcus_x * tinfo.mcus_y;
  if (per_mcu == 0 || total <= 0) {
    jr_set_err(err, err_len, "The quantized patch came back empty.");
    jpeg_destroy_decompress(&tmpinfo);
    free(tmpbuf);
    return -1;
  }

  flat = (JCOEF *)malloc((size_t)total * per_mcu * sizeof(JCOEF));
  if (!flat) {
    jr_set_err(err, err_len, "Out of memory quantizing a %dx%d patch.", width, height);
    jpeg_destroy_decompress(&tmpinfo);
    free(tmpbuf);
    return -1;
  }

  coef_arrays = jpeg_read_coefficients(&tmpinfo);
  access_all(&tmpinfo, coef_arrays, &tinfo, buf, FALSE);
  for (idx = 0; idx < total; idx++)
    mcu_read(buf, &tmpinfo, (int)(idx / tinfo.mcus_x), (int)(idx % tinfo.mcus_x),
             flat + (size_t)idx * per_mcu);

  jpeg_finish_decompress(&tmpinfo);
  jpeg_destroy_decompress(&tmpinfo);
  tmp_live = 0;
  free(tmpbuf);

  *out = (int16_t *)flat;
  *out_count = (size_t)total * per_mcu;
  if (out_mcus_x) *out_mcus_x = tinfo.mcus_x;
  if (out_mcus_y) *out_mcus_y = tinfo.mcus_y;
  return 0;
}

int jr_decode(const uint8_t *in, size_t in_len, int ycbcr,
              uint8_t **pixels, int *width, int *height,
              char *err, size_t err_len) {
  struct jpeg_decompress_struct cinfo;
  struct jr_error_mgr jerr;
  uint8_t *buffer = NULL;
  int gray = 0;
  size_t stride = 0;

  if (!in || in_len == 0 || !pixels) {
    jr_set_err(err, err_len, "No image data to decode.");
    return -1;
  }
  *pixels = NULL;

  cinfo.err = jpeg_std_error(&jerr.pub);
  jerr.pub.error_exit = jr_error_exit;
  jerr.pub.emit_message = jr_emit_message;
  jerr.message[0] = '\0';

  if (setjmp(jerr.setjmp_buffer)) {
    jr_set_err(err, err_len, "libjpeg: %s", jerr.message);
    jpeg_destroy_decompress(&cinfo);
    if (buffer) free(buffer);
    return -1;
  }

  jpeg_create_decompress(&cinfo);
  jpeg_mem_src(&cinfo, in, (unsigned long)in_len);
  jpeg_read_header(&cinfo, TRUE);

  gray = (cinfo.num_components == 1);
  /* JCS_YCbCr skips color conversion entirely, so samples come back as the
     JPEG stored them -- no clipping to the RGB gamut. */
  cinfo.out_color_space = gray ? JCS_GRAYSCALE : (ycbcr ? JCS_YCbCr : JCS_RGB);

  jpeg_start_decompress(&cinfo);

  stride = (size_t)cinfo.output_width * (size_t)cinfo.output_components;
  buffer = (uint8_t *)malloc((size_t)cinfo.output_height * (size_t)cinfo.output_width * 3);
  if (!buffer) {
    jr_set_err(err, err_len, "Out of memory decoding a %ux%u image.",
               cinfo.output_width, cinfo.output_height);
    jpeg_abort_decompress(&cinfo);
    jpeg_destroy_decompress(&cinfo);
    return -1;
  }

  {
    JSAMPROW row = (JSAMPROW)malloc(stride);
    if (!row) {
      jr_set_err(err, err_len, "Out of memory decoding a scanline.");
      free(buffer);
      jpeg_abort_decompress(&cinfo);
      jpeg_destroy_decompress(&cinfo);
      return -1;
    }
    while (cinfo.output_scanline < cinfo.output_height) {
      unsigned int y = cinfo.output_scanline;
      uint8_t *dst = buffer + (size_t)y * (size_t)cinfo.output_width * 3;
      jpeg_read_scanlines(&cinfo, &row, 1);
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
  }

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
  unsigned char *outbuf = NULL;
  unsigned long outsize = 0;
  int copy_markers = (marker_src != NULL && marker_src_len > 0);
  int src_live = 0, dst_live = 0;

  if (!pixels || width <= 0 || height <= 0 || !out || !out_len) {
    jr_set_err(err, err_len, "No pixels to encode.");
    return -1;
  }
  *out = NULL;
  *out_len = 0;

  /* One error manager for both objects, as in jr_apply -- either side
     longjmps to the same recovery point. */
  cinfo.err = jpeg_std_error(&jerr.pub);
  srcinfo.err = &jerr.pub;
  jerr.pub.error_exit = jr_error_exit;
  jerr.pub.emit_message = jr_emit_message;
  jerr.message[0] = '\0';

  if (setjmp(jerr.setjmp_buffer)) {
    jr_set_err(err, err_len, "libjpeg: %s", jerr.message);
    if (dst_live) jpeg_destroy_compress(&cinfo);
    if (src_live) jpeg_destroy_decompress(&srcinfo);
    if (outbuf) free(outbuf);
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
  jpeg_mem_dest(&cinfo, &outbuf, &outsize);
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
