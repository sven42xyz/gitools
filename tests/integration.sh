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
check      "output"   "gitls 0.2.0" "$GITLS" --version

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

# ── cleanup ───────────────────────────────────────────────────────────────────
rm -rf "$WORK"
printf "\n%d passed, %d failed\n" "$passed" "$failed"
[ "$failed" -eq 0 ]
