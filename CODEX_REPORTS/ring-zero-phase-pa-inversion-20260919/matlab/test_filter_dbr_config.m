% TEST_FILTER_DBR_CONFIG  C2 整改：零相位滤波 × DBR × delayCut 配置前置
% 校验矩阵测试（任务 §7.C2/§10）。
%
% 被测对象 = filterDbrConfigCheck.m（参考实现，不接入生产 UI）。覆盖：
%   M1 HP/LP/两者（filterEnabled 三态构成） × 反演开关两态 —— 反演开关不改变
%      本表结论（仅反演开启不因本表错误触发 HP/LP 限制）。
%   M2 DBR 开关两态：DBR 关闭 → E=0（开关语义，非"开启即阶跃"）。
%   M3 E ∈ {0, D−1, D, D+1} × delayCut 两态：
%      E=0 允许；E=D−1 且 delayCut=true 允许；E>=D 且 delayCut=true 明确报错；
%      E>0 且 delayCut=false 明确报错（本版冻结，不再"留待阶段 B 决定"）。
%   M4 每通道/波长独立：一通道失败 → 整组拒绝，报告具体通道名与原因；
%      不只检查全局默认 D（D=358/371 双波长 + 非默认 D 变体）。
%   M5 E 的真实语义：DBR 开启但 maskLength+dbrmaskExtra=0 → E=0 允许；
%      置零长度计入 dbrmaskExtra 与实际样本数边界（E = min(L, Nt)）。
%   M6 全部新功能关闭：不引入新拒绝条件（旧行为保持——任意 delayCut/DBR
%      组合均不因本表拒绝）。
%   M7 非法参数沿用既定校验（报错，不静默截断修复）。
%
% 运行：matlab -batch "run('test_filter_dbr_config.m')"   （工作目录 = matlab/）
% 输出：evidence/test_filter_dbr_config.json；任一断言失败 error() → 非零退出。
% 注：M3/M5 的"预期失败"是被测校验器的正常拒绝（rejection 是功能不是错误）；
% 套件整体成功以全部断言通过为准，拒绝用例的 expected=reject 达成才算 PASS。

outdir = fullfile(fileparts(mfilename('fullpath')), '..', 'evidence');
if ~exist(outdir, 'dir'), mkdir(outdir); end

D1 = 358;  D2 = 371;                  % wl1/wl2 生产默认
Nt = 4000;
mk = @(name, hp, lp, inv, dbr, ml, ex, D, dc, ntv) struct( ...
    'name', name, 'highpassEnabled', hp, 'lowpassEnabled', lp, ...
    'inversionEnabled', inv, 'dbrEnabled', dbr, 'maskLength', ml, ...
    'dbrmaskExtra', ex, 'sysDelay', D, 'delayCut', dc, 'Nt', ntv);

res = struct();
rows = struct('caseId', {}, 'filterEnabled', {}, 'inversionEnabled', {}, ...
    'dbrEnabled', {}, 'E', {}, 'delayCut', {}, 'D', {}, 'expected', {}, ...
    'actual', {}, 'reason', {}, 'pass', {});
pass = struct();

% ---- M1+M2+M3：HP/LP/两者 × 反演 × DBR × E × delayCut 全矩阵 ----
% E 用 maskLength+dbrmaskExtra 构造（默认 300+13=313）：
%   E=0（DBR 关 或 L=0）、E=D−1（=357）、E=D（=358）、E>D（=359/371 通道越界等）
mat = struct();
for fCombo = {{'none', false, false}, {'HP', true, false}, {'LP', false, true}, {'HP+LP', true, true}}
    fSpec = fCombo{1};  fName = fSpec{1};  hp = fSpec{2};  lp = fSpec{3};
    for invSt = [false, true]
        for dcSt = [true, false]
            for eCase = {{'E0_dbrOff', false, 300, 13}, ...
                         {'E0_zeroLen', true, 0, 0}, ...
                         {'EDm1', true, 300, 57}, ...
                         {'ED', true, 300, 58}, ...
                         {'EgtD', true, 300, 59}}
                eSpec = eCase{1};  eName = eSpec{1};  dbrOn = eSpec{2};  ml = eSpec{3};  ex = eSpec{4};
                ch = mk(sprintf('%s/inv=%d/dc=%d/%s', fName, invSt, dcSt, eName), ...
                    hp, lp, invSt, dbrOn, ml, ex, D1, dcSt, Nt);
                [acc, rej] = filterDbrConfigCheck(ch);
                filterEnabled = hp || lp;
                if dbrOn
                    E = min(ml + ex, Nt);
                else
                    E = 0;
                end
                % 期望（冻结规则表）：
                if ~filterEnabled
                    expected = 'accept';            % M6：全部新功能关闭不拒绝
                elseif E == 0
                    expected = 'accept';
                elseif dcSt
                    if E < D1, expected = 'accept'; else, expected = 'reject'; end
                else
                    expected = 'reject';            % E>0 且 delayCut=false
                end
                actual = ternary(acc, 'accept', 'reject');
                reason = '';
                if ~acc, reason = rej(1).reason; end
                ok = strcmp(expected, actual);
                rows(end+1) = struct('caseId', ch.name, 'filterEnabled', filterEnabled, ...
                    'inversionEnabled', invSt, 'dbrEnabled', dbrOn, 'E', E, ...
                    'delayCut', dcSt, 'D', D1, 'expected', expected, ...
                    'actual', actual, 'reason', reason, 'pass', ok); %#ok<AGROW>
            end
        end
    end
