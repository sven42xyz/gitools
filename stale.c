/*
 * stale.c – per-repo branch listing with stale-branch detection
 *
 * repo_branch_list() enumerates every local branch of one repository and
 * classifies the ones that look stale:
 *
 *   STR_MERGED   — tip is reachable from the default branch
 *   STR_SQUASHED — the branch's cumulative diff matches a default-branch commit
 *                  (squash merge, the usual GitHub flow)
 *   STR_GONE     — the upstream tracking branch was deleted on the remote, but
 *                  the work is not (provably) in the default branch
 *
 * A branch is safe_delete when its work is already in the default branch
 * (merged or squashed). A GONE-only branch may carry local-only commits.
 *
 * The API is free of the global Repo/opt_* state so it can back both the
 * watch-mode repo screen and, later, the --json output. It is computed lazily
 * per repo (squash detection walks history and diffs trees, too slow to run
 * on every watch tick).
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "gitools.h"

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

/* ── Default branch ────────────────────────────────────────────────────────── */
/*
 * Resolve the repo's default branch into name/oid:
 *   1. refs/remotes/origin/HEAD — the remote's published default, when a
 *      matching local branch exists (covers develop, trunk, …).
 *   2. A local "main", then "master".
 * Returns 0 on success, -1 when no default could be determined (GONE is still
 * detectable then, MERGED/SQUASHED are not).
 */
static int resolve_default(git_repository *repo, char *name, size_t n, git_oid *oid) {
    git_reference *default_ref = NULL;
    name[0] = '\0';

    git_reference *origin_head = NULL;
    if (git_reference_lookup(&origin_head, repo, "refs/remotes/origin/HEAD") == 0) {
        git_reference *resolved = NULL;
        if (git_reference_resolve(&resolved, origin_head) == 0) {
            const char *full = git_reference_name(resolved);
            const char *prefix = "refs/remotes/origin/";
            size_t pn = strlen(prefix);
            if (full && strncmp(full, prefix, pn) == 0 &&
                git_branch_lookup(&default_ref, repo, full + pn, GIT_BRANCH_LOCAL) == 0)
                snprintf(name, n, "%s", full + pn);
            git_reference_free(resolved);
        }
        git_reference_free(origin_head);
    }

    if (!default_ref) {
        if (git_branch_lookup(&default_ref, repo, "main", GIT_BRANCH_LOCAL) == 0)
            snprintf(name, n, "main");
        else if (git_branch_lookup(&default_ref, repo, "master", GIT_BRANCH_LOCAL) == 0)
            snprintf(name, n, "master");
        else
            return -1;
    }

    int rc = -1;
    git_object *obj = NULL;
    if (git_reference_peel(&obj, default_ref, GIT_OBJECT_COMMIT) == 0) {
        git_oid_cpy(oid, git_object_id(obj));
        git_object_free(obj);
        rc = 0;
    }
    git_reference_free(default_ref);
    if (rc != 0) name[0] = '\0';
    return rc;
}

/* ── Branch list ───────────────────────────────────────────────────────────── */
static bool is_protected(const char *name, char **prot, size_t nprot) {
    /* main/master are conventional defaults — never flag them, even when they
     * are not the resolved default of this particular repo */
    if (strcmp(name, "main") == 0 || strcmp(name, "master") == 0) return true;
    for (size_t i = 0; i < nprot; i++)
        if (strcmp(name, prot[i]) == 0) return true;
    return false;
}

/* Fill upstream name, gone flag and ahead/behind for one branch. */
static void fill_upstream(BranchInfo *b, git_repository *repo,
                          git_reference *ref, const git_oid *tip) {
    git_buf up = GIT_BUF_INIT;
    if (git_branch_upstream_name(&up, repo, git_reference_name(ref)) != 0) {
        git_buf_dispose(&up);
        return;   /* no upstream configured */
    }

    /* show the short name: refs/remotes/origin/foo → origin/foo */
    const char *shown = up.ptr;
    if (strncmp(shown, "refs/remotes/", 13) == 0) shown += 13;
    snprintf(b->upstream, sizeof(b->upstream), "%s", shown);

    git_reference *up_ref = NULL;
    if (git_reference_lookup(&up_ref, repo, up.ptr) != 0) {
        b->upstream_gone = true;
    } else {
        git_object *up_obj = NULL;
        if (git_reference_peel(&up_obj, up_ref, GIT_OBJECT_COMMIT) == 0) {
            size_t a = 0, bh = 0;
            if (git_graph_ahead_behind(&a, &bh, repo, tip, git_object_id(up_obj)) == 0) {
                b->ahead  = a;
                b->behind = bh;
            }
            git_object_free(up_obj);
        }
        git_reference_free(up_ref);
    }
    git_buf_dispose(&up);
}

