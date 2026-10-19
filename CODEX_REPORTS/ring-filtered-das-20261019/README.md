# FilteredDAS 与时变低通联合候选执行回执

实现分支为 `codex/ring-timevar-lowpass-20261016`，继续 exact 起点
`eed98f0aea78ea4180e9284ee51a5e71fdf11988`。未新建实现分支，未 merge/rebase main/task-docs，
未 force push，未改动原始脚本、历史报告、frozen migration provenance 或用户三个未跟踪文件。
最终 SHA、tracked-clean、local/remote 相等证明和包路径以提交后生成的
`artifacts/filtered-das/final-package.json` 及联合包 `git-receipt.txt` 为准。

任务为 `2dbce5c1fa407fb819f8917e6eea59b3e9a89bb2:TASKS/FilteredDAS滤波反投影实时移植_20261019-184012.md`。
执行前 fetch，读取最新 origin/main 的 PROJECT_STATUS、REPOSITORY_BASELINE、BUILD_STANDARD、HANDOFF、
双端指南、原时变低通任务和原始两轮回执；main 仍为 `a8c15cff203da959bd935120605e69dc32ff7145`。
实现分支本地和远端均精确等于 eed98f0，没有需要接续盘点的后续提交。

## 实现与边界

只移植入口脚本的 `pa2d_bandlimited_prefilter + 现有 DAS`，不实现其他模式。
新核为独立 CPU radix-2 FFT，没有抽取或修改 RingTimevarLowpass 核；原 1 MHz fcMin 及启动拒绝修复保留。
默认 true / .5 / 5 MHz / 40 MHz / 60 MHz / 4 / 64 与任务入口一致，不使用 helper 的 40/50 MHz fallback。

顺序为完整 A-line double → 可选原时变低通 → 原 DBR/限幅/DelayCut → 可选 FilteredDAS
→ 原 WL2 对齐/历史 → 原 CUDA DAS。每物理通道/波长使用自己的实际 nt；默认 N5000/7500
与延时 358/371 对应 4643/4630、7143/7130；当前现场 448/464 对应 N7500 的 7053/7037。
不增加 ReconDepth 截断，不补齐通道到较长 nt，不用展平后的平均长度设计新频轴。
原服务 flatten/append 的 `flat.size()/nd`、列顺序、角度/半径和 CUDA 调用契约原样保留。

保留负相位、余弦带限窗、DC/Nyquist 置零、对称补零及同位置裁回。single 的 H 在乘谱前 cast 为 float；
内部 double FFT 的 float 输出通过独立 MATLAB single 阈值。Order=0 仍执行带限滤波。
BatchColumns 控制真正的多列工作区、批次和最大批列数，工作信号每批重写；不同批大小输出逐位一致。
关闭时不扫描/转换信号、不分配 FFT/响应/工作区、不做 FFT，原预处理/重建输出逐位一致。

服务先准备全部实际长度和两项增强，再整体替换已接受状态。各组在局部列表完成处理后才进入 CUDA；
最后一组失败也不提交部分 CUDA、快照或 WL2 历史，不回退未滤波数据。WL2 历史保存已完成两种处理的线，
跨块复用不二次滤波。raw/shared input、DisplayBuffer、FileSaver、publisher、CUDA/SHM ABI 和几何不变。

新增独立配置/设置 helper、主复选框和四参数、专属高级两参数，完整接入草稿/取消/应用/确定、默认保存/恢复、
初次加载和服务重启。JSON 为 ring.filteredDas；INI 为 RingConfigDialog/Defaults 下 filteredDas 前缀。
频率只在核入口由 MHz 换 Hz，显示精度不改写保存的 double。缺字段按出厂值，非法已有字段整体拒绝；
坏 INI 提示并回退完整 disabled 安全快照，不覆盖坏记录，不影响时变低通设置。
Stop 超过当前有效声速/网格空间 Nyquist 时提示；超过时间 Nyquist 则拒绝启动。

每配置上限：nfft 262144、批工作区 256 MiB、全部长度计划 128 MiB；检查乘法/资源边界，超限拒绝。
原子准备时旧/新配置可并存，滤波核动态存储最坏 768 MiB；不含容器/allocator 固定开销、时变低通、帧与 CUDA 内存。
生产默认两种长度的缓存/工作区和真实进程峰值见性能回执；无隐式降低补零、批数或丢线。

## 独立数值与测试

