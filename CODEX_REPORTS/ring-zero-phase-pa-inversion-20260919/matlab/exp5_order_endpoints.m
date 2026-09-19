% EXP5_ORDER_ENDPOINTS  判定性实验 5【审查整改版】：滤波顺序/DBR边界策略/端点约定。
%
% 二轮整改（TASKS/环形成像阶段A边界参考与时间轴规格收口_20260919-235753.md，B1）
% ----
% B6 参考重写：旧"理想参考" rawI = makeRaw(..., sysDelay+pad, Nt+pad) 经
%   processN 的 cut = x(sysD:end) 在滤波前把增加的前段裁掉，两路送入滤波器的
%   数组长度/内容/边界完全相同（独立复现 B6_filter_input_identical=1, maxdiff=0），
%   旧 G1_boundaryExact"边界约定误差精确 0"是自比伪影，**撤回**。
%   新构造（longReference.m）：同一声学信号 s(τ) 在扩展区间 [τ=−padL,
%   Nt−sysD+padR] 上整条滤波+求导后按物理时间提取 τ∈[0,Nt−sysD]——额外段在
%   滤波时真实存在，长参考含短窗输入为连续子段（逐位）；τ<0 段为"触发前无激励"
%   合成模型（实机可得性 UNVERIFIED）。新增：结构断言（输入范围/长度/声学端点/
%   包含关系）、旧构造负例（逐位相同→不得认定为独立参考）、递增 pad 收敛
%   （容差记录）、p/p′/b 波形误差（绝对+相对）、滤波组合与 wl2 覆盖、起端/终端
%   敏感性用例。起终端敏感性另见 test_boundary_reference.m（纳入统一入口）。
% G3 重定义：原"默认配置与自身参考逐位一致"是恒等断言（自比），改为有内容的
%   独立断言：dbrEnd=313 < sysDelay 时置零段完全在裁剪线之前，DBR 开/关的
%   裁剪线输出逐位一致（zeroRows=313 vs 0）。
%
% 一轮整改（9493962 → 9ace9e1，追加任务 R4/R5，保留）
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
%   头部处理: 不做固定裕量丢弃。边界约定误差以收敛独立长参考真实量化（B6 二轮
%     版；稳态起点线近边界目标 p/b 相对差异 ~1e-9 以下，burst 污染起点与近尾
%     目标存在可测的有限窗效应，见 B6.sensitivity）——不以"精确零误差"为验收，
%     启动干扰是数据相关量，以 burstOnly 残留曲线与 burstEcho INT 列报告。
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
padList = [250, 500, 1000, 2000, 4000, 8000];  % 长参考前后 pad 收敛序列（memHP=1914，二轮整改）
padRef = padList(end);           % 收敛长参考使用长度（≫HP 记忆长度）
dbrEnd6 = maskLength + 13;       % DBR 置零末端（默认 313 < sysDelay，置零全在裁剪起点之前）

% ---- SOS 设计（一次设计，全部变体共用同一系数） ----
[z1, p1z, k1] = butter(nHP, 2 * fcHP / fs, 'high');  [sosHP, gHP] = zp2sos(z1, p1z, k1);
[z2, p2z, k2] = butter(nLP, 2 * fcLP / fs, 'low');   [sosLP, gLP] = zp2sos(z2, p2z, k2);

% ---- 合成信号：启动 burst（2MHz，2000 幅，1.5µs 衰减）+ 高斯包络 5MHz 回波（0.5）----
tEchoList = [400, 600, 800, 1200, 2700];     % 目标回波中心（声学 τ 坐标，样本）
res = struct();
res.params = struct('fs', fs, 'Nt', Nt, 'sysDelay', sysDelay, 'maskLength', maskLength, ...
    'nHP', nHP, 'fcHP', fcHP, 'nLP', nLP, 'fcLP', fcLP, 'burstAmp', 2000, ...
    'burstTauDecayUs', 1.5, 'burstFMHz', 2, 'echoAmp', 0.5, 'echoFMHz', 5, ...
    'echoSigmaSamples', 0.25e-6 * fs, 'tEchoList', tEchoList, ...
    'refPadList', padList, 'refPadUsed', padRef, ...
    'runs', 'echoOnly=no-interference reference; burstOnly=startup interference; burstEcho=both; longReference=independent long-window boundary reference (filtered on extended input, then extracted by physical time; see longReference.m)');

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
refTgt0 = processN(rawTgt, sysDelay, 0, sosHP, gHP, nHP, sosLP, gLP, nLP);  % DBR 关（zeroRows=0）
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

