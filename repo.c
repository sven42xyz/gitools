/*
 * repo.c – libgit2 queries, branch switching, repo collection
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "gitools.h"

/* ── Repo collection ───────────────────────────────────────────────────────── */
Repo  *g_repos     = NULL;
size_t g_repo_count = 0;
static size_t g_repo_cap = 0;

void collect_repo(const Repo *r) {
    if (g_repo_count >= g_repo_cap) {
        g_repo_cap = g_repo_cap ? g_repo_cap * 2 : 32;
        g_repos = realloc(g_repos, g_repo_cap * sizeof(Repo));
    }
    g_repos[g_repo_count++] = *r;
}

/* ── Branch ────────────────────────────────────────────────────────────────── */
static void fill_branch(Repo *r, git_repository *repo) {
    if (git_repository_head_unborn(repo) == 1) {
        strncpy(r->branch, "(unborn)", sizeof(r->branch)-1);
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
                strncpy(r->branch, "(detached)", sizeof(r->branch)-1);
            }
            git_reference_free(head);
        } else {
            strncpy(r->branch, "(detached)", sizeof(r->branch)-1);
        }
        return;
    }

    git_reference *head = NULL;
    if (git_repository_head(&head, repo) != 0) {
        strncpy(r->branch, "(?)", sizeof(r->branch)-1);
        return;
    }
    strncpy(r->branch, git_reference_shorthand(head), sizeof(r->branch)-1);
    r->branch[sizeof(r->branch)-1] = '\0';
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
        git_graph_ahead_behind(&r->ahead, &r->behind, repo,
                               git_object_id(local_obj), git_object_id(upstream_obj));
        r->has_remote = 1;
        git_object_free(upstream_obj);
    }

    git_reference_free(upstream_ref);
    git_buf_dispose(&upstream_name);
    git_object_free(local_obj);
    git_reference_free(head);
}

/* ── Last commit time ──────────────────────────────────────────────────────── */
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

    /* Peel the target branch ref to its commit/tree object. */
    git_object *target_obj = NULL;
    if (git_reference_peel(&target_obj, ref, GIT_OBJECT_COMMIT) != 0) {
        git_reference_free(ref);
        return SR_DIRTY;
    }
    git_reference_free(ref);

    /* Step 1: checkout the target tree – updates both index and working tree. */
    git_checkout_options opts = {
        .version           = GIT_CHECKOUT_OPTIONS_VERSION,
        .checkout_strategy = GIT_CHECKOUT_SAFE,
    };
    int err = git_checkout_tree(repo, target_obj, &opts);
    git_object_free(target_obj);
    if (err != 0) return SR_DIRTY;

    /* Step 2: point HEAD at the target branch. */
    char refname[512];
    snprintf(refname, sizeof(refname), "refs/heads/%s", target);
    if (git_repository_set_head(repo, refname) != 0)
        return SR_DIRTY;

    return SR_SWITCHED;
}

/* ── process_repo ──────────────────────────────────────────────────────────── */
void process_repo(const char *path) {
    git_repository *repo = NULL;
    if (git_repository_open(&repo, path) != 0) return;

    Repo r;
    memset(&r, 0, sizeof(r));
    strncpy(r.path, path, sizeof(r.path)-1);

    fill_branch(&r, repo);
    fill_status(&r, repo);

    if (opt_switch) {
        r.switch_result = do_switch(repo, &r, opt_switch_branch);
        if (r.switch_result == SR_SWITCHED) {
            fill_branch(&r, repo);
            fill_status(&r, repo);
        }
    }

    fill_ahead_behind(&r, repo);
    fill_last_commit(&r, repo);

    git_repository_free(repo);
    collect_repo(&r);
}
