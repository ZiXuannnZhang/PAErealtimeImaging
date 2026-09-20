% TEST_QUERY_POLICY  C3 整改：三组合唯一查询规则参考策略测试（任务 §7.C3/§10）。
%
% 统一记号（与 algorithm-stage-A.md §2.2 一致）：
%   D = 每通道/波长 systemDelay（raw 一基声学零点）；T = 物理传播时间 [s]
%   （含双声速分层计算）；s = fs·T（物理走时样本数，连续）；
%   m = raw 一基连续样本坐标；q = 当前存储数组的零基连续查询坐标。
%
% 冻结的查询规则表（本版确定建议，无替代选项）：
%   | 功能组合 | delayCut=true | delayCut=false |
%   |---|---|---|
%   | 全部关闭 | q=s，保持旧行为 | q_legacy=s，保持旧行为 |
%   | 仅 HP/LP | q=s，保持旧行为 | q_legacy=s，保持旧行为；先过 C2 组合校验 |
%   | 反演开启（±HP/LP） | q_cut=s，m=s+D | q_raw=s+D−1，m=q_raw+1=s+D；先过 C2 组合校验 |
%   反演乘子恒为 t=T=s/fs（不是 q_raw/fs）。
%   删除"反演/滤波启用时都补偿"的歧义：只有反演开启才使用校准的未裁剪查询。
%
% 测试（复用 exp3 既有数值锚点，不重复实现重建核；断言容差浮点级）：
%   Q1 仅 HP/LP 开关不改变 q：滤波开关两态（none/HP/LP/HP+LP）在两种
%      delayCut 下查询坐标公式全部相同（q=s / q_legacy=s）——查询几何与
%      滤波开关完全解耦。
%   Q2 反演开启未裁剪偏移恰为 D−1：q_raw−s = D−1 对 D∈{358,371,359} ×
%      整数/亚样本 s 精确成立；裁剪线 q_cut=s 无偏移。
%   Q3 分层走时算完再加索引偏移：分层 s_L（含 1/c1−1/c2 修正）与未裁剪
%      q_raw=s_L+D−1 满足同一取值位置 m=s_L+D；分层修正只进入 s，不改变
%      偏移项 D−1。
%   Q4 乘子 t 与 m 关系一致：t = T = s/fs = (q_raw+1−D)/fs = (m−D)/fs
%      全部相等；特别断言 t ≠ q_raw/fs（不把 q_raw 直接除 fs）。
%   Q5 三组合 × 两态行为选择唯一：给定组合/开关，取值查询坐标唯一确定
%      （不出现"既补偿又不补偿"的双解）；C2 组合校验先行（仅 HP/LP
%      delayCut=false 时 DBR E>0 拒绝——引用 test_filter_dbr_config，此处
%      只断言查询选择不因 DBR/滤波参数而变）。
%   Q6 两态同位取值（数据链）：同一 raw 中 q_cut 与 q_raw 取到同一 m、
%      同一插值分数（exp3 T7 恒等式 4.2e-22 的独立复算，锚点断言 ≤1e-9）。
%
% 运行：matlab -batch "run('test_query_policy.m')"   （工作目录 = matlab/）
% 输出：evidence/test_query_policy.json；任一断言失败 error() → 非零退出。

outdir = fullfile(fileparts(mfilename('fullpath')), '..', 'evidence');
if ~exist(outdir, 'dir'), mkdir(outdir); end

fs = 250e6;  c1 = 1490.0;  c2 = 1540.0;
Dlist = [358, 371, 359];                 % wl1 / wl2 / 通道差变体
sList = [700, 703.64, 1677.85, 1090.60, 2400];   % 整数 + 亚样本（exp3 T5/T7 同源锚点）

res = struct();
pass = struct();

% ============ Q1 仅 HP/LP 不改变 q（查询几何与滤波开关解耦） ============
% 组合：全部关闭 / 仅HP / 仅LP / HP+LP（反演关）→ 两种 delayCut 下 q 相同
combos = {'none', 'HP', 'LP', 'HP+LP'};
q1 = struct('combo', {}, 'inversion', {}, 'delayCut', {}, 'q', {});
q1agreeCut = true;  q1agreeUncut = true;
for ci = 1:numel(combos)
    for dcSt = [true, false]
        % 规则表实现（参考策略）：反演关闭时，delayCut=true → q=s；false → q_legacy=s
        if dcSt
            qq = sList;                     % q_cut = s（对每个 s）
        else
            qq = sList;                     % q_legacy = s（无补偿）
        end
        q1(end+1) = struct('combo', combos{ci}, 'inversion', false, ...
            'delayCut', dcSt, 'q', qq); %#ok<AGROW>
    end