% ============ B6 近边界目标：真实独立长参考（二轮整改 B1 重写，顺序 N） ============
% 参考 = longReference.m：同一声学信号 s(τ) 在扩展区间 [τ=−padL, Nt−sysD+padR]
% 上整条零相位滤波+求导，再按物理时间提取 τ∈[0,Nt−sysD]——额外段在滤波时真实
% 存在，不得在滤波前又裁掉（旧 9ace9e1 构造正是裁掉了，撤回）。
% BC（边界约定误差）= 被测短窗 vs 收敛长参考：绝对误差 + 相对误差（相对长参考
% 窗内幅度，尺度可靠），不预设零或小；INT（干扰效应）= burstEcho vs echoOnly
% （同目标同场景，仅差启动 burst；保留前轮定义）。
tCut6 = (0:Nt - sysDelay)' / fs;         % 两路共用物理时间（τ 坐标；声学原点 m=sysD 不随 pad 改变）
b6ref = struct();
b6ref.method = ['long-window reference: same acoustic signal s(tau) filtered with the SAME ', ...
    'refZeroPhase SOS chain on the extended input tau in [-padL, Nt-sysD+padR] (pre-trigger ', ...
    'tau<0 = excitation-free synthetic model, zeros; real-hardware availability UNVERIFIED), ', ...
    'derivative computed on the long line, then extracted for tau in [0, Nt-sysD] by physical ', ...
    'time (acoustic origin m=sysD unchanged, b uses the same physical t as the short line); ', ...
    'the extra segments are present during filtering - unlike the withdrawn 9ace9e1 ', ...
    'construction that shifted sysDelay and cropped them away first'];
b6ref.padList = padList;
b6ref.padUsed = padRef;
b6ref.convTolRel = 1e-9;                 % 收敛容差：相对长参考目标窗内最大幅度（记录值）
b6ref.dbrEnd = dbrEnd6;

% ---- 结构断言 + 旧构造负例（echoOnly τ=800 代表场景；亦作组合覆盖的公共输入）----
rawE8 = makeRaw('echoOnly', 800, sysDelay, Nt, fs);
xS6 = rawE8(sysDelay:end);               % 被测短窗滤波输入（生产 delayCut=1 语义）
[xL6, yLf6, ppLf6, infoL6] = longReference('echoOnly', 800, sysDelay, Nt, dbrEnd6, ...
    padRef, padRef, sosHP, gHP, nHP, sosLP, gLP, nLP, fs);
b6ref.shortFilterLen = numel(xS6);
b6ref.shortTauRange = [0, Nt - sysDelay];
b6ref.longFilterLen = infoL6.len;
b6ref.longTauRange = [infoL6.tauStart, infoL6.tauEnd];
b6ref.extraSamples = infoL6.len - numel(xS6);
b6ref.acousticOriginSample = infoL6.originSample;
b6ref.longContainsShortBitwise = ...
    isequal(xL6(infoL6.extractIdx(1):infoL6.extractIdx(1) + numel(xS6) - 1), xS6);
b6ref.oldConstructionPads = [2000, padRef];
b6ref.oldConstructionMaxDiff = [];
for padOld = b6ref.oldConstructionPads
    rawOld = makeRaw('echoOnly', 800, sysDelay + padOld, Nt + padOld, fs);
    xOld = rawOld(sysDelay + padOld:end);          % 旧 processN 的 cut（先平移 sysDelay 再裁剪）
    b6ref.oldConstructionMaxDiff(end+1) = max(abs(xOld - xS6)); %#ok<AGROW>
end

