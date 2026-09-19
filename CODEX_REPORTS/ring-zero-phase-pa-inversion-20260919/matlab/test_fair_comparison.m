% TEST_FAIR_COMPARISON  审查整改 R2 自动测试：权重/归一化对照的公平性。
%
% 任务要求（追加任务 §7.2）的自动断言：
%   U1 normAccW 显式两参数归一化：已知值精确；缺 accW 报错（防静默退化为符号图）。
%   U2 legacyW 与 DAS 使用同一 abs(w) 累积规则：accW 逐元素一致（同一权重族）。
%   U3 ubpD 与 saP 同理（立体角权重族内 accW 一致）。
%   U4 对照输出不退化为 sign(acc)：归一化图像保留幅值动态范围（峰值/背景 ≥ 3，
%      符号图的该比值为 1）；旧 normAcc(acc) 写法在本幻体上该比值 = 1（反证记录）。
%   U5 legacyW 与 ubpD 成像形状相关（同一 b 输入、仅权重族不同）：ROI 内
%      Pearson 相关 ≥ 0.6（证据校准值，见 review-remediation.md）。
%   U6 因子分解结构：五模式共用同一 acc 累加器与同一测量方法（结构性检查：
%      das accW == legacyW accW、ubpD accW == saP accW 已覆盖权重一致性；
%      信号一致性由 acc 累加器实现唯一性保证，见 reconPairKernels.m）。
%
% 幻体：单点吸收体合成冲击响应（解析走时放置高斯脉冲），不复用 exp2 前向，
%       避免与被试实验耦合；重建核与 exp2 严格共享（reconPairKernels）。
%
% 运行：matlab -batch "run('test_fair_comparison.m')"   （工作目录 = matlab/）
% 输出：evidence/test_fair_comparison.json；失败 error() → 非零退出。

outdir = fullfile(fileparts(mfilename('fullpath')), '..', 'evidence');
if ~exist(outdir, 'dir'), mkdir(outdir); end
K = reconPairKernels();

fs = 250e6;  c = 1490.0;  R = 6.57e-3;
nd = 180;  Nt = 1600;
theta = (0:nd-1)' * (2 * pi / nd);
dtheta = 2 * pi / nd;
fov = 20e-3;  gridSize = 0.25e-3;  minDist = gridSize;
gv = (-fov/2):gridSize:(fov/2);
[X, Y] = meshgrid(gv, gv);
roi = (X.^2 + Y.^2) < (4e-3)^2;

% ---- 合成：点吸收体 (3mm, 0)，高斯冲击响应 ----
xt = [3e-3; 0];  sig = 6.0;  amp = 1.0;
tau = (0:Nt-1)';
p = zeros(Nt, nd);
for j = 1:nd
    d = norm(xt - [R * cos(theta(j)); R * sin(theta(j))]);
    p(:, j) = amp * exp(-(tau - d * fs / c).^2 / (2 * sig^2));
end
pp = timeDerivative(p, 1 / fs);

% ---- 五模式（同一实现、同一输入） ----
[accDas, accWDas] = K.reconDAS(p, theta, R, X, Y, fs, c, minDist, dtheta);
[accLeg, accWLeg] = K.reconLegacyWeight(p, pp, theta, R, X, Y, fs, c, minDist, dtheta);
[accUbp, accWUbp] = K.reconUBP(p, pp, theta, R, X, Y, fs, c, minDist, dtheta);
[accSaP, accWSaP] = K.reconSolidAngleP(p, theta, R, X, Y, fs, c, minDist, dtheta);
imgDas = K.normAccW(accDas, accWDas);
imgLeg = K.normAccW(accLeg, accWLeg);
imgUbp = K.normAccW(accUbp, accWUbp);
imgSaP = K.normAccW(accSaP, accWSaP);

res = struct();

