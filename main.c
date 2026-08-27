/*
 * main.c – argument parsing and entry point
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <limits.h>
#include <errno.h>
#include <stdbool.h>
#include <unistd.h>

#include <git2.h>
#include "gitools.h"

/* ── Global options ────────────────────────────────────────────────────────── */
int    opt_max_depth          = 5;
bool   opt_all                = false;
bool   opt_no_color           = false;
bool   opt_verbose            = false;
bool   opt_switch             = false;
char   opt_switch_branch[256] = "";
bool   opt_fetch              = false;
bool   opt_pull               = false;
bool   opt_watch              = false;
int    opt_watch_interval     = 3;
bool   opt_dirty_only         = false;
bool   opt_categories         = true;   /* group repos by folder in watch mode */
char   opt_default_dir[PATH_MAX] = "";
char **opt_extra_skip         = NULL;
size_t opt_extra_skip_count   = 0;

/* ── Git availability check ────────────────────────────────────────────────── */
static int git_installed(void) {
    FILE *f = popen("git --version 2>/dev/null", "r");
    if (!f) return 0;
    char buf[4];
    int found = (fread(buf, 1, 1, f) > 0);
    pclose(f);
    return found;
}

/* ── Helpers ───────────────────────────────────────────────────────────────── */
static bool is_all_digits(const char *s) {
    if (!s || !*s) return false;
    for (const char *p = s; *p; p++)
        if (*p < '0' || *p > '9') return false;
    return true;
}

/* ── Usage ─────────────────────────────────────────────────────────────────── */
/* out is stdout for an explicit -h/--help, stderr when reporting a usage error. */
static void usage(FILE *out, const char *prog) {
    fprintf(out,
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
        "  -w, --watch [n]  Watch mode: refresh the table every n seconds (default: 3)\n"
        "  --dirty      Only list repos that are not both clean and in sync\n"
        "  --no-dirty   Show all repos (overrides dirty_only from the config)\n"
        "  -a           Include hidden directories\n"
        "  -v           Verbose: show all repos in summaries, not just changed ones\n"
        "  --no-color   Disable ANSI colours\n"
        "  --version    Show version\n"
        "  -h, --help   Show this help\n"
        "\n"
        "Config: ~/.gitlsrc (override path with GITLS_CONFIG env var)\n"
        "  default_dir=~/projects\n"
        "  max_depth=3\n"
        "  skip_dirs=build,dist,tmp\n"
        "  watch_interval=5\n"
        "  dirty_only=true\n"
        "  categories=false\n"
        "  no_color=true\n",
        prog);
}

