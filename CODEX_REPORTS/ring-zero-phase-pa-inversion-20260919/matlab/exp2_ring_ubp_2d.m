% EXP2_RING_UBP_2D  判定性实验 2：2D 环形几何下 DAS（q=1）与环-UBP 的端到端对比。
% 【审查整改版】R1/R2 修复 + 指标方向化，重跑后全部结论重新计算。
%
% 相对 9493962 版的修复（TASKS/环形成像阶段A审查整改追加_20260919-201935.md）
% ----
% R1 时间导数：ppdA/ppdB 改用 timeDerivative（逐列中心差分/端点单侧，dim 1）。
%   旧版 gradient(matrix, dt, 1) 第三参数被当作 dim2 间距、单输出返回列向梯度：
%   均匀盘（各列相同）导数为零、偏心目标得到跨 A-line 差分。导数约定由
%   test_time_derivative.m 自动验证。
% R2 公平对照：旧 "wrong" 模式（reconUBPwrong 仅返回 acc + 单参数 normAcc
%   自归一化）退化为 sign(acc) 符号图，其误差/FWHM 不可归因于权重。现改为
%   legacyWeight 模式：与 DAS 完全相同的 acc/accW 显式归一化（reconPairKernels
%   共享实现，accW 逐元素一致由 test_fair_comparison.m 验证），五模式构成
%   因子分解（信号 × 权重族 × 归一化），每次只改变一个因素。
% 指标：FWHM 沿目标相对环心的实际径向/切向方向采样（不再固定 x/y 剖面），
%   无半高交点/峰在窗边界时标记不可用；环内/环外目标分开汇总。
% §4.6 导数主导性：以修正后的导数重新实测（RMS 比），不沿用旧 2–3 量级主张。
%
% 五模式（除注明因素外，输入信号/几何/插值/掩码/角度采样/测量方法完全一致）：
%   das     = p 信号 + q=1 权重(Δθ·cosα/d)      + accW 归一化（生产旧默认基准）
%   legacyW = b 信号 + q=1 权重                  + accW 归一化（旧权重配对对照）
%   ubpD    = b 信号 + 立体角权重(Δθ·R·cosα/d²)  + accW 归一化（拟定配对）
%   ubpP    = b 信号 + 立体角权重                + 原始和式 + C_P 标定（物理标度另列）
%   saP     = p 信号 + 立体角权重                + accW 归一化（因子分解补格）
%   其中 b = 2p − 2t·p'，t = τ/fs [s]。
%
% 2D 前向模型（闭式，紧支撑，未改动）
%   均匀盘（半径 a）：圆均值 M(rho0,r) = f0·γ/π，γ=acos(clamp((ρ0²+r²−a²)/(2ρ0r)))
%   高斯吸收体（σ, 幅度 A, 位置 yk）：M(x0,r) = Σ A·exp(−(r²+ρk²)/2σ²)·I0(r·ρk/σ²)
%   p(x0,t) = d/dt[ t·G(t) ]，G(t)=∫0^{π/2} M(x0, c t sinψ) sinψ dψ
%
% 运行：matlab -batch "run('exp2_ring_ubp_2d.m')"   （工作目录 = matlab/）
% 输出：evidence/exp2_ring_ubp_2d.json、evidence/exp2_images.mat

outdir = fullfile(fileparts(mfilename('fullpath')), '..', 'evidence');
if ~exist(outdir, 'dir'), mkdir(outdir); end
K = reconPairKernels();

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
ppdA = timeDerivative(pAd, 1 / fs);        % R1 修复：dp/dt [Pa/s]，dim 1 逐列

[accA, accWA] = K.reconUBP(pAd, ppdA, theta, R, X, Y, fs, c, minDist, dtheta);
[accAD, accWAD] = K.reconDAS(pAd, theta, R, X, Y, fs, c, minDist, dtheta);
[accAL, accWAL] = K.reconLegacyWeight(pAd, ppdA, theta, R, X, Y, fs, c, minDist, dtheta);
[accAS, accWAS] = K.reconSolidAngleP(pAd, theta, R, X, Y, fs, c, minDist, dtheta);

