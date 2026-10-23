# To do

## Verification

- [ ] Convert real clips and compare with the vendors' tools:
  - [ ] Canon CRM (R5 or C70), a short clip over 2 GB as well
  - [ ] ProRes RAW from an Atomos recorder and from a Nikon Z8/Z9
  - [ ] MLV from a Magic Lantern camera: uncompressed, LJ92, spanned
    (.M00), a crop_rec mode, 10 and 12-bit
  - [ ] CinemaDNG from a Blackmagic camera and a Sigma fp
  - [ ] A Canon CR3 RAW burst roll
- [ ] Check the SER files in AutoStakkert!, PIPP, Siril, SER Player,
  WinJUPOS (timestamps) and FTYS.
- [ ] Find out whether Atomos recorders and Nikon Z cameras write the MOV
  creation time in UTC or local time (now treated as a camera clock).
- [ ] Find out which date tags Blackmagic and Sigma fp CinemaDNG frames carry.
- [ ] First run of the CI and release workflows on GitHub (macOS, Windows).

## Formats

- [ ] ProRes RAW: take the start time from the QuickTime timecode track when
  it carries the time of day (sub-second start).
- [ ] ProRes RAW: Bayer patterns other than RGGB once FFmpeg decodes them.
- [ ] MLV: subtract the DARK frame block when present; flag or interpolate
  Dual ISO lines; read mlv_dump's LZMA/delta-compressed files.
- [ ] LibRaw with zlib and libjpeg for deflate and lossy DNG.
- [ ] Nikon N-RAW: research only. Start from high-efficiency NEF stills
  (Adobe DNG Converter gives reference output), check the codestream for
  JPEG XS markers; settle the patent question before any release.
- [ ] Blackmagic RAW through the user's own copy of the SDK (optional plugin).

## Features

- [ ] `--drop-lost`: leave out frames where `--center` finds no object
  (clouds, wind gusts).
- [ ] Seek in the FFmpeg reader instead of reading every packet before a
  late `--frames` start.
- [ ] `--split N`: several SER files of N frames each.
- [ ] A FireCapture-style `.txt` next to the SER, if a stacker reads one.
- [ ] Faster LJ92 decoding (table-driven two-symbol decode, SIMD prediction).

## Housekeeping

- [ ] Windows CI with FFmpeg (vcpkg binary cache).
- [ ] Fuzz the MLV, LJ92, ISO media and TIFF parsers.
