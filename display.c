/*
 * display.c – terminal output: table, switch summary, helpers
 */

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <strings.h>   /* strcasecmp */
#include <time.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <stdatomic.h>
#include <pthread.h>

#include "gitools.h"

/* Terminal width in columns, or 0 when unknown (e.g. output is piped — then
 * the table is printed at full width so scripts get complete data). */
int term_width(void) {
    if (!isatty(STDOUT_FILENO))
        return 0;   /* piped output → full width so scripts get complete data */
    struct winsize ws;
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == 0 && ws.ws_col > 0)
        return ws.ws_col;
    const char *cols = getenv("COLUMNS");
    if (cols) { int v = atoi(cols); if (v > 0) return v; }
    return 0;
}

/* ── UTF-8 display width ───────────────────────────────────────────────────── */
/*
 * Terminal column width of UTF-8 text. We can't use the system wcwidth(): the
 * program runs in the C locale (LC_ALL=C, set for stable git output matching),
 * where wcwidth/mbrtowc don't understand UTF-8. So width is computed from the
 * decoded codepoint against built-in Unicode tables (after Markus Kuhn's
 * public-domain wcwidth, 2007): combining/zero-width marks are 0 columns, East
 * Asian Wide/Fullwidth and the common emoji blocks are 2, everything else 1.
 * This keeps the table aligned when a repo or branch name contains CJK text.
 */

/* Decode one UTF-8 sequence at p into *cp. Returns the byte length (1..4).
 * An invalid lead byte or a truncated sequence decodes as the single lead byte
 * (length 1), so callers always advance and never loop forever. */
static int utf8_decode(const unsigned char *p, uint32_t *cp) {
    unsigned char c = p[0];
    if (c < 0x80) { *cp = c; return 1; }
    int len;
    uint32_t v;
    if      ((c & 0xE0) == 0xC0) { len = 2; v = c & 0x1Fu; }
    else if ((c & 0xF0) == 0xE0) { len = 3; v = c & 0x0Fu; }
    else if ((c & 0xF8) == 0xF0) { len = 4; v = c & 0x07u; }
    else { *cp = c; return 1; }                  /* invalid lead byte */
    for (int i = 1; i < len; i++) {
        if ((p[i] & 0xC0) != 0x80) { *cp = c; return 1; }  /* truncated/invalid */
        v = (v << 6) | (uint32_t)(p[i] & 0x3F);
    }
    *cp = v;
    return len;
}

/* Zero-width (combining / format) codepoint ranges, sorted for binary search. */
static const struct { uint32_t first, last; } COMBINING[] = {
    {0x0300,0x036F},{0x0483,0x0489},{0x0591,0x05BD},{0x05BF,0x05BF},
    {0x05C1,0x05C2},{0x05C4,0x05C5},{0x05C7,0x05C7},{0x0610,0x061A},
    {0x064B,0x065F},{0x0670,0x0670},{0x06D6,0x06DC},{0x06DF,0x06E4},
    {0x06E7,0x06E8},{0x06EA,0x06ED},{0x0711,0x0711},{0x0730,0x074A},
    {0x07A6,0x07B0},{0x07EB,0x07F3},{0x0816,0x0819},{0x081B,0x0823},
    {0x0825,0x0827},{0x0829,0x082D},{0x0900,0x0902},{0x093C,0x093C},
    {0x0941,0x0948},{0x094D,0x094D},{0x0951,0x0957},{0x0962,0x0963},
    {0x0981,0x0981},{0x09BC,0x09BC},{0x09C1,0x09C4},{0x09CD,0x09CD},
    {0x09E2,0x09E3},{0x0A01,0x0A02},{0x0A3C,0x0A3C},{0x0A41,0x0A42},
    {0x0A47,0x0A48},{0x0A4B,0x0A4D},{0x0A70,0x0A71},{0x0A81,0x0A82},
    {0x0ABC,0x0ABC},{0x0AC1,0x0AC5},{0x0AC7,0x0AC8},{0x0ACD,0x0ACD},
    {0x0B01,0x0B01},{0x0B3C,0x0B3C},{0x0B3F,0x0B3F},{0x0B41,0x0B44},
    {0x0B4D,0x0B4D},{0x0B56,0x0B56},{0x0B82,0x0B82},{0x0BC0,0x0BC0},
    {0x0BCD,0x0BCD},{0x0C3E,0x0C40},{0x0C46,0x0C48},{0x0C4A,0x0C4D},
    {0x0C55,0x0C56},{0x0CBC,0x0CBC},{0x0CCC,0x0CCD},{0x0CE2,0x0CE3},
    {0x0D41,0x0D44},{0x0D4D,0x0D4D},{0x0DCA,0x0DCA},{0x0DD2,0x0DD4},
    {0x0DD6,0x0DD6},{0x0E31,0x0E31},{0x0E34,0x0E3A},{0x0E47,0x0E4E},
    {0x0EB1,0x0EB1},{0x0EB4,0x0EB9},{0x0EBB,0x0EBC},{0x0EC8,0x0ECD},
    {0x0F18,0x0F19},{0x0F35,0x0F35},{0x0F37,0x0F37},{0x0F39,0x0F39},
    {0x0F71,0x0F7E},{0x0F80,0x0F84},{0x0F86,0x0F87},{0x0F90,0x0F97},
    {0x0F99,0x0FBC},{0x0FC6,0x0FC6},{0x102D,0x1030},{0x1032,0x1037},
    {0x1039,0x103A},{0x1058,0x1059},{0x1160,0x11FF},{0x135F,0x135F},
    {0x1712,0x1714},{0x1732,0x1734},{0x1752,0x1753},{0x1772,0x1773},
    {0x17B4,0x17B5},{0x17B7,0x17BD},{0x17C6,0x17C6},{0x17C9,0x17D3},
    {0x17DD,0x17DD},{0x180B,0x180D},{0x18A9,0x18A9},{0x1920,0x1922},
    {0x1927,0x1928},{0x1932,0x1932},{0x1939,0x193B},{0x1A17,0x1A18},
    {0x1B00,0x1B03},{0x1B34,0x1B34},{0x1B36,0x1B3A},{0x1B3C,0x1B3C},
    {0x1B42,0x1B42},{0x1B6B,0x1B73},{0x1DC0,0x1DCA},{0x1DFE,0x1DFF},
    {0x200B,0x200F},{0x202A,0x202E},{0x2060,0x2063},{0x206A,0x206F},
    {0x20D0,0x20F0},{0x302A,0x302F},{0x3099,0x309A},{0xA806,0xA806},
    {0xA80B,0xA80B},{0xA825,0xA826},{0xFB1E,0xFB1E},{0xFE00,0xFE0F},
    {0xFE20,0xFE26},{0xFEFF,0xFEFF},{0xFFF9,0xFFFB},{0x1D167,0x1D169},
    {0x1D173,0x1D182},{0x1D185,0x1D18B},{0x1D1AA,0x1D1AD},{0x1D242,0x1D244},
    {0xE0001,0xE0001},{0xE0020,0xE007F},{0xE0100,0xE01EF},
};

