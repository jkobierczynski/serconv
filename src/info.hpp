#pragma once

#include <string>
#include <vector>

#include "reader.hpp"

namespace serconv {

// `serconv info`: describes SER files and raw video inputs.
int runInfo(const std::vector<std::string>& args, const ReaderOptions& options);

}  // namespace serconv
