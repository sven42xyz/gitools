/*
 * watch.c – live watch mode: re-render the status table in place on an
 * interval using raw ANSI escapes and termios (no ncurses dependency).
 *
 * The terminal is switched to the alternate screen buffer with the cursor
 * hidden and stdin put into cbreak mode so a single 'q' keypress quits.
 * The original terminal state is always restored on exit — including on
 * SIGINT / SIGTERM — via a signal handler that asks the loop to stop and an
 * atexit() fallback.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>   /* strcasecmp */
#include <signal.h>
#include <errno.h>
#include <time.h>
#include <poll.h>
#include <unistd.h>
#include <termios.h>

#include "gitools.h"

/* ── Terminal control sequences ────────────────────────────────────────────── */
#define ALT_SCREEN_ON   "\033[?1049h"   /* enter alternate screen buffer */
#define ALT_SCREEN_OFF  "\033[?1049l"   /* leave alternate screen buffer */
#define CURSOR_HIDE     "\033[?25l"
#define CURSOR_SHOW     "\033[?25h"
#define CURSOR_HOME     "\033[H"
#define CLEAR_TO_END    "\033[J"        /* clear from cursor to end of screen */

/* ── Terminal state ────────────────────────────────────────────────────────── */
static struct termios       g_orig_termios;
static int                  g_termios_saved = 0;
static volatile sig_atomic_t g_watch_stop   = 0;
static volatile sig_atomic_t g_winch        = 0;   /* set by SIGWINCH (terminal resize) */

static void write_seq(const char *s) {
    /* write() is async-signal-safe, so this is callable from restore_terminal */
    ssize_t r = write(STDOUT_FILENO, s, strlen(s));
    (void)r;
}

/* Restore terminal: leave alternate screen, show cursor, reset termios. */
static void restore_terminal(void) {
    if (g_termios_saved) {
        tcsetattr(STDIN_FILENO, TCSANOW, &g_orig_termios);
        g_termios_saved = 0;
    }
    write_seq(CURSOR_SHOW);
    write_seq(ALT_SCREEN_OFF);
}

static void on_signal(int sig) {
    (void)sig;
    g_watch_stop = 1;
}

static void on_winch(int sig) {
    (void)sig;
    g_winch = 1;
}

/* Put stdin into cbreak/raw mode: no canonical line buffering, no echo. */
static void enter_raw_mode(void) {
    if (!isatty(STDIN_FILENO)) return;
    if (tcgetattr(STDIN_FILENO, &g_orig_termios) != 0) return;
    g_termios_saved = 1;

    struct termios raw = g_orig_termios;
    raw.c_lflag &= ~(unsigned)(ICANON | ECHO);
    raw.c_cc[VMIN]  = 0;   /* read() returns immediately with whatever is there */
    raw.c_cc[VTIME] = 0;
    tcsetattr(STDIN_FILENO, TCSANOW, &raw);
}

/* ── Logical keys ──────────────────────────────────────────────────────────── */
typedef enum {
    K_NONE, K_CHAR, K_ENTER, K_ESC, K_TAB, K_BACKSPACE, K_UP, K_DOWN, K_EOF
} Key;

static Key read_key(char *ch);

/* ── Input events ──────────────────────────────────────────────────────────── */
typedef enum {
    EV_TIMEOUT,   /* interval elapsed — refresh */
    EV_QUIT,      /* user asked to quit */
    EV_KEY,       /* a key was pressed (see *key_out / *ch_out) */
    EV_RESIZE,    /* terminal was resized — recompute widths and redraw */
} WatchEvent;

/*
 * Block up to timeout_ms milliseconds. Returns EV_TIMEOUT when it elapses,
 * EV_QUIT on 'q'/'Q'/Ctrl-C or a stop signal, EV_RESIZE on a terminal resize,
 * or EV_KEY with the decoded logical key in *key_out (and the byte in *ch_out
 * for K_CHAR).
 */
