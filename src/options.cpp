#include "options.hpp"

#include <cerrno>
#include <cstdint>
#include <cstdlib>

#include "timeutil.hpp"

namespace serconv {

namespace {

[[noreturn]] void usage(const std::string& msg) { throw UsageError(msg); }

int64_t parseInt(const std::string& opt, const std::string& v, int64_t lo, int64_t hi) {
    std::string t = trim(v);
    if (t.empty()) usage(opt + " needs a number");
    char* end = nullptr;
    errno = 0;
    long long n = std::strtoll(t.c_str(), &end, 10);
    if (errno || !end || *end != '\0') usage(strprintf("%s: '%s' is not a whole number", opt.c_str(), v.c_str()));
    if (n < lo || n > hi) usage(strprintf("%s: %lld is out of range", opt.c_str(), n));
    return n;
}

double parseReal(const std::string& opt, const std::string& v, double lo, double hi) {
    std::string t = trim(v);
    char* end = nullptr;
    errno = 0;
    double d = std::strtod(t.c_str(), &end);
    if (t.empty() || errno || !end || *end != '\0') usage(strprintf("%s: '%s' is not a number", opt.c_str(), v.c_str()));
    if (d < lo || d > hi) usage(strprintf("%s: %g is out of range", opt.c_str(), d));
    return d;
}

std::vector<int64_t> parseList(const std::string& opt, const std::string& v, char sep) {
    std::vector<int64_t> out;
    for (const auto& part : split(v, sep)) out.push_back(parseInt(opt, part, 0, 1000000));
    return out;
}

std::string quoteArg(const std::string& a) {
    if (!a.empty() && a.find_first_of(" \t\"'") == std::string::npos) return a;
    std::string q = "\"";
    for (char c : a) {
        if (c == '"') q += "\\\"";
        else q += c;
    }
    return q + "\"";
}

}  // namespace

std::string usageText() {
    return R"(Usage: serconv [options] INPUT...
       serconv info FILE...
       serconv formats

Converts cinema and mirrorless RAW video to SER for planetary, lunar and
solar image stacking (AutoStakkert!, PIPP, Siril, FTYS, ...).

Inputs: .MLV (Magic Lantern), .CRM (Canon Cinema RAW Light), .CR3 (also RAW
burst rolls), .MOV/.MP4 (ProRes RAW, needs the FFmpeg build), and DNG,
CinemaDNG or other still raw sequences given as a folder, a wildcard or files.

Output:
  -o, --output PATH        SER file, or folder (default: the input name + .ser)
  -f, --force              overwrite existing files
      --no-sidecar         do not write NAME.serconv.txt (camera, levels, times)
      --no-timestamps      do not write the per-frame timestamp trailer
      --observer TEXT      SER header fields, 40 ASCII characters each
      --instrument TEXT    (default: the camera name)
      --telescope TEXT

Frames:
      --frames A-B         frames A to B, 1-based ("A-" and "-B" work too)
      --every N            keep every Nth frame
      --max-frames N       stop after N frames

Region of interest:
      --crop X,Y,W,H       fixed crop in sensor pixels
      --center W[xH]       crop W x H pixels around the planet, Moon or Sun,
                           found again in every frame
      --threshold F        detection level between background and peak (0.25)

Levels:
      --levels MODE        scale: black -> 0, white -> 65535 (default)
                           shift: values kept, top sensor bit moved to bit 15
                           raw:   values kept, SER depth = sensor bit depth
      --depth 16|8         output bits for scale and shift (default 16)
      --black N            override the black level (sensor units)
      --white N            override the white level
      --pedestal N         output value given to the black level (scale)

Time:
      --utc-offset +HH:MM  the camera clock minus UTC (+02:00 for a camera
                           set to Central European Summer Time); not used
                           for files that record their time zone
      --start-time TIME    UTC time of the clip's first frame, for example
                           2026-10-23T21:14:05.25Z (overrides the camera)
      --fps F              frame rate for the timestamps when the source
                           gives no frame times

Other:
      --backend NAME       auto, mlv, libraw or ffmpeg
  -j, --threads N          decoding threads (default: cores, at most 8)
  -q, --quiet              only warnings and errors
  -v, --verbose            more detail
  -h, --help               this help
      --version            versions of serconv, LibRaw and FFmpeg
)";
}

