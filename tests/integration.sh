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
# FETCH_SETUP pushes the initial commit so FETCH_REPO can clone a non-empty repo
FETCH_SETUP="$WORK/fetch-setup"
git clone -q "$BARE" "$FETCH_SETUP"
git -C "$FETCH_SETUP" config user.email "test@gitls.test"
git -C "$FETCH_SETUP" config user.name "Test"
printf 'init\n' > "$FETCH_SETUP/README"
git -C "$FETCH_SETUP" add README
git -C "$FETCH_SETUP" commit -q -m "init"
git -C "$FETCH_SETUP" push -q origin HEAD
FETCH_REPO="$WORK/fetch-repo"
git clone -q "$BARE" "$FETCH_REPO"
git -C "$FETCH_REPO" config user.email "test@gitls.test"
git -C "$FETCH_REPO" config user.name "Test"
# FETCH_SETUP pushes another commit; FETCH_REPO is now behind → fetch brings new data
printf 'second\n' >> "$FETCH_SETUP/README"
git -C "$FETCH_SETUP" add README
git -C "$FETCH_SETUP" commit -q -m "second"
git -C "$FETCH_SETUP" push -q origin HEAD
check "fetched"      "fetched 1"    "$GITLS" --no-color fetch "$FETCH_REPO"
# second fetch: nothing new → up to date
check "up to date"   "up to date 1" "$GITLS" --no-color fetch "$FETCH_REPO"

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

# ── subcommand detection: option values not misidentified as subcommands ───────
printf "\nsubcommand detection\n"

# "gitls -s fetch <dir>" – "fetch" is the branch name for -s, not a subcommand
SUBFETCH="$WORK/subcmd-fetch"
mkgit "$SUBFETCH/repo"
git -C "$SUBFETCH/repo" branch fetch
check "-s with branch named 'fetch' switches" "switched" "$GITLS" --no-color -s fetch "$SUBFETCH"

# "gitls -s pull <dir>" – "pull" is the branch name for -s, not a subcommand
# (bug: without the fix this triggers pull+switch rejection)
SUBPULL="$WORK/subcmd-pull"
mkgit "$SUBPULL/repo"
git -C "$SUBPULL/repo" branch pull
check "-s with branch named 'pull' switches" "switched" "$GITLS" --no-color -s pull "$SUBPULL"

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

# ── config: default_dir overridden by explicit "." ────────────────────────────
printf "\nconfig: default_dir\n"
DOT_DIR="$WORK/dottest"
mkgit "$DOT_DIR/repo"
OTHER_DIR="$WORK/othertest"
mkgit "$OTHER_DIR/other"
printf "default_dir=%s\n" "$OTHER_DIR" > "$CFG"
# explicit "." should scan current dir, not default_dir
out=$(cd "$DOT_DIR" && GITLS_CONFIG="$CFG" "$GITLS" --no-color . 2>&1)
if printf '%s' "$out" | grep -qF "repo" && ! printf '%s' "$out" | grep -qF "other"; then
    printf "  ok  explicit . overrides default_dir\n"; passed=$((passed + 1))
else
    printf "FAIL  explicit . overrides default_dir\n     got: %s\n" "$out"
    failed=$((failed + 1))
fi

# ── stale: helper to set up a repo with main as the default branch ────────────
mkmain() {
    dir="$1"
    mkgit "$dir"
    # Rename to main so assertions match the default-branch resolution order.
    current=$(git -C "$dir" symbolic-ref --short HEAD 2>/dev/null)
    [ "$current" = "main" ] || git -C "$dir" branch -m main 2>/dev/null
}

# ── stale: no stale branches ──────────────────────────────────────────────────
printf "\nstale: none\n"
D="$WORK/stale-none"; mkmain "$D"
check "no stale branches" "No stale branches found" "$GITLS" --no-color stale "$D"

# ── stale: merged branch detected ─────────────────────────────────────────────
printf "\nstale: merged branch\n"
D="$WORK/stale-merged"; mkmain "$D"
git -C "$D" checkout -q -b feature-m
git -C "$D" commit -q --allow-empty -m "feature work"
git -C "$D" checkout -q main
git -C "$D" merge -q --no-ff feature-m -m "merge"
out=$("$GITLS" --no-color stale "$D" 2>&1)
if printf '%s' "$out" | grep -qE "merged.*feature-m"; then
    printf "  ok  merged branch listed\n"; passed=$((passed + 1))
