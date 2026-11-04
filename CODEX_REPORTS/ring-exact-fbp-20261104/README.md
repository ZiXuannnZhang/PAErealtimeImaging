# 环形改进 FBP 移植与实机测试说明

后续参数整理见 `../ring-ui-cleanup-20261104/README.md`：旧 FilteredDAS 与三项 DAS 参数已退休。
本文保留初次移植阶段的范围和验证说明，原交付包身份不变。

本任务基于 `origin/main` 的 `6ae7e8940ca7088bb4ea01415da9e303f301a24a`，
在 `codex/ring-exact-fbp-20261104` 本地分支实现。用户已选定同心圆分层；不改成平面界面，
不重新优化已验收 MATLAB 算法的成像效果。本分支和交付包均不推送远端。
最终构建 SHA、tracked-clean 状态、运行库哈希和复测结果以交付目录内的 manifest/validation 为准。
本说明不更新 main 的既有实机验收结论。

## 已移植内容

| MATLAB 最终版 | 环形实时链路 |
| --- | --- |
| 线性帽函数 Abel 积分及中心差分精确核 | 同一离散式；按各通道、波长的实际 Nt 缓存 CUDA K，cuBLAS FP32 `Q=K*P` |
| 费马折射、压力透射、二维射线管扩散 | 同心圆 Snell 不变量 `b=r*sin(alpha)/c`；径向单调及单转向直达射线，取最早到达；曲面 Jacobian 含界面曲率 |
| 直线探头位置和扫描步长 | 每条 A-line 的实际角度、逐通道半径；积分元 `ds=R*dtheta` |
| 相邻位置三点中值、100:end 去均值 | 按物理通道及波长维护跨块邻居；首尾对称端点；不跨缺口或物理圈混合 |
| 亚采样相移、每次采集卡间时延 | 8×2 固定时延快照；可选电干扰模板前缀估计；相对卡2，卡2优先物理通道4 |
| 每日线性 ys/zs 标定 | 环形半径沿用现有逐通道半径；角度及时间偏移由环形标定 JSON 提供；不把直线 ys/zs 当环形位置 |

独立 FBP C ABI 不修改已有 DAS 配置结构。保留 DAS/FilteredDAS 可切换路径；FBP 与
FilteredDAS 互斥。FBP 输出是带物理权重的 RF 累加，不使用 DAS 的 `acc/accW` 归一化，
显示色阶数值不能直接沿用 DAS 幅度。

原有 PhysicalRound/epoch、源端完成事实、全块组装、双波长显示、原始保存与发布边界继续使用。
FBP 使用每条数据自带角度，不复制旧 `shiftWL2` 的跨块数据；界面在 FBP 模式禁用该项。
同一数据位置只积分一次。中值暂存最后一个真实位置，仅在源端完成时收尾；超时/重置丢弃暂存。
CPU 预处理成功后才更新 GPU；GPU 错误清空并停止成像，不发布半更新的波长对。
配置先准备两个波长的走时表和精确核，再替换旧上下文。FBP 启动有服务端预热完成确认，
未确认时不开放成像提交；取消启动、重启及超时均有 token 隔离。

## 预处理与参数边界

FBP 消费当前主程序已有的 **frontend clone**，原始保存不受影响。前端可配置 IIR 正反滤波、
服务端 STFT/DBR/限幅/DelayCut 继续使用现有配置；FBP 增加极性、中值、去均值和时间校正。
这不是 MATLAB `FFT × |H|²` 的逐点预处理复刻：端部延拓和前端滤波器实现不同。
不能将本文的精确核/核心图像误差当作完整采集前处理的逐点等价证明。

若现场按 MATLAB 的预处理参数试用，可在现有前端设高通 0.7 MHz、低通 40 MHz、阶数均为5，
限幅两波长均设2000；STFT/DBR 按现场需要选择。本次不会静默覆盖用户已有滤波配置。
界面系统延时沿用旧语义：`systemDelay=1` 从第一个样本开始；对应 MATLAB 丢弃 `C.delay`
个样本时应填 `C.delay+1`。每个通道/波长分别计算有效 Nt，禁止不同长度错位混排。

有效 Nt 支持128..8192；网格每边2..8192；层边界最多7个；密度按内到外填写，
数量至少覆盖实际层数，未使用项不参与计算。走时表角度默认2049点，径向间隔为 GridSize/2；
单个半径表限制512 MiB，实际可配置规模还受显存限制。没有可行直达射线/临界焦散的格点不贡献；
不模拟反射、多次散射、衍射或界面导波。

## 环形标定文件

`RadiusCalibration/export_fbp_calibration.m` 导出版本1 JSON：

