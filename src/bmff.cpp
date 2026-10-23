#include "bmff.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>

#include "tiff_meta.hpp"

namespace serconv {

namespace {

// Canon's metadata uuid box inside moov (CNCV, CCTP, CMT1..CMT4, THMB).
const uint8_t kCanonUuid[16] = {0x85, 0xc0, 0xb6, 0x87, 0x82, 0x0f, 0x11, 0xe0,
                                0x81, 0x11, 0xf4, 0xce, 0x46, 0x2b, 0x6a, 0x48};

struct Box {
    std::string type;
    size_t start = 0;     // offset of the payload in the buffer
    size_t size = 0;      // payload size
    const uint8_t* uuid = nullptr;
};

// Iterates the boxes in buf[begin, end).
bool nextBox(const std::vector<uint8_t>& buf, size_t& pos, size_t end, Box& box) {
    if (pos + 8 > end) return false;
    uint64_t size = rd32be(&buf[pos]);
    box.type.assign(reinterpret_cast<const char*>(&buf[pos + 4]), 4);
    size_t header = 8;
    if (size == 1) {
        if (pos + 16 > end) return false;
        size = rd64be(&buf[pos + 8]);
        header = 16;
    } else if (size == 0) {
        size = end - pos;
    }
    if (size < header || pos + size > end) return false;
    box.uuid = nullptr;
    if (box.type == "uuid") {
        if (header + 16 > size) return false;
        box.uuid = &buf[pos + header];
        header += 16;
    }
    box.start = pos + header;
    box.size = size_t(size) - header;
    pos += size_t(size);
    return true;
}

struct Stsc {
    uint32_t firstChunk;
    uint32_t samplesPerChunk;
};

struct TrackTables {
    uint32_t fixedSize = 0;
    std::vector<uint32_t> sizes;
    std::vector<uint64_t> chunkOffsets;
    std::vector<Stsc> stsc;
};

void parseStbl(const std::vector<uint8_t>& b, size_t pos, size_t end, BmffTrack& t, TrackTables& tt) {
    Box box;
    while (nextBox(b, pos, end, box)) {
        const uint8_t* p = &b[box.start];
        const size_t n = box.size;
        if (box.type == "stsd" && n >= 8) {
            uint32_t entries = rd32be(p + 4);
            if (entries > 0 && n >= 16) {
                uint32_t esize = rd32be(p + 8);
                t.sampleEntry.assign(reinterpret_cast<const char*>(p + 12), 4);
                // Visual sample entry: width/height 24 bytes into the entry body.
                if (t.handler == "vide" || t.sampleEntry == "CRAW") {
                    if (esize >= 36 && n >= 8 + 36) {
                        t.width = rd16be(p + 8 + 32);
                        t.height = rd16be(p + 8 + 34);
                    }
                }
                // Canon raw entries carry a CMP1 box (the CRX codec header).
                if (t.sampleEntry == "CRAW") {
                    const size_t end = std::min<size_t>(n, 8 + size_t(esize));
                    for (size_t k = 8 + 8; k + 4 <= end; ++k) {
                        if (std::memcmp(p + k, "CMP1", 4) == 0) {
                            t.canonRaw = true;
                            break;
                        }
                    }
                }
            }
        } else if (box.type == "stts" && n >= 8) {
            uint32_t count = rd32be(p + 4);
            for (uint32_t i = 0; i < count && 8 + size_t(i) * 8 + 8 <= n; ++i) {
                t.stts.emplace_back(rd32be(p + 8 + i * 8), rd32be(p + 12 + i * 8));
            }
        } else if (box.type == "stsz" && n >= 12) {
            tt.fixedSize = rd32be(p + 4);
            t.sampleCount = rd32be(p + 8);
            if (tt.fixedSize == 0) {
                uint32_t count = t.sampleCount;
                if (12 + size_t(count) * 4 > n) count = uint32_t((n - 12) / 4);
                tt.sizes.resize(count);
                for (uint32_t i = 0; i < count; ++i) tt.sizes[i] = rd32be(p + 12 + size_t(i) * 4);
            }
        } else if (box.type == "stsc" && n >= 8) {
            uint32_t count = rd32be(p + 4);
            for (uint32_t i = 0; i < count && 8 + size_t(i) * 12 + 12 <= n; ++i) {
                tt.stsc.push_back({rd32be(p + 8 + i * 12), rd32be(p + 12 + i * 12)});
            }
        } else if (box.type == "stco" && n >= 8) {
            uint32_t count = rd32be(p + 4);
            for (uint32_t i = 0; i < count && 8 + size_t(i) * 4 + 4 <= n; ++i) {
                tt.chunkOffsets.push_back(rd32be(p + 8 + size_t(i) * 4));
            }
        } else if (box.type == "co64" && n >= 8) {
            uint32_t count = rd32be(p + 4);
            for (uint32_t i = 0; i < count && 8 + size_t(i) * 8 + 8 <= n; ++i) {
                tt.chunkOffsets.push_back(rd64be(p + 8 + size_t(i) * 8));
            }
        }
    }
}

void computeSampleOffsets(BmffTrack& t, const TrackTables& tt) {
    t.sampleSizes.clear();
    t.sampleOffsets.clear();
    // Sizes come from the stsz table (bounded by the box size); a fixed sample
    // size with an absurd count would only mean a damaged file.
    if (tt.fixedSize == 0 && t.sampleCount > tt.sizes.size()) t.sampleCount = uint32_t(tt.sizes.size());
    if (t.sampleCount > (1u << 26)) t.sampleCount = 1u << 26;
    if (t.sampleCount == 0 || tt.chunkOffsets.empty() || tt.stsc.empty()) return;
    t.sampleSizes.reserve(std::min<uint32_t>(t.sampleCount, 1u << 20));
    t.sampleOffsets.reserve(std::min<uint32_t>(t.sampleCount, 1u << 20));
    uint32_t sample = 0;
    size_t stscIndex = 0;
    for (size_t chunk = 0; chunk < tt.chunkOffsets.size() && sample < t.sampleCount; ++chunk) {
        while (stscIndex + 1 < tt.stsc.size() && tt.stsc[stscIndex + 1].firstChunk <= chunk + 1) ++stscIndex;
        uint64_t offset = tt.chunkOffsets[chunk];
        for (uint32_t k = 0; k < tt.stsc[stscIndex].samplesPerChunk && sample < t.sampleCount; ++k) {
            uint32_t size = tt.fixedSize ? tt.fixedSize : (sample < tt.sizes.size() ? tt.sizes[sample] : 0);
            t.sampleOffsets.push_back(offset);
            t.sampleSizes.push_back(size);
            offset += size;
            ++sample;
        }
    }
}

void parseTrak(const std::vector<uint8_t>& b, size_t pos, size_t end, BmffMovie& movie) {
    BmffTrack t;
    TrackTables tt;
    // Two passes: hdlr must be known before stsd is interpreted.
    Box box;
    std::vector<std::pair<size_t, size_t>> stbls;
    size_t p = pos;
    while (nextBox(b, p, end, box)) {
        if (box.type == "tkhd" && box.size >= 24) {
            const uint8_t* q = &b[box.start];
            t.trackId = q[0] == 1 ? rd32be(q + 20) : rd32be(q + 12);
        } else if (box.type == "mdia") {
            size_t mp = box.start, mend = box.start + box.size;
            Box mb;
            while (nextBox(b, mp, mend, mb)) {
                const uint8_t* q = &b[mb.start];
                if (mb.type == "mdhd" && mb.size >= 24) {
                    if (q[0] == 1 && mb.size >= 36) {
                        t.creationTime = rd64be(q + 4);
                        t.timescale = rd32be(q + 20);
                        t.duration = rd64be(q + 24);
                    } else {
                        t.creationTime = rd32be(q + 4);
                        t.timescale = rd32be(q + 12);
                        t.duration = rd32be(q + 16);
                    }
                } else if (mb.type == "hdlr" && mb.size >= 12) {
                    t.handler.assign(reinterpret_cast<const char*>(q + 8), 4);
                } else if (mb.type == "minf") {
                    size_t ip = mb.start, iend = mb.start + mb.size;
                    Box ib;
                    while (nextBox(b, ip, iend, ib)) {
                        if (ib.type == "stbl") stbls.emplace_back(ib.start, ib.start + ib.size);
                    }
                }
            }
        }
    }
    for (auto& s : stbls) parseStbl(b, s.first, s.second, t, tt);
    computeSampleOffsets(t, tt);
    movie.tracks.push_back(std::move(t));
}

void parseMoov(const std::vector<uint8_t>& b, BmffMovie& movie) {
    size_t pos = 0;
    Box box;
    while (nextBox(b, pos, b.size(), box)) {
        const uint8_t* q = &b[box.start];
        if (box.type == "mvhd" && box.size >= 20) {
            if (q[0] == 1 && box.size >= 32) {
                movie.creationTime = rd64be(q + 4);
                movie.timescale = rd32be(q + 20);
                movie.duration = rd64be(q + 24);
            } else {
                movie.creationTime = rd32be(q + 4);
                movie.timescale = rd32be(q + 12);
                movie.duration = rd32be(q + 16);
            }
        } else if (box.type == "trak") {
            parseTrak(b, box.start, box.start + box.size, movie);
        } else if (box.type == "uuid" && box.uuid && std::memcmp(box.uuid, kCanonUuid, 16) == 0) {
            size_t up = box.start, uend = box.start + box.size;
            Box ub;
            while (nextBox(b, up, uend, ub)) {
                if (ub.type == "CMT1") movie.cmt1.assign(b.begin() + long(ub.start), b.begin() + long(ub.start + ub.size));
                if (ub.type == "CMT2") movie.cmt2.assign(b.begin() + long(ub.start), b.begin() + long(ub.start + ub.size));
            }
        }
    }
}

}  // namespace

uint64_t BmffTrack::sampleTime(uint32_t index) const {
    uint64_t t = 0;
    uint32_t remaining = index;
    for (const auto& e : stts) {
        if (remaining <= e.first) return t + uint64_t(remaining) * e.second;
        t += uint64_t(e.first) * e.second;
        remaining -= e.first;
    }
    // Past the table: extend with the last delta.
    uint32_t last = stts.empty() ? 0 : stts.back().second;
    return t + uint64_t(remaining) * last;
}

const BmffTrack* BmffMovie::largestTrack(const std::string& sampleEntry) const {
    const BmffTrack* best = nullptr;
    for (const auto& t : tracks) {
        if (t.sampleEntry != sampleEntry) continue;
        if (!best || int64_t(t.width) * t.height > int64_t(best->width) * best->height ||
            (int64_t(t.width) * t.height == int64_t(best->width) * best->height && t.sampleCount > best->sampleCount)) {
            best = &t;
        }
    }
    return best;
}

const BmffTrack* BmffMovie::canonRawTrack() const {
    const BmffTrack* best = nullptr;
    for (const auto& t : tracks) {
        if (t.sampleEntry != "CRAW" || !t.canonRaw) continue;
        if (!best || int64_t(t.width) * t.height > int64_t(best->width) * best->height) best = &t;
    }
    return best ? best : largestTrack("CRAW");
}

const BmffTrack* BmffMovie::firstTrack(const std::string& sampleEntry) const {
    for (const auto& t : tracks)
        if (t.sampleEntry == sampleEntry) return &t;
    return nullptr;
}

bool parseBmff(File& file, BmffMovie& movie, std::string* error) {
    const int64_t fsize = file.size();
    int64_t pos = 0;
    bool haveMoov = false;
    while (pos + 8 <= fsize) {
        uint8_t h[16];
        file.readAt(pos, h, 8);
        uint64_t size = rd32be(h);
        std::string type(reinterpret_cast<const char*>(h + 4), 4);
        int64_t header = 8;
        if (size == 1) {
            if (pos + 16 > fsize) break;
            file.readAt(pos + 8, h + 8, 8);
            size = rd64be(h + 8);
            header = 16;
        } else if (size == 0) {
            size = uint64_t(fsize - pos);
        }
        if (size < uint64_t(header) || pos + int64_t(size) > fsize) {
            if (!haveMoov) {
                if (error) *error = "not an ISO media file (bad box structure)";
                return false;
            }
            break;
        }
        if (pos == 0) {
            if (type != "ftyp") {
                if (error) *error = "not an ISO media file (no ftyp box)";
                return false;
            }
            uint8_t brand[4];
            file.readAt(pos + header, brand, 4);
            movie.majorBrand.assign(reinterpret_cast<const char*>(brand), 4);
        }
        if (type == "moov") {
            const uint64_t payload = size - uint64_t(header);
            if (payload > (512ull << 20)) {
                if (error) *error = "moov box is unreasonably large";
                return false;
            }
            std::vector<uint8_t> buf(static_cast<size_t>(payload));
            file.readAt(pos + header, buf.data(), buf.size());
            parseMoov(buf, movie);
            haveMoov = true;
        }
        pos += int64_t(size);
    }
    if (!haveMoov) {
        if (error) *error = "no moov box";
        return false;
    }
    return true;
}

int64_t canonFrameCount(const fs::path& path) {
    try {
        File f(path, "rb");
        BmffMovie movie;
        if (!parseBmff(f, movie, nullptr)) return -1;
        const BmffTrack* raw = movie.canonRawTrack();
        return raw ? int64_t(raw->sampleCount) : -1;
    } catch (const Error&) {
        return -1;
    }
}

int64_t ticksFrom1904(uint64_t seconds) {
    CivilTime base;
    base.year = 1904;
    base.month = 1;
    base.day = 1;
    return ticksFromCivil(base) + int64_t(seconds) * kTicksPerSecond;
}

bool parseCtmdTime(const uint8_t* data, size_t size, CivilTime& time) {
    // Records: u32 size, u16 type, ... (little-endian), payload at +12.
    size_t pos = 0;
    while (pos + 12 <= size) {
        uint32_t recSize = rd32le(data + pos);
        uint16_t type = rd16le(data + pos + 4);
        if (recSize < 12 || pos + recSize > size) return false;
        if (type == 1 && recSize >= 12 + 10) {
            const uint8_t* p = data + pos + 12;
            CivilTime t;
            t.year = rd16le(p + 2);
            t.month = p[4];
            t.day = p[5];
            t.hour = p[6];
            t.minute = p[7];
            t.second = p[8];
            t.subTicks = int64_t(p[9]) * (kTicksPerSecond / 100);
            if (t.year < 1990 || t.year > 2200 || !validCivil(t)) return false;
            time = t;
            return true;
        }
        pos += recSize;
    }
    return false;
}

CanonTiming canonClipTiming(File& f, size_t n) {
    CanonTiming out;
    BmffMovie movie;
    std::string err;
    if (!parseBmff(f, movie, &err)) {
        out.warning = "cannot read the clip's sample tables (" + err + "); no frame times";
        return out;
    }
    std::string relative;
    const BmffTrack* raw = movie.canonRawTrack();
    if (raw && raw->timescale > 0 && !raw->stts.empty() && raw->sampleCount >= n && n > 1) {
        out.times.resize(n);
        for (size_t i = 0; i < n; ++i) out.times[i] = double(raw->sampleTime(uint32_t(i))) / raw->timescale;
        relative = "frame times from the sample table";
    }

    // The EXIF block says which time zone the camera clock was set to (when
    // the camera knows it); that turns the camera times into UTC.
    TiffMeta exif;
    const bool haveExif =
        !movie.cmt2.empty() && readTiffMetaBuffer(movie.cmt2.data(), movie.cmt2.size(), exif) && exif.hasTime;
    const bool zoneKnown = haveExif && exif.hasZone;
    const int64_t zone = zoneKnown ? exif.zoneTicks : 0;
    out.zoneKnown = zoneKnown;
    out.zoneTicks = zone;
    const double t0 = out.times.empty() ? 0.0 : out.times[0];

    // Absolute time: CTMD time stamp of the first frame (camera clock, 1/100 s).
    const BmffTrack* ctmd = movie.firstTrack("CTMD");
    auto ctmdTime = [&](size_t i, CivilTime& t) {
        if (!ctmd || i >= ctmd->sampleOffsets.size()) return false;
        const uint32_t size = ctmd->sampleSizes[i];
        if (size == 0 || size > (1u << 20)) return false;
        std::vector<uint8_t> buf(size);
        try {
            f.readAt(int64_t(ctmd->sampleOffsets[i]), buf.data(), buf.size());
        } catch (const Error&) {
            return false;
        }
        return parseCtmdTime(buf.data(), buf.size(), t);
    };
    CivilTime first;
    if (ctmdTime(0, first)) {
        out.clock = zoneKnown ? Clock::Utc : Clock::Camera;
        out.originTicks = ticksFromCivil(first) - zone - ticksFromSeconds(t0);
        out.source = zoneKnown ? "Canon CTMD time stamp (1/100 s) in the EXIF time zone " + formatOffset(zone)
                               : std::string("Canon CTMD time stamp (camera clock, 1/100 s)");
        // Without sample-table times, use every frame's CTMD stamp.
        if (out.times.empty() && n > 1) {
            std::vector<double> times(n, NAN);
            bool all = true;
            for (size_t i = 0; i < n && all; ++i) {
                CivilTime t;
                all = ctmdTime(i, t);
                if (all) times[i] = secondsFromTicks(ticksFromCivil(t) - zone - out.originTicks);
            }
            if (all) {
                out.times = times;
                relative = "frame times from the CTMD stamps";
            }
        }
    } else if (haveExif) {
        out.clock = zoneKnown ? Clock::Utc : Clock::Camera;
        out.originTicks = exif.timeTicks - zone - ticksFromSeconds(t0);
        out.source = std::string("EXIF DateTimeOriginal") + (exif.hasSubsec ? " with sub-seconds" : "") +
                     (zoneKnown ? " in time zone " + formatOffset(zone) : std::string(" (camera clock)"));
    } else if (movie.creationTime > 0) {
        // ISO media times are UTC (the Canon R6 writes them so).
        out.clock = Clock::Utc;
        out.originTicks = ticksFrom1904(movie.creationTime);
        out.source = "movie header creation time (UTC, 1 s)";
    }
    if (out.times.size() > 1) {
        const double span = out.times.back() - out.times.front();
        if (span > 0) out.fps = double(out.times.size() - 1) / span;
    }
    if (!relative.empty()) out.source += (out.source.empty() ? "" : "; ") + relative;
    return out;
}

}  // namespace serconv
