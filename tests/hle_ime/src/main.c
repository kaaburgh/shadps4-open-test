/*
 * hle_ime: libSceIme keeps non-Latin text intact from the guest's buffer to
 * the IME and back.
 *
 * Each case opens the IME panel (sceImeOpen), feeds it a UTF-16 string of
 * Cyrillic, CJK and accented Latin characters (all in the Basic Multilingual
 * Plane), and waits until the panel is closed with the PS4 Circle button.
 * shadPS4 then writes the panel's text back to the guest's inputTextBuffer and
 * passes it with the PressClose event.
 *
 * The panel has to be closed from outside: tools/press_keys.py presses "b"
 * (Circle in shadPS4's default keyboard mapping) in the test's X display every
 * two seconds. Run it next to the test:
 *
 *   XVFB_HOOK="python3 tests/hle_ime/tools/press_keys.py" \
 *       bash scripts/run-test-lavapipe.sh hle_ime --timeout 120
 *
 * Without the key presses every case times out and the test fails.
 *
 * Cases:
 *   open_text   the string is the initial inputTextBuffer content.
 *   set_text    the buffer starts empty; sceImeSetText supplies the string
 *               and sceImeSetCaret moves the caret inside it.
 *
 * Each case checks the inputTextBuffer and the PressClose event's text
 * against the string, unit by unit. A string with a surrogate pair is only
 * offered to sceImeSetText and its result printed: shadPS4 rejects code
 * units D800-DFFF in IME text, and whether the PS4 does is not established.
 */

#define SHADTEST_NAME "hle_ime"
#include "shadtest_guest.h"

#include <uchar.h>

typedef struct {
    uint32_t id;
    uint32_t pad;
    union {
        struct {
            char16_t* str;
            uint32_t caret_index;
            uint32_t area_num;
            uint8_t areas[48];
        } text;
        uint8_t reserved[64];
    } param;
} ImeEvent;

typedef void (*ImeEventHandler)(void* arg, const ImeEvent* event);

typedef struct {
    int32_t user_id;
    uint32_t type;
    uint64_t supported_languages;
    uint32_t enter_label;
    uint32_t input_method;
    void* filter;
    uint32_t option;
    uint32_t max_text_length;
    char16_t* input_text_buffer;
    float posx;
    float posy;
    uint32_t horizontal_alignment;
    uint32_t vertical_alignment;
    void* work;
    void* arg;
    ImeEventHandler handler;
    int8_t reserved[8];
} ImeParam;

_Static_assert(sizeof(ImeParam) == 96, "SceImeParam size");
_Static_assert(sizeof(ImeEvent) == 72, "SceImeEvent size");

typedef struct {
    float x;
    float y;
    uint32_t height;
    uint32_t index;
} ImeCaret;

int32_t sceUserServiceInitialize(const void* params);
int32_t sceUserServiceGetInitialUser(int32_t* user_id);
int32_t sceImeOpen(const ImeParam* param, const void* extended);
int32_t sceImeClose(void);
int32_t sceImeUpdate(ImeEventHandler handler);
int32_t sceImeSetText(const char16_t* text, uint32_t length);
int32_t sceImeSetCaret(const ImeCaret* caret);

enum {
    EVENT_PRESS_CLOSE = 4,
    EVENT_PRESS_ENTER = 5,
    MAX_TEXT = 64,
    /* sceImeOpen rejects a work area that starts within maxTextLength + 0x1f
     * units after inputTextBuffer, so the buffer reserves that much. */
    BUFFER_UNITS = MAX_TEXT + 0x20,
    WORK_BYTES = 0x6000,
    WAIT_MS = 30000,
};

static const char16_t kText[] = u"Привет, 世界! Ünïcödé €";
enum { TEXT_UNITS = sizeof(kText) / sizeof(kText[0]) - 1 };

static char16_t g_buffer[BUFFER_UNITS];
static char16_t g_event_text[MAX_TEXT + 1];
static _Alignas(16) uint8_t g_work[WORK_BYTES];
static volatile uint32_t g_done_event;

