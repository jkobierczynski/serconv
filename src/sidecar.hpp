// The "<name>.serconv.txt" file written next to each SER: what SER's header
// cannot hold (camera, exposure, levels, time source), as "key = value" lines
// grouped in [sections].
#pragma once

#include <string>
#include <utility>
#include <vector>

#include "util.hpp"

namespace serconv {

class KeyValueText {
public:
    void section(const std::string& name);
    void add(const std::string& key, const std::string& value);
    std::string text() const;

private:
    std::vector<std::pair<std::string, std::vector<std::pair<std::string, std::string>>>> sections_;
};

fs::path sidecarPathFor(const fs::path& serPath);
void writeTextFile(const fs::path& path, const std::string& text);

}  // namespace serconv
