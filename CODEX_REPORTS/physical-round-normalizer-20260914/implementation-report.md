# 物理轮次归一化与首控制触发安全过滤 — 执行报告

## 结论

```text
PROTOCOL_BLOCKED
```

Phase 0 未找到能够确定性识别 `StartupControlTrigger`，或在该触发已于 ingress 前丢失时安全推断其缺失的协议证据。依据任务文档第 5 节，已停止生产代码修改；没有加入 first-observed、计数、幅度、时序或 `triggerSeq` 猜测过滤。

## 任务身份

```text
TASK_DOCUMENT     TASKS/物理轮次归一化与首控制触发安全过滤_20260914-043019.md
BASE_BRANCH       codex/start-admission-fence-fix-20260913-003112
BASE_SHA          6313540f72544c0f68820c4815903abaa0b8c1e1
IMPLEMENTATION_BRANCH codex/physical-round-normalizer-20260914-043019
SOURCE_FIX_SHA    N/A (Phase 0 protocol block; no production source changed)
RECEIPT_SHA       9424b1c（本报告首个 report-only commit；最终分支 HEAD 另行核验）
```

`git rev-parse origin/codex/start-admission-fence-fix-20260913-003112` 的结果为任务要求的 `6313540f72544c0f68820c4815903abaa0b8c1e1`，因此 baseline 未触发 `BASELINE_BLOCKED`。

## Phase 0 协议证据调查

### A. startup/control trigger 身份

结论：`UNKNOWN`，没有正向身份规则。

已检查的采样 wire 定义和实现：

- 根 `README.md:63-65` 与 `MC_410T_MultiCard/delivery/include/Constants.h:12-15` 将采样头定义为 4 字节 `packetSeq + triggerSeq`，没有 trigger type、control bit 或 reserved 字段。
- `MC_410T_MultiCard/delivery/src/MultiPortReceiver.cpp:417-434` 只从前 4 字节解析两个 `uint16_t`，其余内容直接作为 payload；没有依据包头分类 control/startup trigger 的分支。
- 当前生产 PAimage-derived 路径 `MC_410T_MultiCard/delivery/src/PaimageAcquisition/SourceCore.cpp:92-110` 同样只读取 `le16(p)` 和 `le16(p+2)`，并将其用于组包；`SourceCore.h:16-24,30-38` 的 `Decision`/`CardFrame` 也没有 control-trigger 身份字段。
- `MC_410T_MultiCard/delivery/tools/paimage-trace-schema.md:3-7` 只记录 `triggerSeq`、`packetSeq` 和 `rawHeader[4]`。其中 `stage 2` 的 `StartupIdleClear`、`StartupBuffered` 等是主机启动缓存/状态决策，不是 wire-level control-trigger 类型。
- 两个回放器没有额外身份：`PALiveImagingSimSender/src/UdpReplaySender.cpp:347-360` 和 `MC_410T_MultiCard/delivery/src/RingUdpReplay/ring_udp_replay.cpp:182-192` 都只写入这 4 个头字节和采样 payload。
- 对 baseline 源码、模拟器、`docs/` 和根 README 执行了以下定向搜索，结果为 `NO_MATCH_FOR_CONTROL_TRIGGER_IDENTITY_TERMS`：

  ```powershell
  git grep -n -i -E 'triggerType|triggerClass|controlTrigger|startupControl|startupTrigger|control-trigger|startup-trigger|trigger.*reserved|reserved.*trigger|packetFlag|triggerFlag' origin/codex/start-admission-fence-fix-20260913-003112 -- 'MC_410T_MultiCard/delivery/**' 'PALiveImagingSimSender/**' 'docs/**' 'README.md'
  ```

### B. `triggerSeq` 的真实作用域

结论：只有主机表示层的 16 位宽度/回绕是可见的；硬件作用域仍为 `UNKNOWN`。

| 问题 | 结论 | 证据/限制 |
|---|---|---|
| 宽度 | 已知为 `uint16_t` | `DataTypes.h:13-14`；主机使用 16 位序号。 |
| 数值回绕 | 主机按 16 位回绕运算 | `DataTypes.h:14` 注释及 `SourceCore.cpp:105` 的 `uint16_t` 差值；这不能证明 FPGA 的轮次语义。 |
| 所有采集卡是否共享一个计数器 | `UNKNOWN` | 接收端按卡/端口处理，但没有硬件协议保证。 |
| 每个物理轮次是否 reset | `UNKNOWN` | `M3_实时数据传递链路.md:57-58` 将“是否连续/每块是否重置”列为待确认事项。 |
| 仅 FPGA reset 时是否 reset | `UNKNOWN` | 未发现硬件状态机或协议说明。 |
| 控制程序启动时是否 reset | `UNKNOWN` | 回放器从 `g=0` 生成测试流，不是硬件契约。 |
| startup/control trigger 是否占用一个 `triggerSeq` | `UNKNOWN` | 没有类型/边界/序号关系定义。 |
| 是否保证跨轮次连续且仅 modulo 65536 | `UNKNOWN` | 主机算术不能替代 FPGA wire-side 说明。 |

根 `README.md:129` 还明确指出“首个可见 trigger”不等于物理首 trigger，16 位 `triggerSeq` 跨轮次不能直接关联。