```matlab
% 输入应来自环形测量，不能直接填线性 Cal.ys/Cal.zs。
export_fbp_calibration('ring.json',250e6,angleOffsetDeg,delayOffsetSamples);
% 可选 X: samples × positions × 8 channels × 2 wavelengths
% X 必须与 ImagingSvc 输入处于相同 frontend 滤波阶段/参数。
export_fbp_calibration('ring.json',250e6,angleOffsetDeg,delayOffsetSamples,X,startSample,32);
```

`angleOffsetDeg` 正值为逆时针；`delayOffsetSamples` 为8×2，正值表示晚到，重建前向前校正。
模板窗口401点，JSON 按 `[channel][wavelength][sample]` 排列；模板功能要求每波长至少8条，
前缀范围8..256条；时延搜索±12采样。模板不可靠、平坦或峰在搜索边界时明确报错。
关闭/未提供模板时仅应用固定偏移。文件在配置时读取并随配置发送，圈内不会重新读盘。
不附带伪造的实测标定，也不声称已经完成环形每日自动配准；半径/角度/固定延时需现场环形测量。

## 构建与验证复现

主工程仍遵循根目录 `BUILD_STANDARD.md` 的 Qt6.8/MinGW13.1 Debug 构建。
本任务修改了 CUDA 核心，必须先构建本地 CUDA，不能使用不含 FBP 导出符号的旧 prebuilt：

```powershell
cd MC_410T_MultiCard/delivery
.\build_ring_fbp_cuda.cmd
.\build_mingw_debug.cmd
```

CUDA 使用 MSVC14.44 + CUDA12.8，架构89。脚本可通过 `FBP_VCVARS`、`FBP_QT_ROOT`、
`FBP_CUDA_ROOT` 覆盖安装路径，生成 DLL 后生成 MinGW 导入库。
新增部署依赖 `cublas64_12.dll`、`cublasLt64_12.dll`，均来自本机 NVIDIA CUDA12.8 工具链，
与 cudart/cufft/pa_recon_core/ZeroMQ 一并记录 SHA256。运行包不需要 MATLAB。

测试工程为 `delivery/tests`，Qt6_DIR 指向同一 Qt kit，Python3_EXECUTABLE 指向可用 Python。
先构建主工程再构建测试；真实 controller 测试会复制该主工程的 ImagingSvc。
运行 `ctest --test-dir build/fbp_tests --output-on-failure -j4`；Windows 需 Qt/MinGW 在 PATH，
无屏幕测试使用 `QT_QPA_PLATFORM=offscreen`。

MATLAB 独立 CPU oracle：

```matlab
addpath('MC_410T_MultiCard/delivery/tests/reference');
generate_ring_fbp('artifacts/fbp-work/fixtures');
```

然后运行 `build/ring_recon_cuda/bin/ring_fbp_core_test.exe <fixtures目录>`。
它检查精确核、分数时间相移、均匀介质整图、分块等价、清空、独立费马最小化、
正入射曲面透射/扩散，以及分层查表与直接逐射线计算。服务测试覆盖4/8通道、双向、
拼接、分层的16组合及卡时延前缀/跨圈状态；UI 测试覆盖启用、应用、撤销和保存默认。
新增真实 controller 测试覆盖取消未完成启动、重启、服务预热确认、三块快照及源端完成。
这些验证面向移植一致性和工程行为，不重新评判用户已验收的线性成像效果。

## 实机使用

1. 完整解压交付包，保留所有 DLL、plugins 和 diagnostic-tools。运行 `bin/PAimageReceiverDiagnostics.exe`。
2. 选择环形扫描，在“成像参数→重建参数→重建算法”选择“改进 FBP（精确核 / 同心圆折射）”。
3. 确认通道、半径、圆层边界、内到外声速/密度、每通道双波长延时和采集长度；需要时选择当天环形 JSON。
4. 应用后等待状态显示“FBP 走时表和精确核准备完成”，再开始实机采集；重新调整 RF 显示色阶。
5. 保留本包 manifest、现场配置和诊断日志，检查完整圈、缺块、跨圈清空及实际吞吐。

开发机 RTX4060 Laptop 8GB 上，8通道、250MHz、4000采样、每通道每块200条、36mm FOV 的
合成服务测试：0.01mm 网格均匀介质稳定约0.56秒/块；边界8mm、1490/1560m/s 分层约1.49秒/块；
0.02mm 分层约0.49秒/块。配置预热约0.24/4.05/2.14秒（分别对应上述三种情况）。
这些是同步提交的计算耗时，不证明实际触发速率下无丢块；0.01mm 分层配置是否够快需对照现场块间隔。
包含时变低通、非零分数校正、不同半径及更深采样时耗时还会变化。最终包附对应复测日志。

实机图像、环形实测标定及现场完整吞吐尚未验收。交付是用于这一步验证的本地候选包。
