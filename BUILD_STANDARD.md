# PAERealtimeImaging 构建与交付规范

本文件是仓库级长期规范，供 Codex Desktop 在**所有后续实现、验证、回归和实机交付任务**中遵循。

它存放在 `main`，是为了成为稳定、易发现的长期约束；**它不表示所有构建都必须在 `main` 上进行**。实际构建对象必须是当前任务指定的实现分支和精确 commit。

开始构建前先读取：

```text
origin/main:PROJECT_STATUS.md
origin/main:REPOSITORY_BASELINE.md
origin/main:BUILD_STANDARD.md
```

然后读取当前 task document。`PROJECT_STATUS.md` 决定当前哪个分支/commit 是待验证对象；本文件决定如何构建和交付它。

---

## 1. 规范优先级

执行构建时按以下优先级解释要求：

1. 当前任务文档对目标分支、目标 commit、特殊构建配置和测试范围的明确要求；
2. `PROJECT_STATUS.md` 对当前待验证分支/commit 的状态声明；
3. 本文件 `BUILD_STANDARD.md`；
4. 当前目标 commit 中实际存在的 `CMakePresets.json`、构建脚本和 CMake 配置；
5. 仅在以上均未覆盖时，才允许使用临时手工命令。

如果任务文档要求与本规范冲突，执行代理必须明确指出差异，并以任务文档的**任务特定要求**为准；不得静默改变工具链、preset、依赖来源或交付结构。

---

## 2. 构建目标必须可追溯

### 2.1 不默认构建 main

Codex 必须先确认本次实际需要构建的：

```text
branch
commit SHA
working tree state
```

示例：

```powershell
Set-Location "D:\ChatGPT\PAERealtimeImaging"
git fetch --prune origin
git switch <task-branch>
git merge --ff-only origin/<task-branch>
git rev-parse HEAD
git status --short
git status --porcelain --untracked-files=no
```

构建前必须保证：

```text
HEAD == 任务要求的 commit / 远端任务分支 HEAD
tracked working tree clean
```

允许存在明确的、本规范认可的未跟踪本机构建依赖，例如 `cufft64_12.dll`；但必须在执行报告中列出来源。

### 2.2 当前硬件验证候选

截至 `PROJECT_STATUS.md` 当前状态，有两个彼此独立的硬件验证对象。

Physical round / RoundIdentity：

```text
branch = codex/physical-round-normalizer-integrated-20260916
SHA    = 52cf7713d7e0e935cb14663ec3470f3a25bfeb90
```

状态：软件整改、自动化、Windows build、真实 ImagingSvc/CUDA selftest 已通过；**物理轮次归一硬件验收仍 PENDING**。新控制环境约 `4007 physical triggers / round` 的行为仍需从本地实机日志验证，尤其是 CountBoundary / TimeoutBoundary 的真实边界与完整 reset 链。

START admission：

```text
branch = codex/start-admission-fence-fix-20260913-003112
SHA    = 6313540f72544c0f68820c4815903abaa0b8c1e1
```

状态：软件验收已 `APPROVE`，真实 FPGA/NIC 启动 ingress 验证仍 PENDING。

为任一候选构建时：

- checkout/fast-forward 到任务要求的候选分支精确 SHA；
- 从最新 `origin/main` 读取本构建规范和 `PROJECT_STATUS.md`；
- **不要**为了使用本规范而把 `main` 源码 merge/cherry-pick 到候选分支；
- 不得静默加入额外源码修改；
- 任何源码变化都会使原软件验收与实机候选身份失效，必须重新进入审查流程；
- 两个候选代表不同验证目标，不得用一个候选的现场结果替代另一个候选的 acceptance。

### 2.3 最终交付构建必须在最终 commit 上重新 configure

项目 CMake 会把 Git SHA 和 tracked dirty 状态写入 BuildIdentity。若代码提交后没有重新 configure，产物内的 build identity 可能仍对应旧 SHA。

因此最终交付前必须在**最终待交付 commit**上至少重新执行一次 configure，再 build。

禁止把“提交前编译好的旧产物”作为“提交后 commit”的正式交付物。

---

## 3. 当前标准 Windows 工具链

除任务另有明确要求，Windows 开发、回归和实机测试交付采用以下已验证环境：

```text
Qt           : Qt 6.8.0
Qt kit       : mingw_64
MinGW        : Qt Tools/mingw1310_64
Ninja        : Qt Tools/Ninja
CMake        : Qt Tools/CMake_64
C++ standard : C++17
```

当前仓库 preset 使用的默认安装路径：

```text
D:\Qt\Qt6.8.0\6.8.0\mingw_64
D:\Qt\Qt6.8.0\Tools\mingw1310_64
D:\Qt\Qt6.8.0\Tools\Ninja
D:\Qt\Qt6.8.0\Tools\CMake_64
```

