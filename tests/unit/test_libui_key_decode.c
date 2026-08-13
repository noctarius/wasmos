/* Host unit test for libui's GFX_EVENT_KEY decoding.
 *
 * The compositor packs two values into one key code: low byte = the character
 * the vt decoded with the active keymap (0 when the key has none), high byte =
 * the raw set-1 scancode.  See docs/architecture/20-graphics-framebuffer-and-
 * compositor.md §Events.
 *
 * The packed code must NOT reach ui_utf8_encode() directly: typing 'a'
 * (scancode 0x1E) would append U+1E61 instead of 'a' — an unmapped codepoint the
 * font service renders as .notdef (a tofu box) — and Backspace (0x0E08) and
 * Enter (0x1C0A) would stop matching their control codes.  These tests pin the decode contract at
 * the two handlers that consume characters. */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#include "test_shuffle.h"

#include "wasmos/libui.h"

static int g_failures;
static int g_checks;

static void expect(int cond, const char* what) {
    g_checks++;
    if (!cond) {
        g_failures++;
        printf("[fail] %s\n", what);
    }
}

static void expect_text(const ui_text_data_t* td, const char* want, const char* what) {
    const char* got = td->text ? td->text : "";
    g_checks++;
    if (strcmp(got, want) != 0) {
        g_failures++;
        printf("[fail] %s: want \"%s\", got \"%s\"\n", what, want, got);
        return;
    }
    /* text_len must stay consistent with the NUL-terminated content, otherwise
     * the next append writes at the wrong offset. */
    g_checks++;
    if (td->text_len != (int32_t)strlen(want)) {
        g_failures++;
        printf("[fail] %s: text_len %d, want %zu\n", what, td->text_len, strlen(want));
    }
}

/* Build a packed key code the way the compositor does. */
static uint32_t packed(uint32_t scancode, uint32_t ch) {
    return (ch & 0xFFu) | ((scancode & 0xFFu) << 8);
}

/* ---- accessors ------------------------------------------------------------- */

static void test_accessors(void) {
    const uint32_t key = packed(0x1E, 'a');
    expect(key == 0x1E61u, "packed('a') is 0x1E61");
    expect(ui_key_char(key) == 'a', "ui_key_char extracts the character");
    expect(ui_key_scancode(key) == 0x1Eu, "ui_key_scancode extracts the scancode");

    /* Extended keys carry no character; the scancode is the whole signal. */
    const uint32_t up = packed(0x48, 0);
    expect(ui_key_char(up) == 0, "arrow-up has no character");
    expect(ui_key_scancode(up) == 0x48u, "arrow-up keeps its scancode");
}

/* ---- text input ------------------------------------------------------------ */

static void text_input_init(ui_component_t* c, ui_text_data_t* td) {
    memset(c, 0, sizeof(*c));
    memset(td, 0, sizeof(*td));
    c->type = UI_COMPONENT_TEXT_INPUT;
    c->component_data = td;
}

static void test_text_input_types_the_character(void) {
    ui_context_t ctx;
    ui_component_t c;
    ui_text_data_t td;
    memset(&ctx, 0, sizeof(ctx));
    text_input_init(&c, &td);

    /* "hi" — the exact bytes the vt/compositor produce for those two keys. */
    ui_text_input_handle_key(&ctx, &c, packed(0x23, 'h'));
    ui_text_input_handle_key(&ctx, &c, packed(0x17, 'i'));
    expect_text(&td, "hi", "printable keys append their character");
    expect(ctx.dirty == 1, "typing marks the context dirty");

    /* The regression, stated directly: no stray multi-byte sequence. */
    expect(td.text_len == 2, "'h' appended 1 byte, not a 3-byte codepoint");

    free(td.text);
}

static void test_text_input_shifted_and_symbols(void) {
    ui_context_t ctx;
    ui_component_t c;
    ui_text_data_t td;
    memset(&ctx, 0, sizeof(ctx));
    text_input_init(&c, &td);

    ui_text_input_handle_key(&ctx, &c, packed(0x1E, 'A')); /* Shift+a */
    ui_text_input_handle_key(&ctx, &c, packed(0x39, ' ')); /* space */
    ui_text_input_handle_key(&ctx, &c, packed(0x02, '1'));
    expect_text(&td, "A 1", "shifted letters, space and digits round-trip");

    free(td.text);
}

static void test_text_input_backspace(void) {
    ui_context_t ctx;
    ui_component_t c;
    ui_text_data_t td;
    memset(&ctx, 0, sizeof(ctx));
    text_input_init(&c, &td);

    ui_text_input_handle_key(&ctx, &c, packed(0x1E, 'a'));
    ui_text_input_handle_key(&ctx, &c, packed(0x30, 'b'));
    expect_text(&td, "ab", "two characters typed");

    /* Backspace arrives as scancode 0x0E with character 0x08. */
    ui_text_input_handle_key(&ctx, &c, packed(0x0E, 8));
    expect_text(&td, "a", "backspace deletes the last character");

    ui_text_input_handle_key(&ctx, &c, packed(0x0E, 8));
    expect_text(&td, "", "backspace empties the buffer");

    /* Backspace on an empty buffer must not underflow. */
    ui_text_input_handle_key(&ctx, &c, packed(0x0E, 8));
    expect_text(&td, "", "backspace on empty is a no-op");
    expect(td.text_len == 0, "text_len stays non-negative");

    free(td.text);
}

