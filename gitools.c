/*
 * gitools.c – Git Repository Status Scanner
 * Uses libgit2 to recursively scan directories for git repos and display status.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <dirent.h>
#include <sys/stat.h>
#include <limits.h>

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
#define COL_SYNC    7   /* enough for ↑99↓99 */
#define COL_TIME   14
#define SEP_LINE \
    "────────────────────────────────────────────────────────────────────────────────────"

/* ── Data ──────────────────────────────────────────────────────────────────── */
typedef struct {
    char       path[PATH_MAX];
    char       branch[256];
    int        staged;
    int        modified;
    int        untracked;
    size_t     ahead;
    size_t     behind;
    int        has_remote;
    git_time_t last_commit;   /* 0 = no commit yet */
} Repo;

/* ── Global options ────────────────────────────────────────────────────────── */
static int   opt_max_depth  = 5;
static int   opt_all        = 0;   /* include hidden dirs */
static int   opt_no_color   = 0;

/* ── Stats ─────────────────────────────────────────────────────────────────── */
static int   total_repos    = 0;
static int   clean_repos    = 0;
static int   dirty_repos    = 0;
static int   behind_repos   = 0;

/* ── Helpers ───────────────────────────────────────────────────────────────── */
static const char *C(const char *color) {
    return opt_no_color ? "" : color;
}

static const char *relative_time(git_time_t t) {
    static char buf[64];
    if (t == 0) { snprintf(buf, sizeof(buf), "no commits"); return buf; }

    time_t now  = time(NULL);
    long   diff = (long)(now - (time_t)t);
    if (diff < 0) diff = 0;

    if (diff < 60)             snprintf(buf, sizeof(buf), "just now");
    else if (diff < 3600)      snprintf(buf, sizeof(buf), "%ld min ago",    diff/60);
    else if (diff < 86400)     snprintf(buf, sizeof(buf), "%ld hour%s ago", diff/3600, diff/3600==1?"":"s");
    else if (diff < 2592000)   snprintf(buf, sizeof(buf), "%ld day%s ago",  diff/86400, diff/86400==1?"":"s");
    else if (diff < 31536000)  snprintf(buf, sizeof(buf), "%ld mo ago",     diff/2592000);
    else                       snprintf(buf, sizeof(buf), "%ld yr ago",     diff/31536000);
    return buf;
}

/* Count display columns of a UTF-8 string (1 codepoint = 1 column). */
static int utf8_width(const char *s) {
    int w = 0;
    for (const unsigned char *p = (const unsigned char *)s; *p; ) {
        if      (*p < 0x80) { p += 1; }
        else if (*p < 0xE0) { p += 2; }
        else if (*p < 0xF0) { p += 3; }
        else                { p += 4; }
        w++;
    }
    return w;
}

/* Print s left-aligned in a field of `width` display columns.
 * Truncates with '~' if too long (assumes s is mostly ASCII). */
static void write_col(const char *s, int width) {
    int len = (int)strlen(s);
    if (len <= width) {
        printf("%-*s", width, s);
    } else {
        printf("%.*s~", width - 1, s);
    }
}

/* Print the sync indicator left-aligned in COL_SYNC display columns. */
static void write_sync(const Repo *r) {
    char plain[32];
    const char *color;

    if (!r->has_remote) {
        snprintf(plain, sizeof(plain), "?");
        color = COL_DIM;
    } else if (r->ahead && r->behind) {
        snprintf(plain, sizeof(plain), "\xe2\x86\x91%zu\xe2\x86\x93%zu", r->ahead, r->behind); /* ↑N↓M */
        color = COL_MAGENTA;
    } else if (r->ahead) {
        snprintf(plain, sizeof(plain), "\xe2\x86\x91%zu", r->ahead);   /* ↑N */
        color = COL_GREEN;
    } else if (r->behind) {
        snprintf(plain, sizeof(plain), "\xe2\x86\x93%zu", r->behind);  /* ↓N */
        color = COL_RED;
    } else {
        snprintf(plain, sizeof(plain), "\xe2\x89\xa1");                 /* ≡  */
        color = COL_DIM;
    }

    int dw = utf8_width(plain);
    printf("%s%s%s%-*s", C(color), plain, C(COL_RESET), COL_SYNC - dw, "");
}

