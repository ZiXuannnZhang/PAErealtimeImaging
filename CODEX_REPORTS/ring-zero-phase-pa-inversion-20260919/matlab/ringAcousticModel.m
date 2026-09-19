function s = ringAcousticModel(runName, tauEcho, tau, fs)
% RINGACOUSTICMODEL 环形实验合成声学信号 s(τ)（τ 为声学样本坐标，0=触发/声学零点）。
%
% 【二轮整改抽出共享】exp5_order_endpoints.m 原局部 makeRaw 的信号模型原样抽出，
% 供 exp5 与 test_boundary_reference / longReference 共用，保证"同一物理声学
% 信号"在短窗被测线与长窗参考线之间逐位一致（B1 独立参考的前提）。
%
% 输入
%   runName : 'echoOnly'（仅回波）| 'burstOnly'（仅启动 burst）| 'burstEcho'（两者）
%   tauEcho : 回波中心（声学 τ 样本）
%   tau     : 声学 τ 坐标（任意形状；允许 <0 与非整数）
%   fs      : 采样率 [Hz]
% 输出
%   s       : 与 tau 同尺寸的声学信号取值
%
% 模型（与 9ace9e1 版逐值一致）：
%   burst：2 MHz 正弦、幅度 2000、指数衰减 1.5 µs，自 τ=0 起；
%   回波：高斯包络（σ = 0.25 µs·fs）5 MHz 正弦、幅度 0.5，中心 τ = tauEcho；
%   τ<0 段 = "触发前无激励"模型，恒置 0（合成约定；实机是否提供触发前样本
%   UNVERIFIED——长窗参考用它作为额外声学历史，不宣称实机可得）。
m = tau(:);
s = zeros(size(m));
if ~strcmp(runName, 'echoOnly')
    tb = max(m, 0);
    s = s + 2000 * exp(-tb * (1 / fs) / 1.5e-6) .* sin(2 * pi * 2e6 * tb / fs);
end
if ~strcmp(runName, 'burstOnly')
    s = s + 0.5 * exp(-(m - tauEcho).^2 / (2 * (0.25e-6 * fs)^2)) .* ...
        sin(2 * pi * 5e6 * (m - tauEcho) / fs);
end
s(m < 0) = 0;
s = reshape(s, size(tau));
end
