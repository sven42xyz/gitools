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

/* ── Input events ──────────────────────────────────────────────────────────── */
typedef enum {
    EV_TIMEOUT,   /* interval elapsed — refresh */
    EV_QUIT,      /* user asked to quit */
    EV_KEY,       /* a key was pressed (see *key_out) */
} WatchEvent;

/*
 * Block up to interval_sec seconds. Returns EV_TIMEOUT when the interval
 * elapses, EV_QUIT on 'q'/'Q'/Ctrl-C or a stop signal, or EV_KEY (with the
 * byte stored in *key_out) for any other keypress.
 */
static WatchEvent wait_for_event(int interval_sec, char *key_out) {
    struct timespec start;
    clock_gettime(CLOCK_MONOTONIC, &start);
    const long deadline_ms = (long)interval_sec * 1000;

    for (;;) {
        if (g_watch_stop) return EV_QUIT;

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
            char c;
            ssize_t n = read(STDIN_FILENO, &c, 1);
            if (n <= 0) continue;
            if (c == 'q' || c == 'Q' || c == 3 /* Ctrl-C */) return EV_QUIT;
            *key_out = c;
            return EV_KEY;
        }
    }
}

/* ── Branch picker ─────────────────────────────────────────────────────────── */
#define PICK_VISIBLE 8     /* max branch rows shown at once */

typedef enum {
    K_NONE, K_CHAR, K_ENTER, K_ESC, K_TAB, K_BACKSPACE, K_UP, K_DOWN, K_EOF
} Key;

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
static void print_footer(const char *abs_dir, int interval_sec, const char *note) {
    printf("%s\n", EOL());   /* blank separator line (cleared) */
    printf("  %sf%s fetch · %sp%s pull · %ss%s switch · %sr%s refresh · %sq%s quit%s\n",
           C(COL_BOLD), C(COL_RESET), C(COL_BOLD), C(COL_RESET),
           C(COL_BOLD), C(COL_RESET), C(COL_BOLD), C(COL_RESET),
           C(COL_BOLD), C(COL_RESET), EOL());
    int tw = term_width();
    const char *dir = (tw > 0) ? ellipsize(abs_dir, tw - 18) : abs_dir;
    printf("  %sinterval %ds · %s", C(COL_DIM), interval_sec, dir);
    if (note && note[0])
        printf(" · %s", note);
    printf("%s%s\n", C(COL_RESET), EOL());
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

    write_seq(ALT_SCREEN_ON);
    write_seq(CURSOR_HIDE);
    enter_raw_mode();

    /* pending action applied on the next scan: 0 = plain refresh, else a key */
    int  action = 0;
    char branch[256] = "";
    char note[128]   = "";

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

        ColWidths w = compute_col_widths();

        /* redraw in place: home, draw (each line cleared to its end via EOL so
         * a narrower frame leaves no stale columns), then clear leftover rows */
        int tw = term_width();
        printf(CURSOR_HOME);
        printf("%sScanned:%s %s%s\n%s\n", C(COL_BOLD), C(COL_RESET),
               tw > 0 ? ellipsize(abs_dir, tw - 10) : abs_dir, EOL(), EOL());
        print_status_table(&w, opt_dirty_only);
        print_footer(abs_dir, opt_watch_interval, note);
        printf(CLEAR_TO_END);
        fflush(stdout);

        /* clear one-shot action state so it does not repeat on the next tick */
        opt_fetch = opt_pull = opt_switch = false;
        action = 0;

        char key = 0;
        WatchEvent ev = wait_for_event(opt_watch_interval, &key);
        if (ev == EV_QUIT)    break;
        if (ev == EV_TIMEOUT) { note[0] = '\0'; continue; }

        switch (key) {
            case 'f':
                action = 'f';
                snprintf(note, sizeof(note), "fetched");
                break;
            case 'p':
                action = 'p';
                snprintf(note, sizeof(note), "pulled");
                break;
            case 's':
                /* g_paths is still populated — gather recent branches for the
                 * picker, then prompt */
                collect_recent_branches();
                if (read_branch(branch, sizeof(branch))) {
                    action = 's';
                    snprintf(note, sizeof(note), "switched to %s", branch);
                }
                free_recent_branches();
                break;
            case 'r':
            default:
                note[0] = '\0';   /* immediate refresh */
                break;
        }
    }

    free_repo_collection();
    free_recent_branches();
    restore_terminal();
}
