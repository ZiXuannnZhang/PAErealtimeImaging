% EXP2_RING_UBP_2D  判定性实验 2：2D 环形几何下 DAS（q=1）与环-UBP 的端到端对比。
%
% 目的
% ----
% 1) 在与生产一致的环形几何/采样参数下，用闭式 2D 前向模型生成逐线数据；
% 2) 分别用 (a) 现有 DAS q=1、(b) 环-UBP（Xu-Wang Eq.(20)-(22) 信号组合
%    b = 2p − 2t·p' + 立体角权重 ΔΩ = Δθ·R·cosα/d²，两种归一化 P/D）、
%    (c) 错误配对（UBP 信号 + 旧 DAS q=1 权重）重建；
% 3) 用均匀盘标定 UBP 绝对常数，比较：盘内平坦度、高斯吸收体位置误差、
%    FWHM、背景伪影、CNR。
%
% 2D 前向模型（闭式，紧支撑）
%   均匀盘（半径 a）：圆均值 M(rho0,r) = f0·γ/π，γ=acos(clamp((rho0²+r²−a²)/(2ρ0r)))
%   高斯吸收体（σ, 幅度 A, 位置 yk）：M(x0,r) = Σ A·exp(−(r²+ρk²)/2σ²)·I0(r·ρk/σ²)
%   p(x0,t) = d/dt[ t·G(t) ]，G(t)=∫0^{π/2} M(x0, c t sinψ) sinψ dψ
%
% 运行：matlab -batch "run('exp2_ring_ubp_2d.m')"
% 输出：evidence/exp2_ring_ubp_2d.json、exp2_images.mat

outdir = fullfile(fileparts(mfilename('fullpath')), '..', 'evidence');
if ~exist(outdir, 'dir'), mkdir(outdir); end

% ---- 生产一致参数 ----
fs = 250e6;  c = 1490.0;  R = 6.57e-3;
nd = 360;                    % A-line 数（生产 4000/圈，此处减规模）
Nt = 4000;                   % 采样深度
dr = c / fs;                 % 采样间隔 [m]
rGrid = (0:Nt-1)' * dr;
fov = 36e-3;  gridSize = 0.2e-3;  minDist = gridSize;
theta = (0:nd-1)' * (2 * pi / nd);
dtheta = 2 * pi / nd;

nG = round(fov / gridSize) + 1;
gv = (-fov/2):gridSize:(fov/2);
[X, Y] = meshgrid(gv, gv);

% ================= 幻体 A：均匀盘 =================
aDisc = 6e-3;  f0Disc = 1.0;
M0 = discMean(R, rGrid, aDisc) * f0Disc;
pA = forwardP(M0, rGrid, c);
pAd = pA * ones(1, nd);
ppdA = gradient(double(pAd), dr / c, 1);          % dp/dt [Pa/s]，沿时间维(1)，端点单侧          % dp/dt [Pa/s]，端点单侧

[accD_ubp, accWd_ubp] = reconUBP(pAd, ppdA, theta, R, X, Y, fs, c, minDist, dtheta);
imgD_das = reconDAS(pAd, theta, R, X, Y, fs, c, minDist, dtheta);
imgD_wrong = normAcc(reconUBPwrong(pAd, ppdA, theta, R, X, Y, fs, c, minDist, dtheta));

roi = (X.^2 + Y.^2) < (4e-3)^2;
C_P = f0Disc / mean(accD_ubp(roi));            % P 归一化常数（原始累加标定）
imgD_ubpP = C_P * accD_ubp;
imgD_ubpD = normAcc(accD_ubp, accWd_ubp);
C_D = f0Disc / mean(imgD_ubpD(roi));           % D 归一化标定常数
imgD_ubpD = C_D * imgD_ubpD;

