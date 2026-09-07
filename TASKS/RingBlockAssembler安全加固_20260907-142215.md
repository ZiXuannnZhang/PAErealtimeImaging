# RingBlockAssembler safety hardening

## 1. Objective

对实时环扫数据拼装模块 `RingBlockAssembler` 做一次严格限域的 P0 安全加固：

1. 消除短 A-line 导致的越界读取 / Undefined Behavior；
2. 消除 pending overflow 淘汰后继续使用失效引用的风险；
3. 使 pending 淘汰具有正确的时间语义，不再依赖 `uint16_t triggerSeq` 的数值排序，从而正确覆盖乱序及 `65535 -> 0` wrap；
4. 新增专用自动化单元测试，直接覆盖上述故障路径；
5. 保持正常完整输入下现有 block layout、角度、双波长顺序、callback 和重建输入语义不变。

这是安全修复任务，不是 Ring 链路重构任务。

## 2. Baseline

仓库：`ZiXuannnZhang/PAErealtimeImaging`

canonical branch：`main`

本任务发布时最新远端基线：

```text
main@7dd7a4d443c0f5f73fd5873a68570c221c5e1f79
```

Codex Desktop 开始实施前必须执行：

```powershell
git fetch origin
git switch main
git merge --ff-only origin/main
git rev-parse HEAD
git rev-parse origin/main
```

要求 `HEAD == origin/main == 7dd7a4d443c0f5f73fd5873a68570c221c5e1f79`。

若 `origin/main` 已前进到不同 SHA，或本地 `main` 无法 `--ff-only` 同步：停止本任务，不得 reset/rebase/force，不得从旧 SHA 创建实现分支；在执行回执中报告 baseline drift，等待任务规格更新。

实现分支必须从上述同步完成后的 `main` 创建。禁止从 `codex/task-docs` 创建实现分支，禁止直接 push `main`，禁止 force push。

建议实现分支名：

```text
codex/ring-block-assembler-safety-20260907
```

可使用其他清晰且独立的任务分支名，但必须在回执中明确。

## 3. Problem / Evidence

目标实现：

```text
MC_410T_MultiCard/delivery/src/RingBlockAssembler.cpp
MC_410T_MultiCard/delivery/include/RingBlockAssembler.h
```

### 3.1 Short A-line overread

当前 `pushChannelLine()` 对首次到达的 channel：

```cpp
pt.lines[channelId].assign(line, line + std::min(length, m_sampDepth));
pt.mask |= (1u << channelId);
```

因此当 `0 < length < m_sampDepth` 时，保存的 vector 长度小于 `m_sampDepth`，但该 channel 仍被标记完成。

随后 `appendCompletedTrigger()` 无条件：

```cpp
std::memcpy(...,
            pt.lines[ch].data(),
            static_cast<size_t>(m_sampDepth) * sizeof(float));
```

这会读取 vector 尾部之外的内存，属于明确的 Undefined Behavior。

本任务采用的确定性修复语义：

**short line zero-pad。**

即对任何 `0 < length < m_sampDepth` 的首次有效 channel line，pending 中保存的 line 必须最终恰好为 `m_sampDepth` 个 float：前 `length` 个样本保持输入值，其余尾部填 `0.0f`，并继续按当前语义将该 channel 视为已完成。

原因：这是相对当前行为的最小语义改动，可消除 UB，同时保持 trigger completion、global trigger 计数、双波长交替和 progress 语义不因 short line 被静默跳过而改变。

对 `length >= m_sampDepth`，仍只使用前 `m_sampDepth` 个样本，并要求正常路径 byte-equivalent。

### 3.2 Eviction dangling reference

当前代码先取得：

```cpp
auto &pt = m_pending[triggerSeq];
```

然后在 pending 超过 32 时执行：

```cpp
m_pending.erase(m_pending.begin());
```

随后继续访问 `pt`。

当当前 `triggerSeq` 恰好是 map 数值最小 key 时，`erase(begin())` 可能删除当前对象，使 `pt` 立即失效；继续访问属于 Undefined Behavior。

修复后不得持有会跨越潜在 erase 操作的 map element reference / iterator，除非可以由代码结构严格证明其生命周期仍有效。推荐在淘汰完成后重新查找当前 trigger，或采用其他等价且显式安全的生命周期设计。

### 3.3 `uint16_t` numeric order is not temporal order

`m_pending` 当前为：

```cpp
std::map<uint16_t, PendingTrigger>
```

`map.begin()` 是数值最小 key，不是最早进入 pending 的 trigger。

真实 trigger sequence 会 wrap：

```text
65535 -> 0
```

