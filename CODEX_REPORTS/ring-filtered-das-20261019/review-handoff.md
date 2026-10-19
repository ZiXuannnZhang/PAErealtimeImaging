# 分析代理独立审核交接

请以执行回执返回的 exact final SHA 审核原实现分支 `codex/ring-timevar-lowpass-20261016`。
任务规格固定为 `2dbce5c1fa407fb819f8917e6eea59b3e9a89bb2` 的
`TASKS/FilteredDAS滤波反投影实时移植_20261019-184012.md`，起点为 `eed98f0aea78ea4180e9284ee51a5e71fdf11988`。
main 未合并；当前任务包含旧增强移除/时变低通、启动拒绝修复及新 FilteredDAS 的联合候选。

重点核查：

1. 新核是指定 pressure prefilter 后接现有 DAS，默认 .5/5/40/60/4/64，负相位、DC/Nyquist、对称补零与同位置裁回。
2. 服务按实际通道/波长 nt 接入既有预处理后、WL2 前；原 flatten/append、几何、ABI、raw/前端/保存路径保持。
3. 独立开关、真实 BatchColumns 分批、缓存/资源/有限性（响应和输出窄化前范围检查）、关闭无 FFT/转换，以及末组失败的 CUDA/历史原子边界。
4. 主四参数/专属高级两参数、完整 INI/JSON 快照、草稿/保存/重启链、坏值处理及真实 MainWindow inactive/resident 拒绝。
5. 72 组/1008 列 MATLAB single/double oracle、原时变低通 36/327、原 128 和新增 384 OFF 指纹、服务四组合、40 份 OFF 图像 byte hash。
6. 原 51 项全部保留，新增 4 项；55+1 实际执行记录、性能/内存全样本、最终 BuildIdentity、tracked-clean、runtime 来源/hash 和 local==remote。

另外单独审查 `b1b61a065c9e70aa016733c2409a3cbd7bea3e20`：原轮次回归在 MinGW 13.1 下间歇崩溃，
仅把 newest-first cache lookup 的 reverse_iterator 改为 end()/--it。保留故障/GDB 记录、诊断 30 次、正式 100 次与全回归证据；
没有修改分类、计数或边界条件，不宣称已经证明标准库根因。

源码、MATLAB 参考、完整参数/压缩前后 hash、执行工具和开发证据在本提交。
最终提交后的 exact-SHA build/validate/replay、五项依赖 hash、完整 bin/Qt/plugins/diagnostic-tools 与 Git 远端回执在联合包；
需要二进制验真时核对用户提供的包及其中 manifest。候选性能测量服务 byte hash 与最终包一致，旧基线服务单独记录。
测量工具两版源码/二进制均归档，唯一差异为可选帧等待参数（默认仍 20 秒）；两次 20 秒超时原始尝试保留，
后续显式等待 120 秒以捕获长尾。请核对逐单元命令及工具 hash，不把超时尝试计为通过。

请分别给出软件审核结论与尚未验证的边界。实机视觉、真实采集链与真实 T_arr 由用户后续联合验收，当前均为 UNVERIFIED。
不自动合并 main，不扩大历史硬件接受范围。
