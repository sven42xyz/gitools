/*
 * main.c – argument parsing and entry point
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <limits.h>
#include <stdbool.h>

#include <git2.h>
#include "gitools.h"

/* ── Global options ────────────────────────────────────────────────────────── */
int  opt_max_depth          = 5;
bool opt_all                = false;
bool opt_no_color           = false;
bool opt_switch             = false;
char opt_switch_branch[256] = "";

/* ── Usage ─────────────────────────────────────────────────────────────────── */
static void usage(const char *prog) {
    fprintf(stderr,
        "Usage: %s [OPTIONS] [DIRECTORY]\n"
        "\n"
        "Recursively scan DIRECTORY (default: .) for git repositories\n"
        "and display their status.\n"
        "\n"
        "Options:\n"
        "  -s <branch>  Switch all clean repos to <branch> if it exists\n"
        "  -d <n>       Max search depth (default: 5)\n"
        "  -a           Include hidden directories\n"
        "  --no-color   Disable ANSI colours\n"
        "  -h, --help   Show this help\n",
        prog);
}

/* ── main ──────────────────────────────────────────────────────────────────── */
int main(int argc, char **argv) {
    const char *scan_dir = ".";

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) {
            usage(argv[0]);
            return 0;
        } else if (strcmp(argv[i], "-a") == 0) {
            opt_all = true;
        } else if (strcmp(argv[i], "--no-color") == 0) {
            opt_no_color = true;
        } else if (strcmp(argv[i], "-d") == 0) {
            if (i + 1 >= argc) { fprintf(stderr, "Error: -d requires a number\n"); return 1; }
            char *end;
            opt_max_depth = (int)strtol(argv[++i], &end, 10);
            if (*end != '\0') { fprintf(stderr, "Error: -d requires a valid number\n"); return 1; }
            if (opt_max_depth < 0) opt_max_depth = 0;
        } else if (strcmp(argv[i], "-s") == 0) {
            if (i + 1 >= argc) { fprintf(stderr, "Error: -s requires a branch name\n"); return 1; }
            opt_switch = true;
            strncpy(opt_switch_branch, argv[++i], sizeof(opt_switch_branch)-1);
        } else if (argv[i][0] != '-') {
            scan_dir = argv[i];
        } else {
            fprintf(stderr, "Unknown option: %s\n", argv[i]);
            usage(argv[0]);
            return 1;
        }
    }

    char abs_dir[PATH_MAX];
    if (realpath(scan_dir, abs_dir) == NULL) {
        fprintf(stderr, "Error: cannot resolve path '%s'\n", scan_dir);
        free(g_repos);
        return 1;
    }

    git_libgit2_init();
    find_repos(abs_dir, 0);

    /* ── status table header (always shown first) ── */
    printf("%sScanning:%s %s\n\n", C(COL_BOLD), C(COL_RESET), abs_dir);

    if (opt_switch) print_switch_summary();

    print_header();

    int total = 0, clean = 0, dirty = 0, behind = 0;
    for (size_t i = 0; i < g_repo_count; i++) {
        const Repo *r = &g_repos[i];
        print_repo(r);
        total++;
        if (r->staged || r->modified || r->untracked) dirty++; else clean++;
        if (r->behind > 0) behind++;
    }

    printf("  %s%s%s\n", C(COL_DIM), SEP_LINE, C(COL_RESET));
    if (total == 0) {
        printf("  No git repositories found.\n");
    } else {
        printf("  %s%d repo%s%s · %s%d clean%s · %s%d dirty%s",
            C(COL_BOLD), total, total == 1 ? "" : "s", C(COL_RESET),
            C(COL_GREEN), clean,  C(COL_RESET),
            C(COL_RED),   dirty,  C(COL_RESET));
        if (behind > 0)
            printf(" · %s%d behind%s", C(COL_YELLOW), behind, C(COL_RESET));
        printf("\n");
    }

    free(g_repos);
    git_libgit2_shutdown();
    return 0;
}
