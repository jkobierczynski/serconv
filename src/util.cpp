#include "util.hpp"

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cstring>
#include <mutex>

#if defined(_WIN32)
#include <io.h>
#include <windows.h>
#else
#include <unistd.h>
#endif

namespace serconv {

std::string vstrprintf(const char* fmt, va_list ap) {
    va_list ap2;
    va_copy(ap2, ap);
    char small[512];
    int n = std::vsnprintf(small, sizeof small, fmt, ap);
    if (n < 0) {
        va_end(ap2);
        return std::string();
    }
    if (size_t(n) < sizeof small) {
        va_end(ap2);
        return std::string(small, size_t(n));
    }
    std::string out(size_t(n) + 1, '\0');
    std::vsnprintf(&out[0], out.size(), fmt, ap2);
    va_end(ap2);
    out.resize(size_t(n));
    return out;
}

std::string strprintf(const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    std::string s = vstrprintf(fmt, ap);
    va_end(ap);
    return s;
}

void fail(const std::string& message) { throw Error(message); }

// ---- logging ---------------------------------------------------------------

namespace {
std::mutex g_logMutex;
LogLevel g_level = LogLevel::Normal;
bool g_progressActive = false;
size_t g_progressLen = 0;

void endProgressLocked() {
    if (g_progressActive) {
        std::fputc('\n', stderr);
        g_progressActive = false;
        g_progressLen = 0;
    }
}

void emit(const std::string& line) {
    std::lock_guard<std::mutex> lock(g_logMutex);
    endProgressLocked();
    std::fputs(line.c_str(), stderr);
    std::fputc('\n', stderr);
    std::fflush(stderr);
}
}  // namespace

void setLogLevel(LogLevel level) { g_level = level; }
LogLevel logLevel() { return g_level; }

void logInfo(const std::string& message) {
    if (g_level >= LogLevel::Normal) emit(message);
}
void logVerbose(const std::string& message) {
    if (g_level >= LogLevel::Verbose) emit(message);
}
void logWarning(const std::string& message) { emit("warning: " + message); }
void logError(const std::string& message) { emit("error: " + message); }

bool stderrIsTerminal() {
#if defined(_WIN32)
    return _isatty(_fileno(stderr)) != 0;
#else
    return isatty(fileno(stderr)) != 0;
#endif
}

void logProgress(const std::string& message) {
    if (g_level < LogLevel::Normal) return;
    std::lock_guard<std::mutex> lock(g_logMutex);
    if (stderrIsTerminal()) {
        std::string line = "\r" + message;
        if (message.size() < g_progressLen) line.append(g_progressLen - message.size(), ' ');
        std::fputs(line.c_str(), stderr);
        g_progressLen = message.size();
        g_progressActive = true;
    } else {
        std::fputs(message.c_str(), stderr);
        std::fputc('\n', stderr);
    }
    std::fflush(stderr);
}

void finishProgress() {
    std::lock_guard<std::mutex> lock(g_logMutex);
    endProgressLocked();
    std::fflush(stderr);
}

// ---- strings and paths -----------------------------------------------------

fs::path pathFromUtf8(const std::string& s) {
#if defined(_WIN32)
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), int(s.size()), nullptr, 0);
    std::wstring w(size_t(n), L'\0');
    if (n > 0) MultiByteToWideChar(CP_UTF8, 0, s.data(), int(s.size()), &w[0], n);
    return fs::path(w);
#else
    return fs::path(s);
#endif
}

std::string pathToUtf8(const fs::path& p) {
#if defined(_WIN32)
    const std::wstring& w = p.native();
    int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), int(w.size()), nullptr, 0, nullptr, nullptr);
    std::string s(size_t(n), '\0');
    if (n > 0) WideCharToMultiByte(CP_UTF8, 0, w.data(), int(w.size()), &s[0], n, nullptr, nullptr);
    return s;
#else
    return p.string();
#endif
}

