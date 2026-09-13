# PAERealtimeImaging 构建与交付规范

本文件是仓库级长期规范，供 Codex Desktop 在**所有后续实现、验证、回归和实机交付任务**中遵循。

它存放在 `main`，是为了成为稳定、易发现的长期约束；**它不表示所有构建都必须在 `main` 上进行**。实际构建对象必须是当前任务指定的实现分支和精确 commit。

---

## 1. 规范优先级

执行构建时按以下优先级解释要求：

1. 当前任务文档对目标分支、目标 commit、特殊构建配置和测试范围的明确要求；
2. 本文件 `BUILD_STANDARD.md`；
3. 当前目标 commit 中实际存在的 `CMakePresets.json`、构建脚本和 CMake 配置；
4. 仅在以上均未覆盖时，才允许使用临时手工命令。

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
git fetch origin
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

### 2.2 最终交付构建必须在最终 commit 上重新 configure

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

当前定义：

### 4.1 `mingw-debug` — 默认正式开发/实机交付配置

```text
configure preset : mingw-debug
build preset     : mingw-debug-build
binary dir       : MC_410T_MultiCard/delivery/build/mingw_debug
build type       : Debug
USE_ZEROMQ       : ON
```

除非任务明确要求其他配置，**这是 Codex 生成 Windows 可运行实机测试包的默认配置**。

### 4.2 `mingw-asan` — 内存诊断专用

```text
configure preset : mingw-asan
build preset     : mingw-asan-build
binary dir       : MC_410T_MultiCard/delivery/build/mingw_asan
```

该配置用于 AddressSanitizer 定位堆越界等问题。

**ASan 产物不是默认实机交付包。** 不得因为 ASan 构建通过就替代标准 `mingw-debug` 交付。

### 4.3 `msvc2022-release` — 非默认配置

仓库保留：

```text
configure preset : msvc2022-release
build preset     : msvc2022-release-build
```

该 preset 只有在任务明确要求 MSVC/Release，且相关第三方 import library/runtime 已验证兼容时才使用。

**仅仅因为 preset 存在，不代表 MSVC Release 是当前正式实机交付基线。**

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

该脚本负责：

1. 设置 Qt / MinGW / Ninja / CMake 路径；
2. 解析 Ring CUDA import library 和 runtime 路径；
3. 执行 `mingw-debug` configure preset；
4. 执行 `mingw-debug-build` build preset；
5. 检查关键可执行文件是否产生。

脚本要求至少生成：

```text
build/mingw_debug/bin/PAimageReceiverDiagnostics.exe
build/mingw_debug/bin/ImagingSvc.exe
build/mingw_debug/bin/ring_svc_selftest.exe
build/mingw_debug/bin/ring_udp_replay.exe
```

### 5.1 configure-only / build-only

迭代开发时允许：

```powershell
.\build_mingw_debug.cmd configure
.\build_mingw_debug.cmd build
```

但**最终交付构建不得只执行 `build`**。最终待交付 commit 必须走完整 configure + build，确保 CMake cache、BuildIdentity 和依赖路径与该 commit 一致。

### 5.2 禁止随意手写替代命令

只要 canonical build script 能覆盖当前任务，Codex 不应自行绕过脚本拼接另一套 CMake 参数。

若必须手工执行，报告必须给出完整命令，并说明为什么 canonical script 不适用。

---

## 6. Build cache 规范

`MC_410T_MultiCard/delivery/build/` 已被 `.gitignore` 忽略。

最终交付构建应避免复用来源不明或跨工具链的 cache。

推荐策略：

- 普通增量开发可复用当前任务分支自己的 `build/mingw_debug`；
- 切换编译器、Qt kit、CMake generator、关键 dependency path 后，应重新 configure；
- 出现 cache 路径污染或跨工作区迁移时，应只清理对应被忽略的 build 子目录，例如：

```text
MC_410T_MultiCard/delivery/build/mingw_debug
```

不得为了“清 build”执行 Git `reset --hard`、删除源码目录或改写工作树历史。

---

## 7. ZeroMQ 依赖

MinGW 正式构建使用仓库内：

```text
MC_410T_MultiCard/delivery/third_party/zeromq/include
MC_410T_MultiCard/delivery/third_party/zeromq/lib
MC_410T_MultiCard/delivery/third_party/zeromq/bin
```

`mingw-debug` preset 默认：

```text
USE_ZEROMQ=ON
ZMQ_INSTALL=${sourceDir}/third_party/zeromq
IMAGING_ZMQ_DIR=${sourceDir}/third_party/zeromq
```

