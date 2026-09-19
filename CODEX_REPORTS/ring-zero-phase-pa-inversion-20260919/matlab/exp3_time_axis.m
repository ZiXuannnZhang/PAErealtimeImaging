% EXP3_TIME_AXIS  判定性实验 3：时间轴/delayCut 索引公式与单采样脉冲测试。
%
% 冻结并验证的索引约定（与 starting SHA 生产代码逐行对应）
% ----------------------------------------------------------------
% 原始行：样本 m（1 基），m = 1..sampDepth。
% preprocessBlock（ring_recon.cpp:118-127）：
%   delayCut=1：输出行数 = Nt − sysDelay + 1；输出样本 k（1 基）= 原始样本 k+sysDelay−1。
%   delayCut=0：输出样本 k = 原始样本 k。
% CUDA/CPU 查询（ring_das_kernel / dasReconAppend）：
%   tf = d·fs/c（+分层修正）为"连续采样数"；1 基样本 k 位于 τ = k−1；
%   线性插值 v(τ) = col(k) + frac·(col(k+1)−col(k))，k = floor(τ)+1。
% 由此：裁剪线连续坐标 τ ⟷ 原始样本坐标 m(τ) = τ + sysDelay（delayCut=1）
%                              或 m(τ) = τ + 1（delayCut=0）。
% 声学时间约定：原始样本 m 的声学时间 t(m) = (m − sysDelay)/fs
%   （sysDelay 由经验校准对齐声学零点；每波长/每通道独立 sysDelay，
%     各自裁剪后 t = τ/fs 对本波长自洽）。
%   ⟹ delayCut=1：t(τ) = τ/fs；delayCut=0：t(τ) = (τ + 1 − sysDelay)/fs。
%
% 测试内容
%   T1 索引链：按上式生成 raw（含 sysDelay 偏移的脉冲），经
%      preprocessBlock 等价操作裁剪，验证脉冲峰位于 τ = d0·fs/c（±0.5 样本）。
%   T2 脉冲测试（排除一采样点偏差）：窄脉冲（σ=2 样本）置于已知 τ0，
%      验证 p 峰值插值位置、p' 零点位置均与 τ0 对齐（无 ±1 样本错位）；
%      并验证 b = 2p − 2t·p' 的过零结构以 τ0 为中心。
%   T3 delayCut=0 状态：t(τ) = (τ+1−sysDelay)/fs 公式与 raw 数据直接对齐。
%   T4 敏感性：人为把 sysDelay 偏移 ±1 样本，量化重构径向位置偏差，
%      证明该脉冲测试能区分一采样点错位（波前处 p' 反号 → b 反号）。
%
% 运行：matlab -batch "run('exp3_time_axis.m')"
% 输出：evidence/exp3_time_axis.json

outdir = fullfile(fileparts(mfilename('fullpath')), '..', 'evidence');
if ~exist(outdir, 'dir'), mkdir(outdir); end

fs = 250e6;  c = 1490.0;
sampDepth = 4000;
sysDelay = 358;                  % wl1 生产默认
maskLength = 300;                % DBR 置零行数

res = struct();

% ============ T1 索引链 + T2 脉冲测试（delayCut=1）============
d0 = 4.0e-3;                     % 目标距离
tau0 = d0 * fs / c;              % 期望脉冲中心（连续采样坐标）= 1677.85
sigmaSamp = 2.0;                 % 窄脉冲宽度（样本）

% 原始行：声学时间 t=0 对应原始样本 sysDelay（1 基）；t<0 的样本置 0（触发前）
mraw = (1:sampDepth)';
tRaw = (mraw - sysDelay) / fs;   % 声学时间
raw = zeros(sampDepth, 1);
raw = gaussPulse(tRaw, d0 / c, sigmaSamp / fs);

% preprocessBlock 等价（dbrRemove 无关本测试；仅 DelayCut）
cut = raw(sysDelay:end);         % 输出 k（1 基）= raw(k+sysDelay-1)
NtCut = numel(cut);
kcut = (1:NtCut)';
tauK = kcut - 1;                 % 1 基样本 k 位于 τ = k−1

% T1：脉冲峰位置
[~, ipk] = max(cut);
res.T1_peakTau = tauK(ipk);
res.T1_expectedTau = tau0;
res.T1_errSamples = tauK(ipk) - tau0;
fprintf('T1 裁剪线脉冲峰：τ=%.2f，期望 %.2f，误差 %.2f 样本\n', ...
    res.T1_peakTau, res.T1_expectedTau, res.T1_errSamples);

% T2：p 峰插值位置与 p' 零点位置
pp = gradient(double(cut), 1 / fs);          % dp/dt，端点单侧
tauFine = (tau0 - 6 : 0.01 : tau0 + 6)';
vFine = interp1(tauK, cut, tauFine, 'linear');
vpFine = interp1(tauK, pp, tauFine, 'linear');
[~, ipkf] = max(vFine);
tauPmax = tauFine(ipkf);
% p' 的过零点（线性插值）
zr = find(diff(sign(vpFine)) < 0 & vpFine(1:end-1) > 0);   % 由正到负的过零
if isempty(zr)
    tauZc = NaN;
else
    z = zr(1);
    tauZc = tauFine(z) + (0 - vpFine(z)) * (tauFine(z+1) - tauFine(z)) / (vpFine(z+1) - vpFine(z));
