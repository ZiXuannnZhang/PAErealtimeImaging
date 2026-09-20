% TEST_BOUNDARY_REFERENCE  二轮整改 B1 结构/回归测试（任务 §7.1/§10）。
%
% 断言"独立长窗边界参考"的结构性质与测试套件的判别能力，与 exp5 B6 的
% 科学量化互补（exp5 量化误差大小，本测试保证构造有效、旧错误不能复活）：
%   S1 短/长两路实际送入滤波器的样本范围、长度、声学时间端点；长参考更长且
%      额外段在滤波时存在；长输入含短窗输入为连续子段（逐位）；同一声学原点。
%   S2 旧"同时平移 sysDelay 后先裁剪"构造负例：其滤波输入与短窗逐位相同
%      （maxdiff==0）→ 不得被认定为有效长参考（回归保护：该构造一旦复活即失败）。
%   S2b 错位提取负例：长线滤波后若漏加 padL 偏移直接按短线索引提取 → 对齐检查
%      必须能发现（信号不在窗内）；导数时间原点若重新归零 → b 误差必须可测。
%   S3 pad 收敛（C1 整改）：递增前后 pad 序列 vs 最大 pad 参考，目标窗内
%      p/p′/b 相对差达到记录容差（1e-9）；判据走统一入口 padConvergenceCheck：
%      padRef 只作参考（自比恒零禁止作为候选）、至少两个不同非参考 pad 通过、
%      两两稳定（实际两两差）、首个通过 pad 起无再次超差；候选不足/非有限/
%      全超差一律 FAIL。echoOnly 与 burstEcho 两场景。
%   S4 物理时间对齐：远离边界的回波（τ=2700，Nt=8000）短窗 vs 收敛长参考
%      幅值比/位置偏差/波形误差在记录容差内（对齐无整体平移、导数原点正确）。
%   S5 起端/终端敏感性可测性：burst 污染起点、近尾回波（τ=3600）的短窗 vs
%      长参考差异显著大于噪声底（证明套件能测出有限窗边界效应；不预设大小）。
%   S3n 判据正负例（C1 整改，任务 §7.C1）：人工误差表直接喂判据，不重跑前向。
%      N1 仅参考自比通过（其余全超差）、N2 仅一个非参考 pad 通过、N3 尾段
%      回落、N4 NaN/Inf、N5/N5b 无候选/形状非法 → 判据必须 FAIL；
%      P1 稳定尾段、P2 参考行排除、P3 双候选 → 判据必须 PASS。
%   S6 无滤波组合：短窗与长参考提取逐位一致（无滤波→无边界效应，结构自检）。
%
% 运行：matlab -batch "run('test_boundary_reference.m')"   （工作目录 = matlab/）
% 输出：evidence/test_boundary_reference.json；任一断言失败 error() → 非零退出。

outdir = fullfile(fileparts(mfilename('fullpath')), '..', 'evidence');
if ~exist(outdir, 'dir'), mkdir(outdir); end

fs = 250e6;  c0 = 1490.0;
D = 358;  Nt = 4000;  dbrEnd = 313;   % 生产默认（wl1）
nHP = 4;  fcHP = 0.4e6;  nLP = 4;  fcLP = 40e6;
[z1, p1z, k1] = butter(nHP, 2 * fcHP / fs, 'high');  [sosHP, gHP] = zp2sos(z1, p1z, k1);
[z2, p2z, k2] = butter(nLP, 2 * fcLP / fs, 'low');   [sosLP, gLP] = zp2sos(z2, p2z, k2);
padL = 4000;  padR = 4000;  padRefS = 8000;
padListS = [250, 500, 1000, 2000, 4000, 8000];
convTol = 1e-9;
nCut = Nt - D + 1;
tVec = (0:Nt - D)' / fs;

res = struct();
res.params = struct('fs', fs, 'sysDelayD', D, 'Nt', Nt, 'dbrEnd', dbrEnd, ...
    'padL', padL, 'padR', padR, 'padUsed', padRefS, 'padList', padListS, ...
    'convTolRel', convTol, 'shortFilterLen', nCut, ...
    'shortTauRange', [0, Nt - D], 'c', c0);
pass = struct();

