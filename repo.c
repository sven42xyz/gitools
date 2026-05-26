/*
 * repo.c – libgit2 queries, branch switching, fetch, pull, repo collection
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdatomic.h>
#include <pthread.h>
#include <unistd.h>
#include <sys/wait.h>
#include <time.h>

extern char **environ;
#include "gitools.h"

/* Suppress noisy -Wmissing-field-initializers triggered by libgit2 macros */
#pragma GCC diagnostic ignored "-Wmissing-field-initializers"

/* ── Git binary path (resolved once in main before threads start) ───────────── */
static char g_git_path[PATH_MAX] = "";

/*
 * Walk PATH and store the first executable named "git" in g_git_path.
 * Must be called from the main thread before any threads are spawned so that
 * run_git_capture can use execve(g_git_path) instead of execvp("git").
 * execve is a direct syscall (async-signal-safe); execvp calls getenv+malloc.
 */
void resolve_git_path(void) {
    const char *path_env = getenv("PATH");
    if (!path_env) return;
    const char *p = path_env;
    while (*p) {
        const char *end = strchr(p, ':');
        size_t dlen = end ? (size_t)(end - p) : strlen(p);
        char candidate[PATH_MAX];
        if (snprintf(candidate, sizeof(candidate), "%.*s/git", (int)dlen, p) < (int)sizeof(candidate)
                && access(candidate, X_OK) == 0) {
            strncpy(g_git_path, candidate, sizeof(g_git_path) - 1);
            g_git_path[sizeof(g_git_path) - 1] = '\0';
            return;
        }
        if (!end) break;
        p = end + 1;
    }
}

/* ── Path collection (filled by scan.c) ────────────────────────────────────── */
char  **g_paths      = NULL;
size_t  g_path_count = 0;
static size_t g_path_cap = 0;

void collect_path(const char *path) {
    if (g_path_count >= g_path_cap) {
        g_path_cap = g_path_cap ? g_path_cap * 2 : 32;
        char **tmp = realloc(g_paths, g_path_cap * sizeof(char *));
        if (!tmp) { fprintf(stderr, "Error: out of memory\n"); exit(1); }
        g_paths = tmp;
    }
    g_paths[g_path_count] = strdup(path);
    if (!g_paths[g_path_count]) { fprintf(stderr, "Error: out of memory\n"); exit(1); }
    g_path_count++;
}

/* ── Repo array (pre-allocated before threading) ───────────────────────────── */
Repo  *g_repos     = NULL;
size_t g_repo_count = 0;

/* ── Branch ────────────────────────────────────────────────────────────────── */
static void fill_branch(Repo *r, git_repository *repo) {
    if (git_repository_head_unborn(repo) == 1) {
        snprintf(r->branch, sizeof(r->branch), "(unborn)");
        return;
    }

    if (git_repository_head_detached(repo) == 1) {
        git_reference *head = NULL;
        if (git_repository_head(&head, repo) == 0) {
            git_object *obj = NULL;
            if (git_reference_peel(&obj, head, GIT_OBJECT_COMMIT) == 0) {
                char hex[8];
                git_oid_tostr(hex, sizeof(hex), git_object_id(obj));
                snprintf(r->branch, sizeof(r->branch), "(%s)", hex);
                git_object_free(obj);
            } else {
                snprintf(r->branch, sizeof(r->branch), "(detached)");
            }
            git_reference_free(head);
        } else {
            snprintf(r->branch, sizeof(r->branch), "(detached)");
        }
        return;
    }

    git_reference *head = NULL;
    if (git_repository_head(&head, repo) != 0) {
        snprintf(r->branch, sizeof(r->branch), "(?)");
        return;
    }
    snprintf(r->branch, sizeof(r->branch), "%s", git_reference_shorthand(head));
    git_reference_free(head);
}

