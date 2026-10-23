// Small shared helpers: errors, logging, UTF-8 paths, files, byte order.
#pragma once

#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <stdexcept>
#include <string>
#include <vector>

namespace serconv {

namespace fs = std::filesystem;

// Every user-facing failure is an Error; main() prints its message.
struct Error : std::runtime_error {
    using std::runtime_error::runtime_error;
};

// printf-style formatting into a std::string.
std::string strprintf(const char* fmt, ...)
#if defined(__MINGW32__)
    __attribute__((format(__MINGW_PRINTF_FORMAT, 1, 2)))
#elif defined(__GNUC__)
    __attribute__((format(printf, 1, 2)))
#endif
    ;
std::string vstrprintf(const char* fmt, va_list ap);

[[noreturn]] void fail(const std::string& message);

// ---- logging (stderr, thread safe) -----------------------------------------

enum class LogLevel { Quiet = 0, Normal = 1, Verbose = 2 };
void setLogLevel(LogLevel level);
LogLevel logLevel();
void logInfo(const std::string& message);     // shown unless --quiet
void logVerbose(const std::string& message);  // only with --verbose
void logWarning(const std::string& message);  // always shown, prefixed "warning: "
void logError(const std::string& message);    // always shown, prefixed "error: "
// Progress line that overwrites itself on a terminal; finishProgress() ends it.
void logProgress(const std::string& message);
void finishProgress();
bool stderrIsTerminal();

// ---- paths and files --------------------------------------------------------

fs::path pathFromUtf8(const std::string& s);
std::string pathToUtf8(const fs::path& p);
std::string lowerExtension(const fs::path& p);  // ".mlv" for "A001.MLV"
std::string toLower(std::string s);
std::string toUpper(std::string s);
std::string trim(const std::string& s);
std::vector<std::string> split(const std::string& s, char sep);
bool startsWith(const std::string& s, const std::string& prefix);
// Natural ordering: "frame2" before "frame10".
bool naturalLess(const std::string& a, const std::string& b);
// A string from a fixed-size, possibly unterminated char field.
std::string fixedString(const uint8_t* p, size_t n);
std::string withThousands(int64_t v);  // 1234567 -> "1,234,567"
std::string humanBytes(double bytes);  // "1.2 GB"

// RAII wrapper around FILE* with 64-bit offsets and UTF-8 paths.
class File {
public:
    File() = default;
    File(const fs::path& path, const char* mode);
    ~File();
    File(const File&) = delete;
    File& operator=(const File&) = delete;
    File(File&& o) noexcept;
    File& operator=(File&& o) noexcept;

    void open(const fs::path& path, const char* mode);  // throws Error
    bool isOpen() const { return f_ != nullptr; }
    void close();         // ignores errors (for reading, and cleanup)
    void closeChecked();  // throws when the last writes cannot be completed
    FILE* get() const { return f_; }
    const fs::path& path() const { return path_; }

    int64_t size();
    int64_t tell();
    void seek(int64_t offset);
    // Reads exactly n bytes at the current position, throws on a short read.
    void read(void* buf, size_t n);
    // Reads up to n bytes, returns the number read.
    size_t readSome(void* buf, size_t n);
    void readAt(int64_t offset, void* buf, size_t n);
    void write(const void* buf, size_t n);
    void flush();

private:
    FILE* f_ = nullptr;
    fs::path path_;
};

// ---- byte order -------------------------------------------------------------

inline uint16_t rd16le(const uint8_t* p) { return uint16_t(p[0] | (p[1] << 8)); }
inline uint32_t rd32le(const uint8_t* p) {
    return uint32_t(p[0]) | (uint32_t(p[1]) << 8) | (uint32_t(p[2]) << 16) | (uint32_t(p[3]) << 24);
}
inline uint64_t rd64le(const uint8_t* p) { return uint64_t(rd32le(p)) | (uint64_t(rd32le(p + 4)) << 32); }
inline uint16_t rd16be(const uint8_t* p) { return uint16_t((p[0] << 8) | p[1]); }
inline uint32_t rd32be(const uint8_t* p) {
    return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | uint32_t(p[3]);
}
inline uint64_t rd64be(const uint8_t* p) { return (uint64_t(rd32be(p)) << 32) | uint64_t(rd32be(p + 4)); }

inline void wr16le(uint8_t* p, uint16_t v) { p[0] = uint8_t(v); p[1] = uint8_t(v >> 8); }
inline void wr32le(uint8_t* p, uint32_t v) {
    p[0] = uint8_t(v); p[1] = uint8_t(v >> 8); p[2] = uint8_t(v >> 16); p[3] = uint8_t(v >> 24);
}
inline void wr64le(uint8_t* p, uint64_t v) { wr32le(p, uint32_t(v)); wr32le(p + 4, uint32_t(v >> 32)); }

bool hostIsLittleEndian();

}  // namespace serconv
