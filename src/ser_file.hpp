// SER v3 files (Heiko Wilkens / Grischa Hahn): 178-byte header, frames, and an
// optional trailer with one UTC timestamp per frame.
//
// Byte order: the specification calls the header field "LittleEndian" and says
// 1 means little-endian 16-bit data, but the early writers used it the other
// way round and FireCapture, SharpCap, PIPP, SER Player, Siril and
// AutoStakkert! all follow them: 0 means little-endian. serconv writes
// little-endian data with 0, like those programs, and reads 1 as big-endian.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "util.hpp"

namespace serconv {

constexpr int kSerHeaderSize = 178;

struct SerHeader {
    int32_t luId = 0;
    int32_t colorId = 0;
    int32_t littleEndian = 0;  // see the note above: 0 = little-endian data
    int32_t width = 0;
    int32_t height = 0;
    int32_t pixelDepth = 8;  // bits per sample, 1..16; above 8 samples take 2 bytes
    int32_t frameCount = 0;
    std::string observer;
    std::string instrument;
    std::string telescope;
    int64_t dateTime = 0;     // local time of the first frame, SER ticks
    int64_t dateTimeUtc = 0;  // UTC time of the first frame, SER ticks

    int bytesPerSample() const { return pixelDepth > 8 ? 2 : 1; }
    int planes() const { return colorId >= 100 ? 3 : 1; }
    int64_t frameBytes() const {
        return int64_t(width) * int64_t(height) * planes() * bytesPerSample();
    }
};

void encodeSerHeader(const SerHeader& header, uint8_t out[kSerHeaderSize]);
bool decodeSerHeader(const uint8_t in[kSerHeaderSize], SerHeader& header, std::string* error);

// Writes a SER file. The data goes to "<path>.part", which finish() renames
// to the final name; a writer destroyed before finish() removes the part file.
class SerWriter {
public:
    SerWriter() = default;
    ~SerWriter();
    SerWriter(const SerWriter&) = delete;
    SerWriter& operator=(const SerWriter&) = delete;

    void open(const fs::path& path, const SerHeader& header, bool overwrite);
    // `bytes` must equal header.frameBytes(); 16-bit samples little-endian.
    void writeFrame(const uint8_t* data, size_t bytes);
    int64_t framesWritten() const { return frames_; }
    int64_t bytesWritten() const;
    // Sets the frame count and dates in the header and appends the trailer
    // when `timestamps` is not empty (it must have one entry per frame).
    void finish(int64_t dateTimeLocal, int64_t dateTimeUtc, const std::vector<int64_t>& timestamps);
    void abort();
    const fs::path& path() const { return path_; }

private:
    File file_;
    fs::path path_;
    fs::path partPath_;
    SerHeader header_;
    int64_t frames_ = 0;
    bool finished_ = false;
};

class SerReader {
public:
    void open(const fs::path& path);
    const SerHeader& header() const { return header_; }
    int64_t fileSize() const { return fileSize_; }
    int64_t frameCount() const { return frameCount_; }
    int64_t frameBytes() const { return header_.frameBytes(); }
    bool bigEndianData() const { return header_.littleEndian == 1 && header_.bytesPerSample() == 2; }
    const std::vector<int64_t>& timestamps() const { return timestamps_; }
    const std::vector<std::string>& problems() const { return problems_; }
    // Frame bytes as stored (16-bit samples in the file's byte order).
    void readFrame(int64_t index, std::vector<uint8_t>& out);
    // Frame as 16-bit host samples (8-bit data is widened).
    void readFrame16(int64_t index, std::vector<uint16_t>& out);

private:
    File file_;
    SerHeader header_;
    int64_t fileSize_ = 0;
    int64_t frameCount_ = 0;
    std::vector<int64_t> timestamps_;
    std::vector<std::string> problems_;
};

}  // namespace serconv
