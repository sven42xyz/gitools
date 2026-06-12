# gitls

[![CI](https://github.com/sven42xyz/gitools/actions/workflows/ci.yml/badge.svg)](https://github.com/sven42xyz/gitools/actions/workflows/ci.yml)

A fast, minimal tool to inspect and act on multiple git repositories.

![gitls demo](docs/demo.gif)


## Features

- **Fetch** all repos from their remote (`fetch`)
- **Pull** (fast-forward only) all clean repos (`pull`)
- **Branch switching** across all clean repos (`-s`)
- **Stale branch detection** — gone upstreams, regular and squash-merged branches (`stale`), with optional `--prune` and age filter
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

- [libgit2](https://libgit2.org/) >= v1.9
- [git](https://git-scm.com/) (required for `fetch` and `pull` subcommands)

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
gitls [fetch|pull|stale] [OPTIONS] [DIRECTORY]

Subcommands:
  fetch        Fetch all repos from their remote
  pull         Fast-forward pull all clean repos
  stale        List local branches whose upstream is gone or that
               are merged/squash-merged into the default branch

Options:
  -s <branch>          Switch all clean repos to <branch> if it exists
  -d <n>               Max search depth (default: 5)
  -a                   Include hidden directories
  -v                   Verbose: show all repos in summaries, not just changed ones
  --no-color           Disable ANSI colours
  --version            Show version
  -h, --help           Show this help

Stale options:
  --prune              Delete the listed stale branches (with confirmation)
  --yes, -y            Skip the confirmation prompt (use with --prune)
  --older-than <DUR>   Only flag branches whose tip is older than DUR
                       (e.g. 30d, 2w, 6m, 1y)
  --only <reasons>     Comma-separated subset of gone,merged,squash
                       (default: all). Example: --only gone,merged
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

# List stale local branches across all repos
gitls stale ~/projects

# Delete stale branches that haven't been touched in 30 days
gitls stale --prune --older-than 30d ~/projects

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

## Stale branches

`gitls stale` lists local branches that are likely safe to clean up. Three categories are detected:

| Tag | Meaning |
|-----|---------|
| `gone` | The branch has a configured upstream, but the remote-tracking ref no longer exists (e.g. the remote branch was deleted on the server). |
| `merged` | The branch tip is reachable from the default branch — i.e. it was merged via a regular merge or fast-forward. |
| `squash` | The branch's cumulative diff matches a single commit on the default branch — typical for "Squash & merge" workflows. |

```text
gitls stale ~/projects

Scanned: /Users/sven/projects

  api-server  (default: main)
    merged   feature/login
    squash   feature/payment-flow
    gone     bugfix/old-fix

  3 stale branches in 1 repo · 1 gone · 1 merged · 1 squashed
```

### Pruning

Add `--prune` to actually delete the listed branches. By default you get a confirmation prompt; pass `--yes` (or `-y`) to skip it.

```sh
gitls stale --prune ~/projects
gitls stale --prune --yes ~/projects

# Prune only branches whose remote upstream is gone
gitls stale --prune --yes --only gone ~/projects
```

**Safety rules:**

- The current `HEAD` branch is never flagged or deleted.
- The repo's default branch (`main`/`master`) and any names listed under `protected_branches` are excluded — `main` and `master` are always implicitly protected even when they are not the resolved default.
- `gone` branches are re-verified against the default branch before deletion. If they carry local commits that are not reachable from default, they are **refused** with a clear marker rather than deleted, so local-only work is never lost silently.
- `merged` and `squash` branches are safe to delete by construction (their work is already on the default branch).

### Age filter

`--older-than <DUR>` restricts the listing to branches whose tip commit is older than the given duration. Useful for "list branches that have been dormant for at least a month" workflows.

| Suffix | Unit |
|--------|------|
| `s` | seconds |
| `h` | hours |
| `d` | days |
| `w` | weeks |
| `m` | months (≈ 30 days) |
| `y` | years (≈ 365 days) |

```sh
gitls stale --older-than 30d
gitls stale --prune --yes --older-than 6m ~/projects
```

## Config file

Copy the bundled example to get started:

```sh
cp /usr/local/share/doc/gitls/gitlsrc.example ~/.gitlsrc
```

Or create `~/.gitlsrc` manually:

```ini
# ~/.gitlsrc
default_dir=~/projects
max_depth=3
skip_dirs=build,dist,tmp,*.egg-info
protected_branches=develop,staging
no_color=false
```

| Key | Description | Default |
|-----|-------------|---------|
| `default_dir` | Directory to scan when none is given on CLI | `.` (current dir) |
| `max_depth` | Maximum directory recursion depth | `5` |
| `skip_dirs` | Comma-separated list of directory names to skip (glob patterns supported) | — |
| `protected_branches` | Comma-separated branch names that `stale` must never flag or delete (in addition to `main`/`master`, which are always protected) | — |
| `no_color` | Set to `true` or `1` to disable colors | `false` |

CLI flags always override the config file. Passing an explicit directory (including `.`) always overrides `default_dir`:

```sh
gitls .          # scan current directory, ignoring default_dir
gitls ~/other    # scan a specific directory
```

Set `GITLS_CONFIG=/path/to/file` to use a different config path.

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
