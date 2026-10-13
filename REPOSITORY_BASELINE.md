# Repository Baseline and Branch Governance

> Audited 2026-10-13 (UTC+8) against remote `main@29bb51e15243974a6812fd622e291e91550083f4`,
> before this documentation update. Current production/test source remains at `491aa34`.
> The complete 40-branch snapshot below uses that fixed base; later documentation commits advance HEAD.

## Canonical baseline

`main` is the only canonical branch of `ZiXuannnZhang/PAErealtimeImaging`.

As of 2026-09-18, the accepted A/B/C/D source chain has been integrated back into canonical `main`. New work normally starts from latest `origin/main` unless a future task explicitly defines another reviewed baseline.

As of 2026-09-24, the field-validated frontend chain (full-resolution display crop, Frontend Preprocessing Stage, per-A-line zero-phase frontend filtering, the save/gate and write-fault fixes, and card-status/channel display naming) has been transferred into canonical `main` at `491aa34cfa9553954eea949a7703df7194eb7699` by a plain fast-forward: no merge commit, no force push, no history rewrite. Subsequent ring-imaging work starts from latest `origin/main`.

As of 2026-09-25, canonical `main` has moved past `491aa34` by documentation-only commits (branch inventory and branch-role records). No production/test source changed after `491aa34`, so the canonical **source-tree identity** remains `491aa34cfa9553954eea949a7703df7194eb7699`, while the canonical **HEAD** advances with each documentation commit and must always be read from the latest `origin/main`.

At the original 2026-09-28 work-branch registration, both implementation branches forked from `c95917f` (documentation-only past `491aa34`). They were then pending acceptance and unmerged. Current tips and partial acceptance are recorded below; the historical registration state is not the current hardware verdict.

2026-09-29 update: the ring work branch tip moved to `69af295` (the discovery early-exit **A scope** — early exit only — was transplanted into it verbatim at `6744787`; the B scope stays unique to the discovery branch), and a new implementation branch `codex/count-boundary-frontend-only-20260929-024946` (tip `3c6323d`) was forked linearly from that tip for the count-boundary frontend-only decoupling task, with the ring branch kept untouched at `69af295`. All three remain pending acceptance and unmerged; see PROJECT_STATUS §6.4.

