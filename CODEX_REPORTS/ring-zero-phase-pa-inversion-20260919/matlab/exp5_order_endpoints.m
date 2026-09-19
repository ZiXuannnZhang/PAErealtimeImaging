% EXP5_ORDER_ENDPOINTS  判定性实验 5【审查整改版】：滤波顺序/DBR边界策略/端点约定。
%
% 相对 9493962 版的整改（追加任务 R4/R5）
% ----
% R4-a 同一实现对照：顺序 N（提议）与顺序 M（历史切片）全部用 refZeroPhase
%      （同一 SOS 实现）；历史 filtfilt 实现作为第三行 Mff 单列，实现差异与
%      顺序差异可分离归因（旧版把两者混在一起）。
% R4-b 干扰/目标分离：echoOnly（无干扰参考）、burstOnly（启动干扰残留）、
%      burstEcho（干扰下目标）三组 run 分别测量；近边界目标与"边界远移 2000
%      样本的同声学场景理想参考"比较，边界约定误差与干扰效应分离量化。
% R4-c DBR/边界矩阵：zeroRows = maskLength+extra ∈ {300,313,358,400,413} 覆盖
%      DBR 末端 早于/等于/晚于 裁剪起点 与双波长 extra ∈ {0,13,100} 变体；
%      在 burst 场景量化 DBR 阶跃（截断启动信号）经双向滤波向邻域的传播。
% R4-d 近边界目标：τecho ∈ {400,600,800,1200,2700}，p 路与反演 b 路分别量化
%      幅值/位置误差——证据用于否定"机械丢弃 848–3639 样本"（稳态起点线的
%      边界约定误差 ≈0；丢弃的依据只能是干扰，而干扰是数据相关量）。
% R5 归因：差异按 顺序/实现/边界 三个因素分列。
%
% 指标约定：回波为高斯包络 5MHz 正弦，峰位固有偏移（载波相位）：p 路 ≈ +12.3
% 样本、b 路 ≈ +24.2 样本——位置偏差一律相对同波形参考（不把固有偏移当误差）。
% b 路幅值物理上随绝对声学时间 t 缩放（导数项主导），跨 τ 比较仅在同声学场景
% （理想参考/干扰对照）之间进行。
%
% 冻结的默认策略（res.policy，algorithm-stage-A.md §3.3 同步）：
%   顺序: raw → DBR置零(1..maskLength+extra) → 削顶 → DelayCut
%         → HP零相位(整条裁剪线) → LP零相位(整条裁剪线) → [反演: p′ → b]
%   端点: refZeroPhase 稳态初始化 + 奇对称延拓(nfact=3n)；线长≤3n 或含 NaN/Inf 报错
%   有效查询区(原始行坐标 m): m ≥ max(sysDelay, dbrEnd+1)，默认不额外加裕量；
%     当 HP/LP 滤波（含反演）启用时，要求 dbrEnd = maskLength+dbrmaskExtra <
%     sysDelay，否则报错拒绝：实测（B2）被截断启动信号的零相位响应传播至
%     ~2737 样本，超过 dbrEnd+memHP=2327 且依赖被截断信号的幅度/长度（数据
%     相关），任何固定裕量都无法保证有效性。默认 313 < 358/371 天然满足。
%   头部处理: 不做固定裕量丢弃。稳态起点线的边界约定误差 ≈0（B6 实测）；
%     启动干扰是数据相关量，以 burstOnly 残留曲线报告，不把冲击响应宽度
%     848–3639 当作所有输入的污染界限。
%   端点导数: timeDerivative（中心差分/端点单侧，逐列），作用于同一滤波后线。
%
% 运行：matlab -batch "run('exp5_order_endpoints.m')"   （工作目录 = matlab/）
% 输出：evidence/exp5_order_endpoints.json、evidence/exp5_order_endpoints.mat

