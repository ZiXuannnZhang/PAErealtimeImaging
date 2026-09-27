# CODEX_REPORTS/ring-enhance-opt-20260927-205601 — 环形重建增强 FFT 路径性能优化（A1/A2/A3 + A4 门评估）

任务：`TASKS/环形增强FFT性能优化_20260927-141217.md`（实现任务，A5 未派发、未实施）
分支：`codex/ring-reconstruction-enhancement-20260926-181824`
提交链：`7f60a38`（预算证据）→ `6ad2631`（A1+A2）→ `961f606`（A3）→ 本次证据提交。
main 侧基线 `c95917f`（本任务未改动 main 构建产物；Layer 2a 对照复用任务 1 的 main 构建并新跑参照单元）。
A0 门输入：`CODEX_REPORTS/ring-enhance-budget-20260927-151919/`。

## 阶段与验收结果

| 阶段 | commit | 内容 | 硬验收 |
| --- | --- | --- | --- |
| A1 | 6ad2631 | docs/环形重建增强.md §3.1：整样本对称反射延拓定名与理由（C₀ 连续/实 DFT/零相位；补零=矩形窗卷积起振振铃）、C₁ 斜率翻转影响面、奇延拓仅作门控备选不实现 | 文档评审项 |
| A2 | 6ad2631 | Impl 成员缓冲 work/derivative/values + 计划指针缓存（消除逐 A-line 2 次堆分配 + 导数缓冲分配 + unordered_map 查找）；计划按 sampleCount 键控、setConfig 失效 | parity --max-ulp 0：**0 差异**（192 用例）；Layer 2a both 帧 memcmp 60/60（debug+release，vs 任务 1 帧） |
| A3 | 961f606 | pack-trick 实 FFT：N/2 点复 FFT（正逆同享 2×），半谱增益（DC/Nyquist 显式），导数补偿与全关 early-return 未动 | 见下方数值证据 |
| A4 | （本证据提交） | 门评估：**不实施**（数据见 a4_gate_evaluation.md） | 数据齐附 |
| A5 | — | 未派发、未实施 | — |

## A3 数值证据（parity：tests/ring_enhancer_parity_test.cpp，内嵌 pre-A3 算法逐字转录为参考）

- **生产数据（实录 14.dat A-line ×16 根 ×{freq hmax2/hmax20, both}，len 3830，fs=250e6）：
  48/48 用例 0 差异（逐位一致）**。
- 全合成压力矩阵（12 长度 × 4 形态[常数/斜坡/脉冲/噪声] × {freq×2 Hmax, both, bipolar}，
  固定种子）：~4.5M 样本中 108,435 个差异，归因分类（规则见 raw/a3_parity_classified.log
  头注与下节）：
  - A 类（±1 float ULP，预注册允许）：10,648；
  - B 类（双精度噪声残差，|Δ| ≤ 1e-12×Σ|输入|，实测最大 4.55e-13）：97,787；
  - **C 类（算法性偏差）：0 → PASS**。
- strict 口径（仅 ±1 ULP 判收）为 VIOLATED——超出部分全部为 B 类，供审查确认。
- Layer 2a 帧级：branch-both vs 优化前（任务 1 构建）帧：
  - mingw-debug：**777,600,000 / 777,600,000 样本逐位一致（100%）**；
  - mingw-release：bitwise 777,597,060（99.9996%）+ ±1ULP 1,464 + 双精度残差 1,476，
    **超出界样本 0**（60/60 文件 OK）。
- branch-off vs main 帧 memcmp：两口径各 **60/60 逐位一致**（全关路径未受影响，
  R4 端到端保持）。

### parity 差异归因规则（随证据提交，供审查）

- A 类：float 输出 ±1 ULP（任务文档 §3 预注册允许）。
- B 类：绝对差 ≤ 1e-12×Σ|输入|（该界随 FFT 内部项幅度缩放；实测最大残差 4.55e-13，
  比界低 ≥3 个数量级；比任何算法性偏差的量级——符号翻转/错 bin，O(信号尺度)——低 ≥5 个
  数量级）。此类样本的真值≈0（斜坡×双极的设计抵消、脉冲高斯尾翼、浮点下溢区），
  float ULP 计数在该区域无数学意义。
- C 类：其余 → 测试失败。实现过程中曾出现并修复的两处真实缺陷（逆向打包符号、
  freq 输出回路误加 float 中间舍入）均被本测试以 C 类/A 类差异形式捕获，
  修复后 C=0。

## 实测加速（mingw-release，实录数据，中位；详见 optimization_summary.csv）

```text
len 3830:  freq 525-541 -> 258-265 us/A-line  (~2.0x)
           both 529-541 -> 270-291 us/A-line  (~1.9x)
           bipolar 4.3-4.6 -> 4.1 (1.04x，路径未动)
len 12330: freq 2490-2530 -> 1295-1340 us/A-line (~1.9x)
           both 2492-2511 -> 1306 (~1.9x)
```

Layer 2a（svc 端 avg_process_us/块，grid 10µm，block 200，6 圈）：

```text
mingw-release: main 443.1k / off 444.8k -> 447.3k (A3, 不变)
               branch-both 1318.6k(任务1) / 1385.4k(A2) -> 867.2k(A3)  (-34%/-37%)
mingw-debug:   main 473.4k / off 538.4k(噪声窗口) / branch-both 2258.8k(任务1) -> 2145.1k(A3)
```

## A4 门评估结论（数据见 a4_gate_evaluation.md）

