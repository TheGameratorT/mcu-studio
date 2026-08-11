/*
 * jpegrepair_core.h -- coefficient-domain JPEG repair, as a library.
 *
 * Derived from jpegrepair.c, Copyright (c) 2017, Don Mahurin.
 * https://github.com/openrightorg/jpegrepair  (BSD 3-Clause)
 * See LICENSE.jpegrepair for the original notice and disclaimer.
 *
 * Changes from upstream: the command-line program was replaced by the
 * stateless memory-in/memory-out API below, block selection gained rect
 * and mask scopes, and errors are reported instead of exit()ing.
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

typedef enum {
  JR_OP_CDELTA = 1, /* add a constant to the quantized DC coefficient */
  JR_OP_COPY   = 2, /* copy blocks from a relative offset             */
  JR_OP_INSERT = 3, /* shift blocks forward, duplicating at the seam  */
  JR_OP_DELETE = 4, /* shift blocks backward, dropping N              */
  JR_OP_PASTE  = 5  /* overwrite MCUs with coefficients read earlier  */
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
     INSERT, DELETE: a = number of blocks.
     PASTE:  a = number of MCUs in `coefs`. */
  int a, b;
  /* PASTE only: MCUs from jr_read_mcus or jr_quantize_patch, written into the
     blocks the scope covers. Borrowed for the length of the call. Unlike COPY,
     this is a snapshot -- it does not move when earlier ops in the same
     jr_apply reshuffle the stream underneath it.

     A JR_SCOPE_RUN scope writes the payload as a run in scan order from its
     origin, wrapping at the image edge: the shape jr_read_mcus produces, so a
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
} jr_info;

/* Every entry point returns 0 on success and -1 on failure, writing a
   NUL-terminated reason into `err` when `err` is non-NULL. Buffers handed
   back through `out`/`pixels` are owned by the caller: free with jr_free. */

int jr_probe(const uint8_t *in, size_t in_len, jr_info *out_info,
             char *err, size_t err_len);

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

/* Quantizes foreign pixels into the blocks a given JPEG would have stored.
   `ref` is that JPEG -- only its header is read -- and `rgb` is width*height
   interleaved 8-bit RGB, which must measure a whole number of `ref`'s MCUs on
   both axes. The output is MCU-major in jr_read_mcus's layout, ready for
   JR_OP_PASTE into `ref` itself, and *out_mcus_x/y report the grid it covers.

   This is the one way pixels that were never in a JPEG can enter one. They
   have no coefficients of their own, so a color conversion, a chroma
   downsample, a forward DCT and a quantization all have to happen -- there is
   no lossless path for them, and any tool that claims otherwise is re-encoding
   somewhere you cannot see. What is on offer instead is that the cost stops at
   the patch: it is libjpeg's own encoder running with `ref`'s own quantization
   tables and sampling factors, so the blocks handed back are the ones `ref`
   would hold had it been shot with this content, and every block outside the
   patch keeps the coefficients it already had. One generation, on the new data
   only, in the same currency as its neighbors. */
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

/* Re-encodes 8-bit RGB as a baseline JPEG. Used for the stitched output,
   where pixels came from more than one source and a coefficient-domain
   edit cannot express the result.

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