执行代理必须先检查这些路径实际存在。若机器安装路径不同，不得无记录地永久改写仓库 preset；应优先使用任务允许的本地 override，并在报告中记录实际路径。若需要正式修改仓库工具链路径，应单独作为受审查的构建系统修改。

推荐 preflight：

```powershell
Test-Path "D:\Qt\Qt6.8.0\6.8.0\mingw_64"
Test-Path "D:\Qt\Qt6.8.0\Tools\mingw1310_64\bin\g++.exe"
Test-Path "D:\Qt\Qt6.8.0\Tools\Ninja\ninja.exe"
Test-Path "D:\Qt\Qt6.8.0\Tools\CMake_64\bin\cmake.exe"
& "D:\Qt\Qt6.8.0\Tools\CMake_64\bin\cmake.exe" --version
& "D:\Qt\Qt6.8.0\Tools\mingw1310_64\bin\g++.exe" --version
```

---

## 4. CMake Preset 规范

主工程目录：

```text
MC_410T_MultiCard/delivery
```

preset 文件：

```text
MC_410T_MultiCard/delivery/CMakePresets.json
```

### 4.1 `mingw-debug` — 默认正式开发/实机交付配置

```text
configure preset : mingw-debug
build preset     : mingw-debug-build
binary dir       : MC_410T_MultiCard/delivery/build/mingw_debug
build type       : Debug
USE_ZEROMQ       : ON
```

除非任务明确要求其他配置，这是 Codex 生成 Windows 可运行实机测试包的默认配置。

### 4.2 `mingw-asan` — 内存诊断专用

```text
configure preset : mingw-asan
build preset     : mingw-asan-build
binary dir       : MC_410T_MultiCard/delivery/build/mingw_asan
```

ASan 产物不是默认实机交付包。

### 4.3 `msvc2022-release` — 非默认配置

```text
configure preset : msvc2022-release
build preset     : msvc2022-release-build
```

仅在任务明确要求且第三方 import library/runtime 已验证兼容时使用。

---

## 5. 标准构建脚本

默认 Windows 主工程构建入口：

```text
MC_410T_MultiCard/delivery/build_mingw_debug.cmd
```

标准执行：

```powershell
Set-Location "D:\ChatGPT\PAERealtimeImaging\MC_410T_MultiCard\delivery"
.\build_mingw_debug.cmd
```

脚本负责设置 Qt/MinGW/Ninja/CMake、解析 Ring CUDA 依赖、执行 `mingw-debug` configure/build，并检查关键可执行文件。

至少要求生成：

```text
build/mingw_debug/bin/PAimageReceiverDiagnostics.exe
build/mingw_debug/bin/ImagingSvc.exe
build/mingw_debug/bin/ring_svc_selftest.exe
build/mingw_debug/bin/ring_udp_replay.exe
```

最终交付构建不得只执行 `build`；最终待交付 commit 必须完整 configure + build。

---

## 6. Build cache 规范

`MC_410T_MultiCard/delivery/build/` 已被 `.gitignore` 忽略。

- 普通增量开发可复用当前任务分支自己的 `build/mingw_debug`；
- 切换编译器、Qt kit、generator、关键 dependency path 后应重新 configure；
- stale cache 或跨工作区污染时只清理对应被忽略的 build 子目录；
- 不得通过 Git `reset --hard`、rebase 或删除源码来“清缓存”。

---

## 7. ZeroMQ 依赖

MinGW 正式构建使用仓库内：

```text
MC_410T_MultiCard/delivery/third_party/zeromq/include
MC_410T_MultiCard/delivery/third_party/zeromq/lib
MC_410T_MultiCard/delivery/third_party/zeromq/bin
```

运行时应随 `bin` 部署 `libzmq-v141-mt-4_3_5.dll`。

---

## 8. CUDA / Imaging 依赖规范

### 8.1 线性成像 `pa_recon_core`

仓库跟踪：

```text
MC_410T_MultiCard/delivery/libs/imaging/pa_recon_core.dll
MC_410T_MultiCard/delivery/libs/imaging/pa_recon_core.lib
```

运行还依赖未跟踪的：

```text
MC_410T_MultiCard/delivery/libs/imaging/cufft64_12.dll
```

该 DLL 必须来自可确认的 CUDA 12 runtime/toolkit；不得从随机 DLL 下载站获取。正式交付需记录来源和 SHA-256。

### 8.2 环形重建 `ring_recon_cuda`

主 MinGW 工程消费：

```text
libring_recon_cuda.dll.a
ring_recon_cuda.dll
cudart64_12.dll
```

解析优先级：

1. 本地 `MC_410T_MultiCard/delivery/build/ring_recon_cuda`；
2. fallback `_migration_pack/prebuilt_cuda/bin`。

若任务未修改 CUDA 核心，不应无条件重编 CUDA；优先使用已验证 prebuilt/runtime，并记录实际来源。

### 8.3 CUDA 依赖回执

