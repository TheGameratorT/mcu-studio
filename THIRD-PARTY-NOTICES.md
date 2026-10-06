# Third-party notices

MCU Studio is licensed under the GNU General Public License v3.0 (see
`LICENSE`). It includes and links against the following third-party software,
all of which is GPL-compatible.

---

## jpegrepair — vendored and modified

* Upstream: <https://github.com/openrightorg/jpegrepair>
* Files: `third_party/jpegrepair/jpegrepair_core.c`, `jpegrepair_core.h`
* License: BSD 3-Clause, full text in `third_party/jpegrepair/LICENSE.jpegrepair`

The repair transform in `jpegrepair_core.c` is derived from upstream's
`jpegrepair.c`. **Changes made:**

* The command-line program was replaced by a library API (`jr_probe`,
  `jr_apply`, `jr_decode`, `jr_encode_rgb`, and the in-memory `jr_coefs_*`
  family), so a whole list of operations is applied to coefficients held in
  memory instead of one process launch and one file round-trip per operation.
* Coefficients are held MCU-major between edits; the operations work on whole
  MCUs, and on single 8x8 blocks in coding order (`JR_OP_UNIT_INSERT`,
  `JR_OP_UNIT_DELETE`, not in upstream). libjpeg's virtual arrays are read and
  written one row group at a time, as the memory manager expects.
* Insert and delete shift whole MCUs in scan order. Upstream wrapped each
  component's block rows at `width_in_blocks`, which slid luma against chroma
  when the image was not a whole number of MCUs wide.
* Block selection moved behind a `jr_scope` type, which adds a rectangle scope
  and an arbitrary MCU-mask scope alongside upstream's scan-order run.
* The virtual-array accessor is now passed the decompress object itself.
  Upstream passed `&srcinfo` where `srcinfo` was already a pointer, handing
  libjpeg a `j_common_ptr` aimed at a pointer variable.
* The `cdelta` case writes the DC coefficient directly rather than scanning all
  64 coefficients to touch one, and clamps it to what the Huffman coder can
  write. Coefficients a damaged stream decoded out of range are clamped once on
  load (`jr_coefs_sanitize`).
* `jr_read_mcus`, `JR_OP_PASTE`, `jr_quantize_patch` and `jr_trace` were added.
* Failures return an error to the caller instead of calling `exit()`.

> Copyright (c) 2017, Don Mahurin
>
> Redistribution and use in source and binary forms, with or without
> modification, are permitted provided that the following conditions are met:
>
> * Redistributions of source code must retain the above copyright notice, this
>   list of conditions and the following disclaimer.
>
> * Redistributions in binary form must reproduce the above copyright notice,
>   this list of conditions and the following disclaimer in the documentation
>   and/or other materials provided with the distribution.
>
> * Neither the name of the copyright holder nor the names of its
>   contributors may be used to endorse or promote products derived from
>   this software without specific prior written permission.
>
> THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
> AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
> IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
> DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE
> FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
> DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
> SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
> CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY,
> OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
> OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.

Neither Don Mahurin nor the jpegrepair project endorses this tool.

---

## Independent JPEG Group — preview decoder

* File: `third_party/jpegrepair/jr_render.c`
* License: the IJG license, full text in `third_party/jpegrepair/README.ijg`

`jr_render.c` reimplements, against coefficients held in memory, the accurate
integer IDCT (`jidctint.c`), the triangle-filter chroma upsamplers
(`jdsample.c`), the main buffer controller's edge handling (`jdmainct.c`), the
sample range-limit table (`jdmaster.c`) and the JFIF YCbCr-to-RGB tables
(`jdcolor.c`), so that previews match libjpeg-turbo's output sample for sample.

> This software is based in part on the work of the Independent JPEG Group.

---

## Independent JPEG Group — marker-copying helpers

* Files: `third_party/jpegrepair/transupp.c`, `transupp.h` (unmodified)
* License: the IJG license, full text in `third_party/jpegrepair/README.ijg`

> Copyright (C) 1997, Thomas G. Lane.
> This file is part of the Independent JPEG Group's software.

Only `jcopy_markers_setup` and `jcopy_markers_execute` are used, to carry EXIF
and other application markers from the source image into the repaired one.

---

## libjpeg / libjpeg-turbo — linked, not bundled

Provided by the system at build time and found via CMake's `FindJPEG`. Its
license (IJG, plus BSD-3-Clause and zlib terms for libjpeg-turbo) is the one
shipped by your distribution or vendor. Nothing from it is redistributed here.

---

## ONNX Runtime — optional, loaded at run time, bundled in the Windows installer and the AppImage

Built against its headers when they are found, and opened at run time if it
is installed, to run AI fill's local model. MIT License, Copyright (c)
Microsoft Corporation; see <https://github.com/microsoft/onnxruntime>.

Not part of this repository. The Windows installer carries it as MSYS2 builds
it, with the libraries that build needs (ONNX and Abseil, Apache License 2.0;
Protocol Buffers and RE2, BSD-3-Clause), and puts their license texts in the
`licenses` folder beside the program. The AppImage carries the release the
ONNX Runtime project publishes, with its `LICENSE` and `ThirdPartyNotices.txt`
under `usr/share/doc/mcu-studio/onnxruntime`.

---

## LaMa inpainting model — downloaded on request, not bundled

* Model: "Resolution-robust Large Mask Inpainting with Fourier Convolutions"
  (Suvorov et al.), <https://github.com/advimman/lama>, Apache License 2.0
* ONNX export fetched by the program when the user asks for it:
  <https://huggingface.co/Carve/LaMa-ONNX> (`lama_fp32.onnx`), Apache License 2.0

The model file is not part of this repository or of any package built from it.
The program downloads it only on request and checks it against a known
SHA-256 before keeping it.

---

## Qt 6 — linked, not bundled

Used under the GNU Lesser General Public License v3.0. Qt itself is not
redistributed here; see <https://www.qt.io/licensing> for its terms.

---

## PhotoDemon — algorithm reference

The auto-color correction in `src/AutoColor.cpp` reimplements the approach
used by PhotoDemon's automatic white balance and clarity adjustments. No
PhotoDemon code is included.
