# H2/H1 写盘静默丢弃修复 + H4/H5 防复活断言 — 验证回执

- 分支：`codex/save-gate-bugfix-20260924`（续用上一轮分支）
- 起点：`f3e45587d69deb75c70b735ed2c0a5330db75a71`
- `IMPLEMENTATION_SOURCE_SHA` = `bf5c226dd87378e412e82383c07e2a63edae5fad`
- `REPORT_HEAD` = 本提交（receipt-only）
- 日期：2026-09-24（UTC+8）

来源：上一轮 save/gate bugfix 回执第 5 节列了 6 条"静默丢数据"隐患（H1..H6），
当时按范围只报告未修。本轮按**生产可达性**逐条核实后收紧结论，并按用户拍板只修
真漏部分。

---

## 0. 用户已拍板的口径

| 项 | 决定 |
|---|---|
| 修复范围 | **只修 H2/H1 + 加防复活断言**；H3/H4/H5/H6 的生产代码改动不做 |
| 写盘失败策略 | **停保存 + 一次性告警 + A/B 双侧回退** |

---

## 1. 必要性核查（可达性是分水岭）

| | 生产可达 | 证据 | 处置 |
|---|---|---|---|
| **H2 + H1** | **是** | `FileSaver::flushWriteBuffers()` 在 `OutputWorkers::cardLoop` 的 `cardSink_` 热路径上，`QFile::write()` 返回值全丢，失败后仍 `clear()` 掉 `m_writeAccumA/B`。磁盘满 / IO 错误 / 介质拔出即命中，静默丢一整段已提交数据（≤ `WRITE_BUFFER_TRIGGERS` = 16 触发）且零告警 | **本轮修复** |
| H3 | 仅断电 | `QFile::write()` 直接走 file engine（不经 Qt 缓冲落盘路径之外的隐藏副本），`QFile::close()` 后数据已在 OS 页缓存 ⇒ **应用崩溃不丢**，只有断电 / 内核崩溃才丢文件尾（≤ 16 触发） | 不做，见第 4 节 |
| H4 | 否 | 生产保存走 `HostOutput` 构造时注入的 `m_directSaveSink`；`NetworkControllerPaimage.cpp:281` 给 `DataProcessor` 的 `saveQueue` 恒传 `nullptr`、`m_saveEnabled` 恒 false ⇒ `deliverAssembled` 的旧队列分支不可达。`FileSaver::saveTriggerGroup()` / `run()` / `m_saveQueue` 在 `src/` 零调用方（只测试用） | 不改代码，**加防复活断言 S1** |
| H5 | 否 | 同上：走不到旧队列分支，`sessionGen` 因此永远是 `HostOutput::card()` 经 `tagSaveSession(resolveRound(...))` 打的标，`currentGeneration()` 覆盖不会发生 | 不改代码，**加防复活断言 S2** |
| H6 | 实质否 | `OutputWorkers::cardLoop` 把 `saveApply_` / `saveIdle_` / `cardSink_` / `saveExit_` **全部串在同一线程**，FileSaver 的文件句柄 / 序号 / 缓冲状态无并发写。真正有撕裂读风险的 `lastFileRollover()` 等访问器**生产零调用方**（只有 `filesaver_round_boundary_test` 在用） | 不做，见第 4 节 |

### 1.1 更正上一轮回执的一处定性

上一轮把 **H3** 归入"静默丢数据"。这不准确：`QFile::write()` 不经 Qt 写缓冲落盘
（`QIODevice` 的读缓冲不影响写路径），`QFile::close()` 之后数据已在 OS 页缓存，
**应用崩溃不会丢**。H3 是**断电持久性**问题，不是静默丢弃。这直接改变了取舍
（必要性明显低于 H2），故本轮更正并明确不做。

---

## 2. 本轮修复内容

### 2.1 H2/H1 —— 写盘成败可判定 + 按记录边界成对回退

`FileSaver::flushWriteBuffers()` 由 `void` 改为 `bool`：

```text
1. 记录本次刷盘前两文件的 size（= 当前记录边界）。
2. 两通道各写一次，捕获返回值。
3. 成功判据：每通道「不需要写 或 写入字节数 == 期望字节数」。短写按失败处理。
4. 成功 → 清空缓冲，返回 true。
5. 失败 → A、B 双双 resize() 回退到刷盘前的 size，清空缓冲，同步回退
   m_currentFileTriggers / m_savedCount，返回 false。
```

