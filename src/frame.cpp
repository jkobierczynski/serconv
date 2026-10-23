#include "frame.hpp"

#include <algorithm>

#include "util.hpp"

namespace serconv {

namespace {
const char* const kCfaColors[4] = {"RGGB", "GRBG", "GBRG", "BGGR"};
}

const char* layoutName(Layout layout) {
    switch (layout) {
        case Layout::Mono: return "mono";
        case Layout::Bayer: return "Bayer";
        case Layout::RGB: return "RGB";
    }
    return "?";
}

const char* cfaName(Cfa cfa) { return kCfaColors[int(cfa) & 3]; }

bool cfaFromColors(const std::string& four, Cfa& cfa) {
    std::string up = toUpper(four);
    for (int i = 0; i < 4; ++i) {
        if (up == kCfaColors[i]) {
            cfa = Cfa(i);
            return true;
        }
    }
    return false;
}

bool parseCfa(const std::string& text, Cfa& cfa) { return cfaFromColors(trim(text), cfa); }

char cfaColorAt(Cfa cfa, int x, int y) { return kCfaColors[int(cfa)][((y & 1) << 1) | (x & 1)]; }

Cfa cfaShifted(Cfa cfa, int dx, int dy) {
    std::string four;
    four += cfaColorAt(cfa, dx, dy);
    four += cfaColorAt(cfa, dx + 1, dy);
    four += cfaColorAt(cfa, dx, dy + 1);
    four += cfaColorAt(cfa, dx + 1, dy + 1);
    Cfa out = cfa;
    cfaFromColors(four, out);
    return out;
}

int channelsOf(Layout layout) { return layout == Layout::RGB ? 3 : 1; }

double Levels::minBlack(int count) const { return *std::min_element(black, black + std::clamp(count, 1, 4)); }

double Levels::maxBlack(int count) const { return *std::max_element(black, black + std::clamp(count, 1, 4)); }

int serColorId(Layout layout, Cfa cfa) {
    switch (layout) {
        case Layout::Mono: return 0;
        case Layout::RGB: return 100;
        case Layout::Bayer:
            switch (cfa) {
                case Cfa::RGGB: return 8;
                case Cfa::GRBG: return 9;
                case Cfa::GBRG: return 10;
                case Cfa::BGGR: return 11;
            }
    }
    return 0;
}

bool layoutFromSerColorId(int colorId, Layout& layout, Cfa& cfa, bool& bgr) {
    bgr = false;
    cfa = Cfa::RGGB;
    switch (colorId) {
        case 0: layout = Layout::Mono; return true;
        case 8: layout = Layout::Bayer; cfa = Cfa::RGGB; return true;
        case 9: layout = Layout::Bayer; cfa = Cfa::GRBG; return true;
        case 10: layout = Layout::Bayer; cfa = Cfa::GBRG; return true;
        case 11: layout = Layout::Bayer; cfa = Cfa::BGGR; return true;
        case 100: layout = Layout::RGB; return true;
        case 101: layout = Layout::RGB; bgr = true; return true;
        default: return false;
    }
}

std::string serColorName(int colorId) {
    switch (colorId) {
        case 0: return "MONO";
        case 8: return "BAYER_RGGB";
        case 9: return "BAYER_GRBG";
        case 10: return "BAYER_GBRG";
        case 11: return "BAYER_BGGR";
        case 16: return "BAYER_CYYM";
        case 17: return "BAYER_YCMY";
        case 18: return "BAYER_YMCY";
        case 19: return "BAYER_MYYC";
        case 100: return "RGB";
        case 101: return "BGR";
        default: return strprintf("unknown (%d)", colorId);
    }
}

}  // namespace serconv
