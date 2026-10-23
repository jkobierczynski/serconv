// Native MLV reader on generated clips.
#include <cstring>

#include "reader.hpp"
#include "test_util.hpp"
#include "testdata.hpp"
#include "timeutil.hpp"

using namespace serconv;

static void checkClip(const testdata::MlvClip& clip, Reader& r, const testdata::MlvSpec& sp) {
    const StreamInfo& in = r.info();
    CHECK_EQ(in.width, sp.width);
    CHECK_EQ(in.height, sp.height);
    CHECK_EQ(in.bits, sp.bpp);
    CHECK_EQ(in.frameCount, int64_t(clip.frames.size()));
    CHECK(in.layout == Layout::Bayer);
    CHECK_NEAR(in.fps, double(sp.fpsNom) / sp.fpsDenom, 1e-9);
    CHECK_EQ(in.camera, sp.camera);
    CHECK_EQ(in.lens, sp.lens);
    CHECK_NEAR(in.iso, sp.iso, 0);
    CHECK_NEAR(in.exposure, sp.shutterUs / 1e6, 1e-12);
    RawFrame f;
    for (size_t k = 0; k < clip.frames.size(); ++k) {
        CHECK(r.read(int64_t(k), f));
        CHECK_EQ(f.width, sp.width);
        CHECK_EQ(f.height, sp.height);
        CHECK(f.data == clip.frames[k]);
        CHECK_NEAR(f.time, clip.timestampsUs[k] / 1e6, 1e-9);
        CHECK_NEAR(r.frameTime(int64_t(k)), clip.timestampsUs[k] / 1e6, 1e-9);
    }
    CHECK(!r.read(int64_t(clip.frames.size()), f));
}

int main() {
    testutil::TempDir tmp("mlv");

    // Bit packing: the bit layout of Magic Lantern's 14-bit raw (8 pixels in
    // 7 words; first word holds pixel a in its top 14 bits).
    {
        std::vector<uint16_t> px = {0x3FFF, 0, 0x2AAA, 0x1555, 1, 2, 3, 0x3000};
        std::vector<uint8_t> packed = testdata::packMlv(px, 14);
        CHECK_EQ(packed.size(), size_t(14));
        CHECK_EQ(rd16le(&packed[0]) >> 2, 0x3FFFu);
        CHECK_EQ(rd16le(&packed[0]) & 3u, 0u);
    }

    for (int bpp : {10, 12, 14, 16}) {
        testdata::MlvSpec sp;
        sp.bpp = bpp;
        sp.black = bpp == 14 ? 2047 : (bpp == 12 ? 511 : (bpp == 10 ? 127 : 8191));
        sp.white = (1 << bpp) - 1 - 100;
        sp.seed = uint32_t(bpp);
        auto clip = testdata::writeMlv(tmp / strprintf("plain%d.MLV", bpp), sp);
        auto r = openMlvReader(clip.files[0]);
        checkClip(clip, *r, sp);
        CHECK_NEAR(r->info().levels.black[0], sp.black, 0);
        CHECK_NEAR(r->info().levels.white, sp.white, 0);
        CHECK(r->info().cfa == Cfa::RGGB);
        CHECK(r->info().clock == Clock::Camera);
        // RTCI at 21:14:05 was 500 us into the recording.
        CHECK_EQ(r->info().originTicks, ticksFromCivil(sp.clock) - 500 * 10);
        auto c = r->clone();
        RawFrame f;
        CHECK(c->read(3, f));
        CHECK(f.data == clip.frames[3]);
    }

    // LJ92 compressed, as one and as two interleaved components.
    for (int comps : {1, 2}) {
        testdata::MlvSpec sp;
        sp.lj92 = true;
        sp.lj92Components = comps;
        sp.bpp = 14;
        sp.width = 96;
        sp.height = 20;
        auto clip = testdata::writeMlv(tmp / strprintf("lj92_%d.MLV", comps), sp);
        auto r = openMlvReader(clip.files[0]);
        checkClip(clip, *r, sp);
        CHECK(r->info().codec.find("LJ92") != std::string::npos);
    }

    // Spanned over 3 files with frames interleaved, a dropped frame, padding
    // before the frame data, another CFA pattern and an interrupted last frame.
    {
        testdata::MlvSpec sp;
        sp.chunks = 3;
        sp.frames = 9;
        sp.dropFrameNumbers = {4};
        sp.frameSpace = 24;
        sp.cfa = 0x00010102;  // B G G R
        sp.truncateLast = true;
        auto clip = testdata::writeMlv(tmp / "span.MLV", sp);
        CHECK_EQ(clip.files.size(), size_t(3));
        auto r = openMlvReader(clip.files[0]);
        CHECK(r->info().cfa == Cfa::BGGR);
        CHECK_EQ(r->info().frameCount, int64_t(8));
        bool droppedNoted = false, truncatedNoted = false;
        for (const auto& w : r->info().warnings) {
            if (w.find("missing") != std::string::npos) droppedNoted = true;
            if (w.find("incomplete") != std::string::npos) truncatedNoted = true;
        }
        CHECK(droppedNoted);
        CHECK(truncatedNoted);
        checkClip(clip, *r, sp);
        // Opening a spanned file finds the main file.
        auto r2 = openMlvReader(clip.files[1]);
        CHECK_EQ(r2->info().frameCount, int64_t(8));
    }

    // A damaged block in the middle: the frames before it are kept.
    {
        testdata::MlvSpec sp;
        sp.frames = 6;
        auto clip = testdata::writeMlv(tmp / "damaged.MLV", sp);
        std::vector<uint8_t> d = testdata::readAll(clip.files[0]);
        size_t at = 0;
        for (int k = 0; k < 4; ++k) {  // the 4th VIDF block
            at = std::string(d.begin(), d.end()).find("VIDF", k == 0 ? 0 : at + 4);
            CHECK(at != std::string::npos);
        }
        std::memcpy(&d[at], "\x01\x02\x03\x04", 4);
        testdata::writeAll(clip.files[0], d);
        auto r = openMlvReader(clip.files[0]);
        CHECK_EQ(r->info().frameCount, int64_t(3));
        bool damagedNoted = false;
        for (const auto& w : r->info().warnings) damagedNoted = damagedNoted || w.find("damaged block") != std::string::npos;
        CHECK(damagedNoted);
        RawFrame f;
        for (int k = 0; k < 3; ++k) {
            CHECK(r->read(k, f));
            CHECK(f.data == clip.frames[size_t(k)]);
        }
    }

    // Levels written for 14-bit data in a 12-bit clip are scaled down.
    {
        testdata::MlvSpec sp;
        sp.bpp = 12;
        sp.black = 2048;
        sp.white = 15000;
        auto clip = testdata::writeMlv(tmp / "levels12.MLV", sp);
        auto r = openMlvReader(clip.files[0]);
        CHECK_NEAR(r->info().levels.black[0], 512, 0);
        CHECK_NEAR(r->info().levels.white, 3750, 0);
    }

    // No RTCI block: relative times only.
    {
        testdata::MlvSpec sp;
        sp.rtci = false;
        auto clip = testdata::writeMlv(tmp / "nortc.MLV", sp);
        auto r = openMlvReader(clip.files[0]);
        CHECK(r->info().clock == Clock::None);
    }

    // Not an MLV.
    {
        testdata::writeAll(tmp / "bad.MLV", std::vector<uint8_t>(100, 0));
        CHECK_THROWS(openMlvReader(tmp / "bad.MLV"));
    }

    return testutil::finish("test_mlv");
}