end
mat.rows = rows;
mat.nCases = numel(rows);
mat.nReject = sum(strcmp({rows.actual}, 'reject'));
mat.nAccept = sum(strcmp({rows.actual}, 'accept'));
mat.expectedRejectionsAllListed = all(arrayfun(@(r) ...
    ~strcmp(r.expected, 'reject') || strcmp(r.actual, 'reject'), rows));
res.matrix = mat;
% M1：反演开关不改变结论——同 (filter,dc,E) 下 inv=false/true 的 actual 相同
agreeInv = true;
for i = 1:numel(rows)
    for j = 1:numel(rows)
        if i ~= j && strcmp(rows(i).caseId(1:end-1), rows(j).caseId(1:end-1))
            % inv 位在 caseId 中独立，逐对比较由 M1Inv 分组断言承担（见下）
        end
    end
end
% 更直接：M1 断言 = 矩阵中所有 inv=true 用例与其对应 inv=false 用例一致
rowsByInv = rows([rows.inversionEnabled]);
rowsByNoInv = rows(~[rows.inversionEnabled]);
keyFun = @(r) sprintf('%s|dc=%d|E=%d', r.caseId(1:strfind(r.caseId, '/inv=') - 1), ...
    r.delayCut, r.E);
for i = 1:numel(rowsByInv)
    ki = keyFun(rowsByInv(i));
    for j = 1:numel(rowsByNoInv)
        if strcmp(ki, keyFun(rowsByNoInv(j)))
            agreeInv = agreeInv && strcmp(rowsByInv(i).actual, rowsByNoInv(j).actual);
        end
    end
end
pass.M1_inversionIndependent = agreeInv;
pass.M2_matrixAllAsExpected = all([rows.pass]);
pass.M3_rejectsReportReasons = mat.nReject > 0 && ...
    all(arrayfun(@(r) ~isempty(r.reason) || ~strcmp(r.actual, 'reject'), rows));

% ---- M3b 边界值专项：E=D−1 允许 / E=D 报错 / E>D 报错（delayCut=true） ----
b = struct();
b.EDm1 = filterDbrConfigCheck(mk('wl1/EDm1', true, true, false, true, 300, 57, D1, true, Nt));
b.ED = filterDbrConfigCheck(mk('wl1/ED', true, true, false, true, 300, 58, D1, true, Nt));
b.EgtD = filterDbrConfigCheck(mk('wl1/EgtD', true, true, false, true, 300, 59, D1, true, Nt));
b.EDm1_wl2 = filterDbrConfigCheck(mk('wl2/EDm1', true, true, false, true, 300, 70, D2, true, Nt));
b.ED_wl2 = filterDbrConfigCheck(mk('wl2/ED', true, true, false, true, 300, 71, D2, true, Nt));
pass.M3b_EDm1Accepted = b.EDm1;
pass.M3b_EDRejected = ~b.ED;
pass.M3b_EgtDRejected = ~b.EgtD;
pass.M3b_wl2Boundary = b.EDm1_wl2 && ~b.ED_wl2;
res.boundaryCases = struct('EDm1', b.EDm1, 'ED', b.ED, 'EgtD', b.EgtD, ...
    'EDm1_wl2', b.EDm1_wl2, 'ED_wl2', b.ED_wl2);

% ---- M4 逐通道拒绝：一通道失败 → 整组拒绝并指名 ----
g1 = mk('wl1-ok', true, true, false, true, 300, 13, D1, true, Nt);
g2 = mk('wl2-bad', true, true, false, true, 300, 100, D2, true, Nt);   % E=400 > D2
[accG, rejG] = filterDbrConfigCheck([g1, g2]);
pass.M4_groupRejectedOnOneChannel = ~accG && numel(rejG) == 1 && ...
    strcmp(rejG(1).name, 'wl2-bad');