/* ── Table header ──────────────────────────────────────────────────────────── */
static void print_header(void) {
    printf("  %s%-*s  %-*s  %-*s  %-*s  %s%s\n",
        C(COL_DIM),
        COL_NAME,   "NAME",
        COL_BRANCH, "BRANCH",
        COL_SYNC,   "SYNC",
        COL_TIME,   "WHEN",
        "STATUS",
        C(COL_RESET));
    printf("  %s%s%s\n", C(COL_DIM), SEP_LINE, C(COL_RESET));
}

/* ── libgit2 queries ───────────────────────────────────────────────────────── */

static void fill_branch(Repo *r, git_repository *repo) {
    /* unborn branch (no commits yet) */
    if (git_repository_head_unborn(repo) == 1) {
        strncpy(r->branch, "(unborn)", sizeof(r->branch)-1);
        return;
    }

    /* detached HEAD */
    if (git_repository_head_detached(repo) == 1) {
        git_reference *head = NULL;
        if (git_repository_head(&head, repo) == 0) {
            git_object *obj = NULL;
            if (git_reference_peel(&obj, head, GIT_OBJECT_COMMIT) == 0) {
                const git_oid *oid = git_object_id(obj);
                char hex[8];
                git_oid_tostr(hex, sizeof(hex), oid);
                snprintf(r->branch, sizeof(r->branch), "(%s)", hex);
                git_object_free(obj);
            } else {
                strncpy(r->branch, "(detached)", sizeof(r->branch)-1);
            }
            git_reference_free(head);
        } else {
            strncpy(r->branch, "(detached)", sizeof(r->branch)-1);
        }
        return;
    }

    /* normal branch */
    git_reference *head = NULL;
    if (git_repository_head(&head, repo) != 0) {
        strncpy(r->branch, "(?)", sizeof(r->branch)-1);
        return;
    }
    strncpy(r->branch, git_reference_shorthand(head), sizeof(r->branch)-1);
    r->branch[sizeof(r->branch)-1] = '\0';
    git_reference_free(head);
}

static void fill_status(Repo *r, git_repository *repo) {
    git_status_options opts = {
        .version = GIT_STATUS_OPTIONS_VERSION,
        .show    = GIT_STATUS_SHOW_INDEX_AND_WORKDIR,
        .flags   = GIT_STATUS_OPT_INCLUDE_UNTRACKED
                 | GIT_STATUS_OPT_RECURSE_UNTRACKED_DIRS
                 | GIT_STATUS_OPT_EXCLUDE_SUBMODULES,
    };

    git_status_list *list = NULL;
    if (git_status_list_new(&list, repo, &opts) != 0) return;

    size_t n = git_status_list_entrycount(list);
    for (size_t i = 0; i < n; i++) {
        const git_status_entry *e = git_status_byindex(list, i);
        unsigned int s = e->status;

        if (s & (GIT_STATUS_INDEX_NEW | GIT_STATUS_INDEX_MODIFIED |
                 GIT_STATUS_INDEX_DELETED | GIT_STATUS_INDEX_RENAMED |
                 GIT_STATUS_INDEX_TYPECHANGE))
            r->staged++;

        if (s & (GIT_STATUS_WT_MODIFIED | GIT_STATUS_WT_DELETED |
                 GIT_STATUS_WT_TYPECHANGE | GIT_STATUS_WT_RENAMED))
            r->modified++;

        if (s & GIT_STATUS_WT_NEW)
            r->untracked++;
    }
    git_status_list_free(list);
}

