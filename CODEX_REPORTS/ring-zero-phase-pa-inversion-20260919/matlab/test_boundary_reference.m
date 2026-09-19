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
%   S3 pad 收敛：递增前后 pad 序列 vs 最大 pad 参考，目标窗内 p/p′/b 相对差
%      达到记录容差（1e-9），记录 convergedPad（echoOnly 与 burstEcho 两场景）。
%   S4 物理时间对齐：远离边界的回波（τ=2700，Nt=8000）短窗 vs 收敛长参考
%      幅值比/位置偏差/波形误差在记录容差内（对齐无整体平移、导数原点正确）。
%   S5 起端/终端敏感性可测性：burst 污染起点、近尾回波（τ=3600）的短窗 vs
%      长参考差异显著大于噪声底（证明套件能测出有限窗边界效应；不预设大小）。
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
    relP = [];  relPP = [];  relB = [];
    for pv = padListS
        [~, yLp, ppLp, infoLp] = longReference(rn, 800, D, Nt, dbrEnd, pv, pv, ...
            sosHP, gHP, nHP, sosLP, gLP, nLP, fs);
        yp = yLp(infoLp.extractIdx(1):infoLp.extractIdx(2));
        ppp = ppLp(infoLp.extractIdx(1):infoLp.extractIdx(2));
        bp = 2 * yp - 2 * tVec .* ppp;
        relP(end+1) = max(abs(yp(w1:w2) - yR(w1:w2))) / sP; %#ok<AGROW>
        relPP(end+1) = max(abs(ppp(w1:w2) - ppR(w1:w2))) / sPP; %#ok<AGROW>
        relB(end+1) = max(abs(bp(w1:w2) - bR(w1:w2))) / sB; %#ok<AGROW>
    end
    okPads = padListS(relP <= convTol & relPP <= convTol & relB <= convTol);
    cp = min(okPads);
    res.convergence.(rn) = struct('padList', padListS, 'relP', relP, 'relPP', relPP, ...
        'relB', relB, 'scaleP', sP, 'scalePP', sPP, 'scaleB', sB, ...
        'convergedPad', cp, 'convTolRel', convTol, 'window', [w1, w2]);
    fprintf('S3 pad 收敛（%s）：relP=[%s] → convergedPad=%d（容差 %.0e）\n', rn, ...
        sprintf('%.2g ', relP), cp, convTol);
end
pass.S3_padConvergence = isfinite(res.convergence.echoOnly.convergedPad) && ...
    isfinite(res.convergence.burstEcho.convergedPad) && ...
    res.convergence.echoOnly.convergedPad <= padRefS && ...
    res.convergence.burstEcho.convergedPad <= padRefS;

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
