% EXP3_TIME_AXIS  判定性实验 3：时间轴/delayCut 索引公式、单采样脉冲测试、
% 两态完整反演等价（审查整改 R3）。
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
% 生产语义核对（9493962 源码，整改证据）：
%   preprocessBlock delayCut=0 仅保留全行（srcRow0=0），CUDA 查询 tf=d·fs/c
%   直接作用于存储行、无任何 systemDelay 补偿（ring_recon_cuda.cu 无该引用）
%   ⟹ 生产 delayCut=0 的"旧行为"把原始样本 1 当作声学零点，与拟议反演语义
%   （t=(τ+1−sysDelay)/fs）不等价；T5.4 量化两者差异。区分：旧路径兼容行为
%   ≠ 拟议反演语义；阶段 B 需局部适配（tf_eff = tf + sysDelay − 1 或强制
%   delayCut=1），本阶段不修改生产代码。
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
%   T5【R3 新增】delayCut 两态完整反演等价：同一校准声学信号 pAc(t)（解析 +
%      解析导数），经 sysDelay ∈ {358(wl1), 371(wl2), 359(通道差)} 写入 raw，
%      两态在同一物理走时处比较 p、p′、b（整数 + 亚样本走时）；±1 样本扰动
%      可分辨；并量化"不补偿延时"（生产 delayCut=0 旧行为）的偏差。
%   T6【R3 新增】双声速（分层）路径走时的两态等价 + 同速退化。
%
% 运行：matlab -batch "run('exp3_time_axis.m')"   （工作目录 = matlab/）
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
pp = timeDerivative(cut, 1 / fs);            % dp/dt [Pa/s]，端点单侧
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
    ppS = timeDerivative(cutS, 1 / fs);
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

% ============ T5【R3】delayCut 两态完整反演等价（均匀声速） ============
% 校准声学信号（双高斯脉冲，解析 + 解析导数），查询物理时间含整数与亚样本。
t1 = d0 / c;                                 % 4mm 回波
t2 = 6.5e-3 / c;                             % 6.5mm 回波
sigT = sigmaSamp / fs;                       % 包络宽度 [s]
pAc = @(tq) 1.0 * exp(-(tq - t1).^2 / (2 * sigT^2)) + ...
           0.6 * exp(-(tq - t2).^2 / (2 * sigT^2));
ppAc = @(tq) -(tq - t1) / sigT^2 .* exp(-(tq - t1).^2 / (2 * sigT^2)) - ...
            0.6 * (tq - t2) / sigT^2 .* exp(-(tq - t2).^2 / (2 * sigT^2));
% 查询物理时间：整数走时 700/2400 样本、亚样本 703.64（t1+0.13µs）、脉冲中心等
tauQlist = [700, 703.64, tau0, 1090.6, 2400];        % [样本，连续坐标]
tQlist = tauQlist / fs;                              % [s] 物理走时
fracQ = tauQlist - floor(tauQlist);
fprintf('\nT5 两态反演等价：查询 τ=[%s] 样本（小数部分 [%s]——含整数与亚样本）\n', ...
    sprintf('%.2f ', tauQlist), sprintf('%.2f ', fracQ));