static int is_combining(uint32_t cp) {
    int lo = 0, hi = (int)(sizeof(COMBINING) / sizeof(COMBINING[0])) - 1;
    if (cp < COMBINING[0].first || cp > COMBINING[hi].last) return 0;
    while (lo <= hi) {
        int mid = (lo + hi) / 2;
        if      (cp < COMBINING[mid].first) hi = mid - 1;
        else if (cp > COMBINING[mid].last)  lo = mid + 1;
        else return 1;
    }
    return 0;
}

/* East Asian Wide / Fullwidth ranges plus the common double-width emoji blocks. */
static int is_wide(uint32_t cp) {
    return
        (cp >= 0x1100 && cp <= 0x115F) ||   /* Hangul Jamo */
        cp == 0x2329 || cp == 0x232A ||
        (cp >= 0x2E80 && cp <= 0xA4CF && cp != 0x303F) || /* CJK … Yi */
        (cp >= 0xAC00 && cp <= 0xD7A3) ||   /* Hangul syllables */
        (cp >= 0xF900 && cp <= 0xFAFF) ||   /* CJK compat ideographs */
        (cp >= 0xFE10 && cp <= 0xFE19) ||   /* vertical forms */
        (cp >= 0xFE30 && cp <= 0xFE6F) ||   /* CJK compat forms */
        (cp >= 0xFF00 && cp <= 0xFF60) ||   /* fullwidth forms */
        (cp >= 0xFFE0 && cp <= 0xFFE6) ||
        (cp >= 0x1F000 && cp <= 0x1FAFF) || /* emoji & symbols (mostly wide) */
        (cp >= 0x20000 && cp <= 0x3FFFD);   /* CJK extension planes */
}

/* Display columns for one codepoint: 0 (combining/control), 1 (normal), 2 (wide). */
static int codepoint_width(uint32_t cp) {
    if (cp == 0) return 0;
    if (cp < 0x20 || (cp >= 0x7F && cp < 0xA0)) return 0;   /* C0/C1 controls */
    if (is_combining(cp)) return 0;
    if (is_wide(cp))      return 2;
    return 1;
}

int utf8_width(const char *s) {
    int w = 0;
    const unsigned char *p = (const unsigned char *)s;
    while (*p) {
        uint32_t cp;
        p += utf8_decode(p, &cp);
        w += codepoint_width(cp);
    }
    return w;
}

/* Byte length of the longest prefix of s whose display width is <= max_w.
 * When out_w is non-NULL it receives that prefix's actual width, which can be
 * one less than max_w when a wide character would have overflowed the budget. */
static size_t utf8_fit(const char *s, int max_w, int *out_w) {
    const unsigned char *p = (const unsigned char *)s;
    int w = 0;
    while (*p) {
        uint32_t cp;
        int len = utf8_decode(p, &cp);
        int cw  = codepoint_width(cp);
        if (w + cw > max_w) break;
        w += cw;
        p += len;
    }
    if (out_w) *out_w = w;
    return (size_t)(p - (const unsigned char *)s);
}

