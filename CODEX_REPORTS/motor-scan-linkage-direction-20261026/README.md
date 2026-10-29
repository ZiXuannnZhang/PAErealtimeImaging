# motor-scan-linkage-direction-20261026 — 电机扫描方向联动（触发闩锁 / 起点角 / 方向映射）

> 分支：`codex/motor-scan-linkage-direction-20261029-0048`（基于 `origin/main`
> `a8c15cff203da959bd935120605e69dc32ff7145` 重建的独立分支）
> 代码提交：`04e348b96b0e6b6c3ab5484d4a6c2b3bb1f270f8`
> 前序报告：[`../motor-scan-linkage-20261014/README.md`](../motor-scan-linkage-20261014/README.md)
> （初版接入：参数文件解析、方向/步数消费点、CMake 与测试接入）
>
> 本报告记录 2026-10-15 与 2026-10-26 两批改动及其验证。构建/测试/运行证据在
> `evidence/`。

## 1. 目标与现场口径

实时重建的角度序列必须跟随电机的**实际扫描方向**：LabVIEW 前端（
`pact-PC-RFSG_V8b_20260618.vi`）的 `X dir`（顺/逆）由外部桥接写入
`motor_scan_params.json`，采集程序轮询后驱动环形重建的 `sectorCcw` 与起点角。

现场决定（2026-10-26）：**A-line 数不联动，只联动重建方向**。部署 INI 使用
`TotalAlinesRule=directionOnly`，`xStepsNum` 变化只记录
（`motor_linkage_unchanged` / 状态行），不再换算并下发单圈总 A-line 数。
`perEnabledChannel` / `perPeriod` 规则保留在代码中，改 INI 即可恢复。

## 2. 本轮改动

### 2.1 触发闩锁（2026-10-15）

参数若在扫描中途生效，会撕裂本轮的计数与轮次边界。因此在监视器与下游之间加入
应用闸门（`MotorScanLinkageMonitor::setApplyGate`，`std::atomic<bool>`）：

| 边界事件 | 闸门 | 语义 |
|---|---|---|
| `PhysicalRoundEvent::Kind::ControlFiltered`（本轮首个触发，扫描头部控制触发） | 关 | 冻结；本轮不再下发联动参数 |
| `PhysicalRoundEvent::Kind::TimeoutBoundary`（源空闲超时 = 扫描间隔） | 开 | 下一轮开始前恢复轮询应用 |
| `PhysicalRoundEvent::Kind::CountBoundary`（达到每圈设计触发数的计数重置） | 开 | 同上（见 §2.2） |
| `ImagingController::svcStopped`（成像停止） | 开 | 本轮已结束 |

冻结期间仍照常读文件、刷新状态行，但在文本后追加「本轮已闩锁，改动下一轮生效」，
避免把"文件已改"误读为"已下发"。冻结期的改动记为 `m_pendingApply`，开闸后的下一次
`tick()` 补发一次（多次改动只发最终值），保证不丢。

闸门只决定「何时把参数交给下游」，不改变既有超时/计数/轮次划分语义。
INI `TriggerLatch`（默认 `true`）设为 `false` 时 `setApplyGate` 为空操作，回到
"读到变化即应用"的旧行为。

### 2.2 计数重置同样开闸（2026-10-26）

初版只把 `TimeoutBoundary` 当作解冻点。但未勾选「禁用计数重置」时，轮次是以
每圈设计触发数的 `CountBoundary` 结束的——此时若电机停在下一个扫描间隔之前，
参数会一直冻结到首次超时，现场表现为"改了方向不生效"。已把
`CountBoundary` 与 `TimeoutBoundary` 并列作为开闸信号，与设计口径
「超时**或**计数重置后重新轮询」一致。

### 2.3 重建起点角随扫描方向同步（2026-10-26）

角度约定（既有）：`cfg.sectorStartDeg = 180.0 + 界面「成像扇区起始角」`，
即标准角 **180° = 九点钟**、**0° = 三点钟**；正值逆时针偏转。

| 扫描方向 | 起点角 | 旋转方向 |
|---|---|---|
| 逆时针（逆） | `sectorStartDeg = 180°`（九点钟） | `dirSign = +1`，k 递增，与历史行为逐位一致 |
| 顺时针（顺） | `sectorStartDeg = 0°`（三点钟） | `dirSign = −1`，整圈镜像 |

实现：`RingConfigDialog::setMotorScanDirection(bool ccw)` 同时设置
`m_motorSectorCcw` 与起点覆盖 `m_motorStartDeg`，并置 `m_motorStartLinked`。
`config()` 中仅当 `m_motorStartLinked` 为真才用覆盖值，否则沿用界面原逻辑。