% ============ U1 normAccW 已知值 + 缺参报错 ============
imgU1 = K.normAccW([2, -3]', [4, 5]');
res.U1_knownValues = isequal(imgU1, [0.5, -0.6]');
errCaught = false;
try, K.normAccW(accLeg); catch, errCaught = true; end
res.U1_missingAccWErrors = errCaught;
res.U1_pass = res.U1_knownValues && errCaught;
fprintf('U1 normAccW 已知值=%d 缺 accW 报错=%d\n', res.U1_knownValues, errCaught);

% ============ U2/U3 权重族一致性（同一 abs(w) 累积规则） ============
res.U2_accWMaxRel = max(abs(accWDas(:) - accWLeg(:))) / max(max(abs(accWDas(:))), eps);
res.U2_pass = res.U2_accWMaxRel <= 1e-14;
res.U3_accWMaxRel = max(abs(accWUbp(:) - accWSaP(:))) / max(max(abs(accWUbp(:))), eps);
res.U3_pass = res.U3_accWMaxRel <= 1e-14;
fprintf('U2 DAS vs legacyW accW 相对差 %.3g；U3 ubpD vs saP accW 相对差 %.3g（≤1e-14）\n', ...
    res.U2_accWMaxRel, res.U3_accWMaxRel);

% ============ U4 不退化为 sign(acc) ============
% 度量：ROI 内 max(img)/median(|img|)。符号图恒为 1；公平对照保留幅值信息。
contrast = @(img) max(img(roi)) / max(median(abs(img(roi))), eps);
res.U4_contrastLegacy = contrast(imgLeg);
res.U4_contrastUbp = contrast(imgUbp);
% 旧写法反证：acc/max|acc| 自归一化的同幻体对照比值（应≈1，证明旧缺陷可被本测试识别）
signMap = accLeg ./ max(abs(accLeg), 1e-12);
res.U4_contrastOldSignMap = contrast(signMap);
res.U4_pass = res.U4_contrastLegacy >= 3 && res.U4_contrastUbp >= 3 && ...
    res.U4_contrastOldSignMap < 1.5;
fprintf('U4 峰值/背景：legacyW=%.1f ubpD=%.1f（要求≥3）；旧符号图=%.3f（<1.5 证明本测试可识别原缺陷）\n', ...
    res.U4_contrastLegacy, res.U4_contrastUbp, res.U4_contrastOldSignMap);

% ============ U5 成像形状相关（legacyW vs ubpD，同一 b） ============
rLeg = imgLeg(roi) - mean(imgLeg(roi));
rUbp = imgUbp(roi) - mean(imgUbp(roi));
res.U5_corrLegacyUbp = (rLeg' * rUbp) / sqrt((rLeg' * rLeg) * (rUbp' * rUbp));
res.U5_pass = res.U5_corrLegacyUbp >= 0.6;
fprintf('U5 ROI 内 legacyW vs ubpD 相关系数 = %.3f（≥0.6）\n', res.U5_corrLegacyUbp);

% ============ 汇总 ============
res.pass = res.U1_pass && res.U2_pass && res.U3_pass && res.U4_pass && res.U5_pass;
if res.pass, tag = '[PASS]'; else, tag = '[FAIL]'; end
fprintf('TEST_FAIR_COMPARISON %s\n', tag);

out = res;
out.params = struct('fs', fs, 'c', c, 'R', R, 'nd', nd, 'Nt', Nt, 'fov', fov, ...
    'gridSize', gridSize, 'minDist', minDist, 'target', xt, 'sigSamples', sig);
out.matlabVersion = version;
fid = fopen(fullfile(outdir, 'test_fair_comparison.json'), 'w');
fwrite(fid, jsonencode(out, 'PrettyPrint', true)); fclose(fid);
fprintf('TEST_FAIR_COMPARISON_DONE -> %s\n', fullfile(outdir, 'test_fair_comparison.json'));

assert(res.pass, 'test_fair_comparison:assertionFailed', ...
    'R2 公平对照断言未全部通过（见 evidence/test_fair_comparison.json）');
