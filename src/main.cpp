// serconv: cinema and mirrorless RAW video to SER.

#include <cstdio>
#include <new>
#include <string>
#include <vector>

#include "info.hpp"
#include "options.hpp"

#if defined(_WIN32)
#include <windows.h>
#include <shellapi.h>
#endif

using namespace serconv;

int main(int argc, char** argv) {
    std::vector<std::string> args;
#if defined(_WIN32)
    // Take the arguments as UTF-16 so non-ASCII file names survive.
    (void)argc;
    (void)argv;
    int wargc = 0;
    LPWSTR* wargv = CommandLineToArgvW(GetCommandLineW(), &wargc);
    for (int i = 1; wargv && i < wargc; ++i) args.push_back(pathToUtf8(fs::path(wargv[i])));
    if (wargv) LocalFree(wargv);
    SetConsoleOutputCP(CP_UTF8);
#else
    for (int i = 1; i < argc; ++i) args.emplace_back(argv[i]);
#endif

    try {
        ParsedArgs pa = parseArgs(args);
        setLogLevel(pa.logLevel);
        switch (pa.command) {
            case Command::Help:
                std::fputs(usageText().c_str(), stdout);
                return 0;
            case Command::Version:
                std::fputs(buildSummary().c_str(), stdout);
                return 0;
            case Command::Formats:
                std::fputs(formatsHelp().c_str(), stdout);
                return 0;
            case Command::Info:
                return runInfo(pa.files, pa.convert.reader);
            case Command::Convert:
                return runConvert(pa.convert);
        }
    } catch (const UsageError& e) {
        std::fprintf(stderr, "serconv: %s\nTry 'serconv --help'.\n", e.what());
        return 2;
    } catch (const Error& e) {
        finishProgress();
        std::fprintf(stderr, "serconv: error: %s\n", e.what());
        return 1;
    } catch (const std::bad_alloc&) {
        finishProgress();
        std::fprintf(stderr, "serconv: error: out of memory\n");
        return 1;
    } catch (const std::exception& e) {
        finishProgress();
        std::fprintf(stderr, "serconv: error: %s\n", e.what());
        return 1;
    }
    return 0;
}
