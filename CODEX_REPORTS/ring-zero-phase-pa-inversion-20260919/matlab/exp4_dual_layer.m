% EXP4_DUAL_LAYER  判定性实验 4：双声速分层走时一致性与同声速退化。
%
% 生产分层模型（ring_recon_cuda.cu 297-315 / das_recon_circular_gpu_v2.m 一致）：
%   tf = d·fs/c_outer + Σᵢ Lᵢ·fs·(1/cᵢ − 1/cᵢ₊₁)
%   Lᵢ = 探测器→像素直线段落在边界圆 rbᵢ 内的长度（闭式判别式求交）
%   bothIn = 探测器与像素都在边界内 → Lᵢ = d；否则 Lᵢ = 弦长 Lc（相交时）。
%
% 测试内容
%   T1 闭式 Lᵢ vs 密集采样几何直算：一系列 (探测器, 像素) 对的最大 |ΔL|。
%   T2 同声速退化：c1=c2=1490 的双层 τ 与单声速 τ 全网格最大 |Δτ|（应≈0）。
%   T3 位置一致性：吸收体在原点（跨边界），前向数据按同一分层模型放置回波，
%      分层重建后峰值位置误差；并与单声速结果对比（模型差异量化）。
%      注：这是模型自洽性测试，不代表折射物理（生产与本文档均不实现折射）。
%
% 运行：matlab -batch "run('exp4_dual_layer.m')"
% 输出：evidence/exp4_dual_layer.json

outdir = fullfile(fileparts(mfilename('fullpath')), '..', 'evidence');
if ~exist(outdir, 'dir'), mkdir(outdir); end

fs = 250e6;  R = 6.57e-3;
rb = 4.0e-3;                     % 单一边界半径
c1 = 1490.0;  c2 = 1540.0;       % 内层/外层声速
cSingle = 1490.0;

% ============ T1 闭式 Lᵢ vs 密集采样 ============
rng(7);                          % 可复现随机序列
nPair = 400;
maxDL = 0; maxRel = 0;
for it = 1:nPair
    th1 = 2 * pi * rand();
    r1 = R;                                        % 探测器在环上
    det = r1 * [cos(th1); sin(th1)];
    rp = sqrt(rand()) * 10e-3;                     % 像素半径 0..10mm
    tp = 2 * pi * rand();
    pix = rp * [cos(tp); sin(tp)];
    Lc = chordInside(det, pix, rb);
    Lb = chordInsideGeom(det, pix, rb);   % 独立代数路径（垂距+弦长，机器精度）
    maxDL = max(maxDL, abs(Lc - Lb));
    maxRel = max(maxRel, abs(Lc - Lb) / max(Lb, 1e-9));
end
fprintf('T1 生产判别式 vs 独立垂距弦长法（%d 对）：max|ΔL|=%.4g m（相对 %.4g）\n', ...
    nPair, maxDL, maxRel);

% ============ T2 同声速退化 ============
nd = 360;
theta = (0:nd-1)' * (2 * pi / nd);
nG = 121;
gv = linspace(-6e-3, 6e-3, nG);
[X, Y] = meshgrid(gv, gv);
tauLayer = layeredTau(X, Y, theta, R, rb, c1, c2, fs);      % [nG*nG x nd]
tauLayerEq = layeredTau(X, Y, theta, R, rb, cSingle, cSingle, fs);
tauSingle = singleTau(X, Y, theta, R, cSingle, fs);
dTauEq = max(abs(tauLayerEq(:) - tauSingle(:)));
dTau = max(abs(tauLayer(:) - tauSingle(:)));                % 供参考：真实双层差异
fprintf('T2 同声速退化：max|τ_layeredEq − τ_single| = %.4g 样本（参考：真实双层 max|Δτ|=%.4g）\n', ...
    dTauEq, dTau);

% ============ T3 位置一致性（模型自洽）============
sig = 0.5e-3;
absorber = [0; 0];               % 原点：位于边界内（跨边界直线）
nAbsSamp = 4000;
dr = c1 / fs;
rGrid = (0:nAbsSamp-1)' * (c2 / fs);   % 用外层声速定采样间隔（同生产 fs）

