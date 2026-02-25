/*
 * repo.c – libgit2 queries, branch switching, fetch, pull, repo collection
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdatomic.h>
#include <pthread.h>
#include <unistd.h>
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

    git_reference *ref = NULL;
    if (git_branch_lookup(&ref, repo, target, GIT_BRANCH_LOCAL) != 0)
        return SR_NOT_FOUND;

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

    return SR_SWITCHED;
}

/* ── Credential callback ────────────────────────────────────────────────────── */
/*
 * Called by libgit2 when a remote requires authentication.
 * Sequence for SSH remotes: libgit2 may first ask for a username
 * (GIT_CREDENTIAL_USERNAME), then for the actual key.
 * The `payload` points to a retry counter so we don't loop forever.
 */
static int credential_cb(git_credential **out, const char *url,
                          const char *username_from_url,
                          unsigned int allowed_types, void *payload) {
    (void)url;
    int *retries = (int *)payload;
    if ((*retries)++ > 3)
        return GIT_EAUTH;   /* give up to avoid infinite loops */

    const char *user = username_from_url ? username_from_url : "git";

    /* 1. Username request (server needs us to supply the username first) */
    if (allowed_types & GIT_CREDENTIAL_USERNAME)
        return git_credential_username_new(out, user);

    /* 2. SSH key – try the agent first, then common key files */
    if (allowed_types & GIT_CREDENTIAL_SSH_KEY) {
        if (git_credential_ssh_key_from_agent(out, user) == 0)
            return 0;

        static const char * const KEYS[] = {
            "id_ed25519", "id_ecdsa", "id_rsa", NULL
        };
        const char *home = getenv("HOME");
        if (home) {
            char priv[PATH_MAX], pub[PATH_MAX];
            for (int k = 0; KEYS[k]; k++) {
                snprintf(priv, sizeof(priv), "%s/.ssh/%s",     home, KEYS[k]);
                snprintf(pub,  sizeof(pub),  "%s/.ssh/%s.pub", home, KEYS[k]);
                if (access(priv, R_OK) == 0)
                    return git_credential_ssh_key_new(out, user, pub, priv, NULL);
            }
        }
    }

    return GIT_EAUTH;
}

static void init_fetch_opts(git_fetch_options *opts, int *retries) {
    *opts = (git_fetch_options)GIT_FETCH_OPTIONS_INIT;
    opts->callbacks.credentials = credential_cb;
    opts->callbacks.payload     = retries;
}

/* ── Fetch ─────────────────────────────────────────────────────────────────── */
static FetchResult do_fetch(git_repository *repo) {
    git_remote *remote = NULL;
    if (git_remote_lookup(&remote, repo, "origin") != 0)
        return FR_NO_REMOTE;

    int retries = 0;
    git_fetch_options opts;
    init_fetch_opts(&opts, &retries);
    int err = git_remote_fetch(remote, NULL, &opts, NULL);
    git_remote_free(remote);
    return err == 0 ? FR_FETCHED : FR_ERROR;
}

