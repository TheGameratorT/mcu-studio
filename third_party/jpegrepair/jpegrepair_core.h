/*
 * jpegrepair_core.h -- coefficient-domain JPEG repair, as a library.
 *
 * Derived from jpegrepair.c, Copyright (c) 2017, Don Mahurin.
 * https://github.com/openrightorg/jpegrepair  (BSD 3-Clause)
 * See LICENSE.jpegrepair for the original notice and disclaimer.
 *
 * Changes from upstream: the command-line program was replaced by the API
 * below, block selection gained rect and mask scopes, errors are reported
 * instead of exit()ing, and the coefficients can be held in memory between
 * edits (jr_coefs) so that a preview never has to re-run the entropy coder.
 */

#ifndef JPEGREPAIR_CORE_H
#define JPEGREPAIR_CORE_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define JR_MAX_COMPONENTS 4
#define JR_ERR_LEN 512

/* Largest picture any entry point will allocate coefficients for. A corrupt
   or mismatched frame header can claim 65535 x 65535, which would ask for
   ~12 GB before a single byte of picture data is read. */
#define JR_MAX_PIXELS (400LL * 1000 * 1000)

typedef enum {
  JR_OP_CDELTA = 1,      /* add a constant to the quantized DC coefficient  */
  JR_OP_COPY   = 2,      /* copy MCUs from a relative offset                */
  JR_OP_INSERT = 3,      /* shift MCUs forward, duplicating at the seam     */
  JR_OP_DELETE = 4,      /* shift MCUs backward, dropping N                 */
  JR_OP_PASTE  = 5,      /* overwrite MCUs with coefficients read earlier   */
  JR_OP_UNIT_INSERT = 6, /* shift 8x8 blocks forward in coding order        */
  JR_OP_UNIT_DELETE = 7  /* shift 8x8 blocks backward in coding order       */
} jr_op_type;

typedef enum {
  /* Scan-order run starting at MCU (row, col). `count` <= 0 runs to the
     end of the image. This is upstream's `dest BY BX [BN]` form, and the
     only scope that makes sense for INSERT and DELETE, which reshuffle the
     whole bitstream from their origin onward. */
  JR_SCOPE_RUN  = 0,
  /* Rectangle of MCUs. `h`/`w` <= 0 extend to the image edge.
     Upstream's `dest BY BX BH BW` form. */
  JR_SCOPE_RECT = 1,
  /* Arbitrary set of MCUs: a rows*cols byte map, non-zero means selected.
     Not in upstream -- it lets a GUI selection be applied exactly, in the
     coefficient domain, without touching any other block. */
  JR_SCOPE_MASK = 2
} jr_scope_kind;

typedef struct {
  jr_scope_kind kind;
  int row, col;           /* RUN, RECT: origin MCU                        */
  int h, w;               /* RECT: extent in MCUs. RUN: uses `w` as count */
  const uint8_t *mask;    /* MASK: rows*cols bytes, row-major             */
  int mask_rows, mask_cols;
} jr_scope;

typedef struct {
  jr_op_type type;
  jr_scope   scope;
  /* CDELTA: a = component index, b = delta on the quantized DC.
     COPY:   a = row offset, b = column offset, both in MCUs.
     INSERT, DELETE: a = number of MCUs.
     UNIT_INSERT, UNIT_DELETE: a = number of 8x8 blocks, b = which block of
       the scope's first MCU the shift starts at (0 .. blocks_per_mcu-1).
       Only a RUN scope makes sense, and its count is ignored.
     PASTE:  a = number of MCUs in `coefs`. */
  int a, b;
  /* PASTE only: MCUs from jr_read_mcus or jr_quantize_patch, written into the
     blocks the scope covers. Borrowed for the length of the call. Unlike COPY,
     this is a snapshot -- it does not move when earlier ops in the same
     jr_apply reshuffle the stream underneath it.

     A JR_SCOPE_RUN scope writes the payload as a run in scan order from its
     origin, stopping at the image edge: the shape jr_read_mcus produces, so a
     lifted run goes back down the way it came up. A RECT or MASK scope instead
     walks the MCUs it covers in scan order and consumes one payload MCU per
     covered MCU, which is what fills an arbitrary selection from a patch
     assembled to match it. */
  const int16_t *coefs;
  size_t coef_count;
} jr_op;

typedef struct {
  int width, height;
  int num_components;
  int mcu_width, mcu_height; /* pixels */
  int mcus_x, mcus_y;
  int max_h_samp, max_v_samp;
  int h_samp[JR_MAX_COMPONENTS];
  int v_samp[JR_MAX_COMPONENTS];
  /* Step of the DC term in each component's quantization table. A CDELTA of
     1 moves the dequantized DC by this much, i.e. every sample in the block
     by dc_quant/8. It is per-component and per-image -- never assume 1. */
  int dc_quant[JR_MAX_COMPONENTS];
  /* Blocks the entropy coder writes per MCU: sum of h_samp*v_samp over the
     components. Sizes the buffers jr_read_mcus fills. */
  int blocks_per_mcu;
  int progressive;
  /* MCUs per restart interval, 0 when the file has no restart markers. */
  int restart_interval;
  /* The libjpeg J_COLOR_SPACE the file stores (1 gray, 2 RGB, 3 YCbCr,
     4 CMYK, 5 YCCK). */
  int color_space;
  int data_precision;
  int arith_code;
  /* Number of SOS segments in the file. 1 for an ordinary baseline file;
     more for progressive files and for sequential files whose components
     were written one scan at a time, where MCU scan order is not the order
     damage spreads in. */
  int scan_count;
} jr_info;

