#!/usr/bin/env python3
"""
tests/watch_pty.py — PTY-driven tests for gitls watch mode (-w).

Watch mode is interactive (alternate screen, raw termios, key handling), so it
can't be exercised from the plain-pipe integration.sh harness. This script
drives the binary through a pseudo-terminal: it presses keys, reads the
rendered frames and checks the branch picker, the f/p/s/r keys and that the
terminal is restored on exit.

Run directly or via `make test` (skipped automatically if python3 is missing).
"""

import os
import pty
import select
import signal
import subprocess
import sys
import tempfile
import time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
GITLS = os.path.join(ROOT, "gitls")


class Term:
    """Tiny VT100-ish screen emulator — enough to render gitls watch frames and
    catch redraw artifacts (cursor moves, erase-line/display, CR/LF)."""

    def __init__(self, rows=48, cols=120):
        self.rows, self.cols = rows, cols
        self.grid = [[" "] * cols for _ in range(rows)]
        self.r = self.c = 0

    def _clamp(self):
        self.r = max(0, min(self.r, self.rows - 1))
        self.c = max(0, min(self.c, self.cols - 1))

    def feed(self, data):
        i, n = 0, len(data)
        while i < n:
            ch = data[i]
            if ch == "\x1b" and i + 1 < n and data[i + 1] == "[":
                j = i + 2
                if j < n and data[j] == "?":          # private mode — skip
                    j += 1
                    while j < n and data[j] not in "hl":
                        j += 1
                    i = j + 1
                    continue
                params = ""
                while j < n and (data[j].isdigit() or data[j] == ";"):
                    params += data[j]
                    j += 1
                if j >= n:
                    break
                letter = data[j]
                i = j + 1
                nums = [int(x) for x in params.split(";") if x != ""]
                if letter == "H":
                    self.r = (nums[0] - 1) if len(nums) >= 1 else 0
                    self.c = (nums[1] - 1) if len(nums) >= 2 else 0
                    self._clamp()
                elif letter == "A":
                    self.r -= nums[0] if nums else 1; self._clamp()
                elif letter == "B":
                    self.r += nums[0] if nums else 1; self._clamp()
                elif letter == "C":
                    self.c += nums[0] if nums else 1; self._clamp()
                elif letter == "D":
                    self.c -= nums[0] if nums else 1; self._clamp()
                elif letter == "J":
                    if (nums[0] if nums else 0) == 0:
                        for cc in range(self.c, self.cols):
                            self.grid[self.r][cc] = " "
                        for rr in range(self.r + 1, self.rows):
                            self.grid[rr] = [" "] * self.cols
                    elif nums and nums[0] == 2:
                        self.grid = [[" "] * self.cols for _ in range(self.rows)]
                elif letter == "K":
                    if (nums[0] if nums else 0) == 0:
                        for cc in range(self.c, self.cols):
                            self.grid[self.r][cc] = " "
                # ignore SGR (m) and anything else
                continue
            if ch == "\r":
                self.c = 0
            elif ch == "\n":
                self.r += 1; self.c = 0; self._clamp()
            elif ch == "\x1b":
                pass
            elif ch >= " ":
                if self.r < self.rows and self.c < self.cols:
                    self.grid[self.r][self.c] = ch
                self.c += 1; self._clamp()
            i += 1

    def text(self):
        return "\n".join("".join(row).rstrip() for row in self.grid)

passed = 0
failed = 0


def check(desc, cond):
    global passed, failed
    if cond:
        print(f"  ok  {desc}")
        passed += 1
    else:
        print(f"FAIL  {desc}")
        failed += 1


def git(*args, cwd=None, env=None):
    subprocess.run(["git", *args], cwd=cwd, env=env, check=True,
                   stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)


def make_repo(path):
    os.makedirs(path, exist_ok=True)
    git("init", "-q", "-b", "main", cwd=path)
    git("config", "user.email", "t@gitls.test", cwd=path)
    git("config", "user.name", "Test", cwd=path)
    with open(os.path.join(path, "README"), "w") as f:
        f.write("init\n")
    git("add", "README", cwd=path)
    commit(path, "init", "2024-01-01T00:00:00")