static WatchEvent wait_for_event(int timeout_ms, Key *key_out, char *ch_out) {
    struct timespec start;
    clock_gettime(CLOCK_MONOTONIC, &start);
    const long deadline_ms = timeout_ms;

    for (;;) {
        if (g_watch_stop) return EV_QUIT;
        if (g_winch) { g_winch = 0; return EV_RESIZE; }

        struct timespec now;
        clock_gettime(CLOCK_MONOTONIC, &now);
        long elapsed_ms = (now.tv_sec - start.tv_sec) * 1000
                        + (now.tv_nsec - start.tv_nsec) / 1000000;
        long remaining = deadline_ms - elapsed_ms;
        if (remaining <= 0) return EV_TIMEOUT;

        struct pollfd pfd = { .fd = STDIN_FILENO, .events = POLLIN, .revents = 0 };
        int rc = poll(&pfd, 1, (int)remaining);
        if (rc < 0) {
            if (errno == EINTR) continue;   /* signal — re-check g_watch_stop */
            return EV_TIMEOUT;
        }
        if (rc == 0) return EV_TIMEOUT;     /* timeout — time to rescan */

        if (pfd.revents & POLLIN) {
            char c = 0;
            Key k = read_key(&c);
            if (k == K_NONE) continue;
            if (k == K_CHAR && (c == 'q' || c == 'Q')) return EV_QUIT;
            *key_out = k;
            *ch_out  = c;
            return EV_KEY;
        }
    }
}

/* ── Branch picker ─────────────────────────────────────────────────────────── */
#define PICK_VISIBLE 8     /* max branch rows shown at once */

/* Read one logical key, decoding arrow-key CSI sequences (ESC [ A/B). */
static Key read_key(char *ch) {
    char c;
    if (read(STDIN_FILENO, &c, 1) <= 0) return K_NONE;

    if (c == '\r' || c == '\n')      return K_ENTER;
    if (c == '\t')                   return K_TAB;
    if (c == 127 || c == 8)          return K_BACKSPACE;
    if (c == 3)                      return K_ESC;   /* Ctrl-C */
    if (c == 27) {                                    /* ESC or CSI sequence */
        struct pollfd p = { .fd = STDIN_FILENO, .events = POLLIN, .revents = 0 };
        if (poll(&p, 1, 40) > 0) {
            char b1;
            if (read(STDIN_FILENO, &b1, 1) > 0 && b1 == '[') {
                char b2;
                if (read(STDIN_FILENO, &b2, 1) > 0) {
                    if (b2 == 'A') return K_UP;
                    if (b2 == 'B') return K_DOWN;
                }
                return K_NONE;   /* some other CSI sequence — ignore */
            }
        }
        return K_ESC;
    }
    if (c >= 0x20 && c < 0x7f) { *ch = c; return K_CHAR; }
    return K_NONE;
}

/* Case-insensitive substring test. */
static bool ci_contains(const char *hay, const char *needle) {
    if (!*needle) return true;
    for (const char *h = hay; *h; h++) {
        const char *a = h, *b = needle;
        while (*a && *b && (*a | 0x20) == (*b | 0x20)) { a++; b++; }
        if (!*b) return true;
    }
    return false;
}

/* Draw the picker block at the current cursor position; return the number of
 * lines printed so the caller can move the cursor back to the anchor. */
static int draw_picker(const char *buf, const char **filt, size_t nfilt,
                       size_t sel, size_t off) {
    int lines = 0;
    printf("\n");                                                          lines++;
    printf("  %sswitch all clean repos to:%s %s%s%s\xe2\x96\x8f\n",
           C(COL_BOLD), C(COL_RESET), C(COL_CYAN), buf, C(COL_RESET));     lines++;
    printf("  %s\xe2\x86\x91/\xe2\x86\x93 navigate \xc2\xb7 Tab/Enter select \xc2\xb7 "
           "Esc cancel%s\n", C(COL_DIM), C(COL_RESET));                    lines++;

    if (nfilt == 0) {
        printf("  %s(no matching branches \xe2\x80\x94 type a name, Enter to use it)%s\n",
               C(COL_DIM), C(COL_RESET));                                  lines++;
    } else {
        for (size_t i = off; i < nfilt && i < off + PICK_VISIBLE; i++) {
            if (i == sel)
                printf("  %s\xe2\x9d\xb1 %s%s\n", C(COL_GREEN), filt[i], C(COL_RESET));
            else
                printf("    %s%s%s\n", C(COL_DIM), filt[i], C(COL_RESET));
            lines++;
        }
        if (nfilt > off + PICK_VISIBLE) {
            printf("  %s(%zu more)%s\n",
                   C(COL_DIM), nfilt - (off + PICK_VISIBLE), C(COL_RESET));
            lines++;
        }
    }
    return lines;
}

