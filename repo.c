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
#include "gitools.h"

/* Suppress noisy -Wmissing-field-initializers triggered by libgit2 macros */
#pragma GCC diagnostic ignored "-Wmissing-field-initializers"

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
        /* Force English output so string matching in do_pull() is locale-safe */
        setenv("LC_ALL", "C", 1);
        execvp("git", (char *const *)argv);
        _exit(127);   /* exec failed: git not in PATH */
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

    const char *argv[] = { "git", "-C", r->path, "fetch", "--quiet", "origin", NULL };
    char errbuf[256] = "";
    int rc = run_git_capture(argv, errbuf, sizeof(errbuf));
    if (rc != 0) {
        strncpy(r->net_error, errbuf[0] ? errbuf : "git fetch failed",
                sizeof(r->net_error) - 1);
        return FR_ERROR;
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
        }
    }

    /* ahead/behind uses local remote-tracking refs; skip when a fetch will
     * refresh them in phase 2 (stale data would just be overwritten anyway) */
    if (!opt_fetch && !opt_pull)
        fill_ahead_behind(r, repo);

    git_repository_free(repo);
}

/* ── Phase 2: subprocess fetch/pull (sequential, main thread only) ──────────── */
/*
 * Called after all worker threads have joined — no other threads are running.
 * fork/exec is safe here: no concurrent threads, no locked mutexes to inherit.
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

static void *worker_thread(void *arg) {
    (void)arg;
    size_t i;
    while ((i = atomic_fetch_add(&work_idx, 1)) < g_path_count) {
        process_repo_local(g_paths[i], &g_repos[i]);
    }
    return NULL;
}

/* ── process_all_repos ─────────────────────────────────────────────────────── */
void process_all_repos(void) {
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

    atomic_store(&work_idx, 0);

    /* ── Phase 1: parallel local libgit2 queries ── */
    if (nthreads == 1) {
        worker_thread(NULL);
    } else {
        pthread_t *threads = malloc((size_t)nthreads * sizeof(pthread_t));
        if (!threads) { fprintf(stderr, "Error: out of memory\n"); exit(1); }

        int created = 0;
        for (int i = 0; i < nthreads; i++) {
            if (pthread_create(&threads[i], NULL, worker_thread, NULL) != 0) {
                fprintf(stderr, "Warning: could not create worker thread %d\n", i);
                break;
            }
            created++;
        }
        for (int i = 0; i < created; i++)
            pthread_join(threads[i], NULL);

        free(threads);
    }

    /* ── Phase 2: sequential subprocess fetch/pull (no concurrent threads) ──
     * All worker threads have joined. Stop the spinner before fork so no
     * other thread is running while the child process is created. */
    if (opt_fetch || opt_pull) {
        spinner_stop();   /* idempotent; main.c will call it again harmlessly */
        for (size_t i = 0; i < g_repo_count; i++)
            process_repo_network(&g_repos[i]);
    }
}
