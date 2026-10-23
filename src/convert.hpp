// The conversion: read frames, process them, write SER, timestamps and sidecar.
#pragma once

#include <string>
#include <vector>

#include "process.hpp"
#include "reader.hpp"

namespace serconv {

struct ConvertOptions {
    std::vector<std::string> inputs;
    std::string output;  // file, or directory for several inputs
    bool force = false;
    ReaderOptions reader;
    ProcessOptions process;

    // Frame selection, 1-based and inclusive.
    int64_t first = 1;
    int64_t last = -1;  // -1: to the end
    int64_t every = 1;
    int64_t maxFrames = -1;

    // Timing.
    bool utcOffsetSet = false;
    int64_t utcOffset = 0;  // camera clock minus UTC, in ticks
    bool startTimeSet = false;
    int64_t startTimeUtc = 0;  // UTC time of the first frame of the clip
    double fps = 0;            // > 0: frame times are frame number / fps
    bool timestamps = true;    // write the SER timestamp trailer

    std::string observer;
    std::string instrument;  // default: the camera name
    bool instrumentSet = false;
    std::string telescope;

    bool sidecar = true;
    int threads = 0;  // 0: automatic
    std::string commandLine;
};

// Converts every input; returns the process exit code.
int runConvert(const ConvertOptions& options);

// Number of decoding threads for `requested` (0 = automatic).
int resolveThreads(int requested);

}  // namespace serconv
