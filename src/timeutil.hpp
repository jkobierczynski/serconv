// Time handling in SER units: "ticks" of 100 ns since 0001-01-01T00:00:00,
// the .NET DateTime representation the SER header and trailer use.
#pragma once

#include <cstdint>
#include <string>

namespace serconv {

constexpr int64_t kTicksPerSecond = 10000000LL;
constexpr int64_t kTicksPerDay = 864000000000LL;
// Ticks from 0001-01-01T00:00:00 to 1970-01-01T00:00:00.
constexpr int64_t kUnixEpochTicks = 621355968000000000LL;

// Proleptic Gregorian calendar; days are counted from 1970-01-01.
int64_t daysFromCivil(int64_t year, int month, int day);
void civilFromDays(int64_t days, int64_t& year, int& month, int& day);

struct CivilTime {
    int year = 0, month = 0, day = 0;  // month and day are 1-based
    int hour = 0, minute = 0, second = 0;
    int64_t subTicks = 0;  // 0 .. kTicksPerSecond-1
};

bool validCivil(const CivilTime& t);
int64_t ticksFromCivil(const CivilTime& t);
CivilTime civilFromTicks(int64_t ticks);
int64_t ticksFromSeconds(double seconds);  // rounded
double secondsFromTicks(int64_t ticks);
int64_t ticksFromUnixSeconds(double seconds);

// ISO 8601 text, e.g. "2026-10-23T21:14:05.250Z" (the Z only when utc is true).
std::string formatTicks(int64_t ticks, bool utc, int fractionDigits = 3);

// Parses "YYYY-MM-DD[T| ]HH:MM[:SS[.fraction]]" with an optional "Z" or
// "+HH[:MM]" zone, and the EXIF form "YYYY:MM:DD HH:MM:SS". When a zone is
// present, hasZone is set and zoneTicks holds its offset from UTC; ticks are
// the wall-clock reading (UTC = ticks - zoneTicks).
bool parseDateTime(const std::string& text, int64_t& ticks, bool& hasZone, int64_t& zoneTicks);

// Clock offsets such as "+02:00", "-5", "+0530", "+01:00:02.5".
bool parseOffset(const std::string& text, int64_t& ticks);
std::string formatOffset(int64_t ticks);

std::string formatDuration(double seconds);

}  // namespace serconv