chList = struct('name', {'wl1', 'wl2', 'chVar'}, 'sysD', {358, 371, 359});
res.T5 = struct();
for ic = 1:numel(chList)
    sD = chList(ic).sysD;
    rawC = zeros(sampDepth, 1);
    mC = (1:sampDepth)';
    rawC = pAc((mC - sD) / fs);                  % 同一声学信号 + 系统延时 sD
    rawC(mC - sD < 0) = 0;                       % 触发前置零（t<0）
    % --- state 1：delayCut=1（裁剪线，t = τ/fs）---
    cutC = rawC(sD:end);
    ppCut = timeDerivative(cutC, 1 / fs);
    % --- state 0：delayCut=0（全行，t = (τ+1−sysDelay)/fs）---
    ppFull = timeDerivative(rawC, 1 / fs);
    for iq = 1:numel(tauQlist)
        tauQ = tauQlist(iq);  tQ = tQlist(iq);
        % state1：裁剪线连续坐标 τ = tQ·fs
        p1 = interpSamp(cutC, tauQ);
        pp1 = interpSamp(ppCut, tauQ);
        b1 = 2 * p1 - 2 * tQ * pp1;
        % state0：全行连续坐标 τ0 = tQ·fs + sysDelay − 1（原始行坐标 m = τ0+1）
        tau0Q = tauQ + sD - 1;
        p0 = interpSamp(rawC, tau0Q);
        pp0 = interpSamp(ppFull, tau0Q);
        b0 = 2 * p0 - 2 * tQ * pp0;
        % 旧行为（不补偿延时）：生产 delayCut=0 查询 tf = d·fs/c 直接用
        pNaive = interpSamp(rawC, tauQ);
        % 解析参考
        pAn = pAc(tQ);  bAn = 2 * pAn - 2 * tQ * ppAc(tQ);
        res.T5.(chList(ic).name).p(iq) = p1;
        res.T5.(chList(ic).name).b(iq) = b1;
        res.T5.(chList(ic).name).dStateP(iq) = abs(p1 - p0);
        res.T5.(chList(ic).name).dStateB(iq) = abs(b1 - b0);
        res.T5.(chList(ic).name).dNaive(iq) = abs(pNaive - p1);
        res.T5.(chList(ic).name).dAnalyticP(iq) = abs(p1 - pAn);
        res.T5.(chList(ic).name).dAnalyticB(iq) = abs(b1 - bAn);
    end
end
% 跨通道一致性：同一物理走时处，wl1/wl2/chVar 的 state1 查询应逐位一致
dCrossP = 0;  dCrossB = 0;
for iq = 1:numel(tauQlist)
    dCrossP = max(dCrossP, abs(res.T5.wl1.p(iq) - res.T5.wl2.p(iq)));
    dCrossP = max(dCrossP, abs(res.T5.wl1.p(iq) - res.T5.chVar.p(iq)));
    dCrossB = max(dCrossB, abs(res.T5.wl1.b(iq) - res.T5.wl2.b(iq)));
    dCrossB = max(dCrossB, abs(res.T5.wl1.b(iq) - res.T5.chVar.b(iq)));
end
maxStateP = max(max([res.T5.wl1.dStateP, res.T5.wl2.dStateP, res.T5.chVar.dStateP]));
maxStateB = max(max([res.T5.wl1.dStateB, res.T5.wl2.dStateB, res.T5.chVar.dStateB]));
maxNaive = max(max([res.T5.wl1.dNaive, res.T5.wl2.dNaive, res.T5.chVar.dNaive]));
maxAnP = max(max([res.T5.wl1.dAnalyticP, res.T5.wl2.dAnalyticP, res.T5.chVar.dAnalyticP]));
maxAnB = max(max([res.T5.wl1.dAnalyticB, res.T5.wl2.dAnalyticB, res.T5.chVar.dAnalyticB]));
bScale = max(abs(res.T5.wl1.b));
res.T5.maxStateDiffP = maxStateP;
res.T5.maxStateDiffB = maxStateB;
res.T5.maxCrossChannelDiffP = dCrossP;
res.T5.maxCrossChannelDiffB = dCrossB;
res.T5.maxNoCompensationDiff = maxNaive;     % 旧行为与拟议语义之差
res.T5.maxAnalyticErrP = maxAnP;
res.T5.maxAnalyticErrB = maxAnB;
res.T5.maxAnalyticErrBRel = maxAnB / bScale;
res.T5.analyticBNote = ['b-vs-analytic is NOT gated: it measures linear-interpolation error of p'' at sub-sample tau (same in both states), amplified by 2*t; for the sigma=2-sample stress pulse the derivative swings within a few samples, so the b interpolation error is large. The two-state equivalence (the R3 deliverable) is unaffected because both states share identical interpolation error. Real/LP-filtered signals are much wider (exp2 phantom ~84 samples), making this error negligible there.'];
fprintf('T5 两态差：max|Δp|=%.3g max|Δb|=%.3g（同一插值误差，浮点级）\n', maxStateP, maxStateB);
fprintf('T5 跨通道差：p=%.3g b=%.3g；不补偿延时（生产 delayCut=0 旧行为 vs 拟议语义）：max|Δp|=%.4f\n', ...
    dCrossP, dCrossB, maxNaive);
