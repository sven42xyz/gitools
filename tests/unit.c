/*
 * tests/unit.c – unit tests for pure functions
 */

#include <stdio.h>
#include <assert.h>
#include <string.h>
#include <stdint.h>
#include <time.h>

#include "../gitools.h"

/* globals normally defined in main.c */
int    opt_max_depth             = 5;
bool   opt_all                   = false;
bool   opt_no_color              = true;   /* no colour in tests */
bool   opt_verbose               = false;
bool   opt_switch                = false;
char   opt_switch_branch[256]    = "";
bool   opt_fetch                 = false;
bool   opt_pull                  = false;
bool   opt_watch                 = false;
int    opt_watch_interval        = 3;
bool   opt_dirty_only            = false;
char   opt_default_dir[PATH_MAX] = "";
char **opt_extra_skip            = NULL;
size_t opt_extra_skip_count      = 0;

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
    CHECK("mixed ascii+utf8",        utf8_width("ok\xe2\x86\x91") == 3);

    /* wide East Asian characters occupy two terminal columns */
    CHECK("CJK 中 (wide)",            utf8_width("\xe4\xb8\xad") == 2);
    CHECK("CJK 中文 (two wide)",      utf8_width("\xe4\xb8\xad\xe6\x96\x87") == 4);
    CHECK("Hangul 한 (wide)",         utf8_width("\xed\x95\x9c") == 2);
    CHECK("fullwidth Ａ (wide)",      utf8_width("\xef\xbc\xa1") == 2);
    CHECK("emoji 😀 (wide)",          utf8_width("\xf0\x9f\x98\x80") == 2);
    CHECK("ascii + CJK width",       utf8_width("ab\xe4\xb8\xad") == 4);

    /* combining marks add zero width (e + combining acute = one column) */
    CHECK("combining acute (0-width)", utf8_width("e\xcc\x81") == 1);
    CHECK("zero-width space",          utf8_width("a\xe2\x80\x8b" "b") == 2);

    /* truncated / incomplete sequences must terminate and not read past the
     * null terminator; the stray lead byte is counted as a single column */
    CHECK("truncated 3-byte seq",    utf8_width("\xe2\x86") == 1);
    CHECK("truncated 4-byte seq",    utf8_width("\xf0\x9f\x98") == 1);
    CHECK("ascii before truncated",  utf8_width("ab\xe2\x86") == 3);
}

/* ── relative_time ──────────────────────────────────────────────────────────── */
static void test_relative_time(void) {
    printf("\nrelative_time\n");
    time_t now = time(NULL);
    CHECK("zero = no commits",  strcmp(relative_time(0),              "no commits") == 0);
    CHECK("30s = just now",     strcmp(relative_time(now - 30),       "just now")   == 0);
    CHECK("5 min ago",          strcmp(relative_time(now - 300),      "5 min ago")  == 0);
    CHECK("1 hour ago",         strcmp(relative_time(now - 3600),     "1 hour ago") == 0);
    CHECK("3 hours ago",        strcmp(relative_time(now - 10800),    "3 hours ago") == 0);
    CHECK("1 day ago",          strcmp(relative_time(now - 86400),    "1 day ago")  == 0);
    CHECK("3 days ago",         strcmp(relative_time(now - 259200),   "3 days ago") == 0);
    CHECK("1 month ago",        strcmp(relative_time(now - 2592000),  "1 mo ago")   == 0);
    CHECK("2 months ago",       strcmp(relative_time(now - 5184000),  "2 mos ago")  == 0);
    CHECK("1 year ago",         strcmp(relative_time(now - 31536000), "1 yr ago")   == 0);
}

/* ── ellipsize ──────────────────────────────────────────────────────────────── */
static void test_ellipsize(void) {
    printf("\nellipsize\n");
    CHECK("short string unchanged",  strcmp(ellipsize("abc", 10),  "abc")   == 0);
    CHECK("exact fit unchanged",     strcmp(ellipsize("abcde", 5), "abcde") == 0);
    /* drops the front, keeps the tail, prefixes a 1-column ellipsis */
    CHECK("long string truncated",   strcmp(ellipsize("abcdefghij", 5), "\xe2\x80\xa6ghij") == 0);
    CHECK("truncated width fits",    utf8_width(ellipsize("abcdefghij", 5)) == 5);
    CHECK("max_w < 2 returns full",  strcmp(ellipsize("abcdef", 1), "abcdef") == 0);
    /* wide (2-col) characters: the truncated result must still fit the budget */
    CHECK("wide string fits budget", utf8_width(ellipsize("\xe4\xb8\xad\xe6\x96\x87\xe4\xb8\xad\xe6\x96\x87\xe4\xb8\xad", 6)) <= 6);
}

/* ── repo_category ──────────────────────────────────────────────────────────── */
static void test_repo_category(void) {
    printf("\nrepo_category\n");
    char out[PATH_MAX];

    repo_category("/p/tt", "/p/tt/foo", out, sizeof(out));
    CHECK("direct child -> empty",          strcmp(out, "") == 0);

    repo_category("/p/tt", "/p/tt/core/auth", out, sizeof(out));
    CHECK("one level -> single segment",    strcmp(out, "core") == 0);

    repo_category("/p/tt", "/p/tt/core/packages/auth", out, sizeof(out));
    CHECK("two levels -> chevron breadcrumb", strcmp(out, "core \xe2\x80\xba packages") == 0);

    repo_category("/p/tt", "/p/tt/a/b/c/d", out, sizeof(out));
    CHECK("full breadcrumb, chevron joins",  strcmp(out, "a \xe2\x80\xba b \xe2\x80\xba c") == 0);

    /* trailing slash on abs_dir must not shift the boundary */
    repo_category("/p/tt/", "/p/tt/core/auth", out, sizeof(out));
    CHECK("trailing slash on root",          strcmp(out, "core") == 0);

    /* repo identical to root -> empty */
    repo_category("/p/tt", "/p/tt", out, sizeof(out));
    CHECK("repo == root -> empty",           strcmp(out, "") == 0);

    /* sibling dir sharing a prefix must not be treated as nested */
    repo_category("/p/tt", "/p/ttx/foo", out, sizeof(out));
    CHECK("prefix-only sibling -> empty",    strcmp(out, "") == 0);

    /* root is filesystem root */
    repo_category("/", "/srv/repo", out, sizeof(out));
    CHECK("root '/' one level",              strcmp(out, "srv") == 0);
}

/* ── main ───────────────────────────────────────────────────────────────────── */
int main(void) {
    test_utf8_width();
    test_relative_time();
    test_ellipsize();
    test_repo_category();

    printf("\n%d passed, %d failed\n", passed, failed);
    return failed ? 1 : 0;
}
