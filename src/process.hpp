// Per-frame processing: region of interest (fixed or following the planet),
// black/white level mapping and sample depth.
#pragma once

#include <string>
#include <vector>

#include "frame.hpp"

namespace serconv {

enum class LevelMode {
    Scale,  // subtract black, stretch white to full scale (default)
    Shift,  // keep values, move the sensor's top bit to bit 15 (like astro cameras)
    Raw,    // keep values; SER pixel depth = sensor bit depth
};

const char* levelModeName(LevelMode m);
bool parseLevelMode(const std::string& s, LevelMode& m);

struct ProcessOptions {
    LevelMode levels = LevelMode::Scale;
    int depth = 16;         // 8 or 16 (scale and shift modes)
    double pedestal = 0;    // output value for the black level (scale mode)
    bool blackSet = false;  // --black
    double black = 0;
    bool whiteSet = false;  // --white
    double white = 0;

    bool crop = false;  // --crop X,Y,W,H
    int cropX = 0, cropY = 0, cropW = 0, cropH = 0;
    bool center = false;  // --center W[xH]
    int centerW = 0, centerH = 0;
    double threshold = 0.25;  // object detection: fraction from background to peak
};

struct OutputFormat {
    int width = 0;
    int height = 0;
    Layout layout = Layout::Bayer;
    Cfa cfa = Cfa::RGGB;
    int pixelDepth = 16;
    int bytesPerSample = 2;
    int colorId = 8;

    size_t frameBytes() const {
        return size_t(width) * size_t(height) * size_t(channelsOf(layout)) * size_t(bytesPerSample);
    }
};

struct ObjectSearch {
    bool found = false;
    double cx = 0, cy = 0;  // frame pixel coordinates
    double cells = 0;       // size of the detected region in detection cells
};

// Finds the largest bright object (planet, Moon, Sun) in a frame.
ObjectSearch findObject(const RawFrame& frame, double threshold);

struct FrameResult {
    int x0 = 0, y0 = 0;  // crop origin in the source frame
    bool tracked = false;
    bool objectFound = false;
};

class FrameProcessor {
public:
    FrameProcessor(const StreamInfo& info, const ProcessOptions& options);

    const OutputFormat& format() const { return fmt_; }
    const ProcessOptions& options() const { return opt_; }
    // Human-readable mapping, e.g. "black 2047 -> 0, white 15000 -> 65535".
    std::string describeLevels(const Levels& lv) const;
    Levels effectiveLevels(const RawFrame& frame) const;

    // Converts one frame into SER sample bytes (16-bit little-endian or 8-bit).
    FrameResult process(const RawFrame& in, std::vector<uint8_t>& out) const;

private:
    StreamInfo info_;
    ProcessOptions opt_;
    OutputFormat fmt_;
    int srcBits_ = 16;
};

}  // namespace serconv