% ---- S1 结构：两路实际滤波输入范围 ----
rawS = ringAcousticModel('echoOnly', 800, (1:Nt)' - D, fs);
xS = rawS(D:end);
[xL, yLf, ppLf, info] = longReference('echoOnly', 800, D, Nt, dbrEnd, padL, padR, ...
    sosHP, gHP, nHP, sosLP, gLP, nLP, fs);
res.ranges = struct( ...
    'shortInput', struct('len', numel(xS), 'tauStart', 0, 'tauEnd', Nt - D, ...
        'originSample', D), ...
    'longInput', struct('len', info.len, 'tauStart', info.tauStart, ...
        'tauEnd', info.tauEnd, 'originSample', info.originSample, ...
        'extractIdx', info.extractIdx), ...
    'extraSamples', info.len - numel(xS), ...
    'extraPresentAtFilter', info.tauStart < 0 && info.tauEnd > Nt - D);
res.ranges.longContainsShortBitwise = ...
    isequal(xL(info.extractIdx(1):info.extractIdx(1) + nCut - 1), xS);
pass.S1_longerByPad = (info.len == nCut + padL + padR) && (info.len - numel(xS) == padL + padR);
pass.S1_containsShortBitwise = res.ranges.longContainsShortBitwise;
pass.S1_extraPresentAtFilter = res.ranges.extraPresentAtFilter && info.originSample == D;
fprintf('S1 结构：短 %d 样本 τ∈[%d,%d] / 长 %d 样本 τ∈[%d,%d]（+%d），逐位包含=%d\n', ...
    numel(xS), 0, Nt - D, info.len, info.tauStart, info.tauEnd, info.len - nCut, ...
    res.ranges.longContainsShortBitwise);

% ---- S2 旧构造负例：先平移 sysDelay 再裁剪 → 滤波输入与短窗逐位相同 ----
res.oldConstruction = struct();
for padOld = [2000, padRefS]
    rawOld = ringAcousticModel('echoOnly', 800, (1:Nt + padOld)' - (D + padOld), fs);
    xOld = rawOld(D + padOld:end);            % 旧 processN 的 cut
    res.oldConstruction.(sprintf('pad%d_maxDiff', padOld)) = max(abs(xOld - xS));
end
oldD = [res.oldConstruction.pad2000_maxDiff, res.oldConstruction.pad8000_maxDiff];
pass.S2_oldConstructFlaggedInvalid = all(oldD == 0);   % 逐位相同→自比，非独立参考
fprintf('S2 旧构造负例：pad=2000 maxdiff=%.3g，pad=8000 maxdiff=%.3g（==0 → 识别为自比）\n', ...
    oldD(1), oldD(2));

% ---- S2b 错位提取负例：漏加 padL 偏移 → 对齐检查可发现 ----
yWrong = yLf(1:nCut);                          % 错误：未跳过前 padL 个样本
yRight = yLf(info.extractIdx(1):info.extractIdx(2));
[ampW, tauW] = peakTau(yWrong, 800, 500);
[ampR, tauR] = peakTau(yRight, 800, 500);
res.misalign = struct('ampWrong', ampW, 'ampRight', ampR, 'tauWrong', tauW, 'tauRight', tauR);
pass.S2b_misalignDetectable = ampW < 1e-6 * ampR;      % 回波不在错位窗内（偏移 ≈ padL）
fprintf('S2b 错位提取：错位窗内幅值 %.3g vs 正确 %.3g（<1e-6 比值 → 可发现）\n', ampW, ampR);

% ---- S2c 导数时间原点负例：t 重新归零（+padL/fs）→ b 误差可测 ----
ppR2 = timeDerivative(yRight, 1 / fs);
bOk = 2 * yRight - 2 * tVec .* ppR2;
bBad = 2 * yRight - 2 * (tVec + padL / fs) .* ppR2;
res.derivOrigin = struct('absMaxDiff', max(abs(bOk - bBad)), ...
    'scale', max(abs(bOk)));
pass.S2c_derivOriginDetectable = max(abs(bOk - bBad)) >= 0.1 * max(abs(bOk));
fprintf('S2c 导数原点：重归零 b 最大差 %.4g（≥0.1·|b|max=%.4g → 可测）\n', ...
    max(abs(bOk - bBad)), 0.1 * max(abs(bOk)));

% ---- S3 pad 收敛（echoOnly 与 burstEcho；目标 τ=800 窗 [300,1300]）----
% C1 整改（63e088a 撤回旧判据）：padRef 只作参考，pad==padRef 自比恒零禁止
% 作为通过候选；判据统一走 padConvergenceCheck（与 exp5 B6 同一入口，标准不
% 漂移）：至少两个不同非参考 pad 在容差内、两两稳定（实际两两差，同一参考
% 尺度）、从首个通过 pad 到最大非参考 pad 不再超差；候选不足/非有限/全超差
% 一律 FAIL（不允许空数组 all() 真值通过）。旧"convergedPad<=padRef 即通过"
% 即使其余 pad 全部超差也会通过，已撤回。
res.convergence = struct();
for sc = {'echoOnly', 'burstEcho'}
    rn = sc{1};
    [~, yRf, ppRf, infoR] = longReference(rn, 800, D, Nt, dbrEnd, padRefS, padRefS, ...
        sosHP, gHP, nHP, sosLP, gLP, nLP, fs);
    yR = yRf(infoR.extractIdx(1):infoR.extractIdx(2));
    ppR = ppRf(infoR.extractIdx(1):infoR.extractIdx(2));
    bR = 2 * yR - 2 * tVec .* ppR;
    w1 = 300;  w2 = 1300;
    sP = max(abs(yR(w1:w2)));  sPP = max(abs(ppR(w1:w2)));  sB = max(abs(bR(w1:w2)));
    nP = numel(padListS);
    relP = zeros(1, nP);  relPP = zeros(1, nP);  relB = zeros(1, nP);
    yPad = cell(1, nP);  ppPad = cell(1, nP);  bPad = cell(1, nP);
    for ip = 1:nP
        pv = padListS(ip);
        [~, yLp, ppLp, infoLp] = longReference(rn, 800, D, Nt, dbrEnd, pv, pv, ...
            sosHP, gHP, nHP, sosLP, gLP, nLP, fs);
        yp = yLp(infoLp.extractIdx(1):infoLp.extractIdx(2));
        ppp = ppLp(infoLp.extractIdx(1):infoLp.extractIdx(2));
        bp = 2 * yp - 2 * tVec .* ppp;
        yPad{ip} = yp;  ppPad{ip} = ppp;  bPad{ip} = bp;
        relP(ip) = max(abs(yp(w1:w2) - yR(w1:w2))) / sP;
        relPP(ip) = max(abs(ppp(w1:w2) - ppR(w1:w2))) / sPP;
        relB(ip) = max(abs(bp(w1:w2) - bR(w1:w2))) / sB;
    end
    % 实际两两差矩阵（同一参考尺度）：pairRel(i,j) = pad i 与 pad j 结果之差
    pairRel = struct('diffP', zeros(nP), 'diffPP', zeros(nP), 'diffB', zeros(nP));
    for a = 1:nP
        for b = 1:a - 1
            dPij = max(abs(yPad{a}(w1:w2) - yPad{b}(w1:w2))) / sP;
            dPPij = max(abs(ppPad{a}(w1:w2) - ppPad{b}(w1:w2))) / sPP;
            dBij = max(abs(bPad{a}(w1:w2) - bPad{b}(w1:w2))) / sB;
            pairRel.diffP(a, b) = dPij;  pairRel.diffP(b, a) = dPij;
            pairRel.diffPP(a, b) = dPPij;  pairRel.diffPP(b, a) = dPPij;
            pairRel.diffB(a, b) = dBij;  pairRel.diffB(b, a) = dBij;
        end
    end
    [verdict, det] = padConvergenceCheck(padListS, relP, relPP, relB, padRefS, ...
        convTol, pairRel);
    res.convergence.(rn) = struct('padList', padListS, 'relP', relP, 'relPP', relPP, ...
        'relB', relB, 'scaleP', sP, 'scalePP', sPP, 'scaleB', sB, ...
        'convTolRel', convTol, 'window', [w1, w2], ...
        'verdict', verdict, 'detail', det);
    fprintf('S3 pad 收敛（%s）：relP=[%s] → %s（firstOkPad=%g nOk=%d maxPairRel=%.3g relapse=%d）\n', ...
        rn, sprintf('%.2g ', relP), verdict, det.firstOkPad, det.nOk, ...
        det.maxPairRel, det.relapse);
end
pass.S3_padConvergence = strcmp(res.convergence.echoOnly.verdict, 'PASS') && ...
    strcmp(res.convergence.burstEcho.verdict, 'PASS');

% ---- S4 物理时间对齐（远场回波 τ=2700，Nt=8000，距两端均 > HP 记忆长度）----
Ntf = 8000;
rawF = ringAcousticModel('echoOnly', 2700, (1:Ntf)' - D, fs);
yF = rawF(D:end);
yF = refZeroPhase(yF, sosHP, gHP, nHP);
yF = refZeroPhase(yF, sosLP, gLP, nLP);
[~, yLfF, ppLfF, infoF] = longReference('echoOnly', 2700, D, Ntf, dbrEnd, padRefS, padRefS, ...
    sosHP, gHP, nHP, sosLP, gLP, nLP, fs);
yLF = yLfF(infoF.extractIdx(1):infoF.extractIdx(2));
[ampF1, tauF1] = peakTau(yF, 2700, 500);
[ampF2, tauF2] = peakTau(yLF, 2700, 500);
w1f = 2200;  w2f = 3200;
relPf = max(abs(yF(w1f:w2f) - yLF(w1f:w2f))) / max(abs(yLF(w1f:w2f)));
ppF1 = timeDerivative(yF, 1 / fs);  ppF2full = timeDerivative(yLF, 1 / fs);
relPPf = max(abs(ppF1(w1f:w2f) - ppF2full(w1f:w2f))) / max(abs(ppF2full(w1f:w2f)));
bF1 = 2 * yF - 2 * ((0:Ntf - D)' / fs) .* ppF1;
bF2 = 2 * yLF - 2 * ((0:Ntf - D)' / fs) .* timeDerivative(yLF, 1 / fs);
relBf = max(abs(bF1(w1f:w2f) - bF2(w1f:w2f))) / max(abs(bF2(w1f:w2f)));
res.farFieldAlignment = struct('tauEcho', 2700, 'Nt', Ntf, ...
    'ampRatio', ampF1 / ampF2, 'posBias', tauF1 - tauF2, ...
    'relP', relPf, 'relPP', relPPf, 'relB', relBf, ...
    'tolAmp', 1e-6, 'tolPos', 1e-3, 'tolWave', 1e-6);
pass.S4_physicalTimeAligned = abs(ampF1 / ampF2 - 1) <= 1e-6 && ...
    abs(tauF1 - tauF2) <= 1e-3 && relPf <= 1e-6 && relPPf <= 1e-6 && relBf <= 1e-6;
fprintf('S4 远场对齐（τ=2700, Nt=8000）：ampRatio=%.12f posBias=%+.3g |Δp|rel=%.3g |Δp′|rel=%.3g |Δb|rel=%.3g\n', ...
    ampF1 / ampF2, tauF1 - tauF2, relPf, relPPf, relBf);

% ---- S5 起端/终端敏感性可测性（能测出有限窗边界效应；不预设大小）----
[~, yLbF, ~, infoLb] = longReference('burstOnly', 800, D, Nt, dbrEnd, padRefS, padRefS, ...
    sosHP, gHP, nHP, sosLP, gLP, nLP, fs);
yLB = yLbF(infoLb.extractIdx(1):infoLb.extractIdx(2));
rawB = ringAcousticModel('burstOnly', 800, (1:Nt)' - D, fs);
xSb = rawB;  xSb(1:dbrEnd) = 0;  xSb = xSb(D:end);
ySB = refZeroPhase(xSb, sosHP, gHP, nHP);
ySB = refZeroPhase(ySB, sosLP, gLP, nLP);
wS = 1:1500;
sensStart = max(abs(ySB(wS) - yLB(wS))) / max(abs(yLB(wS)));
res.sensitivity.startBurstRelP = sensStart;
res.sensitivity.startBurstWindow = [0, 1499];
tcEnd = 3600;
rawE2 = ringAcousticModel('echoOnly', tcEnd, (1:Nt)' - D, fs);
xSe = rawE2;  xSe(1:dbrEnd) = 0;  xSe = xSe(D:end);
ySE = refZeroPhase(xSe, sosHP, gHP, nHP);
ySE = refZeroPhase(ySE, sosLP, gLP, nLP);
[~, yLfE, ~, infoLe] = longReference('echoOnly', tcEnd, D, Nt, dbrEnd, padRefS, padRefS, ...
    sosHP, gHP, nHP, sosLP, gLP, nLP, fs);
yLE = yLfE(infoLe.extractIdx(1):infoLe.extractIdx(2));
sensEnd = max(abs(ySE - yLE)) / max(abs(yLE));
res.sensitivity.endEchoRelP = sensEnd;
res.sensitivity.endEchoTau = tcEnd;
pass.S5_boundaryEffectsMeasurable = sensStart > 1e-6 && sensEnd > 1e-6;
fprintf('S5 敏感性可测性：起端 burst |Δp|rel=%.3g，终端 τ=3600 |Δp|rel=%.3g（均 >1e-6 → 套件可测出有限窗效应）\n', ...
    sensStart, sensEnd);

% ---- S6 无滤波组合：短窗与长参考提取逐位一致（无滤波→无边界效应）----
pass.S6_noneComboBitwise = isequal(xS, xL(info.extractIdx(1):info.extractIdx(2)));
fprintf('S6 无滤波组合：短窗 vs 长参考提取逐位一致=%d\n', pass.S6_noneComboBitwise);

% ---- S3n 判据负例/正例（人工误差表，不重跑前向；C1 整改，任务 §7.C1）----
% 以下每个用例直接构造 relP/relPP/relB 误差表喂给 padConvergenceCheck。
% "必须失败"用例的预期结果是 FAIL——被测对象是判据本身的辨别能力，
% 判据返回 FAIL 是测试通过；只有判据对应当 FAIL 的用例返回 PASS 才是失败。
tolN = 1e-9;
padN = [250, 500, 1000, 2000, 4000, 8000];
neg = struct();
% N1 只有参考自比误差为零、其余全超差 → 必须 FAIL（旧判据在此会通过）
r1 = [1e-3, 1e-2, 1e-1, 1, 10, 0];
[v1, d1] = padConvergenceCheck(padN, r1, r1, r1, 8000, tolN);
neg.N1_refSelfOnly = struct('expected', 'FAIL', 'actual', v1, 'pass', ...
    strcmp(v1, 'FAIL'), 'detail', d1);
% N2 仅一个非参考 pad 通过 → 必须 FAIL
r2 = [1e-12, 1e-3, 1e-2, 1e-1, 1, 0];
[v2, d2] = padConvergenceCheck(padN, r2, r2, r2, 8000, tolN);
neg.N2_singleOk = struct('expected', 'FAIL', 'actual', v2, 'pass', ...
    strcmp(v2, 'FAIL'), 'detail', d2);
% N3 两个较小 pad 通过但更大非参考 pad 再次超差 → 必须 FAIL（尾段回落）
r3 = [1e-12, 1e-12, 1e-12, 5e-2, 1e-1, 0];
[v3, d3] = padConvergenceCheck(padN, r3, r3, r3, 8000, tolN);
neg.N3_relapseAfterOk = struct('expected', 'FAIL', 'actual', v3, 'pass', ...
    strcmp(v3, 'FAIL'), 'detail', d3);
% N4 NaN / Inf → 必须 FAIL（非有限误差）
r4 = [1e-12, NaN, 1e-12, 1e-12, Inf, 0];
[v4, d4] = padConvergenceCheck(padN, r4, r4, r4, 8000, tolN);
neg.N4_nonFinite = struct('expected', 'FAIL', 'actual', v4, 'pass', ...
    strcmp(v4, 'FAIL'), 'detail', d4);
% N5 没有候选（padList 仅含参考）→ 必须 FAIL（空数组不得真值通过）
[v5, d5] = padConvergenceCheck(8000, 0, 0, 0, 8000, tolN);
neg.N5_noCandidates = struct('expected', 'FAIL', 'actual', v5, 'pass', ...
    strcmp(v5, 'FAIL'), 'detail', d5);
% N5b 形状不合法（误差表长度不一致）→ 必须 FAIL
[v5b, d5b] = padConvergenceCheck(padN, 1e-12, [1e-12, 1e-12], 1e-12, 8000, tolN);
neg.N5b_shapeMismatch = struct('expected', 'FAIL', 'actual', v5b, 'pass', ...
    strcmp(v5b, 'FAIL'), 'detail', d5b);
% P1 至少两个非参考 pad 形成满足规则的稳定尾段 → 必须 PASS
rP1 = [1e-12, 1e-12, 1e-13, 1e-13, 1e-14, 0];
[vP1, dP1] = padConvergenceCheck(padN, rP1, rP1, rP1, 8000, tolN);
neg.P1_stableTail = struct('expected', 'PASS', 'actual', vP1, 'pass', ...
    strcmp(vP1, 'PASS'), 'detail', dP1);
% P2 参考行超差但参考只是"参考"（第 2/3 个非参考 pad 稳定）→ 必须 PASS；
%     该用例锁定"padRef 行不参与容差判定"的语义（参考自身误差表恒为 0，
%     但即使人为给参考行一个超差值也不影响候选判定——padRef 行被排除）。
rP2 = [1e-12, 1e-12, 1e-13, 1e-13, 1e-14, 5e-1];
[vP2, dP2] = padConvergenceCheck(padN, rP2, rP2, rP2, 8000, tolN);
neg.P2_refRowExcluded = struct('expected', 'PASS', 'actual', vP2, 'pass', ...
    strcmp(vP2, 'PASS'), 'detail', dP2);
% P3 仅两个非参考候选（最小合法用例）→ PASS
padP3 = [2000, 4000, 8000];
rP3 = [1e-12, 1e-13, 0];
[vP3, dP3] = padConvergenceCheck(padP3, rP3, rP3, rP3, 8000, tolN);
neg.P3_twoCandidateOk = struct('expected', 'PASS', 'actual', vP3, 'pass', ...
    strcmp(vP3, 'PASS'), 'detail', dP3);

res.criterionNegatives = neg;
pass.S3n_negative1_refSelfOnly = neg.N1_refSelfOnly.pass;
pass.S3n_negative2_singleOk = neg.N2_singleOk.pass;
pass.S3n_negative3_relapse = neg.N3_relapseAfterOk.pass;
pass.S3n_negative4_nonFinite = neg.N4_nonFinite.pass;
pass.S3n_negative5_noCandidates = neg.N5_noCandidates.pass;
pass.S3n_negative5b_shape = neg.N5b_shapeMismatch.pass;
pass.S3n_positive1_stableTail = neg.P1_stableTail.pass;
pass.S3n_positive2_refRowExcluded = neg.P2_refRowExcluded.pass;
pass.S3n_positive3_twoCandidates = neg.P3_twoCandidateOk.pass;
fprintf('S3n 判据正负例（人工误差表）：N1自比=%s N2单pad=%s N3回落=%s N4非有限=%s N5无候选=%s N5b形状=%s | P1稳定尾段=%s P2参考行排除=%s P3双候选=%s\n', ...
    neg.N1_refSelfOnly.actual, neg.N2_singleOk.actual, neg.N3_relapseAfterOk.actual, ...
    neg.N4_nonFinite.actual, neg.N5_noCandidates.actual, neg.N5b_shapeMismatch.actual, ...
    neg.P1_stableTail.actual, neg.P2_refRowExcluded.actual, neg.P3_twoCandidateOk.actual);

% ---- 汇总 ----
res.pass = pass;
res.matlabVersion = version;
fid = fopen(fullfile(outdir, 'test_boundary_reference.json'), 'w');
fwrite(fid, jsonencode(res, 'PrettyPrint', true)); fclose(fid);
fprintf('TEST_BOUNDARY_REFERENCE_DONE -> %s\n', fullfile(outdir, 'test_boundary_reference.json'));

fn = fieldnames(pass);
ok = true;
for k = 1:numel(fn)
    ok = ok && pass.(fn{k});
end
assert(ok, 'test_boundary_reference:failed', ...
    '边界参考结构/收敛/负例测试未全部通过（见 evidence/test_boundary_reference.json）');

% ================= 局部函数 =================
function [amp, tauPk] = peakTau(y, tc, halfWin)
% 窗内最大正值峰 + 抛物线插值峰位（τ 连续样本坐标，0 基；与 exp5 targetMetrics 一致）
i1 = max(1, tc - halfWin);  i2 = min(numel(y), tc + halfWin);
seg = y(i1:i2);
[amk, im] = max(seg);
ipk = i1 + im - 1;
if ipk > 1 && ipk < numel(y)
    ym1 = y(ipk - 1);  y0 = y(ipk);  yp1 = y(ipk + 1);
    den = ym1 - 2 * y0 + yp1;
    if den ~= 0
        delta = 0.5 * (ym1 - yp1) / den;
    else
        delta = 0;
    end
    tauPk = (ipk - 1) + delta;
else
    tauPk = ipk - 1;
end
amp = amk;
end
