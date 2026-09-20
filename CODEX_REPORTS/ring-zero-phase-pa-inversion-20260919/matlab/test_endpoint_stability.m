% TEST_ENDPOINT_STABILITY  阶段A收尾补测：起端/近尾报告区间的长参考稳定性（四轮任务 §7.A）。
%
% 背景（本任务指出的证据缺口；review-final-closure.md 初版相关段落已就地撤回）：
%   C1 的 pad 收敛重算发生在目标 τ=800 的中心区间 [300,1300]，S3n P1/P2 是
%   人工误差表对判据本身的正负例——两者都不是起端/尾端窗口的长参考稳定性
%   证据；起端 burst 与近尾 τ=3600 敏感性（S5/b6sens）实际只用 padRef=8000
%   单一长参考计算。本脚本只补这一个缺口：在两个端点场景的**实际报告区间**上，
%   用至少两个不同非参考 pad（2000/4000）与参考 pad 8000 检验**长参考本身**的
%   pad 稳定性（padConvergenceCheck 统一判据），并复算原敏感性数值，报告其在
%   稳定参考下是否改变。8000 是参考 pad，不是非参考候选。
%
% 坐标约定（0 基声学 τ = MATLAB 索引 − 1；裁剪线 τ=0 ↔ raw 一基样本 m=D）：
%   场景一 startBurst：burstOnly，tauEcho=800（burstOnly 无回波，tauEcho 不进入
%     信号；与 exp5 B6.sensitivity.startBurst 同参数）。原报告区间为 MATLAB 索引
%     1:1500，0 基声学坐标记为 τ=0..1499。
%   场景二 endEcho：echoOnly，tauEcho=3600（距裁剪线末端 43 样本）。原敏感性
%     实际计算区间为 MATLAB 3100:3643（i1=3600−500、i2=min(线末 3643, 4100)），
%     0 基 τ=3099..3642——上限被短窗线末截断，短窗不存在 τ>3642 的样本；不是
%     未截断的 [3100,4100]。
%
% 判据与延长策略：padConvergenceCheck（与 test_boundary_reference S3 / exp5 B6
%   同一唯一入口）：参考 pad 自比不计入、≥2 个非参考 pad 通过容差 1e-9、通过
%   pad 两两稳定（实际两两差、同一参考尺度）、无尾段回落。若 [2000,4000] 不足，
%   仅延长一次（候选 [4000,8000] vs 参考 16000）；仍不满足则如实报告 FAIL 与
%   对原敏感性数值可信度的影响，不放宽阈值、不以自比计入通过、不无限调参。
%
% 锚定复算：padRef=8000 行的敏感性（短窗 vs 8000 长参考）应逐位复现 77b1d37 已
%   发布值——startBurst |Δp|rel=0.31117044891792733（窗 τ=0..1499）、endEcho
%   |Δp|rel=0.63275363030561171 与 |Δb|rel=0.13554445421030059（窗 τ=3099..3642）
%   ——证明本补测与原敏感性计算同构。另报各 pad 参考下的敏感性数值及跨 pad
%   相对稳定度。注意：本测试检验的是**长参考稳定性**，不要求短窗与长窗误差小
%   或为零——起端 0.311/近尾 0.633 的有限窗边界效应是被测对象，不是要消除的
%   对象；短/长导数在窗边缘样本的端点约定差异（单侧 vs 中心差分，exp3 T7 既定
%   规则）属已知真实约定差异，随敏感性数值一并记录、不判定。
%
% 运行：matlab -batch "run('test_endpoint_stability.m')"   （工作目录 = matlab/）
% 输出：evidence/test_endpoint_stability.json；任一断言失败 error() → 非零退出。

outdir = fullfile(fileparts(mfilename('fullpath')), '..', 'evidence');
if ~exist(outdir, 'dir'), mkdir(outdir); end

fs = 250e6;
D = 358;  Nt = 4000;  dbrEnd = 313;            % 生产默认（wl1），与 exp5/S5 同参数
nHP = 4;  fcHP = 0.4e6;  nLP = 4;  fcLP = 40e6;
[z1, p1z, k1] = butter(nHP, 2 * fcHP / fs, 'high');  [sosHP, gHP] = zp2sos(z1, p1z, k1);
[z2, p2z, k2] = butter(nLP, 2 * fcLP / fs, 'low');   [sosLP, gLP] = zp2sos(z2, p2z, k2);

convTol = 1e-9;
padAttempt1 = [2000, 4000, 8000];         % 两个非参考候选 2000/4000 + 参考 pad 8000
padAttempt2 = [4000, 8000, 16000];        % 唯一一次延长（仅当第一次不足时运行）
attMat = {padAttempt1, padAttempt2};

