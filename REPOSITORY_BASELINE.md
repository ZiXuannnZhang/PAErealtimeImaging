# Repository Baseline and Branch Governance

## Canonical baseline

`main` is the only canonical branch of `ZiXuannnZhang/PAErealtimeImaging`.

The complete project source history previously carried on `codex/diagnostic-log-export-20260907` has been merged into `main` with history preserved. That historical branch is retained for traceability only and must not be used as the base of future implementation work.

## Repository-level operating documents

Before starting any implementation, build, validation, or delivery task, Codex Desktop must read the latest versions from `origin/main`:

```text
REPOSITORY_BASELINE.md
BUILD_STANDARD.md
```

`REPOSITORY_BASELINE.md` governs branch/history usage. `BUILD_STANDARD.md` governs the default build toolchain, CMake presets, CUDA/runtime dependencies, artifact packaging, build identity, and delivery evidence for all subsequent work.

A task document may define task-specific overrides, but Codex must identify those overrides explicitly rather than silently bypassing the repository-level standards.

## Branch roles

### `main`

- Sole source-of-truth branch for formal project state.
- Every implementation branch starts from the latest `origin/main`.
- Formal documentation that governs the repository lives on `main`.

### `codex/task-docs`

- Dedicated communication branch for task specifications written for Codex Desktop.
- Only task documents and task-channel governance files belong here.
- It is not an implementation baseline and must not carry source changes.

### implementation branches

- Created by Codex Desktop from the latest `main`.
- One branch per concrete task.
- Must be pushed for ChatGPT review before merge.

### historical branches

Branches such as diagnostic snapshots, migration snapshots, experiments and one-off verification branches are non-canonical. They may remain for traceability but must not become implicit development baselines.

## Task document naming

Single-task documents live under `TASKS/` on `codex/task-docs` and use:

```text
<简要任务说明>_YYYYMMDD-HHMMSS.md
```

The timestamp is the task publication local time, accurate to seconds.

`TASKS/README.md` is the persistent channel specification and is exempt from the per-task naming rule.

## Review and merge policy

1. ChatGPT defines the task and acceptance criteria.
2. Codex Desktop implements from the latest `main` on an isolated branch.
3. Codex Desktop pushes the branch and reports exact build/test evidence.
4. ChatGPT re-reads the remote HEAD and diff and returns `APPROVE` or `REQUEST_CHANGES`.
5. Merge into `main` remains a user-controlled action unless the user explicitly delegates merge authority.

## History policy

- No force push to `main`.
- No rewriting established shared history for routine task integration.
- If local `main` cannot fast-forward to `origin/main`, stop and diagnose the divergence before continuing.
