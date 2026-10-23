# Development notes

Where serconv stands and how it got there. The README says what the program
does; this file records the progress and the reasons behind the decisions, so
that work can be picked up again without rediscovering them. What is left to
do is in `TODO.md`.

Last updated: 2026-10-23, at version 0.1.0.

## Status at a glance

| Area | State |
|---|---|
| SER writer (header, frames, UTC trailer, >4 GB) | Working; layout checked byte by byte, a 5.4 GB file verified |
| MLV reader (own code) | Working on generated clips: 10/12/14/16-bit, LJ92, spanned files, out-of-order frames, dropped and interrupted frames; bit-exact with FFmpeg's MLV demuxer |
| LJ92 decoder (own code) | All 7 predictors, 1-4 components, restarts, point transform; reads liblj92's streams |
| CRM / CR3 rolls (LibRaw) | Built; ISO media timing parsed from a real Canon R6 CR3 and a synthetic CRM. **No real CRM converted yet** |
| ProRes RAW (FFmpeg 9.0.2) | Built; the reader is tested on MLV through FFmpeg. **No real ProRes RAW converted yet** |
| DNG / CinemaDNG / still sequences (LibRaw) | Working on generated DNGs (all four Bayer patterns, lossless JPEG, monochrome) and a real iPhone 12 Pro ProRAW DNG |
| Levels (scale, shift, raw; 8/16-bit) | Working, unit tested |
| Fixed crop with Bayer pattern change | Working |
| Planet tracking (`--center`) | Working on generated scenes with a moon next to the planet |
| Timing (MLV, CRM, DNG, MOV; `--utc-offset`, `--start-time`, `--fps`) | Working on generated files |
| Parallel decoding | Working; output identical for 1 and 4 threads |
| Linux build (GCC 13), FFmpeg 6.1.1 and 9.0.2, without FFmpeg | Tested, all tests pass |
| Windows | A MinGW-w64 cross build passes all tests under Wine 9.0, non-ASCII file names included; the MSVC build is **not yet run** |
| macOS | Workflow written, **not yet run** |
| CI and release workflows | Written after dngconv's, **not yet run** on GitHub |

## History

### 0.1.0 (2026-10-23): first version

The question that started it: which RAW video formats used for planetary
imaging lack an open converter to SER? The answer was mostly the cinema and
mirrorless formats: Canon Cinema RAW Light, ProRes RAW, Magic Lantern MLV and
CinemaDNG, each of which needed vendor tools or several programs before a
stacker could read the frames.

Built in this order:

1. **Research.** SER conventions from SER Player's source (Chris Garry, the
   PIPP author) and Siril's documentation; FFmpeg's ProRes RAW decoder, MLV
   demuxer and raw video decoder; LibRaw's CR3/CRM parser; MLV App's MLV
   definitions and LJ92 coder; Laurent Clévy's CR3 description for the CTMD
   records.
2. **SER writer and reader**, time conversion (SER ticks), the frame model.
3. **Readers**: MLV with LJ92, LibRaw for CRM/CR3/DNG/stills, FFmpeg for MOV.
4. **Processing**: levels, crop, planet tracking, frame selection, timing,
   sidecar, the ordered multi-threaded pipeline.
5. **Tests** with generated data, then cross-checks against independent code
   (below), then the large-file test.
