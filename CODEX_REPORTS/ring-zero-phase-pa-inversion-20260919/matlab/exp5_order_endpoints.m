% EXP5_ORDER_ENDPOINTS  判定性实验 5：滤波顺序、端点约定与启动强信号处理。
%
% 比较两种滤波在链路中的位置（单根线，含启动强信号）：
%   顺序 M（MATLAB 历史）：raw → HP(filtfilt, 301:) → LP(filtfilt, sysDelay+20:)
%                          → DBR 置零(1..300) → 削顶 → DelayCut(358)
%   顺序 N（本任务提议）  ：raw → DBR 置零(1..300) → 削顶 → DelayCut(358)
%                          → HP（整条裁剪线，奇对称延拓+稳态初始化）
%                          → LP（同上）→ 导数（中心差分）
% 指标：
%   A1 有效区间一致性：远离 burst 区两种顺序的最大相对差
%   A2 burst 拖尾长度：|out| 降到 burst 峰 1% 以下的样本序号
%   A3 裁剪线起点 50 样本内 |out|max（端点瞬态）
%   A4 起始 100 样本内 |p'|max（UBP 导数污染指标）
%   A5 短窗报错：Nt_cut ≤ 3n 必须报错（不静默单向滤波）
%   A6 全程无 NaN/Inf
%
% 运行：matlab -batch "run('exp5_order_endpoints.m')"

outdir = fullfile(fileparts(mfilename('fullpath')), '..', 'evidence');
if ~exist(outdir, 'dir'), mkdir(outdir); end

fs = 250e6;
Nt = 4000;
sysDelay = 358;
maskLength = 300;
nHP = 4;  fcHP = 0.4e6;
nLP = 4;  fcLP = 40e6;
t = (0:Nt-1)' / fs;

% ---- 合成线：启动强信号 + PA 回波 ----
burst = 2000 * exp(-t / 1.5e-6) .* sin(2 * pi * 2e6 * t);          % 启动 ring-down
echo1 = 0.5 * exp(-(t - 4.0e-3 / c0()).^2 / (2 * (0.25e-6)^2)) .* sin(2 * pi * 5e6 * (t - 4.0e-3 / c0()));
echo2 = 0.3 * exp(-(t - 8.0e-3 / c0()).^2 / (2 * (0.25e-6)^2)) .* sin(2 * pi * 5e6 * (t - 8.0e-3 / c0()));
raw = burst + echo1 + echo2;

% ---- SOS 设计 ----
[zHP, pHP, kHP] = butter(nHP, 2 * fcHP / fs, 'high');
[sosHP, gHP] = zp2sos(zHP, pHP, kHP);
[zLP, pLP, kLP] = butter(nLP, 2 * fcLP / fs, 'low');
[sosLP, gLP] = zp2sos(zLP, pLP, kLP);

% ================= 顺序 M（MATLAB 历史）=================
xM = raw;
xM(301:end) = filtfilt(sosHP, gHP, xM(301:end));                    % 高通（切片）
xM(sysDelay + 20:end) = filtfilt(sosLP, gLP, xM(sysDelay + 20:end));% 低通（切片）
xM(1:maskLength) = 0;                                               % DBR 置零
cutM = xM(sysDelay:end);                                            % 延时截断

% ================= 顺序 N（提议）=================
xN = raw;
xN(1:maskLength) = 0;                                               % DBR 置零
cutN0 = xN(sysDelay:end);                                           % 延时截断
cutN = refZeroPhase(cutN0, sosHP, gHP, nHP);                             % 整线零相位高通
cutN = refZeroPhase(cutN, sosLP, gLP, nLP);                              % 整线零相位低通