/* Every entry point returns 0 on success and -1 on failure, writing a
   NUL-terminated reason into `err` when `err` is non-NULL. Buffers handed
   back through `out`/`pixels` are owned by the caller: free with jr_free. */

int jr_probe(const uint8_t *in, size_t in_len, jr_info *out_info,
             char *err, size_t err_len);

/* ------------------------------------------------------------------ */
/* Coefficients held in memory                                         */
/* ------------------------------------------------------------------ */

/* Every coefficient of an image, MCU-major: per MCU, each component in turn,
   each component's v_samp*h_samp blocks in raster order, each block 64
   coefficients in natural order. That is the order the entropy coder writes
   them in, so a shift by N blocks in coding order is a memmove, and the layout
   is the one jr_read_mcus and JR_OP_PASTE trade in. */
typedef struct jr_coefs {
  jr_info info;
  int comp_offset[JR_MAX_COMPONENTS]; /* first block of each component in an MCU */
  int ds_width[JR_MAX_COMPONENTS];    /* real samples per component row */
  int ds_height[JR_MAX_COMPONENTS];   /* real sample rows per component */
  uint16_t quant[JR_MAX_COMPONENTS][64]; /* natural order */
  size_t unit_count;                  /* mcus * blocks_per_mcu */
  int16_t *data;                      /* unit_count * 64, exactly as edited */
  /* The DC each block is previewed and written with: data's DC wherever its
     step from the block coded before it fits in 11 bits, the nearest value
     that does otherwise. See jr_coefs_refresh_dc. */
  int16_t *out_dc;                    /* unit_count */
  /* Decoder tables, filled at load so rendering threads only read. */
  int cr_r[256], cb_b[256];
  int32_t cr_g[256], cb_g[256];
} jr_coefs;

/* Entropy-decodes `in` once. Coefficients are kept as decoded, out-of-range
   ones included; see jr_coefs_refresh_dc. */
int jr_coefs_load(const uint8_t *in, size_t in_len, jr_coefs **out,
                  char *err, size_t err_len);
jr_coefs *jr_coefs_clone(const jr_coefs *c);
/* Copies src's coefficients over dst's; both must have the same geometry. */
int jr_coefs_copy_into(jr_coefs *dst, const jr_coefs *src);
void jr_coefs_free(jr_coefs *c);

/* The 64 coefficients of one block, or NULL outside the coded grid. */
int16_t *jr_coefs_block(jr_coefs *c, int comp, int block_row, int block_col);

/* `blk` (a block of c->data) as the preview and the encoder see it: output
   DC, AC clamped to what the coder can write. See jr_coefs_refresh_dc. */
void jr_coefs_output_block(const jr_coefs *c, const int16_t *blk, int16_t *out);

int jr_coefs_apply(jr_coefs *c, const jr_op *ops, size_t n_ops,
                   char *err, size_t err_len);

/* Edits never clip. `data` holds whatever the damaged stream decoded to and
   whatever the edits made of it, so corrections can be stacked, overshot and
   undone freely: a CDELTA of +2047 followed by -2047 is always a no-op. The
   limits of what a baseline Huffman coder can store are applied only on the
   way out, by the preview and by jr_coefs_write alike, so the two always show
   the same picture:

   - AC coefficients are clamped to +/-1023.
   - A DC value may be anything, but the step between it and the DC coded
     before it (same component, coding order, predictor starting at 0) must
     fit in 11 bits. Damage routinely walks a component's DC far past +/-1024
     one legal step at a time; a block is only moved when its own step is too
     big, and the blocks after it are measured from where it landed.

   jr_coefs_load and jr_coefs_apply call this; anything else that writes into
   `data` must call it before rendering or writing. Returns the number of
   blocks whose output DC differs from their stored one. */
long jr_coefs_refresh_dc(jr_coefs *c);

/* Writes `c` as a JPEG. `header_src` is the file the coefficients came from:
   its markers (Exif, ICC, ...) and frame parameters are carried over. The
   coefficients are written as jr_coefs_output_block gives them -- no IDCT, no
   requantization -- with optimized Huffman tables, as a sequential
   (non-progressive) file. */
int jr_coefs_write(const jr_coefs *c, const uint8_t *header_src, size_t header_src_len,
                   uint8_t **out, size_t *out_len, char *err, size_t err_len);

