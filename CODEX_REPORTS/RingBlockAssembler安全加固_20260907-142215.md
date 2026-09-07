# RingBlockAssembler 安全加固执行回执

本文件补充任务 `TASKS/RingBlockAssembler安全加固_20260907-142215.md` 的可审计执行证据。命令均在 Windows PowerShell/CMD 和项目固定 MinGW/Qt 工具链下实际执行；路径中的 `_ring_impl_stage_20260907` 是本次从 `main` 创建的独立工作 checkout。

## 版本与分支

- Task document：`TASKS/RingBlockAssembler安全加固_20260907-142215.md`
- Implementation branch：`codex/ring-block-assembler-safety-20260907`
- Baseline：`7dd7a4d443c0f5f73fd5873a68570c221c5e1f79`
- Source implementation commit：`a5823d7759bf8c6dcb35e9b6761796503606c6ea`
- Receipt file：`CODEX_REPORTS/RingBlockAssembler安全加固_20260907-142215.md`

实施前已执行任务要求的基线同步流程：

```text
git fetch origin
git switch main
git merge --ff-only origin/main
git rev-parse HEAD
git rev-parse origin/main
```

结果：`HEAD` 与 `origin/main` 均为 `7dd7a4d443c0f5f73fd5873a68570c221c5e1f79`，实现分支从该基线创建。

## 改动范围

实现提交只包含以下 4 个文件：

```text
MC_410T_MultiCard/delivery/include/RingBlockAssembler.h
MC_410T_MultiCard/delivery/src/RingBlockAssembler.cpp
MC_410T_MultiCard/delivery/tests/CMakeLists.txt
MC_410T_MultiCard/delivery/tests/ring_block_assembler_test.cpp
```

实现要点：

- 首次保存 channel line 时先分配 `sampDepth` 个 `0.0f`，再复制 `min(length, sampDepth)` 个输入样本；短线仍按原语义完成 trigger，固定长度 `memcpy` 不再越界。
- `PendingTrigger` 保存 `firstSeenOrder`，由 `m_nextPendingOrder` 分配；pending 超过 32 时扫描最小首次进入序号，避免按 `uint16_t` 数值顺序淘汰。
- 淘汰后重新 `find(triggerSeq)`，仅在当前 trigger 仍存在时检查完成；完成项 move 出后再 erase。
- 未修改角度公式、双波长逻辑、CUDA、SHM ABI、ImagingSvc、网络接收或 DataProcessor。

## A. RingBlockAssembler 专用测试

工作目录：

```text
D:\ChatGPT\PAERealtimeImaging\_ring_impl_stage_20260907\MC_410T_MultiCard\delivery\tests
```

实际配置命令：

```powershell
& "D:/Qt/Qt6.8.0/Tools/CMake_64/bin/cmake.exe" -S "D:/ChatGPT/PAERealtimeImaging/_ring_impl_stage_20260907/MC_410T_MultiCard/delivery/tests" -B "D:/ChatGPT/PAERealtimeImaging/_ring_impl_stage_20260907/MC_410T_MultiCard/delivery/build/ring_safety_tests" -G "MinGW Makefiles" -DCMAKE_CXX_COMPILER="D:/Qt/Qt6.8.0/Tools/mingw1310_64/bin/g++.exe" -DCMAKE_MAKE_PROGRAM="D:/Qt/Qt6.8.0/Tools/mingw1310_64/bin/mingw32-make.exe" -DCMAKE_PREFIX_PATH="D:/Qt/Qt6.8.0/6.8.0/mingw_64"
```

结果：exit code `0`；`Configuring done`、`Generating done`。

实际构建命令：

```powershell
& "D:/Qt/Qt6.8.0/Tools/CMake_64/bin/cmake.exe" --build "D:/ChatGPT/PAERealtimeImaging/_ring_impl_stage_20260907/MC_410T_MultiCard/delivery/build/ring_safety_tests" --target ring_block_assembler_test
```

结果：exit code `0`；输出 `[100%] Built target ring_block_assembler_test`。

实际测试命令：

```powershell
& "D:/Qt/Qt6.8.0/Tools/CMake_64/bin/ctest.exe" --test-dir "D:/ChatGPT/PAERealtimeImaging/_ring_impl_stage_20260907/MC_410T_MultiCard/delivery/build/ring_safety_tests" -R ring_block_assembler_test --output-on-failure
```

结果：exit code `0`；`1/1 Test #1: ring_block_assembler_test ... Passed`，`100% tests passed, 0 tests failed`。

测试项结果：

```text
T1 normal full-length                 PASS
T2 short line zero-padding            PASS
T3 long line truncation               PASS
T4 duplicate first-wins               PASS
T5 out-of-order triggers              PASS
T6 temporal overflow                  PASS
T7 numerically smallest current       PASS
T8 sequence wrap overflow             PASS
T9 timeout reset                      PASS
T10 normal golden regression          PASS
```