% ================= 指标 =================
L = numel(cutM);
valid = (2000:L)';                                   % 远离 burst 的有效区间
maxSig = max(abs(cutN(valid)));
res.A1_maxRelDiff = max(abs(cutM(valid) - cutN(valid))) / max(maxSig, eps);
% burst 拖尾：从裁剪线起点向后找 |out| 最后一次 >1%·burst峰 的位置
burstPeakCut = max(abs(burst(sysDelay:end)));
idxTailM = find(abs(cutM) > 0.01 * burstPeakCut, 1, 'last');
idxTailN = find(abs(cutN) > 0.01 * burstPeakCut, 1, 'last');
res.A2_tailSamples_M = idxTailM;
res.A2_tailSamples_N = idxTailN;
res.A3_start50_M = max(abs(cutM(1:50)));
res.A3_start50_N = max(abs(cutN(1:50)));
ppN = gradient(double(cutN), 1 / fs);
ppM = gradient(double(cutM), 1 / fs);
res.A4_derivStart_M = max(abs(ppM(1:100)));
res.A4_derivStart_N = max(abs(ppN(1:100)));
res.A4_derivEcho_M = max(abs(ppM(2000:end)));
res.A4_derivEcho_N = max(abs(ppN(2000:end)));
res.A6_nanInf = any(isnan(cutM(:)) | isinf(cutM(:)) | isnan(cutN(:)) | isinf(cutN(:)));

fprintf('\n=== EXP5 顺序对比（burst 峰 %.0f，回波幅度 0.5）===\n', burstPeakCut);
fprintf('A1 有效区间(≥2000)最大相对差: %.3g\n', res.A1_maxRelDiff);
fprintf('A2 burst 拖尾（>1%%峰值）：M 到样本 %d，N 到样本 %d（裁剪线长 %d）\n', ...
    res.A2_tailSamples_M, res.A2_tailSamples_N, L);
fprintf('A3 起点 50 样本 |out|max：M=%.4g N=%.4g\n', res.A3_start50_M, res.A3_start50_N);
fprintf('A4 起始 100 样本 |p''|max：M=%.4g N=%.4g；回波区 |p''|max：M=%.4g N=%.4g\n', ...
    res.A4_derivStart_M, res.A4_derivStart_N, res.A4_derivEcho_M, res.A4_derivEcho_N);
fprintf('A6 NaN/Inf：%d\n', res.A6_nanInf);

% ---- A5 短窗报错 ----
shortIn = cutN0(1:12);                               % Nt_cut = 3n = 12
errCaught = false;
try
    refZeroPhase(shortIn, sosHP, gHP, nHP);
catch
    errCaught = true;
end
res.A5_errorAt12 = errCaught;
ok13 = true;
try
    y13 = refZeroPhase(cutN0(1:13), sosHP, gHP, nHP);     % Nt_cut = 3n+1 允许
    ok13 = all(isfinite(y13));
catch
    ok13 = false;
end
res.A5_okAt13 = ok13;
fprintf('A5 短窗：Nt_cut=12 报错=%d；Nt_cut=13 正常且有限=%d\n', errCaught, ok13);

res.pass = struct('A1', res.A1_maxRelDiff < 0.05, 'A6', ~res.A6_nanInf, ...
    'A5err', errCaught, 'A5ok', ok13);
save(fullfile(outdir, 'exp5_order_endpoints.mat'), 'raw', 'cutM', 'cutN', 'res', 't');
out = res;
out.params = struct('fs', fs, 'Nt', Nt, 'sysDelay', sysDelay, 'maskLength', maskLength, ...
    'nHP', nHP, 'fcHP', fcHP, 'nLP', nLP, 'fcLP', fcLP);
out.matlabVersion = version;
fid = fopen(fullfile(outdir, 'exp5_order_endpoints.json'), 'w');
fwrite(fid, jsonencode(out, 'PrettyPrint', true)); fclose(fid);
fprintf('EXP5_DONE -> %s\n', fullfile(outdir, 'exp5_order_endpoints.json'));

% ================= 局部函数 =================
function c = c0()
c = 1490.0;
end