/* ── Status ────────────────────────────────────────────────────────────────── */
static void fill_status(Repo *r, git_repository *repo) {
    r->staged = r->modified = r->untracked = 0;

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

/* ── Ahead / behind ────────────────────────────────────────────────────────── */
static void fill_ahead_behind(Repo *r, git_repository *repo) {
    r->ahead = r->behind = 0;
    r->has_remote = 0;

    git_reference *head = NULL;
    if (git_repository_head(&head, repo) != 0) return;

    git_object *local_obj = NULL;
    if (git_reference_peel(&local_obj, head, GIT_OBJECT_COMMIT) != 0) {
        git_reference_free(head);
        return;
    }

    git_buf upstream_name = GIT_BUF_INIT;
    if (git_branch_upstream_name(&upstream_name, repo, git_reference_name(head)) != 0) {
        git_buf_dispose(&upstream_name);
        git_object_free(local_obj);
        git_reference_free(head);
        return;
    }

    git_reference *upstream_ref = NULL;
    if (git_reference_lookup(&upstream_ref, repo, upstream_name.ptr) != 0) {
        git_buf_dispose(&upstream_name);
        git_object_free(local_obj);
        git_reference_free(head);
        return;
    }

    git_object *upstream_obj = NULL;
    if (git_reference_peel(&upstream_obj, upstream_ref, GIT_OBJECT_COMMIT) == 0) {
        if (git_graph_ahead_behind(&r->ahead, &r->behind, repo,
                                   git_object_id(local_obj),
                                   git_object_id(upstream_obj)) == 0) {
            r->has_remote = 1;
        }
        git_object_free(upstream_obj);
    }

    git_reference_free(upstream_ref);
    git_buf_dispose(&upstream_name);
    git_object_free(local_obj);
    git_reference_free(head);
}

/* ── Last commit time ──────────────────────────────────────────────────────── */
static void fill_last_commit(Repo *r, git_repository *repo) {
    if (git_repository_head_unborn(repo) == 1) return;

    git_reference *head = NULL;
    if (git_repository_head(&head, repo) != 0) return;

    git_object *obj = NULL;
    if (git_reference_peel(&obj, head, GIT_OBJECT_COMMIT) == 0) {
        r->last_commit = git_commit_time((git_commit *)obj);
        git_object_free(obj);
    }
    git_reference_free(head);
}

/* ── Branch switching ──────────────────────────────────────────────────────── */
static SwitchResult do_switch(git_repository *repo, const Repo *r,
                               const char *target) {
    if (strcmp(r->branch, target) == 0)
        return SR_ALREADY;

    /* try local branch first, then remote tracking ref */
    git_reference *ref = NULL;
    int from_remote = 0;
    if (git_branch_lookup(&ref, repo, target, GIT_BRANCH_LOCAL) != 0) {
        char remote_ref[PATH_MAX];
        snprintf(remote_ref, sizeof(remote_ref), "refs/remotes/origin/%s", target);
        if (git_reference_lookup(&ref, repo, remote_ref) != 0)
            return SR_NOT_FOUND;
        from_remote = 1;
    }

    if (r->staged || r->modified) {
        git_reference_free(ref);
        return SR_DIRTY;
    }

    git_object *target_obj = NULL;
    if (git_reference_peel(&target_obj, ref, GIT_OBJECT_COMMIT) != 0) {
        git_reference_free(ref);
        return SR_ERROR;
    }
    git_reference_free(ref);

    /* create local tracking branch when switching from a remote ref */
    if (from_remote) {
        git_reference *new_branch = NULL;
        if (git_branch_create(&new_branch, repo, target,
                              (git_commit *)target_obj, 0) != 0) {
            git_object_free(target_obj);
            return SR_ERROR;
        }
        char upstream[sizeof(opt_switch_branch) + 8];
        snprintf(upstream, sizeof(upstream), "origin/%s", target);
        git_branch_set_upstream(new_branch, upstream);  /* tracking is best-effort */
        git_reference_free(new_branch);
    }

    git_checkout_options opts = {
        .version           = GIT_CHECKOUT_OPTIONS_VERSION,
        .checkout_strategy = GIT_CHECKOUT_SAFE,
    };
    int err = git_checkout_tree(repo, target_obj, &opts);
    git_object_free(target_obj);
    if (err != 0) return SR_ERROR;

    char refname[sizeof(opt_switch_branch) + 12];
    int rn = snprintf(refname, sizeof(refname), "refs/heads/%s", target);
    if (rn <= 0 || rn >= (int)sizeof(refname)) return SR_ERROR;
    if (git_repository_set_head(repo, refname) != 0)
        return SR_ERROR;

    return from_remote ? SR_CREATED : SR_SWITCHED;
}

/* ── Subprocess helper ─────────────────────────────────────────────────────── */
/*
 * Fork and exec a git command, capturing combined stdout+stderr into buf.
 * argv must be NULL-terminated (argv[0] == "git").
 * Returns the process exit code, or -1 on fork/exec failure.
 */
static int run_git_capture(const char **argv, char *buf, size_t n) {
    int pfd[2];
    if (pipe(pfd) != 0) return -1;

    pid_t pid = fork();
    if (pid < 0) {
        close(pfd[0]); close(pfd[1]);
        return -1;
    }
    if (pid == 0) {
        close(pfd[0]);
        dup2(pfd[1], STDOUT_FILENO);
        dup2(pfd[1], STDERR_FILENO);
        close(pfd[1]);
        /* execve is a direct syscall (async-signal-safe); execvp is not.
         * g_git_path is resolved in the parent before any threads start.
         * If g_git_path is somehow empty, exit immediately — do not fall
         * back to execvp, which calls getenv/malloc and is not
         * async-signal-safe. */
        if (g_git_path[0])
            execve(g_git_path, (char *const *)argv, environ);
        _exit(127);   /* execve failed or g_git_path not resolved */
    }

    close(pfd[1]);
    size_t total = 0;
    if (buf && n > 0) {
        ssize_t nr;
        while (total + 1 < n &&
               (nr = read(pfd[0], buf + total, n - total - 1)) > 0)
            total += (size_t)nr;
        buf[total] = '\0';
        /* strip trailing newlines */
        while (total > 0 && (buf[total-1] == '\n' || buf[total-1] == '\r'))
            buf[--total] = '\0';
    }
    /* drain remaining output */
    { char tmp[256]; while (read(pfd[0], tmp, sizeof(tmp)) > 0) {} }
    close(pfd[0]);

    int status = 0;
    waitpid(pid, &status, 0);
    return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
}

/* ── Fetch ─────────────────────────────────────────────────────────────────── */
static FetchResult do_fetch(git_repository *repo, Repo *r) {
    /* local check: does an "origin" remote exist? (no network) */
    git_remote *remote = NULL;
    if (git_remote_lookup(&remote, repo, "origin") != 0)
        return FR_NO_REMOTE;
    git_remote_free(remote);

    /* snapshot the remote-tracking ref before fetching so we can detect changes */
    char refspec[320] = "";
    char ref_before[128] = "";
    bool have_before = false;
    if (r->branch[0] != '\0' && r->branch[0] != '(') {
        snprintf(refspec, sizeof(refspec), "refs/remotes/origin/%s", r->branch);
        const char *rp_argv[] = { "git", "-C", r->path, "rev-parse", "--verify", refspec, NULL };
        have_before = (run_git_capture(rp_argv, ref_before, sizeof(ref_before)) == 0
                       && ref_before[0] != '\0');
    }

    const char *fetch_argv[] = { "git", "-C", r->path, "fetch", "--quiet", "origin", NULL };
    char errbuf[256] = "";
    int rc = run_git_capture(fetch_argv, errbuf, sizeof(errbuf));
    if (rc != 0) {
        strncpy(r->net_error, errbuf[0] ? errbuf : "git fetch failed",
                sizeof(r->net_error) - 1);
        return FR_ERROR;
    }

    /* compare ref after fetch; unchanged → nothing new was downloaded */
    if (have_before) {
        char ref_after[128] = "";
        const char *rp_argv[] = { "git", "-C", r->path, "rev-parse", "--verify", refspec, NULL };
        if (run_git_capture(rp_argv, ref_after, sizeof(ref_after)) == 0
                && strcmp(ref_before, ref_after) == 0)
            return FR_UP_TO_DATE;
    }
    return FR_FETCHED;
}

/* ── Pull (fast-forward only) ──────────────────────────────────────────────── */
static PullResult do_pull(git_repository *repo, Repo *r) {
    if (r->staged || r->modified)
        return PR_DIRTY;

    /* local check: does an "origin" remote exist? (no network) */
    git_remote *remote = NULL;
    if (git_remote_lookup(&remote, repo, "origin") != 0)
        return PR_NO_REMOTE;
    git_remote_free(remote);

    const char *argv[] = {
        "git", "-C", r->path, "pull", "--ff-only", "--no-rebase", NULL
    };
    char outbuf[512] = "";
    int rc = run_git_capture(argv, outbuf, sizeof(outbuf));

    if (rc == 0) {
        if (strstr(outbuf, "Already up to date") != NULL)
            return PR_UP_TO_DATE;
        return PR_PULLED;
    }

    /* non-zero exit: classify the failure */
    if (strstr(outbuf, "Not possible to fast-forward") != NULL ||
        strstr(outbuf, "not possible to fast-forward") != NULL)
        return PR_NOT_FF;

    if (strstr(outbuf, "no tracking information") != NULL ||
        strstr(outbuf, "no upstream configured")  != NULL)
        return PR_NO_REMOTE;

    strncpy(r->net_error, outbuf[0] ? outbuf : "git pull failed",
            sizeof(r->net_error) - 1);
    return PR_ERROR;
}

/* ── Patch-id cache for squash-merge detection ─────────────────────────────── */
/*
 * Squash-merge detection compares each candidate branch's cumulative diff
 * (merge-base → branch tip) against the patch-id of each recent default-branch
 * commit. A match means the branch's work was squashed into a single default
 * commit and the branch can be considered merged.
 *
 * We cap the default-branch walk at SQUASH_DEFAULT_DEPTH commits to bound the
 * cost on large histories — squash merges are typically recent.
 */
#define SQUASH_DEFAULT_DEPTH 500

static int commit_first_parent_patchid(git_repository *repo, const git_oid *commit_oid,
                                       git_oid *out) {
    git_commit *commit = NULL;
    if (git_commit_lookup(&commit, repo, commit_oid) != 0) return -1;

    git_tree *new_tree = NULL;
    if (git_commit_tree(&new_tree, commit) != 0) {
        git_commit_free(commit);
        return -1;
    }

    git_tree *old_tree = NULL;
    if (git_commit_parentcount(commit) > 0) {
        git_commit *parent = NULL;
        if (git_commit_parent(&parent, commit, 0) == 0) {
            git_commit_tree(&old_tree, parent);
            git_commit_free(parent);
        }
    }

    git_diff *diff = NULL;
    int rc = -1;
    if (git_diff_tree_to_tree(&diff, repo, old_tree, new_tree, NULL) == 0) {
        if (git_diff_patchid(out, diff, NULL) == 0) rc = 0;
        git_diff_free(diff);
    }

    if (old_tree) git_tree_free(old_tree);
    git_tree_free(new_tree);
    git_commit_free(commit);
    return rc;
}

static int branch_cumulative_patchid(git_repository *repo, const git_oid *branch_tip,
                                     const git_oid *default_tip, git_oid *out) {
    git_oid mb;
    if (git_merge_base(&mb, repo, branch_tip, default_tip) != 0) return -1;

    git_commit *mb_commit = NULL, *branch_commit = NULL;
    if (git_commit_lookup(&mb_commit, repo, &mb) != 0) return -1;
    if (git_commit_lookup(&branch_commit, repo, branch_tip) != 0) {
        git_commit_free(mb_commit);
        return -1;
    }

    git_tree *mb_tree = NULL, *branch_tree = NULL;
    git_commit_tree(&mb_tree, mb_commit);
    git_commit_tree(&branch_tree, branch_commit);

    int rc = -1;
    if (mb_tree && branch_tree) {
        git_diff *diff = NULL;
        if (git_diff_tree_to_tree(&diff, repo, mb_tree, branch_tree, NULL) == 0) {
            if (git_diff_patchid(out, diff, NULL) == 0) rc = 0;
            git_diff_free(diff);
        }
    }

    if (mb_tree) git_tree_free(mb_tree);
    if (branch_tree) git_tree_free(branch_tree);
    git_commit_free(mb_commit);
    git_commit_free(branch_commit);
    return rc;
}

/*
 * Walk the default branch (up to SQUASH_DEFAULT_DEPTH commits), compute each
 * commit's first-parent patch-id, and return them as a newly allocated array.
 * Caller must free *out_ids.
 */
static int build_default_patchids(git_repository *repo, const git_oid *default_tip,
                                  git_oid **out_ids, size_t *out_count) {
    *out_ids = NULL;
    *out_count = 0;

    git_revwalk *walk = NULL;
    if (git_revwalk_new(&walk, repo) != 0) return -1;
    if (git_revwalk_push(walk, default_tip) != 0) {
        git_revwalk_free(walk);
        return -1;
    }

    size_t cap = 64;
    git_oid *ids = malloc(cap * sizeof(*ids));
    if (!ids) { git_revwalk_free(walk); return -1; }
    size_t count = 0;

    git_oid commit_oid;
    int seen = 0;
    while (seen < SQUASH_DEFAULT_DEPTH && git_revwalk_next(&commit_oid, walk) == 0) {
        seen++;
        git_oid pid;
        if (commit_first_parent_patchid(repo, &commit_oid, &pid) != 0) continue;
        if (count >= cap) {
            size_t ncap = cap * 2;
            git_oid *tmp = realloc(ids, ncap * sizeof(*ids));
            if (!tmp) { free(ids); git_revwalk_free(walk); return -1; }
            ids = tmp;
            cap = ncap;
        }
        git_oid_cpy(&ids[count++], &pid);
    }

    git_revwalk_free(walk);
    *out_ids = ids;
    *out_count = count;
    return 0;
}

static int patchid_in_set(const git_oid *needle, const git_oid *set, size_t count) {
    for (size_t i = 0; i < count; i++)
        if (git_oid_cmp(needle, &set[i]) == 0) return 1;
    return 0;
}

/* ── Stale branches ────────────────────────────────────────────────────────── */
/*
 * Detect local branches that are either:
 *   STR_GONE   — upstream tracking branch is configured but no longer exists
 *   STR_MERGED — fully reachable from the repo's default branch (main/master)
 *
 * Skips the current HEAD branch and the default branch itself. The current
 * HEAD's *short* name is compared (e.g. "main"), matching what fill_branch
 * stores into r->branch.
 */
static void append_stale(Repo *r, size_t *cap, const char *name, StaleReason reason) {
    if (r->stale_count >= *cap) {
        size_t ncap = *cap ? *cap * 2 : 8;
        StaleBranch *tmp = realloc(r->stale, ncap * sizeof(*tmp));
        if (!tmp) return;   /* out of memory: silently drop further entries */
        r->stale = tmp;
        *cap = ncap;
    }
    StaleBranch *sb = &r->stale[r->stale_count++];
    strncpy(sb->name, name, sizeof(sb->name) - 1);
    sb->name[sizeof(sb->name) - 1] = '\0';
    sb->reason = reason;
}

static void fill_stale_branches(Repo *r, git_repository *repo) {
    /* Resolve default branch:
     *   1. refs/remotes/origin/HEAD — the remote's published default. This is
     *      what "default branch" actually means for repos that use names other
     *      than main/master (e.g. develop, trunk).
     *   2. Fall back to a local "main" or "master" branch when origin/HEAD
     *      is unavailable (offline clones, no remote, freshly initialised).
     * Without any of these we can still detect GONE branches but not MERGED. */
    git_reference *default_ref = NULL;
    r->default_branch[0] = '\0';

    git_reference *origin_head = NULL;
    if (git_reference_lookup(&origin_head, repo, "refs/remotes/origin/HEAD") == 0) {
        git_reference *resolved = NULL;
        if (git_reference_resolve(&resolved, origin_head) == 0) {
            const char *full = git_reference_name(resolved);
            const char *prefix = "refs/remotes/origin/";
            size_t pn = strlen(prefix);
            if (full && strncmp(full, prefix, pn) == 0) {
                const char *short_name = full + pn;
                /* Only adopt it when a matching local branch exists. */
                if (git_branch_lookup(&default_ref, repo, short_name, GIT_BRANCH_LOCAL) == 0) {
                    strncpy(r->default_branch, short_name, sizeof(r->default_branch) - 1);
                    r->default_branch[sizeof(r->default_branch) - 1] = '\0';
                }
            }
            git_reference_free(resolved);
        }
        git_reference_free(origin_head);
    }

    if (!default_ref) {
        if (git_branch_lookup(&default_ref, repo, "main", GIT_BRANCH_LOCAL) == 0) {
            strncpy(r->default_branch, "main", sizeof(r->default_branch) - 1);
        } else if (git_branch_lookup(&default_ref, repo, "master", GIT_BRANCH_LOCAL) == 0) {
            strncpy(r->default_branch, "master", sizeof(r->default_branch) - 1);
        } else {
            default_ref = NULL;
        }
    }

    git_oid default_oid;
    int have_default_oid = 0;
    if (default_ref) {
        git_object *obj = NULL;
        if (git_reference_peel(&obj, default_ref, GIT_OBJECT_COMMIT) == 0) {
            git_oid_cpy(&default_oid, git_object_id(obj));
            have_default_oid = 1;
            git_object_free(obj);
        }
    }

    /* Build patch-id cache of default branch (for squash detection). Built
     * lazily on first need; safe to leave NULL if default branch is unknown. */
    git_oid *default_patchids = NULL;
    size_t   default_patchids_count = 0;
    int      default_patchids_built = 0;

    git_branch_iterator *it = NULL;
    if (git_branch_iterator_new(&it, repo, GIT_BRANCH_LOCAL) != 0) {
        if (default_ref) git_reference_free(default_ref);
        return;
    }

    size_t cap = 0;
    git_reference *ref = NULL;
    git_branch_t   bt;
    while (git_branch_next(&ref, &bt, it) == 0) {
        const char *name = NULL;
        if (git_branch_name(&name, ref) != 0 || !name) {
            git_reference_free(ref);
            continue;
        }

        /* Skip the current HEAD branch and the default branch itself */
        if (r->branch[0] && strcmp(name, r->branch) == 0) { git_reference_free(ref); continue; }
        if (r->default_branch[0] && strcmp(name, r->default_branch) == 0) {
            git_reference_free(ref);
            continue;
        }

        /* Always protect main/master implicitly; they are conventional default
         * branches and should never be flagged even when not the resolved
         * default of this particular repo. */
        int is_protected = (strcmp(name, "main") == 0 || strcmp(name, "master") == 0);
        for (size_t k = 0; !is_protected && k < opt_protected_branches_count; k++) {
            if (strcmp(name, opt_protected_branches[k]) == 0)
                is_protected = 1;
        }
        if (is_protected) { git_reference_free(ref); continue; }

        /* --older-than filter: skip branches whose tip commit is too recent.
         * Applied before classification to avoid wasted patch-id work. */
        if (opt_older_than_secs > 0) {
            git_object *bobj = NULL;
            if (git_reference_peel(&bobj, ref, GIT_OBJECT_COMMIT) != 0) {
                git_reference_free(ref);
                continue;
            }
            git_time_t t = git_commit_time((git_commit *)bobj);
            git_object_free(bobj);
            time_t now = time(NULL);
            if ((long)((time_t)now - (time_t)t) < opt_older_than_secs) {
                git_reference_free(ref);
                continue;
            }
        }

        /* GONE: upstream is configured but the remote-tracking ref doesn't exist. */
        git_buf upstream_name = GIT_BUF_INIT;
        int has_upstream_cfg =
            (git_branch_upstream_name(&upstream_name, repo, git_reference_name(ref)) == 0);
        if (has_upstream_cfg) {
            git_reference *upstream_ref = NULL;
            int upstream_exists =
                (git_reference_lookup(&upstream_ref, repo, upstream_name.ptr) == 0);
            if (upstream_ref) git_reference_free(upstream_ref);
            git_buf_dispose(&upstream_name);
            if (!upstream_exists) {
                append_stale(r, &cap, name, STR_GONE);
                git_reference_free(ref);
                continue;
            }
        } else {
            git_buf_dispose(&upstream_name);
        }

        /* MERGED / SQUASHED: both need the default OID. */
        if (have_default_oid) {
            git_object *bobj = NULL;
            if (git_reference_peel(&bobj, ref, GIT_OBJECT_COMMIT) == 0) {
                const git_oid *boid = git_object_id(bobj);

                int classified = 0;
                if (git_oid_cmp(boid, &default_oid) != 0) {
                    if (git_graph_descendant_of(repo, &default_oid, boid) == 1) {
                        append_stale(r, &cap, name, STR_MERGED);
                        classified = 1;
                    }
                }

                /* SQUASHED: cumulative branch diff vs merge-base matches some
                 * default-branch commit's patch-id. Build the cache lazily. */
                if (!classified) {
                    if (!default_patchids_built) {
                        build_default_patchids(repo, &default_oid,
                                               &default_patchids, &default_patchids_count);
                        default_patchids_built = 1;
                    }
                    if (default_patchids_count > 0) {
                        git_oid bpid;
                        if (branch_cumulative_patchid(repo, boid, &default_oid, &bpid) == 0
                            && patchid_in_set(&bpid, default_patchids, default_patchids_count))
                            append_stale(r, &cap, name, STR_SQUASHED);
                    }
                }
                git_object_free(bobj);
            }
        }

        git_reference_free(ref);
    }

    free(default_patchids);
    git_branch_iterator_free(it);
    if (default_ref) git_reference_free(default_ref);
}

/* ── Prune stale branches ──────────────────────────────────────────────────── */
/*
 * Delete branches collected by fill_stale_branches(). Runs sequentially after
 * detection and after the user confirmation in main.c.
 *
 * Safety:
 *   STR_MERGED — always safe (already verified merged into default).
 *   STR_GONE   — re-verify reachability from default; refuse if there are
 *                local commits not in default (avoids data loss).
 *
 * Per-branch result is stored in sb->action and rendered by display.c.
 */
void prune_stale_branches(void) {
    size_t deleted = 0, refused = 0, errors = 0;

    for (size_t i = 0; i < g_repo_count; i++) {
        Repo *r = &g_repos[i];
        if (r->stale_count == 0) continue;

        git_repository *repo = NULL;
        if (git_repository_open(&repo, r->path) != 0) {
            for (size_t j = 0; j < r->stale_count; j++) {
                r->stale[j].action = SA_ERROR;
                errors++;
            }
            continue;
        }

        /* Re-resolve default branch OID for the merged-into check on GONE. */
        git_oid default_oid;
        int have_default_oid = 0;
        if (r->default_branch[0]) {
            git_reference *dref = NULL;
            if (git_branch_lookup(&dref, repo, r->default_branch, GIT_BRANCH_LOCAL) == 0) {
                git_object *obj = NULL;
                if (git_reference_peel(&obj, dref, GIT_OBJECT_COMMIT) == 0) {
                    git_oid_cpy(&default_oid, git_object_id(obj));
                    have_default_oid = 1;
                    git_object_free(obj);
                }
                git_reference_free(dref);
            }
        }

        for (size_t j = 0; j < r->stale_count; j++) {
            StaleBranch *sb = &r->stale[j];
            git_reference *bref = NULL;
            if (git_branch_lookup(&bref, repo, sb->name, GIT_BRANCH_LOCAL) != 0) {
                sb->action = SA_ERROR;
                errors++;
                continue;
            }

            /* For GONE, verify the branch is fully reachable from default
             * before deleting — otherwise local-only commits would be lost. */
            if (sb->reason == STR_GONE) {
                if (!have_default_oid) {
                    sb->action = SA_REFUSED_UNMERGED;
                    refused++;
                    git_reference_free(bref);
                    continue;
                }
                git_object *bobj = NULL;
                int merged_in = 0;
                if (git_reference_peel(&bobj, bref, GIT_OBJECT_COMMIT) == 0) {
                    const git_oid *boid = git_object_id(bobj);
                    if (git_oid_cmp(boid, &default_oid) == 0 ||
                        git_graph_descendant_of(repo, &default_oid, boid) == 1)
                        merged_in = 1;
                    git_object_free(bobj);
                }
                if (!merged_in) {
                    sb->action = SA_REFUSED_UNMERGED;
                    refused++;
                    git_reference_free(bref);
                    continue;
                }
            }

            if (git_branch_delete(bref) == 0) {
                sb->action = SA_DELETED;
                deleted++;
            } else {
                sb->action = SA_ERROR;
                errors++;
            }
            git_reference_free(bref);
        }

        git_repository_free(repo);
    }

    print_prune_results(deleted, refused, errors);
}

/* ── Phase 1: local libgit2 queries (no subprocess) ────────────────────────── */
/*
 * Called from worker threads — must NOT call fork/exec.
 * Handles all local queries and, when not fetching first, branch switching.
 * Ahead/behind is filled here only when no network op will refresh it.
 */
static void process_repo_local(const char *path, Repo *r) {
    git_repository *repo = NULL;
    if (git_repository_open(&repo, path) != 0) {
        fprintf(stderr, "Warning: could not open repository at '%s'\n", path);
        memset(r, 0, sizeof(*r));
        strncpy(r->path, path, sizeof(r->path) - 1);
        r->path[sizeof(r->path) - 1] = '\0';
        return;
    }

    strncpy(r->path, path, sizeof(r->path) - 1);
    r->path[sizeof(r->path) - 1] = '\0';

    fill_branch(r, repo);
    fill_status(r, repo);
    fill_last_commit(r, repo);

    /* switch without a preceding fetch: do it here in the thread */
    if (opt_switch && !opt_fetch) {
        r->switch_result = do_switch(repo, r, opt_switch_branch);
        if (r->switch_result == SR_SWITCHED || r->switch_result == SR_CREATED) {
            fill_branch(r, repo);
            fill_status(r, repo);
            fill_last_commit(r, repo);
        }
    }

    /* ahead/behind uses local remote-tracking refs; skip when a fetch will
     * refresh them in phase 2 (stale data would just be overwritten anyway) */
    if (!opt_fetch && !opt_pull)
        fill_ahead_behind(r, repo);

    if (opt_stale)
        fill_stale_branches(r, repo);

    git_repository_free(repo);
}

/* ── Phase 2: subprocess fetch/pull (called from net_worker_thread pool) ────── */
/*
 * Called concurrently from the Phase 2 thread pool after all Phase 1 workers
 * have joined. Only async-signal-safe syscalls (dup2, execve) are used between
 * fork and exec in run_git_capture, so concurrent fork() calls are safe.
 */
static void process_repo_network(Repo *r) {
    if (r->path[0] == '\0') return;   /* slot that failed to open in phase 1 */

    git_repository *repo = NULL;
    if (git_repository_open(&repo, r->path) != 0) return;

    if (opt_fetch) {
        r->fetch_result = do_fetch(repo, r);
        /* after fetch the remote-tracking refs are fresh; now switch if requested */
        if (opt_switch) {
            r->switch_result = do_switch(repo, r, opt_switch_branch);
            if (r->switch_result == SR_SWITCHED || r->switch_result == SR_CREATED) {
                fill_branch(r, repo);
                fill_status(r, repo);
                fill_last_commit(r, repo);
            }
        }
    }

    if (opt_pull) {
        r->pull_result = do_pull(repo, r);
        if (r->pull_result == PR_PULLED) {
            fill_branch(r, repo);
            fill_status(r, repo);
        }
    }

    fill_ahead_behind(r, repo);   /* refreshed after any network op */
    git_repository_free(repo);
}

/* ── Thread pool ────────────────────────────────────────────────────────────── */
static _Atomic size_t work_idx = 0;
static _Atomic size_t net_idx  = 0;

static void *worker_thread(void *arg) {
    (void)arg;
    size_t i;
    while ((i = atomic_fetch_add(&work_idx, 1)) < g_path_count)
        process_repo_local(g_paths[i], &g_repos[i]);
    return NULL;
}

static void *net_worker_thread(void *arg) {
    (void)arg;
    size_t i;
    while ((i = atomic_fetch_add(&net_idx, 1)) < g_repo_count)
        process_repo_network(&g_repos[i]);
    return NULL;
}

/* Spawn up to nthreads threads running fn, fall back to single-threaded. */
static void run_thread_pool(int nthreads, void *(*fn)(void *)) {
    if (nthreads <= 1) {
        fn(NULL);
        return;
    }
    pthread_t *threads = malloc((size_t)nthreads * sizeof(pthread_t));
    if (!threads) { fn(NULL); return; }

    int created = 0;
    for (int i = 0; i < nthreads; i++) {
        if (pthread_create(&threads[i], NULL, fn, NULL) != 0) {
            fprintf(stderr, "Warning: could not create worker thread %d\n", i);
            break;
        }
        created++;
    }
    for (int i = 0; i < created; i++)
        pthread_join(threads[i], NULL);

    if (created == 0)
        fn(NULL);

    free(threads);
}

/* ── process_all_repos ─────────────────────────────────────────────────────── */
void process_all_repos(const char *dir) {
    if (g_path_count == 0) return;

    /* pre-allocate the repo array (one slot per path, preserving scan order) */
    g_repos = calloc(g_path_count, sizeof(Repo));
    if (!g_repos) { fprintf(stderr, "Error: out of memory\n"); exit(1); }
    g_repo_count = g_path_count;

    /* choose thread count: CPU cores, capped at 8, no more than repo count */
#if defined(_SC_NPROCESSORS_ONLN)
    long _ncpus = sysconf(_SC_NPROCESSORS_ONLN);
    int nthreads = (_ncpus > 0) ? (int)_ncpus : 4;
#else
    int nthreads = 4;
#endif
    if (nthreads > 8) nthreads = 8;
    if ((size_t)nthreads > g_path_count) nthreads = (int)g_path_count;

    /* ── Phase 1: parallel local libgit2 queries ── */
    atomic_store(&work_idx, 0);
    run_thread_pool(nthreads, worker_thread);

    /* ── Phase 2: parallel subprocess fetch/pull ──
     * Stop the Phase 1 spinner before starting Phase 2 so we can print an
     * inter-phase status line and start a fresh spinner with the network verb.
     * spinner_stop() is idempotent; the matching call in main() becomes a no-op.
     * The Phase 2 spinner uses write() (async-signal-safe) so it can run safely
     * alongside the fork() calls in net_worker_thread. */
    if (opt_fetch || opt_pull) {
        spinner_stop();
        printf("  Found %zu repo%s\n", g_path_count,
               g_path_count == 1 ? "" : "s");
        fflush(stdout);

        const char *verb = opt_fetch ? "Fetching:" : "Pulling:";
        char phase2[PATH_MAX + 64];
        snprintf(phase2, sizeof(phase2), "%s%s%s %s",
                 C(COL_BOLD), verb, C(COL_RESET), dir);
        spinner_start(phase2);

        atomic_store(&net_idx, 0);
        run_thread_pool(nthreads, net_worker_thread);
        spinner_stop();
    }
}