def commit(path, msg, when):
    env = dict(os.environ, GIT_COMMITTER_DATE=when, GIT_AUTHOR_DATE=when)
    subprocess.run(["git", "commit", "-q", "-m", msg], cwd=path, env=env,
                   check=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)


def make_branch(path, name, when):
    git("checkout", "-q", "-b", name, cwd=path)
    with open(os.path.join(path, "README"), "a") as f:
        f.write(name + "\n")
    git("add", "README", cwd=path)
    commit(path, name, when)
    git("checkout", "-q", "main", cwd=path)


def current_branch(path):
    out = subprocess.run(["git", "branch", "--show-current"], cwd=path,
                         capture_output=True, text=True)
    return out.stdout.strip()


class Watcher:
    """Spawn gitls -w in a PTY and talk to it."""

    def __init__(self, workdir, interval=5):
        self.pid, self.fd = pty.fork()
        if self.pid == 0:
            os.execv(GITLS, [GITLS, "-w", str(interval), workdir])
            os._exit(127)

    def drain(self, seconds):
        buf = b""
        end = time.time() + seconds
        while time.time() < end:
            r, _, _ = select.select([self.fd], [], [], 0.15)
            if r:
                try:
                    d = os.read(self.fd, 65536)
                except OSError:
                    break
                if not d:
                    break
                buf += d
        return buf.decode(errors="replace")

    def send(self, data):
        os.write(self.fd, data)

    def finish(self):
        try:
            os.write(self.fd, b"q")
        except OSError:
            pass
        time.sleep(0.3)
        tail = self.drain(1.0)
        status = self._reap(timeout=3.0)
        return tail, status

    def _reap(self, timeout):
        """Bounded wait so a stuck child never hangs the suite."""
        deadline = time.time() + timeout
        while time.time() < deadline:
            pid, status = os.waitpid(self.pid, os.WNOHANG)
            if pid == self.pid:
                return status
            time.sleep(0.05)
        for sig in (signal.SIGTERM, signal.SIGKILL):
            try:
                os.kill(self.pid, sig)
            except ProcessLookupError:
                break
            time.sleep(0.3)
            pid, status = os.waitpid(self.pid, os.WNOHANG)
            if pid == self.pid:
                return status
        _, status = os.waitpid(self.pid, 0)
        return status


def ordered(text, names):
    pos = [text.find(n) for n in names]
    return all(p >= 0 for p in pos) and pos == sorted(pos)