roi = (X.^2 + Y.^2) < (4e-3)^2;
C_P = f0Disc / mean(accA(roi));             % P 归一化常数（原始累加标定）
imgD_ubpP = C_P * accA;
imgD_ubpD0 = K.normAccW(accA, accWA);
C_D = f0Disc / mean(imgD_ubpD0(roi));       % D 归一化标定常数
imgD_ubpD = C_D * imgD_ubpD0;
imgD_das = K.normAccW(accAD, accWAD);
imgD_legacyW = K.normAccW(accAL, accWAL);
imgD_saP = K.normAccW(accAS, accWAS);

% ================= 幻体 B：孤立高斯吸收体（每个单独前向，避免串扰） =================
sig = 0.5e-3;
gauss = [0 0 1.0; 3e-3 0 0.7; 0 -5e-3 0.5; -5e-3 5e-3 0.6];
nAbs = size(gauss, 1);
imgDas = cell(nAbs,1);  imgLegacyW = cell(nAbs,1);  imgUbpD = cell(nAbs,1);
imgUbpP = cell(nAbs,1);  imgSaP = cell(nAbs,1);
imgG.p = cell(nAbs,1);  imgG.pp = cell(nAbs,1);   % 保留前向与导数供主导性/复检
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
    ppB = timeDerivative(pB, 1 / fs);      % R1 修复
    imgG.p{k} = pB;  imgG.pp{k} = ppB;
    [accB, accWB] = K.reconUBP(pB, ppB, theta, R, X, Y, fs, c, minDist, dtheta);
    [accBD, accWBD] = K.reconDAS(pB, theta, R, X, Y, fs, c, minDist, dtheta);
    [accBL, accWBL] = K.reconLegacyWeight(pB, ppB, theta, R, X, Y, fs, c, minDist, dtheta);
    [accBS, accWBS] = K.reconSolidAngleP(pB, theta, R, X, Y, fs, c, minDist, dtheta);
    imgDas{k} = K.normAccW(accBD, accWBD);                % das
    imgLegacyW{k} = K.normAccW(accBL, accWBL);            % legacyW
    imgUbpD{k} = C_D * K.normAccW(accB, accWB);           % ubpD
    imgUbpP{k} = C_P * accB;                              % ubpP
    imgSaP{k} = K.normAccW(accBS, accWBS);                % saP
end
modes = {'das', imgDas; 'legacyW', imgLegacyW; 'ubpD', imgUbpD; ...
         'ubpP', imgUbpP; 'saP', imgSaP};

% ================= 指标 =================
res = struct();
res.discDas = flatness(imgD_das, roi);
res.discLegacyW = flatness(imgD_legacyW, roi);
res.discUbpD = flatness(imgD_ubpD, roi);
res.discUbpP = flatness(imgD_ubpP, roi);
res.discSaP = flatness(imgD_saP, roi);
res.C_P = C_P;  res.C_D = C_D;
midRow = round(size(imgD_das, 1) / 2);
res.edgeProfile = struct('x', gv, 'das', imgD_das(midRow, :)', ...
    'legacyW', imgD_legacyW(midRow, :)', 'ubpD', imgD_ubpD(midRow, :)', ...
    'ubpP', imgD_ubpP(midRow, :)', 'saP', imgD_saP(midRow, :)');

% 点指标：孤立幻体、±2mm 窗口；FWHM 沿相对环心的实际径向/切向方向
% 环内：k1(0,0) k2(3,0) k3(0,−5)；环外：k4(−5,5)（|r|=7.07mm > R=6.57mm）
pts = struct('mode', {}, 'k', {}, 'posErrMm', {}, 'fwhmRadMm', {}, ...
    'fwhmTanMm', {}, 'fwhmRadValid', {}, 'fwhmTanValid', {}, ...
    'fwhmRadReason', {}, 'fwhmTanReason', {}, 'amp', {}, 'inRing', {});
