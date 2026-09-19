% TEST_TIME_DERIVATIVE  审查整改 R1 自动测试：时间导数维度与离散约定。
%
% 任务要求（追加任务 §7.1）的四项断言 + 端点约定 + 导出向量影响检查：
%   T1 时间方向线性变化、各列相同 → 导数是已知常数。
%      （9493962 的 gradient(matrix,dt,1) 对该输入返回全零——本项能明确识别原错误，
%        诊断字段 legacyExprMax 记录旧写法在 T1 输入上的输出幅值。）
%   T2 时间恒定、仅列间变化 → 时间导数为零（逐元素精确 0）。
%   T3 多列不同振幅/频率解析信号，与解析导数比较，明确离散误差容差：
%      内部中心差分相对误差 ≤ (ω·dt)²/6，端点单侧 ≤ ω·dt/2（一阶泰勒界）。
%   T4 向量路径与矩阵路径一致（timeDerivative 列 == gradient(v,dt)；矩阵形式
%      == [~,gradient(F,1,dt)]），端点单侧差分符合约定（整数精确例）。
%   T5 导出参考向量（filter_reference_vectors.mat 的 p'/b）不受矩阵维度错误影响：
%      它们由向量路径 gradient(v,dt) 生成；用 timeDerivative 重算比较 ≤1e-12 相对。
%
% 运行：matlab -batch "run('test_time_derivative.m')"   （工作目录 = matlab/）
% 失败时 error() → matlab -batch 非零退出。
% 输出：evidence/test_time_derivative.json

outdir = fullfile(fileparts(mfilename('fullpath')), '..', 'evidence');
if ~exist(outdir, 'dir'), mkdir(outdir); end

fs = 250e6;  dt = 1 / fs;
res = struct();

% ============ T1 时间线性、各列相同 → 常数导数（原错误识别项） ============
a = 3.7e6;  b0 = -12.3;                        % F(i,:) = a·t_i + b0
tt = (0:99)' * dt;
F1 = a * tt + b0;
F1 = repmat(F1, 1, 5);                          % 5 列完全相同
D1 = timeDerivative(F1, dt);
res.T1_maxErr = max(abs(D1(:) - a));
res.T1_pass = res.T1_maxErr <= 1e-6 * abs(a) + 1e-9;
% 诊断：旧写法（单输出 + 第三参数）在同一输入上的输出幅值（应为 0 → 原错误）
legacyExpr = gradient(F1, dt, 1);
res.T1_legacyExprMax = max(abs(legacyExpr(:)));
fprintf('T1 线性/列相同：|dF/dt − a|max = %.3g（容差 %.3g）；旧写法输出幅值 = %.3g\n', ...
    res.T1_maxErr, 1e-6 * abs(a) + 1e-9, res.T1_legacyExprMax);

% ============ T2 时间恒定、仅列间变化 → 时间导数为零 ============
colVals = [-5.1, 0.3, 220.7, -1e6];
F2 = repmat(colVals, 200, 1);                   % 每列常数，列间不同
D2 = timeDerivative(F2, dt);
res.T2_maxAbs = max(abs(D2(:)));
res.T2_pass = res.T2_maxAbs == 0;               % 等值差分精确为 0
fprintf('T2 时间恒定/列间变化：max|dF/dt| = %.3g（要求 = 0）\n', res.T2_maxAbs);

% ============ T3 解析信号与解析导数，明确离散容差 ============
% F(:,j) = A_j·sin(2π f_j t + φ_j)；解析 dF/dt = A_j·2π f_j·cos(2π f_j t + φ_j)
Aset = [1.0, 0.05, 3.3];
fset = [2e6, 5e6, 0.4e6];
ph = [0.3, -1.1, 2.0];
Nt3 = 512;
t3 = (0:Nt3-1)' * dt;
F3 = zeros(Nt3, 3);  D3a = zeros(Nt3, 3);
for j = 1:3
    w = 2 * pi * fset(j);
    F3(:, j) = Aset(j) * sin(w * t3 + ph(j));
    D3a(:, j) = Aset(j) * w * cos(w * t3 + ph(j));