/* ── main ──────────────────────────────────────────────────────────────────── */
int main(int argc, char **argv) {
    /* Force English git output for string matching in do_pull/do_fetch.
     * Set here in the parent before any threads start so the child in
     * run_git_capture only needs dup2+execvp (no libc locks between fork/exec). */
    setenv("LC_ALL", "C", 1);

    /* 1. load config – CLI flags parsed next will override these */
    load_config();

    /* 2. subcommand detection – may appear before or after global flags.
     *    Skip option flags and their argument values (-s <branch>, -d <n>)
     *    so that e.g. "gitls -s fetch" does not misidentify "fetch" as a
     *    subcommand when it is the branch name for -s. */
    int subcommand_idx = -1;
    for (int i = 1; i < argc; i++) {
        if (argv[i][0] == '-') {
            if ((strcmp(argv[i], "-s") == 0 || strcmp(argv[i], "-d") == 0) && i + 1 < argc)
                i++; /* skip the option's value token */
            continue;
        }
        if (strcmp(argv[i], "fetch") == 0) { opt_fetch = true;  subcommand_idx = i; break; }
        if (strcmp(argv[i], "pull")  == 0) { opt_pull  = true;  subcommand_idx = i; break; }
    }

    /* 3. option parsing – skip the subcommand token */
    const char *scan_dir = ".";
    bool user_gave_dir = false;

    for (int i = 1; i < argc; i++) {
        if (i == subcommand_idx) continue;
        if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) {
            usage(stdout, argv[0]);
            return 0;
        } else if (strcmp(argv[i], "--version") == 0) {
            printf("gitls %s\n", VERSION_STRING);
            return 0;
        } else if (strcmp(argv[i], "-v") == 0) {
            opt_verbose = true;
        } else if (strcmp(argv[i], "-a") == 0) {
            opt_all = true;
        } else if (strcmp(argv[i], "--no-color") == 0) {
            opt_no_color = true;
        } else if (strcmp(argv[i], "-d") == 0) {
            if (i + 1 >= argc) { fprintf(stderr, "Error: -d requires a number\n"); return 1; }
            char *end;
            errno = 0;
            long depth = strtol(argv[++i], &end, 10);
            if (*end != '\0' || errno == ERANGE || depth < 0 || depth > INT_MAX) {
                fprintf(stderr, "Error: -d requires a valid non-negative number\n");
                return 1;
            }
            opt_max_depth = (int)depth;
        } else if (strcmp(argv[i], "-w") == 0 || strcmp(argv[i], "--watch") == 0) {
            opt_watch = true;
            /* optional numeric interval immediately following -w */
            if (i + 1 < argc && is_all_digits(argv[i + 1])) {
                char *end;
                errno = 0;
                long iv = strtol(argv[++i], &end, 10);
                if (*end != '\0' || errno == ERANGE || iv < 1 || iv > INT_MAX) {
                    fprintf(stderr, "Error: -w requires a positive number of seconds\n");
                    return 1;
                }
                opt_watch_interval = (int)iv;
            }
        } else if (strcmp(argv[i], "--dirty") == 0) {
            opt_dirty_only = true;
        } else if (strcmp(argv[i], "--no-dirty") == 0) {
            opt_dirty_only = false;   /* explicit CLI opt-out overrides config */
        } else if (strcmp(argv[i], "-s") == 0) {
            if (i + 1 >= argc) { fprintf(stderr, "Error: -s requires a branch name\n"); return 1; }
            opt_switch = true;
            strncpy(opt_switch_branch, argv[++i], sizeof(opt_switch_branch) - 1);
            opt_switch_branch[sizeof(opt_switch_branch) - 1] = '\0';
        } else if (argv[i][0] != '-') {
            scan_dir = argv[i];
            user_gave_dir = true;
        } else {
            fprintf(stderr, "Unknown option: %s\n", argv[i]);
            usage(stderr, argv[0]);
            return 1;
        }
    }

    /* 4. validate subcommand/flag combinations */
    if (opt_pull && opt_switch) {
        fprintf(stderr, "Error: 'pull' and '-s' cannot be combined\n");
        return 1;
    }
    if (opt_watch && (opt_fetch || opt_pull || opt_switch)) {
        fprintf(stderr, "Error: -w cannot be combined with fetch/pull/-s\n");
        return 1;
    }
    if (opt_watch && (!isatty(STDIN_FILENO) || !isatty(STDOUT_FILENO))) {
        fprintf(stderr, "Error: -w requires an interactive terminal on stdin and stdout\n");
        return 1;
    }

    /* 5. apply config default_dir only when the user gave no directory */
    if (opt_default_dir[0] != '\0' && !user_gave_dir)
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
    if (maj < 1 || (maj == 1 && min < 7)) {
        fprintf(stderr, "Error: libgit2 >= 1.7 required, found %d.%d.%d\n",
                maj, min, rev);
        git_libgit2_shutdown();
        return 1;
    }

    /* 6. require git binary for fetch/pull subcommands; resolve its absolute
     *    path here (single-threaded) so run_git_capture can use execve instead
     *    of execvp — execve is async-signal-safe, execvp is not. */
    if ((opt_fetch || opt_pull) && !git_installed()) {
        fprintf(stderr, "Error: 'git' is not installed or not in PATH\n");
        git_libgit2_shutdown();
        return 1;
    }
    if (opt_fetch || opt_pull)
        resolve_git_path();

    /* 7. watch mode runs its own render loop (alternate screen, no spinner)
     *    and only returns once the user quits. Resolve the git binary so the
     *    interactive fetch/pull keys work; status display works without it. */
    if (opt_watch) {
        if (git_installed())
            resolve_git_path();
        run_watch(abs_dir);
        if (opt_extra_skip) {
            for (size_t i = 0; i < opt_extra_skip_count; i++)
                free(opt_extra_skip[i]);
            free(opt_extra_skip);
        }
        git_libgit2_shutdown();
        return 0;
    }

    /* 8. spinner — Phase 1 always shows "Scanning:" (local queries only).
     *    fetch/pull get a second spinner in process_all_repos() once repos
     *    are found.  Switch-only uses "Switching:" since that happens in Phase 1. */
    const char *verb = (opt_switch && !opt_fetch && !opt_pull) ? "Switching:"
                                                               : "Scanning:";

    char spin_label[PATH_MAX + 64];
    snprintf(spin_label, sizeof(spin_label), "%s%s%s %s",
             C(COL_BOLD), verb, C(COL_RESET), abs_dir);
    spinner_start(spin_label);
    find_repos(abs_dir, 0);
    process_all_repos(abs_dir);
    spinner_stop();

    ColWidths w = compute_col_widths(0);

    /* ── status table header ── */
    int tw = term_width();
    printf("%sScanned:%s %s\n\n", C(COL_BOLD), C(COL_RESET),
           tw > 0 ? ellipsize(abs_dir, tw - 10) : abs_dir);

    if (opt_fetch) print_fetch_summary(&w);
    if (opt_pull)  print_pull_summary(&w);
    if (opt_switch) print_switch_summary(&w);

    print_status_table(&w, opt_dirty_only);

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
