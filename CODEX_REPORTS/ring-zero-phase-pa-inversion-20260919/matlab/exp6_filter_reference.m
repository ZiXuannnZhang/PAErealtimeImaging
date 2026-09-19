% EXP6_FILTER_REFERENCE  滤波独立参考：SOS 设计、参考零相位实现 vs filtfilt、
% 频率响应（单程/双程截止增益）、参考向量导出（供阶段 B C++ 测试）。
%
% 冻结约定：
%   - double SOS（butter→zp2sos），截止 = 单程 −3dB 点，Wn = 2·fc/fs
%   - 零相位 = 奇对称延拓（nfact = 3n，n = 单程阶数）+ 逐节稳态初始化 + 前后向
%   - 复合增益在 fc 处 ≈ 0.5（−6dB）
%   - 线长 ≤ 3n 时报错
% 交叉验证：refZeroPhase（自写，C++ 目标算法）vs filtfilt(sos,g,·)（MATLAB 实现）
%
% 运行：matlab -batch "run('exp6_filter_reference.m')"

outdir = fullfile(fileparts(mfilename('fullpath')), '..', 'evidence');
if ~exist(outdir, 'dir'), mkdir(outdir); end

res = struct();

% ============ F1 频率响应（解析，来自系数）============
designs = {
    'hp0p4_fs250_n4', 4, 0.4e6, 250e6, 'high'; ...
    'lp40_fs250_n4',  4, 40e6, 250e6, 'low'; ...
    'hp0p4_fs200_n4', 4, 0.4e6, 200e6, 'high'; ...
    'lp40_fs200_n4',  4, 40e6, 200e6, 'low'; ...
    'hp0p4_fs250_n1', 1, 0.4e6, 250e6, 'high'; ...
    'hp0p4_fs250_n2', 2, 0.4e6, 250e6, 'high'; ...
    'hp0p4_fs250_n8', 8, 0.4e6, 250e6, 'high'; ...
    'lp40_fs250_n1',  1, 40e6, 250e6, 'low'; ...
    'lp40_fs250_n8',  8, 40e6, 250e6, 'low'};
fres = struct('name', {}, 'gainFc1pass', {}, 'gainFc2pass', {}, ...
    'gain2Fc1pass', {}, 'gainHalfFc1pass', {});
for i = 1:size(designs, 1)
    nm = designs{i, 1};  nn = designs{i, 2};  fc = designs{i, 3};
    fss = designs{i, 4};  ft = designs{i, 5};
    [sos, g] = designSos(nn, 2 * fc / fss, ft);
    [H1fc, H2fc] = compositeGain(sos, g, fc, fss);
    [H12, ~] = compositeGain(sos, g, 2 * fc, fss);
    [Hh, ~] = compositeGain(sos, g, fc / 2, fss);
    fres(end+1) = struct('name', nm, 'gainFc1pass', H1fc, 'gainFc2pass', H2fc, ...
        'gain2Fc1pass', H12, 'gainHalfFc1pass', Hh); %#ok<AGROW>
end
res.freq = fres;
fprintf('\n=== F1 频率响应（|H|：单程/双程）===\n');
for i = 1:numel(fres)
    fprintf('%-16s fc: %.4f / %.4f（期望 0.7071 / 0.5）；2fc: %.4f；fc/2: %.4g\n', ...
        fres(i).name, fres(i).gainFc1pass, fres(i).gainFc2pass, ...
        fres(i).gain2Fc1pass, fres(i).gainHalfFc1pass);
end

% ============ F2 refZeroPhase vs filtfilt 交叉验证 ============
fs = 250e6;  N = 4000;  t = (0:N-1)' / fs;
[sosHP, gHP] = designSos(4, 2 * 0.4e6 / fs, 'high');
[bHP, aHP] = butter(4, 2 * 0.4e6 / fs, 'high');
[sosLP, gLP] = designSos(4, 2 * 40e6 / fs, 'low');
[bLP, aLP] = butter(4, 2 * 40e6 / fs, 'low');
rng(11);
tests = { ...
    'dc',        0.5 * ones(N, 1); ...
    'sine_in',   sin(2 * pi * 5e6 * t); ...
    'sine_out',  sin(2 * pi * 0.1e6 * t); ...
    'multitone', sin(2 * pi * 1e6 * t) + 0.5 * sin(2 * pi * 8e6 * t) + 0.2 * sin(2 * pi * 60e6 * t) + 0.1; ...
    'pulse',     [zeros(1500,1); 1; zeros(N-1501,1)]; ...
    'burst',     2000 * exp(-t / 1.5e-6) .* sin(2 * pi * 2e6 * t); ...
    'noise',     0.1 * randn(N, 1)};
