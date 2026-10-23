// FFmpeg-based reader: Apple ProRes RAW (MOV, FFmpeg 8.0+) and any other
// stream that FFmpeg decodes to Bayer, grey or RGB samples (CineForm RAW,
// raw Bayer video, MLV through FFmpeg's demuxer, ...).

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/avutil.h>
#include <libavutil/pixdesc.h>
#include <libavutil/version.h>
}

#if LIBAVUTIL_VERSION_INT >= AV_VERSION_INT(60, 33, 100)
extern "C" {
#include <libavutil/raw_color_params.h>
}
#define SERCONV_RAW_COLOR_PARAMS 1
#endif

#include <cstring>
#include <deque>
#include <utility>

#include "reader.hpp"
#include "timeutil.hpp"

namespace serconv {

std::string ffmpegVersionString() {
    return strprintf("%s (libavcodec %d.%d.%d)", av_version_info(), LIBAVCODEC_VERSION_MAJOR,
                     LIBAVCODEC_VERSION_MINOR, LIBAVCODEC_VERSION_MICRO);
}

bool ffmpegHasProResRaw() { return avcodec_find_decoder_by_name("prores_raw") != nullptr; }

namespace {

std::string averr(int code) {
    char buf[AV_ERROR_MAX_STRING_SIZE] = {0};
    av_strerror(code, buf, sizeof buf);
    return buf;
}

struct PixInfo {
    Layout layout;
    Cfa cfa;
    int bits;
    bool bigEndian;
    bool planarGbr;
    bool bgr;
};

bool pixInfo(AVPixelFormat f, PixInfo& p) {
    p = {Layout::Bayer, Cfa::RGGB, 16, false, false, false};
    switch (f) {
        case AV_PIX_FMT_BAYER_RGGB16LE: p.cfa = Cfa::RGGB; return true;
        case AV_PIX_FMT_BAYER_BGGR16LE: p.cfa = Cfa::BGGR; return true;
        case AV_PIX_FMT_BAYER_GBRG16LE: p.cfa = Cfa::GBRG; return true;
        case AV_PIX_FMT_BAYER_GRBG16LE: p.cfa = Cfa::GRBG; return true;
        case AV_PIX_FMT_BAYER_RGGB16BE: p.cfa = Cfa::RGGB; p.bigEndian = true; return true;
        case AV_PIX_FMT_BAYER_BGGR16BE: p.cfa = Cfa::BGGR; p.bigEndian = true; return true;
        case AV_PIX_FMT_BAYER_GBRG16BE: p.cfa = Cfa::GBRG; p.bigEndian = true; return true;
        case AV_PIX_FMT_BAYER_GRBG16BE: p.cfa = Cfa::GRBG; p.bigEndian = true; return true;
        case AV_PIX_FMT_BAYER_RGGB8: p.cfa = Cfa::RGGB; p.bits = 8; return true;
        case AV_PIX_FMT_BAYER_BGGR8: p.cfa = Cfa::BGGR; p.bits = 8; return true;
        case AV_PIX_FMT_BAYER_GBRG8: p.cfa = Cfa::GBRG; p.bits = 8; return true;
        case AV_PIX_FMT_BAYER_GRBG8: p.cfa = Cfa::GRBG; p.bits = 8; return true;
        case AV_PIX_FMT_GRAY8: p.layout = Layout::Mono; p.bits = 8; return true;
        case AV_PIX_FMT_GRAY16LE: p.layout = Layout::Mono; return true;
        case AV_PIX_FMT_GRAY16BE: p.layout = Layout::Mono; p.bigEndian = true; return true;
        case AV_PIX_FMT_GRAY10LE: p.layout = Layout::Mono; p.bits = 10; return true;
        case AV_PIX_FMT_GRAY12LE: p.layout = Layout::Mono; p.bits = 12; return true;
        case AV_PIX_FMT_GRAY14LE: p.layout = Layout::Mono; p.bits = 14; return true;
        case AV_PIX_FMT_GRAY10BE: p.layout = Layout::Mono; p.bits = 10; p.bigEndian = true; return true;
        case AV_PIX_FMT_GRAY12BE: p.layout = Layout::Mono; p.bits = 12; p.bigEndian = true; return true;
        case AV_PIX_FMT_GRAY14BE: p.layout = Layout::Mono; p.bits = 14; p.bigEndian = true; return true;
        case AV_PIX_FMT_RGB24: p.layout = Layout::RGB; p.bits = 8; return true;
        case AV_PIX_FMT_BGR24: p.layout = Layout::RGB; p.bits = 8; p.bgr = true; return true;
        case AV_PIX_FMT_RGB48LE: p.layout = Layout::RGB; return true;
        case AV_PIX_FMT_RGB48BE: p.layout = Layout::RGB; p.bigEndian = true; return true;
        case AV_PIX_FMT_BGR48LE: p.layout = Layout::RGB; p.bgr = true; return true;
        case AV_PIX_FMT_BGR48BE: p.layout = Layout::RGB; p.bgr = true; p.bigEndian = true; return true;
        case AV_PIX_FMT_GBRP16LE: p.layout = Layout::RGB; p.planarGbr = true; return true;
        case AV_PIX_FMT_GBRP16BE: p.layout = Layout::RGB; p.planarGbr = true; p.bigEndian = true; return true;
        case AV_PIX_FMT_GBRP: p.layout = Layout::RGB; p.bits = 8; p.planarGbr = true; return true;
        default: return false;
    }
}

std::string metaValue(AVDictionary* d, const char* key) {
    AVDictionaryEntry* e = av_dict_get(d, key, nullptr, 0);
    return e && e->value ? std::string(e->value) : std::string();
}

class FFmpegReader : public Reader {
public:
    explicit FFmpegReader(const fs::path& path) : path_(path) {
        // A throwing constructor runs no destructor: free what was allocated.
        try {
            init(path);
        } catch (...) {
            release();
            throw;
        }
    }

