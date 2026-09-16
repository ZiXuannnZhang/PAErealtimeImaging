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

A task document may define task-specific overrides, including a baseline that is not `main`. Codex must identify those overrides explicitly rather than silently bypassing repository-level standards.

## Branch roles

### `main`

- Sole source-of-truth branch for formal source baseline and repository-level governance documents.
- New implementation branches normally start from latest `origin/main` **unless a task explicitly defines a different reviewed baseline**.
- A reviewed implementation branch may intentionally remain unmerged while hardware/system validation is pending.
- `main` being canonical does not mean every current hardware-test candidate must already be merged into it.

### `codex/task-docs`

- Dedicated communication branch for task specifications written for Codex Desktop.
- `TASKS/` and task-channel governance are authoritative there.
- Temporary handoff/download files may exist for operational transfer, but they do not become repository-level project truth.
- It is not an implementation baseline and must not carry production source changes.

### implementation / validation branches

- Created for one concrete task or one explicitly continued review/addendum series.
- Must be pushed for ChatGPT review before merge.
- Software `APPROVE` is not equivalent to hardware/system acceptance.
- A branch may remain the exact hardware-test candidate after software review, even when `main` has advanced independently.
- Do not silently rebase, squash, cherry-pick extra code into, or rewrite a reviewed hardware-test candidate; doing so invalidates the exact reviewed commit identity.

## Current retained validation branches

### Physical round normalization / RoundIdentity integrated candidate

```text
branch = codex/physical-round-normalizer-integrated-20260916
SHA    = 52cf7713d7e0e935cb14663ec3470f3a25bfeb90
```

Status:

```text
RoundIdentity code fixes                  = IMPLEMENTED
software review / automated validation    = PASS / APPROVE
Windows build                             = PASS
real ImagingSvc + CUDA service selftests  = PASS
physical-round hardware acceptance        = PENDING
CountBoundary behavior in new 4007 env    = UNVERIFIED
TimeoutBoundary full reset chain          = UNVERIFIED
merge into main                            = HOLD
```

Important history fact: this branch is not a simple fast-forward continuation of current `main`; the histories have diverged. Do not move `main` directly to this SHA, and do not attempt a “cleanup rebase” merely to make the graph linear. If hardware acceptance eventually passes, integration strategy must be designed and reviewed separately.

The current hardware observation is that a different control environment may produce approximately `4007` physical triggers per full round instead of the previously observed `4001`. This does **not** authorize changing logical round configuration to `4006` without first proving the trigger semantics from logs/hardware evidence.

### START admission validation candidate

```text
branch = codex/start-admission-fence-fix-20260913-003112
SHA    = 6313540f72544c0f68820c4815903abaa0b8c1e1
```

Status:

```text
software review / deterministic validation = APPROVE
real FPGA/NIC hardware validation          = PENDING
merge into main                             = HOLD
```

This is a separate startup ingress-loss workstream. Do not use START admission results as proof of PhysicalRoundNormalizer behavior, and do not use 4007 physical-round observations as proof about START admission.

### other retained development branches

`codex/ring-pipeline-refactor-20260912` is an older independent, unmerged direction. It must not be implicitly combined with either current validation candidate or used as a new default baseline.

### historical branches

Branches such as diagnostic snapshots, migration snapshots, experiments, backups, old `master`, superseded `codex/*` branches, and one-off verification branches are non-canonical. They may remain for traceability but must not become implicit development baselines.

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
5. Preserve and verify both current hardware-validation candidates:
   - `codex/physical-round-normalizer-integrated-20260916@52cf7713...`
   - `codex/start-admission-fence-fix-20260913-003112@6313540f...`
6. Do not delete ignored local CUDA/runtime dependencies, build evidence, `artifacts/`, user data, diagnostic ZIPs, extracted logs or hardware captures merely because they are absent from Git.
7. A historical local branch may be removed only after confirming it has no unique unpushed work that needs preservation.

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
6. New hardware evidence may invalidate an operational assumption without invalidating already-proven software invariants; distinguish these explicitly.
7. Merge into `main` remains user-controlled unless the user explicitly delegates merge authority.
8. For a diverged long-lived validation branch, do not choose merge/rebase/cherry-pick strategy until the exact accepted source set and hardware acceptance boundary are known.

## History policy

- No force push to `main`.
- No routine rewriting of established shared history.
- No implicit squash/rebase of an approved implementation/validation branch unless explicitly required.
- If local `main` cannot fast-forward to `origin/main`, stop and diagnose the divergence before continuing.
- Do not create self-referential report commits merely to embed the final commit SHA inside a report that itself changes that SHA. Report final remote HEAD out-of-band in the execution receipt when necessary.
- Historical documentation may be downgraded to `docs/history/` or to a compatibility pointer, but its original contents remain recoverable from Git history.