else
    printf "FAIL  merged branch listed\n     got: %s\n" "$out"
    failed=$((failed + 1))
fi

# ── stale: gone upstream detected ─────────────────────────────────────────────
printf "\nstale: gone upstream\n"
BARE_G="$WORK/stale-gone-bare.git"
git init --bare -q "$BARE_G"
D="$WORK/stale-gone"
git clone -q "$BARE_G" "$D" 2>/dev/null
git -C "$D" config user.email "test@gitls.test"
git -C "$D" config user.name "Test"
git -C "$D" commit -q --allow-empty -m "init"
current=$(git -C "$D" symbolic-ref --short HEAD)
[ "$current" = "main" ] || git -C "$D" branch -m main
git -C "$D" push -q origin main
git -C "$D" checkout -q -b doomed
git -C "$D" commit -q --allow-empty -m "doomed"
git -C "$D" push -q -u origin doomed
git -C "$D" checkout -q main
git -C "$D" push -q origin --delete doomed
git -C "$D" remote prune origin >/dev/null
out=$("$GITLS" --no-color stale "$D" 2>&1)
if printf '%s' "$out" | grep -qE "gone.*doomed"; then
    printf "  ok  gone branch listed\n"; passed=$((passed + 1))
else
    printf "FAIL  gone branch listed\n     got: %s\n" "$out"
    failed=$((failed + 1))
fi

# ── stale: invalid combination ────────────────────────────────────────────────
printf "\nstale: invalid combination\n"
check_exit "stale + -s rejected" 1 "$GITLS" stale -s foo "$WORK/stale-none"

# ── stale --prune: requires stale ─────────────────────────────────────────────
printf "\n--prune without stale\n"
check_exit "rejected" 1 "$GITLS" --prune "$WORK/stale-none"

# ── stale --prune --yes: deletes merged branch ────────────────────────────────
printf "\nstale --prune: merged\n"
D="$WORK/prune-merged"; mkmain "$D"
git -C "$D" checkout -q -b f-merged
git -C "$D" commit -q --allow-empty -m "merged work"
git -C "$D" checkout -q main
git -C "$D" merge -q --no-ff f-merged -m "merge"
out=$("$GITLS" --no-color stale --prune --yes "$D" 2>&1)
if printf '%s' "$out" | grep -qE "deleted.*f-merged" &&
   ! git -C "$D" rev-parse --verify -q f-merged >/dev/null; then
    printf "  ok  merged branch deleted\n"; passed=$((passed + 1))
else
    printf "FAIL  merged branch deleted\n     got: %s\n     branch still exists: %s\n" \
        "$out" "$(git -C "$D" branch)"
    failed=$((failed + 1))
fi

# ── stale: protected_branches config skips listed branches ───────────────────
printf "\nstale: protected_branches config\n"
D="$WORK/stale-protected"; mkmain "$D"
git -C "$D" checkout -q -b develop
git -C "$D" commit -q --allow-empty -m "dev work"
git -C "$D" checkout -q main
git -C "$D" merge -q --no-ff develop -m "merge develop"
# Without the config, develop would be listed as merged.
printf 'protected_branches=develop\n' > "$CFG"
check "develop omitted via config" "No stale branches found" \
    env GITLS_CONFIG="$CFG" "$GITLS" --no-color stale "$D"

# ── stale: main/master implicitly protected ───────────────────────────────────
printf "\nstale: main/master implicit protection\n"
D="$WORK/stale-implicit"; mkgit "$D"
# Force default to master so a separate 'main' branch could otherwise be flagged
git -C "$D" branch -m master 2>/dev/null
git -C "$D" checkout -q -b main
git -C "$D" commit -q --allow-empty -m "extra"
git -C "$D" checkout -q master
git -C "$D" merge -q --no-ff main -m "merge main into master"
# Both default (master) and 'main' should be excluded.
check "main not flagged on master-default repo" "No stale branches found" \
    "$GITLS" --no-color stale "$D"