**成对回退**是这条修复的关键：`.dat` 是定长记录、**无文件头**，一次短写会让该文件
后续所有记录的边界永久错位且无法离线修复；而只回退失败那一侧会让 A/B 两侧记录数
失配。宁可丢整段、两侧一起退，也不留半条记录。

**计数同步回退**让诊断计数与文件里的实际记录数永远一致（现场看到的"已保存触发数"
不可能多于磁盘上的记录）。为此把 `++m_currentFileTriggers` / `m_savedCount` 提到
周期性 flush **之前**——否则扣减会差一条。

### 2.2 失败策略：停保存 + 一次性告警 + 不重入

新增 `handleWriteFault(where)`：置 `m_saving=false` + 一次性 `errorOccurred`
（该信号已由 `NetworkControllerPaimage.cpp:283` 接到 `NetworkController::errorOccurred`
→ UI 日志）。**刻意不调用 `closeFiles()`**，因此不存在
`closeFiles ↔ flushWriteBuffers` 重入，也不会在 `~FileSaver()` 里被隐式发信号。
`m_writeFaulted` 保证持续性故障只告警一次，`startSaving()` 开新会话时复位。

所有非析构调用点改为检查 `flushWriteBuffers()` / `closeFiles()` 的返回值并在失败时
退出（不再推进文件序号、不再换目录）：`startSaving` / `stopSaving` / `consumeTriggerGroup`
的会话代切换、物理轮次轮转、容量轮转、周期性落盘、`serviceCloseRequest` / `run()` 的
closeRequest 与线程退出。`~FileSaver()` 忽略结果、不发信号。

### 2.3 顺手堵掉同一函数里的另一条污染路径（超出 H2/H1 字面，已在提交信息中声明）

原实现在 `resize` / `convertBatch` 抛异常（如 `bad_alloc`）时，缓冲区已经被撑大，
**未转换的尾巴会留在 `m_writeAccumA/B` 里**，下次 flush 会被当作记录写进 `.dat`
——同属"脏字节污染无文件头记录结构"。现在两个 catch 都把缓冲回退到本次触发之前
的大小。

### 2.4 H4/H5 防复活断言（不改生产代码）

`paimage_host_output_test` 新增 S1/S2：

- **S1**：给 `DataProcessor` **故意同时接上 legacy `saveQueue` 并 `setSaveEnabled(true)`**
  （让它"本可"走旧分支），断言 `setDirectSaveSink` 严格优先、返回 `Consumed`、
  队列始终为空。
- **S2**：上游打在 `TriggerGroup::sessionGen` 上的 `resolveRound` 结果（42 / 77）
  在 sink 里原样可见，`setSessionGenReader()` 返回的 999 **不覆盖它**。

这两条把"旧队列分支 + `currentGeneration()` 弱打标"锁死：谁想让它们复活，必须先
改掉断言。

---

## 3. 修复风险与对策（本轮实际踩到的与预防的）

| # | 风险 | 对策 | 结果 |
|---|---|---|---|
| R1 | 失败策略：停保存意味着磁盘满后本轮不再保存（而非继续丢） | 已拍板选它；与现有 `bad_alloc` / 打开失败处理一致，`errorOccurred` 接 UI 日志 | 按设计 |
| R2 | **短写破坏无文件头定长记录结构** | 失败即 A/B 双双 `resize()` 回退 | W1/W2/W3/W6 逐字节断言通过 |
| R3 | `flushWriteBuffers` ↔ `closeFiles` 互调重入 | `handleWriteFault` 不 `closeFiles()`；失败路径先清缓冲 | 无重入（代码路径可查） |
| R4 | `~FileSaver()` 里 emit 信号 | `flushWriteBuffers` 只返回 bool 不 emit | 析构不发信号 |
| R5 | 回退逻辑误伤正常写路径 | W5 无故障基线逐字节 golden | 通过；反证时 W5 也保持 PASS，确认它是对照组 |
| R6 | 告警刷屏 | `m_writeFaulted` 一次性告警，`startSaving()` 复位 | W1 断言 `errors == 1` |
| R7 | 计数回退差一条（flush 发生在记账之前） | 记账移到周期性 flush 之前 | W1/W6 断言 `savedCount()` 精确回退 |
| R8 | **测试断言受 Qt 写缓冲影响**（本轮实际踩到） | `QIODevice::write` 有内部缓冲，未落盘时外部 stat 看不到字节。故障当场只断言内部计数；逐字节内容断言统一放到 `stopSaving()` 关文件后 | 见 build-summary 的反证记录；测试文件内有注释说明 |

