# FilteredDAS 独立参考

任务来源：`2dbce5c1fa407fb819f8917e6eea59b3e9a89bb2:TASKS/FilteredDAS滤波反投影实时移植_20261019-184012.md`
附录 A。`pa2d_bandlimited_prefilter.m` 数值语句保持原样，与外部原 helper 字节 hash 均为
`00976ad7030df64437bd1908ab4c59f0b29c3f3cd250b9ec2a8c8e59d6177404`。
原入口脚本 hash 为 `a05a512191993c1e523fbade42e225eca0463478f1152d96abdf3929d156d390`。
两个原始文件只读，不纳入功能提交或覆盖。

独立 oracle 使用 MATLAB 9.14.0.2206163 (R2023a)、Signal Processing Toolbox 9.2，
Twister seed=20261019。调用显式传 Enabled=true、Order=.5、ReferenceHz=5e6、PassHz=40e6、
StopHz=60e6、PadFactor=4、BatchColumns=64；其他组显式覆盖参数，不能依赖 helper 的 40/50 MHz fallback。

复现：MATLAB 路径加入本目录，执行 `export_filtered_das_fixtures(<this-directory>/filtered_das_fixtures)`，
再用 Python 执行 `pack_filtered_fixtures.py`。版本、完整 opts、nt/nfft/left/类型/列数见 manifest.json。
72 组、1008 条列输入包含两精度、奇偶/非 2 幂及实际 N5000/7500 裁切后长度；
零/DC、固定噪声、首/中/尾脉冲、2/5/20/50/70 MHz 和多频、重复/交错列；
Order=0/.5/1、PadFactor=4/8/4.25、两组带宽。C++ 对每组另跑 BatchColumns=1/7/64/100。

`service_*` 是独立生产链 oracle：同一 full float raw → 可选 MATLAB 时变低通（45/1 MHz）
→ DBR → float 限幅值 → 1-based DelayCut → single FilteredDAS。8 通道/双波长独立长度，
用于生产服务四组合、插入点及现有 CUDA DAS 对照。

原始 `.bin` 为 little-endian、MATLAB column-major；H 的两列是 real/imag，F 为未 shift 频轴。
提交的 `.bin.qz` 使用 Qt qCompress framing：4 字节 big-endian 原始长度，加 zlib 无损压缩，
没有数值转换。`hashes.json` 同时记录原始与压缩字节 hash，测试用 qUncompress 读取。

`capture_filtered_baseline.py` 从 exact eed98f0 提取原 CPU 源码独立编译，
生成时变低通 OFF/ON × DBR/限幅/DelayCut × 长度/延时共 384 个 OFF 字节指纹。
这用于 OFF 回归，不充当新滤波的独立 oracle。原 36 组时变低通和 128 个 OFF 参考保持。