% 77b1d37 已发布锚点（evidence/exp5_order_endpoints.json B6.sensitivity，本分支未改动）
anchorStartRelP = 0.31117044891792733;     % startBurst |Δp|rel（窗 τ=0..1499）
anchorEndRelP   = 0.63275363030561171;     % endEcho   |Δp|rel（窗 τ=3099..3642）
anchorEndRelB   = 0.13554445421030059;     % endEcho   |Δb|rel（同窗）

tVec = (0:Nt - D)' / fs;                   % 物理声学时间（与 exp5 tCut6 同构）

scnList = struct( ...
    'name',      {'startBurst', 'endEcho'}, ...
    'runName',   {'burstOnly', 'echoOnly'}, ...
    'tauEcho',   {800, 3600}, ...
    'winMatlab', {[1, 1500], [3100, 3643]}, ...
    'windowNote', { ...
        '原报告区间 MATLAB 索引 1:1500；0 基声学坐标 τ=0..1499', ...
        '原敏感性实际计算区间 MATLAB 3100:3643（i1=3600−500，i2=min(线末3643, 4100)）；0 基 τ=3099..3642，上限被短窗线末截断——不是未截断的 [3100,4100]'});

res = struct();
res.params = struct('fs', fs, 'sysDelayD', D, 'Nt', Nt, 'dbrEnd', dbrEnd, ...
    'nHP', nHP, 'fcHP', fcHP, 'nLP', nLP, 'fcLP', fcLP, ...
    'shortLen', Nt - D + 1, 'shortTauRange', [0, Nt - D], ...
    'convTolRel', convTol, ...
    'padAttempt1', padAttempt1, 'padAttempt2', padAttempt2, ...
    'anchorStartRelP', anchorStartRelP, 'anchorEndRelP', anchorEndRelP, ...
    'anchorEndRelB', anchorEndRelB, ...
    'runs', ['longReference pad stability on the two endpoint report windows; ', ...
    'criterion = padConvergenceCheck (shared entry with S3/exp5 B6); ', ...
    '8000 is the REFERENCE pad, not a non-reference candidate']);
pass = struct();