正常情况下不应从系统 PATH 或其他工程目录临时链接另一套 ZeroMQ。

运行时应随 `bin` 部署：

```text
libzmq-v141-mt-4_3_5.dll
```

---

## 8. CUDA / Imaging 依赖规范

CUDA 依赖分为两套，不能混为一谈。

### 8.1 线性成像 `pa_recon_core`

仓库跟踪：

```text
MC_410T_MultiCard/delivery/libs/imaging/pa_recon_core.dll
MC_410T_MultiCard/delivery/libs/imaging/pa_recon_core.lib
```

其运行还依赖：

```text
cufft64_12.dll
```

仓库 `.gitignore` 明确不跟踪：

```text
MC_410T_MultiCard/delivery/libs/imaging/cufft64_12.dll
```

因此 Codex 构建前必须检查该 DLL 是否存在。

推荐来源是本机已安装、与当前 `pa_recon_core` 兼容的 CUDA 12 runtime/toolkit；当前工程历史验证使用 CUDA 12 系列，Ring CUDA CMake 默认 runtime 路径为 CUDA v12.8。

如果需要从本机 CUDA Toolkit 补齐，必须：

1. 记录实际 CUDA 安装路径和版本；
2. 复制到 `libs/imaging/cufft64_12.dll` 仅作为本地、未跟踪构建依赖；
3. 不提交该 DLL；
4. 在最终 build manifest 中记录来源和 SHA-256。

不得从任意互联网 DLL 下载站获取或替换 CUDA runtime。

### 8.2 环形重建 `ring_recon_cuda`

主 MinGW 工程本身不直接用 nvcc 编译 CUDA `.cu`；它消费已经构建好的：

```text
libring_recon_cuda.dll.a
ring_recon_cuda.dll
cudart64_12.dll
```

`build_mingw_debug.cmd` 的解析优先级必须保持：

第一优先：

```text
MC_410T_MultiCard/delivery/build/ring_recon_cuda/libring_recon_cuda.dll.a
MC_410T_MultiCard/delivery/build/ring_recon_cuda/bin/ring_recon_cuda.dll
MC_410T_MultiCard/delivery/build/ring_recon_cuda/bin/cudart64_12.dll
```

若本地 CUDA build 不存在，则 fallback：

```text
_migration_pack/prebuilt_cuda/bin/libring_recon_cuda.dll.a
_migration_pack/prebuilt_cuda/bin/ring_recon_cuda.dll
_migration_pack/prebuilt_cuda/bin/cudart64_12.dll
```

当前 `main` 中 fallback 包包含：

```text
cudart64_12.dll
libring_recon_cuda.dll.a
ring_recon_cuda.dll
```

### 8.3 如需重新构建 Ring CUDA

源码：

```text
MC_410T_MultiCard/delivery/src/RingReconCuda
```

该 CUDA 子工程当前要求：

```text
CUDA language standard : 17
CUDA architecture      : 89
CUDA runtime default   : C:/Program Files/NVIDIA GPU Computing Toolkit/CUDA/v12.8/bin
```

CUDA DLL 的构建/验证使用 nvcc + MSVC toolchain；生成供 MinGW 主工程使用的 import library/runtime 后，再由主工程 `build_mingw_debug.cmd` 消费。

若任务并未要求修改 CUDA 核心，不应为了普通上位机任务无条件重编 CUDA；优先使用已验证的 prebuilt/runtime。

### 8.4 CUDA 依赖回执

任何正式实机包必须记录实际使用的是：

```text
Ring CUDA source = local build | _migration_pack/prebuilt_cuda
ring_recon_cuda.dll SHA256
cudart64_12.dll SHA256
cufft64_12.dll SHA256
pa_recon_core.dll SHA256
```

这样实机异常才能区分应用代码变化与 GPU runtime/算法 DLL 变化。

---

## 9. Qt runtime 部署

主工程 CMake 在 Windows 上会寻找并执行：

```text
windeployqt
```

目标为：

```text
PAimageReceiverDiagnostics.exe
```

因此标准 `bin` 应自动包含 Qt runtime 和必要 plugin，例如常见的：

```text
Qt6Core.dll
Qt6Gui.dll
Qt6Widgets.dll
platforms/qwindows.dll
```

具体 DLL/plugin 集以 `windeployqt` 实际结果为准，不要维护一份手工猜测的固定 Qt DLL 列表。

