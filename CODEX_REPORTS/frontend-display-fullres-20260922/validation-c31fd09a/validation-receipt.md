# 前端全分辨率显示基础清理：构建与回归验证回执

## Task / source identity

Task document:

```text
origin/codex/task-docs:TASKS/前端全分辨率显示基础清理构建与回归验证_20260922-013000.md
```

```text
repository       = ZiXuannnZhang/PAErealtimeImaging
branch           = codex/frontend-display-fullres-20260922
BUILD_SOURCE_SHA = c31fd09aa7e1c0707faf121448be49bad9d6dfce
```

This was a validation-only task. No production source, test source, CMake file,
build script, or other tracked build input was modified.

Tracked tree was clean before the receipt files were added:

```text
git status --porcelain --untracked-files=no = empty
```

## Windows build

The required command was executed from `MC_410T_MultiCard/delivery`:

```text
cmd /c build_mingw_debug.cmd
```

Result: `exit 0`.

```text
Qt             = Qt 6.8.0 / mingw_64
MinGW          = GNU 13.1.0
CMake          = 3.30.5
Ninja          = 1.12.1
configure      = mingw-debug
build          = mingw-debug-build
native bin     = MC_410T_MultiCard/delivery/build/mingw_debug/bin
```

BuildIdentity from the newly configured main executable:

```text
PAIMAGE_GIT_SHA          = c31fd09aa7e1c0707faf121448be49bad9d6dfce
PAIMAGE_TRACKED_DIRTY    = false
PAIMAGE_BUILD_TYPE       = Debug
PAIMAGE_COMPILER         = GNU 13.1.0
PAIMAGE_PRODUCT_BASELINE = e66a29bfbbaa6534911a23d48e8624fb8d552bec
PAIMAGE_SOURCE_SHA256    = 2b2f4a8b82ff49fc35c2d3d96fe3f999a0dcc403144f39ef520c5af5cea9a6ec
```

The four required binaries were present and passed the build script's checks:

```text
PAimageReceiverDiagnostics.exe = present, 65270118 bytes
ImagingSvc.exe                 = present, 3336915 bytes
ring_svc_selftest.exe          = present, 1941308 bytes
ring_udp_replay.exe            = present, 333035 bytes
```

The configure log reported only the known missing Vulkan header and the
windeployqt translation/DX compiler warnings; the build itself completed
successfully. The unchanged auxiliary targets were already up to date in the
incremental build tree.

## Regression test tree

The main application CMake project does not register `delivery/tests`; the
repository's existing validation procedure configures that independent test
project separately. The fresh test tree used for this SHA was:

```text
MC_410T_MultiCard/delivery/build/tests_frontend_display_fullres_20260922
```

It was configured with Qt 6.8.0, GNU 13.1.0, Ninja, and CMake 3.30.5, then
built with:

```text
cmake --build MC_410T_MultiCard/delivery/build/tests_frontend_display_fullres_20260922 --parallel 8
```

Result: `290/290` build steps completed with exit `0`.

The literal application tree `build/mingw_debug` contained no registered CTest
tests (`ctest -N: Total Tests: 0`). This was preserved as evidence; no build
input was changed to wire the independent test project into it.

## Results

The six required targeted tests passed after deploying the test tree's Qt and
MinGW runtime DLLs:

```text
paimage_host_output_test              PASS
data_processor_batch_test             PASS
data_processor_imaging_isolation_test PASS
session_boundary_test                 PASS
session_boundary_receiver_test        PASS
filesaver_round_boundary_test         PASS
```

Targeted result: `6/6 PASS`, `15.25 s`.

The final full CTest invocation was the required `ctest --output-on-failure
-j 4` on the fresh test tree. `ctest -N` registered `43` tests. Final result:

```text
42 PASS
1 FAIL: physical_round_normalizer_test (SegFault under -j4)
0 SKIP / NOT_RUN
```

The failed test was rerun once as permitted by the task, in a single-test
`-j 1` invocation, and passed. The required parallel full CTest result remains
`42/43`; no source or test fix was applied.

No hardware validation was performed by this software-only task.

## Receipt scope

All tracked changes in the receipt commit are limited to:

```text
CODEX_REPORTS/frontend-display-fullres-20260922/validation-c31fd09a/
```

The final receipt commit SHA is reported out-of-band in the execution reply;
the report does not contain a self-referential commit SHA.