/* Truncate s to at most max_w display columns. For paths the tail is the most
 * useful part, so the front is dropped and replaced with a leading ellipsis.
 * Returns a pointer to a rotating static buffer. */
const char *ellipsize(const char *s, int max_w) {
    static char buf[PATH_MAX + 8];
    int w = utf8_width(s);
    if (max_w < 2 || w <= max_w) {
        snprintf(buf, sizeof(buf), "%s", s);
        return buf;
    }
    int drop = w - (max_w - 1);          /* columns to drop (1 kept for the …) */
    const unsigned char *p = (const unsigned char *)s;
    int dropped = 0;
    while (*p && dropped < drop) {        /* drop whole codepoints by their width */
        uint32_t cp;
        int len = utf8_decode(p, &cp);
        dropped += codepoint_width(cp);
        p += len;
    }
    snprintf(buf, sizeof(buf), "\xe2\x80\xa6%s", (const char *)p);   /* … + tail */
    return buf;
}

/* ── Colour helper ─────────────────────────────────────────────────────────── */
const char *C(const char *color) {
    return opt_no_color ? "" : color;
}

/* Erase-to-end-of-line marker emitted at the end of every rewritten line in
 * watch mode, so a narrower frame doesn't leave stale characters (e.g. a second
 * WHEN/STATUS column) from the previous, wider one. Empty outside watch mode so
 * piped output stays clean. */
const char *EOL(void) {
    return opt_watch ? "\033[K" : "";
}

/* ── Relative time ─────────────────────────────────────────────────────────── */
const char *relative_time(git_time_t t) {
    static char buf[64];
    if (t == 0) { snprintf(buf, sizeof(buf), "no commits"); return buf; }

    int64_t diff = (int64_t)time(NULL) - (int64_t)t;
    if (diff < 0) diff = 0;

    if (diff < 60)             snprintf(buf, sizeof(buf), "just now");
    else if (diff < 3600)      snprintf(buf, sizeof(buf), "%lld min ago",    (long long)(diff/60));
    else if (diff < 86400)     snprintf(buf, sizeof(buf), "%lld hour%s ago", (long long)(diff/3600),   diff/3600==1?  "":"s");
    else if (diff < 2592000)   snprintf(buf, sizeof(buf), "%lld day%s ago",  (long long)(diff/86400),  diff/86400==1? "":"s");
    else if (diff < 31536000)  snprintf(buf, sizeof(buf), "%lld mo%s ago",   (long long)(diff/2592000), diff/2592000==1?"":"s");
    else                       snprintf(buf, sizeof(buf), "%lld yr ago",     (long long)(diff/31536000));
    return buf;
}

/* ── Column printer ────────────────────────────────────────────────────────── */
void write_col(const char *s, int width) {
    int dw = utf8_width(s);
    if (dw <= width) {
        printf("%s", s);
        printf("%-*s", width - dw, "");
    } else {
        /* truncate at a character boundary; pad so the cell is exactly `width`
         * even when a wide character left the last column short of width-1 */
        int pw = 0;
        size_t bytes = utf8_fit(s, width - 1, &pw);
        printf("%.*s~%-*s", (int)bytes, s, width - pw - 1, "");
    }
}

/* ── Sync string builder ────────────────────────────────────────────────────── */
static void build_sync_str(const Repo *r, char *buf, size_t n, const char **color_out) {
    if (!r->has_remote) {
        *color_out = COL_DIM;
        snprintf(buf, n, "?");
    } else if (r->ahead && r->behind) {
        *color_out = COL_MAGENTA;
        snprintf(buf, n, "\xe2\x86\x91%zu\xe2\x86\x93%zu", r->ahead, r->behind);
    } else if (r->ahead) {
        *color_out = COL_GREEN;
        snprintf(buf, n, "\xe2\x86\x91%zu", r->ahead);
    } else if (r->behind) {
        *color_out = COL_RED;
        snprintf(buf, n, "\xe2\x86\x93%zu", r->behind);
    } else {
        *color_out = COL_DIM;
        snprintf(buf, n, "\xe2\x89\xa1");
    }
}

/* ── Sync indicator ────────────────────────────────────────────────────────── */
static void write_sync(const Repo *r, int width) {
    char plain[32];
    const char *color;
    build_sync_str(r, plain, sizeof(plain), &color);
    int dw = utf8_width(plain);
    printf("%s%s%s%-*s", C(color), plain, C(COL_RESET), width - dw, "");
}

/* ── Dynamic column widths ──────────────────────────────────────────────────── */
/* Number of decimal digits in a non-negative int (for STATUS-width sizing). */
static int digits10(int n) {
    int w = 1;
    while (n >= 10) { n /= 10; w++; }
    return w;
}

