function [verdict, detail] = padConvergenceCheck(padList, relP, relPP, relB, padRef, convTol, pairRel)
% PADCONVERGENCECHECK 长参考 pad 收敛判据唯一入口（C1 整改，任务 §7.C1）。
%
% 旧缺陷（63e088a，撤回）：test_boundary_reference S3 与 exp5 B6 把 padRef 放入
% 候选并与自身比较（自比误差恒 0），门条件仅 convergedPad<=padRef——即使其余
% pad 全部超差也会通过。本函数为两处共用的统一判据，避免标准漂移。
%
% 通过标准（全部满足才 PASS）：
%   1. padRef 只作参考，pad==padRef 项禁止作为通过候选（自比恒零不计入）。
%   2. 至少两个不同的非参考 pad 与参考的 p/p′/b 误差均在容差内。
%   3. 通过的 pad 两两之间也稳定（同一参考尺度下相对差在容差内），且从满足
%      容差的最小 pad 到最大非参考 pad 不出现再次超差（稳定尾段）。
%   4. 候选不足、误差非有限、尺度或形状不合法、全部候选超差 → FAIL；
%      不允许空数组 all() 真值通过。
%
% 输入
%   padList : 递增 pad 序列（严格递增、正、有限；与误差表一一对应）
%   relP/relPP/relB : 各 pad 相对参考的 p/p′/b 最大相对误差（与 padList 等长，
%                     相对同一参考窗内幅度尺度）
%   padRef  : 参考窗口 pad（必须存在于 padList；该行不作为候选）
%   convTol : 相对容差（>0、有限；当前记录值 1e-9）
%   pairRel : 可选。实际两两差 struct('diffP',M,'diffPP',M,'diffB',M)，M 为
%             n×n 矩阵，M(i,j)=第 i/j 个 pad 结果在同一参考尺度下的最大相对差
%             （对角 0）。提供时按实际两两差判定规则 3（真实数据路径）；
%             缺省时用三角不等式保守上界 rel_i+rel_j（人工误差表负例路径）。
%
% 输出
%   verdict : 'PASS' | 'FAIL'
%   detail  : .ok .failReason .tol .padRef .padList .relP .relPP .relB
%             .candIdx（非参考候选下标） .okIdx（通过容差的候选下标）
%             .firstOkPad .maxOkPad .nOk
%             .pairSource('actual'|'sumBound') .maxPairRel
%             .adjPairP/.adjPairPP/.adjPairB（相邻通过 pad 的两两差，nOk-1 项）
%             .relapse .relapsePad
%
% 说明：最大参考（padRef）"足够"仍是数值近似结论，由多个非参考窗口的稳定
% 尾段支持，不宣称严格数学证明（任务 §7.C1 第 5 条）。

verdict = 'FAIL';
detail = struct('ok', false, 'failReason', '', 'tol', convTol, 'padRef', padRef, ...
    'padList', [], 'relP', [], 'relPP', [], 'relB', [], ...
    'candIdx', [], 'okIdx', [], 'firstOkPad', NaN, 'maxOkPad', NaN, ...
    'nOk', 0, 'pairSource', 'sumBound', 'maxPairRel', NaN, ...
    'adjPairP', NaN, 'adjPairPP', NaN, 'adjPairB', NaN, ...
    'relapse', false, 'relapsePad', NaN);

if nargin < 7, pairRel = []; end

% ---- 4. 形状/尺度合法性（不合法直接 FAIL，不产生空数组真值） ----
if ~isnumeric(padList) || ~isvector(padList) || isempty(padList) || any(~isfinite(padList)) || any(padList <= 0)
    detail.failReason = 'padList 非法（非有限/非正/空）';
    return;
end
padList = padList(:)';
n = numel(padList);
if any(diff(padList) <= 0)
    detail.failReason = 'padList 必须严格递增';
    return;
end
detail.padList = padList;
for nm = {'relP', 'relPP', 'relB'}
    v = [];
    switch nm{1}
        case 'relP', v = relP;
        case 'relPP', v = relPP;
        case 'relB', v = relB;
    end
    if ~isnumeric(v) || ~isvector(v) || numel(v) ~= n
        detail.failReason = sprintf('%s 与 padList 长度不一致或非向量', nm{1});
        return;
    end
    v = v(:)';
    if any(~isfinite(v))
        detail.failReason = sprintf('%s 含 NaN/Inf（非有限误差）', nm{1});
        return;
    end
    if any(v < 0)
        detail.failReason = sprintf('%s 含负值（相对误差尺度非法）', nm{1});
        return;
    end
    switch nm{1}
        case 'relP', relP = v;
        case 'relPP', relPP = v;
        case 'relB', relB = v;
    end
    detail.(nm{1}) = v;