xres = struct('name', {}, 'hpMaxRel', {}, 'lpMaxRel', {}, 'hpInteriorRel', {}, ...
    'lpInteriorRel', {}, 'hpMaxAbsScale', {});
for i = 1:size(tests, 1)
    nm = tests{i, 1};  x = double(tests{i, 2});
    yRef = refZeroPhase(x, sosHP, gHP, 4);
    yFf = filtfilt(bHP, aHP, x);  yFf = yFf(:);   % tf 形式 filtfilt：交叉验证基准
    sc = max(abs(yFf));  sc = max(sc, eps);
    nfact = 3 * 4;
    inter = (nfact+1):(N-nfact);
    dHP = max(abs(yRef - yFf)) / sc;
    dHPi = max(abs(yRef(inter) - yFf(inter))) / max(max(abs(yFf(inter))), eps);
    yRefL = refZeroPhase(x, sosLP, gLP, 4);
    yFfL = filtfilt(bLP, aLP, x);  yFfL = yFfL(:);
    scL = max(abs(yFfL));  scL = max(scL, eps);
    dLP = max(abs(yRefL - yFfL)) / scL;
    dLPi = max(abs(yRefL(inter) - yFfL(inter))) / max(max(abs(yFfL(inter))), eps);
    xres(end+1) = struct('name', nm, 'hpMaxRel', dHP, 'lpMaxRel', dLP, ...
        'hpInteriorRel', dHPi, 'lpInteriorRel', dLPi, ...
        'hpMaxAbsScale', max(abs(yRef))); %#ok<AGROW>
end
res.cross = xres;
fprintf('\n=== F2 refZeroPhase vs tf-filtfilt（全段/内部相对最大差）===\n');
for i = 1:numel(xres)
    fprintf('%-10s HP: %.3g/%.3g   LP: %.3g/%.3g\n', xres(i).name, ...
        xres(i).hpMaxRel, xres(i).hpInteriorRel, xres(i).lpMaxRel, xres(i).lpInteriorRel);
end

% ============ F3 阶数 1..8 交叉验证（HP 0.4MHz）============
ores = struct('n', {}, 'maxRel', {}, 'interiorRel', {});
for n = 1:8
    [sos, g] = designSos(n, 2 * 0.4e6 / fs, 'high');
    [bt, at] = butter(n, 2 * 0.4e6 / fs, 'high');
    x = tests{4, 2};   % multitone
    yA = refZeroPhase(x, sos, g, n);
    yB = filtfilt(bt, at, x);  yB = yB(:);
    nfact = 3 * n;
    inter = (nfact+1):(N-nfact);
    ores(end+1) = struct('n', n, 'maxRel', max(abs(yA - yB)) / max(max(abs(yB)), eps), ...
        'interiorRel', max(abs(yA(inter) - yB(inter))) / max(max(abs(yB(inter))), eps)); %#ok<AGROW>
end
res.orderSweep = ores;
fprintf('\n=== F3 阶数 1..8（HP，multitone）refZeroPhase vs tf-filtfilt（全段/内部）===\n');
for i = 1:numel(ores)
    fprintf('n=%d: %.3g / %.3g\n', ores(i).n, ores(i).maxRel, ores(i).interiorRel);
end

% ============ F5 零相位冲击响应展宽 = 滤波器记忆长度 ============
% 冲击置于长零信号中部（远离边界，稳态初始化=真实静止，无约定依赖）。
% 零相位输出 |y| > 1e-6·峰 的半宽 = 记忆长度（边界约定/有效区间裁剪的依据）。
NL = 40000;
mid = floor(NL / 2);
xL = zeros(NL, 1);  xL(mid) = 1;
lres = struct('n', {}, 'memorySamples', {}, 'peak', {});
for n = 1:8
    [sos, g] = designSos(n, 2 * 0.4e6 / fs, 'high');
    yA = refZeroPhase(xL, sos, g, n);
    nz = find(abs(yA) > 1e-6 * max(abs(yA)));
    memSamples = max(mid - nz(1), nz(end) - mid);
    lres(end+1) = struct('n', n, 'memorySamples', memSamples, ...
        'peak', max(abs(yA))); %#ok<AGROW>