/* Decodes MCU rows [mcu_row0, mcu_row1) into `out`, which addresses the whole
   picture (row 0 at `out`, `stride` bytes per row, 3 bytes per pixel). Only
   the picture rows those MCU rows cover are written. The result is the same,
   sample for sample, as libjpeg-turbo's default decode of what jr_coefs_write
   produces (islow IDCT, fancy upsampling, JFIF YCbCr->RGB), with overdriven
   samples saturating as its SIMD IDCTs make them. Safe to call from several
   threads at once on disjoint row ranges. */
void jr_coefs_render_rows(const jr_coefs *c, int mcu_row0, int mcu_row1, int ycbcr,
                          uint8_t *out, size_t stride);

/* Where each block ends up after `ops`, without touching any coefficients:
   `unit_src` (unit_count entries) is filled with the original index of the
   block now at each position, or -1 for a block written from a paste, and
   `mcu_flags` (mcus entries, may be NULL) gets JR_TRACE_* bits. */
#define JR_TRACE_MOVED     1
#define JR_TRACE_DC        2
#define JR_TRACE_PASTED    4
int jr_trace(const jr_info *info, const jr_op *ops, size_t n_ops,
             int32_t *unit_src, uint8_t *mcu_flags, char *err, size_t err_len);

/* ------------------------------------------------------------------ */
/* One-shot entry points                                               */
/* ------------------------------------------------------------------ */

/* Reads coefficients once, applies every op in order, and re-emits the
   entropy-coded stream. No IDCT and no requantization happen, so a block no
   op touched keeps its exact coefficients and decodes pixel-for-pixel to what
   it did before -- there is no generational loss. All markers are copied
   through. (The output *file* is not a byte prefix of the input: libjpeg
   re-serializes the whole stream. The guarantee is about image data.) */
int jr_apply(const uint8_t *in, size_t in_len,
             const jr_op *ops, size_t n_ops,
             uint8_t **out, size_t *out_len,
             char *err, size_t err_len);

/* Copies `count` MCUs out of the coefficient array, walking scan order from
   (row, col) and stopping at the end of the image. The layout is MCU-major:
   per MCU, each component in turn, each component's v_samp*h_samp blocks in
   raster order, each block 64 coefficients in natural order -- the order the
   entropy coder writes them, so a run read here can be written back with
   JR_OP_PASTE. *out_count is in coefficients, not bytes. */
int jr_read_mcus(const uint8_t *in, size_t in_len, int row, int col, int count,
                 int16_t **out, size_t *out_count, char *err, size_t err_len);
/* The same, from coefficients already in memory. */
int jr_coefs_read_mcus(const jr_coefs *c, int row, int col, int count,
                       int16_t **out, size_t *out_count, char *err, size_t err_len);

/* Quantizes foreign pixels into the blocks a given JPEG's quantization would
   give them. `ref` is that JPEG -- only its header is read -- and `rgb` is
   width*height interleaved 8-bit RGB, which must measure a whole number of
   `ref`'s MCUs on both axes. The output is MCU-major in jr_read_mcus's layout,
   ready for JR_OP_PASTE into `ref` itself, and *out_mcus_x/y report the grid
   it covers.

   This is the one way pixels that were never in a JPEG can enter one. They
   have no coefficients of their own, so a color conversion, a chroma
   downsample, a forward DCT and a quantization all have to happen -- there is
   no lossless path for them. What is on offer instead is that the cost stops
   at the patch: it is libjpeg's encoder running with `ref`'s own quantization
   tables and sampling factors, so the blocks handed back are denominated in
   the same tables as their neighbors (a camera's own encoder would round and
   downsample a little differently), and every block outside the patch keeps
   the coefficients it already had. */
int jr_quantize_patch(const uint8_t *ref, size_t ref_len,
                      const uint8_t *rgb, int width, int height,
                      int16_t **out, size_t *out_count,
                      int *out_mcus_x, int *out_mcus_y,
                      char *err, size_t err_len);

/* Decodes to 3 interleaved 8-bit channels. `ycbcr` selects the JPEG's own
   YCbCr samples over RGB -- worth using when measuring color, because the
   RGB conversion clips out-of-range components and drags chroma toward
   neutral on exactly the damaged blocks this tool exists to fix. */
int jr_decode(const uint8_t *in, size_t in_len, int ycbcr,
              uint8_t **pixels, int *width, int *height,
              char *err, size_t err_len);

/* Re-encodes 8-bit RGB as a baseline JPEG with libjpeg's standard tables.

   marker_src, when given, is the JPEG the pixels were decoded from: its APPn
   and COM markers -- Exif above all -- are copied into the output, so a
   re-encode costs a generation of quality but not the image's metadata.
   Pass NULL to write none. */
int jr_encode_rgb(const uint8_t *pixels, int width, int height, int quality,
                  const uint8_t *marker_src, size_t marker_src_len,
                  uint8_t **out, size_t *out_len,
                  char *err, size_t err_len);

void jr_free(void *p);

#ifdef __cplusplus
}
#endif

#endif /* JPEGREPAIR_CORE_H */