---

## 4. 明确不修的三条及理由

| | 不修理由 | 复活防线 |
|---|---|---|
| **H3** 断电下文件尾持久性 | 仅断电/内核崩溃才丢（≤ 16 触发）；补落盘需每次关文件多一次 IO 同步，而自动保存**每轮边界都关文件**，代价在热路径上。是否值得取决于你对断电场景的要求 | 无（属产品取舍） |
| **H4 / H5** 旧队列路径 | 生产不可达（见第 1 节）。修它们等于删死代码，会逼 `filesaver_round_boundary_test` T4 的线程顺序证明改写，风险大于收益 | **S1 / S2 断言** |
| **H6** FileSaver 成员同步 | 生产热路径已串在 `OutputWorkers::cardLoop` 同一线程；有撕裂读风险的访问器生产零调用方 | 无（如后续要暴露这些访问器给 UI，需先改为返回值拷贝） |

---

## 5. 测试

### 5.1 新增契约

**W 系列**（`filesaver_round_boundary_test`，经 `FILESAVER_TEST_SEAM` 注入写故障）

| 用例 | 断言 |
|---|---|
| W1 | 双通道失败 ⇒ 计数精确回退、`m_saving` 转 false、`errorOccurred` **恰好一次**、关文件后两通道都只剩幸存段 |
| W2 | 仅 A 失败 ⇒ **A/B 成对回退**，两侧记录数与内容仍相等（B 侧成功写的字节被撤回） |
| W3 | 仅 B 失败 ⇒ W2 对称 |
| W4 | 失败后继续提交 ⇒ 计数与告警都不动、文件尺寸仍是记录整数倍（无半条记录） |
| W5 | 无故障基线 ⇒ 与 golden **逐字节一致** + 计数与磁盘记录数一致（防回退误伤） |
| W6 | 前 2 段成功、第 3 段失败 ⇒ 只丢失败段，历史 32 条逐字节完整 |

**S 系列**（`paimage_host_output_test`）：S1、S2 见 2.4。

### 5.2 结果

```text
targeted  6/6 PASS
full     44/44 PASS   0 FAIL / 0 SKIP / 0 NOT_RUN
ctest -N Total Tests: 44   —— 与前两轮一致，未增删 add_test 注册项
```

### 5.3 判别力反证

把 `flushWriteBuffers` 的失败分支换回旧语义（忽略 `write()` 返回值、不回退、恒返回
`true`）后重建运行：

```text
16 FAILURE(S)，全部落在 W 系列的写盘故障断言
对照组保持 PASS：T1..T13（覆盖/轮次/序号契约）+ W5（无故障基线）
```

即这批断言只对"写盘失败处理"有判别力，既非恒真式，也未把回退副作用带进正常写路径。
反证后 `git checkout HEAD -- FileSaver.cpp` 还原（tracked tree 立即 clean），重跑
build + 全量 ctest = 44/44 PASS。

---

## 6. 证据口径

按项目约定分层表述。本轮只声称：

```text
source/code correctness                  = 是
automated build / test / selftest        = 是（6/6 定向 + 44/44 全量 + 判别力反证）
real hardware validation                 = 未声称
hardware root-cause attribution          = 未声称
```

W 系列的故障是 seam 注入的（把 `write()` 返回值改写为 `-1`，再由回退逻辑撤掉已落
字节）。**不声称已覆盖真实磁盘满 / 介质拔出 / 驱动层短写**；实机长跑下的真实写盘
故障表现仍属未验证项。若现场遇到写盘故障，`errorOccurred` 文案会带出故障位置
（如「周期性落盘」「物理轮次轮转时的落盘」），配合 `FileSaver::fileRolled` 与
`paimage.save` 诊断事件可定位。

上一轮 bug1 / bug2 的实机复核清单（第 7 节）本轮不变，仍未执行。

---

## 7. 证据目录

```text
CODEX_REPORTS/save-write-fault-20260924/
  validation-receipt.md    本文件
  build-summary.txt        构建 + BuildIdentity + 测试树 seam 隔离 + 判别力反证
  ctest-targeted.txt       6 条定向回归
  ctest-full.txt           ctest -N + 44 条全量
  git-receipt.txt          分支 / SHA / tracked-clean / diff --stat
```

`REPORT_HEAD` 相对 `IMPLEMENTATION_SOURCE_SHA` 的 diff 只能包含该目录。
