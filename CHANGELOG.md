# Changelog

All notable changes to this project will be documented in this file.

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