end
if ~isscalar(convTol) || ~isfinite(convTol) || convTol <= 0
    detail.failReason = 'convTol 非法（非正/非有限）';
    return;
end
if ~isscalar(padRef) || ~isfinite(padRef) || ~any(padList == padRef)
    detail.failReason = 'padRef 必须是 padList 中的有限值';
    return;
end
if ~isempty(pairRel)
    if ~isstruct(pairRel) || ~all(isfield(pairRel, {'diffP', 'diffPP', 'diffB'}))
        detail.failReason = 'pairRel 非法（缺 diffP/diffPP/diffB 字段）';
        return;
    end
    for dm = {'diffP', 'diffPP', 'diffB'}
        M = pairRel.(dm{1});
        if ~isnumeric(M) || ~ismatrix(M) || ~isequal(size(M), [n, n]) || any(~isfinite(M(:))) || any(M(:) < 0)
            detail.failReason = sprintf('pairRel.%s 非法（非 n×n/非有限/负值）', dm{1});
            return;
        end
    end
end

% ---- 1. 排除自比：pad == padRef 不作为候选 ----
isRef = padList == padRef;
candIdx = find(~isRef);
detail.candIdx = candIdx;
if isempty(candIdx)
    detail.failReason = '无非参考候选（padList 仅含参考）';
    return;
end

% ---- 2. 候选与参考的误差均在容差内 ----
okMask = relP(candIdx) <= convTol & relPP(candIdx) <= convTol & relB(candIdx) <= convTol;
okIdx = candIdx(okMask);
detail.okIdx = okIdx;
detail.nOk = numel(okIdx);
if detail.nOk == 0
    detail.failReason = '全部非参考候选超差';
    return;
end
detail.firstOkPad = padList(okIdx(1));
detail.maxOkPad = padList(okIdx(end));

% ---- 3a. 至少两个不同非参考 pad 通过 ----
if detail.nOk < 2
    detail.failReason = sprintf('仅 %d 个非参考 pad 通过容差（需 >=2）', detail.nOk);
    return;
end

% ---- 3b. 通过 pad 两两稳定（同一参考尺度，容差内）----
maxPairRel = 0;
adjP = zeros(1, detail.nOk - 1);  adjPP = zeros(1, detail.nOk - 1);  adjB = zeros(1, detail.nOk - 1);
if isempty(pairRel)
    detail.pairSource = 'sumBound';
    for a = 1:detail.nOk
        for b = 1:detail.nOk
            if a < b
                i = okIdx(a);  j = okIdx(b);
                maxPairRel = max(maxPairRel, ...
                    max([relP(i) + relP(j), relPP(i) + relPP(j), relB(i) + relB(j)]));
            end
        end
    end
    for k = 1:detail.nOk - 1
        i = okIdx(k);  j = okIdx(k + 1);
        adjP(k) = relP(i) + relP(j);  adjPP(k) = relPP(i) + relPP(j);  adjB(k) = relB(i) + relB(j);
    end
else
    detail.pairSource = 'actual';
    for a = 1:detail.nOk
        for b = 1:detail.nOk
            if a < b
                i = okIdx(a);  j = okIdx(b);
                maxPairRel = max(maxPairRel, ...
                    max([pairRel.diffP(i, j), pairRel.diffPP(i, j), pairRel.diffB(i, j)]));
            end
        end
    end
    for k = 1:detail.nOk - 1
        i = okIdx(k);  j = okIdx(k + 1);
        adjP(k) = pairRel.diffP(i, j);  adjPP(k) = pairRel.diffPP(i, j);  adjB(k) = pairRel.diffB(i, j);
    end
end
detail.maxPairRel = maxPairRel;
detail.adjPairP = adjP;  detail.adjPairPP = adjPP;  detail.adjPairB = adjB;
if maxPairRel > convTol
    detail.failReason = sprintf('通过 pad 两两不稳定（%s 相对差 %.3g > 容差 %.0e）', ...
        detail.pairSource, maxPairRel, convTol);
    return;
end

% ---- 3c. 尾段无再次超差：从 firstOkPad 到最大非参考 pad 不许回落 ----
relapse = false;  relapsePad = NaN;
for ci = candIdx(candIdx >= okIdx(1))
    if ~(relP(ci) <= convTol && relPP(ci) <= convTol && relB(ci) <= convTol)
        relapse = true;
        relapsePad = padList(ci);
        break;
    end
end
detail.relapse = relapse;
detail.relapsePad = relapsePad;
if relapse
    detail.failReason = sprintf('尾段再次超差（pad=%g 后出现超差候选）', relapsePad);
    return;
end

% ---- 全部满足 ----
verdict = 'PASS';
detail.ok = true;
end