for mi = 1:size(modes, 1)
    for k = 1:nAbs
        img = modes{mi, 2}{k};
        [pe, fr, ft, amp, vr, vt] = pointMetrics(img, gv, gauss(k, 1:2), 2e-3, R);
        inRing = norm(gauss(k, 1:2)) <= R;
        pts(end+1) = struct('mode', modes{mi, 1}, 'k', k, 'posErrMm', pe, ...
            'fwhmRadMm', fr, 'fwhmTanMm', ft, 'fwhmRadValid', vr, 'fwhmTanValid', vt, ...
            'fwhmRadReason', fwhmReason(vr), 'fwhmTanReason', fwhmReason(vt), ...
            'amp', amp, 'inRing', inRing); %#ok<AGROW>
    end
end
res.points = pts;

% 伪影度量：远场环带（8–12mm，避开探测器环 ±1.5mm）与近场环带（|r−R|<1.5mm）
rr = sqrt(X.^2 + Y.^2);
farMask = (rr > 8e-3) & (rr < 12e-3);
nearMask = abs(rr - R) < 1.5e-3;
res.artifact = struct();
for mi = 1:size(modes, 1)
    img = modes{mi, 2}{3};                 % 伪影用点3（孤立、幅度 0.5）
    nm = modes{mi, 1};
    res.artifact.([nm 'FarStd']) = std(img(farMask));
    res.artifact.([nm 'FarMean']) = mean(img(farMask));
    res.artifact.([nm 'NearStd']) = std(img(nearMask));
    res.artifact.([nm 'NearMax']) = max(abs(img(nearMask)));
end
% CNR：点2 峰值 / 远场 std（各模式自洽比较）
res.cnr = struct();
for mi = 1:size(modes, 1)
    img = modes{mi, 2}{2};
    [~, ~, ~, ampPk] = pointMetrics(img, gv, gauss(2, 1:2), 2e-3, R);
    res.cnr.(modes{mi, 1}) = ampPk / max(std(img(farMask)), eps);
end

% §4.6 导数主导性实测（R5：修正导数后重新测量，替代旧 2–3 量级主张）
res.derivDominance = struct();
res.derivDominance.disc = derivDominance(pAd, ppdA, X, Y, roi, theta, R, fs, c, 10, K.interpLine);
for k = 1:nAbs
    res.derivDominance.(sprintf('point%d', k)) = ...
        derivDominance(imgG.p{k}, imgG.pp{k}, X, Y, roi, theta, R, fs, c, 10, K.interpLine);
end

% ================= 控制台摘要 =================
fprintf('\n=== EXP2 均匀盘内平坦度（f0=1，ROI 半径<4mm）===\n');
fprintf('DAS q=1        : mean=%.4f std=%.4f CV=%.4f\n', res.discDas.mean, res.discDas.std, res.discDas.cv);
fprintf('legacyW(b+q1)  : mean=%.4f std=%.4f CV=%.4f\n', res.discLegacyW.mean, res.discLegacyW.std, res.discLegacyW.cv);
fprintf('UBP-D（accW+C）: mean=%.4f std=%.4f CV=%.4f (C_D=%.4g)\n', res.discUbpD.mean, res.discUbpD.std, res.discUbpD.cv, C_D);
fprintf('UBP-P（原始+C）: mean=%.4f std=%.4f CV=%.4f (C_P=%.4g)\n', res.discUbpP.mean, res.discUbpP.std, res.discUbpP.cv, C_P);
fprintf('saP(p+立体角)  : mean=%.4f std=%.4f CV=%.4f\n', res.discSaP.mean, res.discSaP.std, res.discSaP.cv);
fprintf('\n=== EXP2 孤立高斯点（σ=0.5mm，±2mm 窗）：posErr[mm] / 径向FWHM / 切向FWHM / 峰值 ===\n');
fprintf('（FWHM 沿目标相对环心实际方向采样；valid=0 时数值不可用，原因见 reason）\n');
for k = 1:numel(pts)
    fprintf('%-8s 点%d(环内=%d): posErr=%.3f fwhmR=%.3f(%d,%s) fwhmT=%.3f(%d,%s) amp=%.3f\n', ...
        pts(k).mode, pts(k).k, pts(k).inRing, pts(k).posErrMm, ...
        pts(k).fwhmRadMm, pts(k).fwhmRadValid, pts(k).fwhmRadReason, ...
        pts(k).fwhmTanMm, pts(k).fwhmTanValid, pts(k).fwhmTanReason, pts(k).amp);