### C. `4001` 行为的发生范围

结论：对于仓库已有的特定实测分析段，存在“用户确认的 4001 物理预期”记录；但仓库没有协议或硬件证据证明每一个 physical round 都是 `1 + 4000`，也没有证明它只发生在控制程序首次运行或某一特定边界，因此通用发生范围为 `UNKNOWN`。

证据边界如下：

- `CODEX_REPORTS/receiver-real-regression.json:14-32` 将 round 1 标记为 `basis: user-confirmed`，并记录 `physicalExpected: 4001`、`extraExpected: 1`、`effectiveExpected: 4000`；报告中 `extraTrigger: 4` 也是事后关联字段，不是 wire-level 身份。
- `CODEX_REPORTS/接收侧诊断增强_执行任务书_20260910.md:33` 记录了用户对当次实验“每轮 4001、首个为多余触发”的确认，但同一行也明确要求报文号与物理轮次边界必须有显式依据，不能由首个收到的数据自动确定。
- 同一历史任务文档 `:105-109` 将依据区分为用户确认、协议确认、推断和未知，并要求在缺少可靠轮次起点时不得把第一个收到的有效触发当作多余触发。
- `CODEX_REPORTS/接收诊断_快速候选验证报告.md:12-13` 说明真实轨迹的完整/部分/完全未见触发统计和 `receiver-real-regression.json` 的分析边界；这属于观测/事后分类，不会补足 FPGA 的 control-trigger 定义。

因此这些记录可作为后续实机验证的业务前提，不能作为实现安全过滤所需的正向协议证据。

### D. control trigger 完全丢失时能否安全推断

结论：不能安全推断。

当前没有协议保证以下任一关系：

```text
上一物理轮次最后序号 = N
startup/control trigger = N + 1
第一条真实 scan = N + 2
```

也没有保证 control trigger 与 scan trigger 使用可区分的 type/flag，或保证跨卡、跨轮次序号不 reset。现有分析器也保留了这一边界：`MC_410T_MultiCard/delivery/tools/paimage_trace_analyze.py:432-433` 将物理 slot zero 标为 unknown，`:462` 将 unseen trigger estimate 明确标为非 physical truth。故在 control 完全未进 ingress 时，首个观测 scan 必须 fail-open 保留，不能据序号猜测并删除。

## 停止原因与未实施范围

任务文档的 Phase 0 Stop Condition 要求：无法确定性识别或安全推断时，不实现以下内容，并提交 `PROTOCOL_BLOCKED`：

- `PhysicalRoundNormalizer` 生产组件；
- first observed / first complete / first-after-timeout 过滤；
- count-based `4001` 猜测；
- amplitude、timing 或裸 `triggerSeq` heuristic；
- 保存路径和 Ring 路径的任意一侧过滤。

因此本分支只新增本报告，未修改 `DataTypes`、`DataProcessor`、`SourceCore`、`SocketReceiver`、`FileSaver`、`RingBlockAssembler`、协议解析、CUDA 或保存格式。现有 raw physical counters 未被改写。

## 测试与构建

```text
CMake configure: NOT_RUN — Phase 0 否决后没有源码或构建目标变化
Windows build:   NOT_RUN — 同上
T1–T9:           NOT_RUN — 正向协议证据缺失，不能构造合法的 control-trigger fixture
T10 regressions: NOT_RUN — 本次没有生产代码改动；执行它不会解除协议阻塞
```

没有任何测试被标记为 `PASS`，也没有生成交付构建包；不得将本结果写成 `CODE_IMPLEMENTED`、`AUTOMATED_TESTS_PASS` 或 `HARDWARE_VALIDATED`。

## 任务验收项状态

```text
[x] BASE_SHA 精确匹配
[x] Phase 0 结论已记录
[x] 没有 first-observed blind drop
[x] 没有修改生产过滤/保存/Ring 代码
[x] raw physical counters 未被篡改
[ ] shared classifier / 4001→4000 / timeout / wrap 实现
[ ] 自动化测试与 Windows build
[ ] logical round counters
```

后三类不能在协议阻塞时安全实现，不是测试失败。

## 解除阻塞所需的最小外部信息

需要来自 FPGA/控制程序协议或可重复硬件抓取的正向证据，至少包括：

1. startup/control trigger 的明确 wire identity（header flag/type/reserved field 或等价可验证标记）；
2. `triggerSeq` 的跨卡共享关系、跨物理轮次行为、FPGA reset/控制程序启动 reset 规则及 modulo/wrap 规则；
3. `4001` 额外 trigger 是每个 physical round 的保证，还是仅特定控制程序/边界行为；
4. control trigger 已丢失时，是否有协议保证可由相邻序号唯一推断；若有，给出精确序号关系和跨卡一致性保证。

在这些信息进入仓库并可由测试复核前，保留 `PROTOCOL_BLOCKED` 是唯一满足安全不变量的结果。

## 远端回执

本报告提交后需在推送前核对：

```text
local HEAD == remote codex/physical-round-normalizer-20260914-043019 HEAD
```

本任务没有硬件测试，不能报告 `HARDWARE_VALIDATED`。
