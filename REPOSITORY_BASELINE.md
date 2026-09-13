# Repository Baseline and Branch Governance

## Canonical baseline

`main` is the only canonical branch of `ZiXuannnZhang/PAErealtimeImaging`.

Formal project state is split by purpose:

- `PROJECT_STATUS.md` — current project/branch/validation status.
- `REPOSITORY_BASELINE.md` — branch/history/merge governance.
- `BUILD_STANDARD.md` — build, dependency, packaging and delivery governance.
- `Codex-GitHub双端联动快速上手.md` — ChatGPT/Codex operating workflow.
- `codex/task-docs:TASKS/<task>.md` — task-specific requirements and explicit overrides.

Historical handoffs, migration records, old reports and old branch notes are traceability material only and must not override these current documents.

## Repository-level operating documents

Before starting any implementation, build, validation, delivery, or local-workspace cleanup task, Codex Desktop must read the latest versions from `origin/main`:

```text
PROJECT_STATUS.md
REPOSITORY_BASELINE.md
BUILD_STANDARD.md
```

Then read the current task document if one exists.

A task document may define task-specific overrides, but Codex must identify those overrides explicitly rather than silently bypassing repository-level standards.

## Branch roles

### `main`

- Sole source-of-truth branch for formal source baseline and repository-level governance documents.
- New implementation branches normally start from the latest `origin/main`.
- A reviewed implementation branch may intentionally remain unmerged while hardware/system validation is pending.

### `codex/task-docs`

- Dedicated communication branch for task specifications written for Codex Desktop.
- Only task documents and task-channel governance files belong here.
- It is not an implementation baseline and must not carry source changes.

### implementation branches

- Created for one concrete task or one explicitly continued review/addendum series.
- Must be pushed for ChatGPT review before merge.
- A branch that has received software `APPROVE` does **not** automatically become part of `main`; hardware/system acceptance or user-controlled merge may still be pending.

### current retained START validation branch

The current START admission implementation branch is intentionally retained outside `main`:

```text
codex/start-admission-fence-fix-20260913-003112
6313540f72544c0f68820c4815903abaa0b8c1e1
```

Status:

```text
software review / deterministic validation = APPROVE
real FPGA/NIC hardware validation          = NOT YET COMPLETED
merge into main                             = HOLD
```

Do not merge, rebase, squash, or rewrite this branch merely to make local workspace state look simpler. It is the current hardware-test candidate until project status changes.

### other retained development branches

`codex/ring-pipeline-refactor-20260912` remains an independent, unmerged direction. It must not be implicitly combined with the START admission candidate or used as a new default baseline.

### historical branches

Branches such as diagnostic snapshots, migration snapshots, experiments, backups, old `master`, and one-off verification branches are non-canonical. They may remain for traceability but must not become implicit development baselines.

The user-deleted branch:

```text
codex/local-docs-sync-20260913
```

is not a formal artifact. Do not recreate it during local/remote synchronization.

## Local workspace synchronization rules

When Codex is asked to organize the local workspace according to the remote repository:

1. Start with `git fetch --prune origin`.
2. Inspect local branches, worktrees, unpushed commits and tracked modifications before deleting anything.
3. Local `main` may only follow `origin/main` by fast-forward.
4. If local `main` diverges, stop and report; do not auto-reset/rebase/force.
5. Preserve the current START admission candidate and verify it against remote SHA `6313540f...`.
6. Do not delete ignored local CUDA/runtime dependencies, build evidence, `artifacts/`, user data or hardware captures merely because they are absent from Git.
7. A historical local branch may be removed only after confirming that it has no unique unpushed work that needs preservation.

## Task document naming

Single-task documents live under `TASKS/` on `codex/task-docs` and use:

```text
<简要任务说明>_YYYYMMDD-HHMMSS.md
```

The timestamp is the task publication local time, accurate to seconds.

`TASKS/README.md` is the persistent channel specification and is exempt from the per-task naming rule.

## Review and merge policy

1. ChatGPT defines the task and acceptance criteria.
2. Codex Desktop implements on an isolated implementation branch from the task-defined baseline.
3. Codex Desktop pushes the branch and reports exact build/test evidence.
4. ChatGPT independently re-reads remote HEAD, diff, source and test evidence and returns `APPROVE` or `REQUEST_CHANGES`.
5. Software `APPROVE` is not equivalent to hardware/system acceptance when the task requires real-device validation.
6. Merge into `main` remains user-controlled unless the user explicitly delegates merge authority.

## History policy

- No force push to `main`.
- No routine rewriting of established shared history.
- No implicit squash/rebase of an approved implementation branch unless explicitly required.
- If local `main` cannot fast-forward to `origin/main`, stop and diagnose the divergence before continuing.
- Historical documentation may be downgraded to `docs/history/` or to a compatibility pointer, but its original contents remain recoverable from Git history.