因此 pending overflow 时按 `map.begin()` 淘汰会在 wrap 附近删除新的 trigger，而不是最老 pending。

本任务将“最老 pending”定义为：

**该 trigger 第一次进入 `m_pending` 的先后顺序（first-seen insertion order）。**

乱序到达时同样按 first-seen 顺序决定 eviction；不得尝试仅通过 `uint16_t` 大小、简单减法或 `map.begin()` 推断时间顺序。

## 4. Invariants

以下行为必须保持：

- enabled physical channel selection 语义；
- 同一 trigger 所有启用 channel 到齐后才 append completed trigger；
- duplicate channel 的现有 first-valid-line-wins 行为：同一 trigger/channel 已完成后，后续 duplicate 不覆盖已有 line；
- trigger-major raw block layout；
- selected-channel sector 排列；
- `triggerWlOdd` 控制的双波长交替；
- wl1 / wl2 的现有 angle step offset；
- `perChannelFrame` 内的 round / angle wrap；
- `BlockCallback` 参数、顺序和 `blockSeq` 语义；
- `ProgressCallback` 每完成一个 trigger 调用一次；
- timeout 后 `resetRoundState()` 的既有语义；
- 正常 full-length 输入下 raw / angles / channels / blockSeq 的结果；
- CUDA 输入语义、SHM ABI、reconstruction numerical behavior。

本任务不要求改变“乱序 trigger 在何时完成就何时 append”的现有完成顺序语义。不要借 eviction 修复顺手重排 completed trigger。

## 5. Prohibited Scope

禁止顺手修改：

- angle 公式；
- dual-wavelength 逻辑；
- `m_globalTrigger` 的正常计数规则；
- CUDA / `RingReconCudaConfig`；
- ImagingSvc；
- SHM v2 协议或 ABI；
- ImagingController；
- Ring input 单-slot 协议；
- 网络接收 / IP discovery；
- DataProcessor；
- RadiusCalibration；
- 与本任务无关的格式化、重命名或广泛重构。

除新增测试目标所需的构建文件修改外，不应扩大改动范围。

## 6. Recommended Design

推荐最小设计如下；允许采用等价实现，但必须满足全部验收语义。

### 6.1 Short line

在首次保存 channel line 时，确保 `pt.lines[channelId].size() == m_sampDepth`。

推荐方式之一：

1. `assign(m_sampDepth, 0.0f)`；
2. copy `min(length, m_sampDepth)` 个输入样本；
3. 设置 mask。

也可使用 resize + copy 等等，但不得读取输入 `length` 之外，也不得留下未初始化尾部。

### 6.2 Temporal eviction

给 pending trigger 维护与 `triggerSeq` 数值无关的 monotonic first-seen order。

推荐的低复杂度方案：

- `PendingTrigger` 增加 `uint64_t firstSeenOrder`；
- `RingBlockAssembler` 增加单调递增 counter；
- 仅在 trigger 首次插入 pending 时分配 order；
- pending 超过 32 时，淘汰最小 `firstSeenOrder`；
- 最大 pending 规模只有 33 左右，因此 O(32) 扫描完全可接受，不需要为了性能引入复杂容器。

也允许使用同步维护的 FIFO/deque 等设计，但必须证明 completion、reset、duplicate、eviction 时两个容器不会失配，并由测试覆盖 wrap 与当前 key 数值最小的情况。

### 6.3 Reference / iterator lifetime

不要在可能 erase map element 的代码区间持有随后继续使用的裸引用。

推荐流程：

1. 查找或创建当前 pending；
2. 保存/标记当前 line；
3. 如需 overflow eviction，按 temporal order 安全淘汰；
4. eviction 后重新 `find(triggerSeq)`；
5. 仅在当前 trigger 仍存在时检查 completion；
6. move 出 completed value 后 erase，再 append。

如果采用“新插入项绝不可能被 temporal eviction”的不变量，也仍应把生命周期安全写得清晰，不依赖悬空引用碰巧可用。

## 7. Implementation Freedom

Codex 可以自行决定：

- monotonic order 存在 `PendingTrigger` 还是独立轻量结构；
- 使用 `std::copy_n`、`memcpy` 或 vector 初始化完成 zero-pad；
- 单测使用简单自定义断言还是现有测试风格；
- 测试 target 的具体命名。

但以下不是 implementation freedom：

- short line 的本任务语义固定为 zero-pad + complete；
- eviction 必须基于 first-seen temporal order；
- 不得改变正常 angle / dual-wavelength / block layout；
- 必须新增自动化测试并实际执行。

