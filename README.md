# PAErealtimeImaging

PAErealtimeImaging 是一个 Windows 实时光声成像工作区，覆盖多卡采集、UDP 数据传输、环形扫描重建、显示和半径标定。当前主线仓库保留项目入口说明；完整源码和本次诊断日志实现位于 [`codex/diagnostic-log-export-20260907`](https://github.com/ZiXuannnZhang/PAErealtimeImaging/tree/codex/diagnostic-log-export-20260907)。

## 主要目录

- `MC_410T_MultiCard/`：MC410T 多卡监听程序及交付工程。`delivery/` 包含 Qt/C++ 主程序、数据接收、环形重建、测试和设计文档。
- `PALiveImagingSimSender/`：本机 UDP 联调模拟器。正式监听程序使用显式 `--target-ips` 目标列表；一个 IP 对应一张卡，不再自动发现或复制虚拟卡。
- `实时重建脚本/`：MATLAB 实时重建参考脚本。
- `RadiusCalibration/`：环形阵列半径和通道起始角标定脚本。
- `MC_410T_MultiCard/delivery/docs/`：构建、链路、验收及诊断日志导出说明。

## 诊断日志导出

监听程序运行日志区域提供“导出诊断日志…”入口，可选择当前或历史运行并生成 ZIP。导出包包括 `summary.txt`、`runtime.log`、`events.jsonl`、`network.json`、`settings.json` 和 `manifest.json`，用于定位目标 IP 入选、就绪包、配置发送、反馈、重试和“卡五失败”等问题。导出不会重新扫描网络或发送控制命令。

## 构建与验证

主要工具链是 Qt 6.8、MinGW 和 CMake；部分成像能力依赖 ZeroMQ、CUDA 运行库及部署环境提供的 DLL。可从 `MC_410T_MultiCard/delivery/CMakeLists.txt` 和对应文档开始。诊断功能的独立测试位于 `MC_410T_MultiCard/delivery/tests/`，覆盖日志导出、历史恢复、网络快照、四卡配置确认、第五目标超时和导出期间持续收包。

大型本地采集数据 `testdata/` 与 `cufft64_12.dll` 不纳入 GitHub 源码仓库；它们需要在本机或采集服务器部署环境中单独提供。
