// The FFmpeg reader against the native MLV reader: FFmpeg's MLV demuxer and
// rawvideo decoder are an independent implementation of the same bit packing.
#include "reader.hpp"
#include "test_util.hpp"
#include "testdata.hpp"

using namespace serconv;

int main() {
    testutil::TempDir tmp("ffmpeg");
    std::printf("FFmpeg %s, ProRes RAW decoder: %s\n", ffmpegVersionString().c_str(),
                ffmpegHasProResRaw() ? "yes" : "no");

    for (int bpp : {10, 12, 14}) {
        testdata::MlvSpec sp;
        sp.bpp = bpp;
        sp.width = 80;
        sp.height = 24;
        sp.frames = 7;
        sp.black = 100;
        sp.white = (1 << bpp) - 1;
        auto clip = testdata::writeMlv(tmp / strprintf("c%d.MLV", bpp), sp);
        auto native = openMlvReader(clip.files[0]);
        auto ff = openFFmpegReader(clip.files[0]);
        CHECK(!ff->randomAccess());
        CHECK_EQ(ff->info().width, sp.width);
        CHECK_EQ(ff->info().height, sp.height);
        CHECK(ff->info().layout == Layout::Bayer);
        CHECK(ff->info().cfa == Cfa::RGGB);
        CHECK_EQ(ff->info().frameCount, int64_t(7));
        RawFrame a, b;
        // Sequential with a skip: frames 0, 2, 3, 6.
        for (int64_t i : {0, 2, 3, 6}) {
            CHECK(native->read(i, a));
            CHECK(ff->read(i, b));
            CHECK_EQ(b.index, i);
            CHECK_EQ(b.data.size(), a.data.size());
            // FFmpeg scales n-bit samples to 16 bits by bit replication.
            bool same = b.data.size() == a.data.size();
            for (size_t k = 0; same && k < a.data.size(); ++k) {
                const uint32_t x = a.data[k];
                const uint16_t want = uint16_t((x << (16 - bpp)) | (x >> (2 * bpp - 16)));
                if (b.data[k] != want) {
                    std::fprintf(stderr, "bpp %d frame %lld sample %zu: ffmpeg %u, native %u (%u)\n", bpp,
                                 static_cast<long long>(i), k, b.data[k], want, a.data[k]);
                    same = false;
                }
            }
            CHECK(same);
            CHECK_NEAR(b.time, (clip.timestampsUs[size_t(i)] - clip.timestampsUs[0]) / 1e6, 0.002);
        }
        CHECK(!ff->read(7, b));
    }

    // A file FFmpeg cannot read.
    testdata::writeAll(tmp / "junk.mov", std::vector<uint8_t>(64, 1));
    CHECK_THROWS(openFFmpegReader(tmp / "junk.mov"));

    return testutil::finish("test_ffmpeg");
}
