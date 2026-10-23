#include "tiff_meta.hpp"

#include <cctype>
#include <cstring>
#include <utility>
#include <vector>

#include "timeutil.hpp"

namespace serconv {

namespace {

enum : uint16_t {
    kTagMake = 271,
    kTagModel = 272,
    kTagDateTime = 306,
    kTagExifIfd = 34665,
    kTagDateTimeOriginal = 36867,
    kTagDateTimeDigitized = 36868,
    kTagOffsetTime = 36880,
    kTagOffsetTimeOriginal = 36881,
    kTagOffsetTimeDigitized = 36882,
    kTagSubSecTime = 37520,
    kTagSubSecTimeOriginal = 37521,
    kTagSubSecTimeDigitized = 37522,
    kTagUniqueCameraModel = 50708,
    kTagFrameRate = 51044,
};

struct Reader {
    const ReadAt& read;
    uint64_t base;
    bool little;

    uint16_t u16(const uint8_t* p) const { return little ? rd16le(p) : rd16be(p); }
    uint32_t u32(const uint8_t* p) const { return little ? rd32le(p) : rd32be(p); }
};

int typeSize(uint16_t type) {
    switch (type) {
        case 1: case 2: case 6: case 7: return 1;
        case 3: case 8: return 2;
        case 4: case 9: case 11: case 13: return 4;
        case 5: case 10: case 12: return 8;
        default: return 0;
    }
}

struct Entry {
    uint16_t tag, type;
    uint32_t count;
    uint8_t raw[4];
};

bool entryData(const Reader& r, const Entry& e, std::vector<uint8_t>& out, size_t maxBytes = 1 << 16) {
    int ts = typeSize(e.type);
    if (ts == 0) return false;
    uint64_t n = uint64_t(ts) * e.count;
    if (n == 0 || n > maxBytes) return false;
    out.resize(size_t(n));
    if (n <= 4) {
        std::memcpy(out.data(), e.raw, size_t(n));
        return true;
    }
    return r.read(r.base + r.u32(e.raw), out.data(), size_t(n));
}

std::string entryString(const Reader& r, const Entry& e) {
    std::vector<uint8_t> d;
    if ((e.type != 2 && e.type != 7 && e.type != 1) || !entryData(r, e, d)) return std::string();
    return fixedString(d.data(), d.size());
}

struct TimeTags {
    std::string when, subsec, offset;
};

struct Collected {
    TimeTags original, digitized, file;  // DateTimeOriginal, DateTimeDigitized, DateTime
    uint32_t exifIfd = 0;
};

void parseIfd(const Reader& r, uint32_t offset, TiffMeta& meta, Collected& c, bool exif) {
    uint8_t cnt[2];
    if (!r.read(r.base + offset, cnt, 2)) return;
    uint16_t n = r.u16(cnt);
    if (n == 0 || n > 4000) return;
    std::vector<uint8_t> buf(size_t(n) * 12);
    if (!r.read(r.base + offset + 2, buf.data(), buf.size())) return;
    for (uint16_t i = 0; i < n; ++i) {
        const uint8_t* p = &buf[size_t(i) * 12];
        Entry e;
        e.tag = r.u16(p);
        e.type = r.u16(p + 2);
        e.count = r.u32(p + 4);
        std::memcpy(e.raw, p + 8, 4);
        switch (e.tag) {
            case kTagMake:
                if (!exif) meta.make = entryString(r, e);
                break;
            case kTagModel:
                if (!exif) meta.model = entryString(r, e);
                break;
            case kTagDateTime: c.file.when = entryString(r, e); break;
            case kTagDateTimeOriginal: c.original.when = entryString(r, e); break;
            case kTagDateTimeDigitized: c.digitized.when = entryString(r, e); break;
            case kTagSubSecTime: c.file.subsec = entryString(r, e); break;
            case kTagSubSecTimeOriginal: c.original.subsec = entryString(r, e); break;
            case kTagSubSecTimeDigitized: c.digitized.subsec = entryString(r, e); break;
            case kTagOffsetTime: c.file.offset = entryString(r, e); break;
            case kTagOffsetTimeOriginal: c.original.offset = entryString(r, e); break;
            case kTagOffsetTimeDigitized: c.digitized.offset = entryString(r, e); break;
            case kTagUniqueCameraModel:
                meta.uniqueCameraModel = entryString(r, e);
                break;
            case kTagExifIfd:
                if (!exif && (e.type == 4 || e.type == 13) && e.count >= 1) c.exifIfd = r.u32(e.raw);
                break;
            case kTagFrameRate: {
                std::vector<uint8_t> d;
                if ((e.type == 10 || e.type == 5) && entryData(r, e, d) && d.size() >= 8) {
                    double num = e.type == 10 ? double(int32_t(r.u32(&d[0]))) : double(r.u32(&d[0]));
                    double den = e.type == 10 ? double(int32_t(r.u32(&d[4]))) : double(r.u32(&d[4]));
                    if (den != 0 && num / den > 0) meta.frameRate = num / den;
                }
                break;
            }
            default:
                break;
        }
    }
}

}  // namespace

bool readTiffMeta(const ReadAt& read, uint64_t base, TiffMeta& meta) {
    uint8_t h[8];
    if (!read(base, h, 8)) return false;
    bool little;
    if (h[0] == 'I' && h[1] == 'I') {
        little = true;
    } else if (h[0] == 'M' && h[1] == 'M') {
        little = false;
    } else {
        return false;
    }
    Reader r{read, base, little};
    uint16_t magic = r.u16(h + 2);
    // 42: TIFF; 0x55: Panasonic RW2; "RO"/"RS": Olympus ORF.
    if (magic != 42 && magic != 0x55 && magic != 0x4F52 && magic != 0x5352) return false;
    uint32_t ifd0 = r.u32(h + 4);
    Collected c;
    parseIfd(r, ifd0, meta, c, false);
    if (c.exifIfd) parseIfd(r, c.exifIfd, meta, c, true);

    const std::pair<const TimeTags*, const char*> candidates[] = {
        {&c.original, "DateTimeOriginal"}, {&c.digitized, "DateTimeDigitized"}, {&c.file, "DateTime"}};
    for (const auto& cand : candidates) {
        const TimeTags& t = *cand.first;
        int64_t ticks = 0, zone = 0;
        bool hasZone = false;
        if (t.when.empty() || !parseDateTime(t.when, ticks, hasZone, zone)) continue;
        meta.hasTime = true;
        meta.timeTag = cand.second;
        std::string digits;
        for (char ch : t.subsec) {
            if (std::isdigit(static_cast<unsigned char>(ch))) digits.push_back(ch);
            else break;
        }
        if (!digits.empty()) {
            int64_t scale = kTicksPerSecond / 10, frac = 0;
            for (char ch : digits) {
                if (scale == 0) break;
                frac += int64_t(ch - '0') * scale;
                scale /= 10;
            }
            ticks += frac;
            meta.hasSubsec = true;
        }
        meta.timeTicks = ticks;
        int64_t off = 0;
        if (!t.offset.empty() && parseOffset(t.offset, off)) {
            meta.hasZone = true;
            meta.zoneTicks = off;
        }
        break;
    }
    return true;
}

bool readTiffMetaFile(File& file, TiffMeta& meta) {
    const int64_t size = file.size();
    ReadAt read = [&file, size](uint64_t offset, void* buf, size_t n) {
        if (offset + n > uint64_t(size)) return false;
        file.seek(int64_t(offset));
        return file.readSome(buf, n) == n;
    };
    return readTiffMeta(read, 0, meta);
}

bool readTiffMetaBuffer(const uint8_t* data, size_t size, TiffMeta& meta) {
    ReadAt read = [data, size](uint64_t offset, void* buf, size_t n) {
        if (offset + n > size) return false;
        std::memcpy(buf, data + offset, n);
        return true;
    };
    return readTiffMeta(read, 0, meta);
}

}  // namespace serconv