ParsedArgs parseArgs(const std::vector<std::string>& argsIn) {
    ParsedArgs pa;
    ConvertOptions& c = pa.convert;
    std::vector<std::string> args = argsIn;
    {
        std::string cl = "serconv";
        for (const auto& a : args) cl += " " + quoteArg(a);
        c.commandLine = cl;
    }
    size_t i = 0;
    if (!args.empty()) {
        if (args[0] == "info") {
            pa.command = Command::Info;
            i = 1;
        } else if (args[0] == "formats") {
            pa.command = Command::Formats;
            i = 1;
        }
    }
    bool onlyFiles = false;
    for (; i < args.size(); ++i) {
        std::string a = args[i];
        if (onlyFiles || a.empty() || a[0] != '-' || a == "-") {
            if (pa.command == Command::Info) pa.files.push_back(a);
            else c.inputs.push_back(a);
            continue;
        }
        if (a == "--") {
            onlyFiles = true;
            continue;
        }
        std::string value;
        bool inlineValue = false;
        if (startsWith(a, "--")) {
            size_t eq = a.find('=');
            if (eq != std::string::npos) {
                value = a.substr(eq + 1);
                a = a.substr(0, eq);
                inlineValue = true;
            }
        }
        auto need = [&]() -> std::string {
            if (inlineValue) return value;
            if (i + 1 >= args.size()) usage(a + " needs a value");
            return args[++i];
        };
        auto noValue = [&]() {
            if (inlineValue) usage(a + " takes no value");
        };

        if (a == "-h" || a == "--help") {
            noValue();
            pa.command = Command::Help;
        } else if (a == "--version") {
            noValue();
            pa.command = Command::Version;
        } else if (a == "-q" || a == "--quiet") {
            noValue();
            pa.logLevel = LogLevel::Quiet;
        } else if (a == "-v" || a == "--verbose") {
            noValue();
            pa.logLevel = LogLevel::Verbose;
        } else if (a == "-o" || a == "--output") {
            c.output = need();
        } else if (a == "-f" || a == "--force") {
            noValue();
            c.force = true;
        } else if (a == "--no-sidecar") {
            noValue();
            c.sidecar = false;
        } else if (a == "--no-timestamps") {
            noValue();
            c.timestamps = false;
        } else if (a == "--observer") {
            c.observer = need();
        } else if (a == "--instrument") {
            c.instrument = need();
            c.instrumentSet = true;
        } else if (a == "--telescope") {
            c.telescope = need();
        } else if (a == "--frames") {
            std::string v = trim(need());
            size_t dash = v.find('-');
            if (dash == std::string::npos) {
                c.first = c.last = parseInt("--frames", v, 1, INT64_MAX);
            } else {
                std::string l = v.substr(0, dash), r = v.substr(dash + 1);
                c.first = l.empty() ? 1 : parseInt("--frames", l, 1, INT64_MAX);
                c.last = r.empty() ? -1 : parseInt("--frames", r, 1, INT64_MAX);
                if (c.last >= 0 && c.last < c.first) usage("--frames: the end comes before the start");
            }
        } else if (a == "--every") {
            c.every = parseInt(a, need(), 1, INT64_MAX);
        } else if (a == "--max-frames") {
            c.maxFrames = parseInt(a, need(), 1, INT64_MAX);
        } else if (a == "--crop") {
            auto v = parseList(a, need(), ',');
            if (v.size() != 4) usage("--crop needs X,Y,W,H");
            c.process.crop = true;
            c.process.cropX = int(v[0]);
            c.process.cropY = int(v[1]);
            c.process.cropW = int(v[2]);
            c.process.cropH = int(v[3]);
        } else if (a == "--center" || a == "--centre") {
            std::string v = toLower(need());
            std::vector<int64_t> wh;
            if (v.find('x') != std::string::npos) wh = parseList(a, v, 'x');
            else wh = parseList(a, v, ',');
            if (wh.size() == 1) wh.push_back(wh[0]);
            if (wh.size() != 2 || wh[0] < 2 || wh[1] < 2) usage("--center needs W or WxH (at least 2)");
            c.process.center = true;
            c.process.centerW = int(wh[0]);
            c.process.centerH = int(wh[1]);
        } else if (a == "--threshold") {
            c.process.threshold = parseReal(a, need(), 0.01, 0.99);
        } else if (a == "--levels") {
            if (!parseLevelMode(need(), c.process.levels)) usage("--levels must be scale, shift or raw");
        } else if (a == "--depth") {
            int64_t d = parseInt(a, need(), 8, 16);
            if (d != 8 && d != 16) usage("--depth must be 8 or 16");
            c.process.depth = int(d);
        } else if (a == "--black") {
            c.process.black = parseReal(a, need(), 0, 65535);
            c.process.blackSet = true;
        } else if (a == "--white") {
            c.process.white = parseReal(a, need(), 1, 65535);
            c.process.whiteSet = true;
        } else if (a == "--pedestal") {
            c.process.pedestal = parseReal(a, need(), 0, 65534);
        } else if (a == "--utc-offset") {
            std::string v = need();
            if (!parseOffset(v, c.utcOffset)) usage("--utc-offset: use +HH:MM, for example +02:00");
            c.utcOffsetSet = true;
        } else if (a == "--start-time") {
            std::string v = need();
            int64_t ticks, zone;
            bool hasZone;
            if (!parseDateTime(v, ticks, hasZone, zone)) {
                usage("--start-time: use YYYY-MM-DDTHH:MM:SS[.fff][Z|+HH:MM]");
            }
            c.startTimeSet = true;
            c.startTimeUtc = ticks - (hasZone ? zone : 0);
        } else if (a == "--fps") {
            c.fps = parseReal(a, need(), 0.001, 100000);
        } else if (a == "--backend") {
            c.reader.backend = toLower(need());
        } else if (a == "-j" || a == "--threads") {
            c.threads = int(parseInt(a, need(), 1, 64));
        } else {
            usage("unknown option " + a);
        }
    }
    if (pa.command == Command::Convert && c.inputs.empty()) {
        if (args.empty()) pa.command = Command::Help;
        else usage("no input files");
    }
    if (pa.command == Command::Info && pa.files.empty()) usage("info: no files given");
    if (c.process.blackSet && c.process.whiteSet && c.process.white <= c.process.black) {
        usage("--white must be above --black");
    }
    return pa;
}

}  // namespace serconv
