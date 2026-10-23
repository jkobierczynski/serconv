#include "sidecar.hpp"

#include <algorithm>
#include <system_error>

namespace serconv {

void KeyValueText::section(const std::string& name) { sections_.push_back({name, {}}); }

void KeyValueText::add(const std::string& key, const std::string& value) {
    if (value.empty()) return;
    if (sections_.empty()) section("serconv");
    std::string v = value;
    for (char& c : v)
        if (c == '\n' || c == '\r') c = ' ';
    sections_.back().second.emplace_back(key, v);
}

std::string KeyValueText::text() const {
    std::string out;
    size_t width = 0;
    for (const auto& s : sections_)
        for (const auto& kv : s.second) width = std::max(width, kv.first.size());
    for (const auto& s : sections_) {
        if (s.second.empty()) continue;
        if (!out.empty()) out += "\n";
        out += "[" + s.first + "]\n";
        for (const auto& kv : s.second) {
            out += kv.first;
            out.append(width - kv.first.size(), ' ');
            out += " = " + kv.second + "\n";
        }
    }
    return out;
}

fs::path sidecarPathFor(const fs::path& serPath) {
    fs::path p = serPath;
    p.replace_extension();
    p += ".serconv.txt";
    return p;
}

void writeTextFile(const fs::path& path, const std::string& text) {
    File f(path, "wb");
    f.write(text.data(), text.size());
    f.flush();
    f.closeChecked();
}

}  // namespace serconv