end
fprintf('\n伪影（点3 幻体）：远场环带 8-12mm std / 近场 |r-R|<1.5mm max|img|\n');
for mi = 1:size(modes, 1)
    nm = modes{mi, 1};
    fprintf('%-8s: farStd=%.4g nearMax=%.4g\n', nm, ...
        res.artifact.([nm 'FarStd']), res.artifact.([nm 'NearMax']));
end
fprintf('CNR（点2峰值/远场std）：');
for mi = 1:size(modes, 1)
    fprintf(' %s=%.1f', modes{mi, 1}, res.cnr.(modes{mi, 1}));
end
fprintf('\n导数主导性（查询处 RMS|2t*p''|/RMS|2p|）：盘=%.2f', res.derivDominance.disc);
for k = 1:nAbs
    fprintf(' 点%d=%.2f', k, res.derivDominance.(sprintf('point%d', k)));
end
fprintf('\n');

% ================= 正确性断言门（不含预设优劣结论） =================
gate.calibP = abs(mean(imgD_ubpP(roi)) - f0Disc) <= 1e-6;
gate.calibD = abs(mean(imgD_ubpD(roi)) - f0Disc) <= 1e-6;
% legacyW 不再是符号图：旧版 discWrong cv=0（常数 ±1）；修正后 CV 必须非零
gate.legacyNotSign = res.discLegacyW.cv >= 0.01;
% 幅值信息保留：legacyW 点目标峰值/背景（±2mm 窗内 median|img|）≥ 3
% （符号图该比值 = 1，见 test_fair_comparison U4 反证）
legContrast = zeros(1, nAbs);
winAll = zeros(size(gv));
for k = 1:nAbs
    win = windowMask(gv, gauss(k, 1:2), 2e-3);
    legContrast(k) = max(imgLegacyW{k}(win)) / max(median(abs(imgLegacyW{k}(win))), eps);
end
gate.legacyContrast = all(legContrast >= 3);
% 中心目标（对称、无几何偏差）位置误差 ≤ 0.3mm（1.5 网格），对 das/ubpD/ubpP/saP
posCenter = nan(size(modes, 1), 1);
for mi = 1:size(modes, 1)
    sel = strcmp({pts.mode}, modes{mi, 1}) & [pts.k] == 1;
    posCenter(mi) = pts(sel).posErrMm;
end
gate.centerPos = all(posCenter([1 3 4 5]) <= 0.3);   % 中心项：das/ubpD/ubpP/saP
% 全部图像有限
fin = all(isfinite(imgD_das(:))) && all(isfinite(imgD_legacyW(:))) && ...
    all(isfinite(imgD_ubpD(:))) && all(isfinite(imgD_ubpP(:))) && all(isfinite(imgD_saP(:)));
for mi = 1:size(modes, 1)
    for k = 1:nAbs
        fin = fin && all(isfinite(modes{mi, 2}{k}(:)));
    end
end
gate.finite = fin;
res.gate = gate;
res.centerPosErrMmByMode = struct('modes', {modes(1,:)}, 'posErrMm', posCenter);
res.legacyContrastPerPoint = legContrast;