2026-10-01 update: the count-boundary branch **passed real-machine acceptance** and was fast-forwarded back into the ring work branch by the analysis agent (ring tip `69af295` → `bd69785`, where `bd69785` is the branch's own post-registration delivery-receipt evidence commit; no merge commit, no force push, no history rewrite; the count-boundary ref is kept for traceability). The transplanted discovery **A scope** also **passed real-machine acceptance** on the ring branch (user feedback). The bipolar-compensation tuning work was **shelved** by the user (real-machine verdict: no operator/filter combination compensates the inherent DAS low-pass effect). The ring work branch remains pending its overall merge decision; see PROJECT_STATUS §6.4.

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

These two points are the source identity at which Session D was validated. Canonical `main`'s source tree has since advanced (see Canonical baseline above): do not use `d9daa2d7` or its binaries to represent current `main`, and do not use a build made at `d9daa2d7` as the delivery identity of a later `main` SHA.

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

### `codex/ring-reconstruction-enhancement-20260926-181824` — active work branch (pending joint acceptance)

Registered 2026-09-28. This branch carries the ring-DAS quality-enhancement workflow: the `RingSignalEnhancer` signal-domain operators (bipolar waveform compensation `g(t)=p−t·p′` with fixed λ(t)=t and no rectification, plus the user-requested low-frequency compensation `H(f)`), the FFT-path optimization (A1 boundary documentation, A2 allocation removal, A3 real-FFT pack trick; A4 mixed-radix gated out, A5 not dispatched), and the display-layer work (zero-value circular display mask + millimeter coordinate ticks). Configuration flows through a separate struct and the ring JSON `enhance` key — **no `RingReconCudaConfig` ABI change**.

```text
tip        = bd697851d2a1b6f11c39c06baf7308f1cac372ce (2026-10-01; past 1b7c4c2 it carries
             the discovery early-exit A-scope transplant 6744787 + evidence, and the
             count-boundary branch fast-forwarded in (aa9097e/e8eeba6/3c6323d/bd69785))
fork point = c95917f (canonical main)
status     = implemented, budget-tested and independently source-audited; partial contents
             hardware-accepted on 2026-10-01 (discovery early-exit A scope, count-boundary
             frontend-only decoupling); bipolar-compensation tuning shelved by the user;
             overall merge decision pending; NOT merged into canonical main
```

Rules:

- None of its content is part of canonical `main`. Do not describe branch capabilities as canonical-main capabilities, and do not start new tasks from this branch unless a task explicitly targets it.
- The real-time verdict is **conditional**: the Layer 2b full-chain replay (T_arr measurement, user presence required) has not been executed; see `CODEX_REPORTS/ring-enhance-budget-20260927-151919/layer2b_blocked.md` on the branch.
- Merge decision belongs to the user after real-hardware validation. Fast-forward requires the target main to be an ancestor of the work tip; that condition no longer holds at this audit. Preserve both histories and explicitly resolve the integration policy before any future merge; no force push or implicit history rewrite. Evidence: branch `CODEX_REPORTS/ring-enhance-budget-20260927-151919/`, `ring-enhance-opt-20260927-205601/`, `ring-display-20260928-022839/`; task documents on `codex/task-docs` @ `7d87eee`.

### `codex/discovery-early-exit-localbind-20260926-123437` — unmerged implementation branch

Registered 2026-09-28. Tip `6bca496` (parent `c95917f`), ahead 1 / behind 3 against the audited main: card discovery exits early once the expected card count is reached (`DiscoveryOptions::expectedCardCount`, default 4, 0 = legacy behaviour), the control-packet local bind IP defaults to `192.168.0.1` (INI-overridable), plus tooltip/comment corrections and 3 new test cases. Full CTest 46/46 recorded on the branch. The transplanted A scope was hardware-accepted on the ring branch; no full-branch hardware acceptance is recorded, B disposition remains pending, and the branch is not merged into main.

2026-09-29 note: the early-exit **A scope** was transplanted verbatim into the ring work branch (`6744787`, see above). The **B scope** (local-bind default + registry→INI tooltip copy migration) remains unique to this branch; its disposition (transplant / accept here / drop) is the user's separate decision. The branch is not marked "no further work" until every scope it carries has a home.

2026-10-01 note: the transplanted **A scope** passed real-machine acceptance on the ring work branch (user feedback). The **B scope** disposition is still the user's separate decision; the branch stays unmarked until B has a home.

### `codex/count-boundary-frontend-only-20260929-024946` — merged into the ring work branch (read-only traceability)

Registered 2026-09-29. Forked **linearly from the ring work branch tip `69af295`**. Implements the count-boundary frontend-only decoupling: with 「禁用计数重置」 checked and the received trigger count at or beyond the per-round design count, beyond-boundary sync groups keep refreshing the frontend signal windows (time/frequency displays) while both imaging feeds stay frozen (FramePublisher submit skipped and the frontend stage's Ring leg skipped via `TriggerGroup::frontendDisplayOnly`) — save and card-status counters unaffected; the unchecked mode is bit-identical to pre-change behaviour.

```text
tip        = bd697851d2a1b6f11c39c06baf7308f1cac372ce (3c6323d + delivery-receipt evidence bd69785)
fork point = 69af295 (ring work branch tip at fork time, linear)
status     = real-machine acceptance PASSED (2026-10-01, user feedback); fast-forwarded into
             the ring work branch (69af295 → bd69785, no merge commit, no force push);
             this ref is kept read-only for traceability and no longer evolves on its own
```

Rules:

- None of its content is part of canonical `main`; the fork base is the ring work branch, not canonical main. Real-hardware acceptance items belong to the user (signal displays keep refreshing / imaging stays frozen / save and counters unchanged).
- Known evidence gap remains after the recorded hardware acceptance: the evidence directory's README lists several `.log` transcripts (full CTest output, counter-proof red/green) excluded by `.gitignore` (`*.log`) and not archived as `.txt`. Preserve the acceptance record and separately request an evidence-only archive before future main integration; do not rewrite the hardware result as pending.
- Consolidation record: real-machine acceptance passed on 2026-10-01; the branch was fast-forwarded into `codex/ring-reconstruction-enhancement-20260926-181824` (`69af295` → `bd69785`) by the analysis agent with the fast-forward discipline. Evidence: branch `CODEX_REPORTS/count-boundary-frontend-only-20260929-100554/` (including the `bd69785` delivery receipt); task document `codex/task-docs:TASKS/前端刷新闸门解耦_计数边界后时频持续刷新_20260929-023810.md` (revision 1, `cceb435`).

## 2026-10-13 remote branch inventory

Snapshot base: `main@29bb51e15243974a6812fd622e291e91550083f4` (latest remote commit before this documentation update, 2026-10-01 01:35:57 UTC+8). There are **40 branch refs including main**: **19 non-main tips are ancestors of main**, and **20 have diverged from main**. No non-main ref is simply ahead of main. No branch is deleted or rewritten by this audit.

Ahead = commits reachable from the branch but not the snapshot main; behind = commits reachable from the snapshot main but not the branch. Values come from `git rev-list --left-right --count main...branch` (behind, ahead); ancestor status was separately checked with `git merge-base --is-ancestor`. These counts describe ancestry, not how recent or approved the code is. A later docs-only commit increases the behind count for every unchanged non-main branch; it does not add source capabilities.

| Branch | Tip SHA | Latest commit time (UTC+8) | Ahead | Behind | Relation to snapshot main |
|---|---|---|---:|---:|---|
| `backup/cf-dmas-pcf-20260816` | `40760b42fd5494ddd476a2bbc91ce09da4587365` | 2026-09-07 20:36:35 | 1 | 150 | diverged; unique history retained |
| `backup/local-workspace-source-20260913` | `cab86a1933d659f8d3e68ecadaee6d20b472ec80` | 2026-09-13 16:52:16 | 2 | 150 | diverged; unique history retained |
| `backup/main-pre-diagnostics-20260912` | `3c7904da9b9ec2e96aef384081ac04fc88d7fb29` | 2026-09-07 22:55:08 | 0 | 132 | ancestor; fully contained |
| `backup/sysdelay-per-channel-20260817` | `e3c4bd46a47458fa033d946adb401909e946539a` | 2026-09-07 20:36:53 | 1 | 150 | diverged; unique history retained |
| `codex/b-tier-analysis-d1-d7-20260925` | `8c87c3fac2f62573bab636631645c1cea15c6be8` | 2026-09-25 14:07:48 | 3 | 8 | diverged; unique history retained |
| `codex/baseline-role-ack-20260907-132500` | `2873e8156cc4c1663a65c265ead2088c425b0a5b` | 2026-09-07 13:56:07 | 2 | 143 | diverged; unique history retained |
| `codex/count-boundary-frontend-only-20260929-024946` | `bd697851d2a1b6f11c39c06baf7308f1cac372ce` | 2026-09-29 12:08:20 | 17 | 3 | diverged; unique history retained |
| `codex/das-dual-wavelength-quality-b-tier-20260925` | `2573572e83538afb1c51318654f89690ef96f6f4` | 2026-09-25 16:28:16 | 11 | 8 | diverged; unique history retained |
| `codex/diagnostic-log-export-20260907` | `3c88239afaa1ac9993ad1db9eda4cfd61f242747` | 2026-09-07 13:14:25 | 1 | 148 | diverged; unique history retained |
| `codex/diagnostics-enhancement-20260912` | `c24e915ba76aeee186673943b0a474c4f7c8f5e9` | 2026-09-12 17:15:34 | 0 | 92 | ancestor; fully contained |
| `codex/discovery-early-exit-localbind-20260926-123437` | `6bca496efd76e3e0d482fb6d6584260f6dfc160f` | 2026-09-26 12:50:11 | 1 | 3 | diverged; unique history retained |
| `codex/frontend-display-fullres-20260922` | `9938269220fcaf8e81216820f6bb46549ca59888` | 2026-09-22 03:08:13 | 0 | 20 | ancestor; fully contained |
| `codex/frontend-display-naming-20260924` | `491aa34cfa9553954eea949a7703df7194eb7699` | 2026-09-24 18:04:56 | 0 | 9 | ancestor; fully contained |
| `codex/frontend-filter-stage-20260923` | `40102eae6f78f61c4d5ad4e4a32e7ce4c82f90d6` | 2026-09-23 02:44:41 | 0 | 15 | ancestor; fully contained |
| `codex/frontend-preprocessor-stage-20260922` | `25804b4c2ab0de5c9bda6d91e8954970c7235bec` | 2026-09-23 01:05:54 | 0 | 17 | ancestor; fully contained |
| `codex/host-ingress-protection-experiment-20260908` | `4001e3aff938d86d93783c9cfaf8c9791fd80d7a` | 2026-09-08 12:20:59 | 4 | 132 | diverged; unique history retained |
| `codex/integrate-abcd-to-main-20260918-1829` | `36bb556441dedd760ab363f3df4f38872bbfc93f` | 2026-09-18 18:40:37 | 0 | 27 | ancestor; fully contained |
| `codex/local-history-supplement-20260913` | `1d412e9e54fda9b13564716eb9cc1af2165bd517` | 2026-09-13 19:12:39 | 2 | 79 | diverged; unique history retained |
| `codex/local-workspace-delete-rule-20260925-200656` | `31a0cb75d8d6d186b04b1b70a9bdb280a87a2d63` | 2026-09-25 20:10:15 | 0 | 4 | ancestor; fully contained |
| `codex/network-ingress-observability-20260907` | `38b0244e0fb07ff611849bd32cc5c669be363629` | 2026-09-07 23:49:08 | 3 | 132 | diverged; unique history retained |
| `codex/physical-round-blockers-fix-20260916` | `957f7ac83c6d9b39379a844e4496e03e053a1670` | 2026-09-16 12:18:09 | 3 | 150 | diverged; unique history retained |
| `codex/physical-round-normalizer-20260914-043019` | `b4376203e7c9c8b369b6b87551f3718bd1edf424` | 2026-09-16 10:49:46 | 0 | 62 | ancestor; fully contained |
| `codex/physical-round-normalizer-integrated-20260916` | `52cf7713d7e0e935cb14663ec3470f3a25bfeb90` | 2026-09-16 17:30:15 | 0 | 57 | ancestor; fully contained |
| `codex/ring-pipeline-refactor-20260912` | `d861b4e3671293f4eb79a1401b195ec6cdfb3cad` | 2026-09-12 09:56:54 | 6 | 97 | diverged; unique history retained |
| `codex/ring-reconstruction-enhancement-20260926-181824` | `bd697851d2a1b6f11c39c06baf7308f1cac372ce` | 2026-09-29 12:08:20 | 17 | 3 | diverged; unique history retained |
| `codex/ring-zero-phase-pa-inversion-20260919-025754` | `303255076e301a503e7b69c3251d515999687715` | 2026-09-21 01:59:40 | 10 | 24 | diverged; unique history retained |
| `codex/save-gate-bugfix-20260924` | `e21b019ba73c8d62556fccd0b197b456e11f544d` | 2026-09-24 11:23:54 | 0 | 11 | ancestor; fully contained |
| `codex/session-a-round-policy-core-20260917-114452` | `eb283f64574d9043f4d4823338c31767c3b811fe` | 2026-09-17 14:56:24 | 0 | 54 | ancestor; fully contained |
| `codex/session-b-ui-observability-20260917` | `676df4a946fed56b903474354984bff44ab50875` | 2026-09-17 21:33:00 | 0 | 47 | ancestor; fully contained |
| `codex/session-c-timeout-variable-round-20260917-214638` | `d9daa2d7af6bb8341349e405433824e89e42bcd4` | 2026-09-17 23:52:45 | 0 | 45 | ancestor; fully contained |
| `codex/session-d-integration-validation-20260918-000458` | `69a7606f95c97c839fd618115f4092a4291d8906` | 2026-09-18 01:08:19 | 0 | 44 | ancestor; fully contained |
| `codex/ssh-unattended-test` | `7e53d33bb50e160862eac91e08a45799f7c33d6d` | 2026-09-07 02:46:29 | 1 | 149 | diverged; unique history retained |
| `codex/start-admission-fence-fix-20260913-003112` | `6313540f72544c0f68820c4815903abaa0b8c1e1` | 2026-09-13 10:59:53 | 0 | 84 | ancestor; fully contained |
| `codex/start-race-validation-20260912-205615` | `8431476d7ff120d774d7f6161b898f76d17b8c67` | 2026-09-12 23:34:26 | 4 | 90 | diverged; unique history retained |
| `codex/startup-ingress-diagnostics-20260911` | `c24e915ba76aeee186673943b0a474c4f7c8f5e9` | 2026-09-12 17:15:34 | 0 | 92 | ancestor; fully contained |
| `codex/task-docs` | `2b40dcb922506737e77c063e481bf1908eb7dea8` | 2026-09-30 00:56:38 | 56 | 143 | diverged; unique history retained |
| `docs/readme-main-architecture-20260912` | `f326056ee99e5f9c135635d7e97fe2027ee50fbc` | 2026-09-12 20:55:57 | 0 | 90 | ancestor; fully contained |
| `main` | `29bb51e15243974a6812fd622e291e91550083f4` | 2026-10-01 01:35:57 | 0 | 0 | canonical snapshot |
| `master` | `219188a914f892d67314094b43998a89ca860993` | 2026-09-07 20:36:04 | 1 | 150 | diverged; unique history retained |
| `safety/pre-shm-observability-20260907-155459` | `7dd7a4d443c0f5f73fd5873a68570c221c5e1f79` | 2026-09-07 11:52:03 | 0 | 143 | ancestor; fully contained |

The ring-enhancement and count-boundary refs both point to `bd697851d2a1b6f11c39c06baf7308f1cac372ce`: their trees and histories are identical. They share merge-base `c95917f` with main, have 17 unique commits, and lack the three main-only documentation commits `647b4ca`, `e185b1d`, and `29bb51e`. The count-boundary consolidation was a fast-forward **into the ring branch**, never into main. Discovery tip `6bca496` has the same merge-base with main and has one unique commit; A scope was transplanted into the ring branch at `6744787`, so partial code reuse does not make the discovery tip an ancestor of main.

Direct `git merge --ff-only` of either work branch into current main is therefore impossible. The earlier fast-forward-only integration instruction was a precondition, not evidence that main can advance to those tips today. A future integration decision must explicitly reconcile the documentation and implementation histories while preserving their provenance; do not reset/rebase/force-push main or silently bypass the merge policy. This audit performs no implementation merge.

`codex/task-docs@2b40dcb` has 56 unique commits and latest dispatch dated 2026-09-30 (postFilter harness). This is a specification update, not a production implementation or hardware-acceptance result. The reference-only and retired experimental branch roles above remain in force; their unique commit counts do not promote their content to canonical status.

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

9. Agents must not delete files directly in the local workspace. Move every deletion candidate to the root `_pending_delete/` directory for user review; only the user may perform the final deletion.
10. Group each task's deletion candidates in `_pending_delete/<YYYYMMDD-HHMMSS>/` and include a brief `README.md` listing the candidate paths and why they are pending deletion. Do not delete, empty, or rewrite `_pending_delete/` during synchronization. If a candidate cannot be moved, leave it in place and report it instead of deleting it.

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
- Remote branch snapshot 2026-09-28: 39 branch refs (including `main`). Since the 2026-09-24 inventory, 5 refs were added: `codex/das-dual-wavelength-quality-b-tier-20260925` (see PROJECT_STATUS §6.3), `codex/b-tier-analysis-d1-d7-20260925` (B-tier D1–D7 analysis outputs), `codex/local-workspace-delete-rule-20260925-200656` (its single commit was fast-forwarded into main at `31a0cb7`; the ref is retained), `codex/discovery-early-exit-localbind-20260926-123437` and `codex/ring-reconstruction-enhancement-20260926-181824` (both pending acceptance, see Branch roles above). No branch was deleted. A full re-inventory is deferred to a separate decision.
- Remote branch snapshot 2026-09-29: 40 branch refs (including `main`); one ref added since 2026-09-28 — `codex/count-boundary-frontend-only-20260929-024946` (pending acceptance, see Branch roles above). No branch was deleted.
- 2026-10-01: no ref added or deleted (the count-boundary content was fast-forwarded into the ring work branch; its ref is kept for traceability). Still 40 refs.
