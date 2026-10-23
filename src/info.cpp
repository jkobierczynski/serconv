#include "info.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>

#include "ser_file.hpp"
#include "timeutil.hpp"

namespace serconv {

namespace {

void line(const char* key, const std::string& value) {
    if (value.empty()) return;
    std::printf("  %-13s %s\n", key, value.c_str());
}

void serInfo(const fs::path& path) {
    SerReader r;
    r.open(path);
    const SerHeader& h = r.header();
    std::printf("%s\n", pathToUtf8(path).c_str());
    line("File size:", strprintf("%s bytes", withThousands(r.fileSize()).c_str()));
    std::string bo;
    if (h.bytesPerSample() == 2) {
        bo = h.littleEndian == 0 ? ", little-endian (LittleEndian = 0)"
                                 : strprintf(", big-endian (LittleEndian = %d)", h.littleEndian);
    }
    line("Image:", strprintf("%dx%d, %s (ColorID %d), %d-bit%s", h.width, h.height, serColorName(h.colorId).c_str(),
                             h.colorId, h.pixelDepth, bo.c_str()));
    line("Frames:", strprintf("%lld x %s bytes%s", static_cast<long long>(r.frameCount()),
                              withThousands(r.frameBytes()).c_str(),
                              r.frameCount() != h.frameCount ? strprintf(" (header: %d)", h.frameCount).c_str() : ""));
    line("Observer:", h.observer);
    line("Instrument:", h.instrument);
    line("Telescope:", h.telescope);
    if (h.dateTime > 0) line("DateTime:", formatTicks(h.dateTime, false, 4) + " (local)");
    if (h.dateTimeUtc > 0) line("DateTime_UTC:", formatTicks(h.dateTimeUtc, true, 4));
    const auto& ts = r.timestamps();
    if (!ts.empty()) {
        bool monotonic = true;
        int64_t minStep = INT64_MAX, maxStep = 0;
        for (size_t i = 1; i < ts.size(); ++i) {
            int64_t d = ts[i] - ts[i - 1];
            if (d <= 0) monotonic = false;
            minStep = std::min(minStep, d);
            maxStep = std::max(maxStep, d);
        }
        std::string s = strprintf("%llu, %s to %s", static_cast<unsigned long long>(ts.size()), formatTicks(ts.front(), true, 4).c_str(),
                                  formatTicks(ts.back(), true, 4).c_str());
        if (ts.size() > 1) {
            double span = secondsFromTicks(ts.back() - ts.front());
            s += strprintf(" (%s", formatDuration(span).c_str());
            if (span > 0) s += strprintf(", %.3f fps mean", double(ts.size() - 1) / span);
            s += ")";
            if (!monotonic) s += ", NOT increasing";
            else if (maxStep > minStep * 3 / 2) {
                s += strprintf(", steps %.2f..%.2f ms", double(minStep) / 1e4, double(maxStep) / 1e4);
            }
        }
        line("Timestamps:", s);
    } else {
        line("Timestamps:", "none");
    }
    for (const auto& p : r.problems()) line("Note:", p);
}

void streamInfo(const InputSet& job, const ReaderOptions& options) {
    auto reader = openReader(job, options);
    const StreamInfo& in = reader->info();
    std::printf("%s\n", job.label.c_str());
    line("Format:", in.codec.empty() ? in.container : in.container + ", " + in.codec);
    line("Camera:", in.camera + (in.cameraSerial.empty() ? "" : " (serial " + in.cameraSerial + ")"));
    line("Lens:", in.lens);
    std::string frames = in.frameCount >= 0 ? std::to_string(in.frameCount) : "unknown";
    if (in.fps > 0) frames += strprintf(" at %.3f fps", in.fps);
    line("Frames:", frames);
    line("Image:", strprintf("%dx%d %s, %d-bit", in.width, in.height,
                             in.layout == Layout::Bayer ? (std::string("Bayer ") + cfaName(in.cfa)).c_str()
                                                        : layoutName(in.layout),
                             in.bits));
    const Levels& lv = in.levels;
    if (lv.known) {
        std::string b = lv.maxBlack() - lv.minBlack() < 0.5
                            ? strprintf("%.0f", lv.black[0])
                            : strprintf("%.0f/%.0f/%.0f/%.0f", lv.black[0], lv.black[1], lv.black[2], lv.black[3]);
        line("Levels:", strprintf("black %s, white %.0f", b.c_str(), lv.white));
    } else {
        line("Levels:", "not given by the source");
    }
    std::string expo;
    if (in.exposure > 0) expo = in.exposure < 1 ? strprintf("1/%.0f s", 1.0 / in.exposure) : strprintf("%.3g s", in.exposure);
    if (in.iso > 0) expo += (expo.empty() ? "" : ", ") + strprintf("ISO %.0f", in.iso);
    if (in.aperture > 0) expo += (expo.empty() ? "" : ", ") + strprintf("f/%.1f", in.aperture);
    if (in.focalLength > 0) expo += (expo.empty() ? "" : ", ") + strprintf("%.0f mm", in.focalLength);
    line("Exposure:", expo);
    if (in.clock != Clock::None) {
        const double t0 = reader->frameTime(0);
        const int64_t start = in.originTicks + (std::isfinite(t0) ? ticksFromSeconds(t0) : 0);
        line("Start:", formatTicks(start, in.clock == Clock::Utc, 3) +
                           (in.clock == Clock::Camera ? " (camera clock)" : " (UTC)"));
        if (in.frameCount > 1) {
            const double tn = reader->frameTime(in.frameCount - 1);
            if (std::isfinite(t0) && std::isfinite(tn)) {
                line("Duration:", formatDuration(tn - t0));
            }
        }
    }
    line("Time source:", in.timeSource);
    for (const auto& n : in.notes) line((n.first + ":").c_str(), n.second);
    for (const auto& w : in.warnings) line("Warning:", w);
}

}  // namespace

int runInfo(const std::vector<std::string>& args, const ReaderOptions& options) {
    if (args.empty()) fail("info: give one or more files");
    int failures = 0;
    bool firstOut = true;
    for (const std::string& a : args) {
        try {
            fs::path p = pathFromUtf8(a);
            if (!firstOut) std::printf("\n");
            firstOut = false;
            if (lowerExtension(p) == ".ser") {
                serInfo(p);
            } else {
                for (const auto& job : collectInputs({a})) streamInfo(job, options);
            }
            std::fflush(stdout);
        } catch (const Error& e) {
            std::fflush(stdout);
            logError(strprintf("%s: %s", a.c_str(), e.what()));
            ++failures;
        }
    }
    return failures ? 1 : 0;
}

}  // namespace serconv
