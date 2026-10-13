# 环形显示层优化：零值显示掩膜 + 实时成像毫米坐标

> 日期：2026-09-28
> 性质：显示层实现任务（两项独立工作，可分 commit，各自可回退）。**不触碰成像链路。**
> 实现分支：`codex/ring-reconstruction-enhancement-20260926-181824`，从当前 tip `fb6370b` 续提交；**不并入 main**（与增强/优化工作一起验收）。
> 必读前提：`origin/main` 三份治理文档（PROJECT_STATUS / REPOSITORY_BASELINE / BUILD_STANDARD）；本分支 `docs/环形重建增强.md`。
> 需求裁定记录（用户 2026-09-27/28 确认）：掩膜范围=全部显示输出；默认关闭、半径 6.57 mm；坐标范围仅改双击刻度编辑；y 轴毫米标签按屏幕口径（上=+）。

## 0. 已钉定的代码事实（免推导，直接采用）

1. **像素坐标约定**（`src/RingRecon/ring_recon_cuda.cu:460-466`，与 CPU 参考 `ring_recon.cpp:15-27` 同式）：
   `xv[i] = −fov/2 + i·fov/(nx−1)`（端点式 linspace）。**像素间距 = fov/(nx−1)，不等于 gridSize**（gridSize 仅决定 nx=ceil(fov/gridSize)）。默认 fov=36 mm、gridSize=0.01 mm ⇒ nx=3600，间距≈10.0028 µm。nx 为偶数时环心 (0,0) 落在像素 (nx/2−1, nx/2) 之间，无中心采样点。
2. **屏幕朝向**（`src/ImagingDisplayWindow.cpp:247-281` renderFrame）：QImage 列=ix（x 由小到大，屏幕左=−fov/2）、行=iy 自顶向下（行 0=iy=0=y=−fov/2 在屏幕顶）。即**屏幕右=+x、屏幕上=−y**（重建网格口径；y 轴翻转显示是既定行为，本任务不改动）。`capturePngWriter` 的 render lambda（:331-361）与 renderFrame 同向。
3. **渲染三路与消费方**：屏幕 QImage（renderFrame）；PNG 落盘（capturePngWriter：圈末自动保存 `recon_png` + TimeoutPresentation 捕获）；手动保存/剪贴板（`w->grab()`，自动继承屏幕渲染）。显示缓存 `m_ringFrame`（applyRingFrame 存入）为显示层私有，成像链路（SHM 帧区、acc/accW、snapshot kernel、FileSaver、RoundPresentation 簿记）均不读它。
4. **坐标现状**（`src/RingImageWidget.cpp:117-170, 250-322`）：x 刻度标签=列号；y 刻度标签=ny−1−行号（0 在屏幕底、ny−1 在顶）；"坐标范围"修改方式=双击轴刻度数字行内编辑（输入=该端当前显示的刻度数字；y 输入值语义=ny−1−行号，提交时反解回行号；内部视图状态 m_zoom/m_center 保持像素空间）。右键菜单无坐标范围项（不新增）。
5. **参数流转现状**：`RingConfigDialog::applyConfig()`（:643-718）→ `ImagingController::configureRing` → JSON → ImagingSvc；对话框"设为默认"持久化在 QSettings 组 `RingConfigDialog/Defaults`（应用/确定不落盘）。窗口 `ImagingDisplayWindow` 由 MainWindow 惰性创建（首个快照到达时），成员 `m_ringFrame`/`m_range1/2`/`m_lastDn`。

## 工作一：应用中心零值圆形掩膜（显示层）

### 1.1 需求

环阵内相干图像无意义、造成视觉干扰，但存在穿环传播声信号，**不得限制环内反演**——只在最终视觉效果上以零值圆形掩膜遮蔽环内图像。掩膜不影响任何成像数据与链路。

### 1.2 UI（RingConfigDialog「重建参数」页「成像网格与 DAS」组 grpGrid 末尾追加）

| 控件 | 成员名 | 规格 |
| --- | --- | --- |
| 显示掩膜半径(mm) | `m_spnDisplayMaskRadiusMm` | QDoubleSpinBox，范围 [0.1, 200]，3 位小数，默认 6.57（环阵半径），tooltip 注明单位毫米、仅作用于显示 |
| 启用显示掩膜 | `m_chkDisplayMask` | QCheckBox，默认**不勾选**（不启用=与现状逐位一致的显示行为） |

### 1.3 参数流转（不触 ABI、不进 ImagingSvc）

- 两值**不写入** `RingReconCudaConfig`（避免 ABI 变化）；**不进入** ring JSON / ImagingSvc。
- `applyConfig()` 发出新信号 `void displayMaskChanged(bool enabled, double radiusMm);`。
- MainWindow：连接该信号；持有最新值；`ImagingDisplayWindow` 已存在则即时调 `setDisplayMask(enabled, radiusMm)`，未创建则缓存、创建窗口后注入。
- 持久化：并入对话框既有"设为默认"机制（`RingConfigDialog/Defaults` 新键 `displayMaskEnabled`、`displayMaskRadiusMm`；restoreDefaults 回读）。
- 窗口侧新增 `void setDisplayMask(bool enabled, double radiusMm);`（成员 `m_maskEnabled/m_maskRadiusMm`，变更即触发重绘与 `presentationChanged` 刷新捕获——沿既有 range 变更路径）。

### 1.4 掩膜几何（抽成可测纯函数，新头 `include/RingDisplayMapping.h`）