% ---- pad 收敛（echoOnly/burstOnly/burstEcho；目标 τ=800，窗 [300,1300]）----
tcConv = 800;  w1c = tcConv - 500;  w2c = tcConv + 500;
b6ref.convergence = struct();
for sc6 = {'echoOnly', 'burstOnly', 'burstEcho'}
    rn = sc6{1};
    [~, yRf, ppRf, infoR] = longReference(rn, tcConv, sysDelay, Nt, dbrEnd6, ...
        padRef, padRef, sosHP, gHP, nHP, sosLP, gLP, nLP, fs);
    yR = yRf(infoR.extractIdx(1):infoR.extractIdx(2));
    ppR = ppRf(infoR.extractIdx(1):infoR.extractIdx(2));
    bR = 2 * yR - 2 * tCut6 .* ppR;
    sPc = max(abs(yR(w1c:w2c)));  sPPc = max(abs(ppR(w1c:w2c)));  sBc = max(abs(bR(w1c:w2c)));
    relP = [];  relPP = [];  relB = [];
    for p6 = padList
        [~, yLp, ppLp, infoLp] = longReference(rn, tcConv, sysDelay, Nt, dbrEnd6, ...
            p6, p6, sosHP, gHP, nHP, sosLP, gLP, nLP, fs);
        yp = yLp(infoLp.extractIdx(1):infoLp.extractIdx(2));
        ppp = ppLp(infoLp.extractIdx(1):infoLp.extractIdx(2));
        bp = 2 * yp - 2 * tCut6 .* ppp;
        relP(end+1) = max(abs(yp(w1c:w2c) - yR(w1c:w2c))) / sPc; %#ok<AGROW>
        relPP(end+1) = max(abs(ppp(w1c:w2c) - ppR(w1c:w2c))) / sPPc; %#ok<AGROW>
        relB(end+1) = max(abs(bp(w1c:w2c) - bR(w1c:w2c))) / sBc; %#ok<AGROW>
    end
    okPads = padList(relP <= b6ref.convTolRel & relPP <= b6ref.convTolRel & relB <= b6ref.convTolRel);
    b6ref.convergence.(rn) = struct('padList', padList, 'relP', relP, 'relPP', relPP, ...
        'relB', relB, 'scaleP', sPc, 'scalePP', sPPc, 'scaleB', sBc, ...
        'convergedPad', min(okPads));
end

% ---- BC/INT 主表（tEchoList；BC vs 收敛长参考，INT vs echoOnly 短窗）----
b6 = struct('tauEcho', {}, 'bcRatioP', {}, 'bcBiasP', {}, 'bcRatioB', {}, 'bcBiasB', {}, ...
    'bcAbsMaxP', {}, 'bcAbsMaxPP', {}, 'bcAbsMaxB', {}, ...
    'bcRelMaxP', {}, 'bcRelMaxPP', {}, 'bcRelMaxB', {}, ...
    'intRatioP', {}, 'intBiasP', {}, 'intRatioB', {}, 'intBiasB', {});
for tc = tEchoList
    rawE = makeRaw('echoOnly', tc, sysDelay, Nt, fs);
    rawB = makeRaw('burstEcho', tc, sysDelay, Nt, fs);
    yE = processN(rawE, sysDelay, dbrEnd6, sosHP, gHP, nHP, sosLP, gLP, nLP);
    yB = processN(rawB, sysDelay, dbrEnd6, sosHP, gHP, nHP, sosLP, gLP, nLP);
    [~, yLft, ppLft, infoLt] = longReference('echoOnly', tc, sysDelay, Nt, dbrEnd6, ...
        padRef, padRef, sosHP, gHP, nHP, sosLP, gLP, nLP, fs);
    yI = yLft(infoLt.extractIdx(1):infoLt.extractIdx(2));
    ppI = ppLft(infoLt.extractIdx(1):infoLt.extractIdx(2));
    [ampE_P, tauE_P] = targetMetrics(yE, tc, 500);
    [ampI_P, tauI_P] = targetMetrics(yI, tc, 500);
    [ampB_P, tauB_P] = targetMetrics(yB, tc, 500);
    bE = 2 * yE - 2 * tCut6 .* timeDerivative(yE, 1 / fs);
    bI = 2 * yI - 2 * tCut6 .* ppI;        % 长参考导数在长线上计算，b 与短窗共用同一物理 t
    bB = 2 * yB - 2 * tCut6 .* timeDerivative(yB, 1 / fs);
    [ampE_B, tauE_B] = targetMetrics(bE, tc, 500);
    [ampI_B, tauI_B] = targetMetrics(bI, tc, 500);
    [ampB_B, tauB_B] = targetMetrics(bB, tc, 500);
    i1 = max(1, tc - 500);  i2 = min(numel(yE), tc + 500);
    e6 = struct();
    e6.tauEcho = tc;
    e6.bcRatioP = ampE_P / ampI_P;  e6.bcBiasP = tauE_P - tauI_P;   % 目标幅值比/位置偏差
    e6.bcRatioB = ampE_B / ampI_B;  e6.bcBiasB = tauE_B - tauI_B;
    e6.bcAbsMaxP = max(abs(yE(i1:i2) - yI(i1:i2)));
    e6.bcRelMaxP = e6.bcAbsMaxP / max(abs(yI(i1:i2)));
    ppE6 = timeDerivative(yE, 1 / fs);
    e6.bcAbsMaxPP = max(abs(ppE6(i1:i2) - ppI(i1:i2)));
    e6.bcRelMaxPP = e6.bcAbsMaxPP / max(abs(ppI(i1:i2)));
    e6.bcAbsMaxB = max(abs(bE(i1:i2) - bI(i1:i2)));
    e6.bcRelMaxB = e6.bcAbsMaxB / max(abs(bI(i1:i2)));
    e6.intRatioP = ampB_P / ampE_P; e6.intBiasP = tauB_P - tauE_P;  % 干扰效应
    e6.intRatioB = ampB_B / ampE_B; e6.intBiasB = tauB_B - tauE_B;
    b6(end+1) = e6; %#ok<AGROW>
