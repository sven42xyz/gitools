#!/bin/sh
# tests/integration.sh – gitls integration tests

GITLS="$(cd "$(dirname "$0")/.." && pwd)/gitls"
WORK=$(mktemp -d)
passed=0
failed=0

# ── helpers ───────────────────────────────────────────────────────────────────

check() {
    desc="$1"; expected="$2"; shift 2
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
    mkdir -p "$dir"
    git -C "$dir" init -q
    git -C "$dir" config user.email "test@gitls.test"
    git -C "$dir" config user.name "Test"
    printf 'init\n' > "$dir/README"
    git -C "$dir" add README
    git -C "$dir" commit -q -m "init"
}

# ── version ───────────────────────────────────────────────────────────────────
printf "version\n"
check_exit "exit 0"   0 "$GITLS" --version
check      "output"   "gitls 0.1.0" "$GITLS" --version

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

check "switched"          "switched"         "$GITLS" --no-color -s feature "$SD"
check "already on branch" "already on branch" "$GITLS" --no-color -s feature "$SD"
check "skipped dirty"     "skipped"          "$GITLS" --no-color -s feature "$SD"
check "branch not found"  "branch not found" "$GITLS" --no-color -s feature "$SD"

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

# ── cleanup ───────────────────────────────────────────────────────────────────
rm -rf "$WORK"
printf "\n%d passed, %d failed\n" "$passed" "$failed"
[ "$failed" -eq 0 ]