**未联动时保持原逻辑**（设计目标之一）：`sectorCcw` 初值 1（逆时针）、起点走
界面路径（默认 0 → 180°），即联动前"九点钟起点、逆时针重建"的固定行为；覆盖值
不写回 INI，重启后若参数文件不存在则自动回到默认。

联动下发日志与诊断事件新增起点角字段：runtime.log 打印
`起点角=180°（九点钟）` / `起点角=0°（三点钟）`，events.jsonl 记录
`"sectorStartDeg": 180|0`。

### 2.4 方向映射在链路中统一（2026-10-26）

现场实测：面板开关显示「逆」时，`X dir` 的布尔值为 **false**（旧文档
「true=逆」写反，这正是链路顺逆颠倒的根因）。桥接配置 `dirTrueIsCcw` 由 `true`
改为 `false`，使三方一致：

| 环节 | 逆（CCW） | 顺（CW） |
|---|---|---|
| LabVIEW 面板 `X dir` | false | true |
| 参数文件 `direction` / `xDir` | `"CCW"` / false | `"CW"` / true |
| 采集程序状态行 | 逆时针(逆) | 顺时针(顺) |
| 重建 | 逆时针，九点钟起点 | 镜像，三点钟起点 |

修正做在桥接端（源头统一，`direction` 与 `xDir` 保持自洽），接收端 INI
`DirectionInverted` 保持 `false`。

## 3. 接口契约（便于脱离本机复现）

参数文件 `motor_scan_params.json`（`formatVersion` 必须为 1）：

```json
{"formatVersion":1,"seq":35,"writtenAt":"2026/10/28 12:30:59",
 "source":"LabVIEW:Project:ActiveX:pact-PC-RFSG_V8b_20260618.vi",
 "direction":"CW","xDir":true,"xStepsNum":2500,"xStepsPerPeriod":2}
```

- `direction`：`"CCW"`/`"CW"`，也接受 `"逆"`/`"顺"`；
- `xDir`：面板原始布尔（仅记录，接收端不消费）；
- `seq`/`writtenAt`/`source`：变化序号、写入时间、来源（诊断用）；
- 解析失败（格式版本不符/取值非法）只报状态行，不改变已联动的参数。

写入端样本（不属本仓库，记录以复现）：Python ActiveX 桥接，经工程自动化接口
`OpenProject → MyComputer → Children → item.VI` 读工程内活动面板，
200 ms 轮询，控件名 `X dir` / `X_Steps_Num` / `X_steps_per_Period`，
每 5 s 心跳重写 `writtenAt`，变化时 `seq+1`。

接收端 INI（`[MotorScanLinkage]`，全部有默认值）：

| 键 | 默认 | 本分支部署值 | 说明 |
|---|---|---|---|
| `Enabled` | `true` | `true` | 文件缺失只提示、不算错误 |
| `ParamsFile` | `<exe目录>\motor_scan_params.json` | 同默认（显式写全路径） | 须与写入端 `outPath` 一致，ASCII + 正斜杠 |
| `DirectionInverted` | `false` | `false` | `true` = 方向语义整体反转（与桥接 `dirTrueIsCcw` 二选一） |
| `TotalAlinesRule` | `perEnabledChannel` | **`directionOnly`** | `perEnabledChannel` / `perPeriod` / `directionOnly` |
| `PollMs` | `500` | `500` | 轮询周期 |
| `TriggerLatch` | `true` | `true`（未显式写） | `false` = 立即应用（旧行为回退） |

## 4. 验证

构建（BUILD_STANDARD §2.3 口径，在待交付 commit 上重新 configure）：

- `build_mingw_debug.cmd` 退出码 0；`BuildIdentity.h` 记录
  `PAIMAGE_GIT_SHA="04e348b…"`、`PAIMAGE_TRACKED_DIRTY=false`；
  `windeployqt` 已执行。详见 `evidence/build.txt`、`evidence/build-manifest.txt`。
- 四个 exe 齐全（`ring_udp_replay.exe` 源码未变，增量构建未重链）。

测试（`build/tests_mingw_debug`，全量 CTest）：

- **52/52 通过**（100%，71.99 s，exit 0），含新增 `motor_scan_linkage_test`。
  该用例覆盖：JSON 解析/取值校验、A-line 换算规则、
  监视器变化检测、**触发闩锁 5 组场景**（默认开即应用、冻结不应用但记待应用、
  冻结期多次改动只发最终值、解冻只补发一次、未应用过+冻结时开闸后才首次应用、
  `TriggerLatch=false` 保持旧行为）、组包器逆/顺角度镜像。详见
  `evidence/ctest.txt`。

运行时（`evidence/runtime-evidence.txt`）：