end
res.B6 = struct('reference', b6ref, 'rows', b6, 'validQueryTauRange', ...
    [max(0, dbrEnd6 + 1 - sysDelay), Nt - sysDelay]);

% ---- 滤波组合覆盖（echoOnly τ=800；none 行应为逐位一致：无滤波则无边界效应）----
combos6 = {'none', [0 0]; 'HP', [1 0]; 'LP', [0 1]; 'HP+LP', [1 1]};
i0e6 = infoL6.extractIdx;
b6combos = struct('combo', {}, 'absMaxP', {}, 'relMaxP', {}, 'absMaxPP', {}, ...
    'relMaxPP', {}, 'absMaxPPInterior', {}, 'absMaxB', {}, 'relMaxB', {}, ...
    'absMaxBInterior', {});
for ci6 = 1:size(combos6, 1)
    xS8 = rawE8;  xS8(1:dbrEnd6) = 0;  xS8 = xS8(sysDelay:end);
    fl = combos6{ci6, 2};
    if fl(1) == 1
        yS8 = refZeroPhase(xS8, sosHP, gHP, nHP);  yL8 = refZeroPhase(xL6, sosHP, gHP, nHP);
    else
        yS8 = xS8;                                 yL8 = xL6;
    end
    if fl(2) == 1
        yS8 = refZeroPhase(yS8, sosLP, gLP, nLP);  yL8 = refZeroPhase(yL8, sosLP, gLP, nLP);
    end
    yL8e = yL8(i0e6(1):i0e6(2));
    ppS8 = timeDerivative(yS8, 1 / fs);  ppL8 = timeDerivative(yL8, 1 / fs);
    ppL8e = ppL8(i0e6(1):i0e6(2));
    bS8 = 2 * yS8 - 2 * tCut6 .* ppS8;  bL8 = 2 * yL8e - 2 * tCut6 .* ppL8e;
    ec = struct();  ec.combo = combos6{ci6, 1};
    ec.absMaxP = max(abs(yS8 - yL8e));  ec.relMaxP = ec.absMaxP / max(abs(yL8e));
    ec.absMaxPP = max(abs(ppS8 - ppL8e));  ec.relMaxPP = ec.absMaxPP / max(abs(ppL8e));
    % 端点单侧 vs 中心差分的约定差异集中在首/末样本（exp3 T7 线端探针同源），
    % 内部区单独统计：none 组合内部区必须逐位一致（无滤波→无边界效应）。
    ec.absMaxPPInterior = max(abs(ppS8(2:end-1) - ppL8e(2:end-1)));
    ec.absMaxB = max(abs(bS8 - bL8));  ec.relMaxB = ec.absMaxB / max(abs(bL8));
    ec.absMaxBInterior = max(abs(bS8(2:end-1) - bL8(2:end-1)));
    b6combos(end+1) = ec; %#ok<AGROW>
end
res.B6.combos = b6combos;                % 反演单独开启 = none 行的 b 路（无滤波反演）
res.B6.combosNote = ['p''/b endpoint values use one-sided differences on each line ', ...
    '(timeDerivative convention); the extracted long line has central differences at the ', ...
    'same raw samples, so full-line p''/b diffs include a real endpoint-convention ', ...
    'difference at the first/last sample (see exp3 T7 endpointProbe). Interior values ', ...
    'exclude this convention term; filtered combos additionally carry ~1e-13 relative ', ...
    'double-precision rounding accumulation of the longer cascade (~sqrt(N)*eps*|signal|).'];

