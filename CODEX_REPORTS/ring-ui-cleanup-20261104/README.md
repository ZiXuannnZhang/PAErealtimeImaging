# 环形实时成像参数与旧滤波链路整理

本次在本地 `codex/ring-exact-fbp-20261104` 分支继续开发，整理前提交为
`fe9b87cf8799596141293d4a205b98e024b6c890`。依据用户确认的清理范围执行，不推送远端。
最终源码身份、构建和验证回执见新交付包的 `build-manifest.txt`、`validation.txt`；
此前移植包及 main 的实机验收记录保持原身份。

## 修改与边界

| 项目 | 本次结果 |
| --- | --- |
| Apodization | 删除控件、INI 读写、协议字段及 CUDA Hann/Hamming 窗计算；DAS 各 A-line 不再加该窗 |
| 距离权重指数 | 删除控件、配置及 CPU/CUDA 的距离幂次因子；DAS 保留几何方向投影、角度积分元和绝对权重归一化 |
| MinDistance | 删除控件和用户/网格派生的距离下限截断；保留零距离数值保护 |
| 旧“滤波反投影” | 删除 FilteredDAS 开关、Order/Reference/Pass/Stop、高级 PadFactor/BatchColumns、预滤波实现、缓存/遥测、专用测试和专用参考数据 |
| 改进 FBP | 保留精确核、同心圆分层走时、透射/扩散/几何权重、实际 A-line 角度；本次未改其算法实现 |
| 标定与亚采样 | 保留环形标定 JSON 导入、8×2 固定亚采样延时、角度偏移、电干扰模板、每圈卡间时延估计/校正；保留 MATLAB 参考与标定导出脚本 |
| 其他链路 | 保留前端 IIR、服务端 STFT、DBR、限幅、DelayCut、整数通道/双波长延时、FBP 中值/DC、显示掩膜、越界置零、扇区羽化、物理圈/共享内存/电机联动 |

旧 FilteredDAS 是“频域分数阶预滤波后接 DAS”，并非新 FBP 的依赖。新 FBP 已提供其替代入口，
现在算法选项为 DAS 和改进 FBP。删除的是旧实现，未删除新 FBP 所需的物理权重或时间插值。
DAS 输出会随距离因子的移除发生预期变化，不能要求与旧 DAS 逐点相等；FBP 应保持原数值结果。

“保留树叶标定”指保留现有参考算法、导出工具及实时端标定消费链路；本次未增加实时程序内的
自动树叶采集/拟合功能，也未把线性几何标定文件直接用作环形几何。

## 配置与接口

旧 INI 中 `apod`、`distWeight`、`minDistMm`、`filteredDas*` 启动时不再读取，
不会影响算法选择，也不会因为旧值格式异常阻塞启动。用户成功执行“设为默认”时删除这些旧键。
其他默认参数不自动重置；加载时不改写旧文件。旧协议字段不再发送或消费。

`RingReconCudaConfig` 的三个退休字段改为同类型、同顺序的 reserved 占位，避免改变 C ABI 布局；
占位值不参与计算。必须配套使用本提交构建的 DLL、导入库、ImagingSvc 和主程序，不能混入旧 DLL。
通用采样窗口校验迁到 `RingSampleWindow.h`，在 UI/controller/service 接受配置前检查，
要求启用通道的 DelayCut 后至少保留两个样本；改进 FBP 的有效 Nt=128..8192 约束继续保留。
应用草稿前补齐 UI 层的 FBP 校验：无效密度/标定文件等只拒绝本次编辑，不再进入 controller 的
`svcError` 路径而误改驻留服务的 ready/UI 状态。真实 MainWindow 用例覆盖无服务和驻留服务两种情况。

## 验证与复现

1. 执行 `delivery/build_ring_fbp_cuda.cmd`，再从 `delivery` 执行 `build_mingw_debug.cmd`。
2. `delivery/tests` 按现有 Qt6/MinGW 配置构建并运行 CTest；删除的4个旧算法专测不再注册。
3. 主工程启用 `BUILD_MAINWINDOW_START_TEST=ON`，运行 `mainwindow_start_integration_test`。
   原有配置原子性、真实服务启动、驻留进程、STFT 四组合、电机换向重启覆盖改用 FBP。
4. CUDA 构建目录运行 `ring_das_geometry_test`：独立双精度几何参考，包含探测器零距离、
   小于网格步长的距离和 reserved 值不影响输出；参考数据能明确区分旧距离加权结果。
5. 同目录运行 `ring_fbp_core_test <fixtures>`，复用移植阶段独立 MATLAB 参考，检查核、
   亚采样相移、均匀介质图像、分块一致性和同心圆查表对直接射线参考。

服务测试保留16种通道数/方向/拼接/介质组合；分层组合增加从文件导入非零角度和8×2小数延时，
验证逐块服务输出与整批参考一致。电干扰模板也经过文件导入，验证跨块前缀、已知卡间延时、
原始数据不变、源端完成及每圈状态清零。UI 检查旧控件消失、旧 INI 忽略及显式保存清理。

交付遵循根目录 `BUILD_STANDARD.md`：最终提交 tracked-clean 状态重新 configure/build，
完整 bin、Qt 插件、diagnostic-tools、CUDA/ZeroMQ 依赖、日志及 SHA256 一并打包。
软件回归不替代实机图像验收；本次不改写之前的已验收结论。
