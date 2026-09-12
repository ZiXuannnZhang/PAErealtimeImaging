# 系统接收诊断增强操作卡

版本：2026-09-12  
适用：`PAimageReceiverDiagnostics.exe` 的 PAimage/WinSock 接收路线

## 一次短验证

先启动应用并开始监听。菜单选择“准备系统抓取”，确认状态栏显示“app_only（等待管理员抓取）”。应用会在 `system-capture-channel` 写入本次 `capture-request-<runId>.json`，请求中包含实际 `cardIPs`、8000–8004 端口、`runId`、`listenId`、`measurementSessionId`、token 和独立输出目录；应用不会自动提权。

在管理员 PowerShell 中执行：

```powershell
cd <应用目录>\diagnostic-tools
.\Open-AdminCapture.cmd -ApplicationRequestPath "<应用目录>\system-capture-channel\capture-request-<runId>.json"
```

如果当前目录就是应用的 `diagnostic-tools`，也可以只执行 `.\Open-AdminCapture.cmd`，脚本会从默认 `system-capture-channel` 选择最新请求。需要跳过 WPR 时显式增加 `-NoWpr`；结果必须显示 `pktmon_only_wpr_skipped`，不应被解释成含 WPR 的完整证据。

应用每 500 ms 读取输出目录的 `capture-state.json`，状态栏按证据状态显示：`app_only`、`running`、`captured`、`verified` 或 `failed`。burst 通知只是时间标记，不代表系统抓取成功。

## 结果文件

本次目录至少包含：

* `capture-state.json`：原子状态和生命周期；
* `capture-ready.json`：Pktmon/WPR（或 NoWpr）及应用握手确认；
* `pktmon.etl`、`pktmon.pcapng`、`pktmon.txt`、`pktmon-drop.txt`；
* `wpr.etl`（启用 WPR 时）；
* `validation-summary.json`、`clock-anchors.json`、`system-capture-manifest.json`。

`complete` 只表示流程已收尾；只有 `commandSuccess=true`、`artifactValid=true`、`targetPacketsPresent=true`、时间覆盖有效且 `analysisReady=true` 时，应用才显示 `verified`。`no_target_packets`、`partial`、`failed` 保留为独立结论。

手动收尾使用：

```powershell
.\stop_system_capture_admin.ps1 -StatePath "<本次输出目录>\capture-state.json"
```

该命令对已完成状态幂等；仅清理本次精确登记的过滤器，无法确认归属时不执行整体清理。

## 诊断分析与导出

对应用诊断 ZIP 运行：

```powershell
python .\startup_diagnostics_analyze.py `
  --application-zip .\diagnostic-export.zip `
  --system-capture "<本次输出目录>" `
  --rounds-json .\rounds.json `
  --output .\startup-analysis.json
```

应用诊断导出会把系统抓取目录复制到统一 ZIP 的 `paimage/<runId>/system-capture/`，并在 `paimage/manifest.json` 和对应 `run-config.json` 中保留文件清单及 SHA-256。导出不会无限等待正在进行的系统抓取；未完成状态会被标记为不完整。

## 当前验证边界

本工作站当前不是管理员令牌，因此没有执行真实 Pktmon/WPR 抓取；软件验证结论为通过，真实本机抓取为 `not_verified`。Realtek USB 10GbE 实机需由用户在管理员终端完成一次短流程，结论初始为 `awaiting_user_test`。本卡不包含 50 μs 真实采集、不修改驱动/网卡/电源/亲和性，也不承诺消除硬件或链路丢包。
