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
    SR_CREATED,     /* created local tracking branch from origin/<branch> */
    SR_ALREADY,
    SR_DIRTY,       /* skipped: staged or modified changes */
    SR_NOT_FOUND,   /* branch doesn't exist locally or on origin */
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

/* ── Upstream state (repo.c upstream_ahead_behind) ─────────────────────────── */
typedef enum {
    UP_NONE = 0,    /* no upstream configured */
    UP_GONE,        /* configured, but the remote-tracking ref no longer exists */
    UP_OK,
} UpstreamState;

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
    char         net_error[256];   /* libgit2 error message on fetch/pull failure */
} Repo;

/* ── Branch list / stale detection (stale.c) ───────────────────────────────── */
typedef enum {
    STR_NONE = 0,   /* not stale */
    STR_GONE,       /* upstream deleted on the remote; work not provably merged */
    STR_MERGED,     /* tip reachable from the default branch */
    STR_SQUASHED,   /* cumulative diff matches a default-branch commit */
} StaleReason;

typedef struct {
    char        name[256];
    char        upstream[256];   /* short upstream name, "" = none configured */
    bool        upstream_gone;
    bool        is_head;
    bool        is_default;
    bool        is_protected;    /* main/master or protected_branches */
    size_t      ahead;           /* vs upstream (0/0 when none or gone) */
    size_t      behind;
    git_time_t  last_commit;
    git_oid     tip;             /* commit the classification was made for */
    StaleReason reason;
} BranchInfo;

typedef enum {
    DEL_OK = 0,
    DEL_CHANGED,    /* tip moved since the analysis — reload before deleting */
    DEL_REFUSED,    /* checked-out, default or protected branch */
    DEL_ERROR,
} DeleteResult;

typedef struct {
    char        default_branch[256];   /* "" = could not be determined */
    BranchInfo *branches;
    size_t      count;
} BranchList;

/* ── Watch-mode grouping ───────────────────────────────────────────────────── */
/*
 * Repos are bucketed by their category (breadcrumb relative to the scan root,
 * see repo_category). The bucket with key "" holds uncategorized repos (direct
 * children of the root) and is rendered flat; every other bucket gets a
 * collapsible header row.
 */
typedef struct {
    char    key[PATH_MAX];   /* "core > packages"; "" = uncategorized */
    int    *repo_idx;        /* indices into g_repos */
    size_t  count;
    size_t  cap;
    int     n_dirty;         /* repos with staged/modified/untracked changes */
    int     n_ahead;         /* repos ahead of their remote */
    int     n_behind;        /* repos behind their remote */
    bool    expanded;
} Group;

typedef enum { ROW_REPO, ROW_HEADER } RowKind;
typedef struct {
    RowKind kind;
    int     repo_idx;        /* valid for ROW_REPO */
    int     group_idx;       /* group the row belongs to (0 = uncategorized) */
} VisRow;

/* ── Global options (defined in main.c) ───────────────────────────────────── */
extern int    opt_max_depth;
extern bool   opt_all;
extern bool   opt_no_color;
extern bool   opt_verbose;
extern bool   opt_switch;
extern char   opt_switch_branch[256];
extern bool   opt_fetch;
extern bool   opt_pull;
extern bool   opt_watch;
extern int    opt_watch_interval;
extern bool   opt_dirty_only;
extern bool   opt_categories;
extern char   opt_default_dir[PATH_MAX];
extern char **opt_extra_skip;
extern size_t opt_extra_skip_count;
extern char **opt_protected_branches;
extern size_t opt_protected_branches_count;

/* ── Repo collection (defined in repo.c) ──────────────────────────────────── */
extern Repo  *g_repos;
extern size_t g_repo_count;
extern char **g_paths;
extern size_t g_path_count;
extern char **g_recent_branches;
extern size_t g_recent_branch_count;

/* ── Function prototypes ───────────────────────────────────────────────────── */

/* config.c */
void load_config(void);
void free_list(char **list, size_t count);

/* repo.c */
UpstreamState upstream_ahead_behind(git_repository *repo, const char *refname,
                                    const git_oid *tip, char *up_name, size_t n,
                                    size_t *ahead, size_t *behind);
void resolve_git_path(void);
int  git_available(void);
void collect_path(const char *path);
void repo_category(const char *abs_dir, const char *repo_path,
                   char *out, size_t n);
void process_all_repos(const char *dir);
void free_repo_collection(void);
void collect_recent_branches(void);
void free_recent_branches(void);

/* display.c */
const char *C(const char *color);
const char *EOL(void);
int         term_width(void);
const char *ellipsize(const char *s, int max_w);
int         utf8_width(const char *s);
const char *relative_time(git_time_t t);
void        write_col(const char *s, int width);
ColWidths   compute_col_widths(int min_status_w);
int         group_status_width(const Group *g);
void        print_separator(const ColWidths *w);
void        print_header(const ColWidths *w);
void        print_repo(const Repo *r, const ColWidths *w);
bool        repo_is_dirty(const Repo *r);
void        print_status_table(const ColWidths *w, bool dirty_only);
void        print_grouped_table(const ColWidths *w, bool dirty_only,
                                const Group *groups, size_t ngroups,
                                const VisRow *rows, size_t nrows, int cursor);
int         branch_screen_rows(void);
void        print_branch_screen(const char *repo_path, const BranchList *bl,
                                const bool *marked, int cursor, int offset,
                                int visible, const char *note, const char *prompt);
void        print_switch_summary(const ColWidths *w);
void        print_fetch_summary(const ColWidths *w);
void        print_pull_summary(const ColWidths *w);
void        spinner_start(const char *msg);
void        spinner_stop(void);

/* stale.c */
int         repo_branch_list(const char *path, char **protected_list, size_t nprot,
                             BranchList *out);
void        branch_list_free(BranchList *bl);
const char *branch_exempt_reason(const BranchInfo *b);
bool        branch_safe(const BranchInfo *b);
DeleteResult repo_delete_branch(const char *path, const BranchInfo *b);

/* scan.c */
void find_repos(const char *path, int depth);

/* watch.c */
void run_watch(const char *abs_dir);