fprintf('\n断言门：标定P=%d 标定D=%d legacyW非符号图=%d legacyW对比度=%d 中心位置=%d 有限=%d\n', ...
    gate.calibP, gate.calibD, gate.legacyNotSign, gate.legacyContrast, gate.centerPos, gate.finite);

save(fullfile(outdir, 'exp2_images.mat'), 'imgD_das', 'imgD_legacyW', 'imgD_ubpD', ...
    'imgD_ubpP', 'imgD_saP', 'imgG', 'gv', 'gauss', 'aDisc', 'C_P', 'C_D', 'res', 'pts', ...
    'modes', '-v7');

out = res;
out.params = struct('fs', fs, 'c', c, 'R', R, 'nd', nd, 'Nt', Nt, 'fov', fov, ...
    'gridSize', gridSize, 'minDist', minDist, 'sig', sig, 'gauss', gauss, ...
    'f0Disc', f0Disc, 'aDisc', aDisc);
out.modesDescription = struct( ...
    'das', 'p + q1 weight (dtheta*cosAlpha/d) + accW norm (production legacy baseline)', ...
    'legacyW', 'b + q1 weight + accW norm (fair legacy-weight pairing, was "wrong")', ...
    'ubpD', 'b + solid-angle weight (dtheta*R*cosAlpha/d^2) + accW norm (proposed pairing)', ...
    'ubpP', 'b + solid-angle weight + raw-sum with C_P calibration (physical scale, separate)', ...
    'saP', 'p + solid-angle weight + accW norm (factorial completion)');
out.fixedFactors = 'same input signals p/pp, geometry, interpolation, maskOob, angle sampling, amplitude calibration and metrics across all modes; only the stated factor differs';
out.derivativeConvention = 'dF/dt along dim1 via timeDerivative.m: central difference interior, one-sided ends, per-column; verified by test_time_derivative.m';
out.bDefinition = 'b = 2*p - 2*t*dp/dt with t = tau/fs [s]';
out.fwhmConvention = 'sampled along actual radial (center->peak) and tangential directions; invalid when no half-height crossing or peak at window edge';
out.matlabVersion = version;
fid = fopen(fullfile(outdir, 'exp2_ring_ubp_2d.json'), 'w');
fwrite(fid, jsonencode(out, 'PrettyPrint', true)); fclose(fid);
fprintf('EXP2_DONE -> %s\n', fullfile(outdir, 'exp2_ring_ubp_2d.json'));

gateOk = gate.calibP && gate.calibD && gate.legacyNotSign && gate.legacyContrast && ...
    gate.centerPos && gate.finite;
assert(gateOk, 'exp2:gateFailed', 'EXP2 正确性断言门未全部通过');

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
p = gradient(tGrid(:) .* G, tGrid);   % 向量用法（非矩阵导数，不在 R1 误用范围）
end

function s = flatness(img, roi)
m = mean(img(roi));
s = struct('mean', m, 'std', std(img(roi)), 'cv', std(img(roi)) / max(abs(m), eps));
end

function mask = windowMask(gv, yk, winM)
[~, icx] = min(abs(gv - yk(1)));
[~, icy] = min(abs(gv - yk(2)));
w = max(3, round(winM / (gv(2) - gv(1))));
x1 = max(1, icx - w):min(numel(gv), icx + w);
y1 = max(1, icy - w):min(numel(gv), icy + w);
mask = false(numel(gv), numel(gv));
mask(y1, x1) = true;
end

function [posErr, fwhmRad, fwhmTan, amp, radValid, tanValid] = pointMetrics(img, gv, yk, winM, Rring)
% 窗内找峰；径向 = 环心→峰方向，切向 = 垂直方向（沿实际方向采样剖面）。
% 环心目标（|pos|≈0）径向方向任意，取 x 方向并在 reason 注明。
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
% 径向方向：环心(0,0)→峰位；中心峰方向未定义时取 x 方向
uRad = [px; py];
if norm(uRad) < gv(2) - gv(1)
    uRad = [1; 0];  % 中心：径向方向任意，约定取 x