/* Display width of a repo's STATUS cell: "✓" when clean, else ●staged ✗mod ?untr. */
static int repo_status_width(const Repo *r) {
    if (!(r->staged || r->modified || r->untracked)) return 1;   /* ✓ */
    int w = 0;
    if (r->staged)    w += 1 + digits10(r->staged)    + 1;       /* "●N " */
    if (r->modified)  w += 1 + digits10(r->modified)  + 1;       /* "✗N " */
    if (r->untracked) w += 1 + digits10(r->untracked);          /* "?N"  */
    return w;
}

/* Display width of a category header's aggregated STATUS cell (✓ or ↑N ↓N ●N). */
int group_status_width(const Group *g) {
    if (!g->n_dirty && !g->n_ahead && !g->n_behind) return 1;    /* ✓ */
    int w = 0;
    if (g->n_ahead)  w += 1 + digits10(g->n_ahead)  + 1;
    if (g->n_behind) w += 1 + digits10(g->n_behind) + 1;
    if (g->n_dirty)  w += 1 + digits10(g->n_dirty);
    return w;
}

/*
 * Column widths are sized to the content, never stretched to fill the screen.
 * The NAME column tracks the repo names only — category header breadcrumbs span
 * the whole row at render time, so they don't widen it. min_status_w lets the
 * watch view reserve room for the widest header status aggregate, and the cap
 * uses the *actual* STATUS width rather than a fixed guess, so names are only
 * '~'-truncated when the terminal really is too narrow. Pass 0 when unused.
 */
ColWidths compute_col_widths(int min_status_w) {
    ColWidths w = {
        .name   = (int)strlen("NAME"),
        .branch = (int)strlen("BRANCH"),
        .sync   = (int)strlen("SYNC"),
        .time   = (int)strlen("WHEN"),
    };
    int status_w = MAX(min_status_w, (int)strlen("STATUS"));
    for (size_t i = 0; i < g_repo_count; i++) {
        const Repo *r = &g_repos[i];

        const char *name = strrchr(r->path, '/');
        name = name ? name + 1 : r->path;
        w.name = MAX(w.name, utf8_width(name));

        w.branch = MAX(w.branch, utf8_width(r->branch));

        char sync_buf[32];
        const char *dummy;
        build_sync_str(r, sync_buf, sizeof(sync_buf), &dummy);
        w.sync = MAX(w.sync, utf8_width(sync_buf));

        w.time = MAX(w.time, utf8_width(relative_time(r->last_commit)));

        status_w = MAX(status_w, repo_status_width(r));
    }

    /* Cap the variable NAME / BRANCH columns so a single row never exceeds the
     * terminal width and wraps (which would corrupt the table, and the in-place
     * redraw in watch mode). write_col() truncates over-long content with '~'.
     * The layout is: 2(lead) + name +2 + branch +2 + sync +2 + time +2 + STATUS. */
    int tw = term_width();
    if (tw > 0) {
        const int group_indent = (min_status_w > 0) ? 2 : 0;  /* nested-repo indent */
        int other = 2 + group_indent + 2 + 2 + 2 + 2 + w.sync + w.time + status_w;
        int avail = tw - other;          /* budget shared by NAME + BRANCH */
        if (avail < 10) avail = 10;
        while (w.name + w.branch > avail) {
            if (w.branch > 6 && w.branch >= w.name) w.branch--;
            else if (w.name > 4)                    w.name--;
            else if (w.branch > 6)                  w.branch--;
            else break;                  /* at minimums (NAME 4 / BRANCH 6) */
        }
    }
    return w;
}

/* ── Separator ──────────────────────────────────────────────────────────────── */
void print_separator(const ColWidths *w) {
    int total = w->name + 2 + w->branch + 2 + w->sync + 2 + w->time + 2
                + (int)strlen("STATUS");
    printf("  %s", C(COL_DIM));
    for (int i = 0; i < total; i++) printf("─");
    printf("%s%s\n", C(COL_RESET), EOL());
}

/* ── Table header ──────────────────────────────────────────────────────────── */
void print_header(const ColWidths *w) {
    printf("  %s%-*s  %-*s  %-*s  %-*s  %s%s%s\n",
        C(COL_DIM),
        w->name,   "NAME",
        w->branch, "BRANCH",
        w->sync,   "SYNC",
        w->time,   "WHEN",
        "STATUS",
        C(COL_RESET), EOL());
    print_separator(w);
}

/* ── Single repo row ───────────────────────────────────────────────────────── */
/*
 * Render one repo row. `selected` draws a cursor caret in the lead column (used
 * by the watch-mode grouped view); `indent` nests the name under a category
 * header. The indent is absorbed into the NAME column (not prepended to the
 * whole row), so the BRANCH/SYNC/WHEN/STATUS columns stay aligned with
 * un-indented top-level repo rows.
 */
