// Minimal test helpers: checks that count failures, temporary directories,
// running the serconv binary.
#pragma once

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <random>
#include <sstream>
#include <string>
#include <vector>

#include "util.hpp"

#if !defined(_WIN32)
#include <sys/wait.h>
#endif

namespace testutil {

inline int& failures() {
    static int n = 0;
    return n;
}

template <typename A, typename B>
void checkEq(const A& a, const B& b, const char* ea, const char* eb, const char* file, int line) {
    if (!(a == b)) {
        std::ostringstream os;
        os << a << " != " << b;
        std::fprintf(stderr, "%s:%d: CHECK_EQ(%s, %s) failed: %s\n", file, line, ea, eb, os.str().c_str());
        ++failures();
    }
}

inline int finish(const char* name) {
    if (failures()) {
        std::fprintf(stderr, "%s: %d check(s) failed\n", name, failures());
        return 1;
    }
    std::printf("%s: all checks passed\n", name);
    return 0;
}

struct TempDir {
    std::filesystem::path path;
    explicit TempDir(const std::string& name) {
        std::random_device rd;
        path = std::filesystem::temp_directory_path() /
               ("serconv-test-" + name + "-" + std::to_string(rd() % 1000000));
        std::filesystem::remove_all(path);
        std::filesystem::create_directories(path);
    }
    ~TempDir() {
        std::error_code ec;
        if (!std::getenv("SERCONV_KEEP_TEST_FILES")) std::filesystem::remove_all(path, ec);
    }
    std::filesystem::path operator/(const std::string& name) const { return path / serconv::pathFromUtf8(name); }
};

// Runs `exe args...`, captures stdout+stderr into *output, returns the exit status.
inline int runProgram(const std::string& exe, const std::vector<std::string>& args, std::string* output,
                      const std::filesystem::path& scratch) {
    auto quote = [](const std::string& s) { return "\"" + s + "\""; };
    std::string cmd = quote(exe);
    for (const auto& a : args) cmd += " " + quote(a);
    const std::filesystem::path out = scratch / "command-output.txt";
    cmd += " > " + quote(serconv::pathToUtf8(out)) + " 2>&1";
#if defined(_WIN32)
    cmd = "\"" + cmd + "\"";  // cmd.exe strips one pair of outer quotes
#endif
    int rc = std::system(cmd.c_str());
#if !defined(_WIN32)
    if (rc != -1 && WIFEXITED(rc)) rc = WEXITSTATUS(rc);
#endif
    if (output) {
        output->clear();
        if (FILE* f = std::fopen(serconv::pathToUtf8(out).c_str(), "rb")) {
            char buf[4096];
            size_t n;
            while ((n = std::fread(buf, 1, sizeof buf, f)) > 0) output->append(buf, n);
            std::fclose(f);
        }
    }
    return rc;
}

}  // namespace testutil

#define CHECK(cond)                                                                      \
    do {                                                                                 \
        if (!(cond)) {                                                                   \
            std::fprintf(stderr, "%s:%d: CHECK(%s) failed\n", __FILE__, __LINE__, #cond); \
            ++testutil::failures();                                                      \
        }                                                                                \
    } while (0)

#define CHECK_EQ(a, b) testutil::checkEq((a), (b), #a, #b, __FILE__, __LINE__)

#define CHECK_NEAR(a, b, eps)                                                                                \
    do {                                                                                                     \
        double _a = double(a), _b = double(b);                                                               \
        if (!(std::fabs(_a - _b) <= (eps))) {                                                                \
            std::fprintf(stderr, "%s:%d: CHECK_NEAR(%s, %s) failed: %.9g vs %.9g\n", __FILE__, __LINE__, #a, #b, \
                         _a, _b);                                                                            \
            ++testutil::failures();                                                                          \
        }                                                                                                    \
    } while (0)

#define CHECK_THROWS(expr)                                                                         \
    do {                                                                                           \
        bool _threw = false;                                                                       \
        try {                                                                                      \
            expr;                                                                                  \
        } catch (const std::exception&) {                                                          \
            _threw = true;                                                                         \
        }                                                                                          \
        if (!_threw) {                                                                             \
            std::fprintf(stderr, "%s:%d: CHECK_THROWS(%s) did not throw\n", __FILE__, __LINE__, #expr); \
            ++testutil::failures();                                                                \
        }                                                                                          \
    } while (0)