end
res.T2_tauPmax = tauPmax;
res.T2_tauPzero = tauZc;
res.T2_offsetPmax = tauPmax - tau0;
res.T2_offsetPzero = tauZc - tau0;
fprintf('T2 p 峰插值位置 τ=%.3f（偏差 %+.3f 样本）；p'' 过零 τ=%.3f（偏差 %+.3f 样本）\n', ...
    tauPmax, tauPmax - tau0, tauZc, tauZc - tau0);

% b 信号以 τ0 为中心的结构检查：b = 2p − 2t·p'，t=τ/fs
tFine = tauFine / fs;
bFine = 2 * vFine - 2 * tFine .* vpFine;
% b 的最近过零（b 以 −2t·p' 为主导，过零在回波中心附近）
zAll = find(diff(sign(bFine)) ~= 0);
tauBzc = NaN;
if ~isempty(zAll)
    best = NaN; bestD = inf;
    for z = zAll(:)'
        tz = tauFine(z) + (0 - bFine(z)) * (tauFine(z+1) - tauFine(z)) / (bFine(z+1) - bFine(z));
        if abs(tz - tau0) < bestD
            bestD = abs(tz - tau0); tauBzc = tz;
        end
    end
end
res.T2_tauBzero = tauBzc;
res.T2_offsetBzero = tauBzc - tau0;
fprintf('T2 b 主过零 τ=%.3f（偏差 %+.3f 样本）\n', tauBzc, tauBzc - tau0);

% ============ T3 delayCut=0 状态 ============
% 不裁剪：t(τ) = (τ + 1 − sysDelay)/fs；验证 full 行上脉冲峰的 m 坐标
[~, ipkFull] = max(raw);
mPeak = mraw(ipkFull);                       % 峰的原始 1 基样本
mExpected = sysDelay + tau0;                 % = 连续 τ0 + sysDelay
res.T3_mPeak = mPeak;
res.T3_mExpected = mExpected;
res.T3_errSamples = mPeak - mExpected;
fprintf('T3 delayCut=0：raw 峰样本 m=%d，期望 %.2f，误差 %.2f 样本\n', ...
    mPeak, mExpected, mPeak - mExpected);
% delayCut=0 时 t(τ) 公式自检：τ = mPeak − 1，t 应 = d0/c
tCheck = ((mPeak - 1) + 1 - sysDelay) / fs;
res.T3_tFormulaErr = tCheck - d0 / c;
fprintf('T3 t(τ) 公式自检：t=%.4fµs，期望 %.4fµs，误差 %.4fns\n', ...
    tCheck * 1e6, d0 / c * 1e6, res.T3_tFormulaErr * 1e9);

% ============ T4 一采样点敏感性 ============
% 把 sysDelay 偏移 ±1（等价 raw 数据整体移动 1 样本），看 b 过零位置的偏移
for sh = [-1, +1]
    cutS = raw(sysDelay + sh:end);           % 错误的裁剪起点（±1 样本）
    ppS = gradient(double(cutS), 1 / fs);
    kS = (1:numel(cutS))' - 1;               % τ 坐标
    % 按正确公式重构查询：脉冲真实 τ0 处的 b 值
    vq = interp1(kS, cutS, tau0 - 3:0.01:tau0 + 3, 'linear');
    vpq = interp1(kS, ppS, tau0 - 3:0.01:tau0 + 3, 'linear');
    tq = (tau0 - 3:0.01:tau0 + 3) / fs;
    bq = 2 * vq - 2 * tq .* vpq;
    % b 的主正峰位置偏移
    [~, ipb] = max(bq);
    tauBpk = (tau0 - 3) + (ipb - 1) * 0.01;
    res.T4(sh + 2).shift = sh;
    res.T4(sh + 2).tauBpk = tauBpk;
    res.T4(sh + 2).offset = tauBpk - tau0;
    fprintf('T4 sysDelay%+d：b 主峰 τ=%.3f（偏移 %+.3f 样本，即 %.2fµm 径向）\n', ...
        sh, tauBpk, tauBpk - tau0, (tauBpk - tau0) * c / fs * 1e6);
end

% 断言汇总
res.pass = struct( ...
    'T1_halfSample', abs(res.T1_errSamples) <= 0.5, ...
    'T2_pmaxQuarter', abs(res.T2_offsetPmax) <= 0.25, ...
    'T2_pzeroQuarter', abs(res.T2_offsetPzero) <= 0.25, ...
    'T3_halfSample', abs(res.T3_errSamples) <= 0.5, ...
    'T4_detectOneSample', abs(res.T4(1).offset - res.T4(3).offset) >= 0.8);
fprintf('\n断言：T1(±0.5样本)=%d T2峰(±0.25)=%d T2零(±0.25)=%d T3(±0.5)=%d T4可分辨±1样本=%d\n', ...
    res.pass.T1_halfSample, res.pass.T2_pmaxQuarter, res.pass.T2_pzeroQuarter, ...
    res.pass.T3_halfSample, res.pass.T4_detectOneSample);

out = res;
out.params = struct('fs', fs, 'c', c, 'sampDepth', sampDepth, 'sysDelay', sysDelay, ...
    'maskLength', maskLength, 'd0', d0, 'sigmaSamp', sigmaSamp);
out.matlabVersion = version;
fid = fopen(fullfile(outdir, 'exp3_time_axis.json'), 'w');
fwrite(fid, jsonencode(out, 'PrettyPrint', true)); fclose(fid);
fprintf('EXP3_DONE -> %s\n', fullfile(outdir, 'exp3_time_axis.json'));

% ================= 局部函数 =================
function y = gaussPulse(t, t0, sigma)
% 一阶导形状？——不用。用高斯包络（回波压力波形近似），中心 t0
y = exp(-(t - t0).^2 / (2 * sigma^2));
end