原入口脚本与外部 helper SHA256 均与任务匹配，详见 evidence/reference-hashes.json。
冻结 helper 直接提取附录 A，LF 字节 hash 与原 helper 相同；原始来源只读。
MATLAB 9.14.0.2206163 (R2023a)、Signal Processing Toolbox 9.2、Twister seed=20261019、Fs=250 MHz。
所有业务默认显式传入；每组完整 opts、类型/形状、nfft/left 及原始/无损压缩字节 hash 在 tests/reference/filtered_das_fixtures。
MATLAB 不可在初始沙箱内创建预设目录，保留失败记录，改用项目内隔离 MATLAB_PREFDIR/TEMP 原生导出。
第一次导出中的旧 STFT helper 调用签名不匹配已修正，完整重新导出；不将失败导出计为 oracle 完成。

- 72 组、1008 列、single/double，实际裁切 nt、奇偶/非 2 幂、零/DC/固定噪声、首中尾脉冲、带内/参考/过渡/带外正弦及多频。
- Order=0/.5/1、PadFactor=4/8/4.25、两组合法带宽；每组再跑 BatchColumns=1/7/64/100。
  全样本包含首尾；H/F/nfft/left、共轭、负相位、裁回位置、输出有限性、重复列/交错列、缓存重配和资源拒绝均检查。
- 已执行数值最坏归一化 maxAbs：double `4.86291e-16`、single `2.15395e-7`；整体最坏归一化 L2 `1.45794e-7`。
  double L2 为 `3.4338e-16`，single L2 为 `1.45794e-7`；分别满足 1e-8 / 1e-5，最终 exact-SHA 重跑值见 final validate 日志。
- 生产服务四组合 × 4/8 通道通过：完整 float raw → MATLAB 时变低通/DBR/float 限幅/DelayCut → MATLAB single FilteredDAS
  → 现有 CUDA DAS。逐通道/波长线和重建输出对照，最坏归一化 maxAbs `2.08616e-7`。
- 新 OFF 参考从 exact eed98f0 原 CPU 源码独立编译，384 个预处理字节指纹通过，含当前 448/464 延时。
  原 MATLAB 时变低通 36 组/327 列、128 个旧 OFF fingerprints 保留并通过。
- 真实服务 OFF 回放分别在时变低通 OFF/ON 下与保留的 eed98f0 服务比较，双波长共 40 份图像 raw SHA256 相同。
  首次沙箱旧服务首块超时保留；原生重跑通过。最终 exact-SHA 回放还检查 ON 的 identity barrier、缺块完成语义及 timeout。
- UI/INI 覆盖两个独立高级窗口、父/子取消、应用、完整默认、恢复/重启、禁用联动、坏记录不覆盖与浮点精度。
  真实 MainWindow 入口扩展新参数非法的 inactive/resident 启动拒绝和四组合启动/停止，保留原启动修复全部断言。
- 原 51 项全部保留，新增核/服务/UI/OFF 四项，共 55 项；开发阶段 55/55 通过，真实入口 1/1 通过。
  源码/测试 seam 宏仅在测试目标启用，生产无观察 hook 或启动计数。

最终补充的极端 Reference/输出幅值测试发现转换后检查不足：超范围浮点窄化不能可靠地用于检测溢出。
响应和输出现在都先验证目标类型可表示范围，再转换；服务启动拒绝超大响应，运行期拒绝超范围输出。
该失败记录保留在 evidence/prefinal-*；修正后完整 55/55 通过，见 evidence/finite-guard-tests-*。

## 回归中追加的最小轮次缓存修复

原 physical_round_normalizer_test 在旧缓存及全新独立 tests 目录均间歇 SegFault，原生环境也复现。
GDB 记录的故障在原 4000-trigger 契约测试、既有 deque 反向查找，reverse_iterator 当前指针落在节点区间之外。
不将其归因于新滤波或宣称已证明标准库根因；失败日志和原状态调试证据全部保留。

独立提交 `b1b61a0` 只把该遍历改为 end()/--it，继续 newest-first，不变更分类、缓存容量、计数、轮次或 timeout 条件。
诊断版本连续 30 次通过，正式目标连续 100 次通过，随后完整 55 项回归通过。
这是本次为完成必要回归追加的源码范围，交由分析代理单独检查；MainWindow 的原启动拒绝生产代码未修改。

## 性能、构建和交付复现