- 判定：像素中心到环心的距离 `< R_mask` ⇒ 该像素显示为 0。像素中心 mm 坐标 = linspace 端点式（事实 1）。
- 实现：按行区间 LUT——`maskRows(nx, spacingMm, radiusMm)` 返回每行被掩列区间 `[c0,c1]`（圆的弦截口；行中心 |y|≥R 的行整行不掩）。nx/fov/R 任一变化时重建 LUT。
- `m_ringFrame` 缓存**保持原始数据**：掩膜仅在渲染期作用（渲染时置零），关闭掩膜即完整恢复，可逆；色标范围设置/自适应之外的持久化数据不受影响。

### 1.5 作用点（全部显示输出，用户已裁定）

1. `ImagingDisplayWindow::renderFrame`（静态函数）增加掩膜参数（enabled + 行区间 LUT 或等价入参），屏幕 QImage 渲染置零掩膜像素；
2. `capturePngWriter` 的 render lambda（1600² PNG：圈末自动保存 + 超时捕获）同步置零（对齐点：该 lambda 与 renderFrame 方向一致的既有注释处）；
3. 右键"自适应数据范围"（:186-215）的 min/max 遍历**跳过掩膜内像素**（避免环内相干杂波拉伸色标）；
4. 手动保存/剪贴板经 `grab()` 自动继承屏幕效果，无需额外改动。

### 1.6 边界（禁止改动）

SHM 帧区内容、`m_dispBuf`、acc/accW、`ring_snapshot_kernel`、FileSaver/原始保存、RoundPresentation 簿记状态机、`RingReconCudaConfig` 结构、ring JSON 键集、渲染方向/翻转。掩膜半径不裁剪任何重建、保存、发布行为。

## 工作二：实时成像毫米坐标

### 2.1 需求

实时成像图 xy 坐标由网格点数换为毫米：原点=图像中心，向左下为负、向右上为正（屏幕口径，用户已裁定）；双击轴刻度的坐标范围编辑同步改为毫米输入。

### 2.2 换算公式（屏幕口径）

```text
spacingMm = fovMm / (nx − 1)                      // 真实像素间距（事实 1）
x_mm(col)           = −fovMm/2 + col·spacingMm    // 屏幕左→右：负→正
y_mm(row)           = +fovMm/2 − row·spacingMm    // 屏幕上→下：正→负（上=+ 口径）
等价仿射（对现有显示数字）：x_mm = 列号·spacing − fov/2；y_mm = 显示值·spacing − fov/2
```

注意：spacing ≠ gridSize（差 ~0.008% @默认配置）；文档与 tooltip 说明以重建网格真实间距为准。显示 y = 重建网格 y 取反（既定翻转渲染不变），此关系写入代码注释。

### 2.3 实现

- `RingImageWidget`：刻度标签改毫米（自适应小数位：按当前刻度间隔取 1–3 位，避免默认配置下相邻刻度同值）；双击编辑初始值=该端毫米值，提交时毫米→显示值→行号/列号反解（内部 `m_zoom`/`m_center` 机制不变）；轴角新增静态 "mm" 单位标注（x 轴右下角，避开刻度区）。
- `spacingMm` 注入：MainWindow 从 `m_controller->ringConfig()` 取 fov、结合 nx 计算后经新方法 `ImagingDisplayWindow::setGridGeometry(double spacingMm, double fovMm)` 下发（窗口创建时 + 每次 applyConfig 后）；窗口转授两个 `RingImageWidget`。spacing 未设置（=0）前回退现行像素刻度（不误标）。
- nx/fov 变更（重建参数改动）后下一帧生效；掩膜 LUT 与 mm 标签共用同一 spacing（自洽）。

### 2.4 边界

不改渲染翻转与 PNG 降采样方向；不改内部视图状态语义（仍像素空间）；不改右键菜单结构；不改 PNG 内容（PNG 无坐标轴）。

## 验收

1. 新增 CTest 目标（沿 tests/ `add_executable`+`add_test` 模式；纯函数部分无需 Qt GUI，如需 widget 用 `QT_QPA_PLATFORM=offscreen` 先例）：
   - 映射往返/边界：mm↔像素互逆；nx 奇/偶两口径（偶数无中心像素）；fov/gridSize 组合边界；
   - 掩膜行区间 LUT：圆截口正确性（行中心距与区间端点）、R>fov/2 全掩、R 极小不掩；
   - renderFrame 掩膜断言：掩膜内=0、掩膜外与关闭掩膜的渲染逐位一致；
   - 默认恒等：掩膜关闭 + spacing 未注入时，渲染输出与基线逐位一致。
2. 硬验收（实测）：两开关全默认时屏幕 QImage 与 PNG 渲染和基线**逐位一致**；启用掩膜后环内为零、环外不变、关闭后恢复；mm 标签与掩膜几何共用 spacing 自洽（同一像素的标签 mm 值与掩膜判定一致）。
3. 全量 CTest 通过（`physical_round_normalizer_test` 本机间歇 SegFault 为已知环境性问题，复跑绿色为准，记录即可）。
4. 证据：`CODEX_REPORTS/ring-display-<YYYYMMDD-HHMMSS>/`（git-receipt、BuildIdentity、CTest 全量、掩膜/坐标对比清单、截图对比说明）。
5. 提交：工作一、工作二各自成 commit（可独立回退）；push 实现分支；local HEAD == remote HEAD；**不合 main**。
6. 禁止：改 `RingReconCudaConfig`、ring JSON、ImagingSvc、SHM 布局、渲染方向、内部视图状态语义；force push；用旧 evidence 证明新 SHA。

## 返回格式

```text
Work1/Work2 commit SHA（各自）:
changed files:
CTest 结果（含新增目标名）:
掩膜默认关闭逐位一致证明（QImage/PNG）:
掩膜启用对比说明（环内置零/环外不变/自适应范围行为）:
mm 换算自洽性说明（标签↔掩膜同 spacing）:
已知限制/未验证项:
```
