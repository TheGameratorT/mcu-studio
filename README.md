# MCU Studio

**Repair damaged JPEGs by editing their DCT coefficients directly, with no
re-encoding and no quality loss.**

[![C++17](https://img.shields.io/badge/C%2B%2B-17-00599C?style=for-the-badge&logo=cplusplus&logoColor=white)](https://isocpp.org/)
[![Qt 6](https://img.shields.io/badge/GUI-Qt%206-41CD52?style=for-the-badge&logo=qt&logoColor=white)](https://www.qt.io/)
[![License](https://img.shields.io/badge/License-GPL--3.0-green?style=for-the-badge)](LICENSE)

A Qt 6 desktop application for repairing JPEGs damaged by ransomware encryption
(STOP/DJVU and its relatives), truncation, or bit rot: the kinds of damage that
shift the entropy-coded stream out of alignment or wreck a component's DC level.

MCU Studio works at the level a JPEG is actually stored in, the **minimum coded
unit** and the **DCT coefficients** inside it. Corrections are applied to
`jpeg_read_coefficients` arrays and written straight back out, so blocks you did
not touch keep their exact coefficients and decode to precisely the pixels they
did before. Repairing an image never costs a re-encode generation.

## Why work in the coefficient domain?

Ordinary image editors cannot help with these files. They decode to pixels,
which means every save is a fresh lossy generation over the whole picture, and
they refuse to open the badly damaged files in the first place. The operations
that actually fix a broken JPEG (shift a component's DC level, insert or delete
blocks to re-align a drifted scan, splice on a working header) have no
expression in the pixel domain at all.

The command-line tools that do work at this level exist, but they ask you to
guess: you name a block range and a delta, run the tool, open the result, and
look. MCU Studio puts that loop in one window, with the grid you are selecting
in, a live preview of the correction, and an editable list of the steps.

## Features

- **Donor headers, for files that will not open at all.** Ransomware, a bad
  sector, or a truncated copy usually takes the front of the file (the
  quantization and Huffman tables, the frame header, the scan header), and
  nothing in the surviving picture data can replace them. A camera writes the
  same header into every frame it shoots at the same settings, so another
  photograph from the same card can lend its own. Point the tool at a sibling
  and it splices that header onto the damaged file's data. It suggests one from
  the same folder, ranks its guesses at where the surviving data resumes, and
  previews each one, so a wrong guess costs a click. The donor's Exif, XMP and
  comments are dropped on the way through (a rescued photograph should not
  inherit another shot's date or GPS fix), while the damaged file's own Exif is
  carried over whenever it survived.
- **MCU grid with scan-order selection.** Select a run of blocks the way you
  select text: click and drag, Shift+click to extend. Damage follows the
  entropy-coded stream rather than the picture's geometry, so a wrapping run is
  usually what you want, not a rectangle.
- **Color correction (`cdelta`) per Y/Cb/Cr component, with live preview.** The
  delta is calibrated against the image's own DC quantization step, and the
  panel shows what shift it produces in 0-255 sample terms.
- **Three correction scopes**, all lossless: selected blocks only, selection to
  end of image, whole image.
- **Automatic color matching.** Pick a reference block showing good color and a
  target block showing the damage, and the tool derives the deltas. It warns
  when either block has clipped samples (the match will fall short) or when the
  two differ so much they probably are not the same content.
- **Reference color from another copy of the photograph.** A transplanted header
  can leave nothing to match against *inside* the file: the borrowed tables never
  described this data, so the picture can be the wrong color from its very first
  block. Point the tool at any surviving copy instead (a thumbnail, a phone
  gallery cache, a backup, a copy someone was sent, JPEG or not), drag a box over
  the matching content, and the mean under it becomes the reference. Color is
  what survives being shrunk, so a small or soft copy does the job, and only
  three numbers cross over: none of that file's pixels reach the repair. The box
  can be dropped where the target block falls in that copy, scaled for its size,
  and the tool says so when a copy is grayscale and therefore has no chroma to
  lend. A reference taken this way outlives every edit, because unlike a block of
  the image under repair, it does not describe the render.
- **Block operations:** insert, delete, and copy MCU blocks, the classic fix for
  a stream that has lost or gained bytes.
- **Filling blocks from another copy of the photograph.** Every repair above
  moves coefficients the file already has, which works while the picture data
  survives somewhere in it. Where it does not (an overwritten stretch, a hole a
  truncation left, blocks that decode to nothing), another copy can supply the
  content: a re-render, an export, a backup, a frame from a video, a PNG an
  earlier recovery attempt produced. Any format the system can read will do,
  because what comes across is pixels rather than blocks. See
  [below](#filling-blocks-from-a-picture-that-is-not-a-jpeg) for what that costs
  and where the cost lands.
- **Auto color:** white balance plus midtone contrast. This one is a pixel edit,
  so it re-encodes, and the tool says so before it runs.
- **Metadata and dates survive the repair.** Exif, ICC, XMP and comment markers
  are carried into the output (including through the one lossy operation, auto
  color), and the saved file is stamped with the original's modification and
  access times, so a rescued folder still sorts by when the photos were taken.
- **The repair is a list, not a result.** Every correction lands as a step in a
  side panel, and the image you see is the original file with the ticked steps
  replayed over it in a single pass. Untick one to see the picture without it,
  reorder them, drop one from the middle, and the rest still apply. Undo/redo
  works over the whole session, and clearing the list gives back the original.

  This is not only convenient. libjpeg's compressor fills the dummy blocks that
  pad the last MCU column with a DC-only copy of their neighbour, so every
  re-encode flattens one 8-pixel block column per MCU row. Those blocks are
  outside the visible image, but insert and delete shift the stream *through*
  them, so under a save-and-reapply model a second insert would drag the
  flattened blocks into view as a stripe of detail-less squares. Replaying from
  the original keeps the session to one encode and the hole never opens.

## Installing

### Arch Linux

```sh
paru -S mcu-studio     # or your AUR helper of choice
```

The `PKGBUILD` also lives in [`packaging/aur`](packaging/aur).

### Windows

Download `McuStudio-<version>-Setup.exe` from the
[latest release](https://github.com/TheGameratorT/mcu-studio/releases). It is a
single installer carrying the application and its Qt runtime, and it adds an
"Open with MCU Studio" entry to the JPEG context menu without taking over as
the default handler. See [`installer/`](installer/README.md) for how it is built.

## Building

### Prerequisites

- CMake 3.21+
- A C++17 compiler
- Qt 6.2+ (`Widgets`, `Concurrent`)
- libjpeg or libjpeg-turbo, with development headers

<details>
<summary>Installing dependencies</summary>

```sh
# Arch
sudo pacman -S cmake qt6-base libjpeg-turbo

# Debian / Ubuntu
sudo apt install cmake qt6-base-dev libjpeg-dev

# Fedora
sudo dnf install cmake qt6-qtbase-devel libjpeg-turbo-devel

# macOS
brew install cmake qt libjpeg-turbo

# Windows (MSYS2 MINGW64)
pacman -S mingw-w64-x86_64-{gcc,cmake,ninja,qt6-base,qt6-tools,libjpeg-turbo}
```

</details>

### Build and run

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build
./build/bin/mcu-studio [image.jpg]
```

## How it works

`cdelta COMP dC` adds `dC` to the **quantized** DC coefficient of every block in
scope. The dequantized DC therefore moves by `dC × Q00`, and because the JPEG DCT
normalizes the DC term to eight times the block's mean sample, every sample in
the block moves by:

```
dC × Q00 / 8
```

`Q00` is read from the image's own quantization tables and differs per component
and per quality level. It is not a constant, which is why the tool reads it
rather than assuming `dC / 8`.

Insert and delete shift blocks forward or backward through the entropy-coded
stream from the selected block onward, re-aligning a scan that has drifted.
Copy replaces each selected block with one a fixed number of MCUs away, and the
clipboard lifts a run of whole MCUs out and writes it back somewhere else.

### Donor headers

A transplant is the donor's bytes from `SOI` through the end of its `SOS`
segment, followed by the damaged file's bytes from a chosen offset on. Choosing
that offset is the whole problem, because the damage has no marker at its end.
The tool offers, in order:

1. **Byte 0**, the default. It drops nothing and assumes nothing, and it is
   right whenever the header was lost rather than overwritten: a zeroed front,
   a carved fragment, a file that starts mid-scan.
2. **Right after the file's own scan header**, when part of the header
   survived. The donor then supplies only the tables libjpeg choked on.
3. **The first restart marker past the damage.** These are the ideal entry
   points, because the encoder flushed to a byte boundary and reset its DC
   predictors at each one, so data after a restart decodes correctly with no
   knowledge of what came before. Many cameras write none, and the option says
   so when it is not available rather than quietly vanishing.
4. **Just past the last byte pair no valid JPEG could contain.** Inside a scan
   every `FF` is stuffed (`FF 00`), a restart marker, padding, or the `EOI`.
   Encrypted or random bytes break that rule roughly every 250 bytes, so the
   last violation in the file bounds the damage tightly.
5. **150 KiB and 624 KiB**, the prefix lengths STOP/DJVU and its relatives are
   commonly reported to encrypt.

None of these can be verified from the bytes, so each is judged by decoding it:
the dialog splices, decodes and shows the result, and refuses to open a
combination that does not decode at all. A donor whose tables do not match will
usually decode into confetti rather than fail outright, which is why the
preview, not a compatibility check, is the thing standing between you and a bad
transplant.

### Filling blocks from a picture that is not a JPEG

Copying MCUs between two JPEGs can be lossless, and within one file it always
is: coefficients are lifted and written back as integers, and nothing
dequantizes them on the way. Across two files it holds only where the
quantization tables and sampling factors match, since a coefficient is an index
into a table rather than a value, and the same number means a different amount
under someone else's tables. That is what the clipboard's cross-file warning is
about.

A PNG has no coefficients at all, so nothing can be lifted from it. Its pixels
have to be color converted, chroma downsampled, transformed and quantized before
a JPEG can hold them, and no arrangement of that is lossless. What can be
bounded is where the loss lands:

1. The selection's bounding box is expanded to whole MCUs, which it already is,
   so the patch's block boundaries fall exactly on the file's own.
2. That rectangle of the reference is handed to libjpeg's encoder configured
   from the damaged file's header (its quantization tables, its sampling
   factors, its color space) and the quantized blocks are read straight back
   out. They are the blocks the file would have stored for that content.
3. Those blocks are written into the coefficient array in place of the selected
   ones, and the stream is re-emitted exactly as any other repair re-emits it.

Step 3 is the ordinary coefficient-domain path, so every block the selection
does not name keeps its exact coefficients. That is checkable rather than
claimed: reading every MCU before and after a fill shows changes only inside the
selection, and zero outside it.

Pixels just outside the patch can still shift by a level or two, and this is not
a re-encode leaking. Chroma is stored at half resolution, so the decoder
interpolates each chroma sample across its neighbours on the way out, and a
changed block at the boundary therefore changes the pixel column beside it. The
coefficients there are untouched: it is the decoder reading a new neighbour.

### Matching color against another copy

Everything the match arithmetic touches is measured in YCbCr, which is what
makes a second file usable: those are the samples a JPEG actually stores, and a
mean over them is absolute. Two copies of a photograph need no shared
resolution, quality or quantization tables for their means to be comparable,
which is just as well, since a thumbnail shares none of them. A reference that
did not come out of a JPEG at all is converted with JFIF's full-range matrix,
the same one libjpeg's decoder applies.

What geometry cannot do is find the content for you. The tool will scale the
target block's position into the other copy and put the box there, but a
transplanted header leaves the stream shifted by an unknown number of MCUs, so
until insert and delete have put it back, the block at a given position is
showing something from elsewhere in the picture. The box is meant to be dragged
onto matching content by eye, with the mean and its color re-read as it moves.

## Performance

The repair code is linked in as a library rather than driven as a subprocess, so
a whole operation list is applied to a single coefficient read instead of one
process launch and one temporary-file round trip per operation. On a 3888×2592
image, a three-channel correction applies in ~38 ms and a full preview (apply
plus decode) takes ~69 ms. Previews are debounced and rendered off the UI
thread, so dragging a slider stays responsive.

## Layout

```
src/                        the Qt 6 application
third_party/jpegrepair/     vendored jpegrepair, built as a library
packaging/                  desktop entry, icons, AUR PKGBUILD
installer/                  Windows NSIS installer
.github/workflows/          CI and releases
```

## Contributing

Bug reports and pull requests are welcome. For major changes, please open an
issue first to discuss what you would like to change.

## License

**GNU General Public License v3.0**, see [LICENSE](LICENSE).

This tool includes a modified copy of
[jpegrepair](https://github.com/openrightorg/jpegrepair) by Don Mahurin
(BSD 3-Clause) and marker-copying helpers from the Independent JPEG Group.
See [THIRD-PARTY-NOTICES.md](THIRD-PARTY-NOTICES.md) for the notices and a list
of the changes made.