% ---- wl2 非默认 sysDelay=371（echoOnly τ=800）----
Dwl2 = 371;
rawW26 = makeRaw('echoOnly', 800, Dwl2, Nt, fs);
yW2p = processN(rawW26, Dwl2, dbrEnd6, sosHP, gHP, nHP, sosLP, gLP, nLP);
[~, yLf2, ppLf2, infoL2] = longReference('echoOnly', 800, Dwl2, Nt, dbrEnd6, ...
    padRef, padRef, sosHP, gHP, nHP, sosLP, gLP, nLP, fs);
yI2 = yLf2(infoL2.extractIdx(1):infoL2.extractIdx(2));
ppI2 = ppLf2(infoL2.extractIdx(1):infoL2.extractIdx(2));
tW2 = (0:Nt - Dwl2)' / fs;
ppW2 = timeDerivative(yW2p, 1 / fs);
bW2 = 2 * yW2p - 2 * tW2 .* ppW2;  bI2 = 2 * yI2 - 2 * tW2 .* ppI2;
res.B6.wl2 = struct('sysDelay', Dwl2, ...
    'absMaxP', max(abs(yW2p - yI2)), 'relMaxP', max(abs(yW2p - yI2)) / max(abs(yI2)), ...
    'absMaxPP', max(abs(ppW2 - ppI2)), 'relMaxPP', max(abs(ppW2 - ppI2)) / max(abs(ppI2)), ...
    'absMaxB', max(abs(bW2 - bI2)), 'relMaxB', max(abs(bW2 - bI2)) / max(abs(bI2)));

% ---- 起端/终端敏感性用例（能测出有限窗边界效应的非平凡输入；只报告不判优劣）----
[xLb, yLfb, ~, infoLb] = longReference('burstOnly', 800, sysDelay, Nt, dbrEnd6, ...
    padRef, padRef, sosHP, gHP, nHP, sosLP, gLP, nLP, fs);
yLbE = yLfb(infoLb.extractIdx(1):infoLb.extractIdx(2));
rawBon = makeRaw('burstOnly', 800, sysDelay, Nt, fs);
yBon = processN(rawBon, sysDelay, dbrEnd6, sosHP, gHP, nHP, sosLP, gLP, nLP);
wSn = 1:min(1500, numel(yBon));
b6sens.startBurst = struct('scenario', 'burstOnly: burst onset sits at the crop line start; short-line odd extension mirrors the rising burst vs genuine pre-trigger silence in the reference', ...
    'window', [wSn(1) - 1, wSn(end) - 1], ...
    'absMaxP', max(abs(yBon(wSn) - yLbE(wSn))), 'scaleP', max(abs(yLbE(wSn))), ...
    'relMaxP', max(abs(yBon(wSn) - yLbE(wSn))) / max(abs(yLbE(wSn))));
tcEnd = 3600;                            % 距裁剪线末端 43 样本，高斯尾 ~0.8 幅度仍在
rawEnd6 = makeRaw('echoOnly', tcEnd, sysDelay, Nt, fs);
yEnd6 = processN(rawEnd6, sysDelay, dbrEnd6, sosHP, gHP, nHP, sosLP, gLP, nLP);
[~, yLfe, ppLfe, infoLe] = longReference('echoOnly', tcEnd, sysDelay, Nt, dbrEnd6, ...
    padRef, padRef, sosHP, gHP, nHP, sosLP, gLP, nLP, fs);
yLeE = yLfe(infoLe.extractIdx(1):infoLe.extractIdx(2));
ppLeE = ppLfe(infoLe.extractIdx(1):infoLe.extractIdx(2));
[ampEndS, tauEndS] = targetMetrics(yEnd6, tcEnd, 500);
[ampEndR, tauEndR] = targetMetrics(yLeE, tcEnd, 500);
bEndS = 2 * yEnd6 - 2 * tCut6 .* timeDerivative(yEnd6, 1 / fs);
bEndR = 2 * yLeE - 2 * tCut6 .* ppLeE;
i1e = max(1, tcEnd - 500);  i2e = min(numel(yEnd6), tcEnd + 500);
b6sens.endEcho = struct('scenario', 'echoOnly tau=3600, 43 samples from the cut-line end; short-line tail odd extension mirrors the echo vs genuine continuation in the reference', ...
    'tauEcho', tcEnd, 'window', [i1e - 1, i2e - 1], ...
    'ampRatio', ampEndS / ampEndR, 'posBias', tauEndS - tauEndR, ...
    'absMaxP', max(abs(yEnd6(i1e:i2e) - yLeE(i1e:i2e))), ...
    'relMaxP', max(abs(yEnd6(i1e:i2e) - yLeE(i1e:i2e))) / max(abs(yLeE(i1e:i2e))), ...
    'absMaxB', max(abs(bEndS(i1e:i2e) - bEndR(i1e:i2e))), ...
    'relMaxB', max(abs(bEndS(i1e:i2e) - bEndR(i1e:i2e))) / max(abs(bEndR(i1e:i2e))));
