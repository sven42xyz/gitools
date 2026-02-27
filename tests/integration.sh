#!/bin/sh
# tests/integration.sh – gitls integration tests

GITLS="$(cd "$(dirname "$0")/.." && pwd)/gitls"
WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT
passed=0
failed=0

# ── helpers ───────────────────────────────────────────────────────────────────

check() {
    desc="$1"; expected="$2"; shift 2
    if [ -z "$expected" ]; then
        printf "FAIL  %s: empty expected string\n" "$desc"
        failed=$((failed + 1))
        return
    fi
    out=$("$@" 2>&1)
    if printf '%s' "$out" | grep -qF "$expected"; then
        printf "  ok  %s\n" "$desc"
        passed=$((passed + 1))
    else
        printf "FAIL  %s\n     expected: %s\n     got:      %s\n" \
               "$desc" "$expected" "$out"
        failed=$((failed + 1))
    fi
}

check_exit() {
    desc="$1"; want="$2"; shift 2
    "$@" >/dev/null 2>&1; got=$?
    if [ "$got" -eq "$want" ]; then
        printf "  ok  %s\n" "$desc"
        passed=$((passed + 1))
    else
        printf "FAIL  %s  (exit %d, want %d)\n" "$desc" "$got" "$want"
        failed=$((failed + 1))
    fi
}

mkgit() {
    dir="$1"
    mkdir -p "$dir" &&
    git -C "$dir" init -q &&
    git -C "$dir" config user.email "test@gitls.test" &&
    git -C "$dir" config user.name "Test" &&
    printf 'init\n' > "$dir/README" &&
    git -C "$dir" add README &&
    git -C "$dir" commit -q -m "init" || {
        printf "mkgit: setup failed for %s\n" "$dir" >&2
        exit 1
    }
}

# ── version ───────────────────────────────────────────────────────────────────
printf "version\n"
check_exit "exit 0"   0 "$GITLS" --version
check      "output"   "gitls " "$GITLS" --version

# ── no repos ──────────────────────────────────────────────────────────────────
printf "\nno repos\n"
D="$WORK/empty"; mkdir -p "$D"
check "no repos found" "No git repositories found" "$GITLS" --no-color "$D"

# ── clean repo ────────────────────────────────────────────────────────────────
printf "\nclean repo\n"
D="$WORK/clean"; mkgit "$D"
check "checkmark" "✓" "$GITLS" --no-color "$D"

# ── modified file ─────────────────────────────────────────────────────────────
printf "\nmodified file\n"
D="$WORK/modified"; mkgit "$D"
printf 'change\n' >> "$D/README"
check "modified indicator" "✗1" "$GITLS" --no-color "$D"

# ── staged file ───────────────────────────────────────────────────────────────
printf "\nstaged file\n"
D="$WORK/staged"; mkgit "$D"
printf 'change\n' >> "$D/README"
git -C "$D" add README
check "staged indicator" "●1" "$GITLS" --no-color "$D"

# ── untracked file ────────────────────────────────────────────────────────────
printf "\nuntracked file\n"
D="$WORK/untracked"; mkgit "$D"
printf 'new\n' > "$D/new.txt"
check "untracked indicator" "?1" "$GITLS" --no-color "$D"

# ── branch switch ─────────────────────────────────────────────────────────────
printf "\nbranch switch\n"
SD="$WORK/switch"

# clean repo with target branch → switched
mkgit "$SD/repo-has-branch"
git -C "$SD/repo-has-branch" branch feature

# already on target branch → already on branch
mkgit "$SD/repo-already"
git -C "$SD/repo-already" branch feature
git -C "$SD/repo-already" checkout -q feature

# no target branch → branch not found
mkgit "$SD/repo-no-branch"

# dirty + has target branch → skipped
mkgit "$SD/repo-dirty"
git -C "$SD/repo-dirty" branch feature
printf 'dirty\n' >> "$SD/repo-dirty/README"

check "switched"          "switched"          "$GITLS" --no-color    -s feature "$SD"
check "already on branch" "already on branch" "$GITLS" --no-color -v -s feature "$SD"
check "skipped dirty"     "skipped"           "$GITLS" --no-color    -s feature "$SD"
check "branch not found"  "branch not found"  "$GITLS" --no-color -v -s feature "$SD"
check "not verbose: already in summary" "already 2" "$GITLS" --no-color -s feature "$SD"

