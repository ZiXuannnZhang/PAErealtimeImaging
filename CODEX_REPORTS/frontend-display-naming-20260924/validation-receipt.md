# 前端显示修改：计数反馈去重 + 环形弹窗通道勾选框命名 — 验证回执

- 分支：`codex/frontend-display-naming-20260924`（新建）
- 起点：`e21b019ba73c8d62556fccd0b197b456e11f544d`
- `IMPLEMENTATION_SOURCE_SHA` = `8f9e9cdcd309a831bc425456f127294ed5a003ab`
- `REPORT_HEAD` = 本提交（receipt-only）
- 日期：2026-09-24（UTC+8）

前序两批 bug 修复已实机验收通过（bug1 闸门阈值 / bug2 不覆盖已落盘数据 / H2-H1
写盘失败回退）。本轮是两项**纯前端显示**修改，不改任何数据流、计数口径或配置语义。

---

## 0. 用户已拍板的口径

| 项 | 决定 |
|---|---|
| 悬停表合并粒度 | **只删真重复**（丢失包率 → 丢包、存储队列丢弃 → 触发丢弃），其余项位置不动 |
| 通道勾选框重命名范围 | **一并改**勾选框 + 行标题 + tooltip |

---

## 1. 工作一 —— 定义核查与合并

### 1.1 核查结论（逐项核到计数点，不靠名字猜）

| 项 | 计数点 | 判定 |
|---|---|---|
| `packetLossRate` 丢失包率 | `NetworkController.cpp:840` `packetLossRate = ddrop / totalPkts`，`ddrop` 即 `packetsDropped` 增量 | **真重复**：同一量的相对表达 → 并入 `丢包` |
| `saveQueueDiscards` 存储队列丢弃 | `DataProcessor.cpp:463-469` 两处均为**双增**（同增 `triggersDiscarded`） | **真重复**：`触发丢弃` 的成因细分 → 并入 |
| `socketPacketsReceived` / `processorPacketsDequeued` / `inputQueueDepth` | `MultiPortReceiver.cpp:437` → `DataProcessor.cpp:218` | 同族（管线两点 + 积压）但**不是重复**，各自独立可判积压/丢弃 → 按「其余不动」保留 |
| `missingTriggerCount` 跳号数 | `DataProcessor.cpp:311/339` | 完全 0 包到达的 trigger 数 → **自常驻栏移入悬停表**，作独立项 |
| 其余各项 | — | 定义互不重复，位置不动 |

### 1.2 改动

`include/CardStatusFormatting.h`：

```text
常驻栏 text()   卡%1 | 缺失: %2 | 跳号数: %3 | 已采集: %4
             →  卡%1 | 缺失: %2 | 已采集: %3

悬停表 tooltip()  13 项 → 12 项
  触发完成: …
  跳号数: …                       ← 自常驻栏移入，紧随触发完成（同一触发群体的结果）
  丢包: …（丢失包率: …）           ← 并入
  处队 / 存队 / Socket接收 / Processor出队 / 批边界丢弃 / 速率 / 触发率   ← 原位不动
  触发丢弃: …（存储队列丢弃: …）   ← 并入
  已过滤: …
```

**被并入项的原名以括注保留**——只少占一行，不丢术语。数值格式不变
（`packetLossRate` 仍 `'f',6`，`recvMbps`/`triggerHz` 仍 `'f',2`）。
`MainWindow.cpp` 调用点注释同步（`formatCardStatusText` / `formatCardStatusTooltip`
为透传，签名不变）。

---

## 2. 工作二 —— 通道命名

### 2.1 下标映射（非猜测，已坐实）

```text
ImagingBypass::tryPush      channelA = frame->cardId * 2;  channelB = channelA + 1;
                            ⇒ RingBlockAssembler / enabledChannels 的物理通道号
主窗口信号选项卡            卡%1-通道%2（globalCard+1, ch==0?'A':'B'），card 外层、ch 内层
⇒ 勾选框下标 c 与选项卡逐位同序
   c :  0     1     2     3     4     5     6     7
   选项卡: 卡1-通道A 卡1-通道B 卡2-通道A 卡2-通道B 卡3-通道A 卡3-通道B 卡4-通道A 卡4-通道B
   勾选框:  1-A    1-B    2-A    2-B    3-A    3-B    4-A    4-B
```

### 2.2 新增 `include/ChannelNaming.h`（header-only，仅需 `QString`）

`channelCardNumber()` / `channelLetter()` / `channelFullName()` / `channelShortName()`
是两种显示命名的**唯一换算入口**。主窗口选项卡与环形弹窗都调它，两套命名在结构上
不可能漂移——这是「与前端选项卡对应」这条要求的可执行形式。

改动落点：

| 文件 | 改动 |
|---|---|
| `src/RingConfigDialog.cpp` | 4 处：勾选框文本、`%1 重建半径` 行标题、半径 tooltip、`%1 波长%2 延时截断起点` tooltip |
| `src/MainWindow.cpp` | 2 处 Tab 命名改调 `channelFullName(globalCard * 2 + ch)`——**输出字符串逐字不变**，只把推算收进唯一入口 |

**不动**：行标签「启用通道」、「波长1延时 / 波长2延时」子标签、`MainWindow.cpp:1530`
的 `卡%1-%2`（y 轴历史格式）。

**持久化键 `ch%1` 一字未改**（`saveDefaults` / `restoreDefaults` 仍用
`QString("ch%1").arg(c)`）⇒ 只改显示文本，已存配置与默认值不需要任何迁移。

