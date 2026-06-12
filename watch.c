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

/*
 * Block up to interval_sec seconds, returning 1 if the user asked to quit
 * ('q'/'Q', Ctrl-C byte, or a stop signal) and 0 if the interval elapsed.
 */
static int wait_for_quit(int interval_sec) {
    struct timespec start;
    clock_gettime(CLOCK_MONOTONIC, &start);
    const long deadline_ms = (long)interval_sec * 1000;

    for (;;) {
        if (g_watch_stop) return 1;

        struct timespec now;
        clock_gettime(CLOCK_MONOTONIC, &now);
        long elapsed_ms = (now.tv_sec - start.tv_sec) * 1000
                        + (now.tv_nsec - start.tv_nsec) / 1000000;
        long remaining = deadline_ms - elapsed_ms;
        if (remaining <= 0) return 0;

        struct pollfd pfd = { .fd = STDIN_FILENO, .events = POLLIN, .revents = 0 };
        int rc = poll(&pfd, 1, (int)remaining);
        if (rc < 0) {
            if (errno == EINTR) continue;   /* signal — re-check g_watch_stop */
            return 0;
        }
        if (rc == 0) return 0;              /* timeout — time to rescan */

        if (pfd.revents & POLLIN) {
            char c;
            ssize_t n = read(STDIN_FILENO, &c, 1);
            if (n <= 0) continue;
            if (c == 'q' || c == 'Q' || c == 3 /* Ctrl-C */) return 1;
        }
    }
}

/* ── Footer ─────────────────────────────────────────────────────────────────── */
static void print_footer(const char *abs_dir, int interval_sec) {
    (void)abs_dir;
    char clock[16] = "";
    time_t t = time(NULL);
    struct tm tmv;
    if (localtime_r(&t, &tmv))
        strftime(clock, sizeof(clock), "%H:%M:%S", &tmv);

    printf("\n  %sinterval %ds · last scan %s · q to quit%s\n",
           C(COL_DIM), interval_sec, clock, C(COL_RESET));
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

    while (!g_watch_stop) {
        find_repos(abs_dir, 0);
        process_all_repos(abs_dir);

        ColWidths w = compute_col_widths();

        /* redraw in place: home, draw, then clear any leftover rows below */
        printf(CURSOR_HOME);
        printf("%sScanned:%s %s\n\n", C(COL_BOLD), C(COL_RESET), abs_dir);
        print_status_table(&w, opt_dirty_only);
        print_footer(abs_dir, opt_watch_interval);
        printf(CLEAR_TO_END);
        fflush(stdout);

        free_repo_collection();

        if (wait_for_quit(opt_watch_interval)) break;
    }

    restore_terminal();
}