# ── fetch -s: create tracking branch from remote ──────────────────────────────
printf "\nfetch -s: create tracking branch\n"
BARE_FS="$WORK/fetchswitch-origin.git"
git init --bare -q "$BARE_FS"
FS_SETUP="$WORK/fetchswitch-setup"
git clone -q "$BARE_FS" "$FS_SETUP"
git -C "$FS_SETUP" config user.email "test@gitls.test"
git -C "$FS_SETUP" config user.name "Test"
printf 'init\n' > "$FS_SETUP/README"
git -C "$FS_SETUP" add README
git -C "$FS_SETUP" commit -q -m "init"
git -C "$FS_SETUP" push -q origin HEAD
# create feature branch on remote only
git -C "$FS_SETUP" checkout -q -b feature
printf 'feature\n' >> "$FS_SETUP/README"
git -C "$FS_SETUP" add README
git -C "$FS_SETUP" commit -q -m "feature commit"
git -C "$FS_SETUP" push -q origin feature
# clone without feature branch locally
FS_REPO="$WORK/fetchswitch-repo"
git clone -q "$BARE_FS" "$FS_REPO"
git -C "$FS_REPO" config user.email "test@gitls.test"
git -C "$FS_REPO" config user.name "Test"
check "created & switched" "created & switched" "$GITLS" --no-color fetch -s feature "$FS_REPO"

# ── fetch -s: branch already exists locally → switched (not created) ──────────
printf "\nfetch -s: branch exists locally\n"
BARE_FSL="$WORK/fetchswitch-local-origin.git"
git init --bare -q "$BARE_FSL"
FSL_SETUP="$WORK/fetchswitch-local-setup"
git clone -q "$BARE_FSL" "$FSL_SETUP"
git -C "$FSL_SETUP" config user.email "test@gitls.test"
git -C "$FSL_SETUP" config user.name "Test"
printf 'init\n' > "$FSL_SETUP/README"
git -C "$FSL_SETUP" add README
git -C "$FSL_SETUP" commit -q -m "init"
git -C "$FSL_SETUP" push -q origin HEAD
FSL_REPO="$WORK/fetchswitch-local-repo"
git clone -q "$BARE_FSL" "$FSL_REPO"
git -C "$FSL_REPO" config user.email "test@gitls.test"
git -C "$FSL_REPO" config user.name "Test"
git -C "$FSL_REPO" branch feature   # local branch already exists
check "switched (not created)" "✓ switched" "$GITLS" --no-color fetch -s feature "$FSL_REPO"

# ── fetch -s: branch not found anywhere ───────────────────────────────────────
printf "\nfetch -s: branch not found\n"
BARE_FNF="$WORK/fetchswitch-notfound-origin.git"
git init --bare -q "$BARE_FNF"
FNF_REPO="$WORK/fetchswitch-notfound-repo"
git clone -q "$BARE_FNF" "$FNF_REPO"
git -C "$FNF_REPO" config user.email "test@gitls.test"
git -C "$FNF_REPO" config user.name "Test"
printf 'init\n' > "$FNF_REPO/README"
git -C "$FNF_REPO" add README
git -C "$FNF_REPO" commit -q -m "init"
check "branch not found" "not found 1" "$GITLS" --no-color fetch -s ghost "$FNF_REPO"

# ── fetch -s: dirty repo → skipped ────────────────────────────────────────────
printf "\nfetch -s: dirty repo skipped\n"
BARE_FSD="$WORK/fetchswitch-dirty-origin.git"
git init --bare -q "$BARE_FSD"
FSD_SETUP="$WORK/fetchswitch-dirty-setup"
git clone -q "$BARE_FSD" "$FSD_SETUP"
git -C "$FSD_SETUP" config user.email "test@gitls.test"
git -C "$FSD_SETUP" config user.name "Test"
printf 'init\n' > "$FSD_SETUP/README"
git -C "$FSD_SETUP" add README
git -C "$FSD_SETUP" commit -q -m "init"
git -C "$FSD_SETUP" checkout -q -b feature
git -C "$FSD_SETUP" push -q origin feature
FSD_REPO="$WORK/fetchswitch-dirty-repo"
git clone -q "$BARE_FSD" "$FSD_REPO"
git -C "$FSD_REPO" config user.email "test@gitls.test"
git -C "$FSD_REPO" config user.name "Test"
printf 'dirty\n' >> "$FSD_REPO/README"
check "dirty skipped" "skipped" "$GITLS" --no-color fetch -s feature "$FSD_REPO"

# ── fetch: ahead/behind updates after fetch ────────────────────────────────────
printf "\nfetch: ahead/behind indicator\n"
BARE_AB="$WORK/aheadbehind-origin.git"
git init --bare -q "$BARE_AB"
AB_SETUP="$WORK/aheadbehind-setup"
git clone -q "$BARE_AB" "$AB_SETUP"
git -C "$AB_SETUP" config user.email "test@gitls.test"
git -C "$AB_SETUP" config user.name "Test"
printf 'init\n' > "$AB_SETUP/README"
git -C "$AB_SETUP" add README
git -C "$AB_SETUP" commit -q -m "init"
git -C "$AB_SETUP" push -q origin HEAD
AB_REPO="$WORK/aheadbehind-repo"
git clone -q "$BARE_AB" "$AB_REPO"
git -C "$AB_REPO" config user.email "test@gitls.test"
git -C "$AB_REPO" config user.name "Test"
# push a new commit to origin so AB_REPO is behind
printf 'remote\n' >> "$AB_SETUP/README"
git -C "$AB_SETUP" add README
git -C "$AB_SETUP" commit -q -m "remote commit"
git -C "$AB_SETUP" push -q origin HEAD
check "behind shown after fetch" "↓1" "$GITLS" --no-color fetch "$AB_REPO"

