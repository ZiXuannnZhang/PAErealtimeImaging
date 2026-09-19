function K = reconPairKernels()
% RECONPAIRKERNELS 权重/归一化对照共用的重建核（审查整改 R2）。
%
% exp2 与 test_fair_comparison 必须使用同一实现，保证对照只改变被比较的因素：
%   固定：输入信号（p 或 b）、几何、插值、越界掩码、角度采样、幅度标定、测量方法；
%   改变：权重族（旧 DAS q=1 ↔ 立体角）与/或归一化（accW ↔ 原始和式）。
%
% 因子分解（每次只动一个因素）：
%   das        = p      + q=1 权重   + accW 归一化   （生产旧默认基准）
%   legacyW    = b      + q=1 权重   + accW 归一化   （旧权重配对，公平对照）
%   ubpD       = b      + 立体角权重 + accW 归一化   （拟定配对）
%   ubpP       = b      + 立体角权重 + 原始和式 + C_P（物理标度另列）
%   saP        = p      + 立体角权重 + accW 归一化   （因子分解补格：仅换权重）
%
% 命名说明（审查 R2）：9493962 版的 "wrong"（reconUBPwrong + 单参数 normAcc）
% 有两处缺陷——对照只返回 acc，被 normAcc(acc) 自归一化成 sign(acc) 符号图；
% 现改为 legacyW 命名并同时返回 acc/accW，归一化只用显式两参数 normAccW。
% 单参数自归一化接口已删除（normAccW 对缺 accW 直接报错）。

K.reconDAS = @reconDAS;                       % [acc, accW]
K.reconUBP = @reconUBP;                       % [acc, accW] b + 立体角权重
K.reconLegacyWeight = @reconLegacyWeight;     % [acc, accW] b + 旧 q=1 权重
K.reconSolidAngleP = @reconSolidAngleP;       % [acc, accW] p + 立体角权重
K.normAccW = @normAccW;                       % 显式归一化（两参数）
K.interpLine = @interpLine;
end

% ================= 归一化 =================
function img = normAccW(acc, accW)
% acc/max(accW,1e-12)：与生产 snapshot 相同的显式两参数归一化。
% 不提供单参数形式：acc/max|acc| 会把幅值信息压成 ±1 符号图（R2 缺陷）。
if nargin < 2
    error('reconPairKernels:normAccWNeedsAccW', ...
        'normAccW 必须显式提供 accW；单参数自归一化会退化为 sign(acc) 符号图');
end
img = acc ./ max(accW, 1e-12);
end

% ================= 重建核（全部返回原始累加 acc 与权重累加 accW） =================
function [acc, accW] = reconDAS(p, theta, R, X, Y, fs, c, minDist, dtheta)
% 生产旧默认 DAS q=1：信号 p，权重 Δθ·cosα/d，acc/accW 归一化
[acc, accW] = accumulate(p, theta, R, X, Y, fs, c, minDist, dtheta, false, true);
end

function [acc, accW] = reconUBP(p, pp, theta, R, X, Y, fs, c, minDist, dtheta)
% 拟定配对：信号 b = 2p − 2t·p'，权重 ΔΩ = Δθ·R·cosα/d²（每单位 z 长度）
[acc, accW] = accumulate(p, theta, R, X, Y, fs, c, minDist, dtheta, true, false, pp);
end

function [acc, accW] = reconLegacyWeight(p, pp, theta, R, X, Y, fs, c, minDist, dtheta)
% 旧权重配对（中性命名，替代旧 "wrong"）：信号 b，权重 Δθ·cosα/d（仅换信号不换权重）
[acc, accW] = accumulate(p, theta, R, X, Y, fs, c, minDist, dtheta, true, true, pp);
end

function [acc, accW] = reconSolidAngleP(p, theta, R, X, Y, fs, c, minDist, dtheta)
% 因子分解补格：信号 p，权重 ΔΩ（仅换权重不换信号）
[acc, accW] = accumulate(p, theta, R, X, Y, fs, c, minDist, dtheta, false, false);
end

% ================= 公共累加器：除 (信号, 权重族) 外一切固定 =================
function [acc, accW] = accumulate(p, theta, R, X, Y, fs, c, minDist, dtheta, useB, legacyW, pp)
[nY, nX] = size(X);
acc = zeros(nY, nX);  accW = zeros(nY, nX);
r2 = X.^2 + Y.^2;
for j = 1:size(p, 2)
    proj = X * (R * cos(theta(j))) + Y * (R * sin(theta(j)));
    dotp = proj - R^2;
    dist = sqrt(max((r2 - R^2) - 2 * dotp, 0));
    dsafe = max(dist, minDist);
    tf = dist * fs / c;
    v = interpLine(p(:, j), tf);
    if useB
        tsec = tf / fs;
        vp = interpLine(pp(:, j), tf);
        v = 2 * v - 2 * tsec .* vp;            % b = 2p − 2t·p'（t = τ/fs [s]）
    end
    cosAlpha = (-dotp) ./ (R * dsafe);
    if legacyW
        w = dtheta * cosAlpha ./ dsafe;        % Δθ·cosα/d（旧 q=1 权重族）
    else
        w = dtheta * R * cosAlpha ./ (dsafe.^2); % Δθ·R·cosα/d²（立体角/单位 z 长度）
    end
    acc = acc + w .* v;
    accW = accW + abs(w);                      % 同一 abs(w) 累积规则
end
end

function v = interpLine(col, tf)
% 与生产一致的线性插值查询（maskOob：越界置零）
Nt = numel(col);
i0f = floor(tf);
frac = tf - i0f;
i0 = i0f + 1;
valid = (i0 >= 1) & (i0 <= Nt - 1);
i0c = min(max(i0, 1), Nt - 1);
v0 = col(i0c);
v1 = col(i0c + 1);
v = v0 + frac .* (v1 - v0);
v(~valid) = 0;
end