static void on_event(void* arg, const ImeEvent* event) {
    (void)arg;
    printf("  event %u\n", event->id);
    if (event->id != EVENT_PRESS_CLOSE && event->id != EVENT_PRESS_ENTER) {
        return;
    }
    memset(g_event_text, 0, sizeof(g_event_text));
    if (event->param.text.str) {
        for (unsigned i = 0; i < MAX_TEXT && event->param.text.str[i]; ++i) {
            g_event_text[i] = event->param.text.str[i];
        }
    }
    g_done_event = event->id;
}

static void start(const StCase* c) { printf("case %s: start\n", c->name); }

static void expect_text(StCase* c, const char* what, const char16_t* got) {
    for (unsigned i = 0; i <= TEXT_UNITS; ++i) {
        st_case_expect(c, what, i, kText[i], got[i]);
    }
}

static int32_t open_ime(int32_t user_id) {
    ImeParam param;
    memset(&param, 0, sizeof(param));
    param.user_id = user_id;
    param.max_text_length = MAX_TEXT;
    param.input_text_buffer = g_buffer;
    param.work = g_work;
    param.handler = on_event;
    g_done_event = 0;
    return sceImeOpen(&param, NULL);
}

/* Pumps events until the panel is closed; false on timeout. */
static bool wait_closed(void) {
    for (unsigned ms = 0; ms < WAIT_MS && !g_done_event; ms += 16) {
        sceImeUpdate(on_event);
        sceKernelUsleep(16 * 1000);
    }
    return g_done_event != 0;
}

/* Waits for the close, then checks the buffer and the event text. */
static void finish_case(StCase* c) {
    if (!wait_closed()) {
        st_case_note(c, "not_closed", 0, EVENT_PRESS_CLOSE, 0);
    } else {
        expect_text(c, "buffer", g_buffer);
        expect_text(c, "event_text", g_event_text);
    }
    const int32_t rc = sceImeClose();
    if (rc < 0) {
        st_case_note(c, "close", 0, 0, (uint32_t)rc);
    }
}

enum { C_OPEN_TEXT, C_SET_TEXT, C_COUNT };

int main(void) {
    st_begin();

    StCase cases[C_COUNT] = {
        [C_OPEN_TEXT] = {.name = "open_text"},
        [C_SET_TEXT] = {.name = "set_text"},
    };

    sceUserServiceInitialize(NULL);
    int32_t user_id = -1;
    int32_t rc = sceUserServiceGetInitialUser(&user_id);
    if (rc < 0) {
        return st_fail("reason=initial_user rc=%08x", (uint32_t)rc);
    }

    {
        StCase* c = &cases[C_OPEN_TEXT];
        start(c);
        memset(g_buffer, 0, sizeof(g_buffer));
        memcpy(g_buffer, kText, sizeof(kText));
        rc = open_ime(user_id);
        if (rc < 0) {
            st_case_note(c, "open", 0, 0, (uint32_t)rc);
        } else {
            finish_case(c);
        }
    }
    {
        StCase* c = &cases[C_SET_TEXT];
        start(c);
        memset(g_buffer, 0, sizeof(g_buffer));
        rc = open_ime(user_id);
        if (rc < 0) {
            st_case_note(c, "open", 0, 0, (uint32_t)rc);
        } else {
            static const char16_t surrogate[] = u"A\U0001F600B";
            printf("  surrogate pair: sceImeSetText returned %08x\n",
                   (uint32_t)sceImeSetText(surrogate, 4));
            rc = sceImeSetText(kText, TEXT_UNITS);
            if (rc < 0) {
                st_case_note(c, "set_text", 0, 0, (uint32_t)rc);
            }
            const ImeCaret caret = {0.0f, 0.0f, 0, 3};
            printf("  sceImeSetCaret(3) returned %08x\n",
                   (uint32_t)sceImeSetCaret(&caret));
            finish_case(c);
        }
    }

    return st_finish_cases(cases, C_COUNT, "");
}