# ── flags ─────────────────────────────────────────────────────────────────────
printf "\nflags\n"
D="$WORK/empty"
check_exit "-d accepted"         0 "$GITLS" -d 2 "$D"
check_exit "--no-color accepted" 0 "$GITLS" --no-color "$D"
check      "bad flag error"      "Unknown option" "$GITLS" --bad-flag "$D"

# ── fetch: no remote ──────────────────────────────────────────────────────────
printf "\nfetch: no remote\n"
D="$WORK/fetch-noremote"; mkgit "$D"
check "no remote" "no remote" "$GITLS" --no-color fetch "$D"

# ── fetch: with remote ────────────────────────────────────────────────────────
printf "\nfetch: with remote\n"
BARE="$WORK/fetch-origin.git"
mkdir -p "$BARE"
git init --bare -q "$BARE"
FETCH_REPO="$WORK/fetch-repo"
git clone -q "$BARE" "$FETCH_REPO"
git -C "$FETCH_REPO" config user.email "test@gitls.test"
git -C "$FETCH_REPO" config user.name "Test"
printf 'init\n' > "$FETCH_REPO/README"
git -C "$FETCH_REPO" add README
git -C "$FETCH_REPO" commit -q -m "init"
git -C "$FETCH_REPO" push -q origin HEAD
check "fetched" "fetched" "$GITLS" --no-color fetch "$FETCH_REPO"

# ── pull: no remote ───────────────────────────────────────────────────────────
printf "\npull: no remote\n"
D="$WORK/pull-noremote"; mkgit "$D"
check "no remote" "no remote" "$GITLS" --no-color pull "$D"

# ── pull: dirty skipped ───────────────────────────────────────────────────────
printf "\npull: dirty skipped\n"
D="$WORK/pull-dirty"; mkgit "$D"
printf 'dirty\n' >> "$D/README"
check "skipped dirty" "skipped" "$GITLS" --no-color pull "$D"

# ── pull: fast-forward ────────────────────────────────────────────────────────
printf "\npull: fast-forward\n"
BARE2="$WORK/pull-origin.git"
mkdir -p "$BARE2"
git init --bare -q "$BARE2"
PULL_SETUP="$WORK/pull-setup"
git clone -q "$BARE2" "$PULL_SETUP"
git -C "$PULL_SETUP" config user.email "test@gitls.test"
git -C "$PULL_SETUP" config user.name "Test"
printf 'init\n' > "$PULL_SETUP/README"
git -C "$PULL_SETUP" add README
git -C "$PULL_SETUP" commit -q -m "init"
git -C "$PULL_SETUP" push -q origin HEAD
PULL_REPO="$WORK/pull-repo"
git clone -q "$BARE2" "$PULL_REPO"
git -C "$PULL_REPO" config user.email "test@gitls.test"
git -C "$PULL_REPO" config user.name "Test"
# add a commit to origin so pull_repo is behind
printf 'remote change\n' >> "$PULL_SETUP/README"
git -C "$PULL_SETUP" add README
git -C "$PULL_SETUP" commit -q -m "remote commit"
git -C "$PULL_SETUP" push -q origin HEAD
check "fast-forward pulled" "pulled" "$GITLS" --no-color pull "$PULL_REPO"

# ── pull: not fast-forward ────────────────────────────────────────────────────
printf "\npull: not fast-forward\n"
BARE3="$WORK/notff-origin.git"
mkdir -p "$BARE3"
git init --bare -q "$BARE3"
NOTFF_SETUP="$WORK/notff-setup"
git clone -q "$BARE3" "$NOTFF_SETUP"
git -C "$NOTFF_SETUP" config user.email "test@gitls.test"
git -C "$NOTFF_SETUP" config user.name "Test"
printf 'init\n' > "$NOTFF_SETUP/README"
git -C "$NOTFF_SETUP" add README
git -C "$NOTFF_SETUP" commit -q -m "init"
git -C "$NOTFF_SETUP" push -q origin HEAD
NOTFF_REPO="$WORK/notff-repo"
git clone -q "$BARE3" "$NOTFF_REPO"
git -C "$NOTFF_REPO" config user.email "test@gitls.test"
git -C "$NOTFF_REPO" config user.name "Test"
# local diverging commit
printf 'local change\n' >> "$NOTFF_REPO/README"
git -C "$NOTFF_REPO" add README
git -C "$NOTFF_REPO" commit -q -m "local commit"
# remote diverging commit (different from local)
printf 'remote change\n' > "$NOTFF_SETUP/README2"
git -C "$NOTFF_SETUP" add README2
git -C "$NOTFF_SETUP" commit -q -m "remote commit"
git -C "$NOTFF_SETUP" push -q origin HEAD
check "not fast-forward" "not fast-forward" "$GITLS" --no-color pull "$NOTFF_REPO"

