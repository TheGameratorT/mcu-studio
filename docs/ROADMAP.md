# MCU Studio: audit and roadmap

## Status

Everything below was written against commit `7f67cf3`. Since then:

**Audit findings (part 1)**

| # | Status |
|---|---|
| A1 Donor splice replaced the file's own tables | **Fixed.** The splice keeps the damaged file's surviving DQT/DHT tables slot by slot, and its SOF/SOS/DRI when they survive and agree; the donor fills in the rest. |
| A2 Trailers reported as damage | **Fixed.** Data after a clean EOI is classified (MPF, motion photo, embedded JPEG, STOP/Djvu footer, unknown), kept out of the damage report, and carried into exports with the MPF index rewritten. |
| A3 Boundary heuristic ran into trailers | **Fixed.** The search stops at the image's own end and ignores the file's own header markers. |
| A4 Auto color changed sampling and moved the grid | **Fixed.** Auto color re-quantizes into the file's own tables and sampling, in memory, so the grid and later steps stay put and nothing is written in between. |
| A5 Clipboard could not tell 4:2:2 from 4:4:0 | **Fixed.** Sampling factors are compared. |
| B1 "Block" used for MCU | **Fixed** throughout the UI. "Block" now means an 8×8 block, which the new block-level shift works on. |
| B2 Progressive files | **Addressed.** The info panel and log say when scan order is not MCU order; the Scans tab lists scans and can keep the intact ones. |
| B3 Restart markers | **Fixed.** Spliced data's RST markers are renumbered from RST0 (gaps kept); the modulo-8 ambiguity is documented. |
| B4–B6, B9, B11 Wording | **Fixed** in code and README (bottom dummy row, "same quantization" rather than "the blocks the camera would have stored", auto levels / midtone contrast, the unsourced 624 KiB guess removed, stale comment). |
| B7 Performance claim | **Replaced** with measured numbers, the hardware they came from, and `mcu-studio-cli bench` to reproduce them. |
| B8 libjpeg vs libjpeg-turbo | **Fixed.** Virtual arrays are accessed one row group at a time as the memory manager expects; CMake warns without libjpeg-turbo. |
| B10 Undo history | **Fixed.** The history is saved in the project file. |
| 1.4 Oversized headers, CDELTA range, project identity, tests | **Fixed.** 400 MP cap before allocation; coefficients never clipped while editing, only brought into the writable range (AC ±1023, 11-bit DC steps) by the preview and the export; SHA-256 in projects; a Qt Test suite and libFuzzer harnesses in CI. |

**Roadmap items (part 3)**

