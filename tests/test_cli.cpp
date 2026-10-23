// End-to-end conversions through the command-line options, and the binary.
#include <cmath>
#include <cstring>

#include "convert.hpp"
#include "options.hpp"
#include "test_util.hpp"
#include "testdata.hpp"
#include "timeutil.hpp"

using namespace serconv;

static int convert(const std::vector<std::string>& args) {
    try {
        ParsedArgs pa = parseArgs(args);
        setLogLevel(LogLevel::Quiet);
        return runConvert(pa.convert);
    } catch (const UsageError& e) {
        std::fprintf(stderr, "  (usage error: %s)\n", e.what());
        return 2;
    } catch (const Error& e) {
        std::fprintf(stderr, "  (error: %s)\n", e.what());
        return 1;
    }
}

static std::string slurp(const fs::path& p) {
    auto d = testdata::readAll(p);
    return std::string(d.begin(), d.end());
}

static uint16_t scaled(uint16_t v, double black, double white) {
    double x = (double(v) - black) * 65535.0 / (white - black);
    if (x < 0) x = 0;
    if (x > 65535) x = 65535;
    return uint16_t(std::lround(x));
}

static bool nearlyEqual(const std::vector<uint16_t>& a, const std::vector<uint16_t>& b, int tol) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i)
        if (std::abs(int(a[i]) - int(b[i])) > tol) return false;
    return true;
}

