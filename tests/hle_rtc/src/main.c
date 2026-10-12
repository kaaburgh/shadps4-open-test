/*
 * hle_rtc: libSceRtc date-string parsing and the network-tick setters.
 *
 * Reference instant T0 = 2026-10-12 21:07:46 UTC, as sceRtcGetTick computes
 * it (one tick per microsecond).
 *
 * Cases:
 *   rfc3339         sceRtcParseRFC3339 on "Z", "+02:00" and "-05:30" forms of
 *                   T0, and on fractions ".5", ".50" and ".123456". An offset
 *                   is local time minus UTC (RFC 3339), so every form is T0.
 *   date_time       sceRtcParseDateTime on T0 as RFC 3339 with an offset,
 *                   RFC 2822 with an offset, and asctime, including a
 *                   space-padded day.
 *   round_trip      sceRtcFormatRFC3339 / sceRtcFormatRFC2822 of T0 at +120
 *                   and -330 minutes parse back to T0.
 *   malformed       wrong digits, bad offsets, unknown month names and
 *                   impossible dates fail and leave the output tick alone.
 *   short           strings shorter than their format (down to "") fail and
 *                   leave the output tick alone.
 *   network_tick    each sceRtcSetCurrent*NetworkTick accepts T0, rejects a
 *                   tick before 1970, and survives NULL (whose result is only
 *                   printed: the public evidence says NULL is forwarded to
 *                   clock_settime).
 *
 * Failing calls are only required to return a negative error; the codes are
 * printed for reference. A case that crashes the emulator ends the run
 * without a marker; the log's last "case <name>: start" line names it.
 */

#define SHADTEST_NAME "hle_rtc"
#include "shadtest_guest.h"

typedef struct {
    uint64_t tick;
} RtcTick;

typedef struct {
    uint16_t year;
    uint16_t month;
    uint16_t day;
    uint16_t hour;
    uint16_t minute;
    uint16_t second;
    uint32_t microsecond;
} RtcDateTime;

int32_t sceRtcGetTick(RtcDateTime* date_time, RtcTick* tick);
int32_t sceRtcParseRFC3339(RtcTick* tick_utc, const char* str);
int32_t sceRtcParseDateTime(RtcTick* tick_utc, const char* str);
int32_t sceRtcFormatRFC3339(char* out, const RtcTick* tick_utc, int tz_minutes);
int32_t sceRtcFormatRFC2822(char* out, const RtcTick* tick_utc, int tz_minutes);
int32_t sceRtcSetCurrentNetworkTick(RtcTick* tick);
int32_t sceRtcSetCurrentDebugNetworkTick(RtcTick* tick);
int32_t sceRtcSetCurrentAdNetworkTick(RtcTick* tick);

enum { SENTINEL = 0x5eed };

static void start(const StCase* c) { printf("case %s: start\n", c->name); }

static uint64_t tick_of(uint16_t year, uint16_t month, uint16_t day,
                        uint16_t hour, uint16_t minute, uint16_t second) {
    RtcDateTime dt = {year, month, day, hour, minute, second, 0};
    RtcTick tick = {0};
    if (sceRtcGetTick(&dt, &tick) < 0) {
        return 0;
    }
    return tick.tick;
}

typedef int32_t (*ParseFn)(RtcTick*, const char*);

/* Parses `str`; expects success and `want`. */
static void expect_parse(StCase* c, ParseFn parse, uint32_t index,
                         const char* str, uint64_t want) {
    RtcTick tick = {SENTINEL};
    const int32_t rc = parse(&tick, str);
    if (rc < 0) {
        printf("  \"%s\" returned %08x\n", str, (uint32_t)rc);
        st_case_note(c, "rc", index, 0, (uint32_t)rc);
        return;
    }
    if (tick.tick != want) {
        printf("  \"%s\" gave %llu, want %llu\n", str,
               (unsigned long long)tick.tick, (unsigned long long)want);
        /* The low word of the difference in microseconds. */
        st_case_note(c, "tick", index, 0, (uint32_t)(tick.tick - want));
    }
}