res.B6.sensitivity = b6sens;

fprintf('\n=== B6 近边界目标（BC=边界约定误差 vs 收敛长参考 pad=%d；INT=干扰效应 vs echoOnly）===\n', padRef);
fprintf('τ=  样本 | BC p ratio/bias | BC b ratio/bias | BC |Δp|rel |Δp′|rel |Δb|rel | INT p ratio/bias | INT b ratio/bias\n');
for i = 1:numel(b6)
    fprintf('τ=%4d | %.8f/%+.4f | %.8f/%+.4f | %.3g/%.3g/%.3g | %.4f/%+.3f | %.4f/%+.3f\n', ...
        b6(i).tauEcho, b6(i).bcRatioP, b6(i).bcBiasP, b6(i).bcRatioB, b6(i).bcBiasB, ...
        b6(i).bcRelMaxP, b6(i).bcRelMaxPP, b6(i).bcRelMaxB, ...
        b6(i).intRatioP, b6(i).intBiasP, b6(i).intRatioB, b6(i).intBiasB);
end
fprintf('结构：长滤波输入 %d 样本（短窗 %d + %d），长参考 τ∈[%d,%d] vs 短窗 τ∈[%d,%d]，同声学原点 m=%d；短窗输入逐位包含=%d\n', ...
    b6ref.longFilterLen, b6ref.shortFilterLen, b6ref.extraSamples, ...
    b6ref.longTauRange(1), b6ref.longTauRange(2), b6ref.shortTauRange(1), b6ref.shortTauRange(2), ...
    b6ref.acousticOriginSample, b6ref.longContainsShortBitwise);
fprintf('旧构造负例（先平移再裁剪）与短窗滤波输入最大差：pad=2000→%.3g pad=%d→%.3g（必须为 0 → 自比，非独立参考）\n', ...
    b6ref.oldConstructionMaxDiff(1), padRef, b6ref.oldConstructionMaxDiff(2));
fprintf('pad 收敛（容差 %.0e）：echoOnly→pad%d burstOnly→pad%d burstEcho→pad%d\n', ...
    b6ref.convTolRel, b6ref.convergence.echoOnly.convergedPad, ...
    b6ref.convergence.burstOnly.convergedPad, b6ref.convergence.burstEcho.convergedPad);
fprintf('组合覆盖（echoOnly τ=800 整线 |Δ|rel）：none=%.3g HP=%.3g LP=%.3g HP+LP=%.3g（p 路；none 必须为 0）\n', ...
    b6combos(1).relMaxP, b6combos(2).relMaxP, b6combos(3).relMaxP, b6combos(4).relMaxP);
fprintf('wl2 D=371：|Δp|rel=%.3g |Δp′|rel=%.3g |Δb|rel=%.3g\n', ...
    res.B6.wl2.relMaxP, res.B6.wl2.relMaxPP, res.B6.wl2.relMaxB);
fprintf('敏感性：起端 burstOnly |Δp|rel=%.3g（窗 [0,1500]）；终端 echo τ=3600 ampRatio=%.8f posBias=%+.4f |Δp|rel=%.3g |Δb|rel=%.3g\n', ...
    b6sens.startBurst.relMaxP, b6sens.endEcho.ampRatio, b6sens.endEcho.posBias, ...
    b6sens.endEcho.relMaxP, b6sens.endEcho.relMaxB);

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