int main(int argc, char** argv) {
    testutil::TempDir tmp("cli");
    const std::string exe = argc > 1 ? argv[1] : "";

    testdata::MlvSpec sp;  // 64x32, 14-bit, 6 frames at 25 fps, black 2047, white 15000
    auto clip = testdata::writeMlv(tmp / "clip.MLV", sp);
    const std::string in = pathToUtf8(clip.files[0]);
    const int64_t rtc = ticksFromCivil(sp.clock) - int64_t(sp.rtciTimestampUs) * 10;
    auto frameUtc = [&](size_t k, int64_t offset) {
        return rtc + int64_t(clip.timestampsUs[k]) * 10 - offset;
    };

    // ---- defaults: next to the input, scaled to 16 bits, timestamps, sidecar
    {
        CHECK_EQ(convert({in}), 0);
        auto ser = testdata::readSerFile(tmp / "clip.ser");
        CHECK_EQ(ser.width, 64);
        CHECK_EQ(ser.height, 32);
        CHECK_EQ(ser.colorId, 8);
        CHECK_EQ(ser.depth, 16);
        CHECK_EQ(ser.littleEndian, 0);
        CHECK_EQ(ser.frameCount, 6);
        CHECK_EQ(ser.instrument, sp.camera);
        CHECK_EQ(ser.frames.size(), size_t(6));
        for (size_t k = 0; k < ser.frames.size(); ++k) {
            std::vector<uint16_t> want(clip.frames[k].size());
            for (size_t i = 0; i < want.size(); ++i) want[i] = scaled(clip.frames[k][i], sp.black, sp.white);
            CHECK(nearlyEqual(ser.frames[k], want, 1));
        }
        CHECK_EQ(ser.timestamps.size(), size_t(6));
        for (size_t k = 0; k < ser.timestamps.size(); ++k) CHECK_EQ(ser.timestamps[k], frameUtc(k, 0));
        CHECK_EQ(ser.dateTimeUtc, frameUtc(0, 0));
        CHECK_EQ(ser.dateTime, frameUtc(0, 0));
        auto side = tmp / "clip.serconv.txt";
        CHECK(fs::exists(side));
        std::string s = slurp(side);
        CHECK(s.find("camera") != std::string::npos && s.find(sp.camera) != std::string::npos);
        CHECK(s.find("black_level") != std::string::npos && s.find("2047") != std::string::npos);
        CHECK(s.find("first_frame_utc = 2026-10-23T21:14:05.0005Z") != std::string::npos);

        // Exists: refused without --force.
        CHECK_EQ(convert({in}), 1);
        CHECK_EQ(convert({in, "--force"}), 0);
    }

    // ---- --utc-offset, output folder, header strings
    {
        const std::string out = pathToUtf8(tmp.path / "out") + "/";
        CHECK_EQ(convert({in, "-o", out, "--utc-offset", "+02:00", "--observer", "J. K.", "--telescope", "C14",
                          "--instrument", "5D3"}),
                 0);
        auto ser = testdata::readSerFile(tmp / "out/clip.ser");
        const int64_t off = 2 * 3600 * kTicksPerSecond;
        CHECK_EQ(ser.timestamps.size(), size_t(6));
        CHECK_EQ(ser.timestamps[0], frameUtc(0, off));
        CHECK_EQ(ser.dateTimeUtc, frameUtc(0, off));
        CHECK_EQ(ser.dateTime, frameUtc(0, off) + off);
        CHECK_EQ(ser.observer, std::string("J. K."));
        CHECK_EQ(ser.telescope, std::string("C14"));
        CHECK_EQ(ser.instrument, std::string("5D3"));
    }

    // ---- frame selection
    {
        CHECK_EQ(convert({in, "-o", pathToUtf8(tmp / "sel.ser"), "--frames", "2-5", "--every", "2", "--levels", "raw"}),
                 0);
        auto ser = testdata::readSerFile(tmp / "sel.ser");
        CHECK_EQ(ser.frameCount, 2);
        CHECK_EQ(ser.depth, 14);
        CHECK(ser.frames.size() == 2 && ser.frames[0] == clip.frames[1] && ser.frames[1] == clip.frames[3]);
        CHECK_EQ(ser.timestamps[1], frameUtc(3, 0));
        CHECK_EQ(convert({in, "-o", pathToUtf8(tmp / "max.ser"), "--max-frames", "3", "--frames", "4-"}), 0);
        CHECK_EQ(testdata::readSerFile(tmp / "max.ser").frameCount, 3);
        CHECK_EQ(convert({in, "-o", pathToUtf8(tmp / "x.ser"), "--frames", "9-"}), 1);
        CHECK_EQ(convert({in, "--frames", "5-2"}), 2);
    }

    // ---- shift to 8 bits, crop with a pattern change
    {
        CHECK_EQ(convert({in, "-o", pathToUtf8(tmp / "s8.ser"), "--levels", "shift", "--depth", "8"}), 0);
        auto s8 = testdata::readSerFile(tmp / "s8.ser");
        CHECK_EQ(s8.depth, 8);
        CHECK_EQ(s8.frames[0][5], uint16_t(clip.frames[0][5] >> 6));
        CHECK_EQ(convert({in, "-o", pathToUtf8(tmp / "crop.ser"), "--crop", "1,1,10,8", "--levels", "raw"}), 0);
        auto cr = testdata::readSerFile(tmp / "crop.ser");
        CHECK_EQ(cr.colorId, 11);  // RGGB seen from (1,1) is BGGR
        CHECK_EQ(cr.width, 10);
        CHECK_EQ(cr.frames[2][0], clip.frames[2][1 * 64 + 1]);
        CHECK_EQ(convert({in, "--crop", "60,0,10,8"}), 1);
    }

    // ---- --start-time, no trailer, no sidecar
    {
        CHECK_EQ(convert({in, "-o", pathToUtf8(tmp / "st.ser"), "--start-time", "2026-10-23T20:00:00.5Z"}), 0);
        auto st = testdata::readSerFile(tmp / "st.ser");
        const int64_t start = ticksFromCivil({2026, 10, 23, 20, 0, 0, 5000000});
        CHECK_EQ(st.timestamps[0], start);
        CHECK_EQ(st.timestamps[2], start + int64_t(clip.timestampsUs[2] - clip.timestampsUs[0]) * 10);
        CHECK_EQ(convert({in, "-o", pathToUtf8(tmp / "nt.ser"), "--no-timestamps", "--no-sidecar"}), 0);
        auto nt = testdata::readSerFile(tmp / "nt.ser");
        CHECK(nt.timestamps.empty());
        CHECK(nt.dateTimeUtc == frameUtc(0, 0));
        CHECK(!fs::exists(tmp / "nt.serconv.txt"));
    }

    // ---- threads give identical files
    {
        testdata::MlvSpec big;
        big.width = 128;
        big.height = 64;
        big.frames = 25;
        big.lj92 = true;
        auto c2 = testdata::writeMlv(tmp / "many.MLV", big);
        CHECK_EQ(convert({pathToUtf8(c2.files[0]), "-o", pathToUtf8(tmp / "j1.ser"), "-j", "1"}), 0);
        CHECK_EQ(convert({pathToUtf8(c2.files[0]), "-o", pathToUtf8(tmp / "j4.ser"), "-j", "4"}), 0);
        CHECK(testdata::readAll(tmp / "j1.ser") == testdata::readAll(tmp / "j4.ser"));
    }

    // ---- planet tracking keeps the planet in the middle
    {
        testdata::MlvSpec ps;
        ps.width = 320;
        ps.height = 200;
        ps.frames = 8;
        ps.sceneRadius = 20;
        ps.sceneX = 100;
        ps.sceneY = 80;
        auto c3 = testdata::writeMlv(tmp / "planet.MLV", ps);
        CHECK_EQ(convert({pathToUtf8(c3.files[0]), "-o", pathToUtf8(tmp / "planet.ser"), "--center", "64x48"}), 0);
        auto pl = testdata::readSerFile(tmp / "planet.ser");
        CHECK_EQ(pl.width, 64);
        CHECK_EQ(pl.height, 48);
        CHECK_EQ(pl.colorId, 8);
        for (const auto& fr : pl.frames) {
            double sx = 0, sy = 0, sw = 0;
            for (int y = 0; y < 48; ++y)
                for (int x = 0; x < 64; ++x) {
                    double v = fr[size_t(y) * 64 + size_t(x)];
                    if (v > 20000) {
                        sx += x * v;
                        sy += y * v;
                        sw += v;
                    }
                }
            CHECK(sw > 0);
            if (sw > 0) {
                CHECK_NEAR(sx / sw, 32, 2.5);
                CHECK_NEAR(sy / sw, 24, 2.5);
            }
        }
    }

    // ---- DNG folder -> SER named after the folder; batch into a folder
    {
        for (int i = 0; i < 3; ++i) {
            testdata::DngSpec d;
            d.dateTimeOriginal = "2026:10:23 21:30:00";
            d.subSec = strprintf("%02d", i * 10);
            auto f = tmp / strprintf("night/IMG_%04d.dng", i + 7);
            fs::create_directories(f.parent_path());
            testdata::writeDng(f, d, testdata::makeFrame(d.width, d.height, 1, 16, uint32_t(i)));
        }
        CHECK_EQ(convert({pathToUtf8(tmp.path / "night"), "--utc-offset", "+00:00"}), 0);
        auto seq = testdata::readSerFile(tmp / "night.ser");
        CHECK_EQ(seq.frameCount, 3);
        CHECK_EQ(seq.timestamps.size(), size_t(3));
        CHECK_EQ(seq.timestamps[2] - seq.timestamps[0], int64_t(2000000));

        testdata::MlvSpec b;
        b.frames = 2;
        auto a1 = testdata::writeMlv(tmp / "batch/a.MLV", (fs::create_directories(tmp / "batch"), b));
        auto a2 = testdata::writeMlv(tmp / "batch/b.MLV", b);
        CHECK_EQ(convert({pathToUtf8(a1.files[0]), pathToUtf8(a2.files[0]), "-o", pathToUtf8(tmp / "batchout") + "/"}),
                 0);
        CHECK(fs::exists(tmp / "batchout/a.ser"));
        CHECK(fs::exists(tmp / "batchout/b.ser"));
        CHECK_EQ(convert({pathToUtf8(a1.files[0]), pathToUtf8(a2.files[0]), "-o", pathToUtf8(tmp / "single.ser")}), 1);
    }

    // ---- spanned MLV given by a part; two inputs that would write one file
    {
        testdata::MlvSpec m;
        m.chunks = 2;
        m.frames = 4;
        fs::create_directories(tmp / "span");
        auto c = testdata::writeMlv(tmp / "span/span.MLV", m);
        CHECK_EQ(c.files.size(), size_t(2));
        CHECK_EQ(convert({pathToUtf8(c.files[1])}), 0);  // the .M00 part
        CHECK_EQ(testdata::readSerFile(tmp / "span/span.ser").frameCount, 4);
        // Both parts named: still one conversion (a second would find span.ser).
        CHECK_EQ(convert({pathToUtf8(c.files[0]), pathToUtf8(c.files[1]), "--force"}), 0);
        fs::copy_file(c.files[1], tmp / "orphan.M00");
        CHECK_EQ(convert({pathToUtf8(tmp / "orphan.M00")}), 1);

        testdata::MlvSpec b;
        b.frames = 2;
        fs::create_directories(tmp / "x1");
        fs::create_directories(tmp / "x2");
        auto x1 = testdata::writeMlv(tmp / "x1/same.MLV", b);
        auto x2 = testdata::writeMlv(tmp / "x2/same.MLV", b);
        CHECK_EQ(convert({pathToUtf8(x1.files[0]), pathToUtf8(x2.files[0]), "-o", pathToUtf8(tmp / "clash") + "/"}),
                 1);
        CHECK(!fs::exists(tmp / "clash/same.ser"));
    }

    // ---- a file that records its time zone: --utc-offset is not applied
    {
        testdata::DngSpec d;
        d.dateTimeOriginal = "2026:10:23 23:30:00";
        d.subSec = "5";
        d.offsetTime = "+02:00";
        testdata::writeDng(tmp / "zoned.dng", d, testdata::makeFrame(d.width, d.height, 1, 16, 3));
        CHECK_EQ(convert({pathToUtf8(tmp / "zoned.dng"), "--utc-offset", "+05:00"}), 0);
        auto z = testdata::readSerFile(tmp / "zoned.ser");
        const int64_t utc = ticksFromCivil({2026, 10, 23, 21, 30, 0, 5000000});
        CHECK_EQ(z.dateTimeUtc, utc);
        CHECK_EQ(z.dateTime, utc + int64_t(2) * 3600 * kTicksPerSecond);  // the recorded zone
        CHECK(z.timestamps.size() == 1 && z.timestamps[0] == utc);
        std::string s = slurp(tmp / "zoned.serconv.txt");
        CHECK(s.find("not applied") != std::string::npos);
        CHECK(s.find("camera_time_zone = +02:00") != std::string::npos);
    }

    // ---- the binary itself
    if (!exe.empty()) {
        std::string out;
        CHECK_EQ(testutil::runProgram(exe, {"--version"}, &out, tmp.path), 0);
        CHECK(out.find("serconv ") != std::string::npos && out.find("LibRaw") != std::string::npos);
        CHECK_EQ(testutil::runProgram(exe, {"--help"}, &out, tmp.path), 0);
        CHECK(out.find("--center") != std::string::npos);
        CHECK_EQ(testutil::runProgram(exe, {"formats"}, &out, tmp.path), 0);
        CHECK_EQ(testutil::runProgram(exe, {"--bogus"}, &out, tmp.path), 2);
        CHECK_EQ(testutil::runProgram(exe, {pathToUtf8(tmp / "missing.MLV")}, &out, tmp.path), 1);
        CHECK(out.find("does not exist") != std::string::npos);
        CHECK_EQ(testutil::runProgram(exe, {"info", in, pathToUtf8(tmp / "clip.ser")}, &out, tmp.path), 0);
        CHECK(out.find("Magic Lantern MLV") != std::string::npos);
        CHECK(out.find("Timestamps:") != std::string::npos);
        CHECK(out.find("BAYER_RGGB") != std::string::npos);
        CHECK_EQ(testutil::runProgram(exe, {in, "-o", pathToUtf8(tmp / "bin.ser")}, &out, tmp.path), 0);
        CHECK(out.find("Wrote") != std::string::npos);
        CHECK(fs::exists(tmp / "bin.ser"));
        // "serconv ." inside a folder of frames writes <folder>.ser next to it.
        fs::remove(tmp / "night.ser");
        fs::remove(tmp / "night.serconv.txt");
        const fs::path cwd = fs::current_path();
        fs::current_path(tmp / "night");
        const int rcDot = testutil::runProgram(exe, {"."}, &out, tmp.path);
        fs::current_path(cwd);
        CHECK_EQ(rcDot, 0);
        CHECK(fs::exists(tmp / "night.ser"));
        CHECK(!fs::exists(tmp / "night/..ser"));
        // One input that fails: the error is printed once.
        testdata::writeAll(tmp / "broken.MLV", std::vector<uint8_t>(100, 0));
        CHECK_EQ(testutil::runProgram(exe, {pathToUtf8(tmp / "broken.MLV")}, &out, tmp.path), 1);
        const size_t e1 = out.find("not an MLV file");
        CHECK(e1 != std::string::npos);
        CHECK(e1 == std::string::npos || out.find("not an MLV file", e1 + 1) == std::string::npos);
    }

    return testutil::finish("test_cli");
}
