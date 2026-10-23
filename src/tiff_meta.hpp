// Reads the few TIFF/EXIF tags serconv needs from TIFF-based raw files (DNG,
// CinemaDNG, CR2, NEF, ARW, ...) and from Canon CMT boxes: the capture time
// with sub-seconds and time zone, and the CinemaDNG frame rate.
#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>

#include "util.hpp"

namespace serconv {

struct TiffMeta {
    // The capture time, from DateTimeOriginal, else DateTimeDigitized, else
    // the TIFF DateTime (the file's date, which editing programs may change),
    // each with its SubSecTime and OffsetTime tags.
    bool hasTime = false;
    int64_t timeTicks = 0;  // wall-clock reading of the camera, sub-seconds included
    bool hasSubsec = false;
    bool hasZone = false;   // the matching OffsetTime tag is present
    int64_t zoneTicks = 0;  // offset from UTC
    std::string timeTag;    // "DateTimeOriginal", "DateTimeDigitized" or "DateTime"
    double frameRate = 0;   // DNG FrameRate (CinemaDNG)
    std::string make;
    std::string model;
    std::string uniqueCameraModel;
};

// Random access source: reads n bytes at offset, false when out of range.
using ReadAt = std::function<bool(uint64_t offset, void* buf, size_t n)>;

// `base` is added to all TIFF offsets (the TIFF header position in the source).
bool readTiffMeta(const ReadAt& read, uint64_t base, TiffMeta& meta);
bool readTiffMetaFile(File& file, TiffMeta& meta);
bool readTiffMetaBuffer(const uint8_t* data, size_t size, TiffMeta& meta);

}  // namespace serconv
