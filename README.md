# MCU Studio

**Repair damaged JPEGs at the level they are stored in: the bitstream, the
MCUs and the DCT coefficients, with no re-encoding and no quality loss.**

[![C++17](https://img.shields.io/badge/C%2B%2B-17-00599C?style=for-the-badge&logo=cplusplus&logoColor=white)](https://isocpp.org/)
[![Qt 6](https://img.shields.io/badge/GUI-Qt%206-41CD52?style=for-the-badge&logo=qt&logoColor=white)](https://www.qt.io/)
[![License](https://img.shields.io/badge/License-GPL--3.0-green?style=for-the-badge)](LICENSE)

A Qt 6 desktop application, and a command-line tool, for repairing JPEGs
damaged by ransomware encryption (STOP/Djvu and its relatives), truncation,
bad sectors or bit rot: the kinds of damage that knock the entropy-coded stream
out of step, destroy the header, or leave a component's DC level drifting.

MCU Studio works on the units a JPEG is actually stored in. It reads the
Huffman-coded **bitstream** itself, so it can tell where each **minimum coded
unit** (MCU) starts and where decoding went wrong, and it edits bits and bytes
there. It also edits the **DCT coefficients** libjpeg decodes from that stream,
holding them in memory and writing them straight back out. Blocks you did not
touch keep their exact coefficients, so repairing an image never costs a
re-encode generation.

## What a repair looks like

This is the kind of recovery the tool can achieve.

![A damaged photo as MCU Studio first opens it](docs/screenshots/recover-before.jpg)

*The file as it opens. The top of the picture survives; below it the stream has
lost alignment, so the rest decodes as the wrong content at the wrong DC level.*

![The same photo after twenty-five repair steps](docs/screenshots/recover-after.jpg)

*The same file after 25 steps: twelve MCU inserts to bring the stream back into
alignment, thirteen DC offsets to pull the color back. Every step is a
coefficient move, so nothing here has been re-encoded. A few bands of damage
remain, and the repair steps that produced this are still an editable list.
(Faces blacked out for this README.)*

## Why the coefficient domain, and the bitstream under it?

Ordinary image editors cannot help with these files. They decode to pixels,
which means every save is a fresh lossy generation over the whole picture, and
they refuse to open the badly damaged files in the first place. The operations
that actually fix a broken JPEG (cut the garbage out of the stream, shift a
component's DC level, insert or delete MCUs to re-align a drifted scan, splice
on a working header) have no expression in the pixel domain at all.

Command-line tools that work at this level exist, but they ask you to guess:
you name a block range and a number, run the tool, open the result, and look.
MCU Studio puts that loop in one window, with the grid you are selecting in, a
live preview of the correction, an editable list of the steps, and searches
that rank the likely fixes for you to preview.

## Terms

| Term | Meaning here |
|---|---|
| **Block** | One 8×8 DCT data unit of one component. |
| **MCU** | The group of blocks the encoder writes together: in 4:2:0, a 16×16-pixel area holding 4 Y, 1 Cb and 1 Cr block. Everything the grid shows, selects and shifts is an MCU. |
| **Scan order / coding order** | The order MCUs (and the blocks inside them) are written in the stream. Damage spreads in this order in baseline files; progressive files are coded scan by scan instead, and the tool says so when it opens one. |
| **DC offset** | A constant added to the quantized DC coefficient of every block in scope. |
| **Restart marker** | RST0–RST7, which a camera may write every *n* MCUs. The stream is byte-aligned and the DC predictors are reset there. |
| **Lossless** | No coefficient outside an edit changes. True of every bitstream and coefficient edit here; not of *Fill from reference* or *Auto color*, which quantize new content and say so. |

## Features

### Repair

- **Bitstream map.** The Analysis panel decodes the Huffman stream itself and
  records where every MCU starts (byte and bit) and where decoding stopped
  making sense: an invalid code, a run of coefficients past the end of a block,
  a marker in the middle of an MCU, restart markers missing or out of order.
  Hovering an MCU shows its bits in a hex view. *Select the first decode error*
  jumps to where the damage became visible to the decoder.
- **Bit and byte edits.** Delete or insert bytes, delete or insert bits inside
  the entropy-coded data (with byte stuffing and restart markers rewritten
  correctly), flip a single bit, or cut the file at a point. These are steps in
  the repair list like any other, and always replay before the coefficient steps.
- **Resync search.** From the MCU where damage begins, the tool tries every
  deletion or insertion of up to 64 bits, longer deletions up to 2 KiB, and every
  whole-byte deletion starting between there and the first decode error. It
  keeps the edits after which the stream decodes cleanly, then ranks them. A
  near miss also decodes cleanly, because Huffman codes resynchronize on their
  own, but it leaves a DC offset on everything after it and garbage in the first
  MCU. So candidates are ranked by the DC offset they leave against the
  untouched rows above, and by how the first MCUs' edges meet their neighbors.
  Selecting a candidate previews it. On synthetic damage (whole bytes of junk
  inserted inside an MCU, 49 cases across qualities 50–95), a fix that restores
  every MCU after the damage exactly was ranked first in 38 cases, within the
  first five in 44 and within the first ten in 48. The misses cluster at quality
  92–95, where one DC step is a fraction of a sample level and the offset a
  near miss leaves is hard to tell from texture. Bits inserted at an MCU
  boundary were found first in every case tested.
- **MCU insert and delete**, plus **single-block insert and delete** in coding
  order, for a stream that lost part of an MCU and so sits a block out of step,
  luma against chroma. `[` and `]` preview one MCU at a time, `{` and `}` one
  block, Ctrl+Enter commits.
- **Find the alignment.** Tries every shift of the stream at the selected MCU
  and ranks them by how well the content below continues the content above, with
  any constant color step taken out first so that an uncorrected DC drift does
  not hide the right answer.
- **DC offsets per Y/Cb/Cr component, with live preview.** The panel calibrates
  the offset against the image's own DC quantization step and shows the shift it
  produces in 0–255 sample terms. Scope is the selection, the selection to the
  next restart marker (where a DC error left by a desync ends), the selection to
  the end of the image, or the whole image.
- **Estimate from surroundings.** Measures the step in each channel across the
  edge of the scope, on block edges computed from the coefficients at each
  component's own resolution, and sets the offsets that remove it.
- **Automatic color matching** against a block of the image or a patch of
  another copy of the photograph (a thumbnail, a gallery cache, a backup, JPEG or
  not). Only three numbers cross over: none of that file's pixels reach the
  repair.
- **Copy and paste MCUs**, within a file losslessly, and across files: when
  the quantization tables differ, the coefficients are converted to this file's
  tables, which is lossy only where this file's steps are coarser.
- **Fill MCUs from another copy of the photograph** in any format, quantized
  with the damaged file's own tables and sampling (see below for the cost).
- **Damage map and provenance overlays** (Ctrl+D, Ctrl+P). One colors the MCUs
  that look damaged; the other colors each MCU by what the repair did to it:
  moved, DC-adjusted, pasted or synthesized, or never recovered at all. F3 jumps
  to the next damaged MCU, and `\` toggles the original for comparison.
- **Auto color:** a per-channel levels stretch plus a midtone-contrast curve.
  It works on pixels, so it re-quantizes every MCU, with this file's own tables
  and sampling factors. The MCU grid and every later step stay where they are.

### Headers, ransomware and other files

- **Donor headers, for files that will not open at all.** Another photograph
  from the same card lends its header. Where part of the file's own header
  survives, its own quantization and Huffman tables, frame header and scan header
  are kept table by table, and the donor fills in only what is missing. The
  dialog suggests a sibling donor, ranks its guesses at where the surviving data
  resumes, previews each, and can:
  - **rank a whole folder of donors** by how cleanly the damaged data decodes
    under each one's tables. The wrong tables produce an error every few bytes;
    the right ones hardly any;
  - **edit the frame size** (with a portrait/landscape swap) and **detect the
    width** by trying widths until the MCU rows continue each other;
  - **renumber restart markers** so the first one is RST0. libjpeg treats any
    other as a lost or repeated interval and would shift the picture by one.
  The donor's Exif, XMP and comments are dropped on the way through, so a
  rescued photograph never inherits another shot's date or GPS fix.
- **STOP/Djvu.** Files carrying the STOP/Djvu footer are recognized, the footer
  is stripped before anything reads the data, and the splice point defaults to
  150 KiB, the prefix the ransomware encrypts. The footer holds the victim's
  personal ID. When it has the form of an offline ID (ending in `t1`), the tool
  says that a free decryptor (Emsisoft's STOP Djvu decryptor) may restore the
  file outright, which beats any repair.
- **Data after the image is not damage.** MPF previews, motion-photo videos and
  similar trailers are recognized, left out of every heuristic, and carried into
  the export with the MPF index rewritten to match.
- **Pictures inside the file.** The Exif thumbnail, MPF previews and camera
  previews in maker notes are listed, and can be saved or used directly as the
  color or fill reference.
- **Progressive files.** The Scans tab lists every scan (DC, AC bands,
  refinements) and where its data ends. *Keep scans up to here* drops damaged
  late scans for a softer picture with no corruption in it.
- **Triage a folder** (also in the CLI): every file sorted into healthy, healthy
  with a trailer, ransomware, header damaged, truncated, or decode errors.
- **Carve JPEGs out of any file**: disk images, camera RAW files (whose
  full-size preview is a JPEG), archives.

### The session

- **The repair is a list, not a result.** Every correction lands as a step in a
  side panel, and the image you see is the original with the ticked steps
  replayed over it. Untick one, reorder them, drop one from the middle. Undo and
  redo work over the whole session, and they survive closing the program.
- **There is no Save.** A `.mcup` project file beside the image is rewritten on
  every change. It identifies the image by its SHA-256 hash, so it notices if
  the file underneath changes. Reopen the photograph and the session comes back
  where you left it. What you export is the repaired JPEG, asked for explicitly
  and never written over the damaged original by default.
- **Metadata and dates survive the repair.** Exif, ICC, XMP and comment markers
  are carried into the output. The Exif thumbnail, which shows the picture before
  repair, is replaced with one of the repaired picture (optional). Pixel
  dimensions are corrected after a header transplant, and the exported file is
  stamped with the original's modification and access times.
- **Recovery report** (optional, per export): hashes of input and output, every
  step, and how many MCUs were left untouched, moved, DC-adjusted, pasted or
  synthesized, or never recovered. For when the photograph has to be relied on.

<details>
<summary>Replaying from the original is also what keeps insert and delete honest</summary>

When an image's width is not a multiple of the MCU width, the coefficient array
carries a column of dummy blocks past the right edge; when its height is not a
multiple of the MCU height, a row of them past the bottom. libjpeg overwrites
them on every write with a DC-only copy of their neighbor (the left one at the
right edge, the one above at the bottom), because `jpeg_write_coefficients`
refills dummy blocks even when nothing was decoded to pixels. On its own that
costs nothing, since those blocks are outside the visible image. Insert and
delete are what make it matter: they shift the stream *through* those
positions, so real picture data lands in the dummy blocks and is flattened by
the write. Under an edit-and-reapply model the next insert would then drag
those flattened blocks back into view. Replaying from the original keeps the
session to a single write, so the dummy blocks are only ever clobbered on the
final export, with nothing left to shift them into view.

</details>

## Installing

### Arch Linux

```sh
paru -S mcu-studio     # or your AUR helper of choice
```

The `PKGBUILD` also lives in [`packaging/aur`](packaging/aur).

### Windows

Download `McuStudio-<version>-Setup.exe` from the
[latest release](https://github.com/TheGameratorT/mcu-studio/releases). It is a
single installer carrying the application, the command-line tool and the Qt
runtime, and it adds an "Open with MCU Studio" entry to the JPEG context menu
without taking over as the default handler. See
[`installer/`](installer/README.md) for how it is built.

## Building

### Prerequisites

- CMake 3.21+
- A C++17 compiler
- Qt 6.2+ (`Widgets`, `Concurrent`, and `Test` for the tests)
- libjpeg-turbo with development headers. IJG libjpeg works too, but the
  preview decoder is written to match libjpeg-turbo's output, so with IJG
  libjpeg the preview can differ from the exported file by a level here and there.

<details>
<summary>Installing dependencies</summary>

```sh
# Arch
sudo pacman -S cmake qt6-base libjpeg-turbo

# Debian / Ubuntu
sudo apt install cmake qt6-base-dev libjpeg-turbo8-dev

# Fedora
sudo dnf install cmake qt6-qtbase-devel libjpeg-turbo-devel

# macOS
brew install cmake qt libjpeg-turbo

# Windows (MSYS2 MINGW64)
pacman -S mingw-w64-x86_64-{gcc,cmake,ninja,qt6-base,qt6-tools,libjpeg-turbo}
```

</details>

### Build, test and run

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build
ctest --test-dir build --output-on-failure
./build/bin/mcu-studio [image.jpg]
```

Options: `-DMCU_STUDIO_BUILD_TESTS=OFF`, `-DMCU_STUDIO_BUILD_CLI=OFF`, and
`-DMCU_STUDIO_FUZZ=ON` (with clang; see [`fuzz/`](fuzz/CMakeLists.txt)).

## Command line

`mcu-studio-cli` is the same core without a window:

```sh
mcu-studio-cli info photo.jpg                  # what it is and what is wrong with it
mcu-studio-cli triage ~/rescued --csv out.csv  # sort a folder by what is wrong
mcu-studio-cli resync photo.jpg -o fixed.jpg   # find the edit that resynchronizes the stream
mcu-studio-cli splice ~/encrypted --donor sibling.jpg -o ~/spliced
                                               # one donor onto a folder of STOP/Djvu files
mcu-studio-cli apply photo.jpg.mcup --report   # export a session's repair
mcu-studio-cli carve disk.img -o ~/carved      # every complete JPEG in any file
mcu-studio-cli extract photo.jpg -o ~/previews # the thumbnail and previews inside a JPEG
mcu-studio-cli bench photo.jpg                 # time the engine on your machine
```

`info`, `triage` and `resync` take `--json`.

## How it works

### The engine

The file is entropy-decoded once, into coefficients held in memory MCU by MCU,
in the order the stream codes them. After that an MCU shift is a `memmove`, a
DC offset is a loop over DC terms, and the screen is brought up to date by
decoding only the MCU rows whose coefficients changed. The decoder that does
this reimplements libjpeg's accurate integer IDCT, its "fancy" chroma upsampling
and its JFIF color conversion. Its output is identical, sample for sample, to
libjpeg-turbo decoding the exported file: the test suite checks this across
sampling layouts, odd sizes and progressive files. (Where a block is overdriven
past black or white, it saturates the way libjpeg-turbo's SIMD decoders and so
ordinary viewers do.) The Huffman encoder runs only when a file is exported.

Edits never clip. Coefficients are stored as the damaged stream decoded them
and as your corrections leave them, however far out of range, so corrections
can be stacked, overshot and taken back exactly. What a JPEG can actually hold
(AC terms within ±1023, and each DC within an 11-bit step of the one before it)
is applied only on the way out, by the preview and the export alike.

### DC offsets

A DC offset adds a constant `dC` to the **quantized** DC coefficient of every
block in scope. The dequantized DC therefore moves by `dC × Q00`, and because
the JPEG DCT normalizes the DC term to eight times the block's mean sample,
every sample in the block moves by:

```
dC × Q00 / 8
```

`Q00` is the DC entry of that component's quantization table, read from the
image itself. It differs per component and per quality level, which is why the
tool reads it rather than assuming `dC / 8`. DC values are clamped to the range
an 8-bit Huffman coder can write.

### Why damage looks the way it does

The DC coefficient is coded as a difference from the previous block's. When a
few bits are lost or garbage is inserted, the decoder reads nonsense for a
while, usually falls back into step on its own (Huffman codes resynchronize),
and from then on decodes the right data. But every block after the damage sits
at the wrong position, by however many blocks the garbage decoded as, and at
the wrong DC level, by whatever the garbage's DC differences summed to. Inserting
and deleting MCUs and adding DC offsets approximates the fix. Cutting the
garbage out of the stream is the exact fix, and the resync search finds it.
Without restart markers the error persists to the end of the image; with them,
to the next marker, where the encoder reset its predictors.

### Donor headers

![The donor header dialog previewing a transplant](docs/screenshots/donor-header.jpg)

*Opening a file whose first 150 KiB were destroyed, with another shot from the
same camera roll standing in as the donor. The preview is the splice decoded:
the surviving picture comes back shifted, because there is no way to know how
much data went with the header, and it runs out early, because the overwritten
bytes are gone for good. Insert and delete are what move the rest into place.*

A transplant is a header assembled from the donor, and from the damaged file
where its own header survives, followed by the damaged file's bytes from a
chosen offset on. Choosing that offset is the whole problem, because the damage
has no marker at its end. The tool offers, in order:

1. **Byte 0**, the default. It drops nothing and assumes nothing, and it is
   right whenever the header was lost rather than overwritten: a zeroed front,
   a carved fragment, a file that starts mid-scan.
2. **Right after the file's own scan header**, when part of the header
   survived. The file's own surviving tables are kept, and the donor supplies
   the rest.
3. **The first restart markers past the damage.** The data after one decodes
   correctly, because the encoder byte-aligned the stream and reset its DC
   predictors there. Which interval it is cannot be known from the marker,
   since markers count modulo 8, so the picture may still need shifting by
   whole intervals. The markers are renumbered so the first is RST0, or libjpeg
   would insert or skip an interval. Many cameras write no restart markers, and
   the option says so when it is not available rather than quietly vanishing.
4. **Just past the last byte pair no valid JPEG could contain.** Inside a scan
   every `FF` is stuffed (`FF 00`), a restart marker, padding, or the `EOI`.
   Encrypted or random bytes break that rule roughly every 250 bytes, so the
   last violation bounds the damage tightly. The search stops at the image's own
   end, ignoring a trailing preview or ransomware footer.
5. **150 KiB**, the prefix STOP/Djvu encrypts, and the first restart marker
   after it.

None of these can be verified from the bytes, so each is judged by decoding it:
the dialog splices, decodes and shows the result, and refuses to open a
combination that does not decode at all.

### Filling MCUs from a picture that is not a JPEG

Copying MCUs between two JPEGs can be lossless, and within one file it always
is: coefficients are lifted and written back as integers. Across two files it
holds only where the quantization tables and sampling factors match, since a
coefficient is an index into a table rather than a value. Where the tables
differ, pasting converts the coefficients to this file's tables.

A PNG has no coefficients at all. Its pixels have to be color converted, chroma
downsampled, transformed and quantized before a JPEG can hold them, and no
arrangement of that is lossless. What can be bounded is where the loss lands:
the selection's MCUs are handed to libjpeg's encoder configured with the damaged
file's own quantization tables and sampling factors, and the resulting blocks
are written into the coefficient array in place of the selected ones. They are
in the same currency as their neighbors. A camera's own encoder would have
rounded and downsampled a little differently, so they are not the blocks the
camera would have written. Every MCU outside the selection keeps its exact
coefficients. Pixels just outside the patch can still shift by a level or two,
because the decoder interpolates chroma across the boundary.

### Matching color against another copy

Everything the match arithmetic touches is measured in YCbCr, which is what
makes a second file usable: those are the samples a JPEG actually stores, and a
mean over them is absolute. Two copies of a photograph need no shared
resolution, quality or quantization tables for their means to be comparable. A
reference that did not come out of a JPEG at all is converted with JFIF's
full-range matrix, the same one libjpeg's decoder applies.

## Performance

`mcu-studio-cli bench` times the engine on a given file. On a 3888×2592 4:2:0
JPEG (2.5 MB), on a 4-thread x86-64 cloud VM with distribution libjpeg-turbo
2.1, best of 5 runs:

| Stage | ms |
|---|---:|
| Load (entropy decode, once per file) | 72 |
| DC offset on 3 components, whole image | 5 |
| Insert 3 MCUs mid-image | 0.8 |
| Decode the whole picture (all threads) | 24 |
| Decode one MCU row (what most edits need) | 0.6 |
| Preview a DC edit on 64 MCUs (copy, edit, decode the changed rows) | 39 |
| Write the JPEG (export only) | 85 |
| *For comparison: libjpeg-turbo's own full decode, one thread* | *70* |
| *The previous pipeline, read+edit+write per preview, before decoding* | *164* |

Previewing a DC offset or a shift, and the resync search, run off the UI
thread. Committing a step, and previewing a candidate byte edit (which re-reads
the stream), run on it at the costs above.

## Testing

`ctest` runs the Qt Test suite in `tests/`. It covers the decoder's agreement with
libjpeg, the operations' semantics and their round trips through export,
trailers and MPF rewriting, STOP/Djvu footers, bitstream maps and edits, the
resync search on synthetic damage, donor splicing, ranking and width detection,
alignment and DC estimation, the document model and project files, Exif
editing and carving. Set `MCU_STUDIO_LONG_TESTS=1` for a 40-case randomized
resync sweep.

Everything that parses untrusted bytes has a libFuzzer harness in `fuzz/`, and
CI runs each one under AddressSanitizer and UBSan on every push.

## Layout

```
src/                        the Qt 6 application and the repair core (mcu_core)
src/cli/                    mcu-studio-cli
third_party/jpegrepair/     vendored jpegrepair (modified) and the preview decoder
tests/                      the test suite
fuzz/                       libFuzzer harnesses and their seed corpus
packaging/                  desktop entry, icons, AUR PKGBUILD
installer/                  Windows NSIS installer
docs/                       screenshots, the audit and roadmap
.github/workflows/          CI and releases
```

## Contributing

Bug reports and pull requests are welcome. For major changes, please open an
issue first to discuss what you would like to change.

## License

**GNU General Public License v3.0**, see [LICENSE](LICENSE).

This tool includes a modified copy of
[jpegrepair](https://github.com/openrightorg/jpegrepair) by Don Mahurin
(BSD 3-Clause), marker-copying helpers from the Independent JPEG Group, and a
preview decoder that reimplements IJG algorithms. See
[THIRD-PARTY-NOTICES.md](THIRD-PARTY-NOTICES.md) for the notices and a list of
the changes made.
