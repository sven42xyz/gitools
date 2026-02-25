# gitls

[![CI](https://github.com/sven42xyz/gitools/actions/workflows/ci.yml/badge.svg)](https://github.com/sven42xyz/gitools/actions/workflows/ci.yml)

A fast, minimal tool to inspect and act on multiple git repositories.

```
Scanned: /Users/sven/projects

  NAME                          BRANCH                          SYNC     WHEN            STATUS
  ────────────────────────────────────────────────────────────────────────────────────────────
  gitools                       main                            ≡        2 days ago      ✓
  api-server                    feature/auth                    ↑2       5 hours ago     ●2 ✗1
  frontend                      main                            ↓3       3 days ago      ✓
  ────────────────────────────────────────────────────────────────────────────────────────────
  3 repos · 2 clean · 1 dirty
```

## Features

- Branch switching across all clean repos (`-s`)
- Recursive directory scan with configurable depth
- Branch name (incl. detached HEAD as short SHA)
- Staged / modified / untracked file counts
- Ahead / behind upstream
- Relative last-commit time
- Color output (disable with `--no-color`)
- Skips `vendor/`, `node_modules/`, `.git/` internals automatically

## Requirements

- [libgit2](https://libgit2.org/) >= 1.0

## Installation

### Homebrew (macOS / Linux)

```sh
brew tap sven42xyz/tap
brew install gitls
```

### Build from source

```sh
# macOS
brew install libgit2

# Debian / Ubuntu
sudo apt install libgit2-dev

# Fedora / RHEL
sudo dnf install libgit2-devel

git clone https://github.com/sven42xyz/gitools.git
cd gitools
make
sudo make install        # installs to /usr/local/bin
```

## Usage

```
gitls [OPTIONS] [DIRECTORY]

  -s <branch>  Switch all clean repos to <branch> if it exists
  -d <n>       Max search depth (default: 5)
  -a           Include hidden directories
  --no-color   Disable ANSI colours
  --version    Show version
  -h, --help   Show this help
```

### Examples

```sh
# Scan current directory
gitls

# Scan ~/projects, max 3 levels deep
gitls -d 3 ~/projects

# Switch all clean repos to main
gitls -s main ~/projects

# No colours (useful for scripts)
gitls --no-color ~/projects
```

## Branch switching

The `-s` flag switches all clean repositories to a target branch in one command — useful when working across a multi-repo project and needing to align all repos to the same branch.

```
gitls -s main ~/projects

Switching to branch: main

  api-server        ✓ switched
  frontend          · already on branch
  auth-service      ✓ switched
  legacy-app        ✗ skipped  2 staged, 1 modified
  infra             · branch not found

  switched 2 · already 1 · skipped 1 dirty
```

**Rules:**
- A repo is switched only if it has **no staged or modified files** (untracked files are left untouched)
- If the target branch does not exist in a repo, it is silently skipped
- After switching, the full status table is shown for all repos

## Status indicators

| Symbol | Meaning |
|--------|---------|
| `✓`    | Clean   |
| `●N`   | N staged files |
| `✗N`   | N modified (unstaged) files |
| `?N`   | N untracked files |
| `↑N`   | N commits ahead of remote |
| `↓N`   | N commits behind remote |
| `↑N↓M` | Diverged |
| `≡`    | In sync with remote |
| `?`    | No remote configured |

## License

MIT — see [LICENSE](LICENSE)