| 场景 | 参数文件 | 接收端结果 |
|---|---|---|
| 顺（本分支构建） | `direction=CW, xDir=true, xStepsNum=2500` | `顺时针(顺)`、`起点角=0°（三点钟）`、`totalAlines=0`（directionOnly） |
| 逆（同日更早构建，同代码） | `direction=CCW, xDir=false, xStepsNum=2000` | `逆时针(逆)`、`sectorStartDeg=180`、`totalAlines=0` |

证据边界更正（2026-10-29）：逆向记录来自当时活动面板与桥接；顺向记录是接收端消费
2026-10-28 12:30 写入的真实快照，并非 2026-10-29 当时的活面板链路。
因此只证明顺向接收消费，不证明顺向活链路或真实图像。两者均待实机验收。

## 5. 交付回执（BUILD_STANDARD §15）

```text
Build target branch  : codex/motor-scan-linkage-direction-20261029-0048
Build target SHA     : 04e348b96b0e6b6c3ab5484d4a6c2b3bb1f270f8
                       (evidence/docs commit follows; production/test sources unchanged; delivery README changed)
Tracked tree clean   : yes
Configure preset     : mingw-debug
Build preset         : mingw-debug-build
Build script         : MC_410T_MultiCard/delivery/build_mingw_debug.cmd (exit 0)
Build result         : SUCCESS (4 exe present, windeployqt done)
Native bin directory : MC_410T_MultiCard/delivery/build/mingw_debug/bin
Delivery directory   : none (branch content only; no artifacts/build-delivery package)
Delivery ZIP         : none
Ring CUDA source     : _migration_pack/prebuilt_cuda/bin (fallback prebuilt)
CUDA runtime         : cudart64_12.dll + cufft64_12.dll from the same prebuilt pack
Key runtime SHA256   : pa_recon_core.dll            c1a37e73147c1b8250f96ccdc1fcfc08ca28cecbf1cb3c046d4ab426f2ea0893
                       cufft64_12.dll               2480d8ab849d7e9a375275f6c0278b8764c14ac0c1a3bdacaf256ae4a93c5590
                       ring_recon_cuda.dll          bf40472d5a46363a35dd1084a01a8c15f13ef203f3ed5b4bb5edf767eeec14b5
                       cudart64_12.dll              c2c9a9c22a9bcba90e261825968836787b331038047a26770cffb7a583c28344
                       libzmq-v141-mt-4_3_5.dll     37610023d91951bc4177db1f96b54b28911976ac70b221a852887709a02a9ad3
                       PAimageReceiverDiagnostics.exe e897e57fb68e4ee430b1e89296bec9b83b0b4edfe5df9b682218ba0dd2f64d32
Tests executed       : full CTest (52 tests, includes motor_scan_linkage_test)
Tests result         : 52/52 PASS (100%)
Known limitations    : see §6
```

## 6. 限制与未验证项（UNVERIFIED）

1. **触发闩锁的实机行为未验证**：软件侧只覆盖了单元测试（含 5 组闩锁场景）。
   需实机确认"扫描中改方向 → 本轮不变、下一轮生效"。
2. **顺时针（三点钟起点）的成像方向未实机验证**：链路与参数已实测，但图像
   镜像是否与电机实际转向一致只能上机看。
3. **`ring_svc_selftest` 本轮未执行**；`ring_udp_replay.exe` 未重链（源码未变）。
4. **写入端桥接不在本仓库**（沿用初版报告的口径）：仓库只保证接收侧契约与
   方向映射语义；桥接的 ActiveX 读面板机制见工作区外文档。
5. **无 artifacts 交付包**：本分支只交付源码与证据，未生成
   `artifacts/build-delivery/<时间戳>_<短SHA>/`；正式实机包需另行产出并补齐 manifest。
6. **未做硬件根因归属**：本轮不改动触发计数/超时语义，7 / 4007 等现场观察
   仍未证明其 FPGA/LabVIEW 来源。

## 7. 硬件验收建议

1. 逆时针扫描：图像应与旧版逐位一致（`sectorCcw=1`、起点 180°）。
2. 顺时针扫描：起点应在三点钟方向、整体为逆时针结果的镜像；若镜像相反，
   核对 `bridge_config.txt` 的 `dirTrueIsCcw`（不得同时又开
   `DirectionInverted`）。
3. 扫描进行中翻转面板方向：本轮图像不变，状态行显示「本轮已闩锁」，
   下一轮生效（`events.jsonl` 的 `motor_linkage_applied` 应出现在轮次边界之后）。
4. 让电机停在计数边界之后、超时之前再改方向：应在本轮结束（`CountBoundary`）
   后立即生效，而不是等到首次超时。
5. 本分支 A-line 数不联动：改 `X_Steps_Num` 时状态行显示 seq 变化，但
   `motor_linkage_applied` 的 `totalAlines` 应为 0、每圈设计触发数不变。