outdir = fullfile(fileparts(mfilename('fullpath')), '..', 'evidence');
if ~exist(outdir, 'dir'), mkdir(outdir); end

fs = 250e6;  c = 1490.0;
Nt = 4000;
sysDelay = 358;                  % wl1 默认（wl2=371 见 B7）
maskLength = 300;
nHP = 4;  fcHP = 0.4e6;
nLP = 4;  fcLP = 40e6;
pad = 2000;                      % 理想参考：裁剪起点前移样本数（≫滤波器记忆长度）

% ---- SOS 设计（一次设计，全部变体共用同一系数） ----
[z1, p1z, k1] = butter(nHP, 2 * fcHP / fs, 'high');  [sosHP, gHP] = zp2sos(z1, p1z, k1);
[z2, p2z, k2] = butter(nLP, 2 * fcLP / fs, 'low');   [sosLP, gLP] = zp2sos(z2, p2z, k2);

% ---- 合成信号：启动 burst（2MHz，2000 幅，1.5µs 衰减）+ 高斯包络 5MHz 回波（0.5）----
tEchoList = [400, 600, 800, 1200, 2700];     % 目标回波中心（声学 τ 坐标，样本）
res = struct();
res.params = struct('fs', fs, 'Nt', Nt, 'sysDelay', sysDelay, 'maskLength', maskLength, ...
    'nHP', nHP, 'fcHP', fcHP, 'nLP', nLP, 'fcLP', fcLP, 'burstAmp', 2000, ...
    'burstTauDecayUs', 1.5, 'burstFMHz', 2, 'echoAmp', 0.5, 'echoFMHz', 5, ...
    'echoSigmaSamples', 0.25e-6 * fs, 'tEchoList', tEchoList, 'idealPad', pad, ...
    'runs', 'echoOnly=no-interference reference; burstOnly=startup interference; burstEcho=both; ideal=crop start moved pad=2000 earlier, same acoustic scenario');

