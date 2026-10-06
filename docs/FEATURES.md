# Features

Everything MCU Studio can do, tool by tool. For why each one works the way it
does, see [How it works](HOW-IT-WORKS.md).

## Terms

| Term | Meaning here |
|---|---|
| **Block** | One 8×8 DCT data unit of one component. |
| **MCU** | The group of blocks the encoder writes together: in 4:2:0, a 16×16-pixel area holding 4 Y, 1 Cb and 1 Cr block. Everything the grid shows, selects and shifts is an MCU. |
| **Scan order / coding order** | The order MCUs (and the blocks inside them) are written in the stream. Damage spreads in this order in baseline files; progressive files are coded scan by scan instead, and the tool says so when it opens one. |
| **DC offset** | A constant added to the quantized DC coefficient of every block in scope. |
| **Restart marker** | RST0–RST7, which a camera may write every *n* MCUs. The stream is byte-aligned and the DC predictors are reset there. |
| **Lossless** | No coefficient outside an edit changes. True of every bitstream and coefficient edit here; not of *Fill from reference*, *AI fill* or *Auto color*, which quantize new content and say so. |

## Repair

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
  with the damaged file's own tables and sampling (see [the cost](HOW-IT-WORKS.md#filling-mcus-from-a-picture-that-is-not-a-jpeg)).
- **AI fill**, for damage no other copy can answer: a model makes up content
  for the selected MCUs from the rest of the picture. The selection can be in
  several pieces (Ctrl+drag adds a run, or removes one when it starts on a
  selected MCU; *Select All Damaged MCUs* starts from the damage map), and all
  of them are filled in one step. Each region is shown before and after, the
  model can be asked again, and nothing is written until you accept. The
  content is invented, not recovered, and the tool says so.
- **Damage map and provenance overlays** (Ctrl+D, Ctrl+P). One colors the MCUs
  that look damaged; the other colors each MCU by what the repair did to it:
  moved, DC-adjusted, pasted or synthesized, or never recovered at all. F3 jumps
  to the next damaged MCU, and `\` toggles the original for comparison.
- **Auto color:** a per-channel levels stretch plus a midtone-contrast curve.
  It works on pixels, so it re-quantizes every MCU, with this file's own tables
  and sampling factors. The MCU grid and every later step stay where they are.

## Headers, ransomware and other files

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

## The session

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