    ~FFmpegReader() override { release(); }

    const StreamInfo& info() const override { return info_; }
    bool randomAccess() const override { return false; }

private:
    void init(const fs::path& path) {
        const std::string name = pathToUtf8(path);
        int r = avformat_open_input(&fmt_, name.c_str(), nullptr, nullptr);
        if (r < 0) fail(strprintf("FFmpeg cannot open '%s': %s", name.c_str(), averr(r).c_str()));
        r = avformat_find_stream_info(fmt_, nullptr);
        if (r < 0) fail(strprintf("FFmpeg cannot read '%s': %s", name.c_str(), averr(r).c_str()));
        stream_ = av_find_best_stream(fmt_, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
        if (stream_ < 0) fail(strprintf("'%s' has no video stream", name.c_str()));
        AVStream* st = fmt_->streams[stream_];
        for (unsigned i = 0; i < fmt_->nb_streams; ++i)
            if (int(i) != stream_) fmt_->streams[i]->discard = AVDISCARD_ALL;

        const AVCodec* codec = avcodec_find_decoder(st->codecpar->codec_id);
        if (!codec) {
            const char* cname = avcodec_get_name(st->codecpar->codec_id);
            if (std::string(cname) == "prores_raw") {
                fail(strprintf("'%s' is ProRes RAW, but this FFmpeg (%s) has no ProRes RAW decoder; it needs "
                               "FFmpeg 8.0 or newer",
                               name.c_str(), av_version_info()));
            }
            fail(strprintf("FFmpeg has no decoder for '%s' (codec %s)", name.c_str(), cname));
        }
        dec_ = avcodec_alloc_context3(codec);
        if (!dec_) fail("out of memory");
        avcodec_parameters_to_context(dec_, st->codecpar);
        dec_->thread_count = 0;
        dec_->apply_cropping = 0;  // keep the full Bayer frame; crop flags would shift the pattern
        r = avcodec_open2(dec_, codec, nullptr);
        if (r < 0) fail(strprintf("FFmpeg cannot open the %s decoder: %s", codec->name, averr(r).c_str()));
        const AVCodecDescriptor* desc = avcodec_descriptor_get(st->codecpar->codec_id);
        intraOnly_ = desc && (desc->props & AV_CODEC_PROP_INTRA_ONLY);
        pkt_ = av_packet_alloc();
        frm_ = av_frame_alloc();
        timeBase_ = av_q2d(st->time_base);
        if (st->start_time != AV_NOPTS_VALUE) startPts_ = st->start_time;

        // The pixel format and levels are known once a frame is decoded.
        if (!decodeNext(0, cached_)) fail(strprintf("'%s': no frame could be decoded", name.c_str()));
        haveCached_ = true;

        info_.container = strprintf("%s (%s)", fmt_->iformat->long_name ? fmt_->iformat->long_name : fmt_->iformat->name,
                                    pathToUtf8(path.extension()).c_str());
        info_.codec = desc && desc->long_name ? desc->long_name : codec->name;
        if (const char* prof = avcodec_profile_name(st->codecpar->codec_id, st->codecpar->profile)) {
            info_.codec += std::string(", ") + prof;
        }
        info_.width = cached_.width;
        info_.height = cached_.height;
        info_.layout = cached_.layout;
        info_.cfa = cached_.cfa;
        info_.bits = bits_;
        info_.levels = cached_.levels;
        info_.frameCount = st->nb_frames > 0 ? int64_t(st->nb_frames) : -1;
        if (st->avg_frame_rate.num > 0 && st->avg_frame_rate.den > 0) info_.fps = av_q2d(st->avg_frame_rate);
        else if (st->r_frame_rate.num > 0 && st->r_frame_rate.den > 0) info_.fps = av_q2d(st->r_frame_rate);

        for (const char* key : {"com.apple.quicktime.make", "make", "com.apple.proapps.manufacturer"}) {
            std::string v = metaValue(fmt_->metadata, key);
            if (!v.empty()) {
                info_.camera = v;
                break;
            }
        }
        for (const char* key : {"com.apple.quicktime.model", "model", "com.apple.proapps.cameraName"}) {
            std::string v = metaValue(fmt_->metadata, key);
            if (!v.empty()) {
                info_.camera = info_.camera.empty() ? v : info_.camera + " " + v;
                break;
            }
        }
        std::string created = metaValue(st->metadata, "creation_time");
        if (created.empty()) created = metaValue(fmt_->metadata, "creation_time");
        int64_t ticks, zone;
        bool hasZone;
        if (!created.empty() && parseDateTime(created, ticks, hasZone, zone)) {
            // QuickTime says UTC, but some cameras and recorders write their
            // local time, so this counts as the camera clock: taken as UTC,
            // and --utc-offset corrects it when needed.
            info_.clock = Clock::Camera;
            info_.originTicks = ticks - zone;
            info_.timeSource = "container creation_time (UTC by the QuickTime specification; some cameras write "
                               "local time there) + frame timestamps";
        } else {
            info_.timeSource = "frame timestamps (no creation time in the container)";
        }
        std::string tc = metaValue(st->metadata, "timecode");
        if (tc.empty()) tc = metaValue(fmt_->metadata, "timecode");
        if (!tc.empty()) info_.notes.emplace_back("timecode", tc);
        if (std::string(codec->name) == "prores_raw" && !info_.levels.known) {
            info_.warnings.push_back(strprintf("FFmpeg %s does not linearize ProRes RAW or report its levels "
                                               "(FFmpeg 9.0 does); the frames keep the camera's encoding",
                                               av_version_info()));
        }
        if (std::string(codec->name) == "rawvideo" && st->codecpar->bits_per_coded_sample > 0 &&
            st->codecpar->bits_per_coded_sample < 16 && bits_ == 16) {
            info_.notes.emplace_back("ffmpeg_scaling", strprintf("%d-bit samples scaled to 16 bits by FFmpeg",
                                                                 st->codecpar->bits_per_coded_sample));
        }
    }

    void release() {
        av_frame_free(&frm_);
        av_packet_free(&pkt_);
        avcodec_free_context(&dec_);
        avformat_close_input(&fmt_);
    }

public:

    bool read(int64_t index, RawFrame& frame) override {
        if (haveCached_) {
            if (cached_.index == index) {
                frame = std::move(cached_);
                haveCached_ = false;
                return true;
            }
            haveCached_ = false;
        }
        if (index <= lastIndex_) fail("internal error: FFmpeg reader frames must be read in order");
        return decodeNext(index, frame);
    }

private:
    // Decodes until the frame with number `want` comes out.
    bool decodeNext(int64_t want, RawFrame& out) {
        for (;;) {
            int r = avcodec_receive_frame(dec_, frm_);
            if (r == 0) {
                int64_t idx;
                if (intraOnly_) {
                    if (pending_.empty()) fail("internal error: FFmpeg returned an unexpected frame");
                    idx = pending_.front();
                    pending_.pop_front();
                } else {
                    idx = outCount_++;
                }
                if (idx == want) {
                    convert(frm_, out);
                    out.index = idx;
                    lastIndex_ = idx;
                    av_frame_unref(frm_);
                    return true;
                }
                av_frame_unref(frm_);
                continue;
            }
            if (r == AVERROR_EOF) return false;
            if (r != AVERROR(EAGAIN)) fail(strprintf("FFmpeg decoding failed: %s", averr(r).c_str()));
            if (inputDone_) return false;

            r = av_read_frame(fmt_, pkt_);
            if (r < 0) {
                inputDone_ = true;
                avcodec_send_packet(dec_, nullptr);
                continue;
            }
            if (pkt_->stream_index != stream_) {
                av_packet_unref(pkt_);
                continue;
            }
            const int64_t pIdx = packetCount_++;
            if (intraOnly_ && pIdx < want) {
                // Not wanted: intra-only frames need no decoding to skip.
                av_packet_unref(pkt_);
                continue;
            }
            if (intraOnly_) pending_.push_back(pIdx);
            r = avcodec_send_packet(dec_, pkt_);
            av_packet_unref(pkt_);
            if (r < 0 && r != AVERROR(EAGAIN)) {
                if (intraOnly_ && !pending_.empty()) pending_.pop_back();
                fail(strprintf("FFmpeg cannot decode packet %lld: %s", static_cast<long long>(pIdx + 1),
                               averr(r).c_str()));
            }
        }
    }

    void convert(AVFrame* f, RawFrame& out) {
        PixInfo pi;
        const AVPixelFormat pf = AVPixelFormat(f->format);
        if (!pixInfo(pf, pi)) {
            const char* n = av_get_pix_fmt_name(pf);
            fail(strprintf("the video decodes to pixel format %s, which is not RAW sensor data "
                           "(serconv converts Bayer, grey and RGB streams)",
                           n ? n : "?"));
        }
        bits_ = pi.bits;
        out.width = f->width;
        out.height = f->height;
        out.layout = pi.layout;
        out.cfa = pi.cfa;
        out.data.resize(out.sampleCount());
        const int w = f->width, h = f->height;
        const int bps = pi.bits > 8 ? 2 : 1;
        if (pi.planarGbr) {
            for (int y = 0; y < h; ++y) {
                const uint8_t* g = f->data[0] + size_t(y) * size_t(f->linesize[0]);
                const uint8_t* b = f->data[1] + size_t(y) * size_t(f->linesize[1]);
                const uint8_t* r = f->data[2] + size_t(y) * size_t(f->linesize[2]);
                uint16_t* dst = out.data.data() + size_t(y) * size_t(w) * 3;
                for (int x = 0; x < w; ++x) {
                    auto rd = [&](const uint8_t* p) -> uint16_t {
                        if (bps == 1) return p[x];
                        return pi.bigEndian ? rd16be(p + x * 2) : rd16le(p + x * 2);
                    };
                    dst[x * 3 + 0] = rd(r);
                    dst[x * 3 + 1] = rd(g);
                    dst[x * 3 + 2] = rd(b);
                }
            }
        } else {
            const int ch = channelsOf(pi.layout);
            const bool native16 = bps == 2 && pi.bigEndian == !hostIsLittleEndian();
            for (int y = 0; y < h; ++y) {
                const uint8_t* src = f->data[0] + size_t(y) * size_t(f->linesize[0]);
                uint16_t* dst = out.data.data() + size_t(y) * size_t(w) * size_t(ch);
                const size_t n = size_t(w) * size_t(ch);
                if (bps == 1) {
                    for (size_t i = 0; i < n; ++i) dst[i] = src[i];
                } else if (native16) {
                    std::memcpy(dst, src, n * 2);
                } else {
                    for (size_t i = 0; i < n; ++i) dst[i] = pi.bigEndian ? rd16be(src + i * 2) : rd16le(src + i * 2);
                }
                if (pi.bgr) {
                    for (int x = 0; x < w; ++x) std::swap(dst[x * 3], dst[x * 3 + 2]);
                }
            }
        }
        const double full = double((1u << pi.bits) - 1);
        out.levels = Levels();
        out.levels.setBlack(0);
        out.levels.white = full;
        out.levels.known = false;
#if defined(SERCONV_RAW_COLOR_PARAMS)
        if (AVFrameSideData* sd = av_frame_get_side_data(f, AV_FRAME_DATA_RAW_COLOR_PARAMS)) {
            const AVRawColorParams* rcp = reinterpret_cast<const AVRawColorParams*>(sd->data);
            const double scale = pi.bits > 8 ? 65535.0 : 255.0;
            if (rcp->black_level.den && rcp->white_level.den) {
                out.levels.setBlack(av_q2d(rcp->black_level) * scale);
                out.levels.white = av_q2d(rcp->white_level) * scale;
                out.levels.known = out.levels.white > out.levels.black[0];
                if (!out.levels.known) {
                    out.levels.setBlack(0);
                    out.levels.white = full;
                }
            }
        }
#endif
        out.hasLevels = out.levels.known;
        int64_t pts = f->best_effort_timestamp;
        if (pts != AV_NOPTS_VALUE) {
            if (startPts_ == AV_NOPTS_VALUE) startPts_ = pts;
            out.time = double(pts - startPts_) * timeBase_;
        } else {
            out.time = NAN;
        }
    }

    fs::path path_;
    AVFormatContext* fmt_ = nullptr;
    AVCodecContext* dec_ = nullptr;
    AVPacket* pkt_ = nullptr;
    AVFrame* frm_ = nullptr;
    int stream_ = -1;
    bool intraOnly_ = false;
    bool inputDone_ = false;
    int64_t packetCount_ = 0;
    int64_t outCount_ = 0;
    int64_t lastIndex_ = -1;
    std::deque<int64_t> pending_;
    double timeBase_ = 0;
    int64_t startPts_ = AV_NOPTS_VALUE;
    int bits_ = 16;
    StreamInfo info_;
    RawFrame cached_;
    bool haveCached_ = false;
};

}  // namespace

std::unique_ptr<Reader> openFFmpegReader(const fs::path& path) {
    av_log_set_level(logLevel() >= LogLevel::Verbose ? AV_LOG_INFO : AV_LOG_ERROR);
    return std::make_unique<FFmpegReader>(path);
}

}  // namespace serconv