/* Parses `str`; expects an error and an untouched tick. */
static void expect_reject(StCase* c, ParseFn parse, uint32_t index,
                          const char* str) {
    RtcTick tick = {SENTINEL};
    const int32_t rc = parse(&tick, str);
    printf("  \"%s\" returned %08x\n", str, (uint32_t)rc);
    if (rc >= 0) {
        st_case_note(c, "accepted", index, 0x80000000u, (uint32_t)rc);
    }
    if (tick.tick != SENTINEL) {
        st_case_note(c, "tick_written", index, SENTINEL, (uint32_t)tick.tick);
    }
}

enum {
    C_RFC3339,
    C_DATE_TIME,
    C_ROUND_TRIP,
    C_MALFORMED,
    C_SHORT,
    C_NETWORK_TICK,
    C_COUNT,
};

int main(void) {
    st_begin();

    StCase cases[C_COUNT] = {
        [C_RFC3339] = {.name = "rfc3339"},
        [C_DATE_TIME] = {.name = "date_time"},
        [C_ROUND_TRIP] = {.name = "round_trip"},
        [C_MALFORMED] = {.name = "malformed"},
        [C_SHORT] = {.name = "short"},
        [C_NETWORK_TICK] = {.name = "network_tick"},
    };

    const uint64_t t0 = tick_of(2026, 10, 12, 21, 7, 46);
    if (t0 == 0) {
        return st_fail("reason=get_tick");
    }

    {
        StCase* c = &cases[C_RFC3339];
        start(c);
        expect_parse(c, sceRtcParseRFC3339, 0, "2026-10-12T21:07:46Z", t0);
        expect_parse(c, sceRtcParseRFC3339, 1, "2026-10-12T23:07:46+02:00", t0);
        expect_parse(c, sceRtcParseRFC3339, 2, "2026-10-12T15:37:46-05:30", t0);
        expect_parse(c, sceRtcParseRFC3339, 3, "2026-10-12T21:07:46.00Z", t0);
        expect_parse(c, sceRtcParseRFC3339, 4, "2026-10-12T21:07:46.5Z",
                     t0 + 500000);
        expect_parse(c, sceRtcParseRFC3339, 5, "2026-10-12T21:07:46.50Z",
                     t0 + 500000);
        expect_parse(c, sceRtcParseRFC3339, 6, "2026-10-12T21:07:46.123456Z",
                     t0 + 123456);
        expect_parse(c, sceRtcParseRFC3339, 7,
                     "2026-10-12T23:07:46.25+02:00", t0 + 250000);
    }
    {
        StCase* c = &cases[C_DATE_TIME];
        start(c);
        expect_parse(c, sceRtcParseDateTime, 0, "2026-10-12T21:07:46Z", t0);
        expect_parse(c, sceRtcParseDateTime, 1, "2026-10-12T23:07:46+02:00",
                     t0);
        expect_parse(c, sceRtcParseDateTime, 2,
                     "Mon, 12 Oct 2026 21:07:46 +0000", t0);
        expect_parse(c, sceRtcParseDateTime, 3,
                     "Mon, 12 Oct 2026 23:07:46 +0200", t0);
        expect_parse(c, sceRtcParseDateTime, 4,
                     "Mon, 12 Oct 2026 15:37:46 -0530", t0);
        expect_parse(c, sceRtcParseDateTime, 5, "Mon Oct 12 21:07:46 2026", t0);
        expect_parse(c, sceRtcParseDateTime, 6, "Fri Oct  2 21:07:46 2026",
                     tick_of(2026, 10, 2, 21, 7, 46));
    }
    {
        StCase* c = &cases[C_ROUND_TRIP];
        start(c);
        const RtcTick tick = {t0};
        const int zones[] = {0, 120, -330};
        char buf[64];
        for (unsigned k = 0; k < 3; ++k) {
            memset(buf, 0, sizeof(buf));
            int32_t rc = sceRtcFormatRFC3339(buf, &tick, zones[k]);
            printf("  RFC 3339 at %d: %s\n", zones[k], buf);
            if (rc < 0) {
                st_case_note(c, "format_rfc3339", k, 0, (uint32_t)rc);
            } else {
                expect_parse(c, sceRtcParseRFC3339, k, buf, t0);
            }
            memset(buf, 0, sizeof(buf));
            rc = sceRtcFormatRFC2822(buf, &tick, zones[k]);
            printf("  RFC 2822 at %d: %s\n", zones[k], buf);
            if (rc < 0) {
                st_case_note(c, "format_rfc2822", k, 0, (uint32_t)rc);
            } else {
                expect_parse(c, sceRtcParseDateTime, 10 + k, buf, t0);
            }
        }
    }
    {
        /* Strings of full length: whatever the parser checks, it must not
         * accept these. */
        StCase* c = &cases[C_MALFORMED];
        start(c);
        static const char* const rfc3339[] = {
            "2026-13-12T21:07:46Z",      /* month 13 */
            "2026-02-30T21:07:46Z",      /* February 30 */
            "2026-10-12T25:07:46Z",      /* hour 25 */
            "abcd-10-12T21:07:46Z",      /* year not digits */
            "2026-1x-12T21:07:46Z",      /* month not digits */
            "2026-10-12T21:07:46+xx:00", /* offset not digits */
            "2026-10-12T21:07:46+02:",   /* offset cut */
            "2026-10-12T21:07:46.Z",     /* empty fraction */
        };
        for (unsigned k = 0; k < sizeof(rfc3339) / sizeof(rfc3339[0]); ++k) {
            expect_reject(c, sceRtcParseRFC3339, k, rfc3339[k]);
        }
        static const char* const date_time[] = {
            "Mon, 12 Xyz 2026 21:07:46 +0000", /* unknown month */
            "Mon, 1x Oct 2026 21:07:46 +0000", /* day not digits */
            "Mon, 12 Oct 2026 21:07:46 +02x0", /* offset not digits */
            "Mon Xyz 12 21:07:46 2026",        /* unknown month */
            "Mon Oct 12 21:07:46 20x6",        /* year not digits */
            "2026-13-12T21:07:46Z",            /* RFC 3339, month 13 */
        };
        for (unsigned k = 0; k < sizeof(date_time) / sizeof(date_time[0]);
             ++k) {
            expect_reject(c, sceRtcParseDateTime, 100 + k, date_time[k]);
        }
    }
    {
        StCase* c = &cases[C_SHORT];
        start(c);
        static const char* const strs[] = {
            "", "2", "2026-10-12", "2026-10-12T21:07", "2026-10-12T21:07:4",
            "Mon, 12 Oct 2026 21:07:46 +02", "Mon Oct 12 21:07:46",
        };
        for (unsigned k = 0; k < sizeof(strs) / sizeof(strs[0]); ++k) {
            expect_reject(c, sceRtcParseRFC3339, k, strs[k]);
            expect_reject(c, sceRtcParseDateTime, 100 + k, strs[k]);
        }
    }
    {
        StCase* c = &cases[C_NETWORK_TICK];
        start(c);
        int32_t (*const setters[])(RtcTick*) = {
            sceRtcSetCurrentNetworkTick,
            sceRtcSetCurrentDebugNetworkTick,
            sceRtcSetCurrentAdNetworkTick,
        };
        for (unsigned k = 0; k < 3; ++k) {
            RtcTick tick = {t0};
            int32_t rc = setters[k](&tick);
            printf("  setter %u: T0 returned %08x\n", k, (uint32_t)rc);
            if (rc < 0) {
                st_case_note(c, "t0", k, 0, (uint32_t)rc);
            }
            tick.tick = 1;
            rc = setters[k](&tick);
            printf("  setter %u: tick 1 returned %08x\n", k, (uint32_t)rc);
            if (rc >= 0) {
                st_case_note(c, "before_1970", k, 0x80000000u, (uint32_t)rc);
            }
            rc = setters[k](NULL);
            printf("  setter %u: NULL returned %08x\n", k, (uint32_t)rc);
        }
    }

    return st_finish_cases(cases, C_COUNT, "");
}
