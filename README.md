# serconv

A command-line converter from cinema and mirrorless RAW video to SER, the
video format that planetary, lunar and solar stacking software reads
(AutoStakkert!, PIPP, Siril, WinJUPOS, FTYS). Written in C++17 for Linux,
macOS and Windows.

The output keeps the sensor's Bayer mosaic: no demosaicing, no white balance,
no colour conversion. Debayering is left to the stacker, which does it after
aligning the frames. serconv adds what a planetary workflow needs on the way:
a crop that follows the planet, black/white level handling, and per-frame UTC
timestamps for derotation.

Status: **0.1.0, early.** See [What has been tested](#what-has-been-tested).

## What it reads

| Format | Files | Typical sources | Decoder |
|---|---|---|---|
| Magic Lantern Video | `.MLV` (+ `.M00`, `.M01`, ...) | Canon EOS DSLRs running Magic Lantern (mlv_rec, mlv_lite, crop modes) | built in: 10/12/14/16-bit uncompressed and LJ92 lossless |
| Canon Cinema RAW Light | `.CRM` | Canon cinema cameras (C70, C200, C300 Mark III, C500 Mark II, ...) and EOS R bodies with RAW video (R5, R5 C, R3, R1, ...) | LibRaw (CRX codec) |
| Canon CR3 RAW burst | `.CR3` | RAW burst / pre-shooting rolls of EOS R cameras, and single CR3 files | LibRaw |
| Apple ProRes RAW | `.MOV` | Atomos recorders behind Nikon, Sony, Panasonic, Fujifilm, Canon and other cameras; Nikon Z9/Z8 internally; DJI | FFmpeg 8.0 or newer |
| CinemaDNG / DNG | a folder of `.dng` files | Blackmagic cameras, Sigma fp, DJI, MLV App exports | LibRaw |
| Still raw sequences | CR2, CR3, NEF, ARW, RAF (Bayer), ORF, RW2, PEF, ... | burst shooting | LibRaw |

Through FFmpeg, anything else that decodes to Bayer, grey or RGB samples
(CineForm RAW, raw Bayer video) is accepted too. `serconv formats` lists what
the build at hand supports.

Not supported: Nikon N-RAW (`.NEV`), Blackmagic RAW (`.braw`), RED (`.R3D`),
ARRIRAW and Sony X-OCN. Their codecs are closed, patented or both, with no
open decoder. On a Nikon Z8/Z9 record ProRes RAW instead (up to 4.1K, plenty
for planets). Blackmagic and RED footage can be exported to CinemaDNG or TIFF
with the vendors' free software first.

## Output

- SER v3 with a Bayer ColorID (RGGB, GRBG, GBRG or BGGR) that follows the
  crop, mono for monochrome sensors, RGB for already demosaiced sources
  (Linear DNG such as iPhone ProRAW).
- 16-bit samples by default, 8-bit with `--depth 8`, or the sensor's own bit
  depth with `--levels raw`.
- A timestamp trailer with the UTC time of every frame, and the start time in
  the header's DateTime (camera clock) and DateTime_UTC fields.
- Next to it, `NAME.serconv.txt`: what SER cannot hold, such as camera, lens,
  ISO, exposure, black and white levels, the source frame numbers and where
  the times came from.

## Examples

```
serconv jupiter.MLV
```
writes `jupiter.ser` and `jupiter.serconv.txt` next to the clip.

```
serconv A001C012_261023.CRM --center 800x800 --utc-offset +02:00
```
crops 800 x 800 pixels around the planet in every frame (8K frames would make
a 70 MB SER frame each), and converts the camera clock, set to CEST, to UTC.

```
serconv saturn.MOV -o stacks/ --frames 1-3000 --every 2
serconv ./A003_C001_CinemaDNG/                 # -> A003_C001_CinemaDNG.ser
serconv "IMG_*.CR2" --fps 7.5 --center 600     # a burst of stills
serconv info jupiter.MLV jupiter.ser           # describe inputs and SER files
```

Several clips can be converted in one go; `-o` must then name a folder, and
serconv stops before converting anything when two inputs would write the same
file. A spanned MLV recording can be given by any of its files (`.MLV`,
`.M00`, ...).

## Options

```
Output:
  -o, --output PATH        SER file, or folder (default: the input name + .ser)
  -f, --force              overwrite existing files
      --no-sidecar         do not write NAME.serconv.txt
      --no-timestamps      do not write the per-frame timestamp trailer
      --observer TEXT      SER header fields, 40 ASCII characters each
      --instrument TEXT    (default: the camera name)
      --telescope TEXT
Frames:
      --frames A-B         frames A to B, 1-based ("A-" and "-B" work too)
      --every N            keep every Nth frame
      --max-frames N       stop after N frames
Region of interest:
      --crop X,Y,W,H       fixed crop in sensor pixels
      --center W[xH]       crop W x H pixels around the planet, Moon or Sun
      --threshold F        detection level between background and peak (0.25)
Levels:
      --levels MODE        scale (default), shift or raw, see below
      --depth 16|8         output bits for scale and shift
      --black N, --white N override the source's levels (sensor units)
      --pedestal N         output value given to the black level (scale)
Time:
      --utc-offset +HH:MM  the camera clock minus UTC (for clocks without a time zone)
      --start-time TIME    UTC time of the clip's first frame
      --fps F              frame rate for the timestamps when the source has none
Other:
      --backend NAME       auto, mlv, libraw or ffmpeg
  -j, --threads N          decoding threads (default: cores, at most 8)
  -q, --quiet / -v, --verbose / -h, --help / --version
```

### Levels

- `scale` (default): the black level becomes 0 and the white level 65535
  (255 with `--depth 8`). Per-channel black levels are used where the source
  gives them, and Canon clips can carry a black level per frame. The result is
  what most stackers expect from a 16-bit camera.
- `shift`: the values are kept and moved up so the sensor's top bit becomes
  bit 15, the way astronomy cameras deliver 12-bit data in 16-bit files. The
  black level stays in the data, for dark-frame calibration later.
- `raw`: the values are kept as they are and the SER's pixel depth is the
  sensor's (for example 14). SER Player and Siril read this; some programs
  accept only 8 and 16.

### Planet tracking

`--center` finds the brightest large object in each frame (the planet's disc,
not its moons), and crops the requested size around its centre. Crop origins
stay on even pixels, so the Bayer pattern is the same in every frame. Frames
where nothing stands out of the background are cropped around the frame
centre and counted in a warning.

### Times

Derotation (WinJUPOS) needs the UTC time of the frames. serconv takes it
from the clip:

| Source | Start time | Frame times |
|---|---|---|
| MLV | RTCI camera clock block (1 s) | VIDF frame timestamps (1 us) |
| CRM, CR3 rolls | CTMD time stamp of the first frame (1/100 s); EXIF time zone when the camera has one | sample table (exact), else CTMD per frame |
| ProRes RAW, MOV | container creation time | frame timestamps |
| DNG, stills | EXIF DateTimeOriginal + SubSecTimeOriginal (+ OffsetTimeOriginal); DateTimeDigitized or the TIFF DateTime when that is missing | CinemaDNG FrameRate, else each file's own time |

Camera clocks rarely run on UTC. When the source has no time zone, serconv
takes the camera clock as UTC and says so; give `--utc-offset` with the
offset the camera was set to (`+02:00` for a camera on CEST), or
`--start-time` when the clock is wrong. A file that records its time zone
(EXIF OffsetTime tags in DNGs and Canon clips) is converted with that zone,
and `--utc-offset` is then ignored with a note. MOV creation times are UTC by
the QuickTime specification, but some recorders write local time there, so
they are treated like a camera clock: `--utc-offset` corrects them when
needed. Dropped frames in an MLV recording show as gaps in the timestamps.

## Building

Prebuilt binaries for Linux x86_64, macOS arm64 and Windows x64 are attached
to the [releases](../../releases); they need nothing installed.

From source, with CMake 3.16 or newer and a C++17 compiler:

```
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel
ctest --test-dir build
```

- **LibRaw** is downloaded and built by CMake (`-DSERCONV_LIBRAW_SOURCE_DIR=`
  uses a local tree instead). A system LibRaw is not used on purpose: stock
  LibRaw refuses CR3 and CRM files larger than 2 GB, and Cinema RAW Light
  clips pass that within seconds. serconv's copy has the limit lifted.
- **FFmpeg** is optional (`-DSERCONV_FFMPEG=AUTO|ON|OFF`). It is found
  through vcpkg's FindFFMPEG module or pkg-config. ProRes RAW needs FFmpeg 8.0
  or newer; FFmpeg 9.0 also linearizes it and reports its black and white
  levels. `tools/build-ffmpeg-minimal.sh` builds a small static FFmpeg with
  only the parts serconv uses.

## How the formats are decoded

- **MLV** is read by serconv's own code: the block chain, spanned files,
  frames sorted by frame number, the Canon bit packing (MSB-first in
  little-endian 16-bit words) and Magic Lantern's LJ92 lossless JPEG.
- **CRM and CR3** use the CRX decoder in LibRaw, reverse engineered by Alexey
  Danilchenko. LibRaw selects the frame; serconv reads the ISO media boxes
  itself for timing (sample table, Canon CTMD records, EXIF time zone).
- **ProRes RAW** uses FFmpeg's decoder. The full sensor frame is kept (the
  recommended crop is not applied, as it would shift the Bayer pattern).