如果构建成功但 `windeployqt` 未找到或未执行，**不能直接把裸 exe 当正式交付包**；必须先修正/补齐 runtime deployment，并记录处理方式。

---

## 10. 原生构建输出目录

标准 MinGW Debug 原生输出：

```text
MC_410T_MultiCard/delivery/build/mingw_debug/bin
```

这是 CMake/runtime staging 目录，不是最终人工交付目录。

其中应至少包含：

```text
PAimageReceiverDiagnostics.exe
ImagingSvc.exe
ring_svc_selftest.exe
ring_udp_replay.exe
pa_recon_core.dll
cufft64_12.dll
ring_recon_cuda.dll
cudart64_12.dll
libzmq-v141-mt-4_3_5.dll
Qt runtime / plugins
diagnostic-tools/
```

`diagnostic-tools/` 应随主程序一起交付；其中包含 startup/trace/system-capture 分析和采集工具。

---

## 11. 正式交付目录规范

仓库根目录下的：

```text
artifacts/
```

已被 `.gitignore` 忽略，统一作为本地验收证据和二进制交付 staging 根目录。

正式 build handoff 使用：

```text
artifacts/build-delivery/<YYYYMMDD-HHMMSS>_<short-sha>/
```

示例：

```text
artifacts/build-delivery/20260913-143000_6313540f/
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

### 11.1 `bin/`

必须从：

```text
MC_410T_MultiCard/delivery/build/mingw_debug/bin
```

**完整复制整个目录**，而不是只挑选 exe。

这样可保留：

- Qt plugins；
- diagnostic tools；
- ZeroMQ runtime；
- CUDA runtime；
- ImagingSvc；
- 自检/回放工具。

### 11.2 不交付整个 CMake build tree

不要把下列目录整体作为给用户的运行包：

```text
build/mingw_debug/CMakeFiles
build/mingw_debug/_deps
build.ninja
CMakeCache.txt
```

正式运行包只需要 `bin/` 和上述 manifest/evidence。

---

## 12. 推荐打包命令

以下为推荐 PowerShell 示例，时间戳可以按任务要求调整：

```powershell
$Repo = "D:\ChatGPT\PAERealtimeImaging"
$Delivery = Join-Path $Repo "MC_410T_MultiCard\delivery"
$Sha = (git -C $Repo rev-parse HEAD).Trim()
$ShortSha = $Sha.Substring(0,8)
$Stamp = Get-Date -Format "yyyyMMdd-HHmmss"
$Out = Join-Path $Repo "artifacts\build-delivery\${Stamp}_${ShortSha}"
$Bin = Join-Path $Delivery "build\mingw_debug\bin"

New-Item -ItemType Directory -Force -Path $Out | Out-Null
Copy-Item -Recurse -Force $Bin (Join-Path $Out "bin")
```

生成完整文件哈希：

```powershell
Get-ChildItem -Recurse -File (Join-Path $Out "bin") |
  Sort-Object FullName |
  Get-FileHash -Algorithm SHA256 |
  ForEach-Object { "{0}  {1}" -f $_.Hash, $_.Path.Substring($Out.Length + 1) } |
  Set-Content -Encoding UTF8 (Join-Path $Out "file-sha256.txt")