static void fill_ahead_behind(Repo *r, git_repository *repo) {
    /* get local HEAD oid */
    git_reference *head = NULL;
    if (git_repository_head(&head, repo) != 0) return;

    git_object *local_obj = NULL;
    if (git_reference_peel(&local_obj, head, GIT_OBJECT_COMMIT) != 0) {
        git_reference_free(head);
        return;
    }
    const git_oid *local_oid = git_object_id(local_obj);

    /* get upstream ref name */
    const char *refname = git_reference_name(head);
    git_buf upstream_name = GIT_BUF_INIT;
    if (git_branch_upstream_name(&upstream_name, repo, refname) != 0) {
        git_object_free(local_obj);
        git_reference_free(head);
        return;
    }

    /* resolve upstream ref to oid */
    git_reference *upstream_ref = NULL;
    if (git_reference_lookup(&upstream_ref, repo, upstream_name.ptr) != 0) {
        git_buf_dispose(&upstream_name);
        git_object_free(local_obj);
        git_reference_free(head);
        return;
    }

    git_object *upstream_obj = NULL;
    if (git_reference_peel(&upstream_obj, upstream_ref, GIT_OBJECT_COMMIT) != 0) {
        git_reference_free(upstream_ref);
        git_buf_dispose(&upstream_name);
        git_object_free(local_obj);
        git_reference_free(head);
        return;
    }
    const git_oid *upstream_oid = git_object_id(upstream_obj);

    git_graph_ahead_behind(&r->ahead, &r->behind, repo, local_oid, upstream_oid);
    r->has_remote = 1;

    git_object_free(upstream_obj);
    git_reference_free(upstream_ref);
    git_buf_dispose(&upstream_name);
    git_object_free(local_obj);
    git_reference_free(head);
}

static void fill_last_commit(Repo *r, git_repository *repo) {
    git_reference *head = NULL;
    if (git_repository_head(&head, repo) != 0) return;

    git_object *obj = NULL;
    if (git_reference_peel(&obj, head, GIT_OBJECT_COMMIT) == 0) {
        r->last_commit = git_commit_time((git_commit *)obj);
        git_object_free(obj);
    }
    git_reference_free(head);
}

/* ── Print one repo (single line) ─────────────────────────────────────────── */
static void print_repo(const Repo *r) {
    int is_dirty = (r->staged || r->modified || r->untracked);

    const char *name = strrchr(r->path, '/');
    name = name ? name + 1 : r->path;

    /* NAME */
    printf("  %s", C(COL_CYAN));
    write_col(name, COL_NAME);
    printf("%s  ", C(COL_RESET));

    /* BRANCH */
    printf("%s", C(is_dirty ? COL_YELLOW : COL_GREEN));
    write_col(r->branch, COL_BRANCH);
    printf("%s  ", C(COL_RESET));

    /* SYNC */
    write_sync(r);
    printf("  ");

    /* WHEN */
    printf("%s", C(COL_DIM));
    write_col(relative_time(r->last_commit), COL_TIME);
    printf("%s  ", C(COL_RESET));

    /* STATUS */
    if (!is_dirty) {
        printf("%s✓%s", C(COL_GREEN), C(COL_RESET));
    } else {
        if (r->staged)    printf("%s●%d%s ", C(COL_GREEN),   r->staged,    C(COL_RESET));
        if (r->modified)  printf("%s✗%d%s ", C(COL_RED),     r->modified,  C(COL_RESET));
        if (r->untracked) printf("%s?%d%s",  C(COL_MAGENTA), r->untracked, C(COL_RESET));
    }
    printf("\n");
}

/* ── Process a single repo ─────────────────────────────────────────────────── */
static void process_repo(const char *path) {
    git_repository *repo = NULL;
    if (git_repository_open(&repo, path) != 0) return;

    Repo r;
    memset(&r, 0, sizeof(r));
    strncpy(r.path, path, sizeof(r.path)-1);

    fill_branch(&r, repo);
    fill_status(&r, repo);
    fill_ahead_behind(&r, repo);
    fill_last_commit(&r, repo);

    git_repository_free(repo);

    /* update global stats */
    total_repos++;
    int is_dirty = (r.staged || r.modified || r.untracked);
    if (is_dirty) dirty_repos++; else clean_repos++;
    if (r.behind > 0) behind_repos++;

    print_repo(&r);
}