% ================= 幻体 B：孤立高斯吸收体（每个单独前向，避免串扰） =================
sig = 0.5e-3;
gauss = [0 0 1.0; 3e-3 0 0.7; 0 -5e-3 0.5; -5e-3 5e-3 0.6];
nAbs = size(gauss, 1);
imgG_das = cell(nAbs, 1);  imgG_ubpP = cell(nAbs, 1);
imgG_ubpD = cell(nAbs, 1); imgG_wrong = cell(nAbs, 1);
for k = 1:nAbs
    pB = zeros(Nt, nd);
    for j = 1:nd
        rhoK = norm(gauss(k, 1:2)' - [R * cos(theta(j)); R * sin(theta(j))]);
        % exp(−(r²+ρ²)/2σ²)·I0(rρ/σ²) = exp(−(r−ρ)²/2σ²)·[I0(z)e^{−z}]，z=rρ/σ²
        % 缩放贝塞尔 besseli(0,z,1)=I0(z)e^{−z} 避免远场溢出
        Mk = gauss(k, 3) * exp(-(rGrid - rhoK).^2 / (2 * sig^2)) .* ...
             besseli(0, rGrid * rhoK / sig^2, 1);
        pB(:, j) = forwardP(Mk, rGrid, c);
    end
    ppdB = gradient(double(pB), dr / c, 1);
    [accG, accWg] = reconUBP(pB, ppdB, theta, R, X, Y, fs, c, minDist, dtheta);
    imgG_das{k} = reconDAS(pB, theta, R, X, Y, fs, c, minDist, dtheta);
    imgG_wrong{k} = normAcc(reconUBPwrong(pB, ppdB, theta, R, X, Y, fs, c, minDist, dtheta));
    imgG_ubpP{k} = C_P * accG;
    imgG_ubpD{k} = C_D * normAcc(accG, accWg);
end

% ================= 指标 =================
res = struct();
res.discDas = flatness(imgD_das, roi);
res.discUbpP = flatness(imgD_ubpP, roi);
res.discUbpD = flatness(imgD_ubpD, roi);
res.discWrong = flatness(imgD_wrong, roi);
res.C_P = C_P;  res.C_D = C_D;
res.edgeProfile = struct('x', gv, 'das', imgD_das(round(size(imgD_das,1)/2),:)', ...
    'ubpP', imgD_ubpP(round(size(imgD_ubpP,1)/2),:)', 'ubpD', imgD_ubpD(round(size(imgD_ubpD,1)/2),:)');

% 点指标：孤立幻体、±2mm 窗口
pts = struct('mode', {}, 'k', {}, 'posErrMm', {}, 'fwhmTanMm', {}, 'fwhmRadMm', {}, 'amp', {});
modes = {'das', imgG_das; 'ubpP', imgG_ubpP; 'ubpD', imgG_ubpD; 'wrong', imgG_wrong};
for mi = 1:size(modes, 1)
    for k = 1:nAbs
        img = modes{mi, 2}{k};
        [pe, ft, fr, amp] = pointMetrics(img, gv, gauss(k, 1:2), 2e-3);
        pts(end+1) = struct('mode', modes{mi,1}, 'k', k, 'posErrMm', pe, ...
            'fwhmTanMm', ft, 'fwhmRadMm', fr, 'amp', amp); %#ok<AGROW>
    end
end
res.points = pts;

% 伪影度量：远场环带（8–12mm，避开探测器环 ±1.5mm）与近场环带（|r−R|<1.5mm）
rr = sqrt(X.^2 + Y.^2);
farMask = (rr > 8e-3) & (rr < 12e-3);
nearMask = abs(rr - R) < 1.5e-3;
res.artifact = struct();
fields = {'das', imgG_das; 'ubpP', imgG_ubpP; 'ubpD', imgG_ubpD; 'wrong', imgG_wrong};
for fi = 1:size(fields, 1)
    % 伪影用点3（孤立、幅度 0.5）的图像度量
    img = fields{fi, 2}{3};
    res.artifact.(sprintf('%sFarStd', fields{fi,1})) = std(img(farMask));
    res.artifact.(sprintf('%sFarMean', fields{fi,1})) = mean(img(farMask));
    res.artifact.(sprintf('%sNearStd', fields{fi,1})) = std(img(nearMask));
    res.artifact.(sprintf('%sNearMax', fields{fi,1})) = max(abs(img(nearMask)));
end
% CNR：点2 峰值 / 远场 std（各模式自洽比较）
for fi = 1:size(fields, 1)
    img = fields{fi, 2}{2};
    [~, ~, ~, ampPk] = pointMetrics(img, gv, gauss(2, 1:2), 2e-3);
    res.cnr.(fields{fi,1}) = ampPk / max(std(img(farMask)), eps);
end

fprintf('\n=== EXP2 均匀盘内平坦度（f0=1，ROI 半径<4mm）===\n');
fprintf('DAS q=1       : mean=%.4f std=%.4f CV=%.4f\n', res.discDas.mean, res.discDas.std, res.discDas.cv);
fprintf('UBP-P（原始+C）: mean=%.4f std=%.4f CV=%.4f (C_P=%.4g)\n', res.discUbpP.mean, res.discUbpP.std, res.discUbpP.cv, C_P);
fprintf('UBP-D（accW+C）: mean=%.4f std=%.4f CV=%.4f (C_D=%.4g)\n', res.discUbpD.mean, res.discUbpD.std, res.discUbpD.cv, C_D);
fprintf('UBP+旧q1权重  : mean=%.4f std=%.4f CV=%.4f\n', res.discWrong.mean, res.discWrong.std, res.discWrong.cv);
fprintf('\n=== EXP2 孤立高斯点（σ=0.5mm，±2mm 窗）：位置误差[mm] / 切向FWHM / 径向FWHM / 峰值 ===\n');
for k = 1:numel(pts)
    fprintf('%-6s 点%d: posErr=%.3f fwhmT=%.2f fwhmR=%.2f amp=%.3f\n', ...
        pts(k).mode, pts(k).k, pts(k).posErrMm, pts(k).fwhmTanMm, pts(k).fwhmRadMm, pts(k).amp);
end
fprintf('\n伪影（点3 幻体）：远场环带 8-12mm std / 近场 |r-R|<1.5mm max|img|\n');
fprintf('DAS  : farStd=%.4g nearMax=%.4g\n', res.artifact.dasFarStd, res.artifact.dasNearMax);
fprintf('UBP-P: farStd=%.4g nearMax=%.4g\n', res.artifact.ubpPFarStd, res.artifact.ubpPNearMax);
fprintf('UBP-D: farStd=%.4g nearMax=%.4g\n', res.artifact.ubpDFarStd, res.artifact.ubpDNearMax);
fprintf('wrong: farStd=%.4g nearMax=%.4g\n', res.artifact.wrongFarStd, res.artifact.wrongNearMax);
fprintf('CNR（点2峰值/远场std）：DAS=%.1f UBP-P=%.1f UBP-D=%.1f wrong=%.1f\n', ...
    res.cnr.das, res.cnr.ubpP, res.cnr.ubpD, res.cnr.wrong);

save(fullfile(outdir, 'exp2_images.mat'), 'imgD_das', 'imgD_ubpP', 'imgD_ubpD', 'imgD_wrong', ...
    'imgG_das', 'imgG_ubpP', 'imgG_ubpD', 'imgG_wrong', 'gv', 'gauss', 'aDisc', 'C_P', 'C_D', 'res');
out = res;
out.params = struct('fs', fs, 'c', c, 'R', R, 'nd', nd, 'Nt', Nt, 'fov', fov, ...
    'gridSize', gridSize, 'minDist', minDist, 'sig', sig, 'gauss', gauss, 'f0Disc', f0Disc, 'aDisc', aDisc);
out.matlabVersion = version;
fid = fopen(fullfile(outdir, 'exp2_ring_ubp_2d.json'), 'w');
fwrite(fid, jsonencode(out, 'PrettyPrint', true)); fclose(fid);
fprintf('EXP2_DONE -> %s\n', fullfile(outdir, 'exp2_ring_ubp_2d.json'));

% ================= 局部函数 =================
function M = discMean(rho0, r, a)
% 均匀盘圆均值 = 盘内弧长分数（f0=1）
gamma_ = acos(min(max((rho0^2 + r.^2 - a^2) ./ (2 * rho0 * r), -1), 1));
M = gamma_ / pi;
end

function p = forwardP(M, rGrid, c)
% 2D Poisson：p = d/dt[t*G(t)]，G(t) = ∫0^{π/2} M(c t sinψ) sinψ dψ
nPsi = 1024;
psi = linspace(0, pi / 2, nPsi);
tGrid = rGrid / c;
Mpsi = interp1(rGrid, M, c * tGrid(:) * sin(psi), 'linear', 0);
G = trapz(psi, Mpsi .* sin(psi), 2);
p = gradient(tGrid(:) .* G, tGrid);
end

function img = reconDAS(p, theta, R, X, Y, fs, c, minDist, dtheta)
% 现有 DAS q=1：w = Δθ·cosα/d，acc/accW 归一化
[nY, nX] = size(X);
acc = zeros(nY, nX); accW = zeros(nY, nX);
r2 = X.^2 + Y.^2;
for j = 1:size(p, 2)
    proj = X * (R * cos(theta(j))) + Y * (R * sin(theta(j)));
    dotp = proj - R^2;
    dist = sqrt(max((r2 - R^2) - 2 * dotp, 0));
    dsafe = max(dist, minDist);
    tf = dist * fs / c;
    v = interpLine(p(:, j), tf);
    w = dtheta * (-dotp) ./ (R * dsafe);       % Δθ·cosα/d
    acc = acc + w .* v;
    accW = accW + abs(w);
end
img = acc ./ max(accW, 1e-12);
end

function [acc, accW] = reconUBP(p, pp, theta, R, X, Y, fs, c, minDist, dtheta)
% 环-UBP（立体角权重，每单位 z 长度）：acc = Σ Δθ·R·cosα/d² · b(τ)
% b = 2p − 2t·p'，t = τ/fs [s]。返回原始累加（不做逐像素归一化）。
[nY, nX] = size(X);
acc = zeros(nY, nX); accW = zeros(nY, nX);
r2 = X.^2 + Y.^2;
for j = 1:size(p, 2)
    proj = X * (R * cos(theta(j))) + Y * (R * sin(theta(j)));
    dotp = proj - R^2;
    dist = sqrt(max((r2 - R^2) - 2 * dotp, 0));
    dsafe = max(dist, minDist);
    tf = dist * fs / c;
    tsec = tf / fs;
    v = interpLine(p(:, j), tf);
    vp = interpLine(pp(:, j), tf);
    b = 2 * v - 2 * tsec .* vp;
    cosAlpha = (-dotp) ./ (R * dsafe);
    w = dtheta * R * cosAlpha ./ (dsafe.^2);   % ΔΩ/dz = Δθ·R·cosα/d²
    acc = acc + w .* b;
    accW = accW + abs(w);
end
end

function acc = reconUBPwrong(p, pp, theta, R, X, Y, fs, c, minDist, dtheta)
% 错误配对：UBP 信号 b + 旧 DAS q=1 权重（仅换输入不换权重的失败模式）
[nY, nX] = size(X);
acc = zeros(nY, nX);
r2 = X.^2 + Y.^2;
for j = 1:size(p, 2)
    proj = X * (R * cos(theta(j))) + Y * (R * sin(theta(j)));
    dotp = proj - R^2;
    dist = sqrt(max((r2 - R^2) - 2 * dotp, 0));
    dsafe = max(dist, minDist);
    tf = dist * fs / c;
    tsec = tf / fs;
    v = interpLine(p(:, j), tf);
    vp = interpLine(pp(:, j), tf);
    b = 2 * v - 2 * tsec .* vp;
    cosAlpha = (-dotp) ./ (R * dsafe);
    w = dtheta * cosAlpha ./ dsafe;            % Δθ·cosα/d（错误配对）
    acc = acc + w .* b;
end
end

function img = normAcc(acc, accW)
if nargin < 2, img = acc ./ max(abs(acc), 1e-12); return; end
img = acc ./ max(accW, 1e-12);
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

function s = flatness(img, roi)
m = mean(img(roi));
s = struct('mean', m, 'std', std(img(roi)), 'cv', std(img(roi)) / max(abs(m), eps));
end

function [posErr, fwhmTan, fwhmRad, amp] = pointMetrics(img, gv, yk, winM)
[~, icx] = min(abs(gv - yk(1)));
[~, icy] = min(abs(gv - yk(2)));
w = max(3, round(winM / (gv(2) - gv(1))));
x1 = max(1, icx - w):min(numel(gv), icx + w);
y1 = max(1, icy - w):min(numel(gv), icy + w);
sub = img(y1, x1);
[~, im] = max(sub(:));
[iyy, ixx] = ind2sub(size(sub), im);
px = gv(x1(ixx));  py = gv(y1(iyy));
posErr = hypot(px - yk(1), py - yk(2)) * 1e3;
amp = sub(iyy, ixx);
fwhmTan = fwhm1d(squeeze(sub(iyy, :)), gv(x1), amp);
fwhmRad = fwhm1d(squeeze(sub(:, ixx)), gv(y1), amp);
end

function w = fwhm1d(prof, x, peak)
half = peak / 2;
idx = find(prof >= half);
if numel(idx) < 2, w = NaN; return; end
i1 = idx(1); i2 = idx(end);
if i1 > 1
    xa = x(i1 - 1) + (half - prof(i1 - 1)) * (x(i1) - x(i1 - 1)) / (prof(i1) - prof(i1 - 1));
else
    xa = x(i1);
end
if i2 < numel(x)
    xb = x(i2) + (half - prof(i2)) * (x(i2 + 1) - x(i2)) / (prof(i2 + 1) - prof(i2));
else
    xb = x(i2);
end
w = (xb - xa) * 1e3;
end
