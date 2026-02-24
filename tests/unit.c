/*
 * tests/unit.c – unit tests for pure functions
 */

#include <stdio.h>
#include <assert.h>
#include <string.h>
#include <stdint.h>

#include "../gitools.h"

/* globals normally defined in main.c */
int  opt_max_depth          = 5;
bool opt_all                = false;
bool opt_no_color           = true;   /* no colour in tests */
bool opt_switch             = false;
char opt_switch_branch[256] = "";

static int passed = 0, failed = 0;

#define CHECK(desc, expr) do { \
    if (expr) { \
        printf("  ok  %s\n", desc); \
        passed++; \
    } else { \
        printf("FAIL  %s\n", desc); \
        failed++; \
    } \
} while (0)

/* ── utf8_width ─────────────────────────────────────────────────────────────── */
static void test_utf8_width(void) {
    printf("utf8_width\n");
    CHECK("empty string",            utf8_width("") == 0);
    CHECK("ascii",                   utf8_width("hello") == 5);
    CHECK("arrow up ↑ (3-byte)",     utf8_width("\xe2\x86\x91") == 1);
    CHECK("arrow down ↓ (3-byte)",   utf8_width("\xe2\x86\x93") == 1);
    CHECK("equal ≡ (3-byte)",        utf8_width("\xe2\x89\xa1") == 1);
    CHECK("↑3 (symbol + digit)",     utf8_width("\xe2\x86\x91" "3") == 2);
    CHECK("↑3↓2 (diverged)",         utf8_width("\xe2\x86\x91" "3" "\xe2\x86\x93" "2") == 4);
    CHECK("4-byte char",             utf8_width("\xf0\x9f\x98\x80") == 1);
    CHECK("mixed ascii+utf8",        utf8_width("ok\xe2\x86\x91") == 3);
}

/* ── relative_time ──────────────────────────────────────────────────────────── */
static void test_relative_time(void) {
    printf("\nrelative_time\n");
    CHECK("zero = no commits",    strcmp(relative_time(0), "no commits") == 0);
}

/* ── main ───────────────────────────────────────────────────────────────────── */
int main(void) {
    test_utf8_width();
    test_relative_time();

    printf("\n%d passed, %d failed\n", passed, failed);
    return failed ? 1 : 0;
}