# ── stale: squash-merged branch detected ──────────────────────────────────────
printf "\nstale: squash-merged\n"
D="$WORK/stale-squash"; mkmain "$D"
git -C "$D" checkout -q -b feat-squash
printf 'a\n' > "$D/a.txt"
git -C "$D" add a.txt
git -C "$D" commit -q -m "A"
printf 'b\n' > "$D/b.txt"
git -C "$D" add b.txt
git -C "$D" commit -q -m "B"
git -C "$D" checkout -q main
git -C "$D" merge --squash feat-squash -q
git -C "$D" commit -q -m "squashed"
out=$("$GITLS" --no-color stale "$D" 2>&1)
if printf '%s' "$out" | grep -qE "squash.*feat-squash"; then
    printf "  ok  squash-merged listed\n"; passed=$((passed + 1))
else
    printf "FAIL  squash-merged listed\n     got: %s\n" "$out"
    failed=$((failed + 1))
fi

# ── stale: squash + extra commits NOT flagged (negative) ──────────────────────
printf "\nstale: squash + extra commits\n"
D="$WORK/stale-squash-extra"; mkmain "$D"
git -C "$D" checkout -q -b feat-extra
printf 'a\n' > "$D/a.txt"
git -C "$D" add a.txt
git -C "$D" commit -q -m "A"
git -C "$D" checkout -q main
git -C "$D" merge --squash feat-extra -q
git -C "$D" commit -q -m "squashed"
git -C "$D" checkout -q feat-extra
printf 'x\n' > "$D/extra.txt"
git -C "$D" add extra.txt
git -C "$D" commit -q -m "post-squash work"
git -C "$D" checkout -q main
check "branch with post-squash work not flagged" "No stale branches found" \
    "$GITLS" --no-color stale "$D"

# ── stale --prune --yes: deletes squash-merged ────────────────────────────────
printf "\nstale --prune: squash-merged\n"
D="$WORK/prune-squash"; mkmain "$D"
git -C "$D" checkout -q -b f-sq
printf 'a\n' > "$D/a.txt"
git -C "$D" add a.txt
git -C "$D" commit -q -m "A"
git -C "$D" checkout -q main
git -C "$D" merge --squash f-sq -q
git -C "$D" commit -q -m "squashed"
out=$("$GITLS" --no-color stale --prune --yes "$D" 2>&1)
if printf '%s' "$out" | grep -qE "deleted.*f-sq" &&
   ! git -C "$D" rev-parse --verify -q f-sq >/dev/null; then
    printf "  ok  squash-merged deleted\n"; passed=$((passed + 1))
else
    printf "FAIL  squash-merged deleted\n     got: %s\n" "$out"
    failed=$((failed + 1))
fi

# ── stale --older-than: recent stale not flagged ──────────────────────────────
printf "\nstale --older-than: recent\n"
D="$WORK/stale-recent"; mkmain "$D"
git -C "$D" checkout -q -b recent-merged
git -C "$D" commit -q --allow-empty -m "fresh"
git -C "$D" checkout -q main
git -C "$D" merge -q --no-ff recent-merged -m "merge"
# Recent commit (now) should be filtered out with --older-than 1d
check "recent merged filtered" "No stale branches found" \
    "$GITLS" --no-color stale --older-than 1d "$D"

# ── stale --older-than: old stale still flagged ───────────────────────────────
printf "\nstale --older-than: old\n"
D="$WORK/stale-old"; mkmain "$D"
git -C "$D" checkout -q -b old-merged
# Backdate the tip commit by 10 days so --older-than 1d retains it.
OLD_DATE="$(date -u -v-10d '+%Y-%m-%dT%H:%M:%S' 2>/dev/null || date -u -d '10 days ago' '+%Y-%m-%dT%H:%M:%S')"
GIT_AUTHOR_DATE="$OLD_DATE" GIT_COMMITTER_DATE="$OLD_DATE" \
    git -C "$D" commit -q --allow-empty -m "old work"
git -C "$D" checkout -q main
git -C "$D" merge -q --no-ff old-merged -m "merge"
out=$("$GITLS" --no-color stale --older-than 1d "$D" 2>&1)
if printf '%s' "$out" | grep -qE "merged.*old-merged"; then
    printf "  ok  old merged retained\n"; passed=$((passed + 1))
else
    printf "FAIL  old merged retained\n     got: %s\n" "$out"
    failed=$((failed + 1))
fi