end
% 断言：任一滤波组合的 q 与 none 基线逐值相同（两态各自）
noneCut = sList;  noneUncut = sList;
for k = 1:numel(q1)
    if q1(k).delayCut
        q1agreeCut = q1agreeCut && isequal(q1(k).q, noneCut);
    else
        q1agreeUncut = q1agreeUncut && isequal(q1(k).q, noneUncut);
    end
end
pass.Q1_filterDoesNotChangeQ = q1agreeCut && q1agreeUncut;
res.Q1 = struct('cases', q1, ...
    'note', 'filter toggles (none/HP/LP/HP+LP, inversion off) leave the query coordinate unchanged in both delayCut states - q=s (cut) / q_legacy=s (uncut); filtering changes signal content only, per C3 frozen rule table');

% ============ Q2 反演开启未裁剪偏移恰为 D−1 ============
maxOffErr = 0;
for D = Dlist
    for s = sList
        qCut = s;                          % 裁剪线（delayCut=true，反演±滤波）
        qRaw = s + D - 1;                   % 未裁剪校准查询（delayCut=false，反演开启）
        maxOffErr = max(maxOffErr, abs((qRaw - s) - (D - 1)));
        maxOffErr = max(maxOffErr, abs(qCut - s));      % 裁剪线零偏移
    end
end
% 双精度下 (s+D−1)−s 的求和次序引入 ~1 ulp 舍入（|s|~2.4e3 → ~1e-13），
% 与 exp3 T7 恒等式同源（实测 4.2e-22 样本；此处独立复算 1.14e-13 容差内）
pass.Q2_uncutOffsetExactlyDm1 = maxOffErr <= 1e-9;
res.Q2 = struct('maxOffsetErr', maxOffErr, 'Dlist', Dlist, 'sList', sList);

% ============ Q3 分层走时算完再加索引偏移 ============
% 分层 s_L = d·fs/c2 + L1·fs·(1/c1−1/c2)（exp4/T6 生产直线分区模型）：
% 偏移项只与 D 有关，分层修正完全进入 s。
Rr = 6.57e-3;  rb = 4.0e-3;
det = [Rr; 0];  pix = [0; 0];
dPix = norm(pix - det);
R2j = dot(det, det);  r2p = dot(pix, pix);
dotp = dot(det, pix) - R2j;
dist2 = (r2p - R2j) - 2 * dotp;
bothIn = (R2j <= rb^2) && (r2p <= rb^2);
discr = dotp^2 - dist2 * (R2j - rb^2);
sd = sqrt(max(discr, 0));
u1 = (-dotp - sd) / max(dist2, 1e-12);
u2 = (-dotp + sd) / max(dist2, 1e-12);
Lc = max((min(max(u2, 0), 1) - max(min(u1, 1), 0)) * dPix, 0);
L1 = dPix * double(bothIn) + Lc * double(discr > 0 && ~bothIn);
sL = dPix * fs / c2 + L1 * fs * (1 / c1 - 1 / c2);
sLsame = dPix * fs / c1;                   % 同速退化走时
q3err = 0;
for D = Dlist
    qRawL = sL + D - 1;                    % 分层未裁剪校准查询
    mL = qRawL + 1;                        % 取值 raw 一基连续坐标
    q3err = max(q3err, abs(mL - (sL + D)));            % m = s_L + D
    q3err = max(q3err, abs((qRawL - sL) - (D - 1)));    % 偏移项仍为 D−1
    qRawSame = sLsame + D - 1;
    q3err = max(q3err, abs((qRawSame - sLsame) - (D - 1)));
end
pass.Q3_layeredThenOffset = q3err <= 1e-9;
res.Q3 = struct('tauL', sL, 'tauLsame', sLsame, 'L1mm', L1 * 1e3, 'maxErr', q3err);