fprintf('T5 解析参考：p=%.4f（线性插值界 ≤0.04，判定）；b=%.3g（相对 |b|max %.3g，仅报告不判定——亚样本导数插值误差，见 analyticBNote）\n', ...
    maxAnP, maxAnB, res.T5.maxAnalyticErrBRel);

% ±1 样本扰动可分辨性（两态框架下，错误 sysDelay 的 b 偏差）
sD = 358;
rawC = pAc((mraw - sD) / fs);  rawC(mraw < sD) = 0;
dB = 0;
for sh = [-1, +1]
    cutS = rawC(sD + sh:end);
    ppS = timeDerivative(cutS, 1 / fs);
    for iq = 1:numel(tauQlist)
        tQ = tQlist(iq);
        b1 = 2 * interpSamp(cutS, tauQlist(iq)) - 2 * tQ * interpSamp(ppS, tauQlist(iq));
        dB = max(dB, abs(b1 - res.T5.wl1.b(iq)));
    end
end
res.T5.perturbMaxDb = dB;
res.T5.perturbDetectable = dB >= 0.1 * bScale;
fprintf('T5 ±1 样本扰动：max|Δb|=%.4f（≥0.1·|b|max=%.3f → 可分辨=%d）\n', ...
    dB, 0.1 * bScale, res.T5.perturbDetectable);

% ============ T6【R3】双声速（分层）路径走时的两态等价 ============
Rr = 6.57e-3;  rb = 4.0e-3;  c1 = 1490.0;  c2 = 1540.0;
% 像素在原点、探测器在角度 0：分层走时 τL = d·fs/c2 + L1·fs·(1/c1−1/c2)
det = [Rr; 0];  pix = [0; 0];
dPix = norm(pix - det);
R2j = dot(det, det);  r2p = dot(pix, pix);
dotp6 = dot(det, pix) - R2j;
dist2_6 = (r2p - R2j) - 2 * dotp6;
bothIn = (R2j <= rb^2) && (r2p <= rb^2);
discr4 = dotp6^2 - dist2_6 * (R2j - rb^2);
sd = sqrt(max(discr4, 0));
u1 = (-dotp6 - sd) / max(dist2_6, 1e-12);
u2 = (-dotp6 + sd) / max(dist2_6, 1e-12);
Lc = max((min(max(u2, 0), 1) - max(min(u1, 1), 0)) * dPix, 0);
L1 = dPix * double(bothIn) + Lc * double(discr4 > 0 && ~bothIn);
tauL = dPix * fs / c2 + L1 * fs * (1 / c1 - 1 / c2);
tL = tauL / fs;
% 同速退化检查：c1=c2 时 τL 应 = d·fs/c1
tauLdeg = dPix * fs / c1 + L1 * fs * (1 / c1 - 1 / c1);
res.T6_tauL = tauL;
res.T6_degenErrSamples = abs(tauLdeg - dPix * fs / c1);
% 分层声学信号（脉冲置于 τL），两态查询
pAcL = @(tq) exp(-(tq - tL).^2 / (2 * sigT^2));
ppAcL = @(tq) -(tq - tL) / sigT^2 .* exp(-(tq - tL).^2 / (2 * sigT^2));
dL = 0;  dLan = 0;
for sD = [358, 371]
    rawL = pAcL((mraw - sD) / fs);  rawL(mraw < sD) = 0;
    cutL = rawL(sD:end);
    ppCutL = timeDerivative(cutL, 1 / fs);
    ppFullL = timeDerivative(rawL, 1 / fs);
    p1 = interpSamp(cutL, tauL);
    pp1 = interpSamp(ppCutL, tauL);
    b1 = 2 * p1 - 2 * tL * pp1;
    p0 = interpSamp(rawL, tauL + sD - 1);
    pp0 = interpSamp(ppFullL, tauL + sD - 1);
    b0 = 2 * p0 - 2 * tL * pp0;
    dL = max(dL, max(abs(p1 - p0), abs(b1 - b0)));
    pAn = pAcL(tL);
    dLan = max(dLan, abs(p1 - pAn));
