# gitls Roadmap

A short, deliberate plan for the next releases. The guiding principle: **1.0.0
is a commitment to stability, not a "feature complete" milestone.** The real
trigger for 1.0.0 is the `--json` output — once external tools depend on it, the
schema is a public contract and breaking it means a major-version bump.

## 0.5.0 — Watch-mode categories

Collapsible category folders in the watch-mode table.

- Repos are grouped by their path breadcrumb relative to the scan root, joined
  with a chevron (`core › packages`). Direct children stay flat.
- Single-repo folders fold back to just the repo name.
- Flat repos and category headers interleave alphabetically.
- Aggregated folder status (`✓` / `↑ ↓ ●`) renders in the STATUS column.
- `↑/↓` move a cursor (no highlight until the first key press), `Enter`
  expands/collapses; expand state survives refreshes. Nav hints show only when
  categories exist.
- The table is sized to its content (never stretched to fill the screen); long
  breadcrumbs overflow the row instead of widening the NAME column.

Status: implemented on branch `watch-categories`, not yet merged or tagged.

## 0.6.0 — Interactive repo panel

Press `i` with the cursor on a repo to render a detail panel below the table.

- **v1 (read-only):** list the repo's branches with ahead/behind per branch and
  a stale marker.
- **v2 (delete):** delete stale branches from the panel.

Design notes:

- Reuse the existing watch infrastructure: raw input, the cursor, and the
  "render below the table" pattern the branch picker already uses.
- **Stale detection:** enumerate branches (`git_branch_iterator`); the safest,
  most useful signal is `gone` (the upstream tracking branch was deleted on the
  remote). `merged` (merged into HEAD) is a secondary signal.
- **Deletion safety:** local branches only (`git_branch_delete`, which refuses
  the current branch). Pause the auto-refresh while the panel is open and
  require an explicit confirmation before deleting.
- Cursor on a category header → `i` is a no-op (a folder aggregate could come
  later).

## 1.0.0 — `--json` output + surface freeze

A machine-readable output so `gitls` can back editor/launcher extensions
(VSCode, Raycast, …).

- Flat JSON array of repos; the `Repo` model already carries everything (path,
  branch, staged/modified/untracked, ahead/behind, has_remote, last_commit).
  Consumers group by `path` themselves — no grouping in the payload.
- Include the stale-branch data from 0.6.0 (e.g. `stale_branches`).
- Carry a `"schema": 1` field so the contract can be migrated later.
- Hand-rolled, dependency-free JSON with correct string escaping (paths may
  contain `"`, `\`, Unicode).
- `--json` is mutually exclusive with `-w`; disables colour and the spinner.

### Interaction model — the whole CLI is the API

A consumer (e.g. a VSCode extension) **spawns `gitls` as a subprocess and parses
stdout** — there is no daemon, socket, or library. The CLI-in-`--json`-mode *is*
the API, for reads *and* writes:

- **An operation is just its subcommand.** You don't send a command to a running
  process; you invoke the subcommand:
  - `gitls --json <path>` → list/status
  - `gitls fetch --json <path>` → fetched / up-to-date / no-remote / error
  - `gitls pull --json <path>` → pulled / up-to-date / not-ff / dirty / …
  - `gitls -s <branch> --json <path>` → switched / dirty / not_found / error
- **Targeting is by path** — the directory argument already scopes the operation.
  Point it at a single repo to act on just that repo; point it at a workspace to
  act on all of them. No new flag needed (a `--repo <path>` filter would be a
  later, additive option).
- **Stateless request/response.** Each op is one invocation returning one JSON
  result that includes the repo's updated state — the consumer doesn't need a
  separate re-list afterwards. Crash-isolated, parallelisable, no lifecycle to
  manage.
- `--json` therefore has to cover the **write subcommands too**, serialising the
  existing result enums (`FR_*`, `PR_*`, `SR_*`) as stable strings.

### Contract requirements

These shape the design and must hold from 1.0.0 on:

- **stdout = clean JSON only; stderr = errors/logs.** No spinner/progress on
  stdout in `--json` mode, or consumers can't parse it.
- **Stable exit codes** so callers can tell "no repos" vs "error" vs "git not
  installed" without parsing strings.
- **Machine-stable fields:** absolute paths; status as discrete fields/enums
  (`"clean"`, `"gone"`), never the rendered glyphs (`✓`/`●`) — those are
  presentation.
- **Discovery** via the `"schema"` field and `gitls --version`.
- **The table output is never a contract** — humans only. Consumers must use
  `--json`, never scrape the table. State this in the docs.
- **Polling now, push later:** 1.0.0 ships one-shot output; the consumer polls
  (FS watcher or interval). A streaming `gitls --json --watch` (NDJSON, one frame
  per line) is a possible later addition, deliberately out of 1.0.0.

**Freeze audit:** before tagging 1.0.0, do one deliberate pass over everything
that becomes a contract — CLI flags, `~/.gitlsrc` format, exit codes, and above
all the JSON schema. Rename anything awkward *now*; after 1.0.0 it is a breaking
change.

## Shared building block

A `repo_stale_branches()` core (libgit2 branch iterator + upstream
`gone`/`merged` detection, unit-testable) powers **both** the 0.6.0 panel and
the 1.0.0 JSON `stale_branches` field. Building the panel first means the stale
vocabulary and fields are already settled when the JSON schema is frozen — so
1.0.0 is "just serialize", not a re-design, and the frozen API is complete from
day one.
