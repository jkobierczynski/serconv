// Command-line parsing.
#pragma once

#include <string>
#include <vector>

#include "convert.hpp"

namespace serconv {

struct UsageError : Error {
    using Error::Error;
};

enum class Command { Convert, Info, Formats, Help, Version };

struct ParsedArgs {
    Command command = Command::Convert;
    ConvertOptions convert;
    std::vector<std::string> files;  // for info
    LogLevel logLevel = LogLevel::Normal;
};

// args excludes the program name. Throws UsageError.
ParsedArgs parseArgs(const std::vector<std::string>& args);
std::string usageText();

}  // namespace serconv