| Item | Status |
|---|---|
| P0 | Done (above). |
| F1 bitstream map | Done: per-MCU bit positions, DC predictors and anomaly flags for sequential Huffman scans; hex view. Progressive files are not mapped. |
| F2 byte/bit edits | Done: delete/insert bytes, delete/insert bits with restuffing, flip bit, cut; recipe steps. |
| F3 sub-MCU shift | Done: single-block insert/delete in coding order. |
| F4 scope to next restart | Done. |
| F5 DC re-baselining | Covered by F4 plus the DC estimate. |
| F6 damage map | Done (overlay, F3 to jump). |
| F7 auto-align | Done for MCU shifts (seam ranking); the bit/byte-level counterpart is the resync search, with ranked, previewable candidates. |
| F8 auto DC | Done, measured on coefficient-domain block edges. |
| F9 progressive | Scan list and "keep scans up to here"; no per-scan bit maps. |
| F10 embedded previews | Done. |
| F11 donor library | Done: rank a folder by decode health. |
| F12 header merge/editor | Merge done; frame size and restart interval editable. Sampling factors and scan-header fields are not. |
| F13 width detection | Done. |
| F14 STOP/Djvu | Done: footer detection and stripping, personal ID and offline-ID hint, 150 KiB default, batch splicing in the CLI. |
| F15–F17 engine | Done: in-memory coefficients, sample-exact preview decoder, dirty-row decoding. The DC-only fast path (F17) proved unnecessary at these speeds. |
| F18 checkpoints / threading | Replay from the in-memory base is a few ms, so checkpoints were not needed; Auto color results are cached. Commits still run on the UI thread. |
| F19 viewport decoding | Not done: a full 10 MP decode takes 24 ms across 4 threads. |
| F20 parallelism | Rendering is multithreaded; donor ranking and searches run in the background. The resync search itself is single-threaded. |
| F21 benchmark | `mcu-studio-cli bench`, run in CI. |
| F22 CLI | Done: info, triage, apply, resync, splice (incl. folders), carve, extract, bench. |
| F23 batch triage | Done (dialog and CLI). |
| F24 carving | Done (GUI and CLI). |
| F25 report | Done. |
| F26 provenance | Done (overlay and report). |
| F27 requantized paste | Done. |
| F28 compare | Toggle to the original (`\`); no split or onion-skin view. |
| F29 Exif hygiene | Thumbnail refreshed, pixel dimensions fixed after a transplant, orientation shown. |
| F30 12-bit / arithmetic | Arithmetic-coded files are written arithmetic-coded. 12-bit needs libjpeg-turbo 3's API and is not done. |
| F31 keyboard search | Done (`[` `]` `{` `}`, Ctrl+Enter, Esc). |
| F32 persistent undo | Done. |
| F33 tests | Done (84 tests, plus an opt-in 40-case sweep). |
| F34 fuzzing | Done (6 harnesses, in CI). |
| F35 split MainWindow | Partly: new features live in their own files (analysis dock, dialogs, core modules); MainWindow itself was not broken up. |
| F36 CI matrix | Done: Linux, Windows, macOS, fuzzing. |
| F37 translations | Not done. |

---

This document has three parts. The first checks what the README and the
in-app text claim against what the code does. The second checks that the
terms the tool uses mean what JPEG people take them to mean. The third is a
prioritized plan for turning MCU Studio into a professional-grade, fast JPEG
recovery tool.

Everything in part 1 was checked against the source at commit `7f67cf3`.
Where a claim was tested instead of just read, the test is described.

---

## 1. Claims audit

### 1.1 Claims that are wrong (fix the code or the text)

| # | Claim | Reality | Where |
|---|-------|---------|-------|
| A1 | Donor splice at "Right after this file's own scan header": *"The donor then supplies only the tables libjpeg choked on."* | `donor::splice` copies **every** non-identity segment of the donor (all DQT, DHT, SOF, DRI, SOS). The damaged file's own tables, including any that were intact, are thrown away and replaced. | README "Donor headers" item 2; `DonorHeader.cpp` `splice()` |
| A2 | Files that open normally are treated as undamaged. | `jr::salvageScan` treats **any bytes after the EOI** as damage: `damageAt` is set when `droppedBytes > 0`. Many healthy files carry data after the EOI: MPF second images (common on Fujifilm, Sony and others), Google/Samsung Motion Photos (a whole MP4 after the EOI), and editor or vendor trailers. Every one of these opens with the warning *"Part of the picture data is missing… about N% of its picture data"*, gets flagged as a reconstruction, and loses its trailer on export. For a Motion Photo, N is often over 50%. | `JpegRepair.cpp` `salvageScan()` (clean-EOI branch); `MainWindow.cpp` around line 748 |
| A3 | "Just past the last byte pair no valid JPEG could contain" bounds the damage tightly. | `lastIllegalFf` scans the **whole file** backwards, including anything after the real EOI. It also counts `FF D8`, `FF E1`, `FF DB`, `FF C4` and `FF DA` as illegal. Any trailing JPEG (MPF preview, Exif-like trailer) or MP4 therefore pushes the "boundary" into the trailer, past the end of the picture data. The search should stop at the main image's EOI, and it should step over well-formed marker segments. | `DonorHeader.cpp` `lastIllegalFf()` |
| A4 | Auto color "re-encodes" (it costs one generation) and the steps after it still apply. | `jr_encode_rgb` calls `jpeg_set_defaults`, which always writes **4:2:0** with libjpeg's standard tables at quality 95. A 4:4:4 or 4:2:2 original therefore changes MCU size after an Auto color step. `ImageDocument::adoptRendered` never refreshes `m_info`, so the grid, the selections and every later coefficient step still use the old MCU geometry, and they land in the wrong places. The re-encode also brings back the dummy-block flattening that the replay model exists to avoid, for any insert or delete placed after it. | `jpegrepair_core.c` `jr_encode_rgb`; `ImageDocument.cpp` `render()`, `adoptRendered()` |
| A5 | The clipboard refuses blocks from a differently sampled image: *"a snapshot from a differently sampled image would shear the channels apart."* | `Clipboard::fits` compares only `blocksPerMcu` and `numComponents`. 4:2:2 (2×1) and 4:4:0 (1×2) both have 4 blocks per MCU and pass the check, so the paste shears exactly as the comment warns. The clipboard should store the per-component `h_samp` and `v_samp` and compare those. | `JpegRepair.cpp` `Clipboard::fits` |

### 1.2 Claims that are overstated or incomplete (reword)

| # | Claim | What is accurate |
|---|-------|------------------|
| B1 | "Insert / Delete **N blocks**", "Copied N block(s)", "(N blocks)" in the info panel, "Select All Blocks", and so on. | Every one of these counts **MCUs**. In 4:2:0 one MCU is 6 blocks (4 Y, 1 Cb, 1 Cr). The info line `grid X × Y (N blocks)` prints `mcuCount()`. A user coming from JPEGsnoop or JPEG-Repair Toolkit will read "insert 1 block" as one 8×8 DCT block. See part 2. |
| B2 | "Selections run in scan order… because that is the order damage propagates in." | True for **baseline, interleaved** scans. For progressive files, and for baseline files with non-interleaved scans, each scan carries one component (or one band of coefficients) in that component's own block order. Insert and delete in MCU order are then the wrong model. Progressive files open with only the word "progressive" in the info panel and no warning. |
| B3 | Restart markers: "data after a restart decodes correctly with no knowledge of what came before." | The *content* does. Placement does not: libjpeg expects RST0 first, and `jpeg_resync_to_restart` reacts to an out-of-sequence marker by inserting an empty interval (when the marker is 1–2 ahead) or skipping a segment (when it is 1–2 behind). Tested: splicing a `-restart 4` file at an RST3 makes djpeg report `found marker 0xd4 instead of RST0`. The splice should renumber the RSTs, and the text should say that placement is only known modulo 8 intervals. |
| B4 | The dummy-block explanation mentions only "a column of dummy blocks past the right edge". | `jctrans.c` also makes a **bottom row** of dummy blocks when the height is not a multiple of the MCU height. For those it copies the DC of the block above. The explanation and the code comment in `ImageDocument.h` should cover both edges. |
| B5 | Reference fill: the quantized blocks "are the blocks the file would have stored for that content". | They are the blocks **libjpeg's** encoder produces with the file's tables and sampling factors. A camera's encoder uses its own FDCT, rounding and chroma downsampling filter, so its blocks would differ in detail. Also, the patch is encoded as a separate small image, so chroma downsampling at the patch edge replicates the edge pixels instead of using the real neighbors. Say "in the file's own quantization", not "the blocks the file would have stored". |
| B6 | Auto color is "white balance (a per-channel histogram stretch)". | A per-channel percentile stretch is **auto levels per channel** (Photoshop's "Enhance per channel contrast"). It does neutralize casts, but it also moves the black point, and it is not white balance in the usual sense (scaling channels so that a reference neutral reads as neutral). In the code, `clarity()` is named after PhotoDemon's tone curve, but "Clarity" in Lightroom and PhotoDemon means **local** contrast, and this is a global curve. Rename it to `midtoneContrast`. The LUT also truncates where it should round. |
| B7 | "Performance: a three-channel correction applies in ~38 ms and a full preview takes ~69 ms" on 3888×2592. | No hardware is given. On this audit's 4-vCPU cloud container with distro libjpeg-turbo, a synthetic 3888×2592 4:2:0 q92 file (2.4 MB) measured **70 ms** for apply, **118 ms** for apply plus decode, and **58 ms** for decode alone (best of 10). The README's numbers are plausible on a fast desktop, but they should name the CPU and the libjpeg build. A commit currently re-renders on the **UI thread**: only previews run off-thread. |
| B8 | Build prerequisites: "libjpeg **or** libjpeg-turbo". | `access_all` indexes past the rows it requested from `access_virt_barray`. That is only safe when the coefficient arrays are fully in memory. This is true for libjpeg-turbo's `jmemnobs`, but IJG libjpeg builds with `jmemansi` or `jmemname` can page arrays to a backing store on large images. Either require libjpeg-turbo, or set `max_memory_to_use` high enough and request the full height. |
| B9 | 150 KiB and **624 KiB** prefixes "that STOP/DJVU and its relatives are reported to encrypt". | 150 KiB (0x25800) for STOP/Djvu is widely documented. I could not find a source for 624 KiB (0x9C000). Cite it or drop it. Related: STOP/Djvu also **appends a trailer** that holds the victim's personal ID. The tool neither detects nor strips this trailer, and A3 means it can mislead the boundary heuristic. |
| B10 | "Undo/redo works over the whole session." | It works within one run of the program. `adoptSteps` starts the history over when a `.mcup` is reopened, so undo history does not survive a restart even though the recipe does. Either say so, or persist the history. |
| B11 | The `jr_encode_rgb` comment says it is "used for the stitched output". | There is no stitching feature. The only caller is Auto color. This is a stale comment. |

### 1.3 Claims that hold

I checked these and they are accurate:

- **DC math.** `dC × Q00 / 8` is right. The JPEG FDCT gives F(0,0) = 8 × the mean of the level-shifted block, and libjpeg's coefficient arrays hold absolute quantized DC values (the DPCM is already undone). Q00 is read per component from the image.
- **Losslessness.** Untouched blocks keep their exact coefficients through `jpeg_read_coefficients` and `jpeg_write_coefficients`, with no requantization. The file is re-serialized (standard Huffman tables, and a progressive input comes out sequential), and the README says so.
- **Dummy-block refill.** `jpeg_write_coefficients` refills the dummy blocks on every write. The replay-from-original model does avoid compounding that, provided no Auto color step sits before an insert or delete (see A4).
- **Illegal FF pairs.** In random data an illegal FF pair occurs about once every 267 bytes, which matches "roughly every 250".
- **Metadata.** Markers are copied with `JCOPYOPT_ALL`. The donor's Exif, XMP and COM are stripped, and its JFIF, Adobe and ICC segments are kept. The damaged file's own Exif is carried over when it survives ahead of the splice point.
- **Timestamps.** The exported file gets the original's modification and access times (and birth time where the OS allows setting it).
- **Previews.** They are debounced (90 ms) and rendered with `QtConcurrent`, and stale results are discarded by a generation counter.

### 1.4 Robustness gaps found while reading

- **Oversized dimensions.** A corrupt or mismatched SOF can declare 65535×65535. `jpeg_read_coefficients` will then try to allocate many gigabytes. Set `max_memory_to_use`, cap the pixel count before reading, and report the problem.
- **Unclamped CDELTA.** CDELTA is not clamped in the core. A DC beyond what the Huffman coder can express (11-bit difference category for 8-bit data) aborts the whole render with `JERR_BAD_DCT_COEF`. The UI clamps the delta to ±2047, but the DC values it adds to can already be large.
- **Weak project identity.** A `.mcup` identifies its source by **byte size only**. Two different damaged files of the same size (STOP/Djvu output often has identical sizes) would replay one file's recipe on the other without complaint. Store a SHA-256 hash.
- **No tests or fuzzing.** CI only checks that the app links and answers `--version`. This tool exists to parse hostile, malformed input (`salvageScan`, `walkMarkers`, the project JSON, libjpeg via the core), so that is the riskiest gap in the codebase.

---

## 2. Terminology check

| Term as used | Standard meaning | Verdict |
|---|---|---|
| **MCU** (minimum coded unit) | The smallest group of data units in an interleaved scan: for 4:2:0, 16×16 px holding 4 Y + 1 Cb + 1 Cr blocks. | ✅ Correct where it says MCU. |
| **Block** | One 8×8 DCT data unit of one component. | ❌ The UI uses it for MCU almost everywhere (B1). Rename to "MCU", and keep "block" for 8×8 units, which part 3 needs anyway for sub-MCU editing. |
| **DCT coefficients / quantized DC** | Exactly that. | ✅ |
| **DC offset / "cdelta"** | Adding a constant to the quantized DC. | ✅ The README rightly avoids "cdelta" as a user-facing term. |
| **Lossless** | No information lost. | ✅ For coefficient operations. ⚠️ "Lossless" should never be applied to reference fill or Auto color, and the README does not. |
| **Scan order** | MCU raster order within an interleaved scan. | ⚠️ Correct for baseline interleaved scans only (B2). |
| **Restart marker / interval** | RST0–RST7, cycling, every *Ri* MCUs, where DC predictors reset and the bitstream is byte-aligned. | ✅ Defined correctly. ⚠️ Sequence handling, see B3. |
| **Byte stuffing** | `FF 00` inside entropy-coded data. | ✅ |
| **Donor header / transplant** | The industry term for this technique (JPEG-Repair Toolkit and others use "reference file"). | ✅ |
| **Entropy-coded stream / "shift the stream"** | The Huffman bitstream. | ⚠️ Insert and delete shift **decoded MCUs in the coefficient array**, not bits in the stream. Real desync damage is bit-level (part 3, F1). The README phrase "shift blocks… through the entropy-coded stream" should say "through the image in coding order". |
| **White balance** | Channel gains that neutralize a reference white. | ⚠️ See B6: the feature is "auto levels". |
| **Clarity** | Local midtone contrast (an unsharp mask with a large radius). | ❌ Code-level misnomer (B6). |
| **Salvage / Truncate / Read through** | Product terms. | ✅ Explained well in the UI. |
| **Progressive / baseline** | SOF2 versus SOF0/SOF1. | ✅ Detected and shown, but not acted on (B2). |

---

## 3. Feature plan

The priorities below follow one idea. Today MCU Studio repairs damage at **MCU
granularity, after libjpeg has already interpreted the corrupt bitstream**.
Professional tools work one level lower, on **bits and bytes of the entropy
stream**, and automate the search that MCU Studio currently leaves to the
user's eye. The biggest gains come from adding that layer, then automating
it, then making it fast.

### P0: correctness fixes (before new features)

1. **Trailing-data handling (A2, A3).** Treat bytes after a clean EOI as a trailer, not as damage. Identify MPF (`APP2 "MPF\0"` index), Motion Photo (XMP `GCamera:MotionPhoto` or `MicroVideo`, or an `ftyp` box), the STOP/Djvu trailer, and unknown trailers. Show the trailer in the info panel, keep it on export by default (rewriting the MPF offsets), and exclude it from every heuristic.
2. **Auto color geometry (A4).** Re-encode with the original's sampling factors and quantization tables (`jpeg_copy_critical_parameters` plus RGB input), or refuse to place geometry-dependent steps after an Auto color step. Refresh `m_info` after every render, and assert that the grid matches it.
3. **Clipboard sampling check (A5).** Store the per-component `h_samp` and `v_samp` and compare them.
4. **Rename block to MCU in the UI (B1).** Keep "block" for 8×8 units.
5. **Progressive handling (B2).** On open, warn that insert and delete assume MCU order, and offer **Convert to sequential (lossless)**. This is a `jr_apply` with no ops, which already produces a baseline file. For a *damaged* progressive file, see F9.
6. **Donor tables (A1).** Fix the README wording, and implement **header merge** (F12).
7. **Restart handling (B3).** Renumber RST markers when splicing, and document the modulo-8 ambiguity.
8. **Hardening (1.4).** Add memory caps, the CDELTA range check with a clear message, a SHA-256 hash in `.mcup`, and require libjpeg-turbo (B8).
9. **README fixes.** Correct B4–B11.

### P1: bitstream-level repair (the core professional capability)

**F1. Bitstream map.** Use a custom Huffman decoder (or libjpeg-turbo's `jdhuff` driven by our own source manager) that records, for every MCU, its **bit offset in the file**, its DC values per component, and any decode anomaly (an invalid code, a coefficient index past 63, a marker hit, or an AC run that overflows). This is what JPEGsnoop's MCU detail and JPEG-Repair Toolkit's bit view are built on. With it, clicking an MCU shows the file offset; a hex pane shows the bytes; and the map shows the first MCU where decoding went wrong, which is usually a few MCUs **before** the damage becomes visible.

**F2. Byte and bit edits as recipe steps.** New step kinds that edit the source bytes before decoding: **delete N bytes at offset**, **insert N zero bytes**, **flip bit**, and **delete or insert N bits** (re-stuffing `FF 00` after the edit). A run of lost bytes, which is the common case on flash media and in partial overwrites, is fixed exactly by deleting or inserting bytes at the damage point: the stream re-syncs **bit-exactly**, and the MCUs after it decode with the right content, the right position, and usually the right DC. Today the same damage needs "insert 12 MCUs + 13 DC offsets" as an approximation. Steps stay in the replayable recipe (byte edits first, then coefficient ops).

**F3. Sub-MCU (block-level) shift.** Insert or delete of a single **8×8 block** within a component. A desync often loses or gains part of an MCU, and MCU-granular insert cannot express that, which leaves the luma and chroma of everything after it one block out of phase.

**F4. DC drift scope "to the next restart marker".** A DC predictor error persists until the next RST, not to the end of the image. With F1 the scope can be exact: from the selected MCU to the end of its restart interval.

**F5. Per-component DC re-baselining.** After a desync, re-derive every DC from the **decoded DC differences** with a corrected starting predictor, instead of adding one constant to everything. This fixes drift that changes again at each further hiccup.

### P2: automation (turning the search into suggestions)

**F6. Damage detector.** Score every MCU (from F1 anomalies plus image statistics: DC discontinuity against the neighbor above, AC energy outliers, chroma saturation spikes, and edge mismatch across the MCU-row seam) and draw a heat-map overlay. Add **Jump to next damaged MCU**.

**F7. Auto-align.** At a selected damage point, try insert or delete of *k* MCUs (and, with F2, *k* bytes or bits) over a range. Score each candidate by how continuous the content is across the seam between that MCU row and the one above (a sum of absolute differences of boundary pixels, computed in the coefficient domain if F15 is in place). Show the top candidates as thumbnails. This turns the README's "try 17 blocks, look, try 18" into one click.

**F8. Auto DC.** For a damaged region, solve for the per-component offset that minimizes the luma and chroma step across the region's top and left boundaries. This is the same objective as F7 but over integer DC deltas, with a closed-form mean of the boundary differences, rounded to the Q00 step. It gives a starting value that the user can then fine-tune, instead of hand-picking two MCUs.

**F9. Progressive recovery.** Parse each scan separately. Report which scans are intact, and offer **drop the damaged refinement scans**: decode with the DC and the first AC scans only, giving a lower-detail image with no corruption artifacts, which is often the best a damaged progressive file can give. Add per-scan bit maps for F1.

**F10. Embedded preview extraction.** Pull the Exif IFD1 thumbnail, MPF large previews, and vendor maker-note previews (Canon, Nikon and Sony embed a 1–2 MP JPEG). Offer them automatically as the reference for color (`ReferenceColorDialog`) and fill (`ReferenceFillDialog`). They are often intact when the main image is not, because they sit earlier in the file than the main scan. For STOP/Djvu, where the first 150 KiB are gone, they are gone too, so this mainly helps other damage types.

### P3: ransomware and donor workflow

**F11. Donor library and auto-match.** Scan a folder of good photos and index them by camera model, resolution, DQT hash, DHT hash, sampling, and restart interval. For each damaged file, try every compatible donor and rank them by **decode health** (F1 anomaly rate on the first N KB of entropy data). Today the donor is guessed and judged by eye; a mismatched Huffman table shows up as an immediate anomaly storm, so ranking is cheap and reliable.

**F12. Header merge and header editor.** Keep the damaged file's own intact segments (Exif, ICC, DQT, DHT) and borrow only what is missing or broken. Add a structured editor for SOF width and height (portrait versus landscape from the same camera is a common donor mismatch, so add a **swap W/H** button), sampling factors, DRI, and the SOS component and table selectors.

**F13. Width detection.** When the donor's dimensions are wrong, find the true MCU row length by autocorrelating the decoded image across candidate strides. This is the classic fix for "image looks sheared".

**F14. STOP/Djvu module.** Detect the appended trailer and strip it before any heuristic. Show the personal ID it contains, and if the ID matches the pattern used for **offline keys**, tell the user that a published decryptor (for example Emsisoft's STOP Djvu decryptor) may restore the file completely, which beats any repair. Splice at exactly 150 KiB by default for files that match. Batch-apply one donor and splice point across a folder.

### P4: performance architecture

Measured above: one preview costs a **full Huffman decode, a full Huffman encode, and a full pixel decode** (≈70 + 58 ms here), and every commit, undo or redo replays the whole recipe on the UI thread.

**F15. Parse once, keep coefficients in memory.** Hold the original's coefficient arrays (`int16`, about 15 MB for 10 MP 4:2:0) after one `jpeg_read_coefficients`. Apply ops directly to a working copy, which is already how the core works internally, just not kept between calls. This removes the per-preview entropy decode and encode, so the Huffman encode is needed only on export.

**F16. Direct IDCT for previews.** Decode the preview straight from the coefficient arrays with our own IDCT and upsampler, bit-matched to libjpeg-turbo's `jidctint` and h2v1/h2v2 "fancy" upsampling, and use SIMD through a small kernel or libjpeg-turbo's exported routines where they are reachable. Re-run the IDCT only on **dirty MCUs**: a DC offset over a selection touches only those MCUs, plus one-MCU neighbors for chroma upsampling. A 3-channel DC tweak on a small selection goes from about 120 ms to well under 5 ms.

**F17. DC-only fast path.** A CDELTA changes only DC, so the preview can add `dC × Q00 / 8` to the affected samples directly, before the color conversion, with no IDCT at all. This is exact except for the clamping at 0 and 255.

**F18. Checkpointed replay.** Cache the coefficient state after each step (copy-on-write per MCU row) so that toggling or undoing step *k* replays from *k−1*, not from the original. Move commit, undo and redo off the UI thread, with the same generation-stamp pattern the preview already uses.

**F19. Viewport-aware decoding.** For overviews, decode at 1/2, 1/4 or 1/8 scale using DC only (the 1/8 scale is just the DC values) and refine visible tiles at full resolution. Use `jpeg_crop_scanline` and `jpeg_skip_scanlines` where the full-libjpeg path remains.

**F20. Parallelism.** Run F7 and F11 candidate searches on a thread pool. Each candidate is independent, and with F15 and F16 one costs milliseconds.

**F21. Benchmark harness.** Add a `bench/` target with fixed test images (10 MP and 24 MP, 4:2:0, 4:2:2 and 4:4:4) that records apply, preview and export times in CI, so the README's numbers come from a reproducible run on named hardware.

### P5: workflow and professional features

- **F22. Headless CLI** (`mcu-studio-cli`). Apply a `.mcup` to its image, batch-export a folder, run donor auto-match, and print a JSON health report. This is needed for lab work and scripting, and it is also what the test suite drives.
- **F23. Batch queue.** Open a folder, triage every file into healthy, trailer only, header lost, truncated, or desync, with thumbnails and a damage score, and work through the queue.
- **F24. Carving.** Open a raw disk image or an unknown file and list every SOI to EOI candidate, with fragment validation by decode health. This also extracts the full-size JPEG embedded in camera RAW files (CR2, NEF, ARW, DNG), which is often the best surviving copy of a damaged photo.
- **F25. Recovery report.** On export, write a sidecar or PDF listing the source hash, output hash, every step, the percentage of MCUs touched, the percentage synthesized (fill) versus recovered, and the donor used. This matters for forensic and evidential work, where the output must say what is original.
- **F26. Provenance overlay.** Color MCUs by origin: original, moved (insert, delete or copy), DC-adjusted, synthesized (fill), or gray (no data). Optionally embed the map in XMP.
- **F27. Requantized cross-file paste.** When quantization tables differ, convert coefficients with `round(c × Qsrc / Qdst)` instead of refusing or pasting raw. The result is lossy only where Qdst is coarser than Qsrc, which is far better than pixel re-encoding.
- **F28. Compare view.** A split or onion-skin view against the original, against the previous step, and against a reference image aligned at the MCU grid.
- **F29. Exif hygiene on export.** Regenerate or drop the IFD1 thumbnail (it shows the damaged image), fix `PixelXDimension` and `PixelYDimension` after a header edit, and keep the Orientation tag. The grid is shown in stored orientation, which is correct for editing, but add a "displayed orientation" hint.
- **F30. 12-bit and arithmetic-coded JPEGs.** Use libjpeg-turbo 3's 12-bit API (`jpeg12_*`), and allow arithmetic-coded output when the input used it.
- **F31. Keyboard-driven search.** Map `[` and `]` to insert or delete one MCU at the cursor as a live (uncommitted) preview, `Shift+[` and `Shift+]` to one block (F3), and `Enter` to commit. That gives the "try 17, try 18" loop without the mouse.
- **F32. Persistent undo history** in the `.mcup` (B10).

### P6: engineering quality

- **F33. Test suite** (Qt Test or Catch2, driven through the core and the CLI):
  - golden images with *synthetic* damage (byte deletion, bit flips, zeroed sectors, 150 KiB encryption, truncation, trailers), each with an expected repair recipe;
  - round-trip invariants: coefficients outside every op's scope are bit-identical, and an empty recipe re-emits identical coefficients;
  - regression tests for A2–A5.
- **F34. Fuzzing.** libFuzzer or AFL++ on `salvageScan`, `walkMarkers`, `splicePoints`, the project reader, and `jr_apply`, under ASan and UBSan, in CI. The input is hostile by definition.
- **F35. Split `MainWindow.cpp`** (2,280 lines) into panels (DC, block operations, clipboard, steps), an action controller, and a render service. F15–F18 need a render service anyway.
- **F36. CI matrix.** Add macOS (the README lists brew instructions but CI does not build it). Run the unit tests and fuzzers for a short budget on each push.
- **F37. Translations.** All strings already go through `tr()`; add a `.ts` pipeline.

---

## 4. Suggested sequencing

1. **P0** (one release): the correctness and wording fixes, plus F33's regression tests for them.
2. **F15 → F16 → F17 → F18**: the render service. Every later feature (F1, F6–F8, F11) needs fast, in-memory, repeatable decoding, so this comes before them.
3. **F1 → F2 → F3 → F4**: the bitstream layer. This is the single biggest jump in what the tool can actually recover.
4. **F6 → F7 → F8**: automation built on the bitstream layer.
5. **F11, F12, F14, F22**: the ransomware and batch workflow, which is where most real-world demand is.
6. The remaining items by user demand.