% 每探测器的分层走时到吸收体
tauAbs = layeredTauPoint(absorber, theta, R, rb, c1, c2, fs);   % [nd x 1] 样本
% 前向：回波高斯包络放在分层走时处（模型自洽构造，非折射物理）
sigSamp = sig / (c2 / fs);       % 以采样为单位的包络宽度
pLay = zeros(nAbsSamp, nd);
for j = 1:nd
    pLay(:, j) = exp(-((1:nAbsSamp)' - tauAbs(j)).^2 / (2 * sigSamp^2));
end
% 单声速前向（用于对比）
tauAbsS = fs / cSingle * sqrt((absorber(1) - R * cos(theta)).^2 + (absorber(2) - R * sin(theta)).^2);
pSin = zeros(nAbsSamp, nd);
for j = 1:nd
    pSin(:, j) = exp(-((1:nAbsSamp)' - tauAbsS(j)).^2 / (2 * sigSamp^2));
end

minDist = 0.2e-3;
dtheta = 2 * pi / nd;
imgLay = reconLayered(pLay, theta, R, rb, c1, c2, X, Y, fs, minDist, dtheta);
imgSing = reconLayered(pSin, theta, R, R + 1, cSingle, cSingle, X, Y, fs, minDist, dtheta); % rb>R → 单声速
imgDeg = reconLayered(pSin, theta, R, rb, c1, c1, X, Y, fs, minDist, dtheta);               % 同声速双层

[posLay, ampLay] = peakAt(imgLay, gv, absorber);
[posSing, ampSing] = peakAt(imgSing, gv, absorber);
dDeg = max(abs(imgDeg(:) - imgSing(:)));
fprintf('T3 分层重建峰值位置误差 = %.3f mm（单声速 %.3f mm）；同声速退化 vs 单声速 max|Δimg| = %.4g\n', ...
    posLay * 1e3, posSing * 1e3, dDeg);

res = struct('T1_maxDL', maxDL, 'T1_maxRel', maxRel, 'T2_maxDTau', dTauEq, ...
    'T2_maxDTauTrueLayers', dTau, ...
    'T3_posErrLayeredMm', posLay * 1e3, 'T3_posErrSingleMm', posSing * 1e3, ...
    'T3_degMaxDiff', dDeg, 'T3_ampLayered', ampLay, 'T3_ampSingle', ampSing);
res.pass = struct('T1', res.T1_maxRel < 1e-9, 'T2', res.T2_maxDTau < 1e-6, ...
    'T3', res.T3_posErrLayeredMm <= 0.4, 'T3deg', res.T3_degMaxDiff < 1e-6);
fprintf('断言：T1=%d T2=%d T3=%d 同声速退化=%d\n', ...
    res.pass.T1, res.pass.T2, res.pass.T3, res.pass.T3deg);

out = res;
out.params = struct('fs', fs, 'R', R, 'rb', rb, 'c1', c1, 'c2', c2, ...
    'absorber', absorber, 'sig', sig, 'nd', nd, 'grid', 0.1e-3);
out.matlabVersion = version;
fid = fopen(fullfile(outdir, 'exp4_dual_layer.json'), 'w');
fwrite(fid, jsonencode(out, 'PrettyPrint', true)); fclose(fid);
fprintf('EXP4_DONE -> %s\n', fullfile(outdir, 'exp4_dual_layer.json'));

% ================= 局部函数 =================
function L = chordInside(det, pix, rb)
% 闭式：直线段 det→pix 落在半径 rb 圆内的长度（与生产判别式实现一致）
d = pix - det;
dist = norm(d);
if dist < 1e-12, L = 0; return; end
R2j = dot(det, det);
r2 = dot(pix, pix);
dotp = dot(det, pix) - R2j;      % 与生产 dotp 同义（=proj−R²）
dist2 = (r2 - R2j) - 2 * dotp;   % = |pix−det|²
bothIn = (R2j <= rb^2) && (r2 <= rb^2);
discr4 = dotp^2 - dist2 * (R2j - rb^2);
cross = discr4 > 0;
sd = sqrt(max(discr4, 0));
dist2d = max(dist2, 1e-12);
u1 = (-dotp - sd) / dist2d;
u2 = (-dotp + sd) / dist2d;
Lc = (min(max(u2, 0), 1) - max(min(u1, 1), 0)) * dist;
Lc = max(Lc, 0);
L = dist * double(bothIn) + Lc * double(cross && ~bothIn);
end

function L = chordInsideGeom(det, pix, rb)
% 独立代数路径：直线到圆心垂距 h + 半弦长 sqrt(rb²−h²)，按段参数裁剪
d = pix - det;
dist = norm(d);
if dist < 1e-12, L = 0; return; end
u = d / dist;                                  % 单位方向（det→pix）
h = abs(det(1) * u(2) - det(2) * u(1));        % 圆心(0,0)到直线的垂距
if h >= rb
    L = 0;
    return;
end
s = sqrt(rb^2 - h^2);                          % 半弦长
tFoot = -dot(det, u);                          % 垂足在段上的距离坐标
t1 = max(tFoot - s, 0);
t2 = min(tFoot + s, dist);
L = max(t2 - t1, 0);
end

function tau = singleTau(X, Y, theta, R, cSingle, fs)
% 单声速走时（样本）：d = |x − x0|
npix = numel(X);
Xc = X(:);  Yc = Y(:);
r2 = Xc.^2 + Yc.^2;
tau = zeros(npix, numel(theta));
for j = 1:numel(theta)
    detx = R * cos(theta(j));  dety = R * sin(theta(j));
    R2j = R^2;
    proj = Xc * detx + Yc * dety;
    dotp = proj - R2j;
    dist2 = (r2 - R2j) - 2 * dotp;
    tau(:, j) = sqrt(max(dist2, 0)) * fs / cSingle;
end
end

function tau = layeredTau(X, Y, theta, R, rb, c1, c2, fs)
% [npix x nd] 分层走时（样本）——与生产 CUDA 实现逐式对应
npix = numel(X);
Xc = X(:);  Yc = Y(:);
r2 = Xc.^2 + Yc.^2;
rb2 = rb^2;
tau = zeros(npix, numel(theta));
preOuter = fs / c2;
preCoeff = fs * (1 / c1 - 1 / c2);
for j = 1:numel(theta)
    detx = R * cos(theta(j));  dety = R * sin(theta(j));
    R2j = R^2;
    proj = Xc * detx + Yc * dety;
    dotp = proj - R2j;
    dist2 = (r2 - R2j) - 2 * dotp;
    dist = sqrt(max(dist2, 0));
    tf = dist * preOuter;
    bothIn = (R2j <= rb2) & (r2 <= rb2);
    discr4 = dotp.^2 - dist2 * (R2j - rb2);
    cross = discr4 > 0;
    sd = sqrt(max(discr4, 0));
    dist2d = max(dist2, 1e-12);
    u1 = (-dotp - sd) ./ dist2d;
    u2 = (-dotp + sd) ./ dist2d;
    Lc = (min(max(u2, 0), 1) - max(min(u1, 1), 0)) .* dist;
    Lc = max(Lc, 0);
    Li = dist .* double(bothIn) + Lc .* double(cross & ~bothIn);
    tau(:, j) = tf + Li * preCoeff;
end
end

function tau = layeredTauPoint(y, theta, R, rb, c1, c2, fs)
% 与生产一致：先 d/c_outer，再加各边界内的长度修正
tau = zeros(numel(theta), 1);
for j = 1:numel(theta)
    det = R * [cos(theta(j)); sin(theta(j))];
    d = norm(y - det);
    R2j = R^2;  r2 = dot(y, y);  rb2 = rb^2;
    dotp = dot(det, y) - R2j;
    dist2 = (r2 - R2j) - 2 * dotp;
    dist = sqrt(max(dist2, 0));
    tf = dist * fs / c2;
    bothIn = (R2j <= rb2) && (r2 <= rb2);
    discr4 = dotp^2 - dist2 * (R2j - rb2);
    cross = discr4 > 0;
    sd = sqrt(max(discr4, 0));
    dist2d = max(dist2, 1e-12);
    u1 = (-dotp - sd) / dist2d;
    u2 = (-dotp + sd) / dist2d;
    Lc = max((min(max(u2, 0), 1) - max(min(u1, 1), 0)) * dist, 0);
    Li = dist * double(bothIn) + Lc * double(cross && ~bothIn);
    tau(j) = tf + Li * fs * (1 / c1 - 1 / c2);
end
end

function img = reconLayered(p, theta, R, rb, c1, c2, X, Y, fs, minDist, dtheta)
% 分层走时 + Δθ·cosα/d 权重 + acc/accW（结构同 DAS，走时换分层）
[nY, nX] = size(X);
tau = layeredTau(X, Y, theta, R, rb, c1, c2, fs);   % [npix x nd]
r2 = X(:).^2 + Y(:).^2;
acc = zeros(numel(X), 1); accW = zeros(numel(X), 1);
for j = 1:numel(theta)
    detx = R * cos(theta(j));  dety = R * sin(theta(j));
    R2j = R^2;
    proj = X(:) * detx + Y(:) * dety;
    dotp = proj - R2j;
    dist = sqrt(max((r2 - R2j) - 2 * dotp, 0));
    dsafe = max(dist, minDist);
    tf = tau(:, j);
    Nt = size(p, 1);
    i0f = floor(tf);  frac = tf - i0f;
    i0 = i0f + 1;
    valid = (i0 >= 1) & (i0 <= Nt - 1);
    i0c = min(max(i0, 1), Nt - 1);
    v0 = p(i0c, j);  v1 = p(min(i0c + 1, Nt), j);
    v = v0 + frac .* (v1 - v0);
    v(~valid) = 0;
    w = dtheta * (-dotp) ./ (R * dsafe);
    acc = acc + w .* v;
    accW = accW + abs(w);
end
img = reshape(acc ./ max(accW, 1e-12), size(X));
end

function [posErr, amp] = peakAt(img, gv, yk)
[~, icx] = min(abs(gv - yk(1)));
[~, icy] = min(abs(gv - yk(2)));
w = 8;
x1 = max(1, icx - w):min(numel(gv), icx + w);
y1 = max(1, icy - w):min(numel(gv), icy + w);
sub = img(y1, x1);
[am, im] = max(sub(:));
[iyy, ixx] = ind2sub(size(sub), im);
px = gv(x1(ixx));  py = gv(y1(iyy));
posErr = hypot(px - yk(1), py - yk(2));
amp = am;
end
