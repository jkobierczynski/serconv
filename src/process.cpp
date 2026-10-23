#include "process.hpp"

#include <algorithm>
#include <cmath>

#include "util.hpp"

namespace serconv {

const char* levelModeName(LevelMode m) {
    switch (m) {
        case LevelMode::Scale: return "scale";
        case LevelMode::Shift: return "shift";
        case LevelMode::Raw: return "raw";
    }
    return "?";
}

bool parseLevelMode(const std::string& s, LevelMode& m) {
    std::string v = toLower(trim(s));
    if (v == "scale") m = LevelMode::Scale;
    else if (v == "shift") m = LevelMode::Shift;
    else if (v == "raw") m = LevelMode::Raw;
    else return false;
    return true;
}

// ---- object detection --------------------------------------------------------

ObjectSearch findObject(const RawFrame& frame, double threshold) {
    ObjectSearch res;
    const int w = frame.width, h = frame.height, ch = frame.channels();
    if (w < 4 || h < 4) return res;
    // Detection cells: 2x2 pixels (a Bayer block) times f x f.
    const int sw = w / 2, sh = h / 2;
    const int f = std::max(1, (std::max(sw, sh) + 511) / 512);
    const int bw = sw / f, bh = sh / f;
    if (bw < 3 || bh < 3) return res;
    std::vector<float> map(size_t(bw) * size_t(bh), 0.0f);
    for (int y = 0; y < bh * f * 2; ++y) {
        const uint16_t* row = frame.data.data() + size_t(y) * size_t(w) * size_t(ch);
        float* m = map.data() + size_t(y / (2 * f)) * size_t(bw);
        for (int x = 0; x < bw * f * 2; ++x) {
            float v = 0;
            for (int c = 0; c < ch; ++c) v += row[size_t(x) * size_t(ch) + size_t(c)];
            m[x / (2 * f)] += v;
        }
    }

    // Background and noise from the lower percentiles, peak from a 3x3 mean.
    std::vector<float> sorted(map);
    auto pct = [&](double q) {
        size_t k = std::min(sorted.size() - 1, size_t(q * double(sorted.size() - 1)));
        std::nth_element(sorted.begin(), sorted.begin() + long(k), sorted.end());
        return sorted[k];
    };
    const float p10 = pct(0.10);
    const float p50 = pct(0.50);
    const double bg = p10;
    const double sigma = std::max(1e-3, double(p50 - p10) / 1.2816);
    double peak = -1;
    for (int y = 1; y < bh - 1; ++y) {
        for (int x = 1; x < bw - 1; ++x) {
            double s = 0;
            for (int dy = -1; dy <= 1; ++dy)
                for (int dx = -1; dx <= 1; ++dx) s += map[size_t(y + dy) * size_t(bw) + size_t(x + dx)];
            peak = std::max(peak, s / 9.0);
        }
    }
    if (!(peak - bg > 6.0 * sigma)) return res;
    const float thr = float(bg + threshold * (peak - bg));

    // Connected regions above the threshold; keep the one with the most light.
    std::vector<int> label(map.size(), 0);
    std::vector<size_t> stack;
    int bestLabel = 0;
    double bestFlux = 0, bestX = 0, bestY = 0, bestCells = 0;
    int next = 1;
    for (size_t start = 0; start < map.size(); ++start) {
        if (label[start] || map[start] <= thr) continue;
        double flux = 0, sx = 0, sy = 0, cells = 0;
        stack.push_back(start);
        label[start] = next;
        while (!stack.empty()) {
            size_t i = stack.back();
            stack.pop_back();
            const int x = int(i % size_t(bw)), y = int(i / size_t(bw));
            const double wgt = double(map[i] - thr);
            flux += wgt;
            sx += wgt * (x + 0.5);
            sy += wgt * (y + 0.5);
            cells += 1;
            for (int dy = -1; dy <= 1; ++dy) {
                for (int dx = -1; dx <= 1; ++dx) {
                    const int nx = x + dx, ny = y + dy;
                    if ((dx == 0 && dy == 0) || nx < 0 || ny < 0 || nx >= bw || ny >= bh) continue;
                    const size_t j = size_t(ny) * size_t(bw) + size_t(nx);
                    if (!label[j] && map[j] > thr) {
                        label[j] = next;
                        stack.push_back(j);
                    }
                }
            }
        }
        if (flux > bestFlux) {
            bestFlux = flux;
            bestLabel = next;
            bestX = sx / flux;
            bestY = sy / flux;
            bestCells = cells;
        }
        ++next;
    }
    if (!bestLabel) return res;
    res.found = true;
    res.cx = bestX * 2.0 * f;
    res.cy = bestY * 2.0 * f;
    res.cells = bestCells;
    return res;
}

// ---- processor ---------------------------------------------------------------

FrameProcessor::FrameProcessor(const StreamInfo& info, const ProcessOptions& options) : info_(info), opt_(options) {
    srcBits_ = std::clamp(info.bits, 1, 16);
    if (opt_.depth != 8 && opt_.depth != 16) fail("--depth must be 8 or 16");
    if (opt_.crop && opt_.center) fail("--crop and --center cannot be combined");

    fmt_.layout = info.layout;
    fmt_.cfa = info.cfa;
    fmt_.width = info.width;
    fmt_.height = info.height;
    if (opt_.crop) {
        if (opt_.cropW <= 0 || opt_.cropH <= 0 || opt_.cropX < 0 || opt_.cropY < 0 ||
            opt_.cropX + opt_.cropW > info.width || opt_.cropY + opt_.cropH > info.height) {
            fail(strprintf("--crop %d,%d,%d,%d is outside the %dx%d frame", opt_.cropX, opt_.cropY, opt_.cropW,
                           opt_.cropH, info.width, info.height));
        }
        fmt_.width = opt_.cropW;
        fmt_.height = opt_.cropH;
        if (info.layout == Layout::Bayer) fmt_.cfa = cfaShifted(info.cfa, opt_.cropX, opt_.cropY);
    } else if (opt_.center) {
        int cw = opt_.centerW, chh = opt_.centerH;
        if (info.layout == Layout::Bayer) {
            cw &= ~1;
            chh &= ~1;
        }
        if (cw <= 0 || chh <= 0) fail("--center needs a positive width and height");
        if (cw > info.width || chh > info.height) {
            fail(strprintf("--center %dx%d is larger than the %dx%d frame", cw, chh, info.width, info.height));
        }
        opt_.centerW = cw;
        opt_.centerH = chh;
        fmt_.width = cw;
        fmt_.height = chh;
    }

    switch (opt_.levels) {
        case LevelMode::Scale:
        case LevelMode::Shift:
            fmt_.pixelDepth = opt_.depth;
            break;
        case LevelMode::Raw:
            fmt_.pixelDepth = srcBits_ <= 8 ? 8 : srcBits_;
            break;
    }
    fmt_.bytesPerSample = fmt_.pixelDepth > 8 ? 2 : 1;
    fmt_.colorId = serColorId(fmt_.layout, fmt_.cfa);
    if (opt_.pedestal < 0 || opt_.pedestal >= (opt_.depth == 8 ? 255 : 65535)) fail("--pedestal is out of range");
}

Levels FrameProcessor::effectiveLevels(const RawFrame& frame) const {
    Levels lv = frame.hasLevels ? frame.levels : info_.levels;
    if (opt_.blackSet) {
        lv.setBlack(opt_.black);
        lv.known = true;
    }
    if (opt_.whiteSet) {
        lv.white = opt_.white;
        lv.known = true;
    }
    return lv;
}

std::string FrameProcessor::describeLevels(const Levels& lv) const {
    const int count = info_.layout == Layout::Bayer ? 4 : info_.layout == Layout::RGB ? 3 : 1;
    std::string black;
    const double bmin = lv.minBlack(count), bmax = lv.maxBlack(count);
    if (bmax - bmin < 0.5) {
        black = strprintf("%.0f", bmin);
    } else {
        for (int i = 0; i < count; ++i) black += (i ? "/" : "") + strprintf("%.0f", lv.black[i]);
    }
    switch (opt_.levels) {
        case LevelMode::Scale:
            return strprintf("scale: black %s -> %.0f, white %.0f -> %d", black.c_str(), opt_.pedestal, lv.white,
                             opt_.depth == 8 ? 255 : 65535);
        case LevelMode::Shift:
            return strprintf("shift: %d-bit values moved to %d bits (black %s kept)", srcBits_, opt_.depth,
                             black.c_str());
        case LevelMode::Raw:
            return strprintf("raw: sensor values unchanged, %d-bit (black %s, white %.0f)", fmt_.pixelDepth,
                             black.c_str(), lv.white);
    }
    return "";
}

FrameResult FrameProcessor::process(const RawFrame& in, std::vector<uint8_t>& out) const {
    if (in.width != info_.width || in.height != info_.height || in.layout != info_.layout) {
        fail(strprintf("frame %lld is %dx%d, the stream is %dx%d", static_cast<long long>(in.index + 1), in.width,
                       in.height, info_.width, info_.height));
    }
    FrameResult res;
    const int ch = in.channels();
    const int ow = fmt_.width, oh = fmt_.height;
    if (opt_.crop) {
        res.x0 = opt_.cropX;
        res.y0 = opt_.cropY;
    } else if (opt_.center) {
        res.tracked = true;
        ObjectSearch obj = findObject(in, opt_.threshold);
        double cx = in.width / 2.0, cy = in.height / 2.0;
        if (obj.found) {
            res.objectFound = true;
            cx = obj.cx;
            cy = obj.cy;
        }
        int x0 = int(std::lround((cx - ow / 2.0) / 2.0)) * 2;
        int y0 = int(std::lround((cy - oh / 2.0) / 2.0)) * 2;
        int maxX = (in.width - ow) & ~1, maxY = (in.height - oh) & ~1;
        if (in.layout != Layout::Bayer) {
            x0 = int(std::lround(cx - ow / 2.0));
            y0 = int(std::lround(cy - oh / 2.0));
            maxX = in.width - ow;
            maxY = in.height - oh;
        }
        res.x0 = std::clamp(x0, 0, std::max(0, maxX));
        res.y0 = std::clamp(y0, 0, std::max(0, maxY));
    }

    const Levels lv = effectiveLevels(in);
    const int maxOut = fmt_.bytesPerSample == 2 ? (fmt_.pixelDepth == 16 || opt_.levels != LevelMode::Raw
                                                       ? 65535
                                                       : (1 << fmt_.pixelDepth) - 1)
                                                 : 255;
    // Per-position linear map out = v * g + o.
    float g[4], o[4];
    for (int p = 0; p < 4; ++p) {
        if (opt_.levels == LevelMode::Scale) {
            const double outMax = opt_.depth == 8 ? 255.0 : 65535.0;
            const double range = lv.white - lv.black[p];
            const double gain = range > 0 ? (outMax - opt_.pedestal) / range : 1.0;
            g[p] = float(gain);
            o[p] = float(opt_.pedestal - lv.black[p] * gain);
        } else if (opt_.levels == LevelMode::Shift) {
            const double gain = std::ldexp(1.0, opt_.depth - srcBits_);
            g[p] = float(gain);
            o[p] = 0.0f;
        } else {
            g[p] = 1.0f;
            o[p] = 0.0f;
        }
    }
    const bool shiftDown = opt_.levels == LevelMode::Shift && opt_.depth < srcBits_;
    const int downBits = srcBits_ - opt_.depth;

    out.resize(fmt_.frameBytes());
    const size_t rowSamples = size_t(ow) * size_t(ch);
    const float fmax = float(maxOut);
    for (int y = 0; y < oh; ++y) {
        const int sy = res.y0 + y;
        const uint16_t* src = in.data.data() + (size_t(sy) * size_t(in.width) + size_t(res.x0)) * size_t(ch);
        uint8_t* dst = out.data() + size_t(y) * rowSamples * size_t(fmt_.bytesPerSample);
        const int rowPos = (sy & 1) << 1;
        for (size_t i = 0; i < rowSamples; ++i) {
            int p;
            if (in.layout == Layout::Bayer) p = rowPos | ((res.x0 + int(i)) & 1);
            else if (in.layout == Layout::RGB) p = int(i % 3);
            else p = 0;
            uint32_t value;
            if (opt_.levels == LevelMode::Raw) {
                value = std::min<uint32_t>(src[i], uint32_t(maxOut));
            } else if (shiftDown) {
                value = uint32_t(src[i]) >> downBits;
                if (value > uint32_t(maxOut)) value = uint32_t(maxOut);
            } else {
                float v = float(src[i]) * g[p] + o[p];
                v = v < 0.0f ? 0.0f : (v > fmax ? fmax : v);
                value = uint32_t(v + 0.5f);
            }
            if (fmt_.bytesPerSample == 2) {
                dst[i * 2] = uint8_t(value);
                dst[i * 2 + 1] = uint8_t(value >> 8);
            } else {
                dst[i] = uint8_t(value);
            }
        }
    }
    return res;
}

}  // namespace serconv