6. **Independent review** by a separate agent that had not written the code.
   Its findings, all fixed with a test each:
   - `serconv .` wrote `..ser` inside the folder; folder names are now
     normalized (`.`, `./`, `seq/.`, `../seq` all give `seq.ser` next to it).
   - `--utc-offset` was also subtracted from times that were already UTC
     (Canon and DNG files with an EXIF time zone). It now applies to camera
     clocks only; a recorded zone also gives the header's local time.
   - The TIFF DateTime (the file's date, which editors rewrite) was used like
     DateTimeOriginal. Now: DateTimeOriginal, then DateTimeDigitized, then
     DateTime with a warning; LibRaw's timestamp only for non-TIFF files.
   - Only the first CR3 of a list was checked for a RAW burst roll; every CR3
     is now checked (from the ISO media boxes, without decoding).
   - With several inputs, only serconv's own errors were caught per input; an
     out-of-memory or file system error ended the batch. Two inputs writing
     the same SER (same name, different folders) are now refused up front,
     before anything is written.
   - A failing `fclose` (full disk, network share) went unnoticed; the SER and
     sidecar are now closed with a check.
   - A single failing input printed its error twice.
   - The LJ92 decoder read one byte past an empty scan header (seen with
     AddressSanitizer once the test buffer had no spare capacity).
   - The FFmpeg reader leaked its contexts when opening failed.
   - Messages: a damaged MLV block is reported apart from an interrupted
     recording; a spanned part (`A001.M00`) given alone is converted through
     its `.MLV` instead of "no input files".

## Decisions

### LibRaw is built from source

LibRaw rejects CR3-family files over 2 GB (`LIBRAW_MAX_CR3_RAW_FILE_SIZE`
defaults to `LIBRAW_MAX_NONDNG_RAW_FILE_SIZE`, 2147483647). A Cinema RAW
Light clip from an R5 passes that in a few seconds, so the vcpkg, Homebrew and
distribution packages are unusable for CRM. LibRaw's CR3 parser reads `co64`
chunk offsets into 64-bit fields and its file streams use `fseeko` and
`_fseeki64`, so raising the macro is the only change needed. `cmake/LibRaw.cmake`
fetches the 0.22.2 tag, compiles every `src/**/*.cpp` except the `*_ph.cpp`
placeholders (the same set as LibRaw's `Makefile.dist`), defines
`LIBRAW_NODLL` and the raised limit, and links `ws2_32` on Windows.

### The SER LittleEndian field is written as 0

The SER specification says LittleEndian = 1 means little-endian 16-bit data.
The early writers did the opposite, and SER Player ("little_endian == 1 means
Big endian for some reason"), PIPP, FireCapture, SharpCap, Siril and
AutoStakkert! all follow them. serconv writes little-endian data with 0 and
reads 1 as big-endian, like those programs.

### Pixel depth 16 by default

Some readers accept only 8 and 16 bits per pixel (FTYS does; SER Player and
Siril scale 9 to 15). The default `--levels scale` therefore writes 16 bits
with black at 0. `--levels raw` keeps the sensor's values and depth for those
who calibrate with darks later.

### Crops keep the Bayer pattern

`--center` places its crop on even pixels, so every frame of a tracked SER
has the same pattern and one ColorID describes them all. A `--crop` at odd
offsets is allowed and changes the ColorID accordingly (RGGB seen from (1,1)
is BGGR). FFmpeg's `apply_cropping` is switched off for the same reason:
ProRes RAW's RecommendedCrop has odd margins.

### Times

All readers report times relative to an origin plus the origin's absolute
time and which clock it is on (camera or UTC), and the camera's time zone
when the file records one. The conversion turns camera clocks into UTC with
`--utc-offset` (camera minus UTC, default 0); UTC sources are used as they
are, and `--start-time` replaces either. The header's local DateTime uses the
recorded zone, else `--utc-offset`.

Sources: MLV RTCI + VIDF timestamps; CRM/CR3 CTMD type 1 records (1/100 s,
camera clock) with the zone from the EXIF OffsetTimeOriginal in the CMT2 box,
frame times from the CRX track's `stts`; MOV creation_time + frame pts; DNG
EXIF DateTimeOriginal with SubSecTimeOriginal/OffsetTimeOriginal (then the
Digitized and plain DateTime groups) and the CinemaDNG FrameRate tag. A Canon
R6 CR3 showed the `mvhd` creation time is UTC (10:21:21) while EXIF has
12:21:21.92 +02:00.

MOV creation_time counts as a camera clock although QuickTime defines it as
UTC: it is unknown whether Atomos recorders and Nikon cameras write UTC or
local time there. Without `--utc-offset` the result is the same as treating
it as UTC; with it, a local-time recorder can be corrected.

The TIFF DateTime is kept as the last resort, with a warning, because some
encoders may write no EXIF times at all; editing programs rewrite it, so it
never wins over DateTimeOriginal or DateTimeDigitized.

LibRaw's own timestamp comes from `mktime()` in the local time zone; it is
used only for files that are not TIFF-based (RAF, CRW, X3F, ...), where
`localtime()` turns it back into the camera's fields.

### MLV details

- Bit packing: samples MSB-first in a stream of little-endian 16-bit words
  (Canon's raw buffer). Confirmed against FFmpeg's raw video decoder, which
  byte-swaps the words and reads big-endian bits.
- Frames are sorted by frame number across spanned files; gaps are reported
  as dropped frames; a last block cut short by an interrupted recording is
  ignored with a warning.
- 10/12-bit recordings with 14-bit black/white levels are scaled down.
- LJ92 frames are decoded and used as `width x components` samples per line,
  as MLV App does; the shape of the JPEG does not matter.
- Not read: LZMA/delta (mlv_dump compression), JPEG 2000 (MLV App),
  MotionCam RAW, YUV/H.264 classes.

### FFmpeg

The vcpkg port (9.0.2 at the pinned commit) is used for the releases with
`ffmpeg[core,avcodec,avformat]`: no filters, devices or scalers, and no
external libraries, so the build is LGPL. `cmake/FFmpeg.cmake` link-tests
what it finds; with `SERCONV_FFMPEG=AUTO` an FFmpeg that does not link is left
out with a warning, so a release package is always produced (the workflow
flags it). FFmpeg 9.0 reports ProRes RAW black/white levels as
`AV_FRAME_DATA_RAW_COLOR_PARAMS` side data (lavu 60.33.100); earlier versions
neither linearize nor report them, and serconv warns.

## Verification log (0.1.0)

- `ctest`: 9 programs, all passing (8 without FFmpeg), on GCC 13 with
  FFmpeg 9.0.2 and 6.1.1, and under AddressSanitizer + UBSan; the MinGW
  build's 8 programs pass under Wine. Rerun after the review fixes.
- LJ92: 84 streams from the test encoder (P 8-16, predictors 1-7, 1-3
  components, per-component tables) decoded identically by liblj92
  (imagecodecs 2026.10.10); 15 liblj92 streams decoded identically by serconv.
- FFmpeg 9.0.2 and 6.1.1 (both minimal static builds from
  `tools/build-ffmpeg-minimal.sh`): MLV via FFmpeg equals the native reader
  for 10, 12 and 14 bits (FFmpeg's values are the native ones scaled by bit
  replication). 6.1.1 is what Ubuntu 24.04 ships for the CI.
- Windows: MinGW-w64 13 cross build, all 8 tests (no FFmpeg) pass under
  Wine 9.0, including the tests that start serconv.exe through cmd.exe and a
  clip named with non-ASCII characters. This found two problems, both fixed:
  LibRaw needs `_USE_MATH_DEFINES` on Windows, and `%zu` is not portable to
  MinGW's printf checking.
- LibRaw 0.22.2 reads the generated DNGs, uncompressed and lossless JPEG,
  bit-exact.
- An independent SER reader, serfilesreader 0.0.8 (PyPI), reads serconv's
  header fields, frames and DateTime_UTC as written.
- Sanitizers: all tests clean under AddressSanitizer, UndefinedBehavior-
  Sanitizer and LeakSanitizer, and the threaded tests (parallel MLV and DNG
  conversions, 1 to 4 threads) under ThreadSanitizer. The one UBSan report
  is in LibRaw's lossless JPEG code (`ljpeg_diff`, a shift by -1 for a zero
  difference, harmless and upstream).
- Large files: a 4,717,481,832-byte MLV converted to a 5,391,360,178-byte SER
  (1300 frames of 1920x1080); frames at offsets beyond 4 GB checked.
- Speed on a 2-core cloud machine, 1920x1080 14-bit: 96 frames/s
  uncompressed and 31 frames/s LJ92 on one thread, 178 and 60 frames/s on two.
- Real files: iPhone 12 Pro ProRAW DNG (Linear DNG, RGB SER); the ISO media
  structure of a Canon EOS R6 CR3 (tracks, CRX raw track detection, CMT1/CMT2
  EXIF) from Exiv2's test data, whose image data is pruned.

## Interoperability notes

- SER readers differ on PixelDepthPerPlane between 9 and 15: SER Player
  scales the values up for display, others reject the file. Keep the default
  16 bits unless the reader is known.
- PIPP is Windows-only; SER Player runs everywhere and is the quickest way to
  look at a result.

## Layout of the sources

| File | Purpose |
|---|---|
| `src/main.cpp`, `options.*` | command line |
| `src/convert.*` | the conversion pipeline, threads, timing, sidecar |
| `src/info.*` | `serconv info` |
| `src/inputs.cpp` | directories, wildcards, grouping into jobs, reader choice |
| `src/reader.hpp`, `frame.*` | reader interface, frames and stream descriptions |
| `src/mlv_reader.cpp`, `lj92.*` | Magic Lantern Video |
| `src/libraw_reader.cpp` | CRM, CR3, DNG and stills through LibRaw |
| `src/ffmpeg_reader.cpp` | ProRes RAW and other MOV/MP4 through FFmpeg |
| `src/bmff.*`, `tiff_meta.*` | ISO media boxes, Canon CTMD/CMT, TIFF/EXIF tags |
| `src/process.*` | levels, crops, planet detection |
| `src/ser_file.*` | SER writer and reader |
| `src/timeutil.*`, `util.*` | SER ticks and ISO 8601, files, logging |
| `tests/testdata.*` | generators: MLV, DNG, LJ92 encoder, planet scenes |
