// Magic Lantern Video (MLV) reader.
//
// An MLV clip is a chain of blocks (MLVI file header, RAWI raw format, VIDF
// frames, RTCI clock, IDNT camera, EXPO exposure, LENS...) in a main .MLV
// file and, for long clips, the spanned files .M00, .M01, ... Uncompressed
// frames are 10, 12 or 14-bit samples packed MSB-first into little-endian
// 16-bit words (Canon's raw buffer layout); compressed frames ("LJ92") are
// lossless JPEG.

#include <algorithm>
#include <cctype>
#include <cstring>

#include "lj92.hpp"
#include "reader.hpp"
#include "timeutil.hpp"

namespace serconv {

namespace {

constexpr uint16_t kClassRaw = 0x01;
constexpr uint16_t kFlagLj92 = 0x20;
constexpr uint16_t kFlagDelta = 0x40;
constexpr uint16_t kFlagLzma = 0x80;
constexpr uint16_t kFlagJpeg2k = 0x10;
constexpr uint16_t kFlagMcraw = 0x100;

struct Chunk {
    fs::path path;
    int64_t size = 0;
};

struct FrameRef {
    uint16_t chunk = 0;
    int64_t offset = 0;
    uint32_t size = 0;
    uint32_t frameNumber = 0;
    uint64_t timestamp = 0;  // microseconds since recording start
};

struct MlvIndex {
    std::vector<Chunk> chunks;
    std::vector<FrameRef> frames;
    StreamInfo info;
    int bpp = 14;
    int xRes = 0;
    int yRes = 0;
    bool lj92 = false;
    std::vector<uint16_t> curve;
};

bool printableType(const uint8_t* t) {
    for (int i = 0; i < 4; ++i)
        if (t[i] < 32 || t[i] > 126) return false;
    return true;
}

std::string exposureText(double seconds) {
    if (seconds <= 0) return "";
    if (seconds < 1.0) return strprintf("1/%.0f s", 1.0 / seconds);
    return strprintf("%.2f s", seconds);
}

// Spanned files: NAME.M00 .. NAME.M99 next to NAME.MLV.
std::vector<fs::path> chunkFiles(const fs::path& mainFile) {
    std::vector<fs::path> out{mainFile};
    const fs::path dir = mainFile.parent_path();
    const std::string stem = pathToUtf8(mainFile.stem());
    std::error_code ec;
    for (int i = 0; i < 100; ++i) {
        fs::path found;
        for (const char* fmt : {"%s.M%02d", "%s.m%02d"}) {
            fs::path p = dir / pathFromUtf8(strprintf(fmt, stem.c_str(), i));
            if (fs::is_regular_file(p, ec)) {
                found = p;
                break;
            }
        }
        if (!found.empty()) out.push_back(found);
    }
    return out;
}

class MlvReader : public Reader {
public:
    explicit MlvReader(std::shared_ptr<const MlvIndex> idx) : idx_(std::move(idx)) {
        files_.resize(idx_->chunks.size());
    }

    const StreamInfo& info() const override { return idx_->info; }

    std::unique_ptr<Reader> clone() const override { return std::make_unique<MlvReader>(idx_); }

    double frameTime(int64_t index) const override {
        if (index < 0 || index >= int64_t(idx_->frames.size())) return NAN;
        return double(idx_->frames[size_t(index)].timestamp) / 1e6;
    }

    bool read(int64_t index, RawFrame& frame) override {
        if (index < 0 || index >= int64_t(idx_->frames.size())) return false;
        const FrameRef& fr = idx_->frames[size_t(index)];
        File& f = file(fr.chunk);
        buf_.resize(fr.size);
        f.readAt(fr.offset, buf_.data(), buf_.size());

        const int w = idx_->xRes, h = idx_->yRes;
        frame.index = index;
        frame.width = w;
        frame.height = h;
        frame.layout = Layout::Bayer;
        frame.cfa = idx_->info.cfa;
        frame.hasLevels = false;
        frame.time = double(fr.timestamp) / 1e6;
        const size_t count = size_t(w) * size_t(h);
        frame.data.resize(count);

        if (idx_->lj92) {
            decodeLj92(buf_.data(), buf_.size(), lj_);
            if (lj_.samples.size() != count) {
                fail(strprintf("MLV frame %lld: LJ92 image is %dx%dx%d samples, expected %dx%d",
                               static_cast<long long>(index + 1), lj_.width, lj_.height, lj_.components, w, h));
            }
            std::memcpy(frame.data.data(), lj_.samples.data(), count * 2);
        } else {
            unpack(buf_.data(), buf_.size(), idx_->bpp, frame.data.data(), count, index);
        }
        if (!idx_->curve.empty()) {
            const auto& c = idx_->curve;
            for (uint16_t& v : frame.data) v = v < c.size() ? c[v] : c.back();
        }
        return true;
    }

private:
    File& file(uint16_t chunk) {
        File& f = files_[chunk];
        if (!f.isOpen()) f.open(idx_->chunks[chunk].path, "rb");
        return f;
    }

