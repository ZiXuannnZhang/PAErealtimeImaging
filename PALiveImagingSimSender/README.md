# PALiveImagingSimSender — UDP loopback simulator

这是一个**辅助/历史兼容的同机 UDP 模拟发送器**。它可用于协议、组包、显示和部分 ImagingSvc
软件回归，但不能替代真实 FPGA/NIC/LabVIEW 验收。

当前正式监听程序：

```text
MC_410T_MultiCard/delivery/build/mingw_debug/bin/PAimageReceiverDiagnostics.exe
```

## 推荐用途

- 显式指定 loopback target IP 验证多卡软件路径；
- 回放 11.dat / 14.dat 等 retained 数据；
- 验证 packet/trigger layout、bits-per-channel、基本 save/display/ring 软件路径；
- 快速复现不依赖物理硬件的 UI/协议问题。

不应用于证明：

- FPGA 收到 START 后的真实首包时序；
- NIC/driver 是否丢包；
- 现场额外 startup trigger 的来源；
- 当前 hardware round 的真实 trigger 数。

## Loopback target

主程序支持 `--target-ips` 显式 IPv4 列表。例如四个独立 loopback source：

```powershell
PAimageReceiverDiagnostics.exe --target-ips 127.0.0.1,127.0.0.2,127.0.0.3,127.0.0.4
```

单个地址只代表一个 target，不自动扩展成四卡。

## 数据与采样率

当前真实采集模型按 250 MSa/s（4 ns）计算。模拟器可以从历史数据源采样率做重采样，
因此 UI 中“源数据采样率”和“监听程序/目标采样率”必须按实际测试目的填写，不能把旧
200 MSa/s 默认值当成 production hardware 配置。

回放的源文件为已解调时域数据；发送端生成真实 UDP payload layout，接收端仍走软件采集链。

## Virtual-card behavior

模拟器可发 ready/config feedback 以便软件控制路径联调。ready packet、CONFIG ACK 和实际数据
是不同证据；loopback 成功不应被写成 physical-card acceptance。

## Ring test notes

历史 14.dat 可用于 360° ring 软件回放；启用通道数会影响扇区聚合。具体 current round/filter/
timeout 语义以 canonical main 和 `MC_410T_MultiCard/delivery/README.md` 为准，不以模拟器旧
“5 blocks/4000 triggers”假设定义 production round。

## Build

依赖 Qt 6.8 MinGW + CMake/Ninja。使用本目录 `build.cmd`；生成物在本工具自己的 build/bin。

历史同机操作卡和旧候选报告已经归档到 `CODEX_REPORTS/`。