/* directories to always skip */
static const char *SKIP_DIRS[] = {
    "vendor", "node_modules", ".git",
    NULL
};

static int should_skip(const char *name) {
    for (int i = 0; SKIP_DIRS[i]; i++)
        if (strcmp(name, SKIP_DIRS[i]) == 0) return 1;
    if (!opt_all && name[0] == '.') return 1;
    return 0;
}

/* ── Directory traversal ───────────────────────────────────────────────────── */
static void find_repos(const char *path, int depth) {
    if (depth > opt_max_depth) return;

    /* check for .git – process as repo, then still recurse for nested repos */
    char git_path[PATH_MAX];
    snprintf(git_path, sizeof(git_path), "%s/.git", path);
    struct stat st;
    int is_repo = (stat(git_path, &st) == 0);
    if (is_repo) process_repo(path);

    /* recurse into subdirectories (skip vendor, node_modules, .git, hidden) */
    DIR *dir = opendir(path);
    if (!dir) return;

    struct dirent *ent;
    while ((ent = readdir(dir)) != NULL) {
        if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0) continue;
        if (should_skip(ent->d_name)) continue;

        char sub[PATH_MAX];
        snprintf(sub, sizeof(sub), "%s/%s", path, ent->d_name);

        struct stat sub_st;
        if (lstat(sub, &sub_st) != 0) continue;
        if (!S_ISDIR(sub_st.st_mode)) continue;
        /* skip symlinks to avoid loops */
        if (S_ISLNK(sub_st.st_mode)) continue;

        find_repos(sub, depth + 1);
    }
    closedir(dir);
}

/* ── Help ──────────────────────────────────────────────────────────────────── */
static void usage(const char *prog) {
    fprintf(stderr,
        "Usage: %s [OPTIONS] [DIRECTORY]\n"
        "\n"
        "Recursively scan DIRECTORY (default: .) for git repositories\n"
        "and display their status.\n"
        "\n"
        "Options:\n"
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
            opt_all = 1;
        } else if (strcmp(argv[i], "--no-color") == 0) {
            opt_no_color = 1;
        } else if (strcmp(argv[i], "-d") == 0) {
            if (i + 1 >= argc) {
                fprintf(stderr, "Error: -d requires a number\n");
                return 1;
            }
            opt_max_depth = atoi(argv[++i]);
            if (opt_max_depth < 0) opt_max_depth = 0;
        } else if (argv[i][0] != '-') {
            scan_dir = argv[i];
        } else {
            fprintf(stderr, "Unknown option: %s\n", argv[i]);
            usage(argv[0]);
            return 1;
        }
    }

    /* resolve to absolute path */
    char abs_dir[PATH_MAX];
    if (realpath(scan_dir, abs_dir) == NULL) {
        fprintf(stderr, "Error: cannot resolve path '%s'\n", scan_dir);
        return 1;
    }

    git_libgit2_init();

    printf("%sScanning:%s %s\n\n", C(COL_BOLD), C(COL_RESET), abs_dir);
    print_header();

    find_repos(abs_dir, 0);

    /* ── separator + summary ── */
    printf("  %s%s%s\n", C(COL_DIM), SEP_LINE, C(COL_RESET));

    if (total_repos == 0) {
        printf("  No git repositories found.\n");
    } else {
        printf("  %s%d repo%s%s · %s%d clean%s · %s%d dirty%s",
            C(COL_BOLD), total_repos, total_repos == 1 ? "" : "s", C(COL_RESET),
            C(COL_GREEN), clean_repos,  C(COL_RESET),
            C(COL_RED),   dirty_repos,  C(COL_RESET));
        if (behind_repos > 0)
            printf(" · %s%d behind%s", C(COL_YELLOW), behind_repos, C(COL_RESET));
        printf("\n");
    }

    git_libgit2_shutdown();
    return 0;
}
