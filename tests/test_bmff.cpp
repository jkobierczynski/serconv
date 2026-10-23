// ISO media parsing and Canon clip timing on a synthetic CRM-like file.
#include <cstring>

#include "bmff.hpp"
#include "test_util.hpp"
#include "testdata.hpp"
#include "tiff_meta.hpp"
#include "timeutil.hpp"

using namespace serconv;

namespace {

using Bytes = std::vector<uint8_t>;

void be16(Bytes& b, uint32_t v) {
    b.push_back(uint8_t(v >> 8));
    b.push_back(uint8_t(v));
}
void be32(Bytes& b, uint32_t v) {
    for (int i = 3; i >= 0; --i) b.push_back(uint8_t(v >> (8 * i)));
}
void be64(Bytes& b, uint64_t v) {
    for (int i = 7; i >= 0; --i) b.push_back(uint8_t(v >> (8 * i)));
}
void le16(Bytes& b, uint32_t v) {
    b.push_back(uint8_t(v));
    b.push_back(uint8_t(v >> 8));
}
void le32(Bytes& b, uint32_t v) {
    for (int i = 0; i < 4; ++i) b.push_back(uint8_t(v >> (8 * i)));
}

Bytes box(const char* type, const Bytes& payload) {
    Bytes b;
    be32(b, uint32_t(8 + payload.size()));
    b.insert(b.end(), type, type + 4);
    b.insert(b.end(), payload.begin(), payload.end());
    return b;
}
Bytes cat(std::initializer_list<Bytes> parts) {
    Bytes b;
    for (const auto& p : parts) b.insert(b.end(), p.begin(), p.end());
    return b;
}
Bytes fullBox(const char* type, uint32_t versionFlags, const Bytes& rest) {
    Bytes p;
    be32(p, versionFlags);
    p.insert(p.end(), rest.begin(), rest.end());
    return box(type, p);
}

// A little-endian TIFF with the given ASCII tags in IFD0.
Bytes tiffWith(const std::vector<std::pair<uint16_t, std::string>>& tags) {
    Bytes t = {'I', 'I', 42, 0};
    le32(t, 8);
    le16(t, uint32_t(tags.size()));
    uint32_t dataAt = 8 + 2 + uint32_t(tags.size()) * 12 + 4;
    Bytes data;
    for (const auto& tg : tags) {
        le16(t, tg.first);
        le16(t, 2);
        le32(t, uint32_t(tg.second.size() + 1));
        Bytes s(tg.second.begin(), tg.second.end());
        s.push_back(0);
        if (s.size() <= 4) {
            s.resize(4, 0);
            t.insert(t.end(), s.begin(), s.end());
        } else {
            le32(t, dataAt + uint32_t(data.size()));
            data.insert(data.end(), s.begin(), s.end());
        }
    }
    le32(t, 0);
    t.insert(t.end(), data.begin(), data.end());
    return t;
}

Bytes ctmdSample(int year, int mon, int day, int h, int m, int s, int cs) {
    Bytes r;
    le32(r, 12 + 12);
    le16(r, 1);  // type 1: time stamp
    r.push_back(0);
    r.push_back(0);
    le16(r, 1);
    le16(r, 0);
    le16(r, 0);
    le16(r, uint32_t(year));
    r.push_back(uint8_t(mon));
    r.push_back(uint8_t(day));
    r.push_back(uint8_t(h));
    r.push_back(uint8_t(m));
    r.push_back(uint8_t(s));
    r.push_back(uint8_t(cs));
    r.push_back(0);
    r.push_back(0);
    // A type 5 record after it, as in real files.
    Bytes e;
    le32(e, 12 + 4);
    le16(e, 5);
    e.insert(e.end(), {0, 0, 1, 0, 0xFF, 0xFF, 1, 2, 3, 4});
    r.insert(r.end(), e.begin(), e.end());
    return r;
}

struct Built {
    Bytes file;
};

// frames: number of raw frames; timescale/delta for the raw track.
Bytes buildClip(int frames, uint32_t timescale, uint32_t delta, bool withCtmd, bool withExif, bool withZone,
                bool withStts) {
    // mdat contents: per frame 100 bytes of raw data, then CTMD samples.
    Bytes mdatPayload;
    std::vector<uint64_t> rawOff, ctmdOff;
    std::vector<uint32_t> ctmdSize;
    const uint64_t mdatStartGuess = 0;  // patched below
    (void)mdatStartGuess;
    for (int i = 0; i < frames; ++i) {
        rawOff.push_back(mdatPayload.size());
        mdatPayload.insert(mdatPayload.end(), 100, uint8_t(i));
    }
    for (int i = 0; i < frames; ++i) {
        Bytes c = ctmdSample(2026, 10, 23, 23, 14, 5 + (i * 4) / 100, (i * 4) % 100);
        ctmdOff.push_back(mdatPayload.size());
        ctmdSize.push_back(uint32_t(c.size()));
        mdatPayload.insert(mdatPayload.end(), c.begin(), c.end());
    }

    auto stbl = [&](const char* entry, bool raw, const std::vector<uint64_t>& offs, const std::vector<uint32_t>& sizes,
                    uint64_t base) {
        Bytes sampleEntry;
        Bytes body(6, 0);
        be16(body, 1);  // data reference index
        if (raw) {
            body.insert(body.end(), 16, 0);
            be16(body, 1920);
            be16(body, 1080);
            body.insert(body.end(), 50, 0);
            Bytes cmp1(32, 0);
            Bytes c = box("CMP1", cmp1);
            body.insert(body.end(), c.begin(), c.end());
        }
        sampleEntry = box(entry, body);
        Bytes stsd;
        be32(stsd, 1);
        stsd.insert(stsd.end(), sampleEntry.begin(), sampleEntry.end());
        Bytes stts;
        be32(stts, 1);
        be32(stts, uint32_t(offs.size()));
        be32(stts, delta);
        Bytes stsz;
        be32(stsz, 0);
        be32(stsz, uint32_t(offs.size()));
        for (uint32_t s : sizes) be32(stsz, s);
        Bytes stsc;
        be32(stsc, 1);
        be32(stsc, 1);
        be32(stsc, 1);
        be32(stsc, 1);
        Bytes co64;
        be32(co64, uint32_t(offs.size()));
        for (uint64_t o : offs) be64(co64, base + o);
        Bytes parts = cat({fullBox("stsd", 0, stsd)});
        if (withStts) parts = cat({parts, fullBox("stts", 0, stts)});
        return box("stbl", cat({parts, fullBox("stsz", 0, stsz), fullBox("stsc", 0, stsc), fullBox("co64", 0, co64)}));
    };
    auto trak = [&](uint32_t id, const char* handler, const Bytes& stblBox) {
        Bytes tkhd(20, 0);
        tkhd[11] = uint8_t(id);
        Bytes mdhd;
        be32(mdhd, 0);
        be32(mdhd, 0);
        be32(mdhd, timescale);
        be32(mdhd, delta * uint32_t(frames));
        be32(mdhd, 0);
        Bytes hdlr;
        be32(hdlr, 0);
        hdlr.insert(hdlr.end(), handler, handler + 4);
        hdlr.insert(hdlr.end(), 12, 0);
        return box("trak", cat({fullBox("tkhd", 0, tkhd),
                                box("mdia", cat({fullBox("mdhd", 0, mdhd), fullBox("hdlr", 0, hdlr),
                                                 box("minf", stblBox)}))}));
    };

    // Lay out: ftyp, moov, mdat. Build moov twice: the second time with the real mdat offset.
    Bytes ftyp = box("ftyp", Bytes{'c', 'r', 'x', ' ', 0, 0, 0, 1, 'c', 'r', 'x', ' ', 'i', 's', 'o', 'm'});
    auto moovFor = [&](uint64_t mdatData) {
        Bytes mvhd;
        be32(mvhd, uint32_t(2082844800u + 1792790045u));  // creation: 2026-10-23T21:14:05Z
        be32(mvhd, 0);
        be32(mvhd, 1000);
        be32(mvhd, 1000);
        mvhd.insert(mvhd.end(), 80, 0);
        std::vector<uint32_t> rawSizes(size_t(frames), 100);
        Bytes moov = cat({fullBox("mvhd", 0, mvhd)});
        Bytes uuidPayload = {0x85, 0xc0, 0xb6, 0x87, 0x82, 0x0f, 0x11, 0xe0,
                             0x81, 0x11, 0xf4, 0xce, 0x46, 0x2b, 0x6a, 0x48};
        Bytes cmt1 = box("CMT1", tiffWith({{271, "Canon"}, {272, "Canon EOS R5"}}));
        std::vector<std::pair<uint16_t, std::string>> exifTags = {{36867, "2026:10:23 23:14:05"}, {37521, "50"}};
        if (withZone) exifTags.push_back({36881, "+02:00"});
        Bytes cmt2 = box("CMT2", tiffWith(exifTags));
        uuidPayload.insert(uuidPayload.end(), cmt1.begin(), cmt1.end());
        if (withExif) uuidPayload.insert(uuidPayload.end(), cmt2.begin(), cmt2.end());
        moov = cat({moov, box("uuid", uuidPayload)});
        moov = cat({moov, trak(1, "vide", stbl("CRAW", false, rawOff, rawSizes, mdatData))});  // JPEG-like preview
        moov = cat({moov, trak(2, "vide", stbl("CRAW", true, rawOff, rawSizes, mdatData))});
        if (withCtmd) moov = cat({moov, trak(3, "meta", stbl("CTMD", false, ctmdOff, ctmdSize, mdatData))});
        return box("moov", moov);
    };
    Bytes moov0 = moovFor(0);
    const uint64_t mdatData = ftyp.size() + moov0.size() + 8;
    Bytes moov = moovFor(mdatData);
    return cat({ftyp, moov, box("mdat", mdatPayload)});
}

}  // namespace