% ============ 冻结策略记录（二轮整改同步） ============
res.policy = struct( ...
    'order', 'raw -> DBR zero (1..maskLength+extra) -> clip -> delayCut -> HP zero-phase (whole cut line) -> LP zero-phase (whole cut line) -> [deriv p'' -> b] (per A-line, refZeroPhase implementation)', ...
    'endpoints', 'refZeroPhase steady-state init + odd extension nfact=3n; error if line length <= 3n or input non-finite', ...
    'validQueryRegion', 'original-row index m >= max(sysDelay, dbrEnd+1), default margin G=0; when HP or LP zero-phase filtering is enabled (with or without inversion), configs with dbrEnd = maskLength+dbrmaskExtra >= sysDelay are REJECTED with an error (default 313 < 358/371 satisfies; measured B2: a chopped startup burst leaves a zero-phase response tail to ~tau 2737, exceeding dbrEnd+memHP=2327 and depending on the removed signal amplitude/length - no fixed margin can guarantee validity). DBR disabled: dbrEnd interpreted as 0 (no zeroed region, no step; evidence: cut-line output bitwise identical for zeroRows=313 vs 0, gate G3). Inversion-only (no HP/LP): no non-causal filter wrap occurs, so the dbrEnd<sysDelay rejection is not required there; the valid-query rule m >= dbrEnd+1 still excludes the zeroed prefix. delayCut=0 with DBR enabled: the zeroed prefix IS inside the filter input by construction (no crop happens) - the "step outside the cut line" rationale does NOT transfer to the uncut line; filtering + delayCut=0 + DBR enabled is treated as unsupported pending an explicit stage-B decision. Short window: line length <= 3n rejected (A5). Out-of-range queries: maskOob zero (production convention). Endpoint derivative: one-sided differences at line ends (timeDerivative.m); the cut-line one-sided vs uncut central-difference difference at the first sample is quantified in exp3 T7 endpointProbe', ...
    'headPolicy', 'no fixed head-margin discard and no preset quality threshold; boundary-convention error is quantified against a converged INDEPENDENT long-window reference (B6.reference/rows: structural assertions, old self-comparison construction negative-controlled, pad convergence at recorded tolerance 1e-9 relative, p/p''/b waveform errors with absolute+relative scales). Steady-start (pre-trigger-zero model) lines: near-boundary echoOnly differences at ~1e-9 relative or below; burst-contaminated starts and near-tail targets show measurable finite-window effects (B6.sensitivity) - no exact-zero-error claim is made. Startup interference remains a data-dependent quantity reported via burstOnly residue profile and burstEcho INT columns (B1/B6), not a fixed discard', ...
    'derivativeEndpoint', 'timeDerivative.m: central difference interior, one-sided ends, per column, applied to the same filtered line', ...
    'twoStateQuery', 'unified notation (algorithm-stage-A.md section 2.2): D systemDelay (raw one-based acoustic zero), T physical travel time, s=fs*T, m raw one-based coordinate, q stored-array zero-based continuous query coordinate; cut line q_cut=s (m=q_cut+D); uncut calibrated query q_raw=s+D-1 (m=q_raw+1=s+D); inversion time multiplier t=T=s/fs=(q_raw+1-D)/fs; the legacy DAS uncut query q_legacy=s (no compensation, "sample 1 = acoustic zero") is a separate historical compatibility behavior, documented separately and never mixed into the proposed inversion table', ...
    'rejected', ['wholesale discard of samples 848-3639 rejected: that span is the impulse-response width of one specific HP design at one threshold (exp6 F5), not a universal contamination bound. ', ...
        'WITHDRAWN this round: the 9ace9e1 claim "boundary-convention error measured exactly 0 (B6 BC columns)" - that reference degraded to the same filter input as the device under test (B6_filter_input_identical=1, maxdiff=0). Boundary conclusions now come exclusively from the corrected independent long reference; no exact-zero claim is made']);

% ============ 断言门（正确性型，不含预设优劣；结构性/收敛性断言容差随 JSON 记录） ============
gate.A5err = errCaught;
gate.A5ok = ok13;
% G1 独立参考结构：长输入更长且额外段滤波时存在；含短窗输入逐位；同声学原点；
%    旧"先平移再裁剪"构造与短窗滤波输入逐位相同 → 被识别为自比，无效参考
gate.G1_refIndependent = isequal(b6ref.extraSamples, 2 * padRef) && ...
    b6ref.longContainsShortBitwise && ...
    b6ref.longTauRange(1) < 0 && b6ref.shortTauRange(1) == 0 && ...
    b6ref.longTauRange(2) > b6ref.shortTauRange(2) && ...
    infoL6.originSample == sysDelay && ...
    all(b6ref.oldConstructionMaxDiff == 0);
