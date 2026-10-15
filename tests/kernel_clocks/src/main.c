/*
 * kernel_clocks: sceKernelClockGettime / clock_gettime and
 * sceKernelClockGetres / clock_getres for every PS4 clock id.
 *
 * Clock ids are FreeBSD's, plus PROCTIME (15) and the four network clocks
 * (16-19) the PS4 adds.
 *
 * Cases:
 *   gettime   for each clock and both APIs: two reads 20 ms apart succeed,
 *             tv_nsec is in [0, 1e9), the clock does not go back, wall and
 *             network clocks read a date between 2020 and 2100, and
 *             non-CPU clocks advance by 10 ms .. 5 s across the sleep
 *             (CLOCK_SECOND by 0 .. 5 s, as it may count whole seconds).
 *   errors    a NULL timespec and unknown clock ids (3, 6, 20, 0x7fffffff)
 *             fail: sceKernelClockGettime with a negative code, clock_gettime
 *             with -1. The codes are printed, not checked.
 *   getres    for each clock and both APIs: succeeds with a resolution in
 *             (0, 1 s]. Unknown ids must fail. Last, as a crash ends the
 *             run; each clock's result is printed as it is read.
 *
 * CLOCK_SECOND's tv_nsec is printed: FreeBSD returns whole seconds there.
 */

#define SHADTEST_NAME "kernel_clocks"
#include "shadtest_guest.h"

typedef struct {
    int64_t sec;
    int64_t nsec;
} Ts;

int32_t sce_gettime(int32_t id, Ts* ts) __asm__("sceKernelClockGettime");
int32_t sce_getres(int32_t id, Ts* ts) __asm__("sceKernelClockGetres");
int posix_gettime(int32_t id, Ts* ts) __asm__("clock_gettime");
int posix_getres(int32_t id, Ts* ts) __asm__("clock_getres");
int* errno_ptr(void) __asm__("__error");

enum { CPU = 1, WALL = 2 };

static const struct {
    int32_t id;
    const char* name;
    int kind;
} CLOCKS[] = {
    {0, "REALTIME", WALL},
    {1, "VIRTUAL", CPU},
    {2, "PROF", CPU},
    {4, "MONOTONIC", 0},
    {5, "UPTIME", 0},
    {7, "UPTIME_PRECISE", 0},
    {8, "UPTIME_FAST", 0},
    {9, "REALTIME_PRECISE", WALL},
    {10, "REALTIME_FAST", WALL},
    {11, "MONOTONIC_PRECISE", 0},
    {12, "MONOTONIC_FAST", 0},
    {13, "SECOND", WALL},
    {14, "THREAD_CPUTIME_ID", CPU},
    {15, "PROCTIME", 0},
    {16, "EXT_NETWORK", WALL},
    {17, "EXT_DEBUG_NETWORK", WALL},
    {18, "EXT_AD_NETWORK", WALL},
    {19, "EXT_RAW_NETWORK", WALL},
};
enum { NUM_CLOCKS = sizeof(CLOCKS) / sizeof(CLOCKS[0]) };

static const int32_t BAD_IDS[] = {3, 6, 20, 0x7fffffff};

#define NS_PER_S 1000000000LL
#define YEAR_2020 1577836800LL
#define YEAR_2100 4102444800LL

static int64_t ns(Ts t) { return t.sec * NS_PER_S + t.nsec; }

static int32_t read_clock(int api, int32_t id, Ts* t) {
    if (api == 0) {
        return sce_gettime(id, t);
    }
    *errno_ptr() = 0;
    const int rc = posix_gettime(id, t);
    return rc == 0 ? 0 : -*errno_ptr() - 1;
}

static void check_sample(StCase* c, uint32_t index, const char* what, Ts t,
                         int kind) {
    if (t.nsec < 0 || t.nsec >= NS_PER_S || t.sec < 0) {
        printf("  %s: bad timespec %lld.%09lld\n", what, (long long)t.sec,
               (long long)t.nsec);
        st_case_note(c, "timespec", index, 0, (uint32_t)t.nsec);
    }
    if (kind == WALL && (t.sec < YEAR_2020 || t.sec > YEAR_2100)) {
        printf("  %s: date %lld out of range\n", what, (long long)t.sec);
        st_case_note(c, "date", index, 0, (uint32_t)t.sec);
    }
}

static void end(const StCase* c) {
    printf("case %s: %s\n", c->name, c->reason ? c->reason : "ok");
}

enum { C_GETTIME, C_ERRORS, C_GETRES, C_COUNT };