/*
 * Interactive branch picker. Shows the recently-active branches (most recent
 * first) below an editable field; typing filters the list, arrow keys move the
 * selection, Tab/Enter choose it, Esc / Ctrl-C cancel. Free-form names are
 * allowed via the text field. Returns true with the chosen name in out.
 */
static bool read_branch(char *out, size_t n) {
    char buf[256] = "";
    size_t len = 0;

    const char *filt[256];
    size_t nfilt = 0, sel = 0, off = 0;

    for (;;) {
        /* recompute the filtered view from the current text */
        nfilt = 0;
        for (size_t i = 0; i < g_recent_branch_count && nfilt < 256; i++)
            if (g_recent_branches[i] && ci_contains(g_recent_branches[i], buf))
                filt[nfilt++] = g_recent_branches[i];
        if (sel >= nfilt) sel = nfilt ? nfilt - 1 : 0;
        if (sel < off)                  off = sel;
        if (sel >= off + PICK_VISIBLE)  off = sel - PICK_VISIBLE + 1;

        /* redraw in place at the anchor (the line below the footer): clear the
         * picker region, draw it, then move the cursor back up to the anchor */
        printf("\r" CLEAR_TO_END);
        int lines = draw_picker(buf, filt, nfilt, sel, off);
        if (lines > 0) printf("\033[%dA", lines);
        printf("\r");
        fflush(stdout);

        if (g_watch_stop) return false;

        struct pollfd pfd = { .fd = STDIN_FILENO, .events = POLLIN, .revents = 0 };
        int rc = poll(&pfd, 1, -1);
        if (rc < 0) { if (errno == EINTR) continue; return false; }
        if (rc == 0) continue;

        char ch = 0;
        switch (read_key(&ch)) {
            case K_ESC:
                return false;
            case K_UP:
                if (sel > 0) sel--;
                break;
            case K_DOWN:
                if (sel + 1 < nfilt) sel++;
                break;
            case K_TAB:
                if (nfilt > 0) {
                    snprintf(out, n, "%s", filt[sel]);
                    return true;
                }
                break;
            case K_ENTER:
                /* prefer the highlighted match (so typing to filter then Enter
                 * selects it); fall back to the literal text for a new name */
                if (nfilt > 0)  { snprintf(out, n, "%s", filt[sel]); return true; }
                if (len > 0)    { snprintf(out, n, "%s", buf);       return true; }
                break;
            case K_BACKSPACE:
                if (len > 0) buf[--len] = '\0';
                break;
            case K_CHAR:
                if (len + 1 < sizeof(buf)) { buf[len++] = ch; buf[len] = '\0'; }
                break;
            default:
                break;
        }
    }
}

/* ── Footer ─────────────────────────────────────────────────────────────────── */
static void print_footer(int interval_sec, const char *note, bool has_categories) {
    printf("%s\n", EOL());   /* blank separator line (cleared) */

    /* navigation keys only make sense when there are collapsible categories */
    printf("  ");
    if (has_categories)
        printf("%s\xe2\x86\x91/\xe2\x86\x93%s move · %s\xe2\x8f\x8e%s expand · ",
               C(COL_BOLD), C(COL_RESET), C(COL_BOLD), C(COL_RESET));
    printf("%sf%s fetch · %sp%s pull · %ss%s switch · %sr%s refresh · %sq%s quit%s\n",
           C(COL_BOLD), C(COL_RESET), C(COL_BOLD), C(COL_RESET),
           C(COL_BOLD), C(COL_RESET), C(COL_BOLD), C(COL_RESET),
           C(COL_BOLD), C(COL_RESET), EOL());

    printf("  %sinterval %ds", C(COL_DIM), interval_sec);
    if (opt_dirty_only)
        printf(" · dirty only");   /* easy-to-forget filter, surfaced in the live view */
    if (note && note[0])
        printf(" · %s", note);
    printf("%s%s\n", C(COL_RESET), EOL());
}