性能原始命令、每块记录、cold/warm 分布、A-lines/s、准备时间、计划/工作区和 Windows 进程峰值在
`artifacts/filtered-das/performance/`；汇总见本目录 performance.md。正式矩阵为 36 单元，每单元 1 cold + 30 warm 完整块。
按修正前波动观测预先确定四个单元另作独立复测，不根据本轮结果重新选样；首次数据和复测数据都保留。
系统级根因不作归因，跨单元差值不视作纯算法增量。
包括 eed98f0 8 个负载基线 + 2 个当前几何基线；候选四组合 16 个负载 + 4 个当前几何、4 个高负载和 2 个额外批大小。
当前实际几何保持 N7500、2 通道、block=10、8000 A-lines/圈、36 mm FOV、0.01 mm 网格、6.48 mm 半径、
448/464 延时、拼接/5°羽化、7.5 mm 双声速边界、1490/1540 m/s、mask=375。
benchmark-blocks 只在完整块边界正常停止采样，不修改配置圈长、RoundIdentity、source-final 或 reconstruction-complete。
初次未限制采样圈长的性能尝试因改用统一 31 块采样而中断，保留在 performance-before-limit，不计入正式矩阵。
审阅补正单声速空间提示和输入长度转换边界后的候选测量统一重跑；旧候选记录在 performance-candidate-pre-advisory，不混入正式矩阵。
有限范围修正前的完整矩阵另存 performance-before-finite-guard；正式候选 26 单元和 4 个复测统一使用修正后的二进制重测，旧基线 10 单元不变。
本轮 N7500/C8 两项开启的一次尝试在第 12 块等待超时，11 块已回报；GDB 读取服务计数为 consumed=12、
lastProcessUs=22003342，已回到事件循环，超过工具原有 20 秒等待。全部原始记录、失败退出码和现场读取保留在
evidence/performance/failed-attempts/attempt-01，不计为完成单元。同参数第二次尝试也超时，仅收到首块 13.373 秒；
原始记录在 attempt-02。记录器对无 warm 样本求 median 的异常已加保护，未补造丢失的峰值数据。
为完整捕获长尾，测量工具新增 --frame-timeout-ms，默认仍为 20000；后续主矩阵与四个复测显式用 120000。
这只改变模拟发送器等待，不改变生产服务、计时算式或生产 timeout；不是把原 20 秒超时改判为通过。
两版测量工具源码/二进制及唯一差异随 evidence/performance/tool-versions 保留；所有候选使用同一 DSP 服务 byte hash。
已完成单元保留原工具结果，失败单元从完整 31 块重新测量，全部原始尝试均保留。
最终 timeout 合成回放保留 2 秒阈值及 2.5 秒停顿，使用每通道 20 触发的完整块；圈长仍为 8000。
这使同步模拟发送器的正常 DSP 耗时与有意物理 idle 可分开，不改变生产 timeout owner 或伪造部分块/完成标记。

标准入口为 `delivery/build_mingw_debug.cmd`，mingw-debug / mingw-debug-build，Qt 6.8.0、MinGW 13.1.0、
CMake 3.30.5、Ninja 1.12.1，Debug / USE_ZEROMQ=ON；只对两个 DSP 核保留 -O2，无 fast-math。
tests 显式使用 `C:/Users/yyps/.cache/codex-runtimes/codex-primary-runtime/dependencies/python/python.exe`，
独立构建目录为 delivery/build/filtered_das_tests；主工程 BUILD_MAINWINDOW_START_TEST=ON。

最终提交后执行：

```powershell
python CODEX_REPORTS/ring-filtered-das-20261019/delivery.py build --stage final
python CODEX_REPORTS/ring-filtered-das-20261019/delivery.py validate --stage final
python CODEX_REPORTS/ring-filtered-das-20261019/delivery.py replay --stage final
git push origin codex/ring-timevar-lowpass-20261016
python CODEX_REPORTS/ring-filtered-das-20261019/delivery.py package --stage final
```

工具实际绝对命令、cwd、退出码、耗时与 exact SHA 均写入 artifacts/filtered-das/final-*.json/txt。
package 拒绝 tracked-dirty、BuildIdentity 不符、local/remote 不同、未通过测试或 runtime hash 不符。
完整 native bin 为 delivery/build/mingw_debug/bin；联合包为 artifacts/build-delivery/<timestamp>_<final-short-sha>/，
包含 Qt/plugins、diagnostic-tools、manifest、Git/依赖/文件 hash、BuildIdentity、validation、build.log 及联合操作卡。
打包排除 native bin 中的本机 INI、app_log 和 paimage-diagnostics 会话，原文件保留；包首次启动使用代码出厂默认。

Ring CUDA 仍来自 `_migration_pack/prebuilt_cuda`，未重编核心/改变 ABI。cudart v12.8 与已验证 cuFFT 12.1.0.31、
pa_recon_core、ZeroMQ 原组合保留；五项依赖 byte hash 与原已验证包相同，来源和 hash 在联合包中。
两个开关联合操作见 hardware-operation-card.md。真实采集链、视觉效果与真实 T_arr 均为 UNVERIFIED，
不把合成 GPU 回放或更亮的图像写成实机接受，不自动合并 main。
