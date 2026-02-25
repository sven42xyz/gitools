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
int    opt_max_depth          = 5;
bool   opt_all                = false;
bool   opt_no_color           = false;
bool   opt_switch             = false;
char   opt_switch_branch[256] = "";
bool   opt_fetch              = false;
bool   opt_pull               = false;
char   opt_default_dir[PATH_MAX] = "";
char **opt_extra_skip         = NULL;
size_t opt_extra_skip_count   = 0;

/* ── Usage ─────────────────────────────────────────────────────────────────── */
static void usage(const char *prog) {
    fprintf(stderr,
        "Usage: %s [fetch|pull] [OPTIONS] [DIRECTORY]\n"
        "\n"
        "Recursively scan DIRECTORY (default: .) for git repositories\n"
        "and display their status.\n"
        "\n"
        "Subcommands:\n"
        "  fetch        Fetch all repos from their remote\n"
        "  pull         Fast-forward pull all clean repos\n"
        "\n"
        "Options:\n"
        "  -s <branch>  Switch all clean repos to <branch> if it exists\n"
        "  -d <n>       Max search depth (default: 5)\n"
        "  -a           Include hidden directories\n"
        "  --no-color   Disable ANSI colours\n"
        "  --version    Show version\n"
        "  -h, --help   Show this help\n"
        "\n"
        "Config: ~/.gitlsrc (override path with GITLS_CONFIG env var)\n"
        "  default_dir=~/projects\n"
        "  max_depth=3\n"
        "  skip_dirs=build,dist,tmp\n"
        "  no_color=true\n",
        prog);
}

/* ── main ──────────────────────────────────────────────────────────────────── */
int main(int argc, char **argv) {
    /* 1. load config – CLI flags parsed next will override these */
    load_config();

    /* 2. subcommand detection – may appear before or after global flags */
    int subcommand_idx = -1;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "fetch") == 0) { opt_fetch = true;  subcommand_idx = i; break; }
        if (strcmp(argv[i], "pull")  == 0) { opt_pull  = true;  subcommand_idx = i; break; }
    }

    /* 3. option parsing – skip the subcommand token */
    const char *scan_dir = ".";

    for (int i = 1; i < argc; i++) {
        if (i == subcommand_idx) continue;
        if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) {
            usage(argv[0]);
            return 0;
        } else if (strcmp(argv[i], "--version") == 0) {
            printf("gitls %s\n", VERSION_STRING);
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
            strncpy(opt_switch_branch, argv[++i], sizeof(opt_switch_branch) - 1);
        } else if (argv[i][0] != '-') {
            scan_dir = argv[i];
        } else {
            fprintf(stderr, "Unknown option: %s\n", argv[i]);
            usage(argv[0]);
            return 1;
        }
    }

    /* 4. apply config default_dir only when the user gave no directory */
    if (opt_default_dir[0] != '\0' && strcmp(scan_dir, ".") == 0)
        scan_dir = opt_default_dir;

    char abs_dir[PATH_MAX];
    if (realpath(scan_dir, abs_dir) == NULL) {
        fprintf(stderr, "Error: cannot resolve path '%s'\n", scan_dir);
        return 1;
    }

    if (git_libgit2_init() < 0) {
        const git_error *e = git_error_last();
        fprintf(stderr, "Error: failed to initialize libgit2: %s\n",
                e ? e->message : "(unknown)");
        return 1;
    }

    int maj, min, rev;
    git_libgit2_version(&maj, &min, &rev);
    if (maj < 1) {
        fprintf(stderr, "Error: libgit2 >= 1.0 required, found %d.%d.%d\n",
                maj, min, rev);
        git_libgit2_shutdown();
        return 1;
    }

    /* 5. spinner */
    const char *verb = opt_fetch    ? "Fetching:"
                     : opt_pull     ? "Pulling:"
                     : opt_switch   ? "Switching:"
                     :                "Scanning:";

    char spin_label[PATH_MAX + 16];
    snprintf(spin_label, sizeof(spin_label), "%s%s%s %s",
             C(COL_BOLD), verb, C(COL_RESET), abs_dir);
    spinner_start(spin_label);
    find_repos(abs_dir, 0);
    process_all_repos();
    spinner_stop();

    ColWidths w = compute_col_widths();

    /* ── status table header ── */
    printf("%sScanned:%s %s\n\n", C(COL_BOLD), C(COL_RESET), abs_dir);

    if (opt_fetch) print_fetch_summary(&w);
    if (opt_pull)  print_pull_summary(&w);
    if (opt_switch) print_switch_summary(&w);

    print_header(&w);

    int total = 0, clean = 0, dirty = 0, behind = 0;
    for (size_t i = 0; i < g_repo_count; i++) {
        const Repo *r = &g_repos[i];
        print_repo(r, &w);
        total++;
        if (r->staged || r->modified || r->untracked) dirty++; else clean++;
        if (r->behind > 0) behind++;
    }

    print_separator(&w);
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

    /* cleanup */
    for (size_t i = 0; i < g_path_count; i++)
        free(g_paths[i]);
    free(g_paths);
    free(g_repos);
    if (opt_extra_skip) {
        for (size_t i = 0; i < opt_extra_skip_count; i++)
            free(opt_extra_skip[i]);
        free(opt_extra_skip);
    }
    git_libgit2_shutdown();
    return 0;
}
