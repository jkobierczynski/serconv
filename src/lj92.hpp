// Lossless JPEG (ITU-T T.81 process 14, "SOF3"), as used by Magic Lantern's
// compressed MLV frames ("LJ92") and by DNG.
#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace serconv {

struct Lj92Image {
    int width = 0;       // samples per line per component (X)
    int height = 0;      // lines (Y)
    int components = 0;  // interleaved components (Nf)
    int precision = 0;   // bits per sample (P)
    int predictor = 0;   // 1..7
    std::vector<uint16_t> samples;  // height * width * components, interleaved per line
};

// Decodes the first scan of a lossless JPEG stream. Throws Error on data it
// cannot decode.
void decodeLj92(const uint8_t* data, size_t size, Lj92Image& out);

}  // namespace serconv