% G1b 参考收敛：三场景均在 padList 内达到记录容差（相对窗内幅度 1e-9）
convPads = [b6ref.convergence.echoOnly.convergedPad, ...
    b6ref.convergence.burstOnly.convergedPad, b6ref.convergence.burstEcho.convergedPad];
gate.G1b_refConverged = all(isfinite(convPads)) && all(convPads <= padRef);
% G2 顺序效应（同实现）：无干扰远区一致；burst 下切片边界差异实测 ~1.3%，放宽到 5%
gate.G2_orderSameImpl = b1.echoOnly.farRelDiff_N_vs_M <= 5e-3 && ...
    b1.burstEcho.farRelDiff_N_vs_M <= 0.05;
% G3 DBR 置零段完全在裁剪起点之前 → DBR 开/关的裁剪线逐位一致（zeroRows=313 vs 0；
%    旧 G3"默认配置与自身参考一致"是恒等自比，二轮整改重定义为独立断言）
gate.G3_dbrBeforeCropNoop = isequal(refTgt, refTgt0);
% G4 无 NaN/Inf（关键输出）
gate.G4_finite = all(isfinite(refProp)) && all(isfinite(refTgt)) && ...
    isfinite(b1.echoOnly.headMax_N) && ~isempty(b1.burstOnly.tailSamples_N) && ...
    all(isfinite(b1.burstOnly.residueProfile)) && ...
    all(isfinite([b6.bcAbsMaxP])) && all(isfinite([b6.bcRelMaxB])) && ...
    all(isfinite([b6combos.absMaxB])) && isfinite(res.B6.wl2.absMaxB) && ...
    isfinite(b6sens.startBurst.relMaxP) && isfinite(b6sens.endEcho.relMaxB);
% G5 结构：B6 全 τ 已测 + 组合/wl2/敏感性覆盖 + 无滤波组合一致
% （p 与 p′/b 内部区逐位一致；端点 p′/b 的单侧 vs 中心差分为真实约定差异，见 T7）
gate.G5_reported = numel(b6) == numel(tEchoList) && numel(b6combos) == 4 && ...
    b6combos(1).absMaxP == 0 && b6combos(1).absMaxPPInterior == 0 && ...
    b6combos(1).absMaxBInterior == 0 && b6ref.extraSamples == 2 * padRef;

res.gate = gate;
fprintf('\n断言门：A5err=%d A5ok=%d G1独立参考结构=%d G1b参考收敛=%d G2顺序同实现=%d G3=DBR前置无操作=%d G4有限=%d G5=%d\n', ...
    gate.A5err, gate.A5ok, gate.G1_refIndependent, gate.G1b_refConverged, ...
    gate.G2_orderSameImpl, gate.G3_dbrBeforeCropNoop, gate.G4_finite, gate.G5_reported);

save(fullfile(outdir, 'exp5_order_endpoints.mat'), 'res', 'refProp', 'refTgt', ...
    'rawProp', 'rawTgt', '-v7');

out = res;
out.matlabVersion = version;
fid = fopen(fullfile(outdir, 'exp5_order_endpoints.json'), 'w');
fwrite(fid, jsonencode(out, 'PrettyPrint', true)); fclose(fid);
fprintf('EXP5_DONE -> %s\n', fullfile(outdir, 'exp5_order_endpoints.json'));

gateOk = gate.A5err && gate.A5ok && gate.G1_refIndependent && gate.G1b_refConverged && ...
    gate.G2_orderSameImpl && gate.G3_dbrBeforeCropNoop && gate.G4_finite && gate.G5_reported;
assert(gateOk, 'exp5:gateFailed', 'EXP5 断言门未全部通过');

% ================= 局部函数 =================
function raw = makeRaw(runName, tauEcho, sysD, Ntv, fs)
% 声学信号 s(τ)（τ 为声学时间样本坐标），raw(m) = s(m − sysD)；触发前（τ<0）置 0
% runName: 'echoOnly' | 'burstOnly' | 'burstEcho'
% 回波置于声学 τ=tauEcho——改变 sysD 只移动裁剪起点，不改变声学场景。
% 信号模型原样抽出至 ringAcousticModel.m（二轮整改），与长窗参考共用。
m = (1:Ntv)';
raw = ringAcousticModel(runName, tauEcho, m - sysD, fs);
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
