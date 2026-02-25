/*
 * gitools.h – shared types, globals and prototypes
 */
#pragma once

#include <limits.h>
#include <stdbool.h>
#include <stddef.h>
#include <git2.h>

/* ── ANSI colours ──────────────────────────────────────────────────────────── */
#define COL_RESET   "\033[0m"
#define COL_CYAN    "\033[36m"
#define COL_YELLOW  "\033[33m"
#define COL_GREEN   "\033[32m"
#define COL_RED     "\033[31m"
#define COL_MAGENTA "\033[35m"
#define COL_BOLD    "\033[1m"
#define COL_DIM     "\033[2m"

/* ── Utilities ─────────────────────────────────────────────────────────────── */
#ifndef MAX
#define MAX(a, b) ((a) > (b) ? (a) : (b))
#endif

/* ── Dynamic column widths ─────────────────────────────────────────────────── */
typedef struct {
    int name;
    int branch;
    int sync;
    int time;
} ColWidths;

/* ── Switch result ─────────────────────────────────────────────────────────── */
typedef enum {
    SR_NA = 0,
    SR_SWITCHED,
    SR_ALREADY,
    SR_DIRTY,       /* skipped: staged or modified changes */
    SR_NOT_FOUND,   /* branch doesn't exist in this repo */
    SR_ERROR,       /* checkout or ref operation failed */
} SwitchResult;

/* ── Fetch result ──────────────────────────────────────────────────────────── */
typedef enum {
    FR_NA = 0,
    FR_FETCHED,
    FR_UP_TO_DATE,
    FR_NO_REMOTE,
    FR_ERROR,
} FetchResult;

/* ── Pull result ───────────────────────────────────────────────────────────── */
typedef enum {
    PR_NA = 0,
    PR_PULLED,
    PR_UP_TO_DATE,
    PR_NOT_FF,      /* diverged, can't fast-forward */
    PR_DIRTY,       /* skipped: staged or modified */
    PR_NO_REMOTE,
    PR_ERROR,
} PullResult;

/* ── Repo ──────────────────────────────────────────────────────────────────── */
typedef struct {
    char         path[PATH_MAX];
    char         branch[256];
    int          staged;
    int          modified;
    int          untracked;
    size_t       ahead;
    size_t       behind;
    int          has_remote;
    git_time_t   last_commit;
    SwitchResult switch_result;
    FetchResult  fetch_result;
    PullResult   pull_result;
} Repo;

/* ── Global options (defined in main.c) ───────────────────────────────────── */
extern int    opt_max_depth;
extern bool   opt_all;
extern bool   opt_no_color;
extern bool   opt_switch;
extern char   opt_switch_branch[256];
extern bool   opt_fetch;
extern bool   opt_pull;
extern char   opt_default_dir[PATH_MAX];
extern char **opt_extra_skip;
extern size_t opt_extra_skip_count;

/* ── Repo collection (defined in repo.c) ──────────────────────────────────── */
extern Repo  *g_repos;
extern size_t g_repo_count;
extern char **g_paths;
extern size_t g_path_count;

/* ── Function prototypes ───────────────────────────────────────────────────── */

/* config.c */
void load_config(void);

/* repo.c */
void collect_path(const char *path);
void process_all_repos(void);

/* display.c */
const char *C(const char *color);
int         utf8_width(const char *s);
const char *relative_time(git_time_t t);
void        write_col(const char *s, int width);
ColWidths   compute_col_widths(void);
void        print_separator(const ColWidths *w);
void        print_header(const ColWidths *w);
void        print_repo(const Repo *r, const ColWidths *w);
void        print_switch_summary(const ColWidths *w);
void        print_fetch_summary(const ColWidths *w);
void        print_pull_summary(const ColWidths *w);
void        spinner_start(const char *msg);
void        spinner_stop(void);

/* scan.c */
void find_repos(const char *path, int depth);