end
res.T6.maxStateDiff = dL;
res.T6.maxAnalyticErrP = dLan;
fprintf('T6 分层走时 τL=%.4f 样本（像素原点，c1/c2=%.0f/%.0f，L1=%.4fmm）；同速退化 |Δτ|=%.3g 样本\n', ...
    tauL, c1, c2, L1 * 1e3, res.T6_degenErrSamples);
fprintf('T6 两态差（p 与 b）：%.3g；解析参考误差：%.4f\n', dL, dLan);

% 断言汇总
res.pass = struct( ...
    'T1_halfSample', abs(res.T1_errSamples) <= 0.5, ...
    'T2_pmaxQuarter', abs(res.T2_offsetPmax) <= 0.25, ...
    'T2_pzeroQuarter', abs(res.T2_offsetPzero) <= 0.25, ...
    'T3_halfSample', abs(res.T3_errSamples) <= 0.5, ...
    'T4_detectOneSample', abs(res.T4(1).offset - res.T4(3).offset) >= 0.8, ...
    'T5_stateEquivalence', maxStateP <= 1e-12 && maxStateB <= 1e-9 * bScale, ...
    'T5_crossChannel', dCrossP == 0 && dCrossB == 0, ...
    'T5_analyticP', maxAnP <= 0.04, ...
    'T5_noCompensationLarge', maxNaive >= 0.2, ...
    'T5_perturbDetectable', res.T5.perturbDetectable, ...
    'T6_stateEquivalence', dL <= 1e-9, ...
    'T6_degenerate', res.T6_degenErrSamples <= 1e-9, ...
    'T6_analyticP', dLan <= 0.04);
fprintf('\n断言：T1(±0.5)=%d T2峰=%d T2零=%d T3(±0.5)=%d T4可分辨=%d | T5两态=%d 跨通道=%d 解析=%d 不补偿偏差大=%d 扰动=%d | T6两态=%d 退化=%d 解析=%d\n', ...
    res.pass.T1_halfSample, res.pass.T2_pmaxQuarter, res.pass.T2_pzeroQuarter, ...
    res.pass.T3_halfSample, res.pass.T4_detectOneSample, ...
    res.pass.T5_stateEquivalence, res.pass.T5_crossChannel, res.pass.T5_analyticP, ...
    res.pass.T5_noCompensationLarge, res.pass.T5_perturbDetectable, ...
    res.pass.T6_stateEquivalence, res.pass.T6_degenerate, res.pass.T6_analyticP);

out = res;
out.params = struct('fs', fs, 'c', c, 'sampDepth', sampDepth, 'sysDelay', sysDelay, ...
    'maskLength', maskLength, 'd0', d0, 'sigmaSamp', sigmaSamp, ...
    'tauQlist', tauQlist, 'channels', {fieldnames(res.T5)}, ...
    'productionDelayCut0', 'no delay compensation in query (src/RingRecon: preprocessBlock srcRow0=0; CUDA kernel tf=d*fs/c on stored line, no systemDelay reference) - legacy behavior differs from proposed inversion semantics, stage B adaptation required');
out.matlabVersion = version;
fid = fopen(fullfile(outdir, 'exp3_time_axis.json'), 'w');
fwrite(fid, jsonencode(out, 'PrettyPrint', true)); fclose(fid);
fprintf('EXP3_DONE -> %s\n', fullfile(outdir, 'exp3_time_axis.json'));

passOk = true;
fn = fieldnames(res.pass);
for k = 1:numel(fn)
    passOk = passOk && res.pass.(fn{k});
end
assert(passOk, 'exp3:gateFailed', 'EXP3 断言未全部通过（见 evidence/exp3_time_axis.json）');

% ================= 局部函数 =================
function y = gaussPulse(t, t0, sigma)
% 高斯包络（回波压力波形近似），中心 t0
y = exp(-(t - t0).^2 / (2 * sigma^2));
end

function v = interpSamp(col, tau)
% 与生产一致的线性插值查询（1 基样本 k 位于连续坐标 τ=k−1；越界返回 0）
Nt = numel(col);
i0f = floor(tau);
frac = tau - i0f;
i0 = i0f + 1;
if i0 >= 1 && i0 <= Nt - 1
    i0c = i0;
    v = col(i0c) + frac * (col(i0c + 1) - col(i0c));
else
    v = 0;                                   % maskOob
end
end
