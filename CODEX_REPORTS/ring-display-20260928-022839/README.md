# CODEX_REPORTS/ring-display-20260928-022839 — 环形显示层优化：零值显示掩膜 + 实时成像毫米坐标

任务：`TASKS/环形显示层优化_显示掩膜与毫米坐标_20260928-003123.md`
（origin/codex/task-docs @7d87eee；与 `_task_work/display-opt-20260928/task-spec.md`
逐字节一致，副本随本证据归档 `task-spec.md`）。
分支：`codex/ring-reconstruction-enhancement-20260926-181824`（不合 main）。
提交链：`fb6370b`（任务基线 tip）→ `cfb1bbf`（工作一：掩膜）→ `c649a4e`（工作二：毫米坐标）→ 本证据提交。
职责说明：前会话已完成全部编码；本会话为【验证并交付】——未重写任何实现文件，
发现并修复 1 处测试侧真实缺陷（见 git-receipt.txt 修复记录）。

## 阶段与验收结果

| 阶段 | commit | 内容 | 硬验收 |
| --- | --- | --- | --- |
| 工作一 | cfb1bbf | 零值圆形显示掩膜：RingDisplayMapping.h（新，几何纯函数 + 掩膜行区间 LUT）、RingConfigDialog UI/信号/持久化、ImagingDisplayWindow 渲染三路置零 + 自适应范围跳过、MainWindow 缓存/注入、ring_display_mapping_test | 掩膜关=基线逐位一致；开=环内置零/环外不变；可逆 |
| 工作二 | c649a4e | 毫米坐标：RingImageWidget 毫米刻度/双击毫米编辑/mm 角标、setGridGeometry 注入链（MainWindow 几何守卫）、ring_display_render_test（内嵌 fb6370b 基线逐字转录） | 标签↔掩膜同 spacing 自洽；spacing 未注入回退像素刻度 |
| 证据 | （本提交） | git-receipt、BuildIdentity、CTest 全量、逐位对比清单、截图对比、工具与日志 | 见 bitwise_comparison.md |

中间树验证：Work1 commit 树单独全量构建 + 全量 CTest 50/50 绿（render test 目标随
工作二入库）；最终树 @c649a4e 全量构建（BuildIdentity @c649a4e tracked-clean）+
全量 CTest **51/51 绿**。最终树与本任务实现会话的验证状态逐字节一致
（git diff 备份悬空提交 2c163a4 零差异，9 个修改文件）。

## 关键数字（生产等价配置：nx=3600、spacing=36/3599≈10.0028µm、R=6.57mm）

- 掩膜关闭 vs 基线：屏幕 QImage 与 PNG（1600²）**逐位一致**（测试 memcmp 全行 +
  内嵌基线逐字转录）；掩膜关/开对比中环外 **11,604,688/11,604,688（100%）逐位一致**。
- 掩膜启用：环内 **1,355,312/1,355,312（100%）置零**；全掩块 PNG 输出 0、
  全非掩块与关态逐位一致。
- 标签↔掩膜自洽：mapping_test 200 抽样 + 证据工具 2000 抽样，**全部一致**；
  LUT↔单像素判定 6 口径逐像素一致（含默认 3600 全图）。
- CTest：**51/51**（新增 ring_display_mapping_test / ring_display_render_test）。

## 过程记录与偏差（全部显式）

1. **实现会话遗留缺陷修复 1 处**（tests/ring_display_render_test.cpp）：PNG 产物
   定位改目录枚举。根因：capturePngWriter 落盘名含毫秒时间戳（fb6370b 既有行为），
   固定名查找必失败 → 空 QImage → 4 项假失败 + 247MB 越界告警。生产代码零改动。
2. **CTest 瞬态**（与本任务改动零关联，复跑绿色为准，失败轮日志保留）：
   ring_enhancer_bench SegFault ×2（提交前轮）、physical_round_normalizer_test
   SegFault ×2（任务文档预告的已知环境性问题）、paimage_start_race_analyzer ×1
   （分析器撕裂读取同会话未 flush 完的 trace；坏 trace 扫描为空，单跑即绿）。
3. 拆分提交说明：ImagingDisplayWindow.h 的 `setGridGeometry` 声明随工作一入库
   （声明未定义、无 odr-use，工作二提交提供实现与调用方）；RingDisplayMapping.h
   与 mapping test（含毫米换算纯函数测试）随工作一入库（共享几何头 §1.4 归属）。
4. 证据截图生成工具 `raw/evidence_shots.cpp`（不入库构建，手动 moc+g++ 编译，
   offscreen + QT_QPA_FONTDIR=C:\Windows\Fonts 运行）；合成图样（亮环 + 环内杂波）
   仅用于视觉/数值对比，不进任何正式数据路径。