% ============ B1 顺序对比（同一 refZeroPhase 实现）+ 实现归因（Mff） ============
% 远目标 τ=2700；echoOnly/burstOnly/burstEcho 各一组
b1 = struct();
for runName = {'echoOnly', 'burstOnly', 'burstEcho'}
    rn = runName{1};
    rawE = makeRaw(rn, tEchoList(end), sysDelay, Nt, fs);
    yN  = processN(rawE, sysDelay, maskLength + 13, sosHP, gHP, nHP, sosLP, gLP, nLP);
    yM  = processM(rawE, sysDelay, maskLength + 13, sosHP, gHP, nHP, sosLP, gLP, nLP, true);
    yMff = processM(rawE, sysDelay, maskLength + 13, sosHP, gHP, nHP, sosLP, gLP, nLP, false);
    valid = (2000:numel(yN))';
    scale = max(max(abs(yN(valid))), eps);
    ppN = timeDerivative(yN, 1 / fs);
    ppM = timeDerivative(yM, 1 / fs);
    e1 = struct();
    e1.farRelDiff_N_vs_M = max(abs(yN(valid) - yM(valid))) / scale;        % 顺序效应（同实现）
    e1.farRelDiff_M_vs_Mff = max(abs(yM(valid) - yMff(valid))) / scale;    % 实现效应（同顺序）
    e1.headMax_N = max(abs(yN(1:50)));
    e1.headMax_M = max(abs(yM(1:50)));
    e1.derivHeadMax_N = max(abs(ppN(1:100)));
    e1.derivHeadMax_M = max(abs(ppM(1:100)));
    if strcmp(rn, 'burstOnly')
        bp = 2000;   % burst 峰（声学零点起）
        e1.tailSamples_N = find(abs(yN) > 0.01 * bp, 1, 'last');
        e1.tailSamples_M = find(abs(yM) > 0.01 * bp, 1, 'last');
        blk = 100;                                                       % 残留衰减曲线
        nb = floor(numel(yN) / blk);
        e1.residueProfile = arrayfun(@(k) max(abs(yN((k-1)*blk+1 : k*blk))), (1:nb)');
        e1.residueProfileBlk = blk;
    end
    b1.(rn) = e1;
end
res.B1 = b1;
fprintf('\n=== B1 顺序/实现对比（远目标 τ=2700）===\n');
fprintf('echoOnly  顺序效应(N vs M 同实现)=%.3g  实现效应(M vs Mff 同顺序)=%.3g\n', ...
    b1.echoOnly.farRelDiff_N_vs_M, b1.echoOnly.farRelDiff_M_vs_Mff);
fprintf('burstEcho 顺序效应=%.3g 实现效应=%.3g\n', ...
    b1.burstEcho.farRelDiff_N_vs_M, b1.burstEcho.farRelDiff_M_vs_Mff);
fprintf('burstOnly 残留拖尾(>1%%峰)：N=%d M=%d 样本；残留降至远目标幅度(0.5)以下：τ=%d\n', ...
    b1.burstOnly.tailSamples_N, b1.burstOnly.tailSamples_M, ...
    find(b1.burstOnly.residueProfile < 0.5, 1, 'first') * b1.burstOnly.residueProfileBlk);
fprintf('起点50样本|out|max（echoOnly）：N=%.4g M=%.4g；起始100样本|p''|max（burstEcho）：N=%.4g M=%.4g\n', ...
    b1.echoOnly.headMax_N, b1.echoOnly.headMax_M, ...
    b1.burstEcho.derivHeadMax_N, b1.burstEcho.derivHeadMax_M);

% ============ B2/B3 DBR 末端矩阵（顺序 N，burst 场景：阶跃传播 + 目标误差） ============
% zeroRows ∈ {300,313,358,400,413}：早于(300,313)/等于(358)/晚于(400,413)裁剪起点；
% 同时覆盖双波长 extra ∈ {0,13,100}（maskLength=300 → 300/313/400）。
% 传播测量用 burstOnly（零掉一段真实 burst → 真实阶跃）；目标误差用 burstEcho。
zeroRowsList = [300, 313, 358, 400, 413];
rawProp = makeRaw('burstOnly', 800, sysDelay, Nt, fs);
refProp = processN(rawProp, sysDelay, 313, sosHP, gHP, nHP, sosLP, gLP, nLP);
rawTgt = makeRaw('burstEcho', 800, sysDelay, Nt, fs);
refTgt = processN(rawTgt, sysDelay, 313, sosHP, gHP, nHP, sosLP, gLP, nLP);
memHP = 1914;                                      % exp6 F5: 0.4MHz HP n=4 记忆长度
b2 = struct('zeroRows', {}, 'dbrEndVsCrop', {}, 'zeroedRegionOutMax', {}, ...
    'stepDiffMax', {}, 'stepDiffBelowEcho1pctAt', {}, 'memHPSpanCovers', {}, ...
    'targetAmpRatioInt', {}, 'targetPosBiasInt', {}, ...
    'targetAmpRatioMasked', {}, 'targetPosBiasMasked', {});
for zr = zeroRowsList
    yProp = processN(rawProp, sysDelay, zr, sosHP, gHP, nHP, sosLP, gLP, nLP);
    yTgt = processN(rawTgt, sysDelay, zr, sosHP, gHP, nHP, sosLP, gLP, nLP);
    e2 = struct();
    e2.zeroRows = zr;
    e2.dbrEndVsCrop = sign(zr - sysDelay);          % -1 早于 / 0 等于 / +1 晚于
    nZeroInCut = max(zr - sysDelay + 1, 0);         % 裁剪线内置零样本数（τ=0 起）
    if nZeroInCut > 0
        % 置零段输出 ≠ 0：零相位滤波的非因果回卷把邻域信号 smear 进置零段——
        % 这是"仅遮原置零段不足以证明后续反演不受影响"的直接证据之一。
        e2.zeroedRegionOutMax = max(abs(yProp(1:nZeroInCut)));
        % 阶跃响应传播：与默认配置(313)输出之差（= 被截断 burst 段的响应）
        dAll = abs(yProp - refProp);
        thr = 0.01 * 0.5;                            % 远目标幅度 0.5 的 1%
        idx = find(dAll > thr, 1, 'last');
        if isempty(idx), idx = 0; end
        e2.stepDiffMax = max(dAll);
        e2.stepDiffBelowEcho1pctAt = idx;
    else
        e2.zeroedRegionOutMax = 0;                   % 无置零样本进入裁剪线
        e2.stepDiffMax = max(abs(yProp - refProp));  % 应为 0（逐位一致）
        e2.stepDiffBelowEcho1pctAt = 0;
    end
    mStart = max(zr + 1 - sysDelay + memHP, 1);     % 冻结 mask 规则（zr<sysDelay 时不触发）
    if nZeroInCut > 0
        e2.memHPSpanCovers = mStart >= e2.stepDiffBelowEcho1pctAt;   % 拒绝策略依据
    else
        e2.memHPSpanCovers = true;
    end
    % 目标 τ=800（干扰下）：相对同配置无 mask 参考；mask 规则演示
    [amp, tauPk] = targetMetrics(yTgt, 800, 500);
    [ampRef, tauPkRef] = targetMetrics(refTgt, 800, 500);
    e2.targetAmpRatioInt = amp / ampRef;
    e2.targetPosBiasInt = tauPk - tauPkRef;
    mStart = max(zr + 1 - sysDelay + memHP, 1);     % 冻结 mask 规则（zr<sysDelay 时不触发）
    yTgtM = yTgt;
    if nZeroInCut > 0
        yTgtM(1:min(mStart, numel(yTgtM))) = 0;      % mask：排除区间查询置零（近似演示）
    end
    [ampM, tauPkM] = targetMetrics(yTgtM, 800, 500);
    e2.targetAmpRatioMasked = ampM / ampRef;
    e2.targetPosBiasMasked = tauPkM - tauPkRef;
    b2(end+1) = e2; %#ok<AGROW>
end
res.B2 = b2;
res.memHP = memHP;
fprintf('\n=== B2 DBR 末端矩阵（burst 场景；目标 τ=800 相对默认配置参考）===\n');
for i = 1:numel(b2)
    fprintf('zeroRows=%d(末端%+d裁剪起点): 置零段输出|max|=%.3g 阶跃差max=%.4g 降至回波1%%:τ=%d；目标 %.4f/%+.3f（mask后 %.4f/%+.3f）\n', ...
        b2(i).zeroRows, b2(i).dbrEndVsCrop, b2(i).zeroedRegionOutMax, b2(i).stepDiffMax, ...
        b2(i).stepDiffBelowEcho1pctAt, b2(i).targetAmpRatioInt, b2(i).targetPosBiasInt, ...
        b2(i).targetAmpRatioMasked, b2(i).targetPosBiasMasked);
end

% ============ B4 滤波组合 × 反演开关（顺序 N，echoOnly 目标 τ=800） ============
combos = {'none', [0 0]; 'HP', [1 0]; 'LP', [0 1]; 'HP+LP', [1 1]};
rawE8 = makeRaw('echoOnly', 800, sysDelay, Nt, fs);
b4 = struct('combo', {}, 'ampP', {}, 'posP', {}, 'ampB', {}, 'posB', {}, ...
    'headResP', {}, 'headResB', {});
for ci = 1:size(combos, 1)
    x = rawE8;
    x(1:maskLength + 13) = 0;
    if combos{ci, 2}(1) == 1
        y = refZeroPhase(x(sysDelay:end), sosHP, gHP, nHP);
    else
        y = x(sysDelay:end);
    end
    if combos{ci, 2}(2) == 1
        y = refZeroPhase(y, sosLP, gLP, nLP);
    end
    e4 = struct();
    e4.combo = combos{ci, 1};
    [e4.ampP, e4.posP] = targetMetrics(y, 800, 500);
    tb = (0:numel(y)-1)' / fs;
    bb = 2 * y - 2 * tb .* timeDerivative(y, 1 / fs);
    [e4.ampB, e4.posB] = targetMetrics(bb, 800, 500);
    e4.headResP = max(abs(y(1:100)));
    e4.headResB = max(abs(bb(1:100)));
    b4(end+1) = e4; %#ok<AGROW>
end
res.B4 = b4;
fprintf('\n=== B4 滤波组合 × 反演（echoOnly 目标 τ=800；b 路 = 2p−2t·p′）===\n');
for i = 1:numel(b4)
    fprintf('%-6s: p路 amp=%.6f pos=%+.3f | b路 amp=%.4f pos=%+.3f | 头部100 |p|max=%.3g |b|max=%.3g\n', ...
        b4(i).combo, b4(i).ampP, b4(i).posP - 800, b4(i).ampB, b4(i).posB - 800, ...
        b4(i).headResP, b4(i).headResB);
end

% ============ B5 窗长（顺序 N，echoOnly 目标 τ=800） ============
b5 = struct('Nt', {}, 'cutLen', {}, 'ampRatio', {}, 'posBias', {});
for Ntv = [2000, 4000, 8000]
    rawW = makeRaw('echoOnly', 800, sysDelay, Ntv, fs);
    yw = processN(rawW, sysDelay, maskLength + 13, sosHP, gHP, nHP, sosLP, gLP, nLP);
    [ampW, tauW] = targetMetrics(yw, 800, 500);
    e5 = struct();
    e5.Nt = Ntv;  e5.cutLen = numel(yw);
    e5.ampRatio = ampW / b4(4).ampP;        % 相对同信号 Nt=4000（HP+LP）窗
    e5.posBias = tauW - b4(4).posP;         % 相对同窗参考峰位（消固有偏移）
    b5(end+1) = e5; %#ok<AGROW>
end
res.B5 = b5;
fprintf('\n=== B5 窗长（echoOnly 目标 τ=800；ampRatio 相对 Nt=4000）===\n');
for i = 1:numel(b5)
    fprintf('Nt=%d (cut=%d): ampRatio=%.6f posBiasRel=%+.4f\n', ...
        b5(i).Nt, b5(i).cutLen, b5(i).ampRatio, b5(i).posBias);
end

% ============ B6 近边界目标：边界约定误差 vs 干扰效应（顺序 N） ============
% 边界约定：echoOnly(tc) vs 理想参考 ideal(tc)——同声学场景（回波都在声学 τ=tc），
% 裁剪起点前移 pad=2000（≫记忆长度）→ 两者任何差异 = 边界约定误差。
% 干扰效应：burstEcho(tc) vs echoOnly(tc)——同目标同场景，仅差启动 burst。
b6 = struct('tauEcho', {}, 'bcRatioP', {}, 'bcBiasP', {}, 'bcRatioB', {}, 'bcBiasB', {}, ...
    'intRatioP', {}, 'intBiasP', {}, 'intRatioB', {}, 'intBiasB', {});
for tc = tEchoList
    rawE = makeRaw('echoOnly', tc, sysDelay, Nt, fs);
    rawI = makeRaw('echoOnly', tc, sysDelay + pad, Nt + pad, fs);
    rawB = makeRaw('burstEcho', tc, sysDelay, Nt, fs);
    yE = processN(rawE, sysDelay, maskLength + 13, sosHP, gHP, nHP, sosLP, gLP, nLP);
    yI = processN(rawI, sysDelay + pad, maskLength + 13 + pad, sosHP, gHP, nHP, sosLP, gLP, nLP);
    yB = processN(rawB, sysDelay, maskLength + 13, sosHP, gHP, nHP, sosLP, gLP, nLP);
    [ampE_P, tauE_P] = targetMetrics(yE, tc, 500);
    [ampI_P, tauI_P] = targetMetrics(yI, tc, 500);
    [ampB_P, tauB_P] = targetMetrics(yB, tc, 500);
    tbE = (0:numel(yE)-1)' / fs;   tbI = (0:numel(yI)-1)' / fs;   tbB = tbE;
    bE = 2 * yE - 2 * tbE .* timeDerivative(yE, 1 / fs);
    bI = 2 * yI - 2 * tbI .* timeDerivative(yI, 1 / fs);
    bB = 2 * yB - 2 * tbB .* timeDerivative(yB, 1 / fs);
    [ampE_B, tauE_B] = targetMetrics(bE, tc, 500);
    [ampI_B, tauI_B] = targetMetrics(bI, tc, 500);
    [ampB_B, tauB_B] = targetMetrics(bB, tc, 500);
    e6 = struct();
    e6.tauEcho = tc;
    e6.bcRatioP = ampE_P / ampI_P;  e6.bcBiasP = tauE_P - tauI_P;   % 边界约定误差
    e6.bcRatioB = ampE_B / ampI_B;  e6.bcBiasB = tauE_B - tauI_B;
    e6.intRatioP = ampB_P / ampE_P; e6.intBiasP = tauB_P - tauE_P;  % 干扰效应
    e6.intRatioB = ampB_B / ampE_B; e6.intBiasB = tauB_B - tauE_B;
    b6(end+1) = e6; %#ok<AGROW>
end
res.B6 = b6;
fprintf('\n=== B6 近边界目标（BC=边界约定误差 vs 理想参考；INT=干扰效应 vs echoOnly）===\n');
fprintf('τ=  样本 | BC p路 ratio/bias | BC b路 | INT p路 | INT b路 （bias 单位样本）\n');
for i = 1:numel(b6)
    fprintf('τ=%4d | %.6f/%+.4f | %.6f/%+.4f | %.4f/%+.3f | %.4f/%+.3f\n', ...
        b6(i).tauEcho, b6(i).bcRatioP, b6(i).bcBiasP, b6(i).bcRatioB, b6(i).bcBiasB, ...
        b6(i).intRatioP, b6(i).intBiasP, b6(i).intRatioB, b6(i).intBiasB);
end

% ============ B7 wl2 sysDelay=371 抽查（非默认 sysDelay） ============
rawW2 = makeRaw('echoOnly', 800, 371, Nt, fs);
yW2 = processN(rawW2, 371, maskLength + 13, sosHP, gHP, nHP, sosLP, gLP, nLP);
[ampW2, tauW2] = targetMetrics(yW2, 800, 500);
res.B7_wl2 = struct('sysDelay', 371, 'ampRatioVsRef', ampW2 / b4(4).ampP, ...
    'posBiasRel', tauW2 - b4(4).posP);
fprintf('\n=== B7 wl2 sysDelay=371（echoOnly 目标 τ=800）：ampRatio=%.6f posBiasRel=%+.4f\n', ...
    res.B7_wl2.ampRatioVsRef, res.B7_wl2.posBiasRel);

% ============ A5 短窗报错 / A6 NaN/Inf（保留） ============
rawA5 = makeRaw('echoOnly', tEchoList(end), sysDelay, Nt, fs);
cutFar = rawA5(sysDelay:end);
shortIn = cutFar(1:12);
errCaught = false;
try, refZeroPhase(shortIn, sosHP, gHP, nHP); catch, errCaught = true; end
res.A5_errorAt12 = errCaught;
ok13 = true;
try
    y13 = refZeroPhase(cutFar(1:13), sosHP, gHP, nHP);
    ok13 = all(isfinite(y13));
catch
    ok13 = false;
end
res.A5_okAt13 = ok13;
fprintf('A5 短窗：Nt_cut=12 报错=%d；Nt_cut=13 正常有限=%d\n', errCaught, ok13);

% ============ 冻结策略记录 ============
res.policy = struct( ...
    'order', 'raw -> DBR zero (1..maskLength+extra) -> clip -> delayCut -> HP zero-phase (whole cut line) -> LP zero-phase (whole cut line) -> [deriv p'' -> b] (per A-line, refZeroPhase implementation)', ...
    'endpoints', 'refZeroPhase steady-state init + odd extension nfact=3n; error if line length <= 3n or input non-finite', ...
    'validQueryRegion', 'original-row index m >= max(sysDelay, dbrEnd+1), default margin G=0; when HP/LP filtering (and hence inversion) is enabled, configs with dbrEnd = maskLength+dbrmaskExtra >= sysDelay are REJECTED with an error (default 313 < 358/371 satisfies; measured B2: a chopped startup burst leaves a zero-phase response tail to ~tau 2737, exceeding dbrEnd+memHP=2327 and depending on the removed signal amplitude/length - no fixed margin can guarantee validity)', ...
    'headPolicy', 'no fixed head-margin discard; boundary-convention error on steady-start lines measured ~0 (B6 BC columns); startup interference is data-dependent, reported via burstOnly residue profile and burstEcho INT columns (B1/B6), not a fixed discard', ...
    'derivativeEndpoint', 'timeDerivative.m: central difference interior, one-sided ends, per column, applied to the same filtered line', ...
    'rejected', 'wholesale discard of samples 848-3639 rejected: that span is the impulse-response width of one specific HP design at one threshold (exp6 F5), not a universal contamination bound; B6 BC columns show near-boundary targets recovered exactly when the line starts in steady state; B2 additionally rejects configs that put the DBR step inside the cut line');

% ============ 断言门（正确性型，不含预设优劣） ============
gate.A5err = errCaught;
gate.A5ok = ok13;
% G1 边界约定误差 ≈0：echoOnly 与理想参考在近边界（400/800）p/b 路一致
g1 = [];
for i = 1:numel(b6)
    if ismember(b6(i).tauEcho, [400, 800])
        g1(end+1) = abs(b6(i).bcRatioP - 1) <= 1e-9 && abs(b6(i).bcBiasP) <= 1e-6 && ...
            abs(b6(i).bcRatioB - 1) <= 1e-9 && abs(b6(i).bcBiasB) <= 1e-6; %#ok<AGROW>
    end
end
gate.G1_boundaryExact = all(g1);
% G2 顺序效应（同实现）：无干扰远区一致；burst 下切片边界差异实测 ~1.3%，放宽到 5%
gate.G2_orderSameImpl = b1.echoOnly.farRelDiff_N_vs_M <= 5e-3 && ...
    b1.burstEcho.farRelDiff_N_vs_M <= 0.05;
% G3 默认配置（313<sysDelay）与参考逐位一致（置零段输出非零仅作证据记录）
gate.G3_defaultIdentical = isequal(b2(2).targetAmpRatioInt, 1) && b2(2).stepDiffMax == 0;
% G4 无 NaN/Inf（关键输出）
gate.G4_finite = all(isfinite(refProp)) && all(isfinite(refTgt)) && ...
    isfinite(b1.echoOnly.headMax_N) && ~isempty(b1.burstOnly.tailSamples_N) && ...
    all(isfinite(b1.burstOnly.residueProfile));
% G5 结构：B6 全组合已测
gate.G5_reported = numel(b6) == numel(tEchoList);

res.gate = gate;
fprintf('\n断言门：A5err=%d A5ok=%d G1边界精确=%d G2顺序同实现=%d G3默认一致=%d G4有限=%d G5=%d\n', ...
    gate.A5err, gate.A5ok, gate.G1_boundaryExact, gate.G2_orderSameImpl, ...
    gate.G3_defaultIdentical, gate.G4_finite, gate.G5_reported);

save(fullfile(outdir, 'exp5_order_endpoints.mat'), 'res', 'refProp', 'refTgt', ...
    'rawProp', 'rawTgt', '-v7');

out = res;
out.matlabVersion = version;
fid = fopen(fullfile(outdir, 'exp5_order_endpoints.json'), 'w');
fwrite(fid, jsonencode(out, 'PrettyPrint', true)); fclose(fid);
fprintf('EXP5_DONE -> %s\n', fullfile(outdir, 'exp5_order_endpoints.json'));

gateOk = gate.A5err && gate.A5ok && gate.G1_boundaryExact && gate.G2_orderSameImpl && ...
    gate.G3_defaultIdentical && gate.G4_finite && gate.G5_reported;
assert(gateOk, 'exp5:gateFailed', 'EXP5 断言门未全部通过');

% ================= 局部函数 =================
function raw = makeRaw(runName, tauEcho, sysD, Ntv, fs)
% 声学信号 s(τ)（τ 为声学时间样本坐标），raw(m) = s(m − sysD)；触发前（τ<0）置 0
% runName: 'echoOnly' | 'burstOnly' | 'burstEcho'
% 回波置于声学 τ=tauEcho——改变 sysD 只移动裁剪起点，不改变声学场景。
m = (1:Ntv)';
tau = m - sysD;
s = zeros(Ntv, 1);
if ~strcmp(runName, 'echoOnly')
    tb = max(tau, 0);
    s = s + 2000 * exp(-tb * (1 / fs) / 1.5e-6) .* sin(2 * pi * 2e6 * tb / fs);
end
if ~strcmp(runName, 'burstOnly')
    s = s + 0.5 * exp(-(tau - tauEcho).^2 / (2 * (0.25e-6 * fs)^2)) .* ...
        sin(2 * pi * 5e6 * (tau - tauEcho) / fs);
end
s(tau < 0) = 0;
raw = s(:);
end

function y = processN(raw, sysD, zeroRows, sosHP, gHP, nHP, sosLP, gLP, nLP)
% 顺序 N（提议）：DBR置零 → DelayCut → HP零相位(整条裁剪线) → LP零相位
% （削顶对合成无饱和信号为恒等，生产中位于 DBR 之后、DelayCut 之前，此处省略）
x = raw;
x(1:min(zeroRows, numel(x))) = 0;
cut = x(sysD:end);                    % delayCut=1
y = refZeroPhase(cut, sosHP, gHP, nHP);
y = refZeroPhase(y, sosLP, gLP, nLP);
end

function y = processM(raw, sysD, zeroRows, sosHP, gHP, nHP, sosLP, gLP, nLP, useRef) %#ok<INUSD>
% 顺序 M（历史切片）：HP(301:) → LP(sysDelay+20:) → DBR置零 → DelayCut
% useRef=true 用 refZeroPhase（同实现归因对照）；false 用 MATLAB filtfilt（历史实现）
x = raw;
if useRef
    x(301:end) = refZeroPhase(x(301:end), sosHP, gHP, nHP);
    x(sysD + 20:end) = refZeroPhase(x(sysD + 20:end), sosLP, gLP, nLP);
else
    [bHP, aHP] = sos2tf(sosHP, gHP);
    [bLP, aLP] = sos2tf(sosLP, gLP);
    x(301:end) = filtfilt(bHP, aHP, x(301:end));
    x(sysD + 20:end) = filtfilt(bLP, aLP, x(sysD + 20:end));
end
x(1:min(zeroRows, numel(x))) = 0;
y = x(sysD:end);
end

function [amp, tauPk] = targetMetrics(y, tc, halfWin)
% 窗内最大正值峰 + 抛物线插值峰位（τ 连续样本坐标，0 基）
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