## B. 主工程 Windows 构建

工作目录：

```text
D:\ChatGPT\PAERealtimeImaging\_ring_impl_stage_20260907\MC_410T_MultiCard\delivery
```

实际配置命令：

```powershell
cmd /c build_mingw_debug.cmd configure
```

结果：exit code `0`，CMake configure 成功。

实际完整构建命令：

```powershell
cmd /c build_mingw_debug.cmd build
```

结果：exit code `0`。完整构建输出从 `[1/37]` 开始，包含：

```text
[13/37] Linking CXX executable bin\ring_svc_selftest.exe
[17/37] Linking CXX executable bin\ImagingSvc.exe
[37/37] Linking CXX executable bin\MC410T_Receiver.exe
```

随后再次执行同一构建命令复核工作区：

```text
ninja: no work to do.
```

以下目标均已实际生成：

```text
MC410T_Receiver.exe       exists
ImagingSvc.exe            exists
ring_svc_selftest.exe     exists
```

## C. Ring service regression

本次使用已构建的 `ImagingSvc.exe` 外部进程运行 self-test，实际命令如下（工作目录为上述 `delivery`）：

```powershell
$env:PATH = "$PWD\build\mingw_debug\bin;D:\Qt\Qt6.8.0\6.8.0\mingw_64\bin;D:\Qt\Qt6.8.0\Tools\mingw1310_64\bin;$env:PATH"; & ".\build\mingw_debug\bin\ring_svc_selftest.exe" --data "D:\ChatGPT\PAERealtimeImaging\testdata\14.dat" --id 14 --grid-mm 0.1 --block 200 --out "build\ring_svc_out14_safety_receipt" --svc ".\build\mingw_debug\bin\ImagingSvc.exe"
```

结果：exit code `0`，external `ImagingSvc` 启动并完成 8 通道、双波长回归：

```text
done: channels=8 K=500 rounds=2 blocks=10 total=805.5 ms avg=80.55 ms
```

输出中每块均有 `wl1`/`wl2` diff，未出现启动、IPC、共享内存或重建失败。

## D. staged diff、commit、push 与 SHA 证据

实现提交前实际执行：

```powershell
git add MC_410T_MultiCard/delivery/include/RingBlockAssembler.h MC_410T_MultiCard/delivery/src/RingBlockAssembler.cpp MC_410T_MultiCard/delivery/tests/CMakeLists.txt MC_410T_MultiCard/delivery/tests/ring_block_assembler_test.cpp
git diff --cached --check
git diff --cached --stat
```

结果：无 diff check 错误；staged diff 只有上述 4 个实现/测试文件。

```powershell
git commit -m "fix: harden RingBlockAssembler pending handling"
git push -u origin codex/ring-block-assembler-safety-20260907
```

结果：commit 成功，生成 `a5823d7759bf8c6dcb35e9b6761796503606c6ea`；push 成功。

推送后实际 SHA 核验命令：

```powershell
git fetch origin refs/heads/codex/ring-block-assembler-safety-20260907:refs/remotes/origin/codex/ring-block-assembler-safety-20260907
$local = git rev-parse HEAD
$tracking = git rev-parse origin/codex/ring-block-assembler-safety-20260907
$remote = (git ls-remote --heads origin codex/ring-block-assembler-safety-20260907).Split("`t")[0]
"HEAD=$local"
"origin/codex/ring-block-assembler-safety-20260907=$tracking"
"ls-remote=$remote"
"LOCAL_EQ_TRACKING=$($local -eq $tracking)"
"LOCAL_EQ_REMOTE=$($local -eq $remote)"
git status --short
```

实际结果：

```text
HEAD=a5823d7759bf8c6dcb35e9b6761796503606c6ea
origin/codex/ring-block-assembler-safety-20260907=a5823d7759bf8c6dcb35e9b6761796503606c6ea
ls-remote=a5823d7759bf8c6dcb35e9b6761796503606c6ea
LOCAL_EQ_TRACKING=True
LOCAL_EQ_REMOTE=True
git status --short: empty
```

本文件补充提交后，会再次执行 `git rev-parse HEAD`、`git ls-remote` 和 `git status --short`，并以该次结果作为本回执分支最终 SHA 证据。

## 未验证项与限制

- 当前工作区只有 `testdata/14.dat`，因此未运行 `11.dat` 数据集。
- 主工程构建依赖本机已有的 CUDA/FFT 预编译运行库；这些文件位于被 `.gitignore` 忽略的 build/local dependency 路径，没有作为本任务源码变更提交。
- 本任务未修改 CUDA 内核或重建数值逻辑；`ring_svc_selftest` 验证的是本次 assembler 修改接入现有 ImagingSvc/CUDA 运行链路后的 smoke/regression 行为。