static void print_repo_line(const Repo *r, const ColWidths *w,
                            bool selected, int indent) {
    int is_dirty = (r->staged || r->modified || r->untracked);

    const char *name = strrchr(r->path, '/');
    name = name ? name + 1 : r->path;

    /* lead column (2 cols): cursor caret when selected, else blank */
    if (selected) printf("%s\xe2\x96\xb8%s ", C(COL_GREEN), C(COL_RESET));
    else          printf("  ");
    for (int i = 0; i < indent; i++) putchar(' ');

    printf("%s", C(COL_CYAN));
    write_col(name, w->name - indent);   /* indent eats into the NAME column width */
    printf("%s  ", C(COL_RESET));

    printf("%s", C(is_dirty ? COL_YELLOW : COL_GREEN));
    write_col(r->branch, w->branch);
    printf("%s  ", C(COL_RESET));

    write_sync(r, w->sync);
    printf("  ");

    printf("%s", C(COL_DIM));
    write_col(relative_time(r->last_commit), w->time);
    printf("%s  ", C(COL_RESET));

    if (!is_dirty) {
        printf("%s✓%s", C(COL_GREEN), C(COL_RESET));
    } else {
        if (r->staged)    printf("%s●%d%s ", C(COL_GREEN),   r->staged,    C(COL_RESET));
        if (r->modified)  printf("%s✗%d%s ", C(COL_RED),     r->modified,  C(COL_RESET));
        if (r->untracked) printf("%s?%d%s",  C(COL_MAGENTA), r->untracked, C(COL_RESET));
    }
    printf("%s\n", EOL());
}

void print_repo(const Repo *r, const ColWidths *w) {
    print_repo_line(r, w, false, 0);
}

/* ── Category header row ───────────────────────────────────────────────────── */
/*
 * Render a collapsible category header. The breadcrumb + count is laid out
 * across the row without widening the NAME column the repos use: its status
 * sits in the STATUS column (aligned with the repos) when the breadcrumb is
 * short enough, and overflows past it — up to the terminal edge, then '~'-cut —
 * when the breadcrumb is long. Status is a single ✓ when every repo is clean and
 * in sync, otherwise per-state repo counts (↑ ahead · ↓ behind · ● dirty).
 */
static void print_group_header(const Group *g, const ColWidths *w, bool selected) {
    const char *arrow = g->expanded ? "\xe2\x96\xbc" : "\xe2\x96\xb6";  /* ▼ / ▶ */

    if (selected) printf("%s\xe2\x96\xb8%s ", C(COL_GREEN), C(COL_RESET));
    else          printf("  ");

    /* repo STATUS column position (cols after the lead) — aligns when it fits */
    int block_w = w->name + 2 + w->branch + 2 + w->sync + 2 + w->time + 2;

    char count[32];
    snprintf(count, sizeof(count), "(%zu)", g->count);
    int count_w   = utf8_width(count);
    int label_min = 2 + 1 + count_w;                /* "▶ " + " " + count */
    int key_w     = utf8_width(g->key);
    int status_w  = group_status_width(g);

    /* status position: aligned at block_w, else just past the label; clamped so
     * the status still fits before the terminal edge (room for a 2-col gap) */
    int status_pos = block_w;
    if (label_min + key_w + 2 > status_pos) status_pos = label_min + key_w + 2;
    int tw = term_width();
    if (tw > 0) {
        int max_pos = tw - 2 /*lead*/ - status_w;
        if (max_pos < label_min + 2) max_pos = label_min + 2;
        if (status_pos > max_pos) status_pos = max_pos;
    }

    /* fit the breadcrumb into status_pos, leaving a 2-col gap before the status */
    int key_budget = (status_pos - 2) - label_min;
    if (key_budget < 1) key_budget = 1;

    const char *key = g->key;
    char kbuf[PATH_MAX];
    if (key_w > key_budget) {                       /* truncate, '~' marks the cut */
        int pw = 0;
        size_t bl = utf8_fit(key, key_budget - 1, &pw);
        memcpy(kbuf, key, bl);
        kbuf[bl] = '~';
        kbuf[bl + 1] = '\0';
        key = kbuf;
        key_w = pw + 1;                             /* prefix columns + the '~' */
    }

    /* cyan + bold: same hue as the repo-name rows, just bold so the folder
     * header stays subtly distinct without standing out too much */
    printf("%s%s%s %s%s %s%s%s",
        C(COL_BOLD), C(COL_CYAN), arrow,
        key, C(COL_RESET),
        C(COL_DIM), count, C(COL_RESET));

    int used = label_min + key_w;                   /* arrow+space, key, space, count */
    for (int i = used; i < status_pos; i++) putchar(' ');

    if (!g->n_dirty && !g->n_ahead && !g->n_behind) {
        printf("%s\xe2\x9c\x93%s", C(COL_GREEN), C(COL_RESET));
    } else {
        if (g->n_ahead)
            printf("%s\xe2\x86\x91%d%s ", C(COL_GREEN),  g->n_ahead,  C(COL_RESET));
        if (g->n_behind)
            printf("%s\xe2\x86\x93%d%s ", C(COL_YELLOW), g->n_behind, C(COL_RESET));
        if (g->n_dirty)
            printf("%s\xe2\x97\x8f%d%s",  C(COL_RED),    g->n_dirty,  C(COL_RESET));
    }

    printf("%s\n", EOL());
}