int main(void) {
    st_begin();

    StCase cases[C_COUNT] = {
        [C_GETTIME] = {.name = "gettime"},
        [C_ERRORS] = {.name = "errors"},
        [C_GETRES] = {.name = "getres"},
    };
    static const char* const API[] = {"sce", "posix"};

    {
        StCase* c = &cases[C_GETTIME];
        printf("case %s: start\n", c->name);
        Ts first[2][NUM_CLOCKS];
        int32_t rc1[2][NUM_CLOCKS];
        for (int api = 0; api < 2; ++api) {
            for (uint32_t k = 0; k < NUM_CLOCKS; ++k) {
                rc1[api][k] = read_clock(api, CLOCKS[k].id, &first[api][k]);
            }
        }
        sceKernelUsleep(20000);
        for (int api = 0; api < 2; ++api) {
            for (uint32_t k = 0; k < NUM_CLOCKS; ++k) {
                Ts t2 = {-1, -1};
                const int32_t rc2 = read_clock(api, CLOCKS[k].id, &t2);
                const Ts t1 = first[api][k];
                const int64_t delta = ns(t2) - ns(t1);
                printf("  %s %-17s rc=%08x/%08x %lld.%09lld delta=%lld ns\n",
                       API[api], CLOCKS[k].name, (uint32_t)rc1[api][k],
                       (uint32_t)rc2, (long long)t2.sec, (long long)t2.nsec,
                       (long long)delta);
                const uint32_t index = (uint32_t)(api << 8 | CLOCKS[k].id);
                /* A whole-second clock need not move within 20 ms. */
                const int64_t min_advance = CLOCKS[k].id == 13 ? 0 : 10000000;
                if (rc1[api][k] != 0 || rc2 != 0) {
                    st_case_note(c, "gettime_rc", index, 0,
                                 (uint32_t)(rc1[api][k] ? rc1[api][k] : rc2));
                    continue;
                }
                check_sample(c, index, CLOCKS[k].name, t1, CLOCKS[k].kind);
                check_sample(c, index, CLOCKS[k].name, t2, CLOCKS[k].kind);
                if (delta < 0) {
                    st_case_note(c, "went_back", index, 0, (uint32_t)-delta);
                } else if (CLOCKS[k].kind != CPU &&
                           (delta < min_advance || delta > 5 * NS_PER_S)) {
                    st_case_note(c, "advance", index, 20000000,
                                 (uint32_t)(delta / 1000));
                }
                if (CLOCKS[k].id == 13) {
                    printf("  %s SECOND tv_nsec=%lld\n", API[api],
                           (long long)t2.nsec);
                }
            }
        }
        end(c);
    }
    {
        StCase* c = &cases[C_ERRORS];
        printf("case %s: start\n", c->name);
        for (int api = 0; api < 2; ++api) {
            const int32_t rc = read_clock(api, 0, NULL);
            printf("  %s NULL timespec: %08x\n", API[api], (uint32_t)rc);
            if (rc >= 0) {
                st_case_note(c, "null_ok", (uint32_t)api, 1, (uint32_t)rc);
            }
            for (uint32_t k = 0; k < sizeof(BAD_IDS) / sizeof(BAD_IDS[0]);
                 ++k) {
                Ts t = {0, 0};
                const int32_t r = read_clock(api, BAD_IDS[k], &t);
                printf("  %s clock %d: %08x\n", API[api], BAD_IDS[k],
                       (uint32_t)r);
                if (r >= 0) {
                    st_case_note(c, "bad_id_ok", (uint32_t)BAD_IDS[k], 1,
                                 (uint32_t)r);
                }
            }
        }
        end(c);
    }
    {
        StCase* c = &cases[C_GETRES];
        printf("case %s: start\n", c->name);
        for (int api = 0; api < 2; ++api) {
            for (uint32_t k = 0; k < NUM_CLOCKS; ++k) {
                Ts r = {-1, -1};
                printf("  %s getres %-17s ", API[api], CLOCKS[k].name);
                fflush(stdout);
                int32_t rc;
                if (api == 0) {
                    rc = sce_getres(CLOCKS[k].id, &r);
                } else {
                    *errno_ptr() = 0;
                    rc = posix_getres(CLOCKS[k].id, &r) == 0
                             ? 0
                             : -*errno_ptr() - 1;
                }
                printf("rc=%08x res=%lld.%09lld\n", (uint32_t)rc,
                       (long long)r.sec, (long long)r.nsec);
                const uint32_t index = (uint32_t)(api << 8 | CLOCKS[k].id);
                if (rc != 0) {
                    st_case_note(c, "getres_rc", index, 0, (uint32_t)rc);
                } else if (ns(r) <= 0 || ns(r) > NS_PER_S) {
                    st_case_note(c, "getres_range", index, 0,
                                 (uint32_t)ns(r));
                }
            }
            for (uint32_t k = 0; k < sizeof(BAD_IDS) / sizeof(BAD_IDS[0]);
                 ++k) {
                Ts r = {0, 0};
                printf("  %s getres clock %d: ", API[api], BAD_IDS[k]);
                fflush(stdout);
                const int32_t rc = api == 0 ? sce_getres(BAD_IDS[k], &r)
                                            : posix_getres(BAD_IDS[k], &r);
                printf("%08x\n", (uint32_t)rc);
                if (rc >= 0) {
                    st_case_note(c, "getres_bad_id_ok", (uint32_t)BAD_IDS[k],
                                 1, (uint32_t)rc);
                }
            }
        }
        end(c);
    }

    return st_finish_cases(cases, C_COUNT, "");
}