# ── config: max_depth ─────────────────────────────────────────────────────────
printf "\nconfig: max_depth\n"
CFG="$WORK/test.gitlsrc"
# nested repo at depth 1 (inside a subdir)
D="$WORK/cfgtest/sub/repo"; mkgit "$D"
# with max_depth=0 it must not be found; with default (5) it would be
printf 'max_depth=0\n' > "$CFG"
out=$(GITLS_CONFIG="$CFG" "$GITLS" --no-color "$WORK/cfgtest" 2>&1)
if printf '%s' "$out" | grep -qF "No git repositories found"; then
    printf "  ok  max_depth=0 blocks nested repos\n"; passed=$((passed + 1))
else
    printf "FAIL  max_depth=0 blocks nested repos\n     got: %s\n" "$out"
    failed=$((failed + 1))
fi

# ── pull -s: rejected ─────────────────────────────────────────────────────────
printf "\npull -s: rejected\n"
D="$WORK/pull-s-reject"; mkgit "$D"
check "pull -s error" "cannot be combined" "$GITLS" --no-color pull -s main "$D"

# ── detached HEAD ─────────────────────────────────────────────────────────────
printf "\ndetached HEAD\n"
D="$WORK/detached"; mkgit "$D"
SHA=$(git -C "$D" rev-parse HEAD)
git -C "$D" checkout -q "$SHA"
check "detached HEAD shown" "(" "$GITLS" --no-color "$D"

# ── fetch -v: shows per-repo no-remote line (hidden without -v) ───────────────
printf "\nfetch -v: shows no-remote\n"
D="$WORK/fetch-v-noremote"; mkgit "$D"
DNAME=$(basename "$D")
# without -v the per-repo line is omitted; only the summary count "no remote 1" appears
# with -v the per-repo line "  name  · no remote" is printed too
out_quiet=$("$GITLS" --no-color fetch "$D" 2>&1)
out_verbose=$("$GITLS" --no-color fetch -v "$D" 2>&1)
# check for the per-repo result line which includes the repo name followed by · no remote
if ! printf '%s' "$out_quiet" | grep -qF "$DNAME  · no remote" && \
     printf '%s' "$out_verbose" | grep -qF "$DNAME  · no remote"; then
    printf "  ok  fetch -v shows per-repo no-remote line\n"; passed=$((passed + 1))
else
    printf "FAIL  fetch -v shows per-repo no-remote line\n     quiet: %s\n     verbose: %s\n" \
           "$out_quiet" "$out_verbose"
    failed=$((failed + 1))
fi

# ── pull -v: shows up-to-date ─────────────────────────────────────────────────
printf "\npull -v: shows up-to-date\n"
BARE_VP="$WORK/pull-v-origin.git"
git init --bare -q "$BARE_VP"
VP_REPO="$WORK/pull-v-repo"
git clone -q "$BARE_VP" "$VP_REPO"
git -C "$VP_REPO" config user.email "test@gitls.test"
git -C "$VP_REPO" config user.name "Test"
printf 'init\n' > "$VP_REPO/README"
git -C "$VP_REPO" add README
git -C "$VP_REPO" commit -q -m "init"
git -C "$VP_REPO" push -q origin HEAD
check "pull -v shows up to date" "up to date" "$GITLS" --no-color pull -v "$VP_REPO"

# ── config: skip_dirs ─────────────────────────────────────────────────────────
printf "\nconfig: skip_dirs\n"
SKIP_DIR="$WORK/skiptest"
mkgit "$SKIP_DIR/keep"
mkgit "$SKIP_DIR/ignore"
printf 'skip_dirs=ignore\n' > "$CFG"
out=$(GITLS_CONFIG="$CFG" "$GITLS" --no-color "$SKIP_DIR" 2>&1)
if printf '%s' "$out" | grep -qF "keep" && ! printf '%s' "$out" | grep -qF "ignore"; then
    printf "  ok  skip_dirs omits matched dir\n"; passed=$((passed + 1))
else
    printf "FAIL  skip_dirs omits matched dir\n     got: %s\n" "$out"
    failed=$((failed + 1))
fi

# ── cleanup ───────────────────────────────────────────────────────────────────
rm -rf "$WORK"
printf "\n%d passed, %d failed\n" "$passed" "$failed"
[ "$failed" -eq 0 ]
