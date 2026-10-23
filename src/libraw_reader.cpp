// LibRaw-based readers: Canon Cinema RAW Light (CRM) and CR3 RAW burst rolls
// (one file, many frames), and sequences of still raw files (DNG/CinemaDNG,
// CR2, CR3, NEF, ARW, ...), one frame per file.

#include <libraw/libraw.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <ctime>
#include <mutex>

#include "bmff.hpp"
#include "reader.hpp"
#include "tiff_meta.hpp"
#include "timeutil.hpp"

namespace serconv {

std::string librawVersionString() { return LibRaw::version(); }
std::string librawCodecName(const std::string& fn);

namespace {

struct FrameRef {
    uint32_t file = 0;
    uint32_t shot = 0;
};

struct Shared {
    std::vector<fs::path> files;
    std::vector<FrameRef> refs;
    StreamInfo info;
    bool clip = false;               // one file with many frames
    std::vector<double> clipTimes;   // per-frame times for clips (may be empty)
    bool sequenceTimes = false;      // sequences: derive times from each file's EXIF
    int64_t sequenceOriginTicks = 0;
    double fps = 0;
};

int openFile(LibRaw& lr, const fs::path& path) {
#if defined(_WIN32) && defined(LIBRAW_WIN32_UNICODEPATHS)
    return lr.open_file(path.wstring().c_str());
#else
    return lr.open_file(path.string().c_str());
#endif
}

std::string lrError(int code) { return libraw_strerror(code); }

void configure(LibRaw& lr, unsigned shot) {
    lr.imgdata.rawparams.shot_select = shot;
    // 8K frames are ~70 MB; allow headroom for medium format stills.
    lr.imgdata.rawparams.max_raw_memory_mb = 8192;
}

std::string cameraName(const LibRaw& lr) {
    std::string make = trim(lr.imgdata.idata.make);
    std::string model = trim(lr.imgdata.idata.model);
    if (make.empty()) return model;
    if (startsWith(toLower(model), toLower(make))) return model;
    return make + " " + model;
}

// Black level of each 2x2 position (relative to the visible area origin).
Levels levelsOf(LibRaw& lr, Layout layout) {
    const libraw_colordata_t& c = lr.imgdata.rawdata.color;
    Levels lv;
    lv.known = true;
    lv.white = c.maximum > 0 ? double(c.maximum) : 65535.0;
    const unsigned pw = c.cblack[5], ph = c.cblack[4];
    double patternMean = 0;
    bool patternFits = true;
    if (pw && ph) {
        patternFits = (pw <= 2 && ph <= 2);
        for (unsigned i = 0; i < pw * ph && i < LIBRAW_CBLACK_SIZE - 6; ++i) patternMean += c.cblack[6 + i];
        patternMean /= double(pw * ph);
    }
    for (int p = 0; p < 4; ++p) {
        const int y = p >> 1, x = p & 1;
        double b = double(c.black);
        if (layout == Layout::Bayer) {
            int ci = lr.COLOR(y, x);
            if (ci >= 0 && ci < 4) b += c.cblack[ci];
        } else if (layout == Layout::RGB) {
            if (p < 3) b += c.cblack[p];
        } else {
            b += c.cblack[0];
        }
        if (pw && ph) b += patternFits ? double(c.cblack[6 + (y % ph) * pw + (x % pw)]) : patternMean;
        lv.black[p] = b;
    }
    return lv;
}

struct Decoded {
    Layout layout = Layout::Bayer;
    Cfa cfa = Cfa::RGGB;
    int width = 0, height = 0;
};

// Determines the layout of an unpacked image.
Decoded describe(LibRaw& lr, const std::string& name) {
    Decoded d;
    const auto& s = lr.imgdata.rawdata.sizes;
    d.width = s.width;
    d.height = s.height;
    const unsigned filters = lr.imgdata.idata.filters;
    if (lr.imgdata.rawdata.raw_image) {
        if (filters == 0 || lr.imgdata.idata.colors == 1) {
            d.layout = Layout::Mono;
        } else if (filters == 9) {
            fail(strprintf("'%s' is from an X-Trans sensor; its 6x6 colour pattern cannot be stored as a Bayer SER",
                           name.c_str()));
        } else if (filters < 1000) {
            fail(strprintf("'%s' has an unsupported colour filter layout", name.c_str()));
        } else {
            const char* cdesc = lr.imgdata.idata.cdesc;
            std::string four;
            for (int p = 0; p < 4; ++p) {
                int ci = lr.COLOR(p >> 1, p & 1);
                four += (ci >= 0 && ci < 4 && cdesc[ci]) ? cdesc[ci] : '?';
            }
            // The pattern must repeat every 2x2 pixels.
            for (int y = 0; y < 8; ++y)
                for (int x = 0; x < 2; ++x)
                    if (lr.COLOR(y, x) != lr.COLOR(y & 1, x & 1) &&
                        !(cdesc[lr.COLOR(y, x)] == cdesc[lr.COLOR(y & 1, x & 1)]))
                        fail(strprintf("'%s' has a colour pattern that does not repeat every 2x2 pixels",
                                       name.c_str()));
            if (!cfaFromColors(four, d.cfa)) {
                fail(strprintf("'%s' has a %s colour filter, which SER cannot describe", name.c_str(), four.c_str()));
            }
            d.layout = Layout::Bayer;
        }
    } else if (lr.imgdata.rawdata.color3_image || lr.imgdata.rawdata.color4_image) {
        d.layout = Layout::RGB;
    } else if (lr.imgdata.rawdata.float_image) {
        fail(strprintf("'%s' holds floating-point data, which serconv does not convert", name.c_str()));
    } else {
        fail(strprintf("'%s': LibRaw returned no image data", name.c_str()));
    }
    return d;
}

void extract(LibRaw& lr, const Decoded& d, RawFrame& frame) {
    const auto& s = lr.imgdata.rawdata.sizes;
    const int w = d.width, h = d.height;
    const int top = s.top_margin, left = s.left_margin;
    if (top + h > s.raw_height || left + w > s.raw_width) fail("LibRaw reported an inconsistent image size");
    frame.width = w;
    frame.height = h;
    frame.layout = d.layout;
    frame.cfa = d.cfa;
    frame.data.resize(frame.sampleCount());
    const size_t pitch = s.raw_pitch;  // bytes per row
    if (d.layout == Layout::RGB) {
        if (lr.imgdata.rawdata.color4_image) {
            const uint8_t* base = reinterpret_cast<const uint8_t*>(lr.imgdata.rawdata.color4_image);
            for (int y = 0; y < h; ++y) {
                const uint16_t* src = reinterpret_cast<const uint16_t*>(base + size_t(y + top) * pitch) + size_t(left) * 4;
                uint16_t* dst = frame.data.data() + size_t(y) * size_t(w) * 3;
                for (int x = 0; x < w; ++x) {
                    dst[x * 3 + 0] = src[x * 4 + 0];
                    dst[x * 3 + 1] = src[x * 4 + 1];
                    dst[x * 3 + 2] = src[x * 4 + 2];
                }
            }
        } else {
            const uint8_t* base = reinterpret_cast<const uint8_t*>(lr.imgdata.rawdata.color3_image);
            for (int y = 0; y < h; ++y) {
                const uint16_t* src = reinterpret_cast<const uint16_t*>(base + size_t(y + top) * pitch) + size_t(left) * 3;
                std::memcpy(frame.data.data() + size_t(y) * size_t(w) * 3, src, size_t(w) * 3 * 2);
            }
        }
    } else {
        const uint8_t* base = reinterpret_cast<const uint8_t*>(lr.imgdata.rawdata.raw_image);
        for (int y = 0; y < h; ++y) {
            const uint16_t* src = reinterpret_cast<const uint16_t*>(base + size_t(y + top) * pitch) + left;
            std::memcpy(frame.data.data() + size_t(y) * size_t(w), src, size_t(w) * 2);
        }
    }
}

int bitsFor(double white) {
    int bits = 1;
    while (bits < 16 && double((1u << bits) - 1) < white) ++bits;
    return std::max(bits, 8);
}

// Capture time of a still raw file (camera clock), from EXIF where possible.
// Capture time of a still file.
struct CaptureTime {
    int64_t ticks = 0;       // UTC when zoneKnown, else the camera clock
    bool zoneKnown = false;  // an EXIF OffsetTime tag gave the time zone
    int64_t zoneTicks = 0;
    bool subsec = false;
    std::string tag;  // "DateTimeOriginal", "DateTimeDigitized", "DateTime" or "LibRaw"
};

bool stillCaptureTime(const fs::path& path, LibRaw* lr, CaptureTime& out) {
    out = CaptureTime();
    TiffMeta meta;
    bool parsed = false;
    try {
        File f(path, "rb");
        const std::string ext = lowerExtension(path);
        if (ext == ".cr3" || ext == ".crm") {
            BmffMovie movie;
            if (parseBmff(f, movie, nullptr) && !movie.cmt2.empty()) {
                parsed = readTiffMetaBuffer(movie.cmt2.data(), movie.cmt2.size(), meta);
            }
        } else {
            parsed = readTiffMetaFile(f, meta);
        }
    } catch (const Error&) {
        parsed = false;
    }
    if (parsed) {
        // The TIFF/EXIF tags were read: LibRaw's timestamp comes from the same
        // tags, so there is nothing more to find when they hold no time.
        if (!meta.hasTime) return false;
        out.ticks = meta.timeTicks;
        out.subsec = meta.hasSubsec;
        out.tag = meta.timeTag;
        if (meta.hasZone) {
            out.ticks -= meta.zoneTicks;
            out.zoneKnown = true;
            out.zoneTicks = meta.zoneTicks;
        }
        return true;
    }
    if (lr && lr->imgdata.other.timestamp > 0) {
        // Not a TIFF-based file (RAF, CRW, X3F, ...). LibRaw ran mktime() on
        // the camera's date fields; localtime() gives them back.
        std::time_t t = lr->imgdata.other.timestamp;
        std::tm tm{};
#if defined(_WIN32)
        localtime_s(&tm, &t);
#else
        localtime_r(&t, &tm);
#endif
        CivilTime c;
        c.year = tm.tm_year + 1900;
        c.month = tm.tm_mon + 1;
        c.day = tm.tm_mday;
        c.hour = tm.tm_hour;
        c.minute = tm.tm_min;
        c.second = tm.tm_sec;
        if (validCivil(c)) {
            out.ticks = ticksFromCivil(c);
            out.tag = "LibRaw";
            return true;
        }
    }
    return false;
}

double frameRateOf(const fs::path& path) {
    try {
        File f(path, "rb");
        TiffMeta meta;
        if (readTiffMetaFile(f, meta)) return meta.frameRate;
    } catch (const Error&) {
    }
    return 0;
}

class LibRawReader : public Reader {
public:
    explicit LibRawReader(std::shared_ptr<Shared> shared) : sh_(std::move(shared)), lr_(new LibRaw(LIBRAW_OPTIONS_NONE)) {}