for scnIdx = 1:numel(scnList)
    sc = scnList(scnIdx);
    w1m = sc.winMatlab(1);  w2m = sc.winMatlab(2);
    fprintf('=== 场景 %s（%s，tauEcho=%d）：窗 MATLAB %d:%d = 0基 τ %d..%d ===\n', ...
        sc.name, sc.runName, sc.tauEcho, w1m, w2m, w1m - 1, w2m - 1);

    % ---- 短窗被测线（与 exp5 makeRaw/processN 逐位同构：DBR 置零 → 裁剪 → HP → LP）----
    rawS = ringAcousticModel(sc.runName, sc.tauEcho, (1:Nt)' - D, fs);
    xS = rawS;  xS(1:dbrEnd) = 0;  xS = xS(D:end);
    yS = refZeroPhase(xS, sosHP, gHP, nHP);
    yS = refZeroPhase(yS, sosLP, gLP, nLP);
    ppS = timeDerivative(yS, 1 / fs);
    bS = 2 * yS - 2 * tVec .* ppS;

    attempts = struct([]);
    for attIdx = 1:numel(attMat)
        padList = attMat{attIdx};
        padRef = padList(end);
        nPads = numel(padList);
        yPad = cell(1, nPads);  ppPad = cell(1, nPads);  bPad = cell(1, nPads);
        for padJ = 1:nPads
            padVal = padList(padJ);
            [~, yLf, ppLf, infoL] = longReference(sc.runName, sc.tauEcho, D, Nt, dbrEnd, ...
                padVal, padVal, sosHP, gHP, nHP, sosLP, gLP, nLP, fs);
            yPad{padJ} = yLf(infoL.extractIdx(1):infoL.extractIdx(2));
            ppPad{padJ} = ppLf(infoL.extractIdx(1):infoL.extractIdx(2));
            bPad{padJ} = 2 * yPad{padJ} - 2 * tVec .* ppPad{padJ};
        end
        iRef = find(padList == padRef, 1);
        yR = yPad{iRef};  ppR = ppPad{iRef};  bR = bPad{iRef};
        % 参考窗内幅度尺度（同一参考尺度，与 S3/exp5 B6 约定一致）
        sP = max(abs(yR(w1m:w2m)));  sPP = max(abs(ppR(w1m:w2m)));  sB = max(abs(bR(w1m:w2m)));
        relP = zeros(1, nPads);  relPP = zeros(1, nPads);  relB = zeros(1, nPads);
        absP = zeros(1, nPads);  absPP = zeros(1, nPads);  absB = zeros(1, nPads);
        for padJ = 1:nPads
            absP(padJ)  = max(abs(yPad{padJ}(w1m:w2m) - yR(w1m:w2m)));
            absPP(padJ) = max(abs(ppPad{padJ}(w1m:w2m) - ppR(w1m:w2m)));
            absB(padJ)  = max(abs(bPad{padJ}(w1m:w2m) - bR(w1m:w2m)));
            relP(padJ)  = absP(padJ) / sP;
            relPP(padJ) = absPP(padJ) / sPP;
            relB(padJ)  = absB(padJ) / sB;
        end
        % 实际两两差矩阵（同一参考尺度；与 S3 真实数据路径一致）
        pairRel = struct('diffP', zeros(nPads), 'diffPP', zeros(nPads), 'diffB', zeros(nPads));
        for iA = 1:nPads
            for iB = 1:iA - 1
                dPij  = max(abs(yPad{iA}(w1m:w2m) - yPad{iB}(w1m:w2m))) / sP;
                dPPij = max(abs(ppPad{iA}(w1m:w2m) - ppPad{iB}(w1m:w2m))) / sPP;
                dBij  = max(abs(bPad{iA}(w1m:w2m) - bPad{iB}(w1m:w2m))) / sB;
                pairRel.diffP(iA, iB) = dPij;   pairRel.diffP(iB, iA) = dPij;
                pairRel.diffPP(iA, iB) = dPPij; pairRel.diffPP(iB, iA) = dPPij;
                pairRel.diffB(iA, iB) = dBij;   pairRel.diffB(iB, iA) = dBij;
            end
        end
        [verdict, det] = padConvergenceCheck(padList, relP, relPP, relB, padRef, convTol, pairRel);
        % 两候选窗差（两个非参考 pad 结果在同一参考尺度下的实际差）
        candPair = struct();
        candPair.padA = padList(1);  candPair.padB = padList(2);
        candPair.diffP = pairRel.diffP(1, 2);
        candPair.diffPP = pairRel.diffPP(1, 2);
        candPair.diffB = pairRel.diffB(1, 2);
        % 敏感性数值（短窗 vs 各 pad 参考；pad=8000 行复现 77b1d37 已发布值）
        sensP = zeros(1, nPads);  sensPP = zeros(1, nPads);  sensB = zeros(1, nPads);
        for padJ = 1:nPads
            cP  = max(abs(yPad{padJ}(w1m:w2m)));
            cPP = max(abs(ppPad{padJ}(w1m:w2m)));
            cB  = max(abs(bPad{padJ}(w1m:w2m)));
            sensP(padJ)  = max(abs(yS(w1m:w2m) - yPad{padJ}(w1m:w2m))) / cP;
            sensPP(padJ) = max(abs(ppS(w1m:w2m) - ppPad{padJ}(w1m:w2m))) / cPP;
            sensB(padJ)  = max(abs(bS(w1m:w2m) - bPad{padJ}(w1m:w2m))) / cB;
        end
        sprP  = (max(sensP) - min(sensP)) / max(sensP);
        sprPP = (max(sensPP) - min(sensPP)) / max(sensPP);
        sprB  = (max(sensB) - min(sensB)) / max(sensB);
        rec = struct();
        rec.padList = padList;  rec.padRef = padRef;
        rec.absP = absP;  rec.absPP = absPP;  rec.absB = absB;
        rec.scaleP = sP;  rec.scalePP = sPP;  rec.scaleB = sB;
        rec.relP = relP;  rec.relPP = relPP;  rec.relB = relB;
        rec.candPairDiff = candPair;
        rec.sensP = sensP;  rec.sensPP = sensPP;  rec.sensB = sensB;
        rec.sensSpreadP = sprP;  rec.sensSpreadPP = sprPP;  rec.sensSpreadB = sprB;
        rec.verdict = verdict;  rec.failReason = det.failReason;  rec.detail = det;
        if isempty(attempts)
            attempts = rec;
        else
            attempts(end + 1) = rec; %#ok<AGROW>
        end
        fprintf('  尝试%d pad=%s（参考=%d）：verdict=%s firstOk=%g nOk=%d maxPairRel=%.3g | 两候选窗差 p/p''/b=%.3g/%.3g/%.3g\n', ...
            attIdx, mat2str(padList), padRef, verdict, det.firstOkPad, det.nOk, ...
            det.maxPairRel, candPair.diffP, candPair.diffPP, candPair.diffB);
        fprintf('  敏感性（短窗 vs 各 pad 参考）|Δp|rel=[%s] 跨pad稳定度=%.3g\n', ...
            sprintf('%.4g ', sensP), sprP);
        if strcmp(verdict, 'PASS')
            break;
        end
    end

    % ---- 锚定复算（尝试1 恒含 pad=8000；该行 = 77b1d37 已发布敏感性数值）----
    a1 = attempts(1);
    iAnch = find(a1.padList == 8000, 1);
    anchRelP = a1.sensP(iAnch);
    anchRelB = a1.sensB(iAnch);
    anchErrP = NaN;  anchErrB = NaN;
    if strcmp(sc.name, 'startBurst')
        anchErrP = abs(anchRelP - anchorStartRelP) / anchorStartRelP;
        passAnchor = anchErrP <= 1e-9;
        fprintf('  锚定：|Δp|rel(8000行)=%.17g vs 已发布 %.17g → 相对差 %.3g（%d）\n', ...
            anchRelP, anchorStartRelP, anchErrP, passAnchor);
    else
        anchErrP = abs(anchRelP - anchorEndRelP) / anchorEndRelP;
        anchErrB = abs(anchRelB - anchorEndRelB) / anchorEndRelB;
        passAnchor = anchErrP <= 1e-9 && anchErrB <= 1e-9;
        fprintf('  锚定：|Δp|rel=%.17g vs %.17g（%.3g）；|Δb|rel=%.17g vs %.17g（%.3g）→ %d\n', ...
            anchRelP, anchorEndRelP, anchErrP, anchRelB, anchorEndRelB, anchErrB, passAnchor);
    end

    finalAtt = attempts(end);
    scenRes = struct();
    scenRes.name = sc.name;  scenRes.runName = sc.runName;  scenRes.tauEcho = sc.tauEcho;
    scenRes.winMatlabIdx = [w1m, w2m];
    scenRes.winTau0Based = [w1m - 1, w2m - 1];
    scenRes.windowNote = sc.windowNote;
    scenRes.shortLen = numel(yS);
    scenRes.attemptsUsed = numel(attempts);
    scenRes.attempts = attempts;
    scenRes.finalVerdict = finalAtt.verdict;
    scenRes.finalSensSpreadP = finalAtt.sensSpreadP;
    scenRes.finalSensSpreadPP = finalAtt.sensSpreadPP;
    scenRes.finalSensSpreadB = finalAtt.sensSpreadB;
    scenRes.finalCandPairDiff = finalAtt.candPairDiff;
    scenRes.anchorRelP = anchRelP;   scenRes.anchorErrRelP = anchErrP;
    scenRes.anchorRelB = anchRelB;  scenRes.anchorErrRelB = anchErrB;
    scenRes.anchorPass = passAnchor;
    if scnIdx == 1
        res.scenarios = scenRes;
    else
        res.scenarios(scnIdx) = scenRes;
    end

    if strcmp(sc.name, 'startBurst')
        pass.E1_startRefStable = strcmp(finalAtt.verdict, 'PASS');
        pass.E3_anchorStartReproduced = passAnchor;
        pass.E5_startSensStable = finalAtt.sensSpreadP <= 1e-6 && ...
            finalAtt.sensSpreadPP <= 1e-6 && finalAtt.sensSpreadB <= 1e-6;
    else
        pass.E2_endRefStable = strcmp(finalAtt.verdict, 'PASS');
        pass.E4_anchorEndReproduced = passAnchor;
        pass.E6_endSensStable = finalAtt.sensSpreadP <= 1e-6 && ...
            finalAtt.sensSpreadPP <= 1e-6 && finalAtt.sensSpreadB <= 1e-6;
    end
end

% ---- 汇总 ----
res.pass = pass;
res.matlabVersion = version;
fid = fopen(fullfile(outdir, 'test_endpoint_stability.json'), 'w');
fwrite(fid, jsonencode(res, 'PrettyPrint', true)); fclose(fid);
fprintf('TEST_ENDPOINT_STABILITY_DONE -> %s\n', ...
    fullfile(outdir, 'test_endpoint_stability.json'));

fnE = fieldnames(pass);
allOk = true;
for fIdx = 1:numel(fnE)
    fprintf('  %s = %d\n', fnE{fIdx}, pass.(fnE{fIdx}));
    allOk = allOk && pass.(fnE{fIdx});
end
assert(allOk, 'test_endpoint_stability:failed', ...
    '端点稳定性补测未全部通过（见 evidence/test_endpoint_stability.json）');
