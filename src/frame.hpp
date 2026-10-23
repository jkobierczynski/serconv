// Decoded frames and stream descriptions shared by all readers.
#pragma once

#include <cmath>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace serconv {

enum class Layout : uint8_t { Mono, Bayer, RGB };

// 2x2 colour filter patterns, named by the colours of the top-left 2x2 block
// read row by row.
enum class Cfa : uint8_t { RGGB, GRBG, GBRG, BGGR };

const char* layoutName(Layout layout);
const char* cfaName(Cfa cfa);
bool parseCfa(const std::string& text, Cfa& cfa);
// Colour ('R', 'G' or 'B') at pixel (x, y) of a frame whose origin has `cfa`.
char cfaColorAt(Cfa cfa, int x, int y);
// The pattern seen from origin (dx, dy) of a frame with pattern `cfa`.
Cfa cfaShifted(Cfa cfa, int dx, int dy);
// Builds a pattern from the four colours of a 2x2 block ("RGGB" etc.).
bool cfaFromColors(const std::string& four, Cfa& cfa);
int channelsOf(Layout layout);

// Black and white levels in decoded sample units. For Bayer data black[i]
// belongs to the 2x2 position i = (y & 1) * 2 + (x & 1) relative to the frame
// origin; for RGB to the channel (R, G, B); for mono only black[0] is used.
struct Levels {
    double black[4] = {0, 0, 0, 0};
    double white = 65535;
    bool known = false;  // false: defaulted, the source gave no levels

    double minBlack(int count = 4) const;
    double maxBlack(int count = 4) const;
    void setBlack(double b) {
        for (double& v : black) v = b;
    }
};

// What the absolute frame times of a reader refer to.
enum class Clock : uint8_t {
    None,    // no absolute time known
    Camera,  // the camera's clock; its offset from UTC is unknown
    Utc,     // the container says the time is UTC
};

struct StreamInfo {
    std::string container;  // "Magic Lantern MLV", "Canon Cinema RAW Light"...
    std::string codec;      // "14-bit uncompressed", "LJ92 lossless", "CRX"...
    std::string camera;
    std::string cameraSerial;
    std::string lens;

    int width = 0;
    int height = 0;
    Layout layout = Layout::Bayer;
    Cfa cfa = Cfa::RGGB;
    int bits = 16;  // significant bits per decoded sample
    Levels levels;

    int64_t frameCount = -1;  // -1: unknown until the end of the stream
    double fps = 0;           // 0: unknown

    // Absolute time of time offset 0 (see RawFrame::time), in `clock`.
    Clock clock = Clock::None;
    int64_t originTicks = 0;
    std::string timeSource;  // describes where the times come from
    // The camera's time zone (local minus UTC), when the file records it
    // (EXIF OffsetTime tags); used for the SER header's local time.
    bool zoneKnown = false;
    int64_t zoneTicks = 0;

    double exposure = 0;  // seconds, 0 unknown
    double iso = 0;
    double aperture = 0;
    double focalLength = 0;

    std::vector<std::pair<std::string, std::string>> notes;  // extra facts for info/sidecar
    std::vector<std::string> warnings;                       // reported once per conversion
};

struct RawFrame {
    int64_t index = -1;  // 0-based frame number in the source
    int width = 0;
    int height = 0;
    Layout layout = Layout::Bayer;
    Cfa cfa = Cfa::RGGB;
    std::vector<uint16_t> data;  // row-major, RGB interleaved
    double time = NAN;           // seconds after StreamInfo::originTicks; NaN unknown
    bool hasLevels = false;      // per-frame levels (e.g. Canon black level per frame)
    Levels levels;

    int channels() const { return channelsOf(layout); }
    size_t sampleCount() const { return size_t(width) * size_t(height) * size_t(channels()); }
};

// SER ColorID for a layout/pattern.
int serColorId(Layout layout, Cfa cfa);
// Inverse; returns false for unsupported IDs (CMYG, BGR is mapped to RGB with swapped=true).
bool layoutFromSerColorId(int colorId, Layout& layout, Cfa& cfa, bool& bgr);
std::string serColorName(int colorId);

}  // namespace serconv