- **DNG and stills** use LibRaw; the visible area is copied unchanged.

## What has been tested

- The test suite (`ctest`) generates its inputs: MLV clips at every bit depth,
  LJ92 compressed, spanned over several files with dropped and interrupted
  frames; DNG and CinemaDNG sequences; a synthetic CRM structure; moving
  planets for the tracking.
- Cross-checks with independent implementations: FFmpeg's MLV demuxer gives
  the same samples as serconv's reader (10, 12 and 14-bit); serconv's LJ92
  decoder reads streams from liblj92 (the coder in MLV App) and LibRaw reads
  the test encoder's DNGs; the SER layout and LittleEndian convention follow
  SER Player (PIPP's author) and Siril.
- A 4.7 GB MLV clip converted to a 5.4 GB SER, frames past the 4 GB mark
  verified.
- Real files: an iPhone 12 Pro ProRAW DNG, and the structure of a Canon EOS
  R6 CR3.
- **Not yet tested on real CRM, ProRes RAW or MLV recordings.** Short sample
  clips are welcome.

## Licence

GPL-3.0, see [LICENSE](LICENSE). The binaries contain LibRaw (LGPL-2.1 or
CDDL-1.0) and FFmpeg built without GPL parts (LGPL-2.1 or later); their
licences are in the `licenses` folder of each release package.