static void test_text_input_ignores_non_characters(void) {
    ui_context_t ctx;
    ui_component_t c;
    ui_text_data_t td;
    memset(&ctx, 0, sizeof(ctx));
    text_input_init(&c, &td);

    ui_text_input_handle_key(&ctx, &c, packed(0x1E, 'a'));
    ctx.dirty = 0;

    /* Control characters and characterless keys must not be inserted. Before
     * the fix each of these landed in the buffer as a bogus codepoint. */
    ui_text_input_handle_key(&ctx, &c, packed(0x1C, '\n')); /* Enter */
    ui_text_input_handle_key(&ctx, &c, packed(0x0F, '\t')); /* Tab */
    ui_text_input_handle_key(&ctx, &c, packed(0x48, 0));    /* Arrow up (extended) */
    ui_text_input_handle_key(&ctx, &c, packed(0x4B, 0));    /* Arrow left */
    ui_text_input_handle_key(&ctx, &c, packed(0x3B, 0));    /* F1 */
    expect_text(&td, "a", "control and extended keys insert nothing");
    expect(ctx.dirty == 0, "ignored keys do not mark the context dirty");

    free(td.text);
}

static void test_text_input_high_byte_character(void) {
    ui_context_t ctx;
    ui_component_t c;
    ui_text_data_t td;
    memset(&ctx, 0, sizeof(ctx));
    text_input_init(&c, &td);

    /* A non-US keymap emits Latin-1 bytes: 0xFC is 'ü' on the DE layout. It is
     * a character, so it is encoded as U+00FC (2 UTF-8 bytes) — and backspace
     * must remove the whole codepoint, not one byte of it. */
    ui_text_input_handle_key(&ctx, &c, packed(0x27, 0xFC));
    expect(td.text_len == 2, "Latin-1 character encodes to 2 UTF-8 bytes");
    expect(td.text && (uint8_t)td.text[0] == 0xC3 && (uint8_t)td.text[1] == 0xBC,
           "0xFC encodes as U+00FC");

    ui_text_input_handle_key(&ctx, &c, packed(0x0E, 8));
    expect_text(&td, "", "backspace removes the whole multi-byte character");

    free(td.text);
}

static void test_text_input_growth(void) {
    ui_context_t ctx;
    ui_component_t c;
    ui_text_data_t td;
    memset(&ctx, 0, sizeof(ctx));
    text_input_init(&c, &td);

    /* Push past UI_TEXT_INITIAL_CAP so the realloc path is exercised with the
     * decoded (1-byte) characters rather than the old 3-byte encodings. */
    char want[129];
    for (int i = 0; i < 128; i++) {
        ui_text_input_handle_key(&ctx, &c, packed(0x1E, 'x'));
        want[i] = 'x';
    }
    want[128] = '\0';
    expect_text(&td, want, "buffer grows correctly across 128 keystrokes");

    free(td.text);
}

/* ---- dropdown -------------------------------------------------------------- */

static void dropdown_init(ui_component_t* c, ui_dropdown_data_t* d) {
    memset(c, 0, sizeof(*c));
    memset(d, 0, sizeof(*d));
    c->type = UI_COMPONENT_DROPDOWN;
    c->component_data = d;
    d->list.count = 3;
    d->list.selected = 0;
}

static void test_dropdown_keys(void) {
    ui_context_t ctx;
    ui_component_t c;
    ui_dropdown_data_t d;
    memset(&ctx, 0, sizeof(ctx));
    dropdown_init(&c, &d);

    ui_dropdown_handle_key(&ctx, &c, packed(0x39, ' '));
    expect(d.dropdown_open == 1, "space opens the dropdown");

    ui_dropdown_handle_key(&ctx, &c, packed(0x24, 'j'));
    expect(d.list.selected == 1, "'j' moves the selection down");

    ui_dropdown_handle_key(&ctx, &c, packed(0x25, 'k'));
    expect(d.list.selected == 0, "'k' moves the selection back up");

    /* Selection must clamp at both ends. */
    ui_dropdown_handle_key(&ctx, &c, packed(0x25, 'k'));
    expect(d.list.selected == 0, "'k' clamps at the first item");

    ui_dropdown_handle_key(&ctx, &c, packed(0x01, 27)); /* Esc */
    expect(d.dropdown_open == 0, "Esc closes the dropdown");
}

int main(void) {
    /* Randomized order: a case that leaks state must not be able to make its
     * neighbour pass. Replay a failure with WASMOS_TEST_SEED. */
    static const wasmos_test_void_case_t cases[] = {
        WASMOS_TEST_CASE(test_accessors),
        WASMOS_TEST_CASE(test_text_input_types_the_character),
        WASMOS_TEST_CASE(test_text_input_shifted_and_symbols),
        WASMOS_TEST_CASE(test_text_input_backspace),
        WASMOS_TEST_CASE(test_text_input_ignores_non_characters),
        WASMOS_TEST_CASE(test_text_input_high_byte_character),
        WASMOS_TEST_CASE(test_text_input_growth),
        WASMOS_TEST_CASE(test_dropdown_keys),
    };
    const uint64_t seed = wasmos_test_run_all_void(cases, (int)(sizeof(cases) / sizeof(cases[0])));
    printf("test_libui_key_decode: %d checks, %d failures\n", g_checks, g_failures);
    if (g_failures != 0) {
        wasmos_test_report_seed(seed);
    }
    return g_failures == 0 ? 0 : 1;
}
