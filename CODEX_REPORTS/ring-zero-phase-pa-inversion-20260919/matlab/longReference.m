function [xIn, yFull, ppFull, info] = longReference(runName, tauEcho, sysD, Ntv, dbrEnd, ...
    padL, padR, sosHP, gHP, nHP, sosLP, gLP, nLP, fs)
% LONGREFERENCE 长窗边界参考（二轮整改 B1，任务 §7.1）。
%
% 在同一物理声学信号 s(τ) 的扩展区间上**先**完成零相位滤波与导数，**再**按物理
% 时间提取与被测短窗相同的区间 τ∈[0, Ntv−sysD]。独立性只来自"真实保留的不同
% 边界"：长输入比短窗滤波输入长 padL+padR，额外段（τ<0 的触发前模型段与 τ>
% Ntv−sysD 的尾段）在滤波时真实存在。这与旧"同时平移 sysDelay 后先裁剪"的构造
% 有本质区别——旧构造在滤波前就把额外段裁掉，两路送入滤波器的数组逐位相同
% （9ace9e1 B6 缺陷，独立复现 B6_filter_input_identical=1, maxdiff=0）。
%
% 输入
%   runName     : 'echoOnly' | 'burstOnly' | 'burstEcho'（ringAcousticModel）
%   tauEcho     : 回波中心（声学 τ 样本）
%   sysD        : 系统延时 D（raw 一基声学零点）。长窗与短窗共用同一物理原点
%                 m=sysD（τ=0），不因加 pad 重新归零。
%   Ntv         : 短窗 raw 长度（被测区间 = raw(sysD:end)，τ∈[0, Ntv−sysD]）
%   dbrEnd      : DBR 置零末端；长窗与短窗应用同一 raw 坐标规则（m∈[1,dbrEnd] 置零）
%   padL, padR  : 长窗相对短窗的前/后扩展样本数（声学 τ 延伸至 −padL 与
%                 Ntv−sysD+padR）
%   sos*/g*/n*  : HP、LP 零相位滤波器系数（与 exp5/refZeroPhase 同一实现）
%   fs          : 采样率 [Hz]
% 输出
%   xIn    : 长窗整条滤波输入（结构断言用：长度 = 短窗滤波输入 + padL + padR，
%            且包含短窗输入为连续子段——同物理样本逐位一致）
%   yFull  : 整条长输入的 HP+LP 零相位滤波输出
%   ppFull : yFull 的时间导数（timeDerivative，长线上计算后由调用方按提取索引截取）
%   info   : .tauStart/.tauEnd   长输入声学 τ 端点（样本）
%            .len                xIn 长度
%            .extractIdx         提取 τ∈[0, Ntv−sysD] 的 [起,末] 索引（yFull/ppFull 内）
%            .originSample       声学原点 raw 样本（= sysD，不随 pad 改变）
%
% τ<0 段采用"触发前无激励"模型（恒 0，见 ringAcousticModel.m）；实机是否能提供
% 触发前样本 UNVERIFIED——本参考仅用于隔离边界效应的合成证据，长参考收敛性由
% 递增 pad 序列验证（exp5 B6 / test_boundary_reference S3），不以"理想"自称。

mSpan = (sysD - padL) : (Ntv + padR);
tau = mSpan - sysD;
rawL = ringAcousticModel(runName, tauEcho, tau, fs);
rawL(mSpan >= 1 & mSpan <= dbrEnd) = 0;      % 与短窗同一 DBR 置零规则（raw 坐标）
xIn = rawL(:);                               % 统一列向量（与被测短窗同形状，可逐位比较）
yFull = refZeroPhase(xIn, sosHP, gHP, nHP);
yFull = refZeroPhase(yFull, sosLP, gLP, nLP);
ppFull = timeDerivative(yFull, 1 / fs);
i0 = padL + 1;                               % τ=0 ↔ m=sysD ↔ 长线第 padL+1 个样本
i1 = i0 + (Ntv - sysD);
info = struct('tauStart', tau(1), 'tauEnd', tau(end), 'len', numel(xIn), ...
    'extractIdx', [i0, i1], 'originSample', sysD);
end