res.groupCase = struct('accepted', accG, 'rejects', rejG);
% 全好组通过
[accG2, rejG2] = filterDbrConfigCheck([g1, mk('wl2-ok', true, true, false, true, 300, 13, D2, true, Nt)]);
pass.M4_groupAcceptedWhenAllOk = accG2 && isempty(rejG2);
res.groupCaseAllOk = struct('accepted', accG2);

% ---- M5 E 真实语义：DBR 开但 L=0 → E=0 允许；E 计入 extra 与 Nt 边界 ----
[accE0, rejE0] = filterDbrConfigCheck(mk('zeroLen', true, true, false, true, 0, 0, D1, false, Nt));
pass.M5_dbrOnZeroLenAccepted = accE0;      % delayCut=false 也允许：E=0 无阶跃
res.dbrOnZeroLen = struct('accepted', accE0, 'rejects', rejE0);
% E 截到 Nt 边界：L > Nt 时 E=Nt（仍 >=D → delayCut=true 报错）
[accNt, rejNt] = filterDbrConfigCheck(mk('cappedAtNt', true, true, false, true, 5000, 0, D1, true, Nt));
pass.M5_eCappedByNt = ~accNt && strcmp(rejNt(1).reason, 'DBR_END_NOT_BEFORE_DELAYCUT_START');
res.eCappedByNt = struct('accepted', accNt, 'rejects', rejNt);

% ---- M6 全部新功能关闭：不引入新拒绝（旧行为保持） ----
% none 滤波 × 任意 DBR/delayCut 组合全部接受（既定有效查询/导数端点规则
% 不在本表；本测试断言的是"不因本表拒绝"）。
okNone = true;
for dcSt = [true, false]
    for eSpec = {struct('dbrOn', false, 'ml', 300, 'ex', 13), ...
                 struct('dbrOn', true, 'ml', 300, 'ex', 13), ...
                 struct('dbrOn', true, 'ml', 300, 'ex', 58), ...
                 struct('dbrOn', true, 'ml', 300, 'ex', 100)}
        e = eSpec{1};
        chN = mk('noneCombo', false, false, false, e.dbrOn, e.ml, e.ex, D1, dcSt, Nt);
        [accN, ~] = filterDbrConfigCheck(chN);
        okNone = okNone && accN;
    end
end
% 仅反演开启（无滤波）：不因本表拒绝
[accInv, ~] = filterDbrConfigCheck(mk('invOnly', false, false, true, true, 300, 13, D1, false, Nt));
pass.M6_noneComboNoNewReject = okNone && accInv;
res.noneCombo = struct('allAccepted', okNone, 'inversionOnlyAccepted', accInv);

% ---- M7 非法参数沿用既定校验（报错，不静默修复） ----
err7 = struct();
try, filterDbrConfigCheck(mk('bad', true, true, false, true, -1, 0, D1, true, Nt)); ...
    err7.negMask = false; catch, err7.negMask = true; end
try, filterDbrConfigCheck(mk('bad', true, true, false, true, 300, 0, 0, true, Nt)); ...
    err7.zeroDelay = false; catch, err7.zeroDelay = true; end
try, filterDbrConfigCheck(mk('bad', true, true, false, true, 300, 0, D1, true, Nt - 0.5)); ...
    err7.nonIntNt = false; catch, err7.nonIntNt = true; end
pass.M7_invalidParamsError = err7.negMask && err7.zeroDelay && err7.nonIntNt;
res.invalidParamErrors = err7;

% ---- 汇总 ----
res.pass = pass;
res.matlabVersion = version;
fid = fopen(fullfile(outdir, 'test_filter_dbr_config.json'), 'w');
fwrite(fid, jsonencode(res, 'PrettyPrint', true)); fclose(fid);
fprintf('TEST_FILTER_DBR_CONFIG_DONE -> %s\n', fullfile(outdir, 'test_filter_dbr_config.json'));

fprintf('矩阵：%d 用例（accept=%d reject=%d），M1反演无关=%d M3b边界=%d/%d/%d M4整组=%d M5语义=%d M6旧行为=%d M7非法=%d\n', ...
    mat.nCases, mat.nAccept, mat.nReject, pass.M1_inversionIndependent, ...
    pass.M3b_EDm1Accepted, pass.M3b_EDRejected, pass.M3b_EgtDRejected, ...
    pass.M4_groupRejectedOnOneChannel, pass.M5_eCappedByNt, ...
    pass.M6_noneComboNoNewReject, pass.M7_invalidParamsError);

fn = fieldnames(pass);
ok = true;
for k = 1:numel(fn)
    ok = ok && pass.(fn{k});
end
assert(ok, 'test_filter_dbr_config:failed', ...
    'C2 配置矩阵测试未全部通过（见 evidence/test_filter_dbr_config.json）');

% ================= 局部函数 =================
function out = ternary(cond, a, b)
if cond, out = a; else, out = b; end
end