end
fprintf('\n=== F5 零相位冲击响应展宽（记忆长度）===\n');
for i = 1:numel(lres)
    fprintf('n=%d: memory=%d samples peak=%.4g\n', lres(i).n, lres(i).memorySamples, lres(i).peak);
end

% ============ F4 参考向量导出（供阶段 B C++ 数值测试）============
refExport = struct();
nVec = 0;
for i = 1:size(tests, 1)
    nm = tests{i, 1};  x = double(tests{i, 2});
    nVec = nVec + 1;
    refExport.(sprintf('in_%s', nm)) = x;
    refExport.(sprintf('outHP_%s', nm)) = refZeroPhase(x, sosHP, gHP, 4);
    refExport.(sprintf('outLP_%s', nm)) = refZeroPhase(x, sosLP, gLP, 4);
    pp = gradient(refExport.(sprintf('outHP_%s', nm)), 1 / fs);
    refExport.(sprintf('outHPdp_%s', nm)) = pp;
    tb = (0:N-1)' / fs;
    refExport.(sprintf('outHPb_%s', nm)) = 2 * refExport.(sprintf('outHP_%s', nm)) ...
        - 2 * tb .* pp;    % UBP 反演信号 b（未滤波路径的导数方案示例）
end
refExport.fs = fs;  refExport.N = N;
refExport.sosHP = sosHP;  refExport.gHP = gHP;
refExport.sosLP = sosLP;  refExport.gLP = gLP;
refExport.nHP = 4;  refExport.nLP = 4;
refExport.fcHP = 0.4e6;  refExport.fcLP = 40e6;
refExport.matlabVersion = version;
refExport.toolboxes = 'Signal Processing Toolbox';
refExport.convention = struct('nfact', '3n (n=单程Butterworth阶数)', ...
    'extension', 'odd (2*x1 - x[k], 两端)', ...
    'init', 'per-section steady-state (DF2T), forward then backward', ...
    'deriv', 'central difference, one-sided at ends (MATLAB gradient, dim=1)', ...
    'inversion_b', 'b = 2p - 2*t*dp/dt, t = tau/fs seconds');
save(fullfile(outdir, 'filter_reference_vectors.mat'), '-struct', 'refExport', '-v7');
fprintf('\nF4 参考向量已导出 -> filter_reference_vectors.mat\n');

res.pass = struct( ...
    'fcGain', all(arrayfun(@(s) abs(s.gainFc1pass - 0.7071) < 2e-2, fres)) && ...
              all(arrayfun(@(s) abs(s.gainFc2pass - 0.5) < 2e-2, fres)), ...
    'crossHP', all(arrayfun(@(s) s.hpMaxRel < 1e-6, xres(2:end))), ...
    'crossLP', all(arrayfun(@(s) s.lpMaxRel < 1e-6, xres(2:end))), ...
    'longPad', all(arrayfun(@(s) s.memorySamples < 5000, lres)));
fprintf('断言：fc 增益=%d 交叉验证HP=%d LP=%d 长延拓=%d\n', ...
    res.pass.fcGain, res.pass.crossHP, res.pass.crossLP, res.pass.longPad);

out = res;
out.matlabVersion = version;
fid = fopen(fullfile(outdir, 'exp6_filter_reference.json'), 'w');
fwrite(fid, jsonencode(out, 'PrettyPrint', true)); fclose(fid);
fprintf('EXP6_DONE -> %s\n', fullfile(outdir, 'exp6_filter_reference.json'));

% ================= 局部函数 =================
function y = filterCascade(x, sos)
% SOS 级联前向滤波（零初始状态）
y = double(x);
for k = 1:size(sos, 1)
    y = filter(sos(k, 1:3), sos(k, 4:6), y);
end
end

function [sos, g] = designSos(n, Wn, ftype)
[z, p, k] = butter(n, Wn, ftype);
[sos, g] = zp2sos(z, p, k);
end

function [H1, H2] = compositeGain(sos, g, f, fs)
% 单程/双程复合增益：H = g·Π Hk；双程 = |H|²
w = 2 * pi * f / fs;
H = g * ones(1, 1);
zj = exp(-1i * w);
for k = 1:size(sos, 1)
    b = sos(k, 1:3);  a = sos(k, 4:6);
    H = H * (b(1) + b(2) * zj + b(3) * zj^2) / (a(1) + a(2) * zj + a(3) * zj^2);
end
H1 = abs(H);
H2 = H1^2;
end