end
D3 = timeDerivative(F3, dt);
% 相对误差按每列导数幅值尺度归一（逐点归一会除以过零点附近的小值）
scale3 = Aset(:)' .* (2 * pi * fset(:))';
err3 = abs(D3 - D3a) ./ scale3;
inter = 2:(Nt3-1);
omegaDtMax = 2 * pi * max(fset) * dt;
tolInter = (omegaDtMax)^2 / 6;      % 中心差分主导项界（取最大 ω）
tolEnd = omegaDtMax / 2;            % 端点单侧一阶界
res.T3_interiorMaxRel = max(max(err3(inter, :)));
res.T3_endpointMaxRel = max(max(err3([1, Nt3], :)));
res.T3_tolInterior = tolInter;
res.T3_tolEndpoint = tolEnd;
res.T3_pass = res.T3_interiorMaxRel <= tolInter && res.T3_endpointMaxRel <= tolEnd;
fprintf('T3 解析导数：内部相对误差 %.3g ≤ (ωdt)²/6=%.3g；端点 %.3g ≤ ωdt/2=%.3g\n', ...
    res.T3_interiorMaxRel, tolInter, res.T3_endpointMaxRel, tolEnd);

% ============ T4 向量/矩阵路径一致 + 端点约定（整数精确例） ============
F4 = [0; 1; 4; 9] * 1.0;                        % 整数精确：期望 [1; 2; 4; 5]
D4 = timeDerivative(F4, 1.0);
D4expect = [1; 2; 4; 5];
res.T4_endpointExact = isequal(D4, D4expect);
% 向量路径：每列 == gradient(v, dt)（数值一致到 eps 量级）
rng(5);  F5 = randn(300, 4);
D5 = timeDerivative(F5, dt);
vmax = 0;
for j = 1:4
    g = gradient(F5(:, j), dt);
    vmax = max(vmax, max(abs(D5(:, j) - g)) / max(max(abs(g)), eps));
end
res.T4_vectorPathRel = vmax;
% 矩阵路径：与 [~,gradient(F,1,dt)] 一致（正确 MATLAB 维度用法）
[~, Gm] = gradient(F5, 1, dt);
res.T4_matrixPathRel = max(abs(D5(:) - Gm(:))) / max(max(abs(Gm(:))), eps);
res.T4_pass = res.T4_endpointExact && res.T4_vectorPathRel <= 1e-12 && ...
    res.T4_matrixPathRel <= 1e-12;
fprintf('T4 端点精确=%d；向量路径相对差 %.3g；矩阵路径相对差 %.3g（容差 1e-12）\n', ...
    res.T4_endpointExact, res.T4_vectorPathRel, res.T4_matrixPathRel);

% ============ T5 导出参考向量不受矩阵维度错误影响 ============
matFile = fullfile(outdir, 'filter_reference_vectors.mat');
if exist(matFile, 'file')
    S = load(matFile, 'outHP_multitone', 'outHPdp_multitone', 'outHPb_multitone', 'fs', 'N');
    dpRe = timeDerivative(S.outHP_multitone, 1 / S.fs);
    relDp = max(abs(dpRe - S.outHPdp_multitone)) / max(max(abs(S.outHPdp_multitone)), eps);
    tvec = (0:S.N-1)' / S.fs;
    bRe = 2 * S.outHP_multitone - 2 * tvec .* dpRe;
    relB = max(abs(bRe - S.outHPb_multitone)) / max(max(abs(S.outHPb_multitone)), eps);
    res.T5_exportDpRel = relDp;
    res.T5_exportBRel = relB;
    res.T5_pass = relDp <= 1e-12 && relB <= 1e-12;
    fprintf('T5 导出 p''/b 向量：重算相对差 %.3g / %.3g（≤1e-12，未受矩阵误用影响）\n', ...
        relDp, relB);
else
    res.T5_pass = true;
    res.T5_skipped = true;
    fprintf('T5 跳过：filter_reference_vectors.mat 不存在（首次运行）\n');
end

% ============ 输入校验错误行为 ============
errDt = false;
try, timeDerivative(F4, -1); catch, errDt = true; end
errShort = false;
try, timeDerivative(ones(1, 5), dt); catch, errShort = true; end
res.inputChecks_pass = errDt && errShort;
fprintf('输入校验：dt 非法报错=%d 时间维过短报错=%d\n', errDt, errShort);

res.pass = res.T1_pass && res.T2_pass && res.T3_pass && res.T4_pass && ...
    res.T5_pass && res.inputChecks_pass;
if res.pass, tag = '[PASS]'; else, tag = '[FAIL]'; end
fprintf('TEST_TIME_DERIVATIVE %s\n', tag);

out = res;
out.matlabVersion = version;
fid = fopen(fullfile(outdir, 'test_time_derivative.json'), 'w');
fwrite(fid, jsonencode(out, 'PrettyPrint', true)); fclose(fid);
fprintf('TEST_TIME_DERIVATIVE_DONE -> %s\n', fullfile(outdir, 'test_time_derivative.json'));

assert(res.pass, 'test_time_derivative:assertionFailed', ...
    'R1 时间导数断言未全部通过（见 evidence/test_time_derivative.json）');