std::string toLower(std::string s) {
    for (char& c : s) c = char(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

std::string toUpper(std::string s) {
    for (char& c : s) c = char(std::toupper(static_cast<unsigned char>(c)));
    return s;
}

std::string lowerExtension(const fs::path& p) { return toLower(pathToUtf8(p.extension())); }

std::string trim(const std::string& s) {
    size_t b = 0, e = s.size();
    while (b < e && std::isspace(static_cast<unsigned char>(s[b]))) ++b;
    while (e > b && std::isspace(static_cast<unsigned char>(s[e - 1]))) --e;
    return s.substr(b, e - b);
}

std::vector<std::string> split(const std::string& s, char sep) {
    std::vector<std::string> out;
    size_t start = 0;
    for (;;) {
        size_t pos = s.find(sep, start);
        if (pos == std::string::npos) {
            out.push_back(s.substr(start));
            break;
        }
        out.push_back(s.substr(start, pos - start));
        start = pos + 1;
    }
    return out;
}

bool startsWith(const std::string& s, const std::string& prefix) {
    return s.size() >= prefix.size() && s.compare(0, prefix.size(), prefix) == 0;
}

bool naturalLess(const std::string& a, const std::string& b) {
    size_t i = 0, j = 0;
    while (i < a.size() && j < b.size()) {
        unsigned char ca = static_cast<unsigned char>(a[i]);
        unsigned char cb = static_cast<unsigned char>(b[j]);
        if (std::isdigit(ca) && std::isdigit(cb)) {
            size_t ie = i, je = j;
            while (ie < a.size() && std::isdigit(static_cast<unsigned char>(a[ie]))) ++ie;
            while (je < b.size() && std::isdigit(static_cast<unsigned char>(b[je]))) ++je;
            size_t is = i, js = j;
            while (is + 1 < ie && a[is] == '0') ++is;
            while (js + 1 < je && b[js] == '0') ++js;
            size_t la = ie - is, lb = je - js;
            if (la != lb) return la < lb;
            int c = a.compare(is, la, b, js, lb);
            if (c != 0) return c < 0;
            i = ie;
            j = je;
            continue;
        }
        int la = std::tolower(ca), lb = std::tolower(cb);
        if (la != lb) return la < lb;
        ++i;
        ++j;
    }
    if (a.size() - i != b.size() - j) return a.size() - i < b.size() - j;
    return a < b;
}

std::string fixedString(const uint8_t* p, size_t n) {
    size_t len = 0;
    while (len < n && p[len] != 0) ++len;
    return trim(std::string(reinterpret_cast<const char*>(p), len));
}

std::string withThousands(int64_t v) {
    bool neg = v < 0;
    uint64_t u = neg ? uint64_t(-(v + 1)) + 1 : uint64_t(v);
    std::string digits = std::to_string(u);
    std::string out;
    int count = 0;
    for (size_t k = digits.size(); k-- > 0;) {
        out.push_back(digits[k]);
        if (++count % 3 == 0 && k > 0) out.push_back(',');
    }
    if (neg) out.push_back('-');
    std::reverse(out.begin(), out.end());
    return out;
}

std::string humanBytes(double bytes) {
    const char* units[] = {"bytes", "KB", "MB", "GB", "TB"};
    int u = 0;
    while (bytes >= 1000.0 && u < 4) {
        bytes /= 1000.0;
        ++u;
    }
    if (u == 0) return strprintf("%.0f bytes", bytes);
    return strprintf("%.1f %s", bytes, units[u]);
}

bool hostIsLittleEndian() {
    const uint16_t probe = 1;
    uint8_t first;
    std::memcpy(&first, &probe, 1);
    return first == 1;
}

// ---- File ------------------------------------------------------------------

File::File(const fs::path& path, const char* mode) { open(path, mode); }

File::~File() { close(); }

File::File(File&& o) noexcept : f_(o.f_), path_(std::move(o.path_)) { o.f_ = nullptr; }

File& File::operator=(File&& o) noexcept {
    if (this != &o) {
        close();
        f_ = o.f_;
        path_ = std::move(o.path_);
        o.f_ = nullptr;
    }
    return *this;
}

void File::open(const fs::path& path, const char* mode) {
    close();
    path_ = path;
#if defined(_WIN32)
    std::wstring wmode;
    for (const char* m = mode; *m; ++m) wmode.push_back(wchar_t(*m));
    f_ = _wfopen(path.c_str(), wmode.c_str());
#else
    f_ = std::fopen(path.c_str(), mode);
#endif
    if (!f_) {
        fail(strprintf("cannot open '%s': %s", pathToUtf8(path).c_str(), std::strerror(errno)));
    }
}

void File::close() {
    if (f_) {
        std::fclose(f_);
        f_ = nullptr;
    }
}

void File::closeChecked() {
    if (!f_) return;
    FILE* f = f_;
    f_ = nullptr;
    if (std::fclose(f) != 0) {
        fail(strprintf("cannot write to '%s': %s", pathToUtf8(path_).c_str(), std::strerror(errno)));
    }
}

int64_t File::tell() {
#if defined(_WIN32)
    return _ftelli64(f_);
#else
    return int64_t(ftello(f_));
#endif
}

void File::seek(int64_t offset) {
#if defined(_WIN32)
    int r = _fseeki64(f_, offset, SEEK_SET);
#else
    int r = fseeko(f_, off_t(offset), SEEK_SET);
#endif
    if (r != 0) fail(strprintf("cannot seek in '%s'", pathToUtf8(path_).c_str()));
}

int64_t File::size() {
    int64_t here = tell();
#if defined(_WIN32)
    _fseeki64(f_, 0, SEEK_END);
#else
    fseeko(f_, 0, SEEK_END);
#endif
    int64_t end = tell();
    seek(here);
    return end;
}

void File::read(void* buf, size_t n) {
    if (n == 0) return;
    if (std::fread(buf, 1, n, f_) != n) {
        fail(strprintf("unexpected end of file in '%s'", pathToUtf8(path_).c_str()));
    }
}

size_t File::readSome(void* buf, size_t n) { return n ? std::fread(buf, 1, n, f_) : 0; }

void File::readAt(int64_t offset, void* buf, size_t n) {
    seek(offset);
    read(buf, n);
}

void File::write(const void* buf, size_t n) {
    if (n && std::fwrite(buf, 1, n, f_) != n) {
        fail(strprintf("cannot write to '%s': %s", pathToUtf8(path_).c_str(), std::strerror(errno)));
    }
}

void File::flush() {
    if (std::fflush(f_) != 0) {
        fail(strprintf("cannot write to '%s': %s", pathToUtf8(path_).c_str(), std::strerror(errno)));
    }
}

}  // namespace serconv