任何正式实机包必须记录：

```text
Ring CUDA source = local build | _migration_pack/prebuilt_cuda
ring_recon_cuda.dll SHA256
cudart64_12.dll SHA256
cufft64_12.dll SHA256
pa_recon_core.dll SHA256
```

---

## 9. Qt runtime 部署

主工程 CMake 在 Windows 上使用 `windeployqt` 部署 Qt runtime/plugin。若构建成功但 `windeployqt` 未执行，不得把裸 exe 作为正式交付包。

---

## 10. 原生构建输出目录

标准 MinGW Debug 原生输出：

```text
MC_410T_MultiCard/delivery/build/mingw_debug/bin
```

应至少包含主程序、ImagingSvc、自检/回放工具、ZeroMQ、CUDA/Imaging runtime、Qt runtime/plugins 和 `diagnostic-tools/`。

---

## 11. 正式交付目录规范

仓库根目录的 `artifacts/` 已被忽略，统一作为本地验收和二进制交付 staging 根目录：

```text
artifacts/build-delivery/<YYYYMMDD-HHMMSS>_<short-sha>/
```

推荐结构：

```text
<delivery-root>/
  bin/
  build-manifest.txt
  git-receipt.txt
  dependency-sha256.txt
  file-sha256.txt
  validation.txt
  build.log
```

`bin/` 必须从 `build/mingw_debug/bin` 完整复制，不要只挑 exe；也不要把整个 CMake build tree 当运行包。

---

## 12. Build manifest 必填内容

至少记录：repository、branch、commit SHA、tracked tree clean、构建时间/机器、configure/build preset、build script、CMake/compiler/Qt/Ninja 版本、ZeroMQ/CUDA 路径与来源、native bin 和 delivery path。

`git-receipt.txt` 需记录 HEAD、branch、status、last commit；远端分支还要证明 local SHA == remote SHA。

依赖 SHA 至少覆盖：

```text
pa_recon_core.dll
cufft64_12.dll
ring_recon_cuda.dll
cudart64_12.dll
libzmq-v141-mt-4_3_5.dll
```

---

## 13. 构建成功判定

正式交付至少要求：

1. configure 成功；
2. build 成功；
3. canonical script 关键 exe 检查通过；
4. runtime DLL/Qt plugins 完整；
5. task-required tests/regressions 通过；
6. delivery staging、manifest 和 SHA256 已生成；
7. 交付 commit 与任务要求一致。

缺少关键项时不得只报告“构建成功”。

---

## 14. 实机测试包特别规则

- 不交付 ASan 版本，除非测试目标就是 ASan；
- 不允许 exe 来自不同 commit；
- 不允许手工覆盖 DLL 后不更新 manifest/hash；
- 必须提供 exact Git SHA 和完整 `bin/`；
- 必须保留 `diagnostic-tools/`；
- 若 runtime 与软件回归时不同，必须显式标记；
- 现场日志、截图、抓包和最终结论必须能回溯到 exact candidate SHA 与该测试包依赖哈希。

当前两个候选分别使用：

```text
Physical round / RoundIdentity:
  52cf7713d7e0e935cb14663ec3470f3a25bfeb90

START admission:
  6313540f72544c0f68820c4815903abaa0b8c1e1
```

若现场运行的实际二进制不是对应 SHA 的可追溯构建，则结果不能直接用于该候选的 hardware acceptance。

Physical-round 验收还有额外证据要求：不能只根据最终图像“看起来正常”判定通过。必须尽量保存能重建 distinct physical trigger count、CountBoundary、TimeoutBoundary、RoundIdentity、ring reset / epoch reset、stale drop 和 next-round clean start 的诊断记录；日志无证据的链路应标记 `UNVERIFIED`。

---

## 15. Codex 最终交付回复格式

至少返回：

```text
Build target branch:
Build target SHA:
Tracked tree clean:
Configure preset:
Build preset:
Build script:
Build result:
Native bin directory:
Delivery directory:
Delivery ZIP (if created):
Ring CUDA source:
CUDA runtime version/path:
Key runtime dependency SHA256:
Tests executed:
Tests result:
Known limitations / not verified:
```

---

## 16. Failure / stop conditions

遇到工具链缺失、CUDA 依赖来源不明、Ring CUDA 依赖缺失、cache 跨工作区污染、BuildIdentity 与目标 SHA 不符、`windeployqt` 缺失、关键 runtime 混源、任务测试失败或交付无法追溯来源时，停止并报告，不要通过修改无关源码规避。

---

## 17. 本规范的维护规则

Qt/编译器 baseline、CMake preset、canonical build script、CUDA baseline、artifact layout、ZeroMQ layout、原生输出和正式交付目录发生正式变化时，应同步更新本文件。

任务分支可做实验性 override，但只有经过审查并正式进入 `main` 后，才能成为新的仓库级默认规范。