int main() {
    testutil::TempDir tmp("bmff");
    const int n = 10;

    // Sample table at 25 fps (timescale 25000, delta 1000), CTMD and a time zone.
    {
        auto file = tmp / "clip.crm";
        testdata::writeAll(file, buildClip(n, 25000, 1000, true, true, true, true));
        File f(file, "rb");
        BmffMovie m;
        std::string err;
        CHECK(parseBmff(f, m, &err));
        CHECK_EQ(m.majorBrand, std::string("crx "));
        CHECK_EQ(m.tracks.size(), size_t(3));
        const BmffTrack* raw = m.canonRawTrack();
        CHECK(raw != nullptr);
        if (raw) {
            CHECK_EQ(raw->trackId, 2u);
            CHECK(raw->canonRaw);
            CHECK_EQ(raw->width, 1920);
            CHECK_EQ(raw->height, 1080);
            CHECK_EQ(raw->sampleCount, uint32_t(n));
            CHECK_EQ(raw->sampleTime(3), uint64_t(3000));
            CHECK_EQ(raw->sampleOffsets.size(), size_t(n));
        }
        CHECK(!m.tracks[0].canonRaw);
        TiffMeta t1;
        CHECK(readTiffMetaBuffer(m.cmt1.data(), m.cmt1.size(), t1));
        CHECK_EQ(t1.model, std::string("Canon EOS R5"));
        CHECK_EQ(formatTicks(ticksFrom1904(m.creationTime), true, 0), std::string("2026-10-23T21:14:05Z"));

        CanonTiming t = canonClipTiming(f, size_t(n));
        CHECK(t.clock == Clock::Utc);  // CTMD camera time with the +02:00 EXIF zone
        CHECK_EQ(t.originTicks, ticksFromCivil({2026, 10, 23, 21, 14, 5, 0}));
        CHECK_EQ(t.times.size(), size_t(n));
        if (t.times.size() == size_t(n)) CHECK_NEAR(t.times[9], 0.36, 1e-12);
        CHECK_NEAR(t.fps, 25.0, 1e-9);
        CHECK(t.source.find("CTMD") != std::string::npos);
        CHECK(t.zoneKnown);
        CHECK_EQ(t.zoneTicks, int64_t(2) * 3600 * kTicksPerSecond);
        // The quick frame count used to tell RAW burst rolls from stills.
        CHECK_EQ(canonFrameCount(file), int64_t(n));
        CHECK_EQ(canonFrameCount(tmp / "does-not-exist.cr3"), int64_t(-1));
    }

    // No time zone: the camera clock.
    {
        auto file = tmp / "nozone.crm";
        testdata::writeAll(file, buildClip(n, 25000, 1000, true, true, false, true));
        File f(file, "rb");
        CanonTiming t = canonClipTiming(f, size_t(n));
        CHECK(t.clock == Clock::Camera);
        CHECK(!t.zoneKnown);
        CHECK_EQ(t.originTicks, ticksFromCivil({2026, 10, 23, 23, 14, 5, 0}));
    }

    // No sample-table times: every frame's CTMD stamp (4 hundredths apart).
    {
        auto file = tmp / "nostts.crm";
        testdata::writeAll(file, buildClip(n, 25000, 1000, true, true, true, false));
        File f(file, "rb");
        CanonTiming t = canonClipTiming(f, size_t(n));
        CHECK_EQ(t.times.size(), size_t(n));
        if (t.times.size() == size_t(n)) CHECK_NEAR(t.times[5], 0.20, 1e-9);
        CHECK(t.source.find("CTMD stamps") != std::string::npos);
    }

    // No CTMD: EXIF with sub-seconds; neither: the movie header (UTC).
    {
        auto file = tmp / "exif.crm";
        testdata::writeAll(file, buildClip(n, 25000, 1000, false, true, true, true));
        File f(file, "rb");
        CanonTiming t = canonClipTiming(f, size_t(n));
        CHECK(t.clock == Clock::Utc);
        CHECK_EQ(t.originTicks, ticksFromCivil({2026, 10, 23, 21, 14, 5, 5000000}));
        auto file2 = tmp / "bare.crm";
        testdata::writeAll(file2, buildClip(n, 25000, 1000, false, false, false, true));
        File f2(file2, "rb");
        CanonTiming t2 = canonClipTiming(f2, size_t(n));
        CHECK(t2.clock == Clock::Utc);
        CHECK_EQ(t2.originTicks, ticksFromCivil({2026, 10, 23, 21, 14, 5, 0}));
    }

    // Not ISO media.
    {
        auto file = tmp / "junk.crm";
        testdata::writeAll(file, Bytes(64, 0x55));
        File f(file, "rb");
        BmffMovie m;
        std::string err;
        CHECK(!parseBmff(f, m, &err));
        CHECK(!err.empty());
        CHECK_EQ(canonFrameCount(file), int64_t(-1));
        CanonTiming t = canonClipTiming(f, 5);
        CHECK(t.clock == Clock::None);
        CHECK(!t.warning.empty());
    }

    // TIFF meta in big-endian byte order.
    {
        Bytes t = {'M', 'M', 0, 42, 0, 0, 0, 8, 0, 1};
        // DateTimeOriginal, ASCII, 20 bytes at offset 26
        t.insert(t.end(), {0x90, 0x03, 0, 2, 0, 0, 0, 20, 0, 0, 0, 26, 0, 0, 0, 0});
        const char* when = "2025:01:02 03:04:05";
        t.insert(t.end(), when, when + 20);
        TiffMeta m;
        CHECK(readTiffMetaBuffer(t.data(), t.size(), m));
        CHECK(m.hasTime);
        CHECK_EQ(m.timeTicks, ticksFromCivil({2025, 1, 2, 3, 4, 5, 0}));
        CHECK_EQ(m.timeTag, std::string("DateTimeOriginal"));
    }

    return testutil::finish("test_bmff");
}
