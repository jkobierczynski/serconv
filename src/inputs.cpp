// Input collection (directories, wildcards, sequences) and reader selection.

#include <algorithm>
#include <cctype>
#include <cstring>
#include <functional>
#include <set>

#include "bmff.hpp"
#include "reader.hpp"
#include "version.hpp"

namespace serconv {

namespace {

const char* const kStillExts[] = {".dng", ".cr2", ".cr3", ".crw", ".nef", ".nrw", ".arw", ".srf", ".sr2",
                                  ".raf", ".orf", ".rw2", ".rwl", ".pef", ".ptx", ".srw", ".3fr", ".fff",
                                  ".iiq", ".erf", ".kdc", ".dcr", ".mrw", ".mos", ".mef", ".x3f"};
const char* const kVideoExts[] = {".mov", ".mp4", ".m4v", ".mxf", ".avi", ".mkv", ".nut"};

bool isVideoExtension(const std::string& e) {
    for (const char* x : kVideoExts)
        if (e == x) return true;
    return false;
}

bool isMlvChunkExtension(const std::string& e) {
    return e.size() == 4 && e[0] == '.' && e[1] == 'm' && std::isdigit(static_cast<unsigned char>(e[2])) &&
           std::isdigit(static_cast<unsigned char>(e[3]));
}

bool hasWildcard(const std::string& s) { return s.find_first_of("*?") != std::string::npos; }

bool wildcardMatch(const std::string& pattern, const std::string& name) {
    // Case-insensitive glob with * and ?.
    size_t p = 0, n = 0, star = std::string::npos, mark = 0;
    while (n < name.size()) {
        if (p < pattern.size() && (pattern[p] == '?' || std::tolower(static_cast<unsigned char>(pattern[p])) ==
                                                           std::tolower(static_cast<unsigned char>(name[n])))) {
            ++p;
            ++n;
        } else if (p < pattern.size() && pattern[p] == '*') {
            star = p++;
            mark = n;
        } else if (star != std::string::npos) {
            p = star + 1;
            n = ++mark;
        } else {
            return false;
        }
    }
    while (p < pattern.size() && pattern[p] == '*') ++p;
    return p == pattern.size();
}

void sortNatural(std::vector<fs::path>& files) {
    std::sort(files.begin(), files.end(), [](const fs::path& a, const fs::path& b) {
        return naturalLess(pathToUtf8(a.filename()), pathToUtf8(b.filename()));
    });
}

// Output name for a sequence: the folder's name next to the folder, or the
// first file's name without its frame number.
fs::path sequenceOutputName(const std::vector<fs::path>& files, const fs::path& fromDir) {
    if (!fromDir.empty()) {
        std::error_code ec;
        fs::path dir = fs::absolute(fromDir, ec).lexically_normal();
        std::string name = pathToUtf8(dir.filename());
        if (name.empty()) {  // "folder/" or "folder/."
            dir = dir.parent_path();
            name = pathToUtf8(dir.filename());
        }
        if (name.empty() || name == "." || name == "..") name = "sequence";
        return dir.parent_path() / pathFromUtf8(name + ".ser");
    }
    std::string stem = pathToUtf8(files[0].stem());
    while (!stem.empty() && (std::isdigit(static_cast<unsigned char>(stem.back())) || stem.back() == '_' ||
                             stem.back() == '-' || stem.back() == '.' || stem.back() == ' '))
        stem.pop_back();
    if (stem.empty()) stem = "sequence";
    return files[0].parent_path() / pathFromUtf8(stem + ".ser");
}

// The files of a folder that pass `keep`, without throwing on I/O errors.
std::vector<fs::path> listFolder(const fs::path& dir, const std::function<bool(const fs::path&)>& keep) {
    std::vector<fs::path> out;
    std::error_code ec;
    for (auto it = fs::directory_iterator(dir, ec); !ec && it != fs::directory_iterator(); it.increment(ec)) {
        std::error_code ec2;
        if (it->is_regular_file(ec2) && keep(it->path())) out.push_back(it->path());
    }
    if (ec) fail(strprintf("cannot read the folder '%s': %s", pathToUtf8(dir).c_str(), ec.message().c_str()));
    return out;
}

// The main .MLV file of a spanned part (.M00 ...), empty if there is none.
fs::path mainMlvOf(const fs::path& part) {
    std::error_code ec;
    for (const char* e : {".MLV", ".mlv"}) {
        fs::path m = part;
        m.replace_extension(e);
        if (fs::is_regular_file(m, ec)) return m;
    }
    return fs::path();
}

// A CR3 holding a RAW burst roll (several frames) rather than one still.
bool isCr3Roll(const fs::path& path) {
    int64_t n = canonFrameCount(path);   // quick: reads only the ISO media boxes
    if (n < 0) n = librawFrameCount(path);
    return n > 1;
}

InputSet clipJob(const fs::path& file, bool sequence) {
    InputSet job;
    job.files.push_back(file);
    job.sequence = sequence;
    job.label = pathToUtf8(file.filename());
    job.defaultOutput = file;
    job.defaultOutput.replace_extension(".ser");
    return job;
}

}  // namespace

bool isStillRawExtension(const std::string& e) {
    for (const char* x : kStillExts)
        if (e == x) return true;
    return false;
}

bool isClipExtension(const std::string& e) { return e == ".mlv" || e == ".crm" || isVideoExtension(e); }

std::vector<InputSet> collectInputs(const std::vector<std::string>& args) {
    struct Item {
        fs::path path;
        fs::path fromDir;    // the folder it was found in, if a folder was given
        bool named = false;  // named on the command line (not found by a folder or pattern)
    };
    std::vector<Item> items;
    std::error_code ec;
    for (const std::string& arg : args) {
        fs::path p = pathFromUtf8(arg);
        if (fs::is_directory(p, ec)) {
            std::vector<fs::path> found = listFolder(p, [](const fs::path& f) {
                const std::string e = lowerExtension(f);
                return isStillRawExtension(e) || e == ".mlv" || e == ".crm";
            });
            if (found.empty()) fail(strprintf("'%s' contains no raw files or clips", arg.c_str()));
            sortNatural(found);
            for (auto& f : found) items.push_back({f, p, false});
        } else if (fs::is_regular_file(p, ec)) {
            items.push_back({p, fs::path(), true});
        } else if (hasWildcard(pathToUtf8(p.filename()))) {
            fs::path dir = p.parent_path();
            if (dir.empty()) dir = ".";
            const std::string pattern = pathToUtf8(p.filename());
            std::vector<fs::path> found =
                listFolder(dir, [&](const fs::path& f) { return wildcardMatch(pattern, pathToUtf8(f.filename())); });
            if (found.empty()) fail(strprintf("no files match '%s'", arg.c_str()));
            sortNatural(found);
            for (auto& f : found) items.push_back({f, fs::path(), false});
        } else {
            fail(strprintf("'%s' does not exist", arg.c_str()));
        }
    }

    std::vector<InputSet> jobs;
    std::vector<fs::path> stills;
    fs::path stillsDir;
    bool stillsFromOneDir = true;
    std::set<std::string> seen;
    for (const Item& it : items) {
        fs::path path = it.path;
        std::string e = lowerExtension(path);
        if (isMlvChunkExtension(e)) {
            // A spanned part: the clip is read through its main .MLV file.
            fs::path main = mainMlvOf(path);
            if (main.empty()) {
                if (!it.named) continue;
                fail(strprintf("'%s' is part of a spanned MLV clip, but its .MLV file is missing",
                               pathToUtf8(path).c_str()));
            }
            path = main;
            e = lowerExtension(path);
        }
        const std::string key = pathToUtf8(fs::absolute(path, ec).lexically_normal());
        if (!seen.insert(key).second) continue;
        if (isClipExtension(e)) {
            jobs.push_back(clipJob(path, false));
        } else if (e == ".cr3" && isCr3Roll(path)) {
            jobs.push_back(clipJob(path, false));  // a RAW burst roll is a clip of its own
        } else {
            if (stills.empty()) stillsDir = it.fromDir;
            else if (it.fromDir != stillsDir) stillsFromOneDir = false;
            stills.push_back(path);
        }
    }

    if (stills.size() == 1) {
        jobs.push_back(clipJob(stills[0], lowerExtension(stills[0]) != ".cr3"));
    } else if (!stills.empty()) {
        InputSet job;
        job.files = stills;
        job.sequence = true;
        job.label = strprintf("%llu files (%s ... %s)", static_cast<unsigned long long>(stills.size()),
                              pathToUtf8(stills.front().filename()).c_str(),
                              pathToUtf8(stills.back().filename()).c_str());
        job.defaultOutput = sequenceOutputName(stills, stillsFromOneDir ? stillsDir : fs::path());
        jobs.push_back(job);
    }
    if (jobs.empty()) fail("no input files");
    return jobs;
}

std::unique_ptr<Reader> openReader(const InputSet& input, const ReaderOptions& options) {
    if (input.files.empty()) fail("no input files");
    const std::string backend = toLower(options.backend);
    if (backend != "auto" && backend != "mlv" && backend != "libraw" && backend != "ffmpeg") {
        fail(strprintf("unknown --backend '%s' (use auto, mlv, libraw or ffmpeg)", options.backend.c_str()));
    }
    if (input.sequence) {
        if (backend == "mlv" || backend == "ffmpeg") fail("image sequences are read with LibRaw only");
        return openLibRawSequence(input.files);
    }
    const fs::path& path = input.files[0];
    const std::string ext = lowerExtension(path);

    if (backend == "ffmpeg") {
#if defined(SERCONV_HAVE_FFMPEG)
        return openFFmpegReader(path);
#else
        fail("this serconv was built without FFmpeg");
#endif
    }
    if (backend == "libraw") {
        if (ext == ".crm" || ext == ".cr3") return openLibRawClip(path);
        return openLibRawSequence({path});
    }
    if (backend == "mlv") return openMlvReader(path);

    if (ext == ".mlv") return openMlvReader(path);
    if (ext == ".crm" || ext == ".cr3") return openLibRawClip(path);
    if (isVideoExtension(ext)) {
#if defined(SERCONV_HAVE_FFMPEG)
        return openFFmpegReader(path);
#else
        fail(strprintf("'%s': reading %s files (ProRes RAW and other MOV/MP4 RAW video) needs FFmpeg, and this "
                       "serconv was built without it",
                       pathToUtf8(path.filename()).c_str(), ext.c_str()));
#endif
    }
    if (isStillRawExtension(ext)) return openLibRawSequence({path});

    // Unknown extension: look at the content.
    uint8_t head[16] = {0};
    {
        File f(path, "rb");
        f.readSome(head, sizeof head);
    }
    if (std::memcmp(head, "MLVI", 4) == 0) return openMlvReader(path);
    if (std::memcmp(head + 4, "ftypcrx ", 8) == 0) return openLibRawClip(path);
#if defined(SERCONV_HAVE_FFMPEG)
    if (std::memcmp(head + 4, "ftyp", 4) == 0 || std::memcmp(head, "RIFF", 4) == 0 ||
        std::memcmp(head + 4, "moov", 4) == 0 || std::memcmp(head + 4, "mdat", 4) == 0 ||
        std::memcmp(head + 4, "wide", 4) == 0) {
        return openFFmpegReader(path);
    }
#endif
    return openLibRawSequence({path});
}

std::string buildSummary() {
    std::string s = strprintf("serconv %s\n", SERCONV_VERSION);
    s += "LibRaw: " + librawVersionString() + " (built from source; CR3/CRM size limit lifted)\n";
#if defined(SERCONV_HAVE_FFMPEG)
    s += "FFmpeg: " + ffmpegVersionString() + ", ProRes RAW decoder: " + (ffmpegHasProResRaw() ? "yes" : "no") + "\n";
#else
    s += "FFmpeg: not in this build (no ProRes RAW, MOV or MP4 input)\n";
#endif
    return s;
}

std::string formatsHelp() {
    std::string s;
    s += "Input formats\n";
    s += "  .MLV (+.M00...)   Magic Lantern Video: 10/12/14-bit uncompressed and LJ92 lossless (built in)\n";
    s += "  .CRM              Canon Cinema RAW Light (LibRaw)\n";
    s += "  .CR3              Canon CR3, including RAW burst rolls (LibRaw)\n";
    s += "  folder or files   DNG/CinemaDNG and still raw sequences: CR2, NEF, ARW, RAF (Bayer), ORF, RW2, ... (LibRaw)\n";
#if defined(SERCONV_HAVE_FFMPEG)
    s += std::string("  .MOV/.MP4/...     Apple ProRes RAW") + (ffmpegHasProResRaw() ? "" : " (not in this FFmpeg)") +
         ", CineForm RAW and other Bayer/grey/RGB streams (FFmpeg)\n";
#else
    s += "  .MOV/.MP4/...     not available: built without FFmpeg\n";
#endif
    s += "Not supported (closed or patented codecs): Nikon N-RAW (.NEV), Blackmagic RAW (.braw), RED (.R3D),\n";
    s += "  ARRIRAW, Sony X-OCN. For Nikon Z cameras record ProRes RAW instead.\n";
    s += "\nOutput: SER v3, Bayer/mono/RGB, 8 or 16-bit (or the sensor's bit depth with --levels raw),\n";
    s += "  with a UTC timestamp trailer when the frame times are known.\n";
    return s;
}

}  // namespace serconv
