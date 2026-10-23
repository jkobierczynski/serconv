#include "timeutil.hpp"

#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>

#include "util.hpp"

namespace serconv {

int64_t daysFromCivil(int64_t y, int m, int d) {
    // Howard Hinnant's days_from_civil.
    y -= m <= 2 ? 1 : 0;
    const int64_t era = (y >= 0 ? y : y - 399) / 400;
    const int64_t yoe = y - era * 400;
    const int64_t doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    const int64_t doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + doe - 719468;
}

void civilFromDays(int64_t z, int64_t& y, int& m, int& d) {
    z += 719468;
    const int64_t era = (z >= 0 ? z : z - 146096) / 146097;
    const int64_t doe = z - era * 146097;
    const int64_t yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    y = yoe + era * 400;
    const int64_t doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    const int64_t mp = (5 * doy + 2) / 153;
    d = int(doy - (153 * mp + 2) / 5 + 1);
    m = int(mp < 10 ? mp + 3 : mp - 9);
    if (m <= 2) y += 1;
}

static bool isLeap(int64_t y) { return (y % 4 == 0 && y % 100 != 0) || y % 400 == 0; }

static int daysInMonth(int64_t y, int m) {
    static const int dim[12] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    if (m == 2 && isLeap(y)) return 29;
    return (m >= 1 && m <= 12) ? dim[m - 1] : 0;
}

bool validCivil(const CivilTime& t) {
    return t.year >= 1 && t.year <= 9999 && t.month >= 1 && t.month <= 12 && t.day >= 1 &&
           t.day <= daysInMonth(t.year, t.month) && t.hour >= 0 && t.hour <= 23 && t.minute >= 0 &&
           t.minute <= 59 && t.second >= 0 && t.second <= 60 && t.subTicks >= 0 &&
           t.subTicks < kTicksPerSecond;
}

int64_t ticksFromCivil(const CivilTime& t) {
    int64_t days = daysFromCivil(t.year, t.month, t.day);
    int64_t secs = int64_t(t.hour) * 3600 + int64_t(t.minute) * 60 + t.second;
    return kUnixEpochTicks + days * kTicksPerDay + secs * kTicksPerSecond + t.subTicks;
}

CivilTime civilFromTicks(int64_t ticks) {
    int64_t rel = ticks - kUnixEpochTicks;
    int64_t days = rel / kTicksPerDay;
    int64_t rem = rel % kTicksPerDay;
    if (rem < 0) {
        rem += kTicksPerDay;
        days -= 1;
    }
    CivilTime t;
    int64_t y;
    civilFromDays(days, y, t.month, t.day);
    t.year = int(y);
    int64_t secs = rem / kTicksPerSecond;
    t.subTicks = rem % kTicksPerSecond;
    t.hour = int(secs / 3600);
    t.minute = int((secs / 60) % 60);
    t.second = int(secs % 60);
    return t;
}

int64_t ticksFromSeconds(double seconds) { return int64_t(std::llround(seconds * double(kTicksPerSecond))); }

double secondsFromTicks(int64_t ticks) { return double(ticks) / double(kTicksPerSecond); }

int64_t ticksFromUnixSeconds(double seconds) { return kUnixEpochTicks + ticksFromSeconds(seconds); }

std::string formatTicks(int64_t ticks, bool utc, int fractionDigits) {
    if (fractionDigits < 0) fractionDigits = 0;
    if (fractionDigits > 7) fractionDigits = 7;
    // Round to the requested precision first so 59.9996 does not print as 59.1000.
    int64_t unit = 1;
    for (int i = fractionDigits; i < 7; ++i) unit *= 10;
    int64_t rounded = ((ticks + unit / 2) / unit) * unit;
    CivilTime t = civilFromTicks(rounded);
    std::string s = strprintf("%04d-%02d-%02dT%02d:%02d:%02d", t.year, t.month, t.day, t.hour, t.minute, t.second);
    if (fractionDigits > 0) {
        int64_t frac = t.subTicks / unit;
        s += strprintf(".%0*lld", fractionDigits, static_cast<long long>(frac));
    }
    if (utc) s += "Z";
    return s;
}

namespace {

// Reads exactly n digits.
bool readDigits(const std::string& s, size_t& i, int n, int& out) {
    if (i + size_t(n) > s.size()) return false;
    int v = 0;
    for (int k = 0; k < n; ++k) {
        char c = s[i + size_t(k)];
        if (!std::isdigit(static_cast<unsigned char>(c))) return false;
        v = v * 10 + (c - '0');
    }
    i += size_t(n);
    out = v;
    return true;
}

// Reads a fraction after '.', returns ticks.
int64_t readFraction(const std::string& s, size_t& i) {
    int64_t ticks = 0;
    int64_t scale = kTicksPerSecond / 10;
    while (i < s.size() && std::isdigit(static_cast<unsigned char>(s[i]))) {
        if (scale > 0) ticks += int64_t(s[i] - '0') * scale;
        scale /= 10;
        ++i;
    }
    return ticks;
}

}  // namespace

bool parseOffset(const std::string& textIn, int64_t& ticks) {
    std::string text = trim(textIn);
    if (text.empty()) return false;
    size_t i = 0;
    int sign = 1;
    if (text[i] == '+' || text[i] == '-') {
        sign = text[i] == '-' ? -1 : 1;
        ++i;
    }
    if (i >= text.size()) return false;
    // Forms: H, HH, HHMM, HH:MM, HH:MM:SS(.f)
    std::string rest = text.substr(i);
    int h = 0, m = 0, sec = 0;
    int64_t frac = 0;
    size_t j = 0;
    if (rest.find(':') == std::string::npos) {
        for (char c : rest)
            if (!std::isdigit(static_cast<unsigned char>(c))) return false;
        if (rest.size() <= 2) {
            h = std::atoi(rest.c_str());
        } else if (rest.size() == 4) {
            h = std::atoi(rest.substr(0, 2).c_str());
            m = std::atoi(rest.substr(2, 2).c_str());
        } else {
            return false;
        }
    } else {
        // HH:MM[:SS[.f]]
        size_t start = 0;
        while (j < rest.size() && std::isdigit(static_cast<unsigned char>(rest[j]))) ++j;
        if (j == start || j > 2) return false;
        h = std::atoi(rest.substr(start, j - start).c_str());
        if (j >= rest.size() || rest[j] != ':') return false;
        ++j;
        if (!readDigits(rest, j, 2, m)) return false;
        if (j < rest.size()) {
            if (rest[j] != ':') return false;
            ++j;
            if (!readDigits(rest, j, 2, sec)) return false;
            if (j < rest.size()) {
                if (rest[j] != '.' && rest[j] != ',') return false;
                ++j;
                frac = readFraction(rest, j);
            }
            if (j != rest.size()) return false;
        }
    }
    if (h > 23 || m > 59 || sec > 59) return false;
    ticks = sign * ((int64_t(h) * 3600 + int64_t(m) * 60 + sec) * kTicksPerSecond + frac);
    return true;
}

std::string formatOffset(int64_t ticks) {
    char sign = ticks < 0 ? '-' : '+';
    int64_t a = ticks < 0 ? -ticks : ticks;
    int64_t secs = a / kTicksPerSecond;
    int64_t frac = a % kTicksPerSecond;
    int h = int(secs / 3600), m = int((secs / 60) % 60), s = int(secs % 60);
    std::string out = strprintf("%c%02d:%02d", sign, h, m);
    if (s != 0 || frac != 0) {
        out += strprintf(":%02d", s);
        if (frac != 0) {
            std::string f = strprintf("%07lld", static_cast<long long>(frac));
            while (!f.empty() && f.back() == '0') f.pop_back();
            out += "." + f;
        }
    }
    return out;
}

bool parseDateTime(const std::string& textIn, int64_t& ticks, bool& hasZone, int64_t& zoneTicks) {
    std::string s = trim(textIn);
    hasZone = false;
    zoneTicks = 0;
    CivilTime t;
    size_t i = 0;
    if (!readDigits(s, i, 4, t.year)) return false;
    if (i >= s.size() || (s[i] != '-' && s[i] != ':')) return false;
    char dsep = s[i++];
    if (!readDigits(s, i, 2, t.month)) return false;
    if (i >= s.size() || s[i] != dsep) return false;
    ++i;
    if (!readDigits(s, i, 2, t.day)) return false;
    if (i < s.size()) {
        if (s[i] != 'T' && s[i] != 't' && s[i] != ' ') return false;
        ++i;
        if (!readDigits(s, i, 2, t.hour)) return false;
        if (i >= s.size() || s[i] != ':') return false;
        ++i;
        if (!readDigits(s, i, 2, t.minute)) return false;
        if (i < s.size() && s[i] == ':') {
            ++i;
            if (!readDigits(s, i, 2, t.second)) return false;
            if (i < s.size() && (s[i] == '.' || s[i] == ',')) {
                ++i;
                t.subTicks = readFraction(s, i);
            }
        }
        if (i < s.size()) {
            if (s[i] == 'Z' || s[i] == 'z') {
                hasZone = true;
                ++i;
            } else if (s[i] == '+' || s[i] == '-') {
                if (!parseOffset(s.substr(i), zoneTicks)) return false;
                hasZone = true;
                i = s.size();
            }
        }
        if (i != s.size()) return false;
    }
    if (!validCivil(t)) return false;
    ticks = ticksFromCivil(t);
    return true;
}

std::string formatDuration(double seconds) {
    if (!(seconds >= 0)) return "?";
    if (seconds < 60) return strprintf("%.1f s", seconds);
    int minutes = int(seconds / 60);
    double rest = seconds - minutes * 60.0;
    if (minutes < 60) return strprintf("%d min %.1f s", minutes, rest);
    return strprintf("%d h %d min %.0f s", minutes / 60, minutes % 60, rest);
}

}  // namespace serconv