/* ── Dirty filter ──────────────────────────────────────────────────────────── */
/*
 * A repo is "dirty" (worth showing under --dirty) when it is not both clean
 * and in sync: any staged/modified/untracked files, any ahead/behind commits,
 * or a detached / unborn HEAD (branch rendered as "(...)").
 */
bool repo_is_dirty(const Repo *r) {
    if (r->staged || r->modified || r->untracked) return true;
    if (r->ahead || r->behind)                    return true;
    if (r->branch[0] == '(')                      return true;
    return false;
}

/* ── Summary line ──────────────────────────────────────────────────────────── */
/*
 * Trailing "N repos · N clean · N dirty …" line, counted over every repo
 * regardless of collapse state or the dirty filter. When dirty_only is set,
 * clean+in-sync repos are hidden from the listing but still counted here, and
 * the count of hidden repos is appended as "(N hidden)". Shared by the flat and
 * grouped tables so the wording stays in one place.
 */
static void print_summary(bool dirty_only) {
    int total = 0, clean = 0, dirty = 0, behind = 0, hidden = 0;
    for (size_t i = 0; i < g_repo_count; i++) {
        const Repo *r = &g_repos[i];
        total++;
        if (r->staged || r->modified || r->untracked) dirty++; else clean++;
        if (r->behind > 0) behind++;
        if (dirty_only && !repo_is_dirty(r)) hidden++;
    }

    if (total == 0) {
        printf("  No git repositories found.%s\n", EOL());
        return;
    }
    printf("  %s%d repo%s%s · %s%d clean%s · %s%d dirty%s",
        C(COL_BOLD), total, total == 1 ? "" : "s", C(COL_RESET),
        C(COL_GREEN), clean,  C(COL_RESET),
        C(COL_RED),   dirty,  C(COL_RESET));
    if (behind > 0)
        printf(" · %s%d behind%s", C(COL_YELLOW), behind, C(COL_RESET));
    if (hidden > 0)
        printf(" %s(%d hidden)%s", C(COL_DIM), hidden, C(COL_RESET));
    printf("%s\n", EOL());
}

/* ── Status table ──────────────────────────────────────────────────────────── */
/* basename of a repo path (the segment after the last '/'). */
static const char *path_basename(const char *path) {
    const char *s = strrchr(path, '/');
    return s ? s + 1 : path;
}

/* Order g_repos indices by display name (case-insensitive), scan index as a
 * stable tie-break — matching the watch-mode ordering. */
static int cmp_repo_name_idx(const void *a, const void *b) {
    int ia = *(const int *)a, ib = *(const int *)b;
    int c = strcasecmp(path_basename(g_repos[ia].path),
                       path_basename(g_repos[ib].path));
    return c ? c : (ia - ib);
}

/*
 * Print the header, one row per repo and the trailing summary line.
 * Rows are listed alphabetically by repo name. When dirty_only is set,
 * clean+in-sync repos are hidden from the listing but still counted in the
 * summary, which appends "(N hidden)".
 */
void print_status_table(const ColWidths *w, bool dirty_only) {
    print_header(w);

    /* sorted view over g_repos; falls back to scan order if the alloc fails */
    int *order = malloc(g_repo_count * sizeof(int));
    if (order) {
        for (size_t i = 0; i < g_repo_count; i++) order[i] = (int)i;
        qsort(order, g_repo_count, sizeof(int), cmp_repo_name_idx);
    }

    for (size_t i = 0; i < g_repo_count; i++) {
        const Repo *r = &g_repos[order ? (size_t)order[i] : i];
        if (dirty_only && !repo_is_dirty(r)) continue;
        print_repo(r, w);
    }
    free(order);

    print_separator(w);
    print_summary(dirty_only);
}

/* ── Grouped status table (watch mode) ─────────────────────────────────────── */
/*
 * Like print_status_table, but renders the pre-built visible-row list: flat
 * uncategorized repos first, then a header per category with its repos shown
 * only when expanded. `cursor` is the index into rows of the selected row
 * (-1 for none). The summary line counts every repo, matching the flat table.
 */
void print_grouped_table(const ColWidths *w, bool dirty_only,
                         const Group *groups, size_t ngroups,
                         const VisRow *rows, size_t nrows, int cursor) {
    (void)ngroups;
    print_header(w);

    for (size_t i = 0; i < nrows; i++) {
        bool sel = ((int)i == cursor);
        if (rows[i].kind == ROW_HEADER) {
            print_group_header(&groups[rows[i].group_idx], w, sel);
        } else {
            int indent = rows[i].group_idx > 0 ? 2 : 0;
            print_repo_line(&g_repos[rows[i].repo_idx], w, sel, indent);
        }
    }

    print_separator(w);
    print_summary(dirty_only);   /* counts every repo, independent of collapse */
}

