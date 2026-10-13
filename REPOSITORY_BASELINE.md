# Repository Baseline and Branch Governance

> Updated 2026-10-13 (UTC+8). The canonical source and next development baseline are latest origin/main.

## Canonical integration

The user explicitly accepted a linear content integration from
bd697851d2a1b6f11c39c06baf7308f1cac372ce onto the previous main
6ff0e02fac0d6fe0e5a4edd089c09c85d18ff5fa. The new integration commit has exactly one parent
(the previous main), preserves current repository-level documentation, and imports the complete
ring-branch implementation/tests/evidence. Main is updated with a normal fast-forward push.

The previous main and ring tip had diverged at c95917f (4 main-only / 17 ring-only commits).
A direct fast-forward to that existing ring tip was impossible. The user approved this alternative;
there is no merge commit, rebase, force push, or rewrite of existing commits.

The MC_410T_MultiCard subtree must equal ring tip tree
ff2e987a3f44565496ff57f8467e891ea04757d0. Importing source content does not make bd69785 an
ancestor of main. Original branch refs and accepted commit identities remain preserved remotely.

## Current branch roles

| Branch | Role |
|---|---|
| main | Sole canonical source/docs and next development baseline |
| codex/ring-reconstruction-enhancement-20260926-181824 | Content imported from bd69785; original implementation history retained |
| codex/count-boundary-frontend-only-20260929-024946 | Same tip/source as ring branch; read-only traceability |
| codex/discovery-early-exit-localbind-20260926-123437 | A scope imported through 6744787; B scope disposition remains pending |
| codex/task-docs | Task specifications only; never an implementation baseline |
| codex/ring-zero-phase-pa-inversion-20260919-025754 | Reference-only, no maintenance; Stage B1→B2 starting-point clause superseded |
| codex/das-dual-wavelength-quality-b-tier-20260925 | Retired experiment; no further work or implicit integration |
| Session A/B/C/D, START, PhysicalRound, other backups | Historical provenance; retain remote refs |

The ring-zero-phase Stage A numerical references (formula, time axis, filter ordering/endpoints,
configuration rejection) remain closed references; do not re-derive them. Stage B1 source d8dd3da
is not implicitly imported. The retired B-tier branch's source/tests/docs/conclusions must not be
treated as canonical facts or directly merged into main.

## Accepted provenance and evidence

START-admission 6313540; PhysicalRound 52cf771; Session A eb283f6; B 676df4a;
C accepted code d9daa2d7af6bb8341349e405433824e89e42bcd4;
D traceability 69a7606f95c97c839fd618115f4092a4291d8906.
The old frontend-chain source identity was 491aa34.
The count-boundary implementation is aa9097e, lightweight package build 3c6323d, ring content tip bd69785.

Hardware acceptance remains scoped: A/B/C/D passed the current tested scope; discovery A and
count-boundary passed on 2026-10-01. Bipolar effect tuning is shelved, Layer 2b T_arr not measured,
and discovery B awaits a decision. Main integration does not create new hardware evidence.
The existing count-boundary .log-to-.txt evidence gap remains separately recorded.

## Source of truth and development

PROJECT_STATUS.md owns current state; this file owns branch/history governance;
BUILD_STANDARD.md owns toolchain, dependency and delivery rules; HANDOFF and the collaboration
guide are entry points. Specific task requirements override conflicts.

Local main follows origin/main only by fast-forward. New implementation tasks normally branch
from latest origin/main. If refs diverge, inventory and resolve the method explicitly; do not reset,
force-push, or silently replay accepted histories. Keep old histories for provenance.

## Local cleanup authorized 2026-10-13

The user requested only latest-main source/docs in the active workspace and reviewable disk cleanup.
Before retiring local source copies, prove remote reachability or back up unique source/docs on a
separate archival branch; never silently fold those local experiments into main.
Regenerable build trees/test outputs and remotely backed-up old checkouts may be moved into
_pending_delete/<task>/delete-candidates/ with original paths, byte counts, hashes and recovery refs.
Unique raw data, calibration results, hardware captures and unverified evidence go to a separate
protected-data/ area marked KEEP. Dependencies needed for reproducible builds are also identified
separately. The user alone performs final deletion. Never delete, empty or rewrite old _pending_delete.

Local branch refs may be retired only after remote containment is verified; remote historical branches
are not deleted. Do not recreate the user-deleted codex/local-docs-sync-20260913.
Do not mutate frozen _migration_pack provenance files. Source filenames and Git object identities
must remain recoverable from the recorded refs after a move.

## Task and integration policy

Task documents live on codex/task-docs under TASKS/<简要任务说明>_YYYYMMDD-HHMMSS.md
(TASKS/README.md exempt). Software implementation and task specifications remain separate.
Source review, automated tests, hardware acceptance, and hardware root-cause attribution are distinct.
Future source integration requires an explicit scope and recorded method; this user-approved
single-parent content import is the specific resolution for the 2026-10-13 divergence.

## Historical branch inventory

The following is the fixed audit BEFORE this integration. Its ahead/behind counts use main@29bb51e,
not the new main. It is retained as history, not a live capability/merge-status table.

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
