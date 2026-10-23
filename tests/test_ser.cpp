// SER writer and reader against the byte layout of the specification.
#include <cstring>

#include "ser_file.hpp"
#include "test_util.hpp"
#include "testdata.hpp"
#include "timeutil.hpp"

using namespace serconv;

int main() {
    testutil::TempDir tmp("ser");

    // ---- 16-bit Bayer with timestamps
    {
        const auto file = tmp / "bayer.ser";
        SerHeader h;
        h.colorId = 8;
        h.width = 4;
        h.height = 3;
        h.pixelDepth = 16;
        h.observer = "Jurgen";
        h.instrument = std::string(50, 'I');  // longer than 40: truncated
        h.telescope = "C14 f/10 \xC3\xA9";    // non-ASCII: one '?' per character
        SerWriter w;
        w.open(file, h, false);
        CHECK(fs::exists(tmp / "bayer.ser.part"));
        CHECK(!fs::exists(file));
        std::vector<int64_t> stamps;
        for (int f = 0; f < 3; ++f) {
            std::vector<uint8_t> frame(4 * 3 * 2);
            for (size_t i = 0; i < frame.size() / 2; ++i) wr16le(&frame[i * 2], uint16_t(f * 1000 + i * 257));
            w.writeFrame(frame.data(), frame.size());
            stamps.push_back(ticksFromCivil({2026, 10, 23, 21, 14, 5, 0}) + f * 200000);
        }
        CHECK_THROWS(w.writeFrame(nullptr, 3));
        w.finish(stamps[0] + 2 * 3600 * kTicksPerSecond, stamps[0], stamps);
        CHECK(fs::exists(file));
        CHECK(!fs::exists(tmp / "bayer.ser.part"));

        std::vector<uint8_t> d = testdata::readAll(file);
        CHECK_EQ(d.size(), size_t(178 + 3 * 24 + 3 * 8));
        CHECK(std::memcmp(d.data(), "LUCAM-RECORDER", 14) == 0);
        CHECK_EQ(rd32le(&d[14]), 0u);  // LuID
        CHECK_EQ(rd32le(&d[18]), 8u);  // ColorID RGGB
        CHECK_EQ(rd32le(&d[22]), 0u);  // little-endian data, written as 0 like FireCapture/PIPP
        CHECK_EQ(rd32le(&d[26]), 4u);
        CHECK_EQ(rd32le(&d[30]), 3u);
        CHECK_EQ(rd32le(&d[34]), 16u);
        CHECK_EQ(rd32le(&d[38]), 3u);
        CHECK(std::memcmp(&d[42], "Jurgen", 6) == 0 && d[48] == 0);
        CHECK_EQ(fixedString(&d[82], 40), std::string(40, 'I'));
        CHECK_EQ(fixedString(&d[122], 40), std::string("C14 f/10 ?"));
        CHECK_EQ(int64_t(rd64le(&d[162])), stamps[0] + 2 * 3600 * kTicksPerSecond);
        CHECK_EQ(int64_t(rd64le(&d[170])), stamps[0]);
        // First sample of frame 1, little-endian.
        CHECK_EQ(rd16le(&d[178 + 24]), uint16_t(1000));
        // Trailer.
        for (int f = 0; f < 3; ++f) CHECK_EQ(int64_t(rd64le(&d[178 + 72 + size_t(f) * 8])), stamps[size_t(f)]);

        {
            SerReader r;  // closed before the file is replaced below (Windows)
            r.open(file);
            CHECK_EQ(r.frameCount(), int64_t(3));
            CHECK_EQ(r.timestamps().size(), size_t(3));
            CHECK(r.problems().empty());
            std::vector<uint16_t> f16;
            r.readFrame16(2, f16);
            CHECK_EQ(f16[1], uint16_t(2000 + 257));
        }

        // Refuses to overwrite, unless asked.
        SerWriter w2;
        CHECK_THROWS(w2.open(file, h, false));
        w2.open(file, h, true);
        std::vector<uint8_t> frame(24, 7);
        w2.writeFrame(frame.data(), frame.size());
        w2.finish(0, 0, {});
        SerReader r2;
        r2.open(file);
        CHECK_EQ(r2.frameCount(), int64_t(1));
        CHECK(r2.timestamps().empty());
    }

    // ---- an unfinished writer leaves nothing behind
    {
        const auto file = tmp / "aborted.ser";
        {
            SerWriter w;
            SerHeader h;
            h.width = 2;
            h.height = 2;
            w.open(file, h, false);
            uint8_t px[4] = {1, 2, 3, 4};
            w.writeFrame(px, 4);
        }
        CHECK(!fs::exists(file));
        CHECK(!fs::exists(tmp / "aborted.ser.part"));
    }

    // ---- 8-bit mono and 16-bit RGB frame sizes
    {
        SerHeader m;
        m.colorId = 0;
        m.width = 5;
        m.height = 3;
        m.pixelDepth = 8;
        CHECK_EQ(m.frameBytes(), int64_t(15));
        SerHeader rgb;
        rgb.colorId = 100;
        rgb.width = 5;
        rgb.height = 3;
        rgb.pixelDepth = 16;
        CHECK_EQ(rgb.frameBytes(), int64_t(90));
        SerHeader d14;
        d14.width = 2;
        d14.height = 2;
        d14.pixelDepth = 14;
        CHECK_EQ(d14.bytesPerSample(), 2);
    }

    // ---- reader: header frame count 0 and big-endian data
    {
        const auto file = tmp / "legacy.ser";
        uint8_t hdr[178];
        SerHeader h;
        h.colorId = 0;
        h.width = 2;
        h.height = 1;
        h.pixelDepth = 16;
        h.littleEndian = 1;  // big-endian samples
        h.frameCount = 0;
        encodeSerHeader(h, hdr);
        std::vector<uint8_t> d(hdr, hdr + 178);
        d.insert(d.end(), {0x12, 0x34, 0xAB, 0xCD, 0x00, 0x01, 0x00, 0x02});
        testdata::writeAll(file, d);
        SerReader r;
        r.open(file);
        CHECK_EQ(r.frameCount(), int64_t(2));
        CHECK(!r.problems().empty());
        CHECK(r.bigEndianData());
        std::vector<uint16_t> s;
        r.readFrame16(0, s);
        CHECK_EQ(s[0], uint16_t(0x1234));
        CHECK_EQ(s[1], uint16_t(0xABCD));
        r.readFrame16(1, s);
        CHECK_EQ(s[1], uint16_t(2));
    }

    // ---- not a SER file
    {
        const auto file = tmp / "junk.ser";
        testdata::writeAll(file, std::vector<uint8_t>(300, 0x41));
        SerReader r;
        CHECK_THROWS(r.open(file));
    }

    return testutil::finish("test_ser");
}