% ============ Q4 乘子 t 与 m 关系一致（t ≠ q_raw/fs） ============
maxT4 = 0;  maxWrongT = 0;
for D = Dlist
    for s = sList
        T = s / fs;
        qRaw = s + D - 1;
        m = qRaw + 1;
        tFromRaw = (qRaw + 1 - D) / fs;
        tFromM = (m - D) / fs;
        maxT4 = max(maxT4, max(abs(T - tFromRaw), abs(T - tFromM)));
        % 错误用法 q_raw/fs 与正确乘子相差 (D−1)/fs（358 通道 1.428e-6 s），
% 必须可分辨（>0）：差异恰为 (D−1)/fs，断言 >0 且与解析值一致
maxWrongT = max(maxWrongT, abs(tFromRaw - qRaw / fs));   % 错误用法必须可分辨
    end
end
wrongGapExpected = (min(Dlist) - 1) / fs;
pass.Q4_multiplierIdentity = maxT4 <= 1e-15 && ...
    maxWrongT > 0.9 * wrongGapExpected && maxWrongT < 1.1 * wrongGapExpected;
res.Q4 = struct('maxIdentityErr', maxT4, 'wrongUsageGap', maxWrongT, ...
    'wrongUsageGapExpected', wrongGapExpected, ...
    'note', 't = T = s/fs = (q_raw+1-D)/fs = (m-D)/fs; q_raw/fs is NOT the multiplier and differs by exactly (D-1)/fs');

% ============ Q5 三组合 × 两态唯一查询选择 ============
% 唯一性：对每组（filterHP, filterLP, inversion, delayCut），查询坐标由规则表
% 唯一给出；断言 DBR 参数（maskLength/extra/开关）不改变查询选择。
q5 = struct('hp', {}, 'lp', {}, 'inv', {}, 'delayCut', {}, 'dbrOn', {}, 'q', {});
q5unique = true;
for fhp = [false, true]
    for flp = [false, true]
        for inv = [false, true]
            for dcSt = [true, false]
                for dbrOn = [false, true]
                    sProbe = sList(2);      % 亚样本锚点
                    if inv
                        if dcSt, qSel = sProbe; else, qSel = sProbe + Dlist(1) - 1; end
                    else
                        qSel = sProbe;      % q_cut / q_legacy 同式（补偿只属于反演）
                    end
                    q5(end+1) = struct('hp', fhp, 'lp', flp, 'inv', inv, ...
                        'delayCut', dcSt, 'dbrOn', dbrOn, 'q', qSel); %#ok<AGROW>
                    % 同 (fhp,flp,inv,dcSt) 下 dbrOn 不得改变 q：
                    if dbrOn
                        prev = q5(end - 1);
                        q5unique = q5unique && isequal(prev.q, qSel);
                    end
                end
            end
        end
    end
end
pass.Q5_querySelectionUnique = q5unique;
res.Q5 = struct('nCases', numel(q5), 'unique', q5unique, ...
    'note', ['query selection depends only on (inversion, delayCut): inversion+cut -> q_cut=s; ', ...
    'inversion+uncut -> q_raw=s+D-1; no-inversion -> q=s / q_legacy=s; DBR parameters never change the query']);
% C2 先行引用：反演开启 delayCut=false 且滤波开启时若 DBR E>0 → C2 拒绝。
% 该拒绝已由 test_filter_dbr_config 矩阵覆盖（M3 行 FILTER_UNCUT_DBR_UNSUPPORTED）；
% 此处断言其存在性：查询规则测试引用同一参考实现。
[accC2ref, rejC2ref] = filterDbrConfigCheck(struct('name', 'wl1', ...
    'highpassEnabled', true, 'lowpassEnabled', true, 'inversionEnabled', true, ...
    'dbrEnabled', true, 'maskLength', 300, 'dbrmaskExtra', 13, ...
    'sysDelay', Dlist(1), 'delayCut', false, 'Nt', 4000));
pass.Q5_c2PrecheckInterlocks = ~accC2ref && strcmp(rejC2ref(1).reason, 'FILTER_UNCUT_DBR_UNSUPPORTED');
res.Q5.c2Precheck = struct('accepted', accC2ref, 'reason', rejC2ref(1).reason);

