// Minimal ISO base media file format (MP4/QuickTime) parsing for Canon CR3
// and Cinema RAW Light (CRM) files: track sample tables, timing, Canon timed
// metadata (CTMD) and the CMT metadata boxes. LibRaw decodes the frames; this
// only recovers what LibRaw does not expose, chiefly per-frame timing.
#pragma once

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include "frame.hpp"
#include "timeutil.hpp"
#include "util.hpp"

namespace serconv {

struct BmffTrack {
    uint32_t trackId = 0;
    std::string handler;      // 'vide', 'meta', 'soun'...
    std::string sampleEntry;  // 'CRAW', 'CTMD', 'apcn'...
    bool canonRaw = false;    // CRAW entry with a CMP1 box: CRX raw data (not a JPEG preview)
    int width = 0;            // from a visual sample entry
    int height = 0;
    uint32_t timescale = 0;
    uint64_t duration = 0;
    uint64_t creationTime = 0;  // seconds since 1904-01-01
    std::vector<std::pair<uint32_t, uint32_t>> stts;  // (sample count, delta)
    uint32_t sampleCount = 0;
    std::vector<uint64_t> sampleOffsets;  // file offset of each sample
    std::vector<uint32_t> sampleSizes;

    // Decode time of sample i in timescale units.
    uint64_t sampleTime(uint32_t index) const;
};

struct BmffMovie {
    std::string majorBrand;  // "crx " for Canon CR3/CRM
    uint32_t timescale = 0;  // from mvhd
    uint64_t duration = 0;
    uint64_t creationTime = 0;  // seconds since 1904-01-01
    std::vector<BmffTrack> tracks;
    std::vector<uint8_t> cmt1;  // Canon: TIFF IFD0
    std::vector<uint8_t> cmt2;  // Canon: TIFF Exif IFD

    const BmffTrack* largestTrack(const std::string& sampleEntry) const;
    // The CRX raw track (CRAW with CMP1), else the largest CRAW track.
    const BmffTrack* canonRawTrack() const;
    const BmffTrack* firstTrack(const std::string& sampleEntry) const;
};

// Parses the moov box (and Canon uuid metadata boxes). Returns false with a
// reason when the file is not an ISO media file or has no moov box.
bool parseBmff(File& file, BmffMovie& movie, std::string* error);

// Frames in the CRX raw track of a CR3/CRM file (a RAW burst roll has many),
// -1 when the file cannot be read as one.
int64_t canonFrameCount(const fs::path& path);

// Ticks of a 1904-based QuickTime/MP4 time (taken as a wall-clock reading).
int64_t ticksFrom1904(uint64_t seconds);

// Canon CTMD sample: the record of type 1 holds the capture time to 1/100 s
// (camera clock). Returns false when there is none.
bool parseCtmdTime(const uint8_t* data, size_t size, CivilTime& time);

// Frame times and the absolute start of a Canon CRM/CR3 clip, from the CRX
// track's sample table, the CTMD time stamps and the EXIF time zone.
struct CanonTiming {
    std::vector<double> times;  // seconds after originTicks, one per frame (may be empty)
    double fps = 0;
    Clock clock = Clock::None;
    int64_t originTicks = 0;
    bool zoneKnown = false;  // the camera's time zone, from EXIF
    int64_t zoneTicks = 0;
    std::string source;
    std::string warning;
};
CanonTiming canonClipTiming(File& file, size_t frameCount);

}  // namespace serconv
