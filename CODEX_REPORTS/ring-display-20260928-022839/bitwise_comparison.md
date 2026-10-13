# 掩膜/坐标逐位对比清单 — ring-display 20260928-022839

规格来源：`TASKS/环形显示层优化_显示掩膜与毫米坐标_20260928-003123.md`
（origin/codex/task-docs @7d87eee，与本地 `_task_work/display-opt-20260928/task-spec.md`
逐字节一致）。硬验收 = 规格验收 2。

## 1. 掩膜默认关闭：与基线逐位一致（QImage / PNG）

基线参照 = fb6370b `renderFrame` / `capturePngWriter` render lambda 的**逐字转录**
（tests/ring_display_render_test.cpp 内嵌 `baselineRenderFrame` / `baselineRenderPng`，
沿 ring_enhancer_parity_test 内嵌参考先例），测试内 memcmp 全行比较（尺寸/格式/
bytesPerLine/每行字节）。

| 路径 | 用例 | 结果 |
| --- | --- | --- |
| 屏幕 QImage（renderFrame 静态函数） | 无掩膜参数（默认重载） | 逐位一致（测试 §1 + `raw/ring_display_render_test_run.txt`） |
| 同上 | 掩膜对象 enabled=false | 逐位一致 |
| 同上 | LUT 尺寸与帧失配（nx+1）→ 回退不掩 | 逐位一致 |
| 同上 | spacing 未注入（0）→ 回退不掩 | 逐位一致 |
| PNG 写入器（capturePngWriter，1600²，圈末自动保存/TimeoutPresentation 同路径） | 掩膜关闭，两波长 | 逐位一致（`baselineRenderPng` 对照） |
| 手动保存/剪贴板 | 经 `grab()` 自动继承屏幕渲染，无独立代码路径 | 由屏幕路径覆盖 |

生产等价配置实测（nx=3600、spacing=36/3599≈10.0028µm、R=6.57mm，
`screenshots/` + `raw/evidence_shots_output.txt`）：掩膜关闭渲染 vs 开启渲染，
环外 **11,604,688 / 11,604,688 像素逐位一致（100%）**。

## 2. 掩膜启用：环内置零 / 环外不变 / 关闭恢复

| 断言 | 结果 |
| --- | --- |
| 掩膜内像素 = 0（v=0 经 [0,400] 色标 → 字节 0） | 生产配置 1,355,312 / 1,355,312 = **100%**（测试小网格 §2 全部通过） |
| 掩膜外像素与关闭掩膜渲染逐位一致 | 11,604,688 / 11,604,688 = **100%** |
| PNG：全掩块输出 0、全非掩块与关态逐位一致、整图与掩膜期块平均预期逐位一致 | 测试 §3 全部通过（掩膜内源像素按 0 参与块平均） |
| 右键"自适应数据范围"跳过掩膜内像素 | 测试覆盖跳过逻辑路径；掩膜关闭时遍历顺序与基线逐位一致（x 外层/行内层 = 线性顺序不变） |
| 可逆性（关闭恢复） | `m_ringFrame` 缓存保持原始数据，掩膜仅渲染期置零；`setDisplayMask(false,…)` 后重绘即恢复（关闭态输出与基线逐位一致即其证明） |

截图：`screenshots/maskoff_*_frame1_532nm.png`（关）vs
`screenshots/maskon_*_frame2_532nm.png`（开）——环内置零、环带保留；
`ticks_pixel_fallback.png` vs `ticks_mm_mask_on.png`——回退口径/毫米口径对照
（同图样：像素口径下环内径向杂波可见，毫米口径截图中被掩膜置零）。

## 3. mm 换算自洽（标签 ↔ 掩膜同 spacing）

- 单一几何来源：`include/RingDisplayMapping.h`，全部换算只依赖 (nx, spacingMm)；
  MainWindow 由 `ringConfig().fov`（米）×1e3 + `ringDisplayNx()` 计算
  spacing=fov/(nx−1) 经 `setGridGeometry` 一次下发，毫米刻度标签与掩膜 LUT
  共用同一值（`ImagingDisplayWindow::setGridGeometry` 内同时转发两 widget 并重建 LUT）。
- `ring_display_mapping_test`（`raw/ring_display_mapping_test_run.txt`）：
  - mm↔像素往返互逆（nx 奇/偶/退化/组合全列）；
  - colToXmm ≡ 规格公式 −fov/2 + col·spacing（1e-9 容差）；
  - spacing ≠ gridSize（默认 36/3599 vs 0.01，相对差 ~0.028%）；
  - LUT ↔ 单像素判定逐像素一致（6 口径含默认 3600 全图 12,960,000 判定）；
  - **标签↔掩膜自洽（验收 4）**：200 个抽样像素用毫米标签值反推的判定与
    掩膜判定一致；
  - 生产配置边界：环心被掩/角点不掩/R 边界两侧像素判定相反。
- 证据工具复核（生产配置）：2000/2000 抽样一致
  （`raw/evidence_shots_output.txt` LABEL_MASK_CONSISTENCY）。
- y 轴口径：屏幕上=+（显示值 v=ny−1−row → y_mm=+fov/2−row·spacing），
  截图 `ticks_mm_mask_on.png` 刻度自上而下 18.0→−18.0 可视核对；
  渲染翻转（屏幕上=−y 的重建网格既定行为）未改动，注释已写入
  RingDisplayMapping.h 头注。

## 4. CTest

- 全量 51/51 绿（含新增 `ring_display_mapping_test` / `ring_display_render_test`）：
  `raw/ctest_full_51of51_green.txt`（@c649a4e tracked clean）。
- Work1 中间树（仅掩膜 commit）亦全绿 50/50（当时 render test 目标未在树中）。
- 瞬态失败轮（全部复跑绿色为准，日志保留 `raw/ctest_flaky_runs_transient.txt`）：
  `ring_enhancer_bench` SegFault ×2（提交前轮，5 连复跑全绿）、
  `physical_round_normalizer_test` SegFault ×2（任务文档预告的已知环境性问题）、
  `paimage_start_race_analyzer` ×1（分析器读到同会话刚落盘未完全 flush 的
  trace 撕裂内容；今日 scan 无残留坏 trace，单跑即绿）。三者源码与本任务改动
  零关联（无 Python/诊断工具/trace 格式改动）。

## 5. 已知限制/未验证项

- msvc2022 口径不可用（本机无 MSVC/Qt-msvc kit，与前期任务一致），参考口径为
  MinGW-Debug（canonical build_mingw_debug.cmd）。
- 真实硬件链路的端到端视觉核对（实际环阵数据、实际显示器）不在本环境可行；
  以生产等价配置（nx=3600/R=6.57mm/真实 spacing）合成数据 + 单元/组件测试覆盖。
- RingImageWidget 双击毫米编辑的反解路径由单元测试覆盖换算函数
  （xmmToColF/ymmToDisplayValueF 分数精度往返），widget 交互本体无自动化
  GUI 测试（离屏 grab 截图可视核对了刻度渲染路径）。
- 掩膜半径边界：判定为严格小于（距离 < R ⇒ 掩），与规格文字一致；
  R 恰好落在某像素中心距离上的浮点行为由 LUT↔谓词一致性测试约束（同式同源）。
