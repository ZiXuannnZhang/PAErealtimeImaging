# Handoff — Session D (integration review and candidate delivery validation), 2026-09-18

## Baseline / candidate identity

```text
Session C base branch      = codex/session-c-timeout-variable-round-20260917-214638
Session C base SHA         = d9daa2d7af6bb8341349e405433824e89e42bcd4
Session D branch           = codex/session-d-integration-validation-20260918-000458
Session D candidate code SHA = d9daa2d7af6bb8341349e405433824e89e42bcd4
bugfix commits             = none (no deterministic frozen-contract violation was found;
                              production/test source is byte-identical to the
                              independently reviewed Session C code)
final remote HEAD          = reported out-of-band in the execution report
```

The Session D branch adds only receipt/handoff/checklist documentation on top of
the Session C code commit; its HEAD does not change the candidate binaries.

## Frozen A/B/C behavior summary (audited in Session D, unchanged)

- **Session A core round policy**: shared `PhysicalRoundNormalizer` with
  `startupFilterTriggerCount` (X distinct physical identities filtered; next
  distinct = logical index 0) and `disableCountBoundary`; multi-card shared
  `(measurementSession, triggerSeq)` decision cache; one-shot
  `roundComplete`; stable `isFinalLogicalTrigger`;
  `RoundIdentity=(measurementSession, roundGeneration)`.
- **Session B config / observability**: single persistence source
  `RingConfigDialog/Defaults` via `RoundPolicySettings` (defaults 1 / false);
  backend startup loads saved policy before any dialog exists; Apply/OK
  propagates through the Session A production setters; UI semantics frozen:
  缺失=triggersPartial, 跳号数=missingTriggerCount (production owner
  SourceCore -> Decision::TriggerGap -> CardStats), 丢包=packetsDropped,
  已采集/已过滤 from the normalizer snapshot with current->lastCompleted
  fallback.
- **Session C variable-length round**: disable=false keeps the Session A
  CountBoundary; disable=true makes the configured count a realtime imaging
  cap only (raw/save continues past N; sync/display/Ring/CUDA capped; no
  modulo/reopen inside one physical round); the 20 ms poll chain
  (NetworkController -> Backend::poll -> HostOutput::pollPhysicalRoundTimeout
  -> timeoutBoundaryIfIdle) is the single idle-boundary owner, effective in
  partial-startup rounds; timeout closure order is binding commit ->
  capture-before-reset presentation -> Ring/CUDA reset with stale cutoff or
  fail-closed admission -> async old-round PNG write -> next clean identity;
  no synthetic completion of any kind.

## Session D validation results

```text
integration audit (task sections 5-9) = PASS (12 owner paths unique; 9 duplicate-owner searches clean)
policy matrix P1-P4                    = PASS (four-combo deterministic matrix, N=4)
variable-round scenarios D1-D5        = PASS (composed from the deterministic regressions)
focal tests                           = PASS (14/14 exit 0)
full CTest                            = PASS (45/45, 24.20 s, on the candidate code SHA test tree)
Windows full build                    = PASS (cmd /c build_mingw_debug.cmd exit 0;
                                        mingw-debug configure re-ran at the candidate SHA)
Ring/CUDA software selftest           = PASS (real ImagingSvc + CUDA on testdata/14.dat;
                                        default and identity-jump runs; SHM
                                        mismatch/duplicate/gap=0)
```

Environment note: the managed sandbox intermittently corrupts child-process
spawns (one SEGFAULT-ghost ctest run; 18 unkillable ghost processes). The
authoritative full-suite result was executed outside the sandbox; see
`SESSION_D_INTEGRATION_RECEIPT_20260918.md` for details. No product/test code
change was required.

## Ring/CUDA selftest state

Session C had marked the selftest NOT RUN for lack of a traceable `--data`
acquisition file. Session D found the migration-pack ring dataset
`testdata/14.dat` (8300 columns x 4000 points, float64 column-major,
SHA256 FBCBC105343D8CAF8D1B231A93CF00CDBF4710F94A19B93B9F1A86F812E9E056,
documented in `_migration_pack/HANDOFF.md`) and ran the existing selftest on
the candidate build: default scenario and identity-jump scenario both PASS.
This is a software selftest on retained ring data, not hardware validation.

## Delivery staging identity

```text
delivery path   = D:/ChatGPT/PAERealtimeImaging/_worktrees/session-c/artifacts/build-delivery/20260918-005157_d9daa2d/
bin/            = full copy of candidate build/mingw_debug/bin (43 files)
manifests       = build-manifest.txt / git-receipt.txt / dependency-sha256.txt /
                  file-sha256.txt / validation.txt / build.log
candidate code SHA = d9daa2d7af6bb8341349e405433824e89e42bcd4
main exe SHA256 = E7752E8D256EBFE7D91CA926E4473083FCB47C9CD9DBAAF721D56E568F6B2926
dependency hashes = complete (5 required runtime DLLs + 2 import libraries;
                    cufft64_12.dll hash matches the Session A provenance record)
SESSION_D_DELIVERY_CANDIDATE = READY
```

## Hardware validation status

`HARDWARE_VALIDATION_CHECKLIST_SESSION_D_20260918.md` (repo root of the
Session D branch) is the operational checklist for the next real
FPGA/NIC/LabVIEW window: evidence identity requirements, configuration
snapshot fields, and scenarios H1-H5. No hardware measurement was initiated in
Session D.

```text
PHYSICAL_ROUND_HARDWARE_VALIDATION = PENDING
FPGA/LABVIEW_TRIGGER_SEMANTICS     = NOT PROVEN
```

## Main integration

Session D did not merge, rebase or cherry-pick anything into `main`. The A/B/C/D
chain remains on implementation branches; merging into `main` stays a separate
user decision, to be made together with the hardware acceptance outcome.

```text
MAIN_INTEGRATION = NOT PERFORMED / USER DECISION REQUIRED
```
