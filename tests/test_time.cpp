// Time conversions: SER ticks, ISO 8601 text, clock offsets.
#include "test_util.hpp"
#include "timeutil.hpp"

using namespace serconv;

int main() {
    // The .NET epoch offset used by SER.
    CivilTime unix{1970, 1, 1, 0, 0, 0, 0};
    CHECK_EQ(ticksFromCivil(unix), kUnixEpochTicks);
    CivilTime year1{1, 1, 1, 0, 0, 0, 0};
    CHECK_EQ(ticksFromCivil(year1), int64_t(0));

    // Round trips across leap years and centuries.
    for (int y : {1, 4, 100, 400, 1900, 1904, 1999, 2000, 2024, 2026, 2100, 9999}) {
        for (int mo : {1, 2, 3, 12}) {
            CivilTime t{y, mo, mo == 2 ? 28 : 31, 23, 59, 58, 1234567};
            if (mo == 12) t.day = 31;
            CivilTime back = civilFromTicks(ticksFromCivil(t));
            CHECK_EQ(back.year, t.year);
            CHECK_EQ(back.month, t.month);
            CHECK_EQ(back.day, t.day);
            CHECK_EQ(back.hour, t.hour);
            CHECK_EQ(back.minute, t.minute);
            CHECK_EQ(back.second, t.second);
            CHECK_EQ(back.subTicks, t.subTicks);
        }
    }
    CivilTime leap{2024, 2, 29, 12, 0, 0, 0};
    CHECK(validCivil(leap));
    CivilTime notLeap{2026, 2, 29, 12, 0, 0, 0};
    CHECK(!validCivil(notLeap));

    // A known value: 2026-10-23T21:14:05Z is Unix time 1792790045.
    CivilTime t{2026, 10, 23, 21, 14, 5, 0};
    CHECK_EQ(ticksFromCivil(t), kUnixEpochTicks + int64_t(1792790045) * kTicksPerSecond);

    // Formatting, with rounding into the next second.
    CHECK_EQ(formatTicks(ticksFromCivil(t) + 2500000, true, 3), std::string("2026-10-23T21:14:05.250Z"));
    CHECK_EQ(formatTicks(ticksFromCivil(t) + 9999999, false, 3), std::string("2026-10-23T21:14:06.000"));
    CHECK_EQ(formatTicks(ticksFromCivil(t), true, 0), std::string("2026-10-23T21:14:05Z"));

    // Parsing.
    int64_t ticks, zone;
    bool hasZone;
    CHECK(parseDateTime("2026-10-23T21:14:05.25Z", ticks, hasZone, zone));
    CHECK(hasZone);
    CHECK_EQ(zone, int64_t(0));
    CHECK_EQ(ticks, ticksFromCivil(t) + 2500000);
    CHECK(parseDateTime("2026-10-23 23:14:05+02:00", ticks, hasZone, zone));
    CHECK(hasZone);
    CHECK_EQ(ticks - zone, ticksFromCivil(t));
    CHECK(parseDateTime("2026:10:23 21:14:05", ticks, hasZone, zone));  // EXIF
    CHECK(!hasZone);
    CHECK_EQ(ticks, ticksFromCivil(t));
    CHECK(parseDateTime("2026-10-23T21:14", ticks, hasZone, zone));
    CHECK(parseDateTime("2026-10-23", ticks, hasZone, zone));
    CHECK(parseDateTime("2024-01-01T00:00:00.000000Z", ticks, hasZone, zone));  // FFmpeg creation_time
    CHECK(!parseDateTime("2026-13-01T00:00:00", ticks, hasZone, zone));
    CHECK(!parseDateTime("2026-10-23T25:00:00", ticks, hasZone, zone));
    CHECK(!parseDateTime("yesterday", ticks, hasZone, zone));
    CHECK(!parseDateTime("2026-10-23T21:14:05 junk", ticks, hasZone, zone));

    // Offsets.
    int64_t off;
    CHECK(parseOffset("+02:00", off));
    CHECK_EQ(off, 2 * 3600 * kTicksPerSecond);
    CHECK(parseOffset("-5", off));
    CHECK_EQ(off, -5 * 3600 * kTicksPerSecond);
    CHECK(parseOffset("+0530", off));
    CHECK_EQ(off, (5 * 3600 + 30 * 60) * kTicksPerSecond);
    CHECK(parseOffset("-01:00:02.5", off));
    CHECK_EQ(off, -(3602 * kTicksPerSecond + 5000000));
    CHECK(parseOffset("2", off));
    CHECK_EQ(off, 2 * 3600 * kTicksPerSecond);
    CHECK(!parseOffset("+25:00", off));
    CHECK(!parseOffset("abc", off));
    CHECK(!parseOffset("", off));
    CHECK_EQ(formatOffset(2 * 3600 * kTicksPerSecond), std::string("+02:00"));
    CHECK_EQ(formatOffset(-(3602 * kTicksPerSecond + 5000000)), std::string("-01:00:02.5"));

    // QuickTime 1904 epoch used by MP4/CR3 headers.
    CHECK_EQ(formatTicks(ticksFromCivil({1904, 1, 1, 0, 0, 0, 0}) + int64_t(2082844800) * kTicksPerSecond, true, 0),
             std::string("1970-01-01T00:00:00Z"));

    CHECK_EQ(formatDuration(20.04), std::string("20.0 s"));
    CHECK_EQ(formatDuration(192.4), std::string("3 min 12.4 s"));

    return testutil::finish("test_time");
}
