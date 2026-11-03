# 联合功能实机接受与 main 快进记录

用户确认日期：2026-11-04（Asia/Shanghai）。当前状态以根 PROJECT_STATUS.md 为准。

## 用户确认与授权

用户原话：

> 我已经用本地最新的交付包完成了实机测试，该分支上过去工作也一并通过了实机测试。
> 现在将本地进度推送到远端，并根据远端main中的一级目录下的项目级文档治理规则将本分支工作fast-forward为main，
> 同时更新main下的项目级文档，令文档内容为当前项目的最新状态。

这取代此前“仅本地更新交付包、暂不推送”的发布限制，接受范围是该分支已交付工作，
包括低通/FilteredDAS 与电机联动、文件选择、实机方向修正。本轮未附新的现场日志/截图，
不声称取得额外的测试参数、逐规则组合测量、Layer 2b T_arr 或 FPGA/LabVIEW 根因证据。

## 身份与集成方式

- 读取治理文档时远端 main：a8c15cff203da959bd935120605e69dc32ff7145。
- 原低通 tip：3340fbff5ed93ff2ba87b496275e663e877fa693，ref 不变。
- 已验收源码/包 BuildIdentity：1fb9478e71c95815671c5e1a2c1b3d6a302746d1。
- 生产/测试子树：f178261cbf95cfdf937af01ee705f26f7c034f5a。
- 原 main 是 accepted source 祖先，文档前差距 0 behind / 10 ahead。
- 在已验收分支上追加文档，普通 fast-forward 推送功能分支与远端 main 到同一后代。
  不改生产/测试源码，不产生 merge commit，不 force/rebase，不移动原低通 ref 或删除历史分支。
- 本地旧 main d3054d51e19570a6ef38774b8a82326738dce53b 有一份独有文档/开发配置提交，
  保留不改；当前联合分支在发布后对应 origin/main。该独有提交未进入 main。

## 包与已有验证

验收包：artifacts/build-delivery/20261029-213219_1fb9478e_lowpass_motor_local_direction/。
本轮逐项核对包内 file-sha256、BuildIdentity、exact-SHA 构建与验证 JSON，结果见 verification.json。
已有验证为标准 configure/build、56/56 CTest、真实 MainWindow 1/1、合成服务 selftest PASS，
对应 exact 1fb9478、tracked-clean；这些是此前执行记录，本次文档更新未重新运行或改标测试身份。

包内 local_only、旧远端 SHA 和 UNVERIFIED 文案保留当时真实状态，包及冻结回执不改写。
本次用户确认补充后续实机接受与发布授权。文档提交不产生新包；将来交付新的 main SHA
仍须按 BUILD_STANDARD.md 重新 configure/build，不得将已验收二进制重标为文档 SHA。
方向约定：逆/CCW → raw sectorCcw=0、180°；顺/CW → raw sectorCcw=1、0°。
桥接/INI 保持 DirectionInverted=false；起点和轮内参数冻结未改。