/* ── Switch summary ────────────────────────────────────────────────────────── */
void print_switch_summary(const ColWidths *w) {
    int switched = 0, created = 0, already = 0, not_found = 0, skipped = 0;

    printf("%sSwitched to branch:%s %s%s%s\n\n",
        C(COL_BOLD), C(COL_RESET),
        C(COL_YELLOW), opt_switch_branch, C(COL_RESET));

    for (size_t i = 0; i < g_repo_count; i++) {
        const Repo *r = &g_repos[i];
        const char *name = strrchr(r->path, '/');
        name = name ? name + 1 : r->path;

        /* accumulate counts regardless of verbosity */
        switch (r->switch_result) {
            case SR_SWITCHED:   switched++;   break;
            case SR_CREATED:    created++;    break;
            case SR_ALREADY:    already++;    break;
            case SR_NOT_FOUND:  not_found++;  break;
            case SR_DIRTY:
            case SR_ERROR:      skipped++;    break;
            default: break;
        }

        /* skip uninteresting rows unless -v */
        if (!opt_verbose &&
            (r->switch_result == SR_ALREADY || r->switch_result == SR_NOT_FOUND))
            continue;

        printf("  %s", C(COL_CYAN));
        write_col(name, w->name);
        printf("%s  ", C(COL_RESET));

        switch (r->switch_result) {
            case SR_SWITCHED:
                printf("%s✓ switched%s\n", C(COL_GREEN), C(COL_RESET));
                break;
            case SR_CREATED:
                printf("%s✓ created & switched%s\n", C(COL_GREEN), C(COL_RESET));
                break;
            case SR_ALREADY:
                printf("%s· already on branch%s\n", C(COL_DIM), C(COL_RESET));
                break;
            case SR_DIRTY:
                printf("%s✗ skipped%s  %s", C(COL_RED), C(COL_RESET), C(COL_DIM));
                if (r->staged)   printf("%d staged", r->staged);
                if (r->staged && r->modified) printf(", ");
                if (r->modified) printf("%d modified", r->modified);
                printf("%s\n", C(COL_RESET));
                break;
            case SR_NOT_FOUND:
                printf("%s· branch not found%s\n", C(COL_DIM), C(COL_RESET));
                break;
            case SR_ERROR:
                printf("%s✗ error (checkout failed)%s\n", C(COL_RED), C(COL_RESET));
                break;
            default:
                break;
        }
    }

    printf("\n");
    print_separator(w);
    printf("  switched %s%d%s · already %s%d%s",
        C(COL_GREEN), switched,  C(COL_RESET),
        C(COL_DIM),   already,   C(COL_RESET));
    if (created)
        printf(" · created %s%d%s", C(COL_CYAN), created, C(COL_RESET));
    if (not_found)
        printf(" · not found %s%d%s", C(COL_DIM), not_found, C(COL_RESET));
    if (skipped)
        printf(" · skipped %s%d dirty%s", C(COL_RED), skipped, C(COL_RESET));
    printf("\n\n");
}

/* ── Fetch summary ──────────────────────────────────────────────────────────── */
void print_fetch_summary(const ColWidths *w) {
    int fetched = 0, up_to_date = 0, no_remote = 0, errors = 0;

    printf("%sFetch results:%s\n\n", C(COL_BOLD), C(COL_RESET));

    for (size_t i = 0; i < g_repo_count; i++) {
        const Repo *r = &g_repos[i];
        const char *name = strrchr(r->path, '/');
        name = name ? name + 1 : r->path;

        /* accumulate counts regardless of verbosity */
        switch (r->fetch_result) {
            case FR_FETCHED:      fetched++;    break;
            case FR_UP_TO_DATE:   up_to_date++; break;
            case FR_NO_REMOTE:    no_remote++;  break;
            case FR_ERROR:        errors++;     break;
            default: break;
        }

        /* skip uninteresting rows unless -v */
        if (!opt_verbose &&
            (r->fetch_result == FR_UP_TO_DATE || r->fetch_result == FR_NO_REMOTE))
            continue;

        printf("  %s", C(COL_CYAN));
        write_col(name, w->name);
        printf("%s  ", C(COL_RESET));

        switch (r->fetch_result) {
            case FR_FETCHED:
                printf("%s✓ fetched%s\n", C(COL_GREEN), C(COL_RESET));
                break;
            case FR_UP_TO_DATE:
                printf("%s· up to date%s\n", C(COL_DIM), C(COL_RESET));
                break;
            case FR_NO_REMOTE:
                printf("%s· no remote%s\n", C(COL_DIM), C(COL_RESET));
                break;
            case FR_ERROR:
                printf("%s✗ error%s", C(COL_RED), C(COL_RESET));
                if (r->net_error[0])
                    printf("  %s%s%s", C(COL_DIM), r->net_error, C(COL_RESET));
                printf("\n");
                break;
            default:
                break;
        }
    }

    printf("\n");
    print_separator(w);
    printf("  fetched %s%d%s",
        C(COL_GREEN), fetched, C(COL_RESET));
    if (up_to_date)
        printf(" · up to date %s%d%s", C(COL_DIM), up_to_date, C(COL_RESET));
    if (no_remote)
        printf(" · no remote %s%d%s", C(COL_DIM), no_remote, C(COL_RESET));
    if (errors)
        printf(" · errors %s%d%s", C(COL_RED), errors, C(COL_RESET));
    printf("\n\n");
}