def main():
    if not os.path.exists(GITLS):
        print("watch_pty: gitls binary not found — run `make` first", file=sys.stderr)
        return 1

    work = tempfile.mkdtemp(prefix="gitls-pty-")
    a = os.path.join(work, "a")
    b = os.path.join(work, "b")
    make_repo(a)
    make_branch(a, "oldfeature", "2024-02-01T00:00:00")
    make_branch(a, "develop",    "2024-06-01T00:00:00")
    make_branch(a, "hotfix",     "2025-01-01T00:00:00")
    # second repo shares main so a tree-wide switch is observable on both
    make_repo(b)
    make_branch(b, "develop", "2024-06-01T00:00:00")

    # ── 1. basic render + footer + terminal restore ──
    w = Watcher(work)
    first = w.drain(1.5)
    check("alternate screen entered", "\033[?1049h" in first)
    check("cursor hidden", "\033[?25l" in first)
    check("footer shows keys + interval", "fetch" in first and "interval 5s" in first)
    check("footer omits directory", work not in first.split("interval")[-1])
    check("footer omits 'last scan'", "last scan" not in first)
    # a/b are top-level repos (no categories) -> no nav hints in the footer
    check("footer hides nav without categories", "expand" not in first)
    tail, status = w.finish()
    check("alternate screen left on quit", "\033[?1049l" in (first + tail))
    check("cursor shown on quit", "\033[?25h" in (first + tail))
    check("clean exit on q", os.WIFEXITED(status) and os.WEXITSTATUS(status) == 0)

    # ── 2. picker opens, recent-first ordering, drawn below the table ──
    w = Watcher(work)
    w.drain(1.2)
    w.send(b"s")
    pick = w.drain(1.0)
    check("picker header shown", "switch all clean repos to:" in pick)
    check("picker lists recent branches first", ordered(pick, ["hotfix", "develop", "oldfeature"]))
    # the picker must appear after (below) the status table, not overwrite it
    check("picker drawn below table", pick.rfind("NAME") < pick.rfind("hotfix") or "NAME" not in pick)
    w.send(b"\x1b")  # Esc cancel
    w.drain(0.4)
    w.finish()
    check("Esc cancel leaves repos untouched", current_branch(a) == "main")

    # ── 3. arrow navigation + Tab selects across the tree ──
    git("checkout", "-q", "main", cwd=a)
    git("checkout", "-q", "main", cwd=b)
    w = Watcher(work)
    w.drain(1.2)
    w.send(b"s"); w.drain(0.4)
    w.send(b"\x1b[B"); w.drain(0.2)   # down -> develop
    w.send(b"\x1b[B"); w.drain(0.2)   # down -> oldfeature
    w.send(b"\t")                      # Tab selects highlighted
    w.drain(2.0)
    w.finish()
    check("arrow+Tab switches repo a to oldfeature", current_branch(a) == "oldfeature")

    # ── 4. type-to-filter + Enter selects the highlighted match ──
    git("checkout", "-q", "main", cwd=a)
    git("checkout", "-q", "main", cwd=b)
    w = Watcher(work)
    w.drain(1.2)
    w.send(b"s"); w.drain(0.4)
    w.send(b"dev"); w.drain(0.4)       # filter to develop
    w.send(b"\r")                       # Enter selects highlighted develop
    w.drain(2.0)
    w.finish()
    check("filter 'dev'+Enter switches a to develop", current_branch(a) == "develop")
    check("filter 'dev'+Enter switches b to develop", current_branch(b) == "develop")

    # ── 5. switching to a narrower branch leaves no stale columns ──
    # (regression: a wider previous frame used to leave a duplicate WHEN/STATUS)
    wide = tempfile.mkdtemp(prefix="gitls-pty-wide-")
    long_branch = "feature-with-a-really-long-branch-name"
    for name in ("ra", "rb"):
        p = os.path.join(wide, name)
        make_repo(p)
        git("checkout", "-q", "-b", long_branch, cwd=p)   # start on the long branch
    w = Watcher(wide)
    raw = w.drain(1.2)                     # wide frame
    w.send(b"s"); w.drain(0.4)
    w.send(b"main"); w.drain(0.3)
    w.send(b"\r")                          # switch to main -> narrow branch column
    raw += w.drain(1.5)
    w.finish()
    term = Term()
    term.feed(raw)
    screen = term.text()
    headers = [l for l in screen.splitlines() if "BRANCH" in l and "STATUS" in l]
    check("narrowing branch leaves no stale columns", long_branch[:12] not in screen)
    check("exactly one header row after switch", len(headers) == 1)
    subprocess.run(["rm", "-rf", wide])

    # ── 6. nested repos collapse under a category header ──
    grp = tempfile.mkdtemp(prefix="gitls-grp-")
    make_repo(os.path.join(grp, "flat"))
    make_repo(os.path.join(grp, "alpha"))                       # sorts before flat
    make_repo(os.path.join(grp, "core", "packages", "auth"))
    make_repo(os.path.join(grp, "core", "packages", "api"))
    open(os.path.join(grp, "core", "packages", "api", "scratch"), "w").write("x")  # dirty
    make_repo(os.path.join(grp, "lib", "a"))                    # all-clean category
    make_repo(os.path.join(grp, "lib", "b"))
    make_repo(os.path.join(grp, "solo", "widget"))             # lone repo -> folded flat
    w = Watcher(grp)
    raw = w.drain(1.5)
    s1 = (lambda t: (t.feed(raw), t.text())[1])(Term())

    def line_with(text, needle):
        return next((l for l in text.splitlines() if needle in l), "")

    check("flat repo shown at top", "flat" in s1)
    check("flat repos sorted alphabetically", ordered(s1, ["alpha", "flat", "widget"]))
    # category header interleaves alphabetically: core › packages sits between
    # the "alpha" and "flat" repos, not in a separate block below them
    check("category interleaved alphabetically",
          ordered(s1, ["alpha", "core › packages", "flat"]))
    check("single-repo folder folded to repo name", "widget" in s1 and "solo" not in s1)
    check("category header shown", "core › packages" in s1)
    check("category header shows count", "(2)" in s1)
    check("nested repos hidden by default", "auth" not in s1)
    # aggregated folder status: dirty count on the header, ✓ when all clean
    check("category aggregates dirty count", "●" in line_with(s1, "core › packages"))
    check("clean category shows check", "✓" in line_with(s1, "lib (2)"))
    # folder status and top-level repo status share the same STATUS column
    status_col = line_with(s1, "STATUS").index("STATUS")
    check("folder status aligned to STATUS column",
          line_with(s1, "core › packages").find("●") == status_col
          and line_with(s1, "lib (2)").find("✓") == status_col)
    check("top repo status aligned to STATUS column",
          line_with(s1, "alpha").find("✓") == status_col)
    # nav hints appear because categories exist
    check("footer shows nav with categories", "expand" in s1)
    # cursor starts unselected (-1): first ↓ selects row 0 (alpha), second ↓
    # reaches the header at row 1, then Enter expands
    w.send(b"\x1b[B"); w.drain(0.2)
    w.send(b"\x1b[B"); w.drain(0.2)
    w.send(b"\r")                      # enter: expand
    raw2 = w.drain(1.0)
    s2 = (lambda t: (t.feed(raw + raw2), t.text())[1])(Term())
    check("nested repos shown after expand", "auth" in s2)
    # the indent of a nested repo is absorbed into the NAME column, so its
    # BRANCH/SYNC/WHEN/STATUS align with un-indented top-level repo rows
    status_col2 = line_with(s2, "STATUS").index("STATUS")
    check("nested repo status aligned with top-level",
          line_with(s2, "auth").find("✓") == status_col2
          and line_with(s2, "alpha").find("✓") == status_col2)
    # collapse again
    w.send(b"\r")
    raw3 = w.drain(1.0)
    s3 = (lambda t: (t.feed(raw + raw2 + raw3), t.text())[1])(Term())
    check("nested repos hidden after re-collapse", "auth" not in s3)
    w.finish()
    subprocess.run(["rm", "-rf", grp])

    # ── 7. a long breadcrumb overflows the row instead of widening NAME ──
    deep = tempfile.mkdtemp(prefix="gitls-deep-")
    make_repo(os.path.join(deep, "x"))                 # short top-level repo
    crumb = ["platform", "services", "authentication", "backend"]
    make_repo(os.path.join(deep, *crumb, "one"))
    make_repo(os.path.join(deep, *crumb, "two"))
    w = Watcher(deep)
    raw = w.drain(1.5)
    sd = (lambda t: (t.feed(raw), t.text())[1])(Term())
    header = " › ".join(crumb)                     # "platform › … › backend"
    check("long breadcrumb shown in full (no ~)", header in sd and "~" not in sd)
    col = line_with(sd, "STATUS").index("STATUS")
    # the NAME column is sized to the short repo names, not the breadcrumb: the
    # repo status stays in the STATUS column while the header status overflows it
    check("repo status stays in STATUS column", line_with(sd, " x ").find("✓") == col)
    check("long breadcrumb overflows past STATUS", line_with(sd, "backend").find("✓") > col)
    w.finish()
    subprocess.run(["rm", "-rf", deep])

    subprocess.run(["rm", "-rf", work])
    print(f"\n{passed} passed, {failed} failed")
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