/* ── Pull (fast-forward only) ──────────────────────────────────────────────── */
static PullResult do_pull(git_repository *repo, Repo *r) {
    if (r->staged || r->modified)
        return PR_DIRTY;

    /* fetch first */
    git_remote *remote = NULL;
    if (git_remote_lookup(&remote, repo, "origin") != 0)
        return PR_NO_REMOTE;

    int retries = 0;
    git_fetch_options fetch_opts;
    init_fetch_opts(&fetch_opts, &retries);
    int err = git_remote_fetch(remote, NULL, &fetch_opts, NULL);
    git_remote_free(remote);
    if (err != 0)
        return PR_ERROR;

    /* resolve current HEAD */
    git_reference *head = NULL;
    if (git_repository_head(&head, repo) != 0)
        return PR_ERROR;

    /* find upstream reference name */
    git_buf upstream_name = GIT_BUF_INIT;
    if (git_branch_upstream_name(&upstream_name, repo, git_reference_name(head)) != 0) {
        git_reference_free(head);
        return PR_NO_REMOTE;
    }

    git_reference *upstream_ref = NULL;
    if (git_reference_lookup(&upstream_ref, repo, upstream_name.ptr) != 0) {
        git_buf_dispose(&upstream_name);
        git_reference_free(head);
        return PR_NO_REMOTE;
    }
    git_buf_dispose(&upstream_name);

    /* build annotated commit for merge analysis */
    git_annotated_commit *their_head = NULL;
    if (git_annotated_commit_from_ref(&their_head, repo, upstream_ref) != 0) {
        git_reference_free(upstream_ref);
        git_reference_free(head);
        return PR_ERROR;
    }

    git_merge_analysis_t   merge_analysis;
    git_merge_preference_t merge_preference;
    const git_annotated_commit *their_heads[1] = { their_head };
    if (git_merge_analysis(&merge_analysis, &merge_preference,
                           repo, their_heads, 1) != 0) {
        git_annotated_commit_free(their_head);
        git_reference_free(upstream_ref);
        git_reference_free(head);
        return PR_ERROR;
    }

    PullResult result;
    if (merge_analysis & GIT_MERGE_ANALYSIS_UP_TO_DATE) {
        result = PR_UP_TO_DATE;
    } else if (merge_analysis & GIT_MERGE_ANALYSIS_FASTFORWARD) {
        git_object *target_obj = NULL;
        if (git_reference_peel(&target_obj, upstream_ref, GIT_OBJECT_COMMIT) != 0) {
            result = PR_ERROR;
        } else {
            git_checkout_options co_opts = GIT_CHECKOUT_OPTIONS_INIT;
            co_opts.checkout_strategy = GIT_CHECKOUT_SAFE;
            if (git_checkout_tree(repo, target_obj, &co_opts) != 0) {
                result = PR_ERROR;
            } else {
                git_reference *new_ref = NULL;
                if (git_reference_set_target(&new_ref, head,
                                             git_object_id(target_obj), NULL) != 0) {
                    result = PR_ERROR;
                } else {
                    git_reference_free(new_ref);
                    result = PR_PULLED;
                }
            }
            git_object_free(target_obj);
        }
    } else {
        result = PR_NOT_FF;
    }

    git_annotated_commit_free(their_head);
    git_reference_free(upstream_ref);
    git_reference_free(head);
    return result;
}

/* ── Process a single repo into a pre-allocated slot ──────────────────────── */
static void process_repo(const char *path, Repo *r) {
    git_repository *repo = NULL;
    if (git_repository_open(&repo, path) != 0) {
        fprintf(stderr, "Warning: could not open repository at '%s'\n", path);
        /* leave r zeroed; path is set so it still appears in output */
        strncpy(r->path, path, sizeof(r->path) - 1);
        return;
    }

    memset(r, 0, sizeof(*r));
    strncpy(r->path, path, sizeof(r->path) - 1);

    fill_branch(r, repo);
    fill_status(r, repo);

    if (opt_switch) {
        r->switch_result = do_switch(repo, r, opt_switch_branch);
        if (r->switch_result == SR_SWITCHED) {
            fill_branch(r, repo);
            fill_status(r, repo);
        }
    }

    if (opt_fetch) {
        r->fetch_result = do_fetch(repo);
        fill_ahead_behind(r, repo);   /* refresh after fetch */
    } else if (opt_pull) {
        r->pull_result = do_pull(repo, r);
        if (r->pull_result == PR_PULLED) {
            fill_branch(r, repo);
            fill_status(r, repo);
        }
        fill_ahead_behind(r, repo);   /* refresh after pull */
    } else {
        fill_ahead_behind(r, repo);
    }

    fill_last_commit(r, repo);
    git_repository_free(repo);
}

/* ── Thread pool ────────────────────────────────────────────────────────────── */
static _Atomic size_t work_idx = 0;

static void *worker_thread(void *arg) {
    (void)arg;
    size_t i;
    while ((i = atomic_fetch_add(&work_idx, 1)) < g_path_count) {
        process_repo(g_paths[i], &g_repos[i]);
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

    if (nthreads == 1) {
        /* avoid thread overhead for small sets */
        worker_thread(NULL);
        return;
    }

    pthread_t *threads = malloc((size_t)nthreads * sizeof(pthread_t));
    if (!threads) { fprintf(stderr, "Error: out of memory\n"); exit(1); }

    for (int i = 0; i < nthreads; i++)
        pthread_create(&threads[i], NULL, worker_thread, NULL);
    for (int i = 0; i < nthreads; i++)
        pthread_join(threads[i], NULL);

    free(threads);
}
