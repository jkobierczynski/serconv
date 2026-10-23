#include "convert.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <ctime>
#include <exception>
#include <map>
#include <mutex>
#include <thread>

#include "ser_file.hpp"
#include "sidecar.hpp"
#include "timeutil.hpp"
#include "version.hpp"

namespace serconv {

int resolveThreads(int requested) {
    if (requested > 0) return std::min(requested, 64);
    unsigned hw = std::thread::hardware_concurrency();
    if (hw == 0) hw = 2;
    return int(std::min(hw, 8u));
}

namespace {

struct Produced {
    std::vector<uint8_t> bytes;
    double time = NAN;
    int64_t index = -1;
    FrameResult result;
};

// Absolute frame times: UTC = baseUtc + (t - baseOffset).
struct TimeModel {
    bool absolute = false;
    int64_t baseUtc = 0;
    double baseOffset = 0;
    int64_t localMinusUtc = 0;
    std::string description;

    int64_t utc(double t) const { return baseUtc + ticksFromSeconds(t - baseOffset); }
};

fs::path outputFor(const InputSet& job, const ConvertOptions& o, size_t jobCount) {
    if (o.output.empty()) return job.defaultOutput;
    fs::path p = pathFromUtf8(o.output);
    const char last = o.output.back();
    const bool dirLike = last == '/' || last == '\\';
    std::error_code ec;
    if (dirLike && !fs::exists(p, ec)) fs::create_directories(p, ec);
    if (fs::is_directory(p, ec)) return p / job.defaultOutput.filename();
    if (jobCount > 1) fail("with several inputs, -o must name a directory");
    if (p.extension().empty()) p += ".ser";
    return p;
}

std::string exposureText(double s) {
    if (s <= 0) return "";
    if (s < 1.0) return strprintf("1/%.0f s (%.6g s)", 1.0 / s, s);
    return strprintf("%.3g s", s);
}

class Progress {
public:
    Progress(int64_t total, bool enabled) : total_(total), enabled_(enabled), start_(Clock::now()), last_(start_) {}
    void update(int64_t done, bool force = false) {
        if (!enabled_) return;
        auto now = Clock::now();
        if (!force && now - last_ < std::chrono::milliseconds(stderrIsTerminal() ? 250 : 5000)) return;
        last_ = now;
        double secs = std::chrono::duration<double>(now - start_).count();
        double rate = secs > 0 ? double(done) / secs : 0;
        if (total_ > 0) {
            logProgress(strprintf("  frame %lld/%lld (%.0f%%, %.1f frames/s)", static_cast<long long>(done),
                                  static_cast<long long>(total_), 100.0 * double(done) / double(total_), rate));
        } else {
            logProgress(strprintf("  frame %lld (%.1f frames/s)", static_cast<long long>(done), rate));
        }
    }
    void finish(int64_t done) {
        update(done, true);
        if (enabled_) finishProgress();
    }
    double elapsed() const { return std::chrono::duration<double>(Clock::now() - start_).count(); }

private:
    using Clock = std::chrono::steady_clock;
    int64_t total_;
    bool enabled_;
    Clock::time_point start_, last_;
};

void convertOne(const InputSet& job, const ConvertOptions& opt, size_t jobCount) {
    logInfo(strprintf("Reading %s", job.label.c_str()));
    std::unique_ptr<Reader> reader = openReader(job, opt.reader);
    const StreamInfo& info = reader->info();

    std::string desc = strprintf("  %s", info.container.c_str());
    if (!info.codec.empty()) desc += ", " + info.codec;
    logInfo(desc);
    std::string geo = strprintf("  %dx%d %s", info.width, info.height,
                                info.layout == Layout::Bayer ? (std::string("Bayer ") + cfaName(info.cfa)).c_str()
                                                             : layoutName(info.layout));
    geo += strprintf(", %d-bit", info.bits);
    if (info.frameCount >= 0) {
        geo += strprintf(", %lld frame%s", static_cast<long long>(info.frameCount), info.frameCount == 1 ? "" : "s");
    }
    if (info.fps > 0) geo += strprintf(" at %.3f fps", info.fps);
    if (!info.camera.empty()) geo += ", " + info.camera;
    logInfo(geo);
    for (const auto& w : info.warnings) logWarning(w);

    FrameProcessor proc(info, opt.process);
    const OutputFormat& fmt = proc.format();

    // Frame selection (0-based indices).
    if (opt.first < 1) fail("--frames starts at 1");
    if (opt.every < 1) fail("--every must be at least 1");
    const int64_t first0 = opt.first - 1;
    int64_t last0 = opt.last < 0 ? -1 : opt.last - 1;
    const bool known = info.frameCount >= 0;
    std::vector<int64_t> indices;
    if (known) {
        if (first0 >= info.frameCount) {
            fail(strprintf("--frames starts at %lld, but there are only %lld frames", static_cast<long long>(opt.first),
                           static_cast<long long>(info.frameCount)));
        }
        if (last0 < 0 || last0 >= info.frameCount) last0 = info.frameCount - 1;
        for (int64_t i = first0; i <= last0; i += opt.every) {
            if (opt.maxFrames >= 0 && int64_t(indices.size()) >= opt.maxFrames) break;
            indices.push_back(i);
        }
        if (indices.empty()) fail("no frames selected");
    }

    const fs::path outPath = outputFor(job, opt, jobCount);
    SerHeader header;
    header.colorId = fmt.colorId;
    header.littleEndian = 0;
    header.width = fmt.width;
    header.height = fmt.height;
    header.pixelDepth = fmt.pixelDepth;
    header.observer = opt.observer;
    header.instrument = opt.instrumentSet ? opt.instrument : info.camera;
    header.telescope = opt.telescope;
    SerWriter writer;
    writer.open(outPath, header, opt.force);

    // Timing.
    TimeModel tm;
    const double t0src = opt.fps > 0 ? 0.0 : reader->frameTime(0);
    const int64_t offset = opt.utcOffsetSet ? opt.utcOffset : 0;
    // Local time for the SER header: the zone recorded in the file, else --utc-offset.
    const int64_t localZone = info.zoneKnown ? info.zoneTicks : offset;
    if (opt.startTimeSet) {
        tm.absolute = true;
        tm.baseUtc = opt.startTimeUtc;
        tm.baseOffset = std::isfinite(t0src) ? t0src : 0.0;
        tm.localMinusUtc = localZone;
        tm.description = "--start-time (UTC of the first frame of the clip)";
    } else if (info.clock == Clock::Camera) {
        tm.absolute = true;
        tm.baseUtc = info.originTicks - offset;
        tm.localMinusUtc = offset;
        tm.description = info.timeSource;
        if (!opt.utcOffsetSet) {
            logInfo("  Times: the camera clock is taken as UTC (give --utc-offset if it was set to local time)");
        }
    } else if (info.clock == Clock::Utc) {
        tm.absolute = true;
        tm.baseUtc = info.originTicks;
        tm.localMinusUtc = localZone;
        tm.description = info.timeSource;
        if (opt.utcOffsetSet) {
            logInfo("  Times: --utc-offset is not applied, the source time is already UTC (" + info.timeSource + ")");
        }
    }

    const int threads = resolveThreads(opt.threads);
    std::vector<std::unique_ptr<Reader>> clones;
    bool parallel = known && reader->randomAccess() && threads > 1 && indices.size() > 1;
    if (parallel) {
        for (int k = 1; k < threads; ++k) {
            auto c = reader->clone();
            if (!c) {
                parallel = false;
                clones.clear();
                break;
            }
            clones.push_back(std::move(c));
        }
    }

    std::vector<int64_t> stamps;
    bool allTimes = true;
    int64_t lost = 0;
    int64_t written = 0;
    int64_t firstIndex = -1, lastIndex = -1;
    double firstT = NAN, lastT = NAN;
    Progress progress(known ? int64_t(indices.size()) : -1, logLevel() >= LogLevel::Normal);

    auto frameTimeOf = [&](const Produced& p) -> double {
        if (opt.fps > 0) return double(p.index) / opt.fps;
        if (std::isfinite(p.time)) return p.time;
        double t = reader->frameTime(p.index);
        if (std::isfinite(t)) return t;
        if (info.fps > 0) return double(p.index) / info.fps;
        return NAN;
    };
    auto consume = [&](Produced& p) {
        writer.writeFrame(p.bytes.data(), p.bytes.size());
        const double t = frameTimeOf(p);
        if (!std::isfinite(t)) allTimes = false;
        if (tm.absolute && std::isfinite(t)) stamps.push_back(tm.utc(t));
        if (written == 0) {
            firstIndex = p.index;
            firstT = t;
        }
        lastIndex = p.index;
        lastT = t;
        if (p.result.tracked && !p.result.objectFound) ++lost;
        ++written;
        progress.update(written);
    };

    if (parallel) {
        const int64_t n = int64_t(indices.size());
        const int64_t queue = int64_t(threads) * 2;
        std::vector<Produced> slots(static_cast<size_t>(queue));
        std::vector<char> ready(static_cast<size_t>(queue), 0);
        std::mutex m;
        std::condition_variable cvReady, cvFree;
        std::atomic<int64_t> nextJob{0};
        int64_t writtenSeq = 0;
        bool abort = false;
        std::exception_ptr error;

        auto worker = [&](Reader* rd) {
            RawFrame raw;
            Produced local;
            try {
                for (;;) {
                    const int64_t seq = nextJob.fetch_add(1);
                    if (seq >= n) break;
                    {
                        std::unique_lock<std::mutex> lk(m);
                        cvFree.wait(lk, [&] { return abort || seq < writtenSeq + queue; });
                        if (abort) break;
                    }
                    if (!rd->read(indices[size_t(seq)], raw)) fail("frame ended unexpectedly");
                    local.result = proc.process(raw, local.bytes);
                    local.time = raw.time;
                    local.index = raw.index;
                    {
                        std::lock_guard<std::mutex> lk(m);
                        const size_t s = size_t(seq % queue);
                        std::swap(slots[s], local);
                        ready[s] = 1;
                    }
                    cvReady.notify_all();
                }
            } catch (...) {
                std::lock_guard<std::mutex> lk(m);
                if (!error) error = std::current_exception();
                abort = true;
                cvReady.notify_all();
                cvFree.notify_all();
            }
        };

        std::vector<std::thread> pool;
        pool.emplace_back(worker, reader.get());
        for (auto& c : clones) pool.emplace_back(worker, c.get());

        Produced item;
        try {
            for (int64_t seq = 0; seq < n; ++seq) {
                const size_t s = size_t(seq % queue);
                {
                    std::unique_lock<std::mutex> lk(m);
                    cvReady.wait(lk, [&] { return abort || ready[s]; });
                    if (abort && !ready[s]) break;
                    std::swap(item, slots[s]);
                    ready[s] = 0;
                    writtenSeq = seq + 1;
                }
                cvFree.notify_all();
                consume(item);
            }
        } catch (...) {
            std::lock_guard<std::mutex> lk(m);
            if (!error) error = std::current_exception();
            abort = true;
        }
        cvFree.notify_all();
        cvReady.notify_all();
        for (auto& t : pool) t.join();
        if (error) std::rethrow_exception(error);
    } else {
        RawFrame raw;
        Produced item;
        if (known) {
            for (int64_t idx : indices) {
                if (!reader->read(idx, raw)) {
                    finishProgress();
                    logWarning(strprintf("the stream ended at frame %lld, before the end of the selection",
                                         static_cast<long long>(idx + 1)));
                    break;
                }
                item.result = proc.process(raw, item.bytes);
                item.time = raw.time;
                item.index = raw.index;
                consume(item);
            }
        } else {
            for (int64_t idx = first0;; idx += opt.every) {
                if (last0 >= 0 && idx > last0) break;
                if (opt.maxFrames >= 0 && written >= opt.maxFrames) break;
                if (!reader->read(idx, raw)) break;
                item.result = proc.process(raw, item.bytes);
                item.time = raw.time;
                item.index = raw.index;
                consume(item);
            }
        }
        if (written == 0) fail("no frames were decoded");
    }
    progress.finish(written);

    const bool trailer = opt.timestamps && tm.absolute && allTimes && int64_t(stamps.size()) == written;
    int64_t dateUtc = 0, dateLocal = 0;
    const bool haveDate = tm.absolute && (std::isfinite(firstT) || opt.startTimeSet);
    if (haveDate) {
        dateUtc = std::isfinite(firstT) ? tm.utc(firstT) : tm.baseUtc;
        dateLocal = dateUtc + tm.localMinusUtc;
    }
    writer.finish(dateLocal, dateUtc, trailer ? stamps : std::vector<int64_t>());
    const double seconds = progress.elapsed();

    // Report.
    std::string fmtText = strprintf("%dx%d %s", fmt.width, fmt.height,
                                    fmt.layout == Layout::Bayer ? (std::string("Bayer ") + cfaName(fmt.cfa)).c_str()
                                                                : layoutName(fmt.layout));
    logInfo(strprintf("Wrote %s: %lld frame%s, %s, %d-bit, %s (%.1f s, %.1f frames/s)", pathToUtf8(outPath).c_str(),
                      static_cast<long long>(written), written == 1 ? "" : "s", fmtText.c_str(), fmt.pixelDepth,
                      humanBytes(double(writer.bytesWritten() + (trailer ? 8 * written : 0))).c_str(), seconds,
                      seconds > 0 ? double(written) / seconds : 0.0));
    Levels lv0 = proc.effectiveLevels(RawFrame());
    if (!lv0.known && opt.process.levels == LevelMode::Scale) {
        logWarning("the source gave no black/white levels; scaled from 0 and the full range (set --black/--white)");
    }
    logInfo("  Levels: " + proc.describeLevels(lv0));
    if (haveDate) {
        std::string t = "  UTC " + formatTicks(dateUtc, true, 3);
        if (std::isfinite(lastT) && written > 1) {
            t += " to " + formatTicks(tm.utc(lastT), true, 3) + strprintf(" (%s)", formatDuration(lastT - firstT).c_str());
        }
        if (!trailer) t += opt.timestamps ? ", no per-frame timestamps (frame times unknown)" : ", timestamps off";
        logInfo(t);
    } else {
        logInfo("  No absolute time known: SER dates left empty (use --start-time)");
    }
    if (lost > 0) {
        logWarning(strprintf("no object found in %lld frame(s); those were cropped around the frame centre",
                             static_cast<long long>(lost)));
    }

    if (opt.sidecar) {
        KeyValueText kv;
        kv.section("serconv");
        kv.add("version", SERCONV_VERSION);
        kv.add("converted", formatTicks(ticksFromUnixSeconds(double(std::time(nullptr))), true, 0));
        kv.add("command", opt.commandLine);
        kv.section("source");
        if (job.files.size() == 1) kv.add("input", pathToUtf8(job.files[0]));
        else kv.add("input", strprintf("%llu files: %s ... %s", static_cast<unsigned long long>(job.files.size()), pathToUtf8(job.files.front()).c_str(),
                                      pathToUtf8(job.files.back()).c_str()));
        kv.add("format", info.container);
        kv.add("codec", info.codec);
        kv.add("camera", info.camera);
        kv.add("camera_serial", info.cameraSerial);
        kv.add("lens", info.lens);
        kv.add("size", strprintf("%dx%d", info.width, info.height));
        kv.add("layout", info.layout == Layout::Bayer ? std::string("Bayer ") + cfaName(info.cfa) : layoutName(info.layout));
        kv.add("bits", std::to_string(info.bits));
        if (info.frameCount >= 0) kv.add("frames", std::to_string(info.frameCount));
        if (info.fps > 0) kv.add("fps", strprintf("%.3f", info.fps));
        const Levels& sl = info.levels;
        kv.add("black_level", sl.known ? (sl.maxBlack() - sl.minBlack() < 0.5
                                              ? strprintf("%.0f", sl.black[0])
                                              : strprintf("%.0f/%.0f/%.0f/%.0f", sl.black[0], sl.black[1], sl.black[2],
                                                          sl.black[3]))
                                       : "unknown");
        kv.add("white_level", sl.known ? strprintf("%.0f", sl.white) : "unknown");
        if (info.iso > 0) kv.add("iso", strprintf("%.0f", info.iso));
        kv.add("exposure", exposureText(info.exposure));
        if (info.aperture > 0) kv.add("aperture", strprintf("f/%.1f", info.aperture));
        if (info.focalLength > 0) kv.add("focal_length", strprintf("%.0f mm", info.focalLength));
        for (const auto& n : info.notes) kv.add(n.first, n.second);
        for (const auto& w : info.warnings) kv.add("warning", w);
        kv.section("output");
        kv.add("file", pathToUtf8(outPath.filename()));
        kv.add("frames", std::to_string(written));
        if (firstIndex >= 0) {
            kv.add("source_frames", strprintf("%lld-%lld%s", static_cast<long long>(firstIndex + 1),
                                              static_cast<long long>(lastIndex + 1),
                                              opt.every > 1 ? strprintf(", every %lld", static_cast<long long>(opt.every)).c_str()
                                                            : ""));
        }
        kv.add("size", strprintf("%dx%d", fmt.width, fmt.height));
        kv.add("color_id", strprintf("%d (%s)", fmt.colorId, serColorName(fmt.colorId).c_str()));
        kv.add("pixel_depth", std::to_string(fmt.pixelDepth));
        kv.add("levels", proc.describeLevels(lv0));
        if (opt.process.crop) {
            kv.add("crop", strprintf("%d,%d,%d,%d", opt.process.cropX, opt.process.cropY, opt.process.cropW,
                                     opt.process.cropH));
        }
        if (opt.process.center) {
            kv.add("center", strprintf("%dx%d around the brightest object (threshold %.2f)", fmt.width, fmt.height,
                                       opt.process.threshold));
            kv.add("object_not_found", std::to_string(lost));
        }
        kv.add("observer", header.observer);
        kv.add("instrument", header.instrument);
        kv.add("telescope", header.telescope);
        kv.section("time");
        if (tm.absolute) {
            kv.add("source", tm.description);
            std::string offsetText;
            const std::string given = opt.utcOffsetSet ? formatOffset(opt.utcOffset) : std::string();
            if (opt.startTimeSet) {
                offsetText = !opt.utcOffsetSet ? "not given"
                             : info.zoneKnown  ? given + " given, not used (the file records its time zone)"
                                               : given + " (for the local time in the header only)";
            } else if (info.clock == Clock::Utc) {
                offsetText = opt.utcOffsetSet ? given + " given, not applied (the source time is UTC)"
                                              : "not needed (the source time is UTC)";
            } else {
                offsetText = opt.utcOffsetSet ? given : "not given (camera clock taken as UTC)";
            }
            kv.add("utc_offset", offsetText);
            if (info.zoneKnown) kv.add("camera_time_zone", formatOffset(info.zoneTicks));
            if (haveDate) kv.add("first_frame_utc", formatTicks(dateUtc, true, 4));
            if (std::isfinite(lastT)) kv.add("last_frame_utc", formatTicks(tm.utc(lastT), true, 4));
        } else {
            kv.add("source", "none");
        }
        if (std::isfinite(firstT) && std::isfinite(lastT) && written > 1 && lastT > firstT) {
            kv.add("duration", strprintf("%.4f s", lastT - firstT));
            kv.add("mean_fps", strprintf("%.3f", double(lastIndex - firstIndex) / (lastT - firstT)));
        }
        if (opt.fps > 0) kv.add("fps_assumed", strprintf("%.3f (--fps)", opt.fps));
        kv.add("timestamps", trailer ? "per-frame UTC trailer written" : "none");
        const fs::path sidecar = sidecarPathFor(outPath);
        writeTextFile(sidecar, kv.text());
        logVerbose("  Sidecar: " + pathToUtf8(sidecar));
    }
}

}  // namespace

int runConvert(const ConvertOptions& opt) {
    std::vector<InputSet> jobs = collectInputs(opt.inputs);
    if (jobs.size() == 1) {
        convertOne(jobs[0], opt, 1);  // an error is reported once, by main()
        return 0;
    }
    // Several inputs. Check the output names first: an -o that is not a
    // folder, or two inputs that would write the same file, stop everything.
    std::map<std::string, std::string> outputs;  // output path -> input
    for (const InputSet& job : jobs) {
        const fs::path out = outputFor(job, opt, jobs.size());
        std::error_code ec;
        std::string key = pathToUtf8(fs::absolute(out, ec).lexically_normal());
#if defined(_WIN32) || defined(__APPLE__)
        key = toLower(key);  // case-insensitive file systems
#endif
        auto ins = outputs.emplace(key, job.label);
        if (!ins.second) {
            fail(strprintf("%s and %s would both be written to '%s'; convert them separately, each with its own -o",
                           ins.first->second.c_str(), job.label.c_str(), pathToUtf8(out).c_str()));
        }
    }
    // A failed input is reported and the others are still converted.
    int failures = 0;
    for (const InputSet& job : jobs) {
        try {
            convertOne(job, opt, jobs.size());
        } catch (const std::bad_alloc&) {
            finishProgress();
            logError(job.label + ": out of memory");
            ++failures;
        } catch (const std::exception& e) {
            finishProgress();
            logError(strprintf("%s: %s", job.label.c_str(), e.what()));
            ++failures;
        }
    }
    if (failures) {
        logInfo(strprintf("%d of %llu conversions failed", failures, static_cast<unsigned long long>(jobs.size())));
        return 1;
    }
    return 0;
}

}  // namespace serconv