end
uRad = uRad / norm(uRad);
uTan = [-uRad(2); uRad(1)];
p0 = [px; py];
stepM = gv(2) - gv(1);
halfLen = (w) * stepM;
[vR, sR] = sampleAlong(img, gv, p0, uRad, halfLen, stepM);
[vT, sT] = sampleAlong(img, gv, p0, uTan, halfLen, stepM);
[fwhmRad, radValid] = fwhmFromProfile(vR, sR, amp, halfLen);
[fwhmTan, tanValid] = fwhmFromProfile(vT, sT, amp, halfLen);
end

function [vals, s] = sampleAlong(img, gv, p0, u, halfLen, stepM)
% 沿方向 u 从 p0 采样 [−halfLen, +halfLen]（双线性插值，越界 NaN）
[Xg, Yg] = meshgrid(gv, gv);
s = (-halfLen:stepM:halfLen)';
xy = p0' + s * u';
vals = interp2(Xg, Yg, img, xy(:,1), xy(:,2), 'linear', NaN);
end

function [wMm, valid] = fwhmFromProfile(vals, s, amp, halfLen)
% 由剖面找半高交点：峰在 s=0，向两侧找首次跌破 amp/2 的位置（线性插值）。
% invalid：无交点 / 峰位于窗边界（剖面端部仍高于半高）。
valid = true;  wMm = NaN;
half = amp / 2;
n0 = find(abs(s) < 1e-12, 1);
if isempty(n0), n0 = floor((numel(s)+1)/2); end
if n0 <= 1 || n0 >= numel(s) || abs(s(n0)) > halfLen - 1e-12
    valid = false;  return;   % 峰在窗边界
end
% 左侧
iL = n0;
while iL > 1 && vals(iL-1) >= half && ~isnan(vals(iL-1))
    iL = iL - 1;
end
if iL == 1 || isnan(vals(iL-1)) || vals(iL-1) >= half
    valid = false;  return;   % 左侧到窗边界仍高于半高
end
sL = s(iL-1) + (half - vals(iL-1)) * (s(iL) - s(iL-1)) / (vals(iL) - vals(iL-1));
% 右侧
iR = n0;
while iR < numel(s) && vals(iR+1) >= half && ~isnan(vals(iR+1))
    iR = iR + 1;
end
if iR == numel(s) || isnan(vals(iR+1)) || vals(iR+1) >= half
    valid = false;  return;   % 右侧到窗边界仍高于半高
end
sR = s(iR+1) + (half - vals(iR+1)) * (s(iR) - s(iR+1)) / (vals(iR+1) - vals(iR));
wMm = (sR - sL) * 1e3;
end

function reason = fwhmReason(valid)
if valid, reason = 'ok'; else, reason = 'noHalfCrossingOrWindowEdge'; end
end

function ratio = derivDominance(p, pp, X, Y, roiMask, theta, R, fs, c, stepLine, interpFun)
% 查询位置处的导数项/常数项 RMS 比：RMS(|2·t·p′|)/RMS(|2p|)（抽样线 × ROI 像素）
xs = X(roiMask);  ys = Y(roiMask);
r2 = xs.^2 + ys.^2;
sumD = 0;  sumP = 0;
for j = 1:stepLine:size(p, 2)
    proj = xs * (R * cos(theta(j))) + ys * (R * sin(theta(j)));
    dotp = proj - R^2;
    dist = sqrt(max((r2 - R^2) - 2 * dotp, 0));
    tf = dist * fs / c;
    tsec = tf / fs;
    v = interpFun(p(:, j), tf);
    vp = interpFun(pp(:, j), tf);
    d = 2 * tsec .* vp;
    p2 = 2 * v;
    sumD = sumD + sum(d.^2);
    sumP = sumP + sum(p2.^2);
end
ratio = sqrt(sumD / max(sumP, eps));
end