> **与计划的一处小偏离**：计划里四个函数写成全局 `inline`，实际放进
> `namespace ChannelNaming`（与既有 `CardStatusFormatting` 命名空间风格一致，
> 也避免 4 个短名进全局作用域）。语义与计划逐条一致。

---

## 3. 测试

并入既有 `card_status_formatting_test`（只链 `Qt6::Core`、include `../include`，
本就是显示命名契约的宿主）。`add_test` 注册项计数保持 **44**。

| 用例 | 断言 |
|---|---|
| A1 | `text()` == `"卡1 \| 缺失: 3 \| 已采集: 4007"`，且**不含** `跳号数` |
| A2 | 常驻栏紧凑性守卫（不含 已过滤/处队/存队/丢弃） |
| A3 | `tooltip()` 含 `跳号数: 4`（移入） |
| A4 | 含 `丢包: 7（丢失包率: 0.000125）` 与 `触发丢弃: 5（存储队列丢弃: 3）` |
| A5 | 两项**不再单独成行**（`\n丢失包率:` / `\n存储队列丢弃:` 均不存在） |
| A6 | 其余 9 个 key 逐项仍在 + 悬停表恰为 12 行（`\n` 计数 = 11） |
| C1 | `channelShortName(0..7)` == `1-A, 1-B, 2-A, 2-B, 3-A, 3-B, 4-A, 4-B` |
| C2 | `channelFullName(0..7)` == `卡1-通道A … 卡4-通道B` |
| C3 | 两种命名都由同一 `(channelCardNumber, channelLetter)` 生成，逐 i 比对 |

夹具补了 `packetLossRate` / `triggersDiscarded` / `saveQueueDiscards` 三个原先为 0 的
字段，使 A4 的合并断言有判别力（否则括注里都是 0，看不出是否取对了字段）。
`roundDisplayForUi` 的两条既有 fallback 断言与「不得叫不完整触发缺包数」断言不动。

### 3.1 结果

```text
targeted  4/4 PASS   （card_status_formatting / round_policy_settings /
                       diagnostic_dialog / frontend_preprocessor）
full     44/44 PASS   0 FAIL / 0 SKIP / 0 NOT_RUN（本次捕获的单次原始输出）
ctest -N Total Tests: 44   —— 未增删 add_test 注册项
```

### 3.2 判别力反证

把 `CardStatusFormatting.h` 换回改动前版本、`channelShortName` 还原成 `通道%1` 后重建：

```text
24 FAILURE(S) = A1×2 + A3 + A4×2 + A5×2 + A6 + C1×8 + C3×8
对照组保持 PASS：A2（常驻栏紧凑性）、C2（完整名未变）
```

即这批断言只对本轮两处改动有判别力，未把副作用带进未改动项。反证后还原、重跑全绿。

---

## 4. 发现但本轮不修：`physical_round_normalizer_test` 并发偶发 SEGFAULT

全量 `-j 4` 时观察到 `37 - physical_round_normalizer_test (SEGFAULT)` 偶发。**多次运行
统计如实记录**（不只取绿的一次）：

| 运行方式 | 新树（含本轮改动） | 旧树（不含本轮改动） |
|---|---|---|
| 孤立运行该用例 | 13 次 → 13 PASS | 3 次 → 3 PASS |
| 全量 `ctest -j 4`（44 条） | 9 次 → 7 PASS / 2 SEGFAULT | 4 次 → **2 PASS / 2 SEGFAULT** |

**与本轮改动无关**，两条独立证据：

1. 旧树（不含本轮任何文件）复现率 2/4，**高于**新树的 2/9；
2. 链接集不相交——该用例只编译
   `tests/paimage_core/physical_round_normalizer_test.cpp` 并链接 `paimage_source`
   （`SourceCore` / `OutputQueues` / `ControlState` / `FrameConverter` /
   `PhysicalRoundNormalizer`），include 只有 `PaimageAcquisition/PhysicalRoundNormalizer.h`；
   本轮改动的 5 个文件既不在其翻译单元，也不在其链接集内。

该用例是单线程纯逻辑（0 处 thread 引用），偶发只出现在 `-j 4` 并发全量下。属**套件
既有偶发**，建议单独立项排查。本轮只做定位与影响面排除，不修。

---

## 5. 证据口径

```text
source/code correctness                  = 是
automated build / test / selftest        = 是（4/4 定向 + 44/44 全量 + 24 条判别力反证）
real hardware validation                 = 未声称（本轮为纯显示层，观感需用户实机确认）
hardware root-cause attribution          = 未声称
```

**需用户实机确认的两项观感**：① 悬停表 12 行是否比原先 13 项更好判断（跳号数是否
找得到、两项括注是否清楚）；② 勾选框 `1-A`…`4-B` 是否与现场通道一一对上（可勾单个
勾选框看对应信号选项卡是否只亮那一相）。

前两批修复的实机判据已由用户验收通过，本回执不重复声称。

---

## 6. 证据目录

```text
CODEX_REPORTS/frontend-display-naming-20260924/
  validation-receipt.md    本文件
  build-summary.txt        构建 + BuildIdentity + FLAKE TALLY + 判别力反证
  ctest-targeted.txt       4 条定向回归
  ctest-full.txt           ctest -N + 44 条全量（含偶发说明）
  git-receipt.txt          分支 / SHA / tracked-clean / diff --stat
```

`REPORT_HEAD` 相对 `IMPLEMENTATION_SOURCE_SHA` 的 diff 只能包含该目录。