    const StreamInfo& info() const override { return sh_->info; }
    std::unique_ptr<Reader> clone() const override { return std::make_unique<LibRawReader>(sh_); }

    double frameTime(int64_t index) const override {
        if (index < 0 || index >= int64_t(sh_->refs.size())) return NAN;
        if (sh_->clip) return size_t(index) < sh_->clipTimes.size() ? sh_->clipTimes[size_t(index)] : NAN;
        if (sh_->fps > 0) return double(index) / sh_->fps;
        return index == 0 && sh_->sequenceTimes ? 0.0 : NAN;
    }

    bool read(int64_t index, RawFrame& frame) override {
        if (index < 0 || index >= int64_t(sh_->refs.size())) return false;
        const FrameRef& ref = sh_->refs[size_t(index)];
        const fs::path& path = sh_->files[ref.file];
        const std::string name = pathToUtf8(path.filename());
        lr_->recycle();
        configure(*lr_, ref.shot);
        int r = openFile(*lr_, path);
        if (r != LIBRAW_SUCCESS) fail(strprintf("LibRaw cannot open '%s': %s", name.c_str(), lrError(r).c_str()));
        r = lr_->unpack();
        if (r != LIBRAW_SUCCESS) {
            fail(strprintf("LibRaw cannot decode frame %lld of '%s': %s", static_cast<long long>(ref.shot + 1),
                           name.c_str(), lrError(r).c_str()));
        }
        Decoded d = describe(*lr_, name);
        const StreamInfo& si = sh_->info;
        if (d.width != si.width || d.height != si.height || d.layout != si.layout ||
            (d.layout == Layout::Bayer && d.cfa != si.cfa)) {
            fail(strprintf("'%s' is %dx%d %s, unlike the first frame (%dx%d %s)", name.c_str(), d.width, d.height,
                           d.layout == Layout::Bayer ? cfaName(d.cfa) : layoutName(d.layout), si.width, si.height,
                           si.layout == Layout::Bayer ? cfaName(si.cfa) : layoutName(si.layout)));
        }
        extract(*lr_, d, frame);
        frame.index = index;
        frame.levels = levelsOf(*lr_, d.layout);
        frame.hasLevels = true;
        frame.time = NAN;
        if (sh_->clip) {
            if (size_t(index) < sh_->clipTimes.size()) frame.time = sh_->clipTimes[size_t(index)];
        } else if (sh_->fps > 0) {
            frame.time = double(index) / sh_->fps;
        } else if (sh_->sequenceTimes) {
            CaptureTime ct;
            if (stillCaptureTime(path, lr_.get(), ct) && ct.zoneKnown == sh_->info.zoneKnown) {
                frame.time = secondsFromTicks(ct.ticks - sh_->sequenceOriginTicks);
            }
        }
        lr_->recycle();
        return true;
    }

private:
    std::shared_ptr<Shared> sh_;
    std::unique_ptr<LibRaw> lr_;
};

// Opens the first frame and fills the stream description.
void describeStream(Shared& sh) {
    std::unique_ptr<LibRaw> lr(new LibRaw(LIBRAW_OPTIONS_NONE));
    const fs::path& path = sh.files[0];
    const std::string name = pathToUtf8(path.filename());
    configure(*lr, 0);
    int r = openFile(*lr, path);
    if (r != LIBRAW_SUCCESS) fail(strprintf("LibRaw cannot open '%s': %s", name.c_str(), lrError(r).c_str()));
    r = lr->unpack();
    if (r != LIBRAW_SUCCESS) fail(strprintf("LibRaw cannot decode '%s': %s", name.c_str(), lrError(r).c_str()));
    Decoded d = describe(*lr, name);

    StreamInfo& info = sh.info;
    info.camera = cameraName(*lr);
    info.lens = trim(lr->imgdata.lens.Lens);
    info.width = d.width;
    info.height = d.height;
    info.layout = d.layout;
    info.cfa = d.cfa;
    info.levels = levelsOf(*lr, d.layout);
    info.bits = bitsFor(info.levels.white);
    info.iso = lr->imgdata.other.iso_speed;
    info.exposure = lr->imgdata.other.shutter;
    info.aperture = lr->imgdata.other.aperture;
    info.focalLength = lr->imgdata.other.focal_len;
    info.codec = librawCodecName(lr->unpack_function_name());
    if (lr->imgdata.idata.dng_version) {
        unsigned v = lr->imgdata.idata.dng_version;
        info.notes.emplace_back("dng_version",
                                strprintf("%u.%u.%u.%u", v >> 24, (v >> 16) & 255, (v >> 8) & 255, v & 255));
    }
    if (sh.clip) {
        int count = std::max(1, int(lr->imgdata.idata.raw_count));
        for (int i = 0; i < count; ++i) sh.refs.push_back({0, uint32_t(i)});
    }
}

// Timing of a CRM/CR3 clip from its sample tables and Canon timed metadata.
void clipTiming(Shared& sh) {
    File f(sh.files[0], "rb");
    CanonTiming t = canonClipTiming(f, sh.refs.size());
    StreamInfo& info = sh.info;
    sh.clipTimes = t.times;
    if (t.fps > 0) info.fps = t.fps;
    info.clock = t.clock;
    info.originTicks = t.originTicks;
    info.timeSource = t.source;
    info.zoneKnown = t.zoneKnown;
    info.zoneTicks = t.zoneTicks;
    if (!t.warning.empty()) info.warnings.push_back(t.warning);
}

}  // namespace

std::string librawCodecName(const std::string& fn) {
    static const std::pair<const char*, const char*> names[] = {
        {"crxLoadRaw", "Canon CRX"},
        {"lossless_dng_load_raw", "DNG lossless JPEG"},
        {"packed_dng_load_raw", "DNG uncompressed"},
        {"uncompressed_fp_dng_load_raw", "DNG floating point"},
        {"lossy_dng_load_raw", "DNG lossy JPEG"},
        {"deflate_dng_load_raw", "DNG deflate"},
        {"lossless_jpeg_load_raw", "lossless JPEG"},
        {"nikon_load_raw", "Nikon lossless/lossy compressed"},
        {"sony_arw2_load_raw", "Sony ARW2 compressed"},
        {"fuji_compressed_load_raw", "Fujifilm compressed"},
    };
    for (const auto& n : names)
        if (fn.find(n.first) != std::string::npos) return n.second;
    std::string s = fn;
    if (s.size() > 2 && s.compare(s.size() - 2, 2, "()") == 0) s.resize(s.size() - 2);
    return "LibRaw " + s;
}

int librawFrameCount(const fs::path& path) {
    std::unique_ptr<LibRaw> lr(new LibRaw(LIBRAW_OPTIONS_NONE));
    configure(*lr, 0);
    if (openFile(*lr, path) != LIBRAW_SUCCESS) return -1;
    return int(lr->imgdata.idata.raw_count);
}

std::unique_ptr<Reader> openLibRawClip(const fs::path& path) {
    auto sh = std::make_shared<Shared>();
    sh->files.push_back(path);
    sh->clip = true;
    describeStream(*sh);
    const std::string ext = lowerExtension(path);
    sh->info.container = ext == ".crm" ? "Canon Cinema RAW Light" : (sh->refs.size() > 1 ? "Canon CR3 RAW burst roll" : "Canon CR3");
    sh->info.frameCount = int64_t(sh->refs.size());
    if (ext == ".crm" || ext == ".cr3") clipTiming(*sh);
    return std::make_unique<LibRawReader>(sh);
}

std::unique_ptr<Reader> openLibRawSequence(const std::vector<fs::path>& files) {
    if (files.empty()) fail("no input files");
    auto sh = std::make_shared<Shared>();
    sh->files = files;
    sh->clip = false;
    for (size_t i = 0; i < files.size(); ++i) sh->refs.push_back({uint32_t(i), 0});
    describeStream(*sh);
    StreamInfo& info = sh->info;
    const std::string ext = lowerExtension(files[0]);
    if (ext == ".dng") {
        info.container = files.size() > 1 ? "DNG sequence" : "DNG";
        double fr = frameRateOf(files[0]);
        if (fr > 0) {
            sh->fps = fr;
            info.fps = fr;
            info.container = files.size() > 1 ? "CinemaDNG sequence" : "CinemaDNG";
        }
    } else {
        info.container = files.size() > 1 ? "raw image sequence" : "raw image";
    }
    info.frameCount = int64_t(files.size());

    std::unique_ptr<LibRaw> lr(new LibRaw(LIBRAW_OPTIONS_NONE));
    configure(*lr, 0);
    bool opened = openFile(*lr, files[0]) == LIBRAW_SUCCESS;
    CaptureTime ct;
    if (stillCaptureTime(files[0], opened ? lr.get() : nullptr, ct)) {
        info.clock = ct.zoneKnown ? Clock::Utc : Clock::Camera;
        info.originTicks = ct.ticks;
        info.zoneKnown = ct.zoneKnown;
        info.zoneTicks = ct.zoneTicks;
        sh->sequenceOriginTicks = ct.ticks;
        sh->sequenceTimes = true;
        std::string what = ct.tag == "LibRaw" ? std::string("the camera's date fields (as read by LibRaw, 1 s)")
                                              : (ct.tag == "DateTime" ? "TIFF " : "EXIF ") + ct.tag +
                                                    (ct.subsec ? " with sub-seconds" : ", 1 s") +
                                                    (ct.zoneKnown ? " in time zone " + formatOffset(ct.zoneTicks) : "");
        if (sh->fps > 0) {
            info.timeSource = strprintf("capture time of the first file (%s) + CinemaDNG frame rate (%.3f fps)",
                                        what.c_str(), sh->fps);
        } else {
            info.timeSource = "capture time of each file (" + what + ")";
            if (!ct.subsec && files.size() > 1) {
                info.warnings.push_back("the files carry capture times to the second only; use --fps to space "
                                        "the frame times evenly");
            }
        }
        if (ct.tag == "DateTime") {
            info.warnings.push_back("the capture time comes from the TIFF DateTime tag (the files have no EXIF "
                                    "DateTimeOriginal); programs that edit files change that tag, so check the "
                                    "times or give --start-time");
        }
    }
    return std::make_unique<LibRawReader>(sh);
}

}  // namespace serconv
