# Worktree support — design

Forward-looking design notes. For *what ships today*, see the §"Per-project
worktree strategy" section in `README.md` and the SPRINT-012 history doc
(in premium). For *what we want next*, read this file.

## Status

Verified against `src/worktree/`, `src/handlers/parallel_handler.cpp`,
`src/handlers/fan_in_handler.cpp`, `src/engine/fan_in_merger.cpp`,
`src/engine/checkpoint_manager.cpp`, `src/server/http_server.cpp`:

**What works (post-SPRINT-012):**

- Parallel branches inside a single run each get their own `git worktree`
  at `<repo>-wt-<run_id>-<branch_id>` when `worktree.strategy = auto`.
- `WorktreeManager::ensure_ready` is idempotent and refuses to overwrite
  a non-worktree at the target path.
- Branch commits cherry-pick deterministically into the launch repo at
  fan-in. Conflicts surface via `fan_in.<id>.cherry_pick_conflict` and
  classify as `FailureKind::CherryPickConflict`.
- Checkpoint persists `branch_worktrees[<branch_id>]` so resume picks up
  the same worktrees.
- `cleanup = remove-on-success` removes per-branch worktrees on fan-in
  success.

**What does *not* work yet:**

- **Whole-pipeline scoping.** `http_server.cpp` and the CLI start runs
  directly against `project_dir`. Two pipelines launched in the same
  repo share one checkout and clobber each other. The README has long
  claimed worktrees fix this — they fix it for parallel branches inside
  one run, not for two independent runs.
- **No UI surface.** Strategy/branch/path/cleanup can only be configured
  by hand-editing `~/.needle/config.json`. The dashboard does not
  display which worktree a run lives in.
- **`WorktreeStrategy::Manual`** is parsed (`strategy.cpp:13`) but no
  handler branches on it — pure stub.
- **No worktree lifecycle CLI.** `needle worktree list / prune / remove`
  are not implemented; stale worktrees from crashed runs accumulate.

## Vision

Three layers, ordered by dependency:

1. **Whole-pipeline worktree at run-start.** Call `ensure_ready` once
   from both `http_server.cpp` and `cli/router.cpp` before constructing
   the engine. The pipeline runs entirely inside the run's worktree;
   parallel-branch worktrees nest underneath as today.
2. **UI exposure.** Per-run worktree column on the runs table. Override
   form on the launch screen (defaults from config). Stale-worktree
   listing with prune action.
3. **Concurrency policy.** When `strategy = off` and a second run starts
   in the same repo, warn (don't block). Block is paternalistic and
   breaks legitimate read-only / API-only pipelines.

## Scenarios to handle

Decisions still open are flagged **[OPEN]**.

### Launch-state propagation

- **Dirty working tree at launch.** Today most pipelines implicitly
  assume "what I see in the editor is what the pipeline sees." A fresh
  worktree off `HEAD` discards uncommitted edits.
  **[OPEN]** Stash-and-restore, copy uncommitted into the worktree,
  or hard-refuse-and-tell-user-to-commit? *Lean: hard-refuse with a
  clear message; the others leak state silently.*
- **Untracked-but-needed files** (`.env`, local config, large binary
  inputs). Worktrees don't see them. **[OPEN]** Opt-in copy-list via
  `worktree.copy_into_worktree`? Fail-closed otherwise.
- **Build artifacts** (`node_modules`, `target/`, `venv/`, `.cargo/`).
  Worktree starts empty; first build is cold. Possible mitigations:
  shared `CARGO_TARGET_DIR` outside the worktree, symlinking, or
  accepting the cost. Surface in the UI ("first run will be slow").

### Result delivery

- **Where do commits end up?** Parallel-branch case cherry-picks at
  fan-in. For a whole-pipeline worktree, **[OPEN]** who merges and
  when? *Options:* auto-cherry-pick to launch branch on success, leave
  the branch in the worktree and tell the user to merge, push to a
  remote.
- **Editor visibility.** User has the editor open on `~/src/foo`; run
  executes in `~/src/foo-wt-<id>`. They want diffs/PRs visible in the
  editor. Auto-cherry-pick fixes this; leave-in-worktree does not.

### Resume and lifecycle

- **Worktree deleted between launches.** Checkpoint has
  `branch_worktrees`, run resumes, path is gone. **[OPEN]**
  Auto-recreate from the branch (if it still exists), or escalate to
  operator?
- **Stale worktrees from crashed runs.** Disk fills up. Needs a
  `needle worktree list / prune` CLI verb and a UI counterpart.
- **Worktree on a branch the user has since rebased / force-pushed.**
  Cherry-pick base disappears. Detect at fan-in; escalate.

### Non-git and partial-git projects

- **Project isn't a git repo.** `strategy = auto` must hard-fail at
  run-start, not mid-pipeline. CLI already refuses bare repos; need
  the same for "no `.git`".
- **Submodules, LFS, sparse checkout.** `git worktree add` is finicky
  here. Either document the limitation, or detect-and-warn.

### Concurrency boundaries

- **Two runs land on the same worktree path.** `${run_id}` collisions
  are statistically improbable but not impossible across machines /
  checkouts. Manager already refuses to overwrite non-worktrees; also
  detect "path exists, is a worktree, on a different branch" and pick
  a new path rather than failing the run.
- **CLI vs server starting runs simultaneously.** Two entry points,
  both need `ensure_ready`. Single helper to avoid drift.
- **Nested pipelines (`nested_run`).** Today it inherits `project_dir`.
  Nested runs should *not* nest worktrees (would fan-out of fan-out);
  keep inheriting the parent's worktree.

### Tooling integration

- **Agent sandbox / permission scope.** When `codex --cwd <path>` runs,
  its permission scope is keyed off `<path>`. If the worktree is at
  `../foo-wt-…`, sandboxes that allow-list "the project dir" must
  follow. Easy to miss; subtle to debug.
- **`needle troubleshoot` after a worktree-scoped failure.** Diagnose
  uses `run_dir`, but its git probes (`git_log_commits_since`,
  `git status`) need to know whether to run in the launch repo or the
  worktree. Per-branch failures: worktree. Whole-pipeline-worktree
  mode: the worktree.

### UX in the dashboard

- **Manual mode flow.** User pre-creates a worktree by hand and wants
  to attach it. UI needs a "use existing worktree at path X" option.
  Otherwise `manual` stays a stub forever.
- **Nested worktree visibility.** A whole-pipeline run with parallel
  branches has 1 + N worktrees. UI should show the tree, not just one
  path.

## Must-resolve before shipping whole-pipeline scoping

In rough priority order:

1. Dirty-state policy (refuse / stash / copy).
2. Result delivery (where do commits land).
3. Non-git project handling (hard-fail at run-start).
4. Sandbox / cwd propagation to agent invocations.

Everything else can defer to a v2 if scoped clearly in the sprint doc.

## Cross-references

- Manager / strategy implementation: `src/worktree/`.
- Parallel-branch integration: `src/handlers/parallel_handler.cpp`,
  `src/engine/fan_in_merger.cpp`.
- Checkpoint persistence: `src/engine/checkpoint_manager.cpp`.
- Server entry point (where whole-pipeline scoping must land):
  `src/server/http_server.cpp:298`.
- Config defaults: `src/config/needle_config.cpp:150`.
- History: SPRINT-012 (parallel-branch slice).
