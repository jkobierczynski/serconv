// LibRaw path: DNG and CinemaDNG sequences written by the test generator.
#include "reader.hpp"
#include "test_util.hpp"
#include "testdata.hpp"
#include "timeutil.hpp"

using namespace serconv;

int main() {
    testutil::TempDir tmp("libraw");

    // Every Bayer pattern, uncompressed and lossless JPEG.
    for (int pat = 0; pat < 4; ++pat) {
        for (bool lj : {false, true}) {
            testdata::DngSpec sp;
            sp.width = 40;
            sp.height = 26;
            sp.cfa = Cfa(pat);
            sp.black = 512;
            sp.white = 15800;
            sp.lj92 = lj;
            auto data = testdata::makeFrame(sp.width, sp.height, 1, 14, uint32_t(pat * 2 + lj));
            auto file = tmp / strprintf("single_%d_%d.dng", pat, int(lj));
            testdata::writeDng(file, sp, data);
            auto r = openLibRawSequence({file});
            const StreamInfo& in = r->info();
            CHECK_EQ(in.width, sp.width);
            CHECK_EQ(in.height, sp.height);
            CHECK(in.layout == Layout::Bayer);
            CHECK_EQ(std::string(cfaName(in.cfa)), std::string(cfaName(sp.cfa)));
            CHECK_NEAR(in.levels.black[0], 512, 0);
            CHECK_NEAR(in.levels.black[3], 512, 0);
            CHECK_NEAR(in.levels.white, 15800, 0);
            CHECK_EQ(in.bits, 14);
            RawFrame f;
            CHECK(r->read(0, f));
            CHECK(f.data == data);
            if (lj) CHECK(in.codec.find("lossless") != std::string::npos);
        }
    }

    // A monochrome (LinearRaw) DNG.
    {
        testdata::DngSpec sp;
        sp.mono = true;
        sp.width = 30;
        sp.height = 24;  // LibRaw ignores images under 22 pixels
        auto data = testdata::makeFrame(sp.width, sp.height, 1, 16, 77);
        auto file = tmp / "mono.dng";
        testdata::writeDng(file, sp, data);
        auto r = openLibRawSequence({file});
        CHECK(r->info().layout == Layout::Mono);
        RawFrame f;
        CHECK(r->read(0, f));
        CHECK(f.data == data);
    }

    // A sequence with EXIF capture times (sub-seconds, time zone).
    {
        std::vector<fs::path> files;
        std::vector<std::vector<uint16_t>> frames;
        for (int i = 0; i < 4; ++i) {
            testdata::DngSpec sp;
            sp.dateTimeOriginal = "2026:10:23 23:14:05";
            sp.subSec = strprintf("%03d", i * 250);
            sp.offsetTime = "+02:00";
            auto data = testdata::makeFrame(sp.width, sp.height, 1, 16, uint32_t(100 + i));
            auto file = tmp / strprintf("seq/frame_%04d.dng", i + 1);
            fs::create_directories(file.parent_path());
            testdata::writeDng(file, sp, data);
            files.push_back(file);
            frames.push_back(data);
        }
        auto r = openLibRawSequence(files);
        const StreamInfo& in = r->info();
        CHECK_EQ(in.frameCount, int64_t(4));
        CHECK(in.clock == Clock::Utc);
        CHECK_EQ(in.originTicks, ticksFromCivil({2026, 10, 23, 21, 14, 5, 0}));
        auto c = r->clone();
        RawFrame f;
        for (int i = 3; i >= 0; --i) {  // random access, reverse order
            CHECK(c->read(i, f));
            CHECK(f.data == frames[size_t(i)]);
            CHECK_NEAR(f.time, i * 0.25, 1e-9);
        }
        // collectInputs: a folder becomes one sequence named after the folder.
        auto jobs = collectInputs({pathToUtf8(tmp.path / "seq")});
        CHECK_EQ(jobs.size(), size_t(1));
        CHECK(jobs[0].sequence);
        CHECK_EQ(jobs[0].files.size(), size_t(4));
        CHECK_EQ(pathToUtf8(jobs[0].defaultOutput.filename()), std::string("seq.ser"));
        CHECK_EQ(pathToUtf8(jobs[0].files[0].filename()), std::string("frame_0001.dng"));
        // A wildcard.
        auto wild = collectInputs({pathToUtf8(tmp.path / "seq" / "FRAME_*.DNG")});
        CHECK_EQ(wild.size(), size_t(1));
        CHECK_EQ(wild[0].files.size(), size_t(4));
        CHECK_EQ(pathToUtf8(wild[0].defaultOutput.filename()), std::string("frame.ser"));
    }

    // CinemaDNG frame rate drives the frame times.
    {
        std::vector<fs::path> files;
        for (int i = 0; i < 3; ++i) {
            testdata::DngSpec sp;
            sp.frameRate = 50;
            sp.dateTimeOriginal = "2026:10:23 21:00:00";
            auto data = testdata::makeFrame(sp.width, sp.height, 1, 12, uint32_t(i));
            auto file = tmp / strprintf("cdng/clip_%06d.dng", i);
            fs::create_directories(file.parent_path());
            testdata::writeDng(file, sp, data);
            files.push_back(file);
        }
        auto r = openLibRawSequence(files);
        CHECK_NEAR(r->info().fps, 50, 1e-9);
        CHECK(r->info().container.find("CinemaDNG") != std::string::npos);
        CHECK_NEAR(r->frameTime(2), 0.04, 1e-12);
        CHECK(r->info().clock == Clock::Camera);
    }

    // Which date tag gives the capture time.
    {
        auto infoOf = [&](const std::string& name, const testdata::DngSpec& sp) {
            auto file = tmp / name;
            testdata::writeDng(file, sp, testdata::makeFrame(sp.width, sp.height, 1, 16, 7));
            return openLibRawSequence({file})->info();
        };
        // DateTimeOriginal wins over DateTimeDigitized and the file's DateTime.
        testdata::DngSpec a;
        a.dateTimeOriginal = "2026:10:23 21:00:00";
        a.dateTimeDigitized = "2026:10:23 21:30:00";
        a.dateTime = "2026:11:02 08:00:00";  // e.g. edited later
        StreamInfo ia = infoOf("tags_a.dng", a);
        CHECK(ia.clock == Clock::Camera);
        CHECK_EQ(ia.originTicks, ticksFromCivil({2026, 10, 23, 21, 0, 0, 0}));
        CHECK(ia.timeSource.find("DateTimeOriginal") != std::string::npos);
        CHECK(!ia.zoneKnown);
        // Then DateTimeDigitized.
        testdata::DngSpec b;
        b.dateTimeDigitized = "2026:10:23 21:30:00";
        b.dateTime = "2026:11:02 08:00:00";
        StreamInfo ib = infoOf("tags_b.dng", b);
        CHECK_EQ(ib.originTicks, ticksFromCivil({2026, 10, 23, 21, 30, 0, 0}));
        CHECK(ib.timeSource.find("DateTimeDigitized") != std::string::npos);
        // The TIFF DateTime alone is used, with a warning.
        testdata::DngSpec c;
        c.dateTime = "2026:10:23 22:00:00";
        StreamInfo ic = infoOf("tags_c.dng", c);
        CHECK(ic.clock == Clock::Camera);
        CHECK_EQ(ic.originTicks, ticksFromCivil({2026, 10, 23, 22, 0, 0, 0}));
        bool warned = false;
        for (const auto& w : ic.warnings) warned = warned || w.find("TIFF DateTime") != std::string::npos;
        CHECK(warned);
        // No date tag at all: no time (LibRaw's timestamp is not consulted for TIFF files).
        testdata::DngSpec d;
        StreamInfo id = infoOf("tags_d.dng", d);
        CHECK(id.clock == Clock::None);
        // A time zone makes the time UTC and is kept for the header's local time.
        testdata::DngSpec e;
        e.dateTimeOriginal = "2026:10:23 23:00:00";
        e.offsetTime = "+02:00";
        StreamInfo ie = infoOf("tags_e.dng", e);
        CHECK(ie.clock == Clock::Utc);
        CHECK(ie.zoneKnown);
        CHECK_EQ(ie.zoneTicks, int64_t(2) * 3600 * kTicksPerSecond);
        CHECK_EQ(ie.originTicks, ticksFromCivil({2026, 10, 23, 21, 0, 0, 0}));
    }

    // Frames of different sizes are rejected.
    {
        testdata::DngSpec a, b;
        b.width = a.width + 2;
        auto fa = tmp / "mix_1.dng", fb = tmp / "mix_2.dng";
        testdata::writeDng(fa, a, testdata::makeFrame(a.width, a.height, 1, 16, 1));
        testdata::writeDng(fb, b, testdata::makeFrame(b.width, b.height, 1, 16, 2));
        auto r = openLibRawSequence({fa, fb});
        RawFrame f;
        CHECK(r->read(0, f));
        CHECK_THROWS(r->read(1, f));
    }

    // Not a raw file.
    {
        testdata::writeAll(tmp / "fake.dng", std::vector<uint8_t>(1000, 3));
        CHECK_THROWS(openLibRawSequence({tmp / "fake.dng"}));
    }

    return testutil::finish("test_libraw");
}