```

如果需要 ZIP 便于人工拷贝：

```powershell
Compress-Archive -Path (Join-Path $Out "*") -DestinationPath "${Out}.zip" -Force
```

ZIP 是传输格式，目录本身仍是验收证据的 canonical staging 内容。

---

## 13. Build manifest 必填内容

`build-manifest.txt` 至少记录：

```text
repository
branch
commit SHA
short SHA
tracked working tree clean = true/false
build date/time
build machine
configure preset
build preset
build script
CMake version
C++ compiler version
Qt path/version
Ninja version
USE_ZEROMQ
RING_RECON_CUDA_IMPORT_LIB
RING_RECON_CUDA_BIN
Ring CUDA source (local/prebuilt)
CUDA toolkit/runtime version and path
cufft64_12.dll source
native bin path
delivery path
```

### 13.1 Git receipt

`git-receipt.txt` 至少记录：

```powershell
git rev-parse HEAD
git branch --show-current
git status --short
git status --porcelain --untracked-files=no
git log -1 --oneline
```

若交付对象是远端实现分支，还应记录：

```text
remote branch HEAD
local == remote
```

### 13.2 Dependency SHA

`dependency-sha256.txt` 至少包括：

```text
pa_recon_core.dll
cufft64_12.dll
ring_recon_cuda.dll
cudart64_12.dll
libzmq-v141-mt-4_3_5.dll
```

---

## 14. 构建成功判定

“cmake --build 返回 0”不是完整的交付判定。

正式交付至少要求：

1. configure 成功；
2. build 成功；
3. canonical script 的关键 exe 检查通过；
4. `build/mingw_debug/bin` 存在；
5. 主程序、ImagingSvc 和 runtime DLL 齐全；
6. Qt plugins/runtime 已部署；
7. CUDA/pa_recon runtime 已部署；
8. task-required tests/regressions 通过；
9. 交付目录已生成；
10. manifest + SHA256 已生成；
11. 交付 commit 与任务要求一致。

缺少其中任何关键项时，不得只报告“构建成功”。

---

## 15. 测试与构建的关系

本规范不替代任务文档中的测试要求。

每个任务仍必须运行任务指定的：

```text
unit tests
CTest
integration tests
loopback/network regression
analyzer
hardware test preparation
```

最终 build package 应对应**已经完成软件验收的同一个 commit**。

如果为了修测试又产生新 commit，旧二进制自动失去“最终交付产物”资格，必须重新 configure/build/package。

---

## 16. 实机测试包特别规则

交给用户做真实 FPGA/NIC/GPU 实机测试时：

- 不交付 ASan 版本，除非测试目标就是 ASan；
- 不交付只链接成功但 runtime 不完整的裸 exe；
- 不允许交付目录中的 exe 来自不同 commit；
- 不允许手工覆盖某个 DLL 后不更新 manifest/hash；
- 不允许使用未说明来源的 CUDA DLL；
- 必须提供 exact Git SHA；
- 必须提供完整 `bin/`；
- 必须保留 `diagnostic-tools/`，方便现场采集 START/UDP/system-capture 证据。

如果实机测试包使用与软件回归时不同的 CUDA/runtime 依赖，必须显式标记，否则硬件结果不可直接与软件验收结论关联。

---

## 17. Codex 最终交付回复格式

Codex 在完成正式构建后，至少向用户返回：

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

不得只说：

```text
build passed
```

也不得只给 `build/mingw_debug/bin` 路径而不生成正式交付 staging。

---

## 18. Failure / stop conditions

遇到以下情况，Codex 应停止并报告，而不是通过修改无关源码来“让机器能编译”：

1. Qt/MinGW/Ninja/CMake 标准工具链缺失；
2. `cufft64_12.dll` 来源无法确认；
3. Ring CUDA import library/runtime 缺失；
4. canonical build script 与目标 branch 的 CMake schema 不匹配；
5. configure 时发现 stale cache 指向其他工作区/toolchain；
6. BuildIdentity 对应 SHA 与待交付 SHA 不一致；
7. `windeployqt` 未部署 runtime；
8. 主程序与 ImagingSvc 使用了不同来源的关键算法/runtime DLL；
9. task-required tests 不通过；
10. 交付目录无法证明来源 commit 和依赖哈希。

报告时给出：

```text
Observed problem
Exact command/error
Affected path/dependency
Whether source code is implicated
Minimal remediation
Whether task scope needs expansion
```

---

## 19. 本规范的维护规则

本文件属于仓库正式治理文档，存放在 `main`。

当以下任一项发生正式变化时，应同步更新本文件：

- Qt 主版本/kit；
- MinGW/MSVC baseline；
- CMake preset 名称；
- canonical build script；
- CUDA major/minor baseline；
- Ring CUDA artifact layout；
- ZeroMQ layout；
- CMake native output path；
- 正式 build-delivery staging path；
- 必交付可执行文件/runtime 清单。

任务分支可以为实验目的临时改变构建方法，但只有经过审查并合并到 `main` 后，才能成为新的仓库级构建规范。

---

## 20. Codex 开始任何任务时的读取要求

在读取具体任务文档后、开始实现或构建前，Codex 必须读取最新版：

```text
origin/main:REPOSITORY_BASELINE.md
origin/main:BUILD_STANDARD.md
```

推荐：

```powershell
git fetch origin
git show origin/main:REPOSITORY_BASELINE.md
git show origin/main:BUILD_STANDARD.md
git show origin/codex/task-docs:TASKS/<task-file>.md
```

其中：

- `REPOSITORY_BASELINE.md` 决定分支/历史治理；
- `BUILD_STANDARD.md` 决定默认构建、依赖、runtime 和交付方式；
- 具体 task document 决定本次任务的目标 branch/commit、特殊测试和任何显式 override。
