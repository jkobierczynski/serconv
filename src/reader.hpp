// Input readers: one per source format, all delivering RawFrame.
#pragma once

#include <memory>
#include <string>
#include <vector>

#include "frame.hpp"
#include "util.hpp"

namespace serconv {

struct ReaderOptions {
    // "auto", "mlv" (native), "libraw" or "ffmpeg".
    std::string backend = "auto";
};

class Reader {
public:
    virtual ~Reader() = default;
    virtual const StreamInfo& info() const = 0;

    // Decodes frame `index` (0-based). Random-access readers take any order;
    // sequential readers need increasing indices and may skip ahead. Returns
    // false when the stream ends before `index` (only when the frame count is
    // unknown); throws Error on corrupt data.
    virtual bool read(int64_t index, RawFrame& frame) = 0;

    virtual bool randomAccess() const { return true; }
    // An independent reader on the same input, for parallel decoding, or
    // nullptr when the format cannot be read in parallel.
    virtual std::unique_ptr<Reader> clone() const { return nullptr; }
    // Time of frame `index` in seconds after info().originTicks, if known
    // without decoding; NaN otherwise.
    virtual double frameTime(int64_t index) const {
        (void)index;
        return NAN;
    }
};

// One conversion job: a single container file, or a sequence of stills.
struct InputSet {
    std::vector<fs::path> files;
    bool sequence = false;  // files are frames of one sequence
    std::string label;      // for messages and the default output name
    fs::path defaultOutput;
};

// Expands directories and wildcards and groups the inputs into jobs: each
// video container is a job, all still raw files together form one sequence.
std::vector<InputSet> collectInputs(const std::vector<std::string>& args);

std::unique_ptr<Reader> openReader(const InputSet& input, const ReaderOptions& options);

// Format readers (in their own source files).
std::unique_ptr<Reader> openMlvReader(const fs::path& path);
std::unique_ptr<Reader> openLibRawClip(const fs::path& path);
std::unique_ptr<Reader> openLibRawSequence(const std::vector<fs::path>& files);
#if defined(SERCONV_HAVE_FFMPEG)
std::unique_ptr<Reader> openFFmpegReader(const fs::path& path);
std::string ffmpegVersionString();
bool ffmpegHasProResRaw();
#endif
std::string librawVersionString();
// Frames in a raw file according to LibRaw (CR3 rolls hold many), -1 on error.
int librawFrameCount(const fs::path& path);

// What the build supports, for --version and `serconv formats`.
std::string buildSummary();
std::string formatsHelp();

// Extensions handled as still raw images (one frame per file) and as clips.
bool isStillRawExtension(const std::string& lowerExt);
bool isClipExtension(const std::string& lowerExt);

}  // namespace serconv