/* HEAD first, then stale branches, then the rest — newest commit first. */
static int cmp_branch(const void *pa, const void *pb) {
    const BranchInfo *a = pa, *b = pb;
    if (a->is_head != b->is_head) return a->is_head ? -1 : 1;
    bool sa = a->reason != STR_NONE, sb = b->reason != STR_NONE;
    if (sa != sb) return sa ? -1 : 1;
    if (a->last_commit != b->last_commit) return a->last_commit > b->last_commit ? -1 : 1;
    return strcmp(a->name, b->name);
}

int repo_branch_list(const char *path, char **prot, size_t nprot, BranchList *out) {
    memset(out, 0, sizeof(*out));

    git_repository *repo = NULL;
    if (git_repository_open(&repo, path) != 0) return -1;

    git_oid default_oid;
    bool have_default =
        resolve_default(repo, out->default_branch, sizeof(out->default_branch),
                        &default_oid) == 0;

    /* patch-id cache of the default branch, built on first need */
    git_oid *default_pids = NULL;
    size_t   default_pid_count = 0;
    bool     default_pids_built = false;

    git_branch_iterator *it = NULL;
    if (git_branch_iterator_new(&it, repo, GIT_BRANCH_LOCAL) != 0) {
        git_repository_free(repo);
        return -1;
    }

    size_t cap = 0;
    git_reference *ref = NULL;
    git_branch_t   bt;
    while (git_branch_next(&ref, &bt, it) == 0) {
        const char *name = NULL;
        git_object *tip_obj = NULL;
        if (git_branch_name(&name, ref) != 0 || !name ||
            git_reference_peel(&tip_obj, ref, GIT_OBJECT_COMMIT) != 0) {
            git_reference_free(ref);
            continue;
        }

        if (out->count >= cap) {
            size_t ncap = cap ? cap * 2 : 16;
            BranchInfo *tmp = realloc(out->branches, ncap * sizeof(*tmp));
            if (!tmp) { git_object_free(tip_obj); git_reference_free(ref); break; }
            out->branches = tmp;
            cap = ncap;
        }
        BranchInfo *b = &out->branches[out->count++];
        memset(b, 0, sizeof(*b));

        const git_oid *tip = git_object_id(tip_obj);
        snprintf(b->name, sizeof(b->name), "%s", name);
        b->is_head      = git_branch_is_head(ref) == 1;
        b->is_default   = have_default && strcmp(name, out->default_branch) == 0;
        b->is_protected = is_protected(name, prot, nprot);
        b->last_commit  = git_commit_time((git_commit *)tip_obj);
        fill_upstream(b, repo, ref, tip);

        /* the checked-out, default and protected branches are never stale */
        if (!b->is_head && !b->is_default && !b->is_protected) {
            /* MERGED: tip reachable from default. A tip equal to default is a
             * fresh branch with no work yet — not stale. */
            if (have_default && git_oid_cmp(tip, &default_oid) != 0 &&
                git_graph_descendant_of(repo, &default_oid, tip) == 1)
                b->reason = STR_MERGED;

            /* SQUASHED: cumulative branch diff matches a default commit */
            if (b->reason == STR_NONE && have_default &&
                git_oid_cmp(tip, &default_oid) != 0) {
                if (!default_pids_built) {
                    build_default_patchids(repo, &default_oid,
                                           &default_pids, &default_pid_count);
                    default_pids_built = true;
                }
                git_oid bpid;
                if (default_pid_count > 0 &&
                    branch_cumulative_patchid(repo, tip, &default_oid, &bpid) == 0 &&
                    patchid_in_set(&bpid, default_pids, default_pid_count))
                    b->reason = STR_SQUASHED;
            }

            b->safe_delete = (b->reason == STR_MERGED || b->reason == STR_SQUASHED);

            /* GONE only when the work is not already accounted for above — a
             * squash-merged branch whose remote was deleted shows as squashed
             * (the UPSTREAM column still says gone) and stays safe to delete */
            if (b->reason == STR_NONE && b->upstream_gone)
                b->reason = STR_GONE;
        }

        git_object_free(tip_obj);
        git_reference_free(ref);
    }

    free(default_pids);
    git_branch_iterator_free(it);
    git_repository_free(repo);

    if (out->count > 1)
        qsort(out->branches, out->count, sizeof(BranchInfo), cmp_branch);
    return 0;
}

void branch_list_free(BranchList *bl) {
    free(bl->branches);
    bl->branches = NULL;
    bl->count = 0;
}

const char *stale_reason_str(StaleReason r) {
    switch (r) {
        case STR_GONE:     return "gone";
        case STR_MERGED:   return "merged";
        case STR_SQUASHED: return "squashed";
        default:           return "";
    }
}
