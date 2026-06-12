# Changelog

All notable changes to this project will be documented in this file.

## [Unreleased]

### Added
- `gitls -w` / `--watch` — watch mode: refreshes the status table in place at a
  configurable interval (default 3 s, e.g. `-w 10`). Uses the alternate screen
  buffer so scrollback is left untouched, hides the cursor, and quits on `q` or
  Ctrl-C. The terminal is always restored on exit, including on `SIGINT` /
  `SIGTERM`. No `ncurses` dependency — raw ANSI escapes and `termios` only.
  Interactive keys act on the whole tree without leaving the view: `f` fetch,
  `p` pull, `s` switch, `r` refresh now. The `s` key opens a branch picker that
  lists recently active branches (most recent first) with type-to-filter, ↑/↓
  navigation and Tab/Enter selection.
- `gitls --dirty` — list only repos that are not clean and in sync (staged /
  modified / untracked files, ahead/behind, diverged or detached `HEAD`). The
  summary line still counts all scanned repos and appends `(N hidden)`. Works in
  one-shot mode and under `-w`.
- `~/.gitlsrc` keys `watch_interval` and `dirty_only`; CLI flags override them.

### Fixed
- Long branch names (or any over-wide row) no longer wrap and corrupt the table:
  the NAME / BRANCH columns are now capped to the terminal width (content is
  truncated with `~`). Affects both the one-shot table and watch mode. Piped
  output is still emitted at full width. Over-long `Scanned:` / footer paths are
  shortened with a leading `…`.

- Watch mode no longer corrupted the table when a refresh produced a narrower
  frame (e.g. switching to a shorter branch name) — each rewritten line is now
  cleared to its end, so no stale columns (a duplicate WHEN/STATUS) are left
  behind. The table also stays on screen while an action runs.

### Changed
- Watch mode now animates a spinner with the action verb (Fetching / Pulling /
  Switching) while a fetch / pull / switch runs, instead of a static line.

## [0.3.1] – 2026-03-08

### Fixed
- `fetch` and `pull` now run in parallel (thread pool), significantly faster on directories with many repos
- Spinner stays visible during network operations instead of stopping early after Phase 1
- `fork()` safety: replaced `execvp` with `execve` using a pre-resolved absolute git path, eliminating a potential deadlock when the spinner thread holds a libc lock at fork time
- `fetch` now correctly returns "up to date" when no new commits were downloaded (previously always reported "fetched")
- `-s fetch` and `-s pull` no longer misidentify the branch name as a subcommand
- Last-commit time (WHEN column) is now refreshed after a successful branch switch
- Thread pool falls back to single-threaded execution if all `pthread_create` calls fail

### Changed
- RPM spec: added `Requires: git` (needed for fetch/pull subcommands)

## [0.3.0] – 2026-02-28

### Added
- `gitls fetch` — fetch all repos from their `origin` remote; shows per-repo result (fetched / up to date / no remote / error)
- `gitls pull` — fast-forward pull all clean repos; dirty repos are skipped, diverged repos are reported
- `gitls fetch -s <branch>` — fetch first, then switch; creates a local tracking branch automatically if the branch only exists on the remote
- `~/.gitlsrc` config file — persistent defaults for `default_dir`, `max_depth`, `skip_dirs` (glob patterns via fnmatch), `no_color`
- `GITLS_CONFIG` environment variable to override the config file path
- `gitlsrc.example` installed to `$(PREFIX)/share/doc/gitls/` as a reference
- `make uninstall` target
- `make help` target
- Compiler-generated header dependencies (`-MMD -MP`)
- Version derived from git tags via `git describe` — no manual version bumps needed

### Fixed
- `gitls .` now correctly scans the current directory even when `default_dir` is set in the config

### Changed
- Parallel repo processing uses a two-phase design: local libgit2 queries run in a thread pool (Phase 1), subprocess fetch/pull runs sequentially on the main thread (Phase 2) — eliminates fork-in-multithreaded-process issues
- `make clean` removes all `*.d` dependency files including those in subdirectories

## [0.2.0] – 2026-02-24

### Added
- Dynamic column widths — table adapts to the longest repo name and branch name
- Spinner shown during scan with current operation label (Scanning / Fetching / Pulling / Switching)
- `-v` / `--verbose` flag — show all repos in summaries, not just changed ones
- `-s <branch>` now creates a local tracking branch when the target branch only exists on the remote (`✓ created & switched`)

### Changed
- Parallel repo processing via thread pool (min(nCPU, 8) threads)

## [0.1.0] – 2026-02-24

### Added
- Recursive directory scan for git repositories
- Status table: branch name (incl. detached HEAD as short SHA), staged / modified / untracked counts, ahead/behind upstream, relative last-commit time
- `-s <branch>` — switch all clean repos to a branch in one command
- `-d <n>` — configurable max search depth (default: 5)
- `-a` — include hidden directories
- `--no-color` — disable ANSI colors
- `--version` — show version string
- Automatic skip of `vendor/`, `node_modules/`, `.git/` internals
- Homebrew formula, Debian package, RPM spec
- MIT license