**不实施混合基（next_fast_len）**：12330 点 freq 在 A3 后为 1295-1340 µs，
占每 A-line 预算（≈1990 µs，任务 1 §8）的 65-67%，不满足"接近或超过预算"的实施门；
3830 点 FFT 段占预算 13.0%（>10%，该子判据不触发"不实施"表述，但混合基对 4096 填充
仅省 7% FFT 时间 ≈ 18 µs/A-line ≈ 0.9% 预算，收益不成立）。A5 未派发、未实施。

## 过程记录与偏差（全部显式）

1. **CTest 瞬态 SegFault**：A2/A3 各出现 1-2 次 `ring_enhancer_bench` /
   `physical_round_normalizer_test` SegFault，均通过独立复跑 + 全量复跑证实为环境性
   （新建链 exe 的 AV 首触/系统干扰）；最终各以 47/47 绿色记录为准
   （a2_ctest_full_rerun.txt / a3_ctest_full_run5.txt）。失败轮日志一并保留。
2. **陈旧二进制事故**：A3 首轮 CTest 的 bench 失败为**旧版本 bench exe**（符号错误
   修复后仅重链了 parity）所致；全量重建后消失。教训已吸收：源码变更后全量重建再验收。
3. **僵尸 svc 污染**：A3 debug Layer 2a 的首轮 off/both（session=2/3，耗时异常）被
   上一次失败运行残留的 ImagingSvc 抢占 PAIR 而污染（帧数据实际来自 main 构建），
   已作废并以 off2/both2（session=1，强制进程清理后）替代；污染轮日志保留于
   a3_dbg_off.log / a3_dbg_both.log 供追溯。
4. **与任务 1 相同的环境限制**：msvc2022-release 口径不可用（本机无 MSVC/Qt-msvc kit），
   参考口径为 MinGW-Release；11.dat 不存在（实录列取自 14.dat）；Layer 2b 仍未执行
   （T_arr 为设计节拍，预算结论保持条件性）。
5. **本机偶发测试不稳（范围外发现）**：`physical_round_normalizer_test` 在本机以
   ~50% 概率 SegFault（gdb 栈：PhysicalRoundNormalizer.cpp:142 的 recentDecisions_
   deque 遍历），其源码/二进制与本任务改动**零关联**（exe 构建于任务 1 时期 15:11，
   ninja 确认无重链；该文件在分支上无任何改动）。属范围外既有/环境性问题，
   建议单独立项排查（生产代码 PaimageAcquisition/PhysicalRoundNormalizer.cpp）。
6. 两次对 CTest/证据文件的 Mimosa 扫描拦截（均为误报，调整写法后通过）不影响产物。

## 证据索引

| 文件 | 内容 |
| --- | --- |
| git-receipt.txt | 两侧 SHA、BuildIdentity、tracked-clean、生产 diff 范围 |
| optimization_summary.csv | Layer 1 bench：任务 1 基线 vs A2 vs A3（两口径） |
| layer2a_opt_summary.csv | Layer 2a 各阶段单元 × 指标 + 帧比较结论 |
| a4_gate_evaluation.md | A4 门数据与结论 |
| raw/a2_parity_calibration.log | pre-A2 构建上参考实现校准（192 用例 0 差异） |
| raw/a2_parity_maxulp0.txt | A2 验收（--max-ulp 0 = 0 差异） |
| raw/a3_parity_classified.log | A3 分类 parity 全量输出（A/B/C + 样本明细） |
| raw/a3_parity_precise_intermediate.log | 修复中间精度缺陷后的中间轮（追溯用） |
| raw/a2_/a3_memcmp_*.txt、a3_framediff_*.txt | 全关 memcmp 与帧级差异聚合 |
| raw/a2_bench_*.log、a3_bench_*.log | bench 原始输出（4×A2 + 4×A3 轮次，含噪声窗口） |
| raw/a2_dbg_*/a2_rel_*/a3_dbg_*/a3_rel_*/（目录） | Layer 2a 逐块帧（未入库，~19GB 本地） |
| raw/build_*.log | 两侧构建日志（162b712/6ad2631 间接、961f606 最终） |
| raw/ctest_*.txt | A2/A3 全量 CTest 全部轮次（含失败轮，按 README 说明解读） |
| raw/frame_diff.cpp/.exe | 帧级 float 比较工具（含分类口径） |

## 复跑命令

```text
# 构建（exact SHA、tracked clean）
cd <tree>\MC_410T_MultiCard\delivery && cmd /c build_mingw_debug.cmd
# parity（strict 口径 + 分类）
ring_enhancer_parity_test.exe --max-ulp 0 --data D:\ChatGPT\PAERealtimeImaging\testdata\14.dat   # A2 口径（要求 0 差异）
ring_enhancer_parity_test.exe --max-ulp 1 --data D:\ChatGPT\PAERealtimeImaging\testdata\14.dat   # A3 口径（C 类=0 为 PASS）
# bench
ring_enhancer_bench.exe --data D:\ChatGPT\PAERealtimeImaging\testdata\14.dat
# Layer 2a（每轮运行前清理残留 ImagingSvc 进程）
ring_svc_selftest.exe --data D:\ChatGPT\PAERealtimeImaging\testdata\14.dat --id 14 --grid-mm 0.01 --block 200 --rounds 6 [--enhance off|both] --svc <ImagingSvc.exe> --out <dir>
# 全量 CTest（需 PATH 含 Qt/MinGW bin）
ctest --test-dir <tests 构建目录>
```