/* ── Expanded-category set (persists across refreshes) ─────────────────────── */
/*
 * Which category headers are currently expanded, keyed by breadcrumb string.
 * Repos are re-scanned every tick (g_repos is freed and rebuilt), so the
 * collapse state has to live here, outside the per-tick data. Empty by default
 * → every category starts collapsed.
 */
typedef struct { char **keys; size_t count, cap; } KeySet;

static bool keyset_has(const KeySet *s, const char *key) {
    for (size_t i = 0; i < s->count; i++)
        if (strcmp(s->keys[i], key) == 0) return true;
    return false;
}

static void keyset_toggle(KeySet *s, const char *key) {
    for (size_t i = 0; i < s->count; i++) {
        if (strcmp(s->keys[i], key) == 0) {       /* present → remove */
            free(s->keys[i]);
            s->keys[i] = s->keys[--s->count];
            return;
        }
    }
    if (s->count >= s->cap) {                      /* absent → add */
        size_t cap = s->cap ? s->cap * 2 : 8;
        char **tmp = realloc(s->keys, cap * sizeof(char *));
        if (!tmp) return;
        s->keys = tmp;
        s->cap  = cap;
    }
    char *dup = strdup(key);
    if (dup) s->keys[s->count++] = dup;
}

static void keyset_free(KeySet *s) {
    for (size_t i = 0; i < s->count; i++) free(s->keys[i]);
    free(s->keys);
    s->keys = NULL;
    s->count = s->cap = 0;
}

/* ── Per-tick grouping ─────────────────────────────────────────────────────── */
static Group  *g_groups      = NULL;
static size_t  g_group_count = 0;
static size_t  g_group_cap   = 0;
static VisRow *g_rows        = NULL;
static size_t  g_row_count   = 0;
static size_t  g_row_cap     = 0;

static void free_groups(void) {
    for (size_t i = 0; i < g_group_count; i++) free(g_groups[i].repo_idx);
    free(g_groups);
    g_groups = NULL;
    g_group_count = g_group_cap = 0;
}

static Group *find_or_add_group(const char *key) {
    for (size_t i = 0; i < g_group_count; i++)
        if (strcmp(g_groups[i].key, key) == 0) return &g_groups[i];

    if (g_group_count >= g_group_cap) {
        size_t cap = g_group_cap ? g_group_cap * 2 : 8;
        Group *tmp = realloc(g_groups, cap * sizeof(Group));
        if (!tmp) return NULL;
        g_groups = tmp;
        g_group_cap = cap;
    }
    Group *g = &g_groups[g_group_count++];
    memset(g, 0, sizeof(*g));
    strncpy(g->key, key, sizeof(g->key) - 1);
    return g;
}

static void group_push(Group *g, int repo_idx) {
    if (g->count >= g->cap) {
        size_t cap = g->cap ? g->cap * 2 : 8;
        int *tmp = realloc(g->repo_idx, cap * sizeof(int));
        if (!tmp) return;
        g->repo_idx = tmp;
        g->cap = cap;
    }
    g->repo_idx[g->count++] = repo_idx;
}

static const char *repo_basename(int idx) {
    const char *p = strrchr(g_repos[idx].path, '/');
    return p ? p + 1 : g_repos[idx].path;
}

/* Sort repos by display name (case-insensitive), stable on the repo index. */
static int cmp_repo_idx(const void *a, const void *b) {
    int ia = *(const int *)a, ib = *(const int *)b;
    int c = strcasecmp(repo_basename(ia), repo_basename(ib));
    return c ? c : (ia - ib);
}

/*
 * Build the per-tick groups from g_repos. The uncategorized bucket (key "") is
 * always at index 0. dirty_only hides clean repos entirely (they are still
 * counted in the summary). A category holding a single repo is folded back into
 * the flat bucket so a lone repo shows as just its name, not a collapsible
 * header. Repos within each category are sorted alphabetically here; the
 * top-level ordering (flat repos interleaved with category headers) is done in
 * build_visrows. Expand state is applied from the persistent set (the
 * uncategorized bucket is always open).
 */
