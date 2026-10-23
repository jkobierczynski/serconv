// Synthetic test inputs: MLV clips, DNG files, lossless JPEG streams and
// planet-like scenes. Nothing here ships in serconv itself.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "frame.hpp"
#include "timeutil.hpp"
#include "util.hpp"

namespace testdata {

using serconv::fs::path;

// Deterministic xorshift generator.
struct Rng {
    uint64_t s;
    explicit Rng(uint64_t seed) : s(seed * 0x9E3779B97F4A7C15ull + 1) {}
    uint64_t next() {
        s ^= s << 13;
        s ^= s >> 7;
        s ^= s << 17;
        return s;
    }
    uint32_t below(uint32_t n) { return uint32_t(next() % n); }
};

// A frame of plausible sensor data in [0, 2^bits - 1].
std::vector<uint16_t> makeFrame(int width, int height, int channels, int bits, uint32_t seed, bool noisy = true);

// Background with noise, a bright disc (planet) and a small moon.
std::vector<uint16_t> makeScene(int width, int height, int bits, double cx, double cy, double radius,
                                double moonX, double moonY, uint32_t seed);

// ---- lossless JPEG encoder ---------------------------------------------------

struct Lj92Spec {
    int width = 0, height = 0, components = 1, precision = 16, predictor = 1;
    int restartInterval = 0;   // MCUs (pixels) per restart interval, 0 = none
    bool perComponentTables = false;
    int pointTransform = 0;
};
std::vector<uint8_t> encodeLj92(const std::vector<uint16_t>& samples, const Lj92Spec& spec);

// ---- MLV writer -------------------------------------------------------------

struct MlvSpec {
    int width = 64, height = 32, bpp = 14;
    bool lj92 = false;
    int lj92Components = 2;  // the stream is width/c x height with c components
    int frames = 6;
    uint32_t fpsNom = 25000, fpsDenom = 1000;
    int black = 2047, white = 15000;
    uint32_t cfa = 0x02010100;  // RGGB
    bool rtci = true;
    serconv::CivilTime clock{2026, 10, 23, 21, 14, 5, 0};
    uint64_t rtciTimestampUs = 500;
    uint64_t firstFrameUs = 1000;
    uint64_t frameStepUs = 40000;
    std::vector<uint32_t> dropFrameNumbers;  // frame numbers left out (dropped frames)
    int chunks = 1;                         // .MLV + .M00, .M01 ...
    uint32_t frameSpace = 0;                // padding before frame data
    bool truncateLast = false;              // cut the last frame short
    std::string camera = "Canon EOS 5D Mark III";
    uint32_t iso = 800;
    uint64_t shutterUs = 4000;
    std::string lens = "EF 100-400mm f/4.5-5.6L IS II";
    uint32_t seed = 1;
    // When > 0: frames show a planet of this radius drifting 2 px per frame.
    double sceneRadius = 0;
    double sceneX = 0, sceneY = 0;
};

struct MlvClip {
    std::vector<path> files;
    std::vector<std::vector<uint16_t>> frames;  // the frames that are complete, in order
    std::vector<uint32_t> frameNumbers;
    std::vector<uint64_t> timestampsUs;
};

// MSB-first packing into little-endian 16-bit words (Canon/ML layout).
std::vector<uint8_t> packMlv(const std::vector<uint16_t>& samples, int bpp);
MlvClip writeMlv(const path& mainFile, const MlvSpec& spec);

// ---- DNG writer -------------------------------------------------------------

struct DngSpec {
    int width = 32, height = 24;
    serconv::Cfa cfa = serconv::Cfa::RGGB;
    bool mono = false;
    int black = 0, white = 65535;
    bool lj92 = false;
    int lj92Components = 2;
    std::string make = "serconv", model = "Test Camera";
    std::string dateTimeOriginal;   // "YYYY:MM:DD HH:MM:SS"
    std::string subSec;             // "250"
    std::string offsetTime;         // "+02:00"
    std::string dateTimeDigitized;  // EXIF 36868
    std::string dateTime;           // TIFF 306 in IFD0 (the file's date)
    double frameRate = 0;          // CinemaDNG FrameRate
};
void writeDng(const path& file, const DngSpec& spec, const std::vector<uint16_t>& data);

// ---- SER helper ----------------------------------------------------------------

struct SerContents {
    int width = 0, height = 0, colorId = -1, depth = 0, littleEndian = -1;
    int frameCount = 0;
    std::string observer, instrument, telescope;
    int64_t dateTime = 0, dateTimeUtc = 0;
    std::vector<std::vector<uint16_t>> frames;
    std::vector<int64_t> timestamps;
};
// Independent of serconv's SerReader: parses the bytes by the SER layout.
SerContents readSerFile(const path& file);

std::vector<uint8_t> readAll(const path& file);
void writeAll(const path& file, const std::vector<uint8_t>& data);

}  // namespace testdata
