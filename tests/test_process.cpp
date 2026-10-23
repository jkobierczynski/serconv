// Level mapping, crops and planet tracking.
#include <cmath>

#include "process.hpp"
#include "test_util.hpp"
#include "testdata.hpp"

using namespace serconv;

static StreamInfo bayerInfo(int w, int h, int bits, double black, double white) {
    StreamInfo in;
    in.width = w;
    in.height = h;
    in.layout = Layout::Bayer;
    in.cfa = Cfa::RGGB;
    in.bits = bits;
    in.levels.setBlack(black);
    in.levels.white = white;
    in.levels.known = true;
    return in;
}

static RawFrame frameOf(const StreamInfo& in, std::vector<uint16_t> data) {
    RawFrame f;
    f.width = in.width;
    f.height = in.height;
    f.layout = in.layout;
    f.cfa = in.cfa;
    f.data = std::move(data);
    f.index = 0;
    return f;
}

static uint16_t at16(const std::vector<uint8_t>& b, size_t i) { return uint16_t(b[i * 2] | (b[i * 2 + 1] << 8)); }

int main() {
    // ---- scale: black -> 0, white -> 65535, clipping both ends
    {
        StreamInfo in = bayerInfo(4, 2, 14, 2048, 16000);
        RawFrame f = frameOf(in, {0, 2048, 16000, 16383, 9024, 2049, 2047, 12000});
        ProcessOptions o;
        FrameProcessor p(in, o);
        std::vector<uint8_t> out;
        p.process(f, out);
        CHECK_EQ(p.format().pixelDepth, 16);
        CHECK_EQ(p.format().colorId, 8);
        CHECK_EQ(at16(out, 0), uint16_t(0));
        CHECK_EQ(at16(out, 1), uint16_t(0));
        CHECK_EQ(at16(out, 2), uint16_t(65535));
        CHECK_EQ(at16(out, 3), uint16_t(65535));
        CHECK_EQ(at16(out, 4), uint16_t(std::lround((9024.0 - 2048) * 65535.0 / (16000 - 2048))));
        CHECK_EQ(at16(out, 5), uint16_t(std::lround(65535.0 / (16000 - 2048))));
        CHECK_EQ(at16(out, 6), uint16_t(0));
        // Per-position black levels (R, G1 / G2, B).
        in.levels.black[0] = 2000;
        in.levels.black[1] = 2010;
        in.levels.black[2] = 2020;
        in.levels.black[3] = 2030;
        FrameProcessor p2(in, o);
        RawFrame g = frameOf(in, {2000, 2010, 2000, 2010, 2020, 2030, 2020, 2030});
        p2.process(g, out);
        for (int i = 0; i < 8; ++i) CHECK_EQ(at16(out, size_t(i)), uint16_t(0));
        // Pedestal and 8-bit output.
        o.pedestal = 100;
        FrameProcessor p3(in, o);
        p3.process(g, out);
        CHECK_EQ(at16(out, 0), uint16_t(100));
        o.pedestal = 0;
        o.depth = 8;
        FrameProcessor p4(in, o);
        RawFrame h = frameOf(in, {2000, 16000, 9000, 9005, 2020, 2030, 16000, 2030});
        p4.process(h, out);
        CHECK_EQ(p4.format().pixelDepth, 8);
        CHECK_EQ(out.size(), size_t(8));
        CHECK_EQ(int(out[0]), 0);
        CHECK_EQ(int(out[1]), 255);
        CHECK_EQ(int(out[6]), 255);
    }

    // ---- shift and raw
    {
        StreamInfo in = bayerInfo(2, 2, 12, 256, 4095);
        RawFrame f = frameOf(in, {0, 1, 4095, 256});
        std::vector<uint8_t> out;
        ProcessOptions o;
        o.levels = LevelMode::Shift;
        FrameProcessor ps(in, o);
        ps.process(f, out);
        CHECK_EQ(ps.format().pixelDepth, 16);
        CHECK_EQ(at16(out, 1), uint16_t(16));
        CHECK_EQ(at16(out, 2), uint16_t(4095 * 16));
        CHECK_EQ(at16(out, 3), uint16_t(256 * 16));
        o.depth = 8;
        FrameProcessor ps8(in, o);
        ps8.process(f, out);
        CHECK_EQ(int(out[2]), 255);
        CHECK_EQ(int(out[3]), 16);
        o.levels = LevelMode::Raw;
        o.depth = 16;
        FrameProcessor pr(in, o);
        pr.process(f, out);
        CHECK_EQ(pr.format().pixelDepth, 12);
        CHECK_EQ(at16(out, 2), uint16_t(4095));
        CHECK_EQ(at16(out, 3), uint16_t(256));
        // --black/--white overrides.
        o.levels = LevelMode::Scale;
        o.blackSet = true;
        o.black = 0;
        o.whiteSet = true;
        o.white = 4095;
        FrameProcessor po(in, o);
        po.process(f, out);
        CHECK_EQ(at16(out, 2), uint16_t(65535));
        CHECK_EQ(at16(out, 3), uint16_t(std::lround(256 * 65535.0 / 4095)));
    }

    // ---- fixed crop at odd offsets shifts the Bayer pattern
    {
        StreamInfo in = bayerInfo(8, 6, 16, 0, 65535);
        std::vector<uint16_t> d(48);
        for (size_t i = 0; i < d.size(); ++i) d[i] = uint16_t(i);
        RawFrame f = frameOf(in, d);
        std::vector<uint8_t> out;
        for (int ox = 0; ox < 2; ++ox) {
            for (int oy = 0; oy < 2; ++oy) {
                ProcessOptions o;
                o.levels = LevelMode::Raw;
                o.crop = true;
                o.cropX = 1 + ox;
                o.cropY = 1 + oy;
                o.cropW = 4;
                o.cropH = 3;
                FrameProcessor p(in, o);
                p.process(f, out);
                CHECK_EQ(p.format().width, 4);
                CHECK_EQ(p.format().height, 3);
                CHECK(p.format().cfa == cfaShifted(Cfa::RGGB, o.cropX, o.cropY));
                CHECK_EQ(at16(out, 0), uint16_t(o.cropY * 8 + o.cropX));
                CHECK_EQ(at16(out, 5), uint16_t((o.cropY + 1) * 8 + o.cropX + 1));
            }
        }
        CHECK(cfaShifted(Cfa::RGGB, 1, 0) == Cfa::GRBG);
        CHECK(cfaShifted(Cfa::RGGB, 0, 1) == Cfa::GBRG);
        CHECK(cfaShifted(Cfa::RGGB, 1, 1) == Cfa::BGGR);
        ProcessOptions bad;
        bad.crop = true;
        bad.cropX = 6;
        bad.cropY = 0;
        bad.cropW = 4;
        bad.cropH = 2;
        CHECK_THROWS(FrameProcessor(in, bad));
    }

    // ---- planet tracking: the disc is found, the moon ignored
    {
        const int w = 640, h = 400;
        StreamInfo in = bayerInfo(w, h, 14, 800, 16383);
        for (auto pos : std::vector<std::pair<double, double>>{{200.3, 150.7}, {500, 300}, {60, 340}, {320, 200}}) {
            auto data = testdata::makeScene(w, h, 14, pos.first, pos.second, 40, pos.first + 90 > w ? 20 : pos.first + 80,
                                            pos.second, 7);
            RawFrame f = frameOf(in, data);
            ObjectSearch s = findObject(f, 0.25);
            CHECK(s.found);
            CHECK_NEAR(s.cx, pos.first, 2.5);
            CHECK_NEAR(s.cy, pos.second, 2.5);
            ProcessOptions o;
            o.center = true;
            o.centerW = 160;
            o.centerH = 120;
            FrameProcessor p(in, o);
            std::vector<uint8_t> out;
            FrameResult res = p.process(f, out);
            CHECK(res.objectFound);
            CHECK_EQ(res.x0 % 2, 0);
            CHECK_EQ(res.y0 % 2, 0);
            CHECK(res.x0 >= 0 && res.x0 + 160 <= w);
            CHECK(res.y0 >= 0 && res.y0 + 120 <= h);
            if (pos.first > 80 && pos.first < w - 80 && pos.second > 60 && pos.second < h - 60) {
                CHECK_NEAR(res.x0 + 80, pos.first, 3);
                CHECK_NEAR(res.y0 + 60, pos.second, 3);
            }
            CHECK(p.format().cfa == Cfa::RGGB);
        }
        // An empty frame: nothing found, crop at the centre.
        std::vector<uint16_t> flat(size_t(w) * h, 1000);
        RawFrame f = frameOf(in, flat);
        CHECK(!findObject(f, 0.25).found);
        ProcessOptions o;
        o.center = true;
        o.centerW = 101;  // odd: rounded down to keep the Bayer pattern
        o.centerH = 100;
        FrameProcessor p(in, o);
        CHECK_EQ(p.format().width, 100);
        std::vector<uint8_t> out;
        FrameResult res = p.process(f, out);
        CHECK(!res.objectFound);
        CHECK_EQ(res.x0, 270);
        CHECK_EQ(res.y0, 150);
        ProcessOptions big;
        big.center = true;
        big.centerW = 700;
        big.centerH = 100;
        CHECK_THROWS(FrameProcessor(in, big));
    }

    // ---- RGB and mono
    {
        StreamInfo in;
        in.width = 2;
        in.height = 1;
        in.layout = Layout::RGB;
        in.bits = 16;
        in.levels.black[0] = 100;
        in.levels.black[1] = 200;
        in.levels.black[2] = 300;
        in.levels.white = 1100;
        in.levels.known = true;
        RawFrame f = frameOf(in, {100, 200, 300, 1100, 1100, 1100});
        ProcessOptions o;
        FrameProcessor p(in, o);
        std::vector<uint8_t> out;
        p.process(f, out);
        CHECK_EQ(p.format().colorId, 100);
        CHECK_EQ(out.size(), size_t(12));
        for (int i = 0; i < 3; ++i) CHECK_EQ(at16(out, size_t(i)), uint16_t(0));
        for (int i = 3; i < 6; ++i) CHECK_EQ(at16(out, size_t(i)), uint16_t(65535));
        in.layout = Layout::Mono;
        RawFrame m = frameOf(in, {100, 1100});
        FrameProcessor pm(in, o);
        pm.process(m, out);
        CHECK_EQ(pm.format().colorId, 0);
        CHECK_EQ(at16(out, 0), uint16_t(0));
        CHECK_EQ(at16(out, 1), uint16_t(65535));
    }

    return testutil::finish("test_process");
}
