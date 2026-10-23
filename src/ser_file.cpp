#include "ser_file.hpp"

#include <cstring>
#include <system_error>

#include "frame.hpp"

namespace serconv {

namespace {

// The SER string fields are 40 ASCII characters padded with zero bytes.
void putString(uint8_t* dst, const std::string& s) {
    std::memset(dst, 0, 40);
    size_t out = 0;
    for (size_t i = 0; i < s.size() && out < 40;) {
        unsigned char c = static_cast<unsigned char>(s[i]);
        if (c < 0x80) {
            dst[out++] = (c >= 32 && c < 127) ? c : uint8_t('?');
            ++i;
        } else {
            // One '?' per UTF-8 code point.
            dst[out++] = '?';
            ++i;
            while (i < s.size() && (static_cast<unsigned char>(s[i]) & 0xC0) == 0x80) ++i;
        }
    }
}

}  // namespace

void encodeSerHeader(const SerHeader& h, uint8_t out[kSerHeaderSize]) {
    std::memset(out, 0, kSerHeaderSize);
    std::memcpy(out, "LUCAM-RECORDER", 14);
    wr32le(out + 14, uint32_t(h.luId));
    wr32le(out + 18, uint32_t(h.colorId));
    wr32le(out + 22, uint32_t(h.littleEndian));
    wr32le(out + 26, uint32_t(h.width));
    wr32le(out + 30, uint32_t(h.height));
    wr32le(out + 34, uint32_t(h.pixelDepth));
    wr32le(out + 38, uint32_t(h.frameCount));
    putString(out + 42, h.observer);
    putString(out + 82, h.instrument);
    putString(out + 122, h.telescope);
    wr64le(out + 162, uint64_t(h.dateTime));
    wr64le(out + 170, uint64_t(h.dateTimeUtc));
}

bool decodeSerHeader(const uint8_t in[kSerHeaderSize], SerHeader& h, std::string* error) {
    if (std::memcmp(in, "LUCAM-RECORDER", 14) != 0) {
        if (error) *error = "not a SER file (no LUCAM-RECORDER signature)";
        return false;
    }
    h.luId = int32_t(rd32le(in + 14));
    h.colorId = int32_t(rd32le(in + 18));
    h.littleEndian = int32_t(rd32le(in + 22));
    h.width = int32_t(rd32le(in + 26));
    h.height = int32_t(rd32le(in + 30));
    h.pixelDepth = int32_t(rd32le(in + 34));
    h.frameCount = int32_t(rd32le(in + 38));
    h.observer = fixedString(in + 42, 40);
    h.instrument = fixedString(in + 82, 40);
    h.telescope = fixedString(in + 122, 40);
    h.dateTime = int64_t(rd64le(in + 162));
    h.dateTimeUtc = int64_t(rd64le(in + 170));
    if (h.width <= 0 || h.height <= 0 || h.pixelDepth < 1 || h.pixelDepth > 16) {
        if (error) *error = strprintf("invalid SER header (size %dx%d, depth %d)", h.width, h.height, h.pixelDepth);
        return false;
    }
    return true;
}

// ---- writer -----------------------------------------------------------------

SerWriter::~SerWriter() {
    if (!finished_) abort();
}

void SerWriter::open(const fs::path& path, const SerHeader& header, bool overwrite) {
    std::error_code ec;
    if (!overwrite && fs::exists(path, ec)) {
        fail(strprintf("'%s' exists already (use --force to overwrite it)", pathToUtf8(path).c_str()));
    }
    path_ = path;
    partPath_ = path;
    partPath_ += ".part";
    header_ = header;
    header_.frameCount = 0;
    file_.open(partPath_, "wb");
    // Large buffer: frames are written sequentially.
    std::setvbuf(file_.get(), nullptr, _IOFBF, 4 << 20);
    uint8_t buf[kSerHeaderSize];
    encodeSerHeader(header_, buf);
    file_.write(buf, sizeof buf);
    frames_ = 0;
    finished_ = false;
}

void SerWriter::writeFrame(const uint8_t* data, size_t bytes) {
    if (int64_t(bytes) != header_.frameBytes()) {
        fail(strprintf("internal error: frame of %llu bytes, SER frame size is %lld", static_cast<unsigned long long>(bytes),
                       static_cast<long long>(header_.frameBytes())));
    }
    if (frames_ >= 0x7FFFFFFF) fail("a SER file cannot hold more than 2147483647 frames");
    file_.write(data, bytes);
    ++frames_;
}

int64_t SerWriter::bytesWritten() const { return kSerHeaderSize + frames_ * header_.frameBytes(); }

void SerWriter::finish(int64_t dateTimeLocal, int64_t dateTimeUtc, const std::vector<int64_t>& timestamps) {
    if (!timestamps.empty() && int64_t(timestamps.size()) != frames_) {
        fail("internal error: timestamp count does not match the frame count");
    }
    if (!timestamps.empty()) {
        std::vector<uint8_t> trailer(timestamps.size() * 8);
        for (size_t i = 0; i < timestamps.size(); ++i) wr64le(&trailer[i * 8], uint64_t(timestamps[i]));
        file_.write(trailer.data(), trailer.size());
    }
    header_.frameCount = int32_t(frames_);
    header_.dateTime = dateTimeLocal;
    header_.dateTimeUtc = dateTimeUtc;
    uint8_t buf[kSerHeaderSize];
    encodeSerHeader(header_, buf);
    file_.flush();
    file_.seek(0);
    file_.write(buf, sizeof buf);
    file_.flush();
    file_.closeChecked();

    std::error_code ec;
    fs::rename(partPath_, path_, ec);
    if (ec) {
        // Windows will not rename over an existing file.
        std::error_code ec2;
        fs::remove(path_, ec2);
        fs::rename(partPath_, path_, ec);
        if (ec) fail(strprintf("cannot rename '%s' to '%s': %s", pathToUtf8(partPath_).c_str(),
                               pathToUtf8(path_).c_str(), ec.message().c_str()));
    }
    finished_ = true;
}

void SerWriter::abort() {
    if (file_.isOpen()) file_.close();
    if (!partPath_.empty()) {
        std::error_code ec;
        fs::remove(partPath_, ec);
    }
    finished_ = true;
}

// ---- reader -----------------------------------------------------------------

void SerReader::open(const fs::path& path) {
    file_.open(path, "rb");
    fileSize_ = file_.size();
    if (fileSize_ < kSerHeaderSize) fail(strprintf("'%s' is too small to be a SER file", pathToUtf8(path).c_str()));
    uint8_t buf[kSerHeaderSize];
    file_.readAt(0, buf, sizeof buf);
    std::string error;
    if (!decodeSerHeader(buf, header_, &error)) fail(pathToUtf8(path) + ": " + error);

    problems_.clear();
    timestamps_.clear();
    Layout layout;
    Cfa cfa;
    bool bgr;
    if (!layoutFromSerColorId(header_.colorId, layout, cfa, bgr)) {
        problems_.push_back(strprintf("ColorID %d (%s) is not one serconv reads", header_.colorId,
                                      serColorName(header_.colorId).c_str()));
    }
    if (header_.littleEndian != 0 && header_.littleEndian != 1) {
        problems_.push_back(strprintf("LittleEndian field is %d (expected 0 or 1)", header_.littleEndian));
    }

    const int64_t fb = header_.frameBytes();
    const int64_t payload = fileSize_ - kSerHeaderSize;
    const int64_t fitting = fb > 0 ? payload / fb : 0;
    frameCount_ = header_.frameCount;
    if (frameCount_ <= 0 || frameCount_ > fitting) {
        if (frameCount_ > fitting) {
            problems_.push_back(strprintf("header says %d frames but the file holds only %lld", header_.frameCount,
                                          static_cast<long long>(fitting)));
        } else {
            problems_.push_back("header frame count is 0; counted the frames in the file");
        }
        frameCount_ = fitting;
    }
    const int64_t afterFrames = kSerHeaderSize + frameCount_ * fb;
    const int64_t rest = fileSize_ - afterFrames;
    if (rest == frameCount_ * 8 && frameCount_ > 0) {
        timestamps_.resize(size_t(frameCount_));
        std::vector<uint8_t> t(static_cast<size_t>(rest));
        file_.readAt(afterFrames, t.data(), t.size());
        for (int64_t i = 0; i < frameCount_; ++i) timestamps_[size_t(i)] = int64_t(rd64le(&t[size_t(i) * 8]));
    } else if (rest != 0) {
        problems_.push_back(strprintf("%lld bytes after the frames are not a timestamp trailer",
                                      static_cast<long long>(rest)));
    }
}

void SerReader::readFrame(int64_t index, std::vector<uint8_t>& out) {
    if (index < 0 || index >= frameCount_) fail("SER frame index out of range");
    const int64_t fb = header_.frameBytes();
    out.resize(size_t(fb));
    file_.readAt(kSerHeaderSize + index * fb, out.data(), out.size());
}

void SerReader::readFrame16(int64_t index, std::vector<uint16_t>& out) {
    std::vector<uint8_t> raw;
    readFrame(index, raw);
    if (header_.bytesPerSample() == 1) {
        out.assign(raw.begin(), raw.end());
        return;
    }
    out.resize(raw.size() / 2);
    const bool be = bigEndianData();
    for (size_t i = 0; i < out.size(); ++i) out[i] = be ? rd16be(&raw[i * 2]) : rd16le(&raw[i * 2]);
}

}  // namespace serconv
