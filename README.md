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

- **Fetch** all repos from their remote (`fetch`)
- **Pull** (fast-forward only) all clean repos (`pull`)
- **Branch switching** across all clean repos (`-s`)
- Recursive directory scan with configurable depth
- Branch name (incl. detached HEAD as short SHA)
- Staged / modified / untracked file counts
- Ahead / behind upstream
- Relative last-commit time
- Color output (disable with `--no-color`)
- Skips `vendor/`, `node_modules/`, `.git/` internals automatically
- Config file `~/.gitlsrc` for persistent defaults
- Parallel repo processing for fast scans

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
gitls [fetch|pull] [OPTIONS] [DIRECTORY]

Subcommands:
  fetch        Fetch all repos from their remote
  pull         Fast-forward pull all clean repos

Options:
  -s <branch>  Switch all clean repos to <branch> if it exists
  -d <n>       Max search depth (default: 5)
  -a           Include hidden directories
  -v           Verbose: show all repos in summaries, not just changed ones
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

# Fetch all repos
gitls fetch ~/projects

# Pull (fast-forward) all clean repos
gitls pull ~/projects

# Switch all clean repos to main
gitls -s main ~/projects

# Fetch and switch to a branch (creates local tracking branch if needed)
gitls fetch -s feature-branch ~/projects

# No colours (useful for scripts)
gitls --no-color ~/projects
```

## Fetch

`gitls fetch` fetches all repos from their `origin` remote and shows the updated ahead/behind status. By default, only fetched repos and errors are shown per line. Add `-v` to see all repos including up-to-date and no-remote ones.

```
gitls fetch ~/projects

Fetch results:

  api-server    ✓ fetched
  frontend      ✓ fetched
  auth-service  · no remote
  legacy-app    ✓ fetched

  fetched 3 · up to date 0 · no remote 1
```

## Pull

`gitls pull` fast-forward-pulls all clean repos. Dirty repos are skipped; diverged repos are flagged.

```
gitls pull ~/projects

Pull results:

  api-server    ✓ pulled
  frontend      · up to date
  auth-service  · no remote
  legacy-app    ✗ skipped  (dirty)
  infra         · not fast-forward

  pulled 1 · up to date 1 · skipped 1 dirty · not fast-forward 1
```

**Rules:**
- A repo is pulled only if it has **no staged or modified files**
- Only fast-forward merges are performed — diverged repos are reported, never force-merged
- Repos without a remote are listed but skipped

By default, only pulled repos and errors are shown per line. Add `-v` to see all repos including up-to-date and no-remote ones.

## Branch switching

The `-s` flag switches all clean repositories to a target branch in one command.

```
gitls -s main ~/projects

Switched to branch: main

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

By default, only switched repos and errors are shown per line. Add `-v` to also see repos that were already on the branch or where the branch wasn't found.

### Fetch and switch

Combine `fetch` with `-s` to switch to a branch that only exists on the remote.
gitls fetches first, then switches — creating a local tracking branch automatically
if the branch isn't present locally yet.

```
gitls fetch -s feature-x ~/projects

Switched to branch: feature-x

  api-server        ✓ created & switched
  frontend          ✓ switched
  auth-service      · branch not found
  legacy-app        ✗ skipped  1 modified

  switched 1 · created 1 · skipped 1 dirty
```

| Result | Meaning |
|--------|---------|
| `✓ switched` | Branch existed locally, checked out |
| `✓ created & switched` | Local tracking branch created from `origin/<branch>`, checked out |
| `· already on branch` | Already on that branch |
| `· branch not found` | Branch doesn't exist locally or on `origin` |
| `✗ skipped` | Repo has staged or modified files |

## Config file

Create `~/.gitlsrc` to set persistent defaults:

```ini
# ~/.gitlsrc
default_dir=~/projects
max_depth=3
skip_dirs=build,dist,tmp,*.egg-info
no_color=false
```

| Key | Description | Default |
|-----|-------------|---------|
| `default_dir` | Directory to scan when none is given on CLI | `.` (current dir) |
| `max_depth` | Maximum directory recursion depth | `5` |
| `skip_dirs` | Comma-separated list of directory names to skip (glob patterns supported) | — |
| `no_color` | Set to `true` or `1` to disable colors | `false` |

CLI flags always override the config file. Set `GITLS_CONFIG=/path/to/file` to use a different config path.

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