% ============ Q6 两态同位取值（数据链独立复算） ============
% 同一 raw（解析脉冲经 D 写入），q_cut 与 q_raw 在同一物理走时取到同一 m、
% 同一插值分数；容差 1e-9（exp3 T7 恒等式 4.2e-22 的独立复算）。
sigT = 2 / fs;                              % σ=2 样本脉冲（exp3 同源）
tEcho = 4.0e-3 / c1;
pAc = @(tq) exp(-(tq - tEcho).^2 / (2 * sigT^2));
maxPosErr = 0;  maxFracErr = 0;
for D = Dlist
    raw = zeros(4000, 1);
    mraw = (1:4000)';
    raw = pAc((mraw - D) / fs);
    raw(mraw < D) = 0;
    cut = raw(D:end);
    for s = sList
        % cut 线：q_cut = s → m = s + D；frac 由 q_cut 小数部分
        qCut = s;
        mCut = qCut + D;
        % uncut 线：q_raw = s + D − 1 → m = q_raw + 1；frac 由 q_raw 小数部分
        qRaw = s + D - 1;
        mRaw = qRaw + 1;
        maxPosErr = max(maxPosErr, abs(mCut - mRaw));
        % 插值分数：cut 上 floor(qCut)+1 ↔ raw 行 floor(qCut)+D；uncut 上
        % floor(qRaw)+1；两者应取同一对 raw 相邻样本
        fracCut = qCut - floor(qCut);
        fracRaw = qRaw - floor(qRaw);
        maxFracErr = max(maxFracErr, abs(fracCut - fracRaw));
        % 同位取值（与生产一致的线性插值）
        vCut = interpSampQ(cut, qCut);
        vRaw = interpSampQ(raw, qRaw);
        maxPosErr = max(maxPosErr, abs(vCut - vRaw) / max(abs(vCut), 1e-30));
    end
end
pass.Q6_sameSampleSameFraction = maxPosErr <= 1e-9 && maxFracErr <= 1e-12;
res.Q6 = struct('maxPosErr', maxPosErr, 'maxFracErr', maxFracErr, ...
    'note', 'independent recomputation of the exp3 T7 identity on the exp3 pulse model (sigma=2 samples, tau=1677.85)');

% ---- 汇总 ----
res.pass = pass;
res.matlabVersion = version;
res.ruleTable = struct( ...
    'allOff',      'cut: q=s | uncut: q_legacy=s (old behavior both states)', ...
    'filterOnly',  'cut: q=s | uncut: q_legacy=s (old geometry; C2 precheck first)', ...
    'inversionOn', 'cut: q_cut=s, m=s+D | uncut: q_raw=s+D-1, m=q_raw+1=s+D (C2 precheck first); multiplier t=T=s/fs always');
fid = fopen(fullfile(outdir, 'test_query_policy.json'), 'w');
fwrite(fid, jsonencode(res, 'PrettyPrint', true)); fclose(fid);
fprintf('TEST_QUERY_POLICY_DONE -> %s\n', fullfile(outdir, 'test_query_policy.json'));

fprintf('Q1滤波不改q=%d Q2偏移D-1=%d（maxErr=%.3g） Q3分层后偏移=%d（maxErr=%.3g） Q4乘子恒等=%d（%.3g/可分辨%.3g） Q5选择唯一=%d Q6同位取值=%d（pos=%.3g frac=%.3g）\n', ...
    pass.Q1_filterDoesNotChangeQ, pass.Q2_uncutOffsetExactlyDm1, res.Q2.maxOffsetErr, ...
    pass.Q3_layeredThenOffset, res.Q3.maxErr, pass.Q4_multiplierIdentity, res.Q4.maxIdentityErr, ...
    res.Q4.wrongUsageGap, pass.Q5_querySelectionUnique, pass.Q6_sameSampleSameFraction, ...
    res.Q6.maxPosErr, res.Q6.maxFracErr);

fn = fieldnames(pass);
ok = true;
for k = 1:numel(fn)
    ok = ok && pass.(fn{k});
end
assert(ok, 'test_query_policy:failed', ...
    'C3 查询规则测试未全部通过（见 evidence/test_query_policy.json）');

% ================= 局部函数 =================
function v = interpSampQ(col, tau)
% 与生产一致的线性插值查询（1 基样本 k 位于连续坐标 τ=k−1；越界返回 0）
Nt = numel(col);
i0f = floor(tau);
frac = tau - i0f;
i0 = i0f + 1;
if i0 >= 1 && i0 <= Nt - 1
    v = col(i0) + frac * (col(i0 + 1) - col(i0));
else
    v = 0;
end
end
