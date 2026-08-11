# Third-party notices

MCU Studio is licensed under the GNU General Public License v3.0 (see
`LICENSE`). It includes and links against the following third-party software,
all of which is GPL-compatible.

---

## jpegrepair — vendored and modified

* Upstream: <https://github.com/openrightorg/jpegrepair>
* Files: `third_party/jpegrepair/jpegrepair_core.c`, `jpegrepair_core.h`
* Licence: BSD 3-Clause, full text in `third_party/jpegrepair/LICENSE.jpegrepair`

The repair transform in `jpegrepair_core.c` is derived from upstream's
`jpegrepair.c`. **Changes made:**

* The command-line program was replaced by a stateless memory-in/memory-out
  library API (`jr_probe`, `jr_apply`, `jr_decode`, `jr_encode_rgb`), so a whole
  list of operations is applied to a single coefficient read instead of one
  process launch and one file round-trip per operation.
* Block selection moved behind a `jr_scope` type, which adds a rectangle scope
  and an arbitrary MCU-mask scope alongside upstream's scan-order run.
* The virtual-array accessor is now passed the decompress object itself.
  Upstream passed `&srcinfo` where `srcinfo` was already a pointer, handing
  libjpeg a `j_common_ptr` aimed at a pointer variable.
* The `cdelta` case writes the DC coefficient directly rather than scanning all
  64 coefficients to touch one.
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

## Independent JPEG Group — marker-copying helpers

* Files: `third_party/jpegrepair/transupp.c`, `transupp.h` (unmodified)
* Licence: the IJG licence, full text in `third_party/jpegrepair/README.ijg`

> Copyright (C) 1997, Thomas G. Lane.
> This file is part of the Independent JPEG Group's software.

Only `jcopy_markers_setup` and `jcopy_markers_execute` are used, to carry EXIF
and other application markers from the source image into the repaired one.

---

## libjpeg / libjpeg-turbo — linked, not bundled

Provided by the system at build time and found via CMake's `FindJPEG`. Its
licence (IJG, plus BSD-3-Clause and zlib terms for libjpeg-turbo) is the one
shipped by your distribution or vendor. Nothing from it is redistributed here.

---

## Qt 6 — linked, not bundled

Used under the GNU Lesser General Public License v3.0. Qt itself is not
redistributed here; see <https://www.qt.io/licensing> for its terms.

---

## PhotoDemon — algorithm reference

The auto-colour correction in `src/AutoColor.cpp` reimplements the approach
used by PhotoDemon's automatic white balance and clarity adjustments. No
PhotoDemon code is included.
