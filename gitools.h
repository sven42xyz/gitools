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

/* ── Column widths (display chars) ────────────────────────────────────────── */
#define COL_NAME   28
#define COL_BRANCH 30
#define COL_SYNC    7
#define COL_TIME   14
#define SEP_LINE \
    "────────────────────────────────────────────────────────────────────────────────────"

/* ── Switch result ─────────────────────────────────────────────────────────── */
typedef enum {
    SR_NA = 0,
    SR_SWITCHED,
    SR_ALREADY,
    SR_DIRTY,       /* skipped: staged or modified changes */
    SR_NOT_FOUND,   /* branch doesn't exist in this repo */
    SR_ERROR,       /* checkout or ref operation failed */
} SwitchResult;

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
} Repo;

/* ── Global options (defined in main.c) ───────────────────────────────────── */
extern int  opt_max_depth;
extern bool opt_all;
extern bool opt_no_color;
extern bool opt_switch;
extern char opt_switch_branch[256];

/* ── Repo collection (defined in repo.c) ──────────────────────────────────── */
extern Repo  *g_repos;
extern size_t g_repo_count;

/* ── Function prototypes ───────────────────────────────────────────────────── */

/* repo.c */
void collect_repo(const Repo *r);
void process_repo(const char *path);

/* display.c */
const char *C(const char *color);
int         utf8_width(const char *s);
const char *relative_time(git_time_t t);
void        write_col(const char *s, int width);
void        print_header(void);
void        print_repo(const Repo *r);
void        print_switch_summary(void);

/* scan.c */
void find_repos(const char *path, int depth);