若实现过程中发现上述固定语义与真实 caller contract 有明确冲突，停止扩大实现，在回执中给出代码证据；不要自行改成 reject/reset/协议重构。

## 8. Expected Changed Files

预期主要改动：

```text
MC_410T_MultiCard/delivery/src/RingBlockAssembler.cpp
MC_410T_MultiCard/delivery/include/RingBlockAssembler.h
MC_410T_MultiCard/delivery/tests/CMakeLists.txt
MC_410T_MultiCard/delivery/tests/ring_block_assembler_test.cpp
```

测试文件名允许等价命名。

如果改动其他文件，执行回执必须逐项说明必要性；无关文件修改视为 scope violation。

## 9. Required Tests

新增 `RingBlockAssembler` 专用自动化测试，至少覆盖以下场景。

### T1. Normal full-length line

- `length == sampDepth`；
- 至少 2 个非连续 physical channels，例如 ch1/ch3；
- 至少完成一个 block；
- 精确断言 raw trigger-major layout、angles、channels、blockSeq；
- 使用可精确表示的角度参数，使 expected output 可直接精确比较。

### T2. Short line zero-padding

- `0 < length < sampDepth`；
- short line 仍应参与 trigger completion；
- 输出前 `length` 样本与输入一致；
- 尾部 `[length, sampDepth)` 必须逐项为 `0.0f`；
- 不允许 crash / sanitizer-like symptom / 非确定尾部。

至少再覆盖一个极短输入（例如 length=1）。

### T3. Long line truncation

- `length > sampDepth`；
- 只取前 `sampDepth`；
- 正常输出不改变。

### T4. Duplicate channel

- 同一 trigger/channel 先送 valid line A，再送 duplicate line B；
- 完成后输出必须使用 A，不能被 B 覆盖；
- progress / completion 不应重复计数。

### T5. Out-of-order trigger

构造两个以上 pending trigger，以乱序 channel arrival 完成；验证：

- 每个 trigger 仍只完成一次；
- 不发生错误淘汰；
- 不因本任务引入额外 trigger 数值排序/reordering。

### T6. Pending overflow >32

- 使用至少 2 个 enabled channels；
- 只送一个 channel，使 33 个 trigger 同时 pending；
- 验证 overflow 只保留 32 个，并淘汰 first-seen 最老项；
- 通过后续补齐第二 channel 的可观测行为证明保留/淘汰对象正确，而不是只检查容器 size。

### T7. Current trigger numerically smallest at overflow

必须直接覆盖旧 dangling-reference 高风险形态：

- 先创建 32 个较大数值 trigger pending；
- 再插入数值更小的当前 trigger（例如 0）作为第 33 个；
- 新当前 trigger 不应因为 `map.begin()` 逻辑被删除；
- 后续补齐该 trigger 应正常完成一次；
- 测试不得依赖 UB 是否恰好 crash 来判断通过。

### T8. `65535 -> 0` wrap

推荐序列：先 first-see 一组高位 trigger，再跨 wrap first-see `0..N`，使总 pending 达 33。

验证：

- 淘汰的是 first-seen 最老高位 trigger；
- 新近的 trigger 0 仍被保留并可完成；
- 不得按数值最小 key 淘汰 0。

### T9. Timeout reset

- 先形成非空 pending 或 partial block；
- 触发 `timeoutResetSec`；
- 验证 timeout callback、pending/partial block/global round state 的现有 reset 行为；
- timeout 后新数据从 reset 后状态继续，blockSeq / progress 与既有语义一致。

允许使用短的 `std::this_thread::sleep_for`，但阈值应选择为在 Windows CI/本地调度抖动下仍稳定的值；不要引入生产代码 clock hook，除非确有必要并说明理由。

### T10. Normal output regression / byte-equivalence

建立至少一个 deterministic golden case，覆盖：

- 2 个以上 enabled physical channels；
- 2 个以上 trigger；
- wl1/wl2 交替；
- trigger-major raw；
- channel vector；
- angle step offset；
- blockSeq。

对 baseline 正常 full-length 语义做精确断言。raw 和 channel 输出必须 byte-equivalent；angles 应使用可精确表示的参数并做 exact comparison，避免以宽松 epsilon 掩盖公式变化。

## 10. Regression Baseline

本任务正式 regression baseline 是：

```text
7dd7a4d443c0f5f73fd5873a68570c221c5e1f79
```

除新增 short-line deterministic zero padding 和正确 temporal eviction 外，正常输入行为必须与该 baseline 一致。

特别检查：

- full-length raw bytes；
- angles；
- channels；
- blockSeq；
- duplicate first-wins；
- timeout reset；
- `ring_svc_selftest` 现有正常链路。