## 证据索引

| 文件 | 内容 |
| --- | --- |
| git-receipt.txt | 提交链、remote pre-push、tracked-clean、范围证明、BuildIdentity、缺陷修复记录 |
| BuildIdentity.txt | @c649a4e 生成头（SHA/tracked-clean/编译器）+ 嵌入目标说明 |
| bitwise_comparison.md | 逐位对比清单（QImage/PNG/grab/自适应范围/mm 自洽）+ 已知限制 |
| task-spec.md | 任务规格归档副本（与正典逐字节一致） |
| raw/ctest_full_51of51_green.txt | @c649a4e 全量 CTest 绿色记录（51/51） |
| raw/ctest_flaky_runs_transient.txt | 3 轮瞬态失败全量日志（bench/normalizer/race_analyzer） |
| raw/build_final_log.txt | 提交后全量构建日志（exit 0） |
| raw/ring_display_mapping_test_run.txt、ring_display_render_test_run.txt | 新增测试运行输出（PASS） |
| raw/evidence_shots.cpp、raw/evidence_shots_output.txt | 截图工具源码与数值输出 |
| screenshots/ | 掩膜关/开 PNG 对（1600²）、像素/毫米刻度 widget 截图对（560²） |

## 复跑命令

```text
# 构建（exact SHA、tracked clean）
cd <tree>\MC_410T_MultiCard\delivery && build_mingw_debug.cmd
# 新增测试
build\tests_all_mingw_debug\ring_display_mapping_test.exe
set QT_QPA_PLATFORM=offscreen && build\tests_all_mingw_debug\ring_display_render_test.exe
# 全量 CTest（需 PATH 含 Qt/MinGW bin）
ctest --test-dir build\tests_all_mingw_debug
# 截图工具（如需重生成 screenshots/）
g++ -std=c++17 -I include -I <QT>\include\QtCore ... raw/evidence_shots.cpp（见该文件头）
```

## 返回格式（任务文档要求）

```text
Work1/Work2 commit SHA（各自）:
  Work1（掩膜）  = cfb1bbff33e88a941618c0de8cb017663b3588a1
  Work2（毫米坐标）= c649a4e1450f8fa256ff4853e7b91d9744650b31
changed files:
  Work1: include/RingDisplayMapping.h(new) include/RingConfigDialog.h
         include/ImagingDisplayWindow.h include/MainWindow.h
         src/RingConfigDialog.cpp src/ImagingDisplayWindow.cpp
         src/MainWindow.cpp tests/CMakeLists.txt(mapping 段)
         tests/ring_display_mapping_test.cpp(new)   = 9 files, +653/−20
  Work2: include/RingImageWidget.h src/RingImageWidget.cpp
         src/ImagingDisplayWindow.cpp(setGridGeometry 实现)
         include/MainWindow.h(几何守卫成员) src/MainWindow.cpp(push+调用点)
         tests/CMakeLists.txt(render 段) tests/ring_display_render_test.cpp(new)
         = 7 files, +493/−13
CTest 结果（含新增目标名）:
  全量 51/51 绿 @c649a4e（含 ring_display_mapping_test、ring_display_render_test）；
  瞬态 3 轮（bench/normalizer SegFault、race_analyzer 撕裂读）复跑全绿并留档
掩膜默认关闭逐位一致证明（QImage/PNG）:
  renderFrame 默认重载/关态/失配回退/未注入回退 ×4 与内嵌 fb6370b 基线逐字转录
  memcmp 全行一致；capturePngWriter 关态两波长与 baselineRenderPng 逐位一致
  （raw/ring_display_render_test_run.txt）
掩膜启用对比说明（环内置零/环外不变/自适应范围行为）:
  生产等价配置环内 1,355,312/1,355,312 置零、环外 11,604,688/11,604,688 与关态
  逐位一致；PNG 全掩块黑/全非掩块与关态同；自适应范围跳过掩膜内像素（关态遍历
  顺序与基线一致）；m_ringFrame 原始缓存不动，关闭即恢复；截图 screenshots/
mm 换算自洽性说明（标签↔掩膜同 spacing）:
  MainWindow 以 ringConfig().fov(米)+ringDisplayNx() 计算 spacing=fov/(nx−1) 一次
  下发 setGridGeometry → 两 widget 刻度与掩膜 LUT 同源（RingDisplayMapping.h）；
  mapping_test 标签反推判定 200/200、证据工具 2000/2000、LUT↔谓词逐像素一致
已知限制/未验证项:
  无 MSVC 口径（无 kit）；真实硬件端到端视觉核对不可行（以生产等价配置合成
  数据覆盖）；双击毫米编辑的反解函数有单元测试、widget 交互本体无自动化 GUI 测试
```