/* ── Pull summary ───────────────────────────────────────────────────────────── */
void print_pull_summary(const ColWidths *w) {
    int pulled = 0, up_to_date = 0, dirty = 0, not_ff = 0, no_remote = 0, errors = 0;

    printf("%sPull results:%s\n\n", C(COL_BOLD), C(COL_RESET));

    for (size_t i = 0; i < g_repo_count; i++) {
        const Repo *r = &g_repos[i];
        const char *name = strrchr(r->path, '/');
        name = name ? name + 1 : r->path;

        /* accumulate counts regardless of verbosity */
        switch (r->pull_result) {
            case PR_PULLED:      pulled++;     break;
            case PR_UP_TO_DATE:  up_to_date++; break;
            case PR_DIRTY:       dirty++;      break;
            case PR_NOT_FF:      not_ff++;     break;
            case PR_NO_REMOTE:   no_remote++;  break;
            case PR_ERROR:       errors++;     break;
            default: break;
        }

        /* skip uninteresting rows unless -v */
        if (!opt_verbose &&
            (r->pull_result == PR_UP_TO_DATE || r->pull_result == PR_NO_REMOTE))
            continue;

        printf("  %s", C(COL_CYAN));
        write_col(name, w->name);
        printf("%s  ", C(COL_RESET));

        switch (r->pull_result) {
            case PR_PULLED:
                printf("%s✓ pulled%s\n", C(COL_GREEN), C(COL_RESET));
                break;
            case PR_UP_TO_DATE:
                printf("%s· up to date%s\n", C(COL_DIM), C(COL_RESET));
                break;
            case PR_DIRTY:
                printf("%s✗ skipped%s  %s(dirty)%s\n",
                    C(COL_RED), C(COL_RESET), C(COL_DIM), C(COL_RESET));
                break;
            case PR_NOT_FF:
                printf("%s· not fast-forward%s\n", C(COL_YELLOW), C(COL_RESET));
                break;
            case PR_NO_REMOTE:
                printf("%s· no remote%s\n", C(COL_DIM), C(COL_RESET));
                break;
            case PR_ERROR:
                printf("%s✗ error%s\n", C(COL_RED), C(COL_RESET));
                break;
            default:
                break;
        }
    }

    printf("\n");
    print_separator(w);
    printf("  pulled %s%d%s",
        C(COL_GREEN), pulled, C(COL_RESET));
    if (up_to_date)
        printf(" · up to date %s%d%s", C(COL_DIM), up_to_date, C(COL_RESET));
    if (dirty)
        printf(" · skipped %s%d dirty%s", C(COL_RED), dirty, C(COL_RESET));
    if (not_ff)
        printf(" · not fast-forward %s%d%s", C(COL_YELLOW), not_ff, C(COL_RESET));
    if (no_remote)
        printf(" · no remote %s%d%s", C(COL_DIM), no_remote, C(COL_RESET));
    if (errors)
        printf(" · errors %s%d%s", C(COL_RED), errors, C(COL_RESET));
    printf("\n\n");
}

/* ── Spinner ────────────────────────────────────────────────────────────────── */
static const char   *SPINNER_FRAMES[] = {
    "⠋", "⠙", "⠹", "⠸", "⠼", "⠴", "⠦", "⠧", "⠇", "⠏"
};
static const int     SPINNER_N = 10;
static _Atomic int   spinner_active = 0;
static pthread_t     spinner_tid;

static void *spinner_run(void *arg) {
    const char *msg = (const char *)arg;
    int i = 0;
    while (atomic_load(&spinner_active)) {
        char buf[512];
        int n = snprintf(buf, sizeof(buf), "\r  %s %s", SPINNER_FRAMES[i % SPINNER_N], msg);
        if (n > 0) {
            size_t len = ((size_t)n >= sizeof(buf)) ? sizeof(buf) - 1 : (size_t)n;
            write(STDOUT_FILENO, buf, len);
        }
        i++;
        usleep(80000);
    }
    /* clear the spinner line — write() is async-signal-safe, no libc locks */
    write(STDOUT_FILENO, "\r\033[K", 4);
    return NULL;
}

void spinner_start(const char *msg) {
    if (!isatty(STDOUT_FILENO) || opt_no_color) return;
    atomic_store(&spinner_active, 1);
    if (pthread_create(&spinner_tid, NULL, spinner_run, (void *)msg) != 0) {
        atomic_store(&spinner_active, 0);
    };
}

void spinner_stop(void) {
    if (!atomic_load(&spinner_active)) return;
    atomic_store(&spinner_active, 0);
    pthread_join(spinner_tid, NULL);
}