不要以“新单测通过”替代现有 ring service regression。

## 11. Build / Test Commands

必须在 Windows 项目实际工具链下完成本地验证，并在回执中记录**实际执行的完整命令和退出结果**。

最低要求：

### A. 配置/构建专用 tests

使用仓库 `MC_410T_MultiCard/delivery/tests/CMakeLists.txt` 构建新增测试 target。可使用已有可复现的 MinGW/Qt test build 目录，或新建独立 build 目录。

至少实际执行：

```powershell
cmake -S MC_410T_MultiCard/delivery/tests -B <test-build-dir> -G "MinGW Makefiles" <必要的 Qt/CMake 参数>
cmake --build <test-build-dir> --target <ring-block-assembler-test-target>
ctest --test-dir <test-build-dir> -R <ring-block-assembler-test-name> --output-on-failure
```

若本机使用仓库固定 Qt/CMake 绝对路径，请在回执中给出实际路径；不要把 `<...>` 占位符原样作为“执行命令”。

### B. 主工程构建

在 `MC_410T_MultiCard/delivery` 下使用仓库现有 MinGW Debug 构建方式：

```powershell
.\build_mingw_debug.cmd build
```

若现有 build 目录尚未配置，可先：

```powershell
.\build_mingw_debug.cmd configure
.\build_mingw_debug.cmd build
```

至少确认：

- `MC410T_Receiver` build success；
- `ring_svc_selftest` target build success。

### C. Ring service regression

执行当前环境可运行的 `ring_svc_selftest` baseline smoke/regression。优先复用仓库已有已验证 testdata / 参数，不要求本任务新增 CUDA 测试模式。

回执必须写明：

- 实际命令；
- 是否 external `ImagingSvc`；
- 使用的数据/参数；
- pass/fail；
- 若因当前机器缺少真实依赖无法执行，明确列为“未验证”，并提供已完成的 assembler unit tests 和构建证据；不得把未执行写成通过。

## 12. Acceptance Criteria

只有同时满足以下条件才可提交审核：

1. short A-line 不再发生 overread，尾部确定性 zero-pad；
2. `pt` / iterator 不跨潜在 erase 形成 dangling access；
3. pending overflow 淘汰按 first-seen temporal order；
4. `uint16_t` wrap `65535 -> 0` 下不错误淘汰新 trigger；
5. 当前 trigger 数值最小时的 overflow 场景无 UB，且可正常完成；
6. pending 上限仍为 32；
7. duplicate channel first-wins 保持；
8. 正常 full-length block raw/layout/angles/channels/blockSeq 不变；
9. angle、dual-wavelength、CUDA、SHM ABI 未修改；
10. 新增专用自动化测试覆盖 T1-T10；
11. 专用测试实际通过；
12. 主工程实际构建通过；
13. `ring_svc_selftest` 已执行并通过，或因明确环境原因如实列为未验证；
14. staged/final diff 只包含本任务必要修改；
15. 实现 commit 已 push 到独立远端分支；
16. 回执中的 local commit SHA 与 remote branch HEAD SHA 一致。

## 13. Required Execution Report

Codex Desktop 完成后提交完整执行回执，至少包含：

```text
Task document:
Implementation branch:
Baseline SHA:
Final commit SHA:
Remote branch HEAD SHA:
Local SHA == Remote SHA: yes/no

Changed files:
- ...

Implementation summary:
- short-line policy and exact code path
- temporal eviction representation
- reference/iterator lifetime strategy

Executed commands:
1. ...
2. ...

Build results:
- assembler unit-test target: PASS/FAIL
- MC410T_Receiver: PASS/FAIL
- ring_svc_selftest target: PASS/FAIL

Test results:
- T1 ... PASS/FAIL
- T2 ... PASS/FAIL
- ...
- T10 ... PASS/FAIL

Ring service regression:
- command/data/mode/result

Regression evidence:
- normal raw/layout/angles/channels/blockSeq comparison

Staged/final diff review:
- unexpected files: none / list

Unverified items:
- ...

Known limitations:
- ...
```

在 commit 前必须执行并检查 staged diff；commit/push 后再次确认：

```powershell
git rev-parse HEAD
git rev-parse origin/<implementation-branch>
git status --short
```

要求工作区状态、local SHA、remote SHA 如实报告。

完成后不要自行 merge `main`。等待 ChatGPT 从 GitHub 重新读取远端实现分支 HEAD、diff、源码和测试证据，并给出 `APPROVE` 或 `REQUEST_CHANGES`。