static void build_groups(const char *abs_dir, bool dirty_only, const KeySet *expanded) {
    free_groups();
    find_or_add_group("");   /* uncategorized bucket at index 0 */

    for (size_t i = 0; i < g_repo_count; i++) {
        Repo *r = &g_repos[i];
        if (dirty_only && !repo_is_dirty(r)) continue;

        char key[PATH_MAX];
        if (opt_categories)
            repo_category(abs_dir, r->path, key, sizeof(key));
        else
            key[0] = '\0';            /* categories off: one flat, sorted bucket */
        Group *g = find_or_add_group(key);
        if (!g) continue;
        group_push(g, (int)i);
        if (r->staged || r->modified || r->untracked) g->n_dirty++;
        if (r->ahead > 0)                             g->n_ahead++;
        if (r->behind > 0)                            g->n_behind++;
    }

    /* fold single-repo categories into the flat bucket, then compact the array */
    Group *flat = &g_groups[0];
    size_t keep = 1;
    for (size_t i = 1; i < g_group_count; i++) {
        if (g_groups[i].count == 1) {
            group_push(flat, g_groups[i].repo_idx[0]);
            free(g_groups[i].repo_idx);
        } else {
            if (keep != i) g_groups[keep] = g_groups[i];   /* moves repo_idx ownership */
            keep++;
        }
    }
    g_group_count = keep;

    /* sort repos within each category; flat-bucket order is set in build_visrows */
    for (size_t i = 0; i < g_group_count; i++)
        qsort(g_groups[i].repo_idx, g_groups[i].count, sizeof(int), cmp_repo_idx);

    for (size_t i = 0; i < g_group_count; i++)
        g_groups[i].expanded = (g_groups[i].key[0] == '\0')
                             ? true : keyset_has(expanded, g_groups[i].key);
}

static void rows_push(RowKind kind, int repo_idx, int group_idx) {
    if (g_row_count >= g_row_cap) {
        size_t cap = g_row_cap ? g_row_cap * 2 : 32;
        VisRow *tmp = realloc(g_rows, cap * sizeof(VisRow));
        if (!tmp) return;
        g_rows = tmp;
        g_row_cap = cap;
    }
    g_rows[g_row_count++] = (VisRow){ .kind = kind, .repo_idx = repo_idx,
                                      .group_idx = group_idx };
}

/*
 * A top-level entry is either a flat (uncategorized) repo or a category header.
 * Both are sorted into one alphabetical sequence by their display key so a
 * category like "core > packages" lands directly under a repo/category "core",
 * rather than in a separate block below all flat repos.
 */
typedef struct {
    const char *key;     /* repo basename, or the category breadcrumb */
    bool        is_group;
    int         idx;     /* repo index (flat) or group index (category) */
} TopEntry;

static int cmp_topentry(const void *a, const void *b) {
    const TopEntry *x = a, *y = b;
    int c = strcasecmp(x->key, y->key);
    if (c) return c;
    if (x->is_group != y->is_group) return x->is_group - y->is_group; /* repo first */
    return x->idx - y->idx;
}

/* Flatten the groups into the selectable visible-row list the renderer and the
 * cursor navigation share: flat repos and category headers interleaved
 * alphabetically, each expanded category followed by its (already sorted) repos. */
static void build_visrows(void) {
    g_row_count = 0;

    Group *u = &g_groups[0];                 /* uncategorized / flat bucket */
    size_t ntop = u->count + (g_group_count - 1);
    if (ntop == 0) return;

    TopEntry *top = malloc(ntop * sizeof(TopEntry));
    if (!top) {                              /* OOM: fall back to flat-then-groups */
        for (size_t j = 0; j < u->count; j++) rows_push(ROW_REPO, u->repo_idx[j], 0);
        for (size_t gi = 1; gi < g_group_count; gi++) {
            rows_push(ROW_HEADER, -1, (int)gi);
            if (g_groups[gi].expanded)
                for (size_t j = 0; j < g_groups[gi].count; j++)
                    rows_push(ROW_REPO, g_groups[gi].repo_idx[j], (int)gi);
        }
        return;
    }

    size_t k = 0;
    for (size_t j = 0; j < u->count; j++)
        top[k++] = (TopEntry){ repo_basename(u->repo_idx[j]), false, u->repo_idx[j] };
    for (size_t gi = 1; gi < g_group_count; gi++)
        top[k++] = (TopEntry){ g_groups[gi].key, true, (int)gi };

    qsort(top, ntop, sizeof(TopEntry), cmp_topentry);

    for (size_t i = 0; i < ntop; i++) {
        if (!top[i].is_group) {
            rows_push(ROW_REPO, top[i].idx, 0);
        } else {
            int gi = top[i].idx;
            rows_push(ROW_HEADER, -1, gi);
            if (g_groups[gi].expanded)
                for (size_t j = 0; j < g_groups[gi].count; j++)
                    rows_push(ROW_REPO, g_groups[gi].repo_idx[j], gi);
        }
    }
    free(top);
}