# ── stale --older-than: invalid duration rejected ─────────────────────────────
printf "\nstale --older-than: invalid duration\n"
check_exit "invalid duration" 1 "$GITLS" stale --older-than xyz "$WORK/stale-recent"
check_exit "zero duration rejected" 1 "$GITLS" stale --older-than 0d "$WORK/stale-recent"

# ── --older-than without stale ────────────────────────────────────────────────
printf "\n--older-than without stale\n"
check_exit "rejected" 1 "$GITLS" --older-than 1d "$WORK/stale-recent"

# ── stale --yes without --prune rejected ──────────────────────────────────────
printf "\nstale --yes without --prune\n"
check_exit "rejected" 1 "$GITLS" stale --yes "$WORK/stale-recent"

# ── stale: custom default branch (origin/HEAD points elsewhere) ───────────────
printf "\nstale: custom default branch\n"
BARE_C="$WORK/custom-bare.git"
git init --bare -q "$BARE_C"
D="$WORK/custom-default"
git clone -q "$BARE_C" "$D" 2>/dev/null
git -C "$D" config user.email "test@gitls.test"
git -C "$D" config user.name "Test"
git -C "$D" commit -q --allow-empty -m "init"
# Use 'develop' as the default branch — neither main nor master.
current=$(git -C "$D" symbolic-ref --short HEAD)
git -C "$D" branch -m "$current" develop
git -C "$D" push -q -u origin develop
git -C "$BARE_C" symbolic-ref HEAD refs/heads/develop
git -C "$D" remote set-head origin develop >/dev/null
git -C "$D" checkout -q -b feat-c
git -C "$D" commit -q --allow-empty -m "feat work"
git -C "$D" checkout -q develop
git -C "$D" merge -q --no-ff feat-c -m "merge"
out=$("$GITLS" --no-color stale "$D" 2>&1)
if printf '%s' "$out" | grep -qE "default: develop" &&
   printf '%s' "$out" | grep -qE "merged.*feat-c"; then
    printf "  ok  custom default branch resolved\n"; passed=$((passed + 1))
else
    printf "FAIL  custom default branch resolved\n     got: %s\n" "$out"
    failed=$((failed + 1))
fi

# ── config: protected_branches trims whitespace ───────────────────────────────
printf "\nconfig: protected_branches whitespace\n"
D="$WORK/stale-trim"; mkmain "$D"
git -C "$D" checkout -q -b release
git -C "$D" commit -q --allow-empty -m "release work"
git -C "$D" checkout -q main
git -C "$D" merge -q --no-ff release -m "merge"
# Note the leading space — should still match the branch name "release".
printf 'protected_branches=develop, release\n' > "$CFG"
check "trimmed token matches" "No stale branches found" \
    env GITLS_CONFIG="$CFG" "$GITLS" --no-color stale "$D"

# ── stale --prune --yes: refuses gone+unmerged ────────────────────────────────
printf "\nstale --prune: refused unmerged\n"
BARE_U="$WORK/prune-unmerged-bare.git"
git init --bare -q "$BARE_U"
D="$WORK/prune-unmerged"
git clone -q "$BARE_U" "$D" 2>/dev/null
git -C "$D" config user.email "test@gitls.test"
git -C "$D" config user.name "Test"
git -C "$D" commit -q --allow-empty -m "init"
current=$(git -C "$D" symbolic-ref --short HEAD)
[ "$current" = "main" ] || git -C "$D" branch -m main
git -C "$D" push -q origin main
git -C "$D" checkout -q -b orphan
git -C "$D" commit -q --allow-empty -m "orphan work"
git -C "$D" push -q -u origin orphan
git -C "$D" push -q origin --delete orphan
git -C "$D" remote prune origin >/dev/null
git -C "$D" checkout -q main
out=$("$GITLS" --no-color stale --prune --yes "$D" 2>&1)
if printf '%s' "$out" | grep -qE "refused.*orphan" &&
   git -C "$D" rev-parse --verify -q orphan >/dev/null; then
    printf "  ok  gone+unmerged refused\n"; passed=$((passed + 1))
else
    printf "FAIL  gone+unmerged refused\n     got: %s\n" "$out"
    failed=$((failed + 1))
fi

# ── cleanup ───────────────────────────────────────────────────────────────────
rm -rf "$WORK"
printf "\n%d passed, %d failed\n" "$passed" "$failed"
[ "$failed" -eq 0 ]