    static void unpack(const uint8_t* src, size_t bytes, int bpp, uint16_t* dst, size_t count, int64_t index) {
        const size_t need = (count * size_t(bpp) + 7) / 8;
        if (bytes < need) {
            fail(strprintf("MLV frame %lld is truncated (%llu of %llu bytes)", static_cast<long long>(index + 1),
                           static_cast<unsigned long long>(bytes), static_cast<unsigned long long>(need)));
        }
        if (bpp == 16) {
            for (size_t i = 0; i < count; ++i) dst[i] = rd16le(src + i * 2);
            return;
        }
        // MSB-first bit stream over little-endian 16-bit words.
        const uint32_t mask = (1u << bpp) - 1;
        uint64_t acc = 0;
        int nbits = 0;
        size_t si = 0;
        for (size_t i = 0; i < count; ++i) {
            if (nbits < bpp) {
                uint32_t word = si + 1 < bytes ? rd16le(src + si) : (si < bytes ? src[si] : 0u);
                si += 2;
                acc = (acc << 16) | word;
                nbits += 16;
            }
            nbits -= bpp;
            dst[i] = uint16_t((acc >> nbits) & mask);
        }
    }

    std::shared_ptr<const MlvIndex> idx_;
    std::vector<File> files_;
    std::vector<uint8_t> buf_;
    Lj92Image lj_;
};

struct ScanState {
    bool haveMain = false;
    uint64_t guid = 0;
    uint16_t videoClass = 0;
    double fps = 0;
    uint32_t declaredFrames = 0;
    bool haveRawi = false;
    int32_t black = 0, white = 0, cfaPattern = 0, rawW = 0, rawH = 0;
    bool haveRtci = false;
    CivilTime rtci;
    uint64_t rtciTimestamp = 0;
    int16_t rtciGmtOff = 0;
    std::string camera, serial, lens;
    double iso = 0, exposure = 0, aperture = 0, focal = 0;
    uint32_t dualIso = 0;
    std::vector<std::string> infoStrings;
    std::vector<std::string> versions;
    std::vector<uint16_t> curve;
    bool truncated = false;            // the recording ends with an incomplete block or frame
    std::vector<std::string> damaged;  // files whose reading stopped at a damaged block
};

void scanChunk(MlvIndex& idx, uint16_t chunkNo, ScanState& st) {
    File f(idx.chunks[chunkNo].path, "rb");
    const int64_t size = f.size();
    idx.chunks[chunkNo].size = size;
    const std::string name = pathToUtf8(idx.chunks[chunkNo].path.filename());

    uint8_t hdr[52];
    if (size < 52) fail(strprintf("'%s' is too small to be an MLV file", name.c_str()));
    f.readAt(0, hdr, sizeof hdr);
    if (std::memcmp(hdr, "MLVI", 4) != 0) fail(strprintf("'%s' is not an MLV file (no MLVI header)", name.c_str()));
    const uint32_t hsize = rd32le(hdr + 4);
    const uint64_t guid = rd64le(hdr + 16);
    if (chunkNo == 0) {
        st.haveMain = true;
        st.guid = guid;
        st.videoClass = rd16le(hdr + 32);
        st.declaredFrames = rd32le(hdr + 36);
        uint32_t nom = rd32le(hdr + 44), den = rd32le(hdr + 48);
        if (den) st.fps = double(nom) / double(den);
    } else if (guid != st.guid) {
        logWarning(strprintf("'%s' belongs to another recording (different MLV file GUID); skipped", name.c_str()));
        return;
    }

    int64_t pos = hsize >= 52 ? int64_t(hsize) : 52;
    std::vector<uint8_t> payload;
    while (pos + 16 <= size) {
        uint8_t h[16];
        f.readAt(pos, h, 16);
        const uint32_t bsize = rd32le(h + 4);
        if (!printableType(h) || bsize < 16 || pos + int64_t(bsize) > size) {
            const bool incomplete = printableType(h) && bsize >= 16 && pos + int64_t(bsize) > size;
            if (incomplete) st.truncated = true;
            else st.damaged.push_back(name);
            logVerbose(strprintf("'%s': stopped reading at byte %lld (%s)", name.c_str(), static_cast<long long>(pos),
                                 incomplete ? "the last block is incomplete" : "damaged block header"));
            break;
        }
        const std::string type(reinterpret_cast<const char*>(h), 4);
        const uint64_t ts = rd64le(h + 8);
        const uint32_t plen = bsize - 16;
        auto loadPayload = [&](size_t maxLen) {
            payload.resize(std::min<size_t>(plen, maxLen));
            f.readAt(pos + 16, payload.data(), payload.size());
        };

        if (type == "VIDF") {
            if (bsize >= 32) {
                uint8_t v[16];
                f.readAt(pos + 16, v, 16);
                FrameRef fr;
                fr.chunk = chunkNo;
                fr.frameNumber = rd32le(v);
                const uint32_t space = rd32le(v + 12);
                if (space <= bsize - 32) {
                    fr.offset = pos + 32 + space;
                    fr.size = bsize - 32 - space;
                    fr.timestamp = ts;
                    idx.frames.push_back(fr);
                }
            }
        } else if (type == "RAWI") {
            loadPayload(164);
            if (payload.size() >= 164) {
                st.haveRawi = true;
                idx.xRes = rd16le(&payload[0]);
                idx.yRes = rd16le(&payload[2]);
                st.rawH = int32_t(rd32le(&payload[12]));
                st.rawW = int32_t(rd32le(&payload[16]));
                idx.bpp = int32_t(rd32le(&payload[28]));
                st.black = int32_t(rd32le(&payload[32]));
                st.white = int32_t(rd32le(&payload[36]));
                st.cfaPattern = int32_t(rd32le(&payload[80]));
            }
        } else if (type == "RTCI") {
            loadPayload(28);
            if (payload.size() >= 20 && !st.haveRtci) {
                CivilTime t;
                t.second = rd16le(&payload[0]);
                t.minute = rd16le(&payload[2]);
                t.hour = rd16le(&payload[4]);
                t.day = rd16le(&payload[6]);
                t.month = rd16le(&payload[8]) + 1;
                t.year = rd16le(&payload[10]) + 1900;
                st.rtciGmtOff = int16_t(rd16le(&payload[18]));
                if (validCivil(t)) {
                    st.haveRtci = true;
                    st.rtci = t;
                    st.rtciTimestamp = ts;
                }
            }
        } else if (type == "IDNT") {
            loadPayload(68);
            if (payload.size() >= 32) st.camera = fixedString(&payload[0], 32);
            if (payload.size() >= 68) st.serial = fixedString(&payload[36], 32);
        } else if (type == "EXPO") {
            loadPayload(24);
            if (payload.size() >= 24) {
                st.iso = rd32le(&payload[4]);
                st.exposure = double(rd64le(&payload[16])) / 1e6;
            }
        } else if (type == "LENS") {
            loadPayload(80);
            if (payload.size() >= 48) {
                st.focal = rd16le(&payload[0]);
                st.aperture = rd16le(&payload[4]) / 100.0;
                st.lens = fixedString(&payload[16], 32);
            }
        } else if (type == "DISO") {
            loadPayload(8);
            if (payload.size() >= 4) st.dualIso = rd32le(&payload[0]);
        } else if (type == "INFO") {
            loadPayload(1024);
            std::string s = fixedString(payload.data(), payload.size());
            if (!s.empty()) st.infoStrings.push_back(s);
        } else if (type == "VERS") {
            loadPayload(1024);
            if (payload.size() > 4) {
                std::string s = fixedString(payload.data() + 4, payload.size() - 4);
                if (!s.empty()) st.versions.push_back(s);
            }
        } else if (type == "CURV") {
            loadPayload(size_t(65536) * 2);
            st.curve.resize(payload.size() / 2);
            for (size_t i = 0; i < st.curve.size(); ++i) st.curve[i] = rd16le(&payload[i * 2]);
        }
        pos += int64_t(bsize);
    }
}

}  // namespace

std::unique_ptr<Reader> openMlvReader(const fs::path& pathIn) {
    fs::path path = pathIn;
    // A spanned file was given: use its main file.
    std::string ext = lowerExtension(path);
    if (ext.size() == 4 && ext[1] == 'm' && std::isdigit(static_cast<unsigned char>(ext[2]))) {
        std::error_code ec;
        for (const char* e : {".MLV", ".mlv"}) {
            fs::path m = path;
            m.replace_extension(e);
            if (fs::is_regular_file(m, ec)) {
                path = m;
                break;
            }
        }
    }

    auto idx = std::make_shared<MlvIndex>();
    for (const auto& p : chunkFiles(path)) idx->chunks.push_back({p, 0});

    ScanState st;
    for (size_t c = 0; c < idx->chunks.size(); ++c) scanChunk(*idx, uint16_t(c), st);

    const std::string name = pathToUtf8(path.filename());
    if ((st.videoClass & 0x0F) != kClassRaw) {
        fail(strprintf("'%s' does not contain raw video (MLV video class 0x%x)", name.c_str(), st.videoClass));
    }
    if (st.videoClass & (kFlagDelta | kFlagLzma)) {
        fail(strprintf("'%s' was compressed with mlv_dump (LZMA/delta); decompress it with mlv_dump first",
                       name.c_str()));
    }
    if (st.videoClass & (kFlagJpeg2k | kFlagMcraw)) {
        fail(strprintf("'%s' uses an MLV compression serconv does not read (video class 0x%x)", name.c_str(),
                       st.videoClass));
    }
    if (!st.haveRawi) fail(strprintf("'%s' has no RAWI block (raw format description)", name.c_str()));
    if (idx->xRes <= 0 || idx->yRes <= 0) fail(strprintf("'%s': invalid frame size", name.c_str()));
    idx->lj92 = (st.videoClass & kFlagLj92) != 0;
    if (!idx->lj92 && idx->bpp != 10 && idx->bpp != 12 && idx->bpp != 14 && idx->bpp != 16) {
        fail(strprintf("'%s': unsupported bit depth %d", name.c_str(), idx->bpp));
    }
    if (idx->frames.empty()) fail(strprintf("'%s' contains no video frames", name.c_str()));

    // Frames may be out of order across spanned files: order by frame number.
    std::stable_sort(idx->frames.begin(), idx->frames.end(), [](const FrameRef& a, const FrameRef& b) {
        return a.frameNumber < b.frameNumber;
    });
    idx->frames.erase(std::unique(idx->frames.begin(), idx->frames.end(),
                                  [](const FrameRef& a, const FrameRef& b) { return a.frameNumber == b.frameNumber; }),
                      idx->frames.end());
    // Drop a last frame that is too short (recording interrupted).
    if (!idx->lj92) {
        const size_t need = (size_t(idx->xRes) * size_t(idx->yRes) * size_t(idx->bpp) + 7) / 8;
        while (!idx->frames.empty() && idx->frames.back().size < need) {
            idx->frames.pop_back();
            st.truncated = true;
        }
        if (idx->frames.empty()) fail(strprintf("'%s' contains no complete frames", name.c_str()));
    }

    StreamInfo& info = idx->info;
    info.container = "Magic Lantern MLV";
    info.codec = idx->lj92 ? strprintf("LJ92 lossless, %d-bit", idx->bpp) : strprintf("uncompressed %d-bit", idx->bpp);
    if (idx->chunks.size() > 1) info.codec += strprintf(", %llu files", static_cast<unsigned long long>(idx->chunks.size()));
    info.camera = st.camera;
    info.cameraSerial = st.serial;
    info.lens = st.lens;
    info.width = idx->xRes;
    info.height = idx->yRes;
    info.layout = Layout::Bayer;
    info.bits = idx->bpp;
    info.frameCount = int64_t(idx->frames.size());
    info.fps = st.fps;
    info.iso = st.iso;
    info.exposure = st.exposure;
    info.aperture = st.aperture;
    info.focalLength = st.focal;

    // cfa_pattern holds the 2x2 colours as bytes (0 R, 1 G, 2 B), row by row.
    info.cfa = Cfa::RGGB;
    if (st.cfaPattern != 0) {
        std::string four;
        for (int i = 0; i < 4; ++i) {
            int c = (uint32_t(st.cfaPattern) >> (8 * i)) & 0xFF;
            four += c == 0 ? 'R' : c == 1 ? 'G' : c == 2 ? 'B' : '?';
        }
        if (!cfaFromColors(four, info.cfa)) {
            info.warnings.push_back(strprintf("unknown CFA pattern 0x%08x; assuming RGGB", uint32_t(st.cfaPattern)));
        }
    }

    // Levels. Some 10/12-bit recordings carry 14-bit black/white levels.
    double black = st.black, white = st.white;
    const double maxv = double((1 << idx->bpp) - 1);
    if (white > maxv && idx->bpp < 14) {
        const double scale = double(1 << (14 - idx->bpp));
        black /= scale;
        white /= scale;
        info.warnings.push_back(strprintf("black/white levels were given for 14-bit data; scaled to %d-bit", idx->bpp));
    }
    if (!(white > black) || white > 65535) {
        info.warnings.push_back(strprintf("implausible levels (black %d, white %d); using 0 and %d", st.black,
                                          st.white, int(maxv)));
        black = 0;
        white = maxv;
    }
    info.levels.setBlack(black);
    info.levels.white = white;
    info.levels.known = true;
    if (!st.curve.empty()) {
        idx->curve = st.curve;
        info.notes.emplace_back("linearization", strprintf("CURV table with %llu entries applied", static_cast<unsigned long long>(st.curve.size())));
    }

    if (st.haveRtci) {
        info.clock = Clock::Camera;
        info.originTicks = ticksFromCivil(st.rtci) - int64_t(st.rtciTimestamp) * 10;
        info.timeSource = "MLV RTCI camera clock (1 s resolution) + VIDF frame timestamps (1 us)";
        if (st.rtciGmtOff != 0) info.notes.emplace_back("rtci_gmt_offset", std::to_string(st.rtciGmtOff));
    } else {
        info.timeSource = "VIDF frame timestamps (no RTCI clock block)";
    }

    // Dropped frames show as gaps in the frame numbers.
    int64_t missing = 0;
    for (size_t i = 1; i < idx->frames.size(); ++i) {
        missing += int64_t(idx->frames[i].frameNumber) - int64_t(idx->frames[i - 1].frameNumber) - 1;
    }
    if (missing > 0) {
        info.warnings.push_back(strprintf("%lld frame(s) are missing from the recording (%s)%s",
                                          static_cast<long long>(missing),
                                          st.damaged.empty() ? "dropped by the camera" : "dropped by the camera or "
                                                                                          "lost in a damaged file",
                                          st.haveRtci ? "; the SER timestamps show the gaps" : ""));
        info.notes.emplace_back("dropped_frames", std::to_string(missing));
    }
    for (const std::string& d : st.damaged) {
        info.warnings.push_back(strprintf("'%s' has a damaged block; the frames after it could not be read",
                                          d.c_str()));
    }
    if (st.declaredFrames && st.declaredFrames > uint32_t(0) && idx->chunks.size() == 1 &&
        st.declaredFrames != uint32_t(idx->frames.size())) {
        info.notes.emplace_back("mlvi_frame_count", std::to_string(st.declaredFrames));
    }
    if (st.truncated) info.warnings.push_back("the recording ends with an incomplete block; it was ignored");
    if (st.dualIso) {
        info.warnings.push_back("this is a Dual ISO recording: alternate line pairs have different gain and "
                                "will show as bands");
    }
    if (st.exposure > 0) info.notes.emplace_back("exposure", exposureText(st.exposure));
    for (const auto& s : st.infoStrings) info.notes.emplace_back("mlv_info", s);
    for (const auto& s : st.versions) info.notes.emplace_back("mlv_version", s);

    return std::make_unique<MlvReader>(idx);
}

}  // namespace serconv