/* ── Public entry point ────────────────────────────────────────────────────── */
void run_watch(const char *abs_dir) {
    /* install cleanup hooks before touching terminal state, so a signal in the
     * window before/while we switch screens still restores the terminal */
    atexit(restore_terminal);
    struct sigaction sa = { 0 };
    sa.sa_handler = on_signal;
    sigaction(SIGINT,  &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);

    struct sigaction wa = { 0 };
    wa.sa_handler = on_winch;
    sigaction(SIGWINCH, &wa, NULL);

    write_seq(ALT_SCREEN_ON);
    write_seq(CURSOR_HIDE);
    enter_raw_mode();

    /* pending action applied on the next scan: 0 = plain refresh, else a key */
    int    action   = 0;
    char   branch[256] = "";
    char   note[128]   = "";
    KeySet expanded    = { 0 };   /* categories the user has opened */
    int    cursor      = -1;      /* selected visible row, -1 = no highlight yet */

    while (!g_watch_stop) {
        /* free the previous tick's scan here (not before the wait) so g_paths
         * stays available for the branch picker while we wait for input */
        free_repo_collection();

        /* translate a pending action into the globals the scan honours */
        opt_fetch  = (action == 'f');
        opt_pull   = (action == 'p');
        opt_switch = (action == 's');
        if (action == 's') {
            strncpy(opt_switch_branch, branch, sizeof(opt_switch_branch) - 1);
            opt_switch_branch[sizeof(opt_switch_branch) - 1] = '\0';
        }

        /* network/switch actions can take a moment — keep the current table on
         * screen and animate a spinner with the action verb on the line below
         * the footer (the cursor sits there from the previous render). The
         * table is refreshed in place once the action completes. */
        char spinmsg[PATH_MAX + 96];
        bool spinning = false;
        if (action) {
            const char *verb = action == 'f' ? "Fetching"
                             : action == 'p' ? "Pulling"
                             :                 "Switching";
            printf("\r" CLEAR_TO_END);                            /* clear any picker */
            printf("  %s%s…%s", C(COL_DIM), verb, C(COL_RESET));  /* no-color fallback */
            fflush(stdout);

            if (action == 's')
                snprintf(spinmsg, sizeof(spinmsg), "%s%s%s to %s%s%s",
                         C(COL_BOLD), verb, C(COL_RESET),
                         C(COL_CYAN), branch, C(COL_RESET));
            else
                snprintf(spinmsg, sizeof(spinmsg), "%s%s%s all repos",
                         C(COL_BOLD), verb, C(COL_RESET));
            spinner_start(spinmsg);   /* overwrites the fallback line via '\r' */
            spinning = true;
        }

        find_repos(abs_dir, 0);
        process_all_repos(abs_dir);
        if (spinning) spinner_stop();

        /* footer note: report the outcome now that results are in, so a failed
         * action reads "… failed" rather than falsely confirming success */
        if (action) {
            int failed = 0;
            for (size_t i = 0; i < g_repo_count; i++) {
                if (action == 'f' && g_repos[i].fetch_result  == FR_ERROR) failed++;
                if (action == 'p' && g_repos[i].pull_result   == PR_ERROR) failed++;
                if (action == 's' && g_repos[i].switch_result == SR_ERROR) failed++;
            }
            const char *msg = NULL;
            if (action == 'f')      msg = failed ? "fetch failed"  : "fetched";
            else if (action == 'p') msg = failed ? "pull failed"   : "pulled";
            if (msg)
                snprintf(note, sizeof(note), "%s", msg);
            else if (failed)        /* switch */
                snprintf(note, sizeof(note), "switch failed");
            else
                snprintf(note, sizeof(note), "switched to %s", branch);
        }

        /* clear one-shot action state so it does not repeat on the next tick */
        opt_fetch = opt_pull = opt_switch = false;
        action = 0;

        build_groups(abs_dir, opt_dirty_only, &expanded);
        /* size the STATUS column for the widest header aggregate (breadcrumbs
         * don't widen NAME — they span the row at render time) */
        int max_status = 0;
        for (size_t gi = 1; gi < g_group_count; gi++)
            max_status = MAX(max_status, group_status_width(&g_groups[gi]));
        ColWidths w = compute_col_widths(max_status);

        /* inner loop: re-render on cursor moves and expand/collapse without
         * rescanning; break out to rescan on the interval, 'r', or an action */
        struct timespec tick_start;
        clock_gettime(CLOCK_MONOTONIC, &tick_start);
        const long tick_ms = (long)opt_watch_interval * 1000;

        for (;;) {
            build_visrows();
            /* cursor may be -1 (no selection until the first arrow key); compare
             * as int so -1 isn't promoted to a huge size_t and clamped to the end */
            if (g_row_count == 0)                      cursor = -1;
            else if (cursor >= (int)g_row_count)       cursor = (int)g_row_count - 1;

            /* redraw in place: home, draw (each line cleared to its end via EOL
             * so a narrower frame leaves no stale columns), then clear the rest */
            int tw = term_width();
            printf(CURSOR_HOME);
            printf("%sScanned:%s %s%s\n%s\n", C(COL_BOLD), C(COL_RESET),
                   tw > 0 ? ellipsize(abs_dir, tw - 10) : abs_dir, EOL(), EOL());
            print_grouped_table(&w, opt_dirty_only, g_groups, g_group_count,
                                g_rows, g_row_count, cursor);
            print_footer(opt_watch_interval, note, g_group_count > 1);
            printf(CLEAR_TO_END);
            fflush(stdout);

            struct timespec now;
            clock_gettime(CLOCK_MONOTONIC, &now);
            long elapsed = (now.tv_sec - tick_start.tv_sec) * 1000
                         + (now.tv_nsec - tick_start.tv_nsec) / 1000000;
            long remaining = tick_ms - elapsed;
            if (remaining <= 0) { note[0] = '\0'; break; }   /* time to rescan */

            Key  k  = K_NONE;
            char ch = 0;
            WatchEvent ev = wait_for_event((int)remaining, &k, &ch);
            if (ev == EV_QUIT)    goto done;
            if (ev == EV_TIMEOUT) { note[0] = '\0'; break; }
            if (ev == EV_RESIZE) {
                /* resize the columns to the new terminal width without a rescan;
                 * the redraw at the top of the loop picks up the new widths */
                w = compute_col_widths(max_status);
                continue;
            }

            if (k == K_UP) {
                if (cursor < 0)      cursor = (int)g_row_count - 1;  /* enter from bottom */
                else if (cursor > 0) cursor--;
                continue;
            }
            if (k == K_DOWN) {
                if (cursor < 0)                             cursor = 0;  /* enter from top */
                else if ((size_t)cursor + 1 < g_row_count)  cursor++;
                continue;
            }
            if (k == K_ENTER) {
                if (cursor >= 0 && g_rows[cursor].kind == ROW_HEADER) {
                    keyset_toggle(&expanded, g_groups[g_rows[cursor].group_idx].key);
                    /* re-apply expand state in place — no rescan needed */
                    build_groups(abs_dir, opt_dirty_only, &expanded);
                }
                continue;
            }
            if (k == K_CHAR) {
                /* one-shot actions; the footer note is set after the scan
                 * completes so it can reflect success vs failure */
                if (ch == 'f') { action = 'f'; break; }
                if (ch == 'p') { action = 'p'; break; }
                if (ch == 's') {
                    /* g_paths is still populated — gather recent branches for
                     * the picker, then prompt */
                    collect_recent_branches();
                    if (read_branch(branch, sizeof(branch)))
                        action = 's';
                    free_recent_branches();
                    break;
                }
                if (ch == 'r') { note[0] = '\0'; break; }   /* immediate refresh */
            }
            /* any other key (incl. Esc, Tab) — ignore and keep the view */
        }
    }

done:
    free_groups();
    free(g_rows);
    keyset_free(&expanded);
    free_repo_collection();
    free_recent_branches();
    restore_terminal();
}
