# Repository Baseline and Branch Governance

## Canonical baseline

`main` is the only canonical branch of `ZiXuannnZhang/PAErealtimeImaging`.

As of 2026-09-18, the accepted A/B/C/D source chain has been integrated back into canonical `main`. New work normally starts from latest `origin/main` unless a future task explicitly defines another reviewed baseline.

As of 2026-09-24, the field-validated frontend chain (full-resolution display crop, Frontend Preprocessing Stage, per-A-line zero-phase frontend filtering, the save/gate and write-fault fixes, and card-status/channel display naming) has been transferred into canonical `main` at `491aa34cfa9553954eea949a7703df7194eb7699` by a plain fast-forward: no merge commit, no force push, no history rewrite. Subsequent ring-imaging work starts from latest `origin/main`.

Formal project state is split by purpose:

- `PROJECT_STATUS.md` — current project/validation status.
- `REPOSITORY_BASELINE.md` — branch/history/merge governance.
- `BUILD_STANDARD.md` — build, dependency, packaging and delivery governance.
- `Codex-GitHub双端联动快速上手.md` — ChatGPT/Codex operating workflow.
- `codex/task-docs:TASKS/<task>.md` — task-specific requirements and explicit overrides.

Historical handoffs, receipts, reports and old branch notes are traceability material only.

## Accepted A/B/C/D provenance

The accepted production/test source tree used for Session D validation is:

```text
candidate code SHA = d9daa2d7af6bb8341349e405433824e89e42bcd4
Session D final traceability HEAD = 69a7606f95c97c839fd618115f4092a4291d8906
```

This ancestry also contains the accepted START-admission software changes. Do not remove or replay only a subset of these ancestors merely to produce a cleaner-looking history; doing so would create a different source combination.

Hardware status for this round:

```text
A/B/C/D functional validation in current tested scope = PASS
exact FPGA/LabVIEW source of extra startup triggers   = NOT PROVEN
7 / 4007                                              = field observation, not protocol constant
```

## Branch roles

### `main`

- Sole source-of-truth branch for formal source and repository-level governance.
- Local `main` may follow `origin/main` only by fast-forward.
- New implementation branches start from latest `origin/main` unless explicitly overridden by a reviewed task.
- Do not force-push or routinely rewrite main history.

### `codex/task-docs`

- Task specification / communication branch.
- `TASKS/` is authoritative for task-specific requirements.
- It is not a production implementation baseline.

### implementation / validation branches

Session A/B/C/D, physical-round, START-admission and other historical `codex/*` branches are retained non-destructively for traceability.

They are no longer implicit default development baselines after canonical integration.

Important retained anchors include:

```text
codex/start-admission-fence-fix-20260913-003112
  6313540f72544c0f68820c4815903abaa0b8c1e1

codex/physical-round-normalizer-integrated-20260916
  52cf7713d7e0e935cb14663ec3470f3a25bfeb90

codex/session-a-round-policy-core-20260917-114452
  eb283f64574d9043f4d4823338c31767c3b811fe

codex/session-b-ui-observability-20260917
  676df4a946fed56b903474354984bff44ab50875

codex/session-c-timeout-variable-round-20260917-214638
  d9daa2d7af6bb8341349e405433824e89e42bcd4

codex/session-d-integration-validation-20260918-000458
  69a7606f95c97c839fd618115f4092a4291d8906
```

The existence of these branch refs does not make them active baselines.

### `codex/ring-zero-phase-pa-inversion-20260919-025754` — reference only

Decided 2026-09-24. This branch holds ring-imaging Stage A (algorithm basis and numerical reference for the photoacoustic inversion formula, time axis, filter ordering/endpoints and configuration-rejection rules) and Stage B1 (ring high/low-pass zero-phase filtering).

```text
tip              = 3032550
Stage B1 source  = d8dd3daf2f1bce3afdc01e0452e7ae487f3a1697
```

Rules:

- Read-only reference. No further implementation or maintenance happens on this branch.
- `CODEX_REPORTS/ring-zero-phase-pa-inversion-20260919/` stays the numerical reference for those closed conclusions; do not re-derive them.
- The Stage B1 task clause "B1 通过后的 exact SHA 将作为 B2 起点" is **superseded**. Future ring-imaging work branches from latest `origin/main`, not from this branch.
- This branch and canonical `main` share base `fb10721e` and have never been merged. Its Stage B1 artifacts (including `src/RingRecon/zero_phase_filter.cpp`) are **not** in canonical `main`. If a future task needs that filter behaviour it must be landed or explicitly migrated onto `main`; never assume `main` already has it.

## START-admission status

The START-admission software fix is included in the accepted canonical source ancestry and remains software-approved.

Do not convert that fact into the stronger hardware claim that the historical FPGA/NIC startup-loss root cause has been uniquely proven. Dedicated hardware/root-cause attribution remains separate.

## Local workspace synchronization rules

When Codex organizes the local workspace from the remote repository:

1. `git fetch --prune origin`.
2. Treat latest `origin/main` as canonical.
3. Local `main` may only fast-forward to `origin/main`.
4. If local `main` diverges or local commits are not on remote, stop and inventory before destructive action.
5. Do not reset/rebase away unique local work.
6. Preserve ignored CUDA/runtime dependencies, `testdata/`, `artifacts/`, build evidence, hardware captures, logs and user data.
7. Historical local branches/worktrees may be removed only after confirming they contain no unique unpushed work that needs preservation.
8. Remote historical branches are not deleted as part of this canonical integration; remote cleanup is intentionally non-destructive.

## Task document naming

Single-task documents live under `TASKS/` on `codex/task-docs` and use:

```text
<简要任务说明>_YYYYMMDD-HHMMSS.md
```

`TASKS/README.md` is exempt.

## Review and merge policy

1. ChatGPT defines task scope and acceptance criteria.
2. Codex implements on an isolated task branch.
3. Codex pushes exact build/test evidence.
4. ChatGPT independently reviews remote source/tests/receipt.
5. Software approval and hardware/root-cause proof remain distinct claims.
6. Merge into main is explicit and traceable.
7. Approved validation histories must not be silently rebased/squashed/cherry-picked into materially different source combinations.
8. After canonical integration, future tasks should normally use latest `origin/main`.

## History policy

- No force push to `main`.
- No routine rewriting of shared history.
- No destructive remote branch cleanup in this closeout.
- No implicit squash/rebase of approved histories.
- Historical branches may later be pruned only by a separate explicit cleanup decision after local/remote inventory.
- Do not recreate the previously user-deleted `codex/local-docs-sync-20260913`.
- Remote branch inventory 2026-09-24 (explicitly non-destructive — **no branch deleted**): 33 remote branches classified. 18 are fully contained in canonical `main` (zero unique commits: the 5 frontend-chain work branches plus 13 historical validation points). 15 retain unique commits and must be kept (`codex/task-docs`, `codex/ring-zero-phase-pa-inversion-20260919-025754`, and 13 historical/experiment branches). Deleting the zero-unique-commit set would lose no commits, but it was deliberately deferred to a separate cleanup decision.
