/*
 * display.c – terminal output: table, switch summary, helpers
 */

#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <time.h>

#include "gitools.h"

/* ── Colour helper ─────────────────────────────────────────────────────────── */
const char *C(const char *color) {
    return opt_no_color ? "" : color;
}

/* ── Relative time ─────────────────────────────────────────────────────────── */
const char *relative_time(git_time_t t) {
    static char buf[64];
    if (t == 0) { snprintf(buf, sizeof(buf), "no commits"); return buf; }

    int64_t diff = (int64_t)time(NULL) - (int64_t)t;
    if (diff < 0) diff = 0;

    if (diff < 60)             snprintf(buf, sizeof(buf), "just now");
    else if (diff < 3600)      snprintf(buf, sizeof(buf), "%lld min ago",    (long long)(diff/60));
    else if (diff < 86400)     snprintf(buf, sizeof(buf), "%lld hour%s ago", (long long)(diff/3600),   diff/3600==1?"":"s");
    else if (diff < 2592000)   snprintf(buf, sizeof(buf), "%lld day%s ago",  (long long)(diff/86400),  diff/86400==1?"":"s");
    else if (diff < 31536000)  snprintf(buf, sizeof(buf), "%lld mo%s ago",   (long long)(diff/2592000), diff/2592000==1?"":"s");
    else                       snprintf(buf, sizeof(buf), "%lld yr ago",     (long long)(diff/31536000));
    return buf;
}

/* ── UTF-8 display width ───────────────────────────────────────────────────── */
int utf8_width(const char *s) {
    int w = 0;
    for (const unsigned char *p = (const unsigned char *)s; *p; ) {
        if      (*p < 0x80) { p += 1; }
        else if (*p < 0xE0) { p += 2; }
        else if (*p < 0xF0) { p += 3; }
        else                { p += 4; }
        w++;
    }
    return w;
}

/* ── Column printer ────────────────────────────────────────────────────────── */
void write_col(const char *s, int width) {
    int dw = utf8_width(s);
    if (dw <= width) {
        printf("%s", s);
        printf("%-*s", width - dw, "");  /* pad remaining columns */
    } else {
        printf("%.*s~", width - 1, s);   /* truncate (byte-based, fine for ASCII) */
    }
}

/* ── Sync indicator ────────────────────────────────────────────────────────── */
static void write_sync(const Repo *r) {
    char plain[32];
    const char *color;

    if (!r->has_remote) {
        snprintf(plain, sizeof(plain), "?");
        color = COL_DIM;
    } else if (r->ahead && r->behind) {
        snprintf(plain, sizeof(plain), "\xe2\x86\x91%zu\xe2\x86\x93%zu", r->ahead, r->behind);
        color = COL_MAGENTA;
    } else if (r->ahead) {
        snprintf(plain, sizeof(plain), "\xe2\x86\x91%zu", r->ahead);
        color = COL_GREEN;
    } else if (r->behind) {
        snprintf(plain, sizeof(plain), "\xe2\x86\x93%zu", r->behind);
        color = COL_RED;
    } else {
        snprintf(plain, sizeof(plain), "\xe2\x89\xa1");
        color = COL_DIM;
    }

    int dw = utf8_width(plain);
    printf("%s%s%s%-*s", C(color), plain, C(COL_RESET), COL_SYNC - dw, "");
}

/* ── Table header ──────────────────────────────────────────────────────────── */
void print_header(void) {
    printf("  %s%-*s  %-*s  %-*s  %-*s  %s%s\n",
        C(COL_DIM),
        COL_NAME,   "NAME",
        COL_BRANCH, "BRANCH",
        COL_SYNC,   "SYNC",
        COL_TIME,   "WHEN",
        "STATUS",
        C(COL_RESET));
    printf("  %s%s%s\n", C(COL_DIM), SEP_LINE, C(COL_RESET));
}

/* ── Single repo row ───────────────────────────────────────────────────────── */
void print_repo(const Repo *r) {
    int is_dirty = (r->staged || r->modified || r->untracked);

    const char *name = strrchr(r->path, '/');
    name = name ? name + 1 : r->path;

    printf("  %s", C(COL_CYAN));
    write_col(name, COL_NAME);
    printf("%s  ", C(COL_RESET));

    printf("%s", C(is_dirty ? COL_YELLOW : COL_GREEN));
    write_col(r->branch, COL_BRANCH);
    printf("%s  ", C(COL_RESET));

    write_sync(r);
    printf("  ");

    printf("%s", C(COL_DIM));
    write_col(relative_time(r->last_commit), COL_TIME);
    printf("%s  ", C(COL_RESET));

    if (!is_dirty) {
        printf("%s✓%s", C(COL_GREEN), C(COL_RESET));
    } else {
        if (r->staged)    printf("%s●%d%s ", C(COL_GREEN),   r->staged,    C(COL_RESET));
        if (r->modified)  printf("%s✗%d%s ", C(COL_RED),     r->modified,  C(COL_RESET));
        if (r->untracked) printf("%s?%d%s",  C(COL_MAGENTA), r->untracked, C(COL_RESET));
    }
    printf("\n");
}

/* ── Switch summary ────────────────────────────────────────────────────────── */
void print_switch_summary(void) {
    int switched = 0, already = 0, skipped = 0;

    printf("%sSwitching to branch:%s %s%s%s\n\n",
        C(COL_BOLD), C(COL_RESET),
        C(COL_YELLOW), opt_switch_branch, C(COL_RESET));

    for (size_t i = 0; i < g_repo_count; i++) {
        const Repo *r = &g_repos[i];
        const char *name = strrchr(r->path, '/');
        name = name ? name + 1 : r->path;

        printf("  %s", C(COL_CYAN));
        write_col(name, COL_NAME);
        printf("%s  ", C(COL_RESET));

        switch (r->switch_result) {
            case SR_SWITCHED:
                printf("%s✓ switched%s\n", C(COL_GREEN), C(COL_RESET));
                switched++;
                break;
            case SR_ALREADY:
                printf("%s· already on branch%s\n", C(COL_DIM), C(COL_RESET));
                already++;
                break;
            case SR_DIRTY:
                printf("%s✗ skipped%s  %s", C(COL_RED), C(COL_RESET), C(COL_DIM));
                if (r->staged)   printf("%d staged", r->staged);
                if (r->staged && r->modified) printf(", ");
                if (r->modified) printf("%d modified", r->modified);
                printf("%s\n", C(COL_RESET));
                skipped++;
                break;
            case SR_NOT_FOUND:
                printf("%s· branch not found%s\n", C(COL_DIM), C(COL_RESET));
                break;
            case SR_ERROR:
                printf("%s✗ error (checkout failed)%s\n", C(COL_RED), C(COL_RESET));
                skipped++;
                break;
            default:
                break;
        }
    }

    printf("\n  %s%s%s\n", C(COL_DIM), SEP_LINE, C(COL_RESET));
    printf("  switched %s%d%s · already %s%d%s",
        C(COL_GREEN), switched, C(COL_RESET),
        C(COL_DIM),   already,  C(COL_RESET));
    if (skipped)
        printf(" · skipped %s%d dirty%s", C(COL_RED), skipped, C(COL_RESET));
    printf("\n\n");
}
