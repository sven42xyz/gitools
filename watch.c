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

/*
 * Prompt for a branch name on the current line and read it in raw mode.
 * Enter confirms, Esc / Ctrl-C cancels, Backspace deletes. Returns true with
 * the (non-empty) name in buf, false if cancelled.
 */
static bool read_branch(char *buf, size_t n) {
    size_t len = 0;
    buf[0] = '\0';
    write_seq(CURSOR_SHOW);

    for (;;) {
        printf("\r" CLEAR_TO_END "  %sswitch to branch:%s %s",
               C(COL_BOLD), C(COL_RESET), buf);
        fflush(stdout);

        if (g_watch_stop) { write_seq(CURSOR_HIDE); return false; }

        struct pollfd pfd = { .fd = STDIN_FILENO, .events = POLLIN, .revents = 0 };
        int rc = poll(&pfd, 1, -1);
        if (rc < 0) { if (errno == EINTR) continue; break; }

        char c;
        if (read(STDIN_FILENO, &c, 1) <= 0) continue;
        if (c == '\r' || c == '\n')        { write_seq(CURSOR_HIDE); return len > 0; }
        if (c == 27 /* Esc */ || c == 3)   { write_seq(CURSOR_HIDE); return false; }
        if (c == 127 || c == 8) {           /* Backspace */
            if (len > 0) buf[--len] = '\0';
            continue;
        }
        if (c >= 0x20 && c < 0x7f && len + 1 < n) {
            buf[len++] = c;
            buf[len]   = '\0';
        }
    }
    write_seq(CURSOR_HIDE);
    return false;
}

/* ── Footer ─────────────────────────────────────────────────────────────────── */
static void print_footer(int interval_sec, const char *note) {
    char clock[16] = "";
    time_t t = time(NULL);
    struct tm tmv;
    if (localtime_r(&t, &tmv))
        strftime(clock, sizeof(clock), "%H:%M:%S", &tmv);

    printf("\n  %sf%s fetch · %sp%s pull · %ss%s switch · %sr%s refresh · %sq%s quit\n",
           C(COL_BOLD), C(COL_RESET), C(COL_BOLD), C(COL_RESET),
           C(COL_BOLD), C(COL_RESET), C(COL_BOLD), C(COL_RESET),
           C(COL_BOLD), C(COL_RESET));
    printf("  %sinterval %ds · last scan %s", C(COL_DIM), interval_sec, clock);
    if (note && note[0])
        printf(" · %s", note);
    printf("%s\n", C(COL_RESET));
}

/* ── Public entry point ────────────────────────────────────────────────────── */
void run_watch(const char *abs_dir) {
    write_seq(ALT_SCREEN_ON);
    write_seq(CURSOR_HIDE);
    enter_raw_mode();

    atexit(restore_terminal);
    struct sigaction sa = { 0 };
    sa.sa_handler = on_signal;
    sigaction(SIGINT,  &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);

    /* pending action applied on the next scan: 0 = plain refresh, else a key */
    int  action = 0;
    char branch[256] = "";
    char note[128]   = "";

    while (!g_watch_stop) {
        /* translate a pending action into the globals the scan honours */
        opt_fetch  = (action == 'f');
        opt_pull   = (action == 'p');
        opt_switch = (action == 's');
        if (action == 's') {
            strncpy(opt_switch_branch, branch, sizeof(opt_switch_branch) - 1);
            opt_switch_branch[sizeof(opt_switch_branch) - 1] = '\0';
        }

        /* network/switch actions can take a moment — show immediate feedback */
        if (action) {
            const char *verb = action == 'f' ? "Fetching"
                             : action == 'p' ? "Pulling"
                             :                 "Switching";
            printf(CURSOR_HOME "%sScanned:%s %s\n\n  %s%s…%s\n" CLEAR_TO_END,
                   C(COL_BOLD), C(COL_RESET), abs_dir,
                   C(COL_DIM), verb, C(COL_RESET));
            fflush(stdout);
        }

        find_repos(abs_dir, 0);
        process_all_repos(abs_dir);

        ColWidths w = compute_col_widths();

        /* redraw in place: home, draw, then clear any leftover rows below */
        printf(CURSOR_HOME);
        printf("%sScanned:%s %s\n\n", C(COL_BOLD), C(COL_RESET), abs_dir);
        print_status_table(&w, opt_dirty_only);
        print_footer(opt_watch_interval, note);
        printf(CLEAR_TO_END);
        fflush(stdout);

        free_repo_collection();

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
                if (read_branch(branch, sizeof(branch))) {
                    action = 's';
                    snprintf(note, sizeof(note), "switched to %s", branch);
                }
                break;
            case 'r':
            default:
                note[0] = '\0';   /* immediate refresh */
                break;
        }
    }

    restore_terminal();
}
