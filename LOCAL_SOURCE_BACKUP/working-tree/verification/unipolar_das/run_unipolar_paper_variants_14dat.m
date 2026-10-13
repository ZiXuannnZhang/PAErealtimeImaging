% RUN_UNIPOLAR_PAPER_VARIANTS_14DAT
% Hakakzadeh et al., IEEE IUS 2022 (DOI 10.1109/IUS54386.2022.9957429)
% "Unipolar Back-Projection Algorithm for Photoacoustic Tomography" 的
% 忠实实现与变体对比（离线 MATLAB 验证脚本）。
%
% 数据：已切换到 20260929 新数据集（D:\zzx\data\20260929\），数据路径与
%       导入链从 run_enhance_variant_compare_20260929.m 逐字移植（读数函数
%       Func_8ch_DirecSample_SplitData / halfprecision_to_double /
%       ResolveDirectSampleFilePath、双波长切分、④区预处理滤波）。
%       算子变体部分未改动（仍为论文六步忠实实现）。
%
% 论文方法（Section II，六步文字流程，全文无编号公式）：
%   1. 初始双极图   I1 = DAS(p)
%   2. 生成新信号   s2 = -1 * dp/dt
%   3. 乘正的动态时间斜率 lambda(t)   ==>  s2 = -lambda(t) .* dp/dt
%      [G6] 论文未给出 lambda(t) 的显式形式（PDF 文本层符号即为空），
%           本脚本沿用项目既有取值 lambda(t)=lambdaSlope*t，t 以秒计、
%           零点=DelayCut 后首样本。
%   4. 次级双极图   I2 = DAS(s2)
%   5. 两图分别归一化后求和
%      [G6] 归一化规则论文未指定 ==> 本脚本参数化：
%           'maxabs'（默认）| 'l2' | 'none'（none 等价于信号域线性合并，
%           即 run_unipolar_das_14dat.m / 实时 RingSignalEnhancer 的现行行为）
%   6. 单极化：取正值、置零负值
%
% 论文对比基线中可实现者：
%   - positive-only：max(I1, 0)（论文 'positive method'）
%   - UBP 基线：按本仓审核文档 E1/G2 口径成对实现——权重
%     w_UBP = R*dTheta*cosAlpha/d^2 与信号项 b = 2p - 2t*dp/dt（t 用秒）
%     同时使用（G2 恒等式 b = 2p - 2r~*dp/dr~，r~=c*t）。
% 论文对比基线中不可从本文忠实实现者（故不实现）：
%   - MVHT（论文仅引用 [11]，本文无定义）
%   - UBP+Hilbert 平面阵基线（平面构型专用，本文圆阵对比未使用）
%
% 治理边界（本仓《前提_UBP与FBP调研报告审核更正说明》R1/R3/G6）：
%   本脚本是**离线对比研究**。第 5/6 步（逐图归一化、硬整流）是非线性
%   图像域操作，仅用于观感/质量对比；其输出**不得**进入生产定量链、
%   **不得**回写 acc/accW 或落盘数据、**不得**作为双波长定量的输入。
%   未经实机验收前，不构成对生产实现的修改依据。

clear; clc;

%% ---------- 路径与依赖 ----------
scriptDir = fileparts(mfilename('fullpath'));
repoRoot = fileparts(fileparts(scriptDir));   % 真实仓库根（脚本位于 verification/unipolar_das）
% RadiusCalibration 工具（recon_ring_das_multir / norm_corr2）。与
% run_enhance_variant_compare_20260929.m 同一口径：真实仓库优先。
candidateToolPaths = { ...
    fullfile(repoRoot, 'RadiusCalibration'), ...
    'D:\zzx\data\20260929\RadiusCalibration'};
for iTool = 1:numel(candidateToolPaths)
    if exist(candidateToolPaths{iTool}, 'dir')
        addpath(candidateToolPaths{iTool});
    end
end
assert(~isempty(which('recon_ring_das_multir')), ...
    '未找到 recon_ring_das_multir.m：请确认 RadiusCalibration 已在路径上。');

%% ---------- 数据与几何参数（逐字移植自 run_enhance_variant_compare_20260929.m ③区） ----------
dataRoot    = 'D:\zzx\data\20260929\';
ChannelNum  = [3 4];          % 参与读数的文件通道序号（1..8 对应 Card1_ChA..Card4_ChB）
FolderNum   = [077];          % 数据文件夹号（可多个，逐个完整跑一轮）
FileNum     = 5;              % 每通道分段 .dat 文件数（000..005）
FileName_custom = '_test_';   % 文件名中段
Channelselect = 4;            % 用哪个通道矩阵做双波长切分（4 = Card2_ChB）

sampDepth   = 7500;           % 每条 A-line 采样深度
fs          = 250e6;          % 采样率：实卡 250 MHz（14.dat 旧数据才是 200 MHz）
nRawColumns = 4000;           % 双波长切分所用列数（取通道矩阵尾部 nRawColumns 列）
nAlinesPerWavelength = 2000;  % 每波长 A-line 数
sysDelay    = [760, 775];     % 双波长延时截断（样本数；wl2-wl1=15 与实机 dbrmaskExtra 同源）
maskLength  = 0;              % DBR 掩膜长度（本套数据 =0：wl1 无前导零、wl2 仅 15 个）
shiftWL2    = true;           % 1064 nm 列整体前移一位（与实机 shiftWL2 同义）
radius      = 6.4e-3;         % 环阵半径 (m)
soundSpeed  = 1490.0;         % 声速 (m/s)
fov         = 28e-3;          % 重建视场 (m)
gridSizeMm  = 0.01;           % 网格步长 (mm)
coverageDeg = 360;            % 全环覆盖
distanceWeightExponent = 1;   % 距离加权指数（与实机一致）

% 几何网格（只依赖上述参数，与变体无关，算一次共用）
nGrid    = ceil(fov / (gridSizeMm * 1e-3));
x        = linspace(-fov / 2, fov / 2, nGrid);
y        = x;
thetaDeg = (0:nAlinesPerWavelength - 1) * (coverageDeg / nAlinesPerWavelength);
radii    = radius * ones(1, nAlinesPerWavelength);
das      = localDasParams(soundSpeed, distanceWeightExponent, gridSizeMm * 1e-3);
wavelengthNm = [532, 1064];

%% ---------- 预处理滤波器开关（逐字移植自 harness ④区；作用于所有变体） ----------
% 位置：双波长切分之后、DBR 掩膜/DelayCut 之前——作用在整条原始 A-line 上。
% 当前取值 = harness 现行状态（HP 0.4 MHz/4 阶 + LP 40 MHz/4 阶，双开）。
% 注意：归一化截止 Wn = 2*f/fs 必须用本数据真实采样率 fs=250e6，
% 老 14.dat 代码里的 DAQ=200e6 是历史字段，不要沿用。
preFilter.hpEnable    = true;   % 高通（去低频/基线漂移）；实机前端默认 true
preFilter.hpCutoffMhz = 0.4;    % 高通截止 (MHz)
preFilter.hpOrder     = 4;      % 高通阶数；实机前端默认 2
preFilter.lpEnable    = true;   % 低通（去高频噪声）；实机前端默认 true
preFilter.lpCutoffMhz = 40;     % 低通截止 (MHz)
preFilter.lpOrder     = 4;      % 低通阶数；实机前端默认 2

if preFilter.hpEnable || preFilter.lpEnable
    assert(~isempty(which('butter')) && ~isempty(which('filtfilt')), ...
        '启用了预处理滤波：需要 Signal Processing Toolbox（butter/filtfilt）。');
end

%% ---------- 变体开关（论文六步；算法部分未改动） ----------
lambdaSlope = 1.0;          % [G6] 论文未指定 lambda(t)；t 为秒，零点=DelayCut 后首样本
step5Norm = 'maxabs';       % [G6] 论文未指定第 5 步归一化规则：'maxabs' | 'l2' | 'none'
enableFreqComp = false;     % H(f) 低频补偿是**项目自加**（非论文内容）；论文忠实口径默认关闭
freqCompFcMhz = 10.0;
freqCompHmax = 2.0;
freqCompOrder = 2.0;

%% ---------- 各变体运行开关（1=运行，0=跳过该变体） ----------
% 关闭某变体只跳过其重建与出图；被其他启用变体依赖的中间结果（I1/I2/dp）
% 会按需自动重建，无需手动保持一致。
sw.initial   = 1;   % I1 = DAS(p)：初始双极图 —— 论文第 1 步
sw.secondary = 1;   % I2 = DAS(-λ·p′)：次级双极图 —— 论文第 2-4 步（λ(t)=lambdaSlope·t，t 用秒）
sw.sumNoNorm = 1;   % I1+I2：无归一化直接相加 —— 现行实时 RingSignalEnhancer（g=p-t·p′ 信号域合并）与 run_unipolar_das_14dat.m 的等价像，对照组
sw.normI1    = 1;   % norm(I1)：论文第 5 步归一化后的初始图（看归一化对 I1 的作用）
sw.normI2    = 1;   % norm(I2)：论文第 5 步归一化后的次级图（看归一化对 I2 的作用）
sw.sumNorm   = 1;   % norm(I1)+norm(I2)：论文第 5 步求和（归一化规则=step5Norm，G6：论文未指定）
sw.unipolar  = 0;   % max(norm(I1)+norm(I2), 0)：论文第 1-6 步完整忠实输出（论文最终单极图）
sw.positive  = 0;   % max(I1,0)：论文对比基线 positive-only（只留正值、零化负值）
sw.ubp       = 1;   % UBP 基线：本仓审核 E1/G2 成对口径（权重 R·Δθ·cosα/d² 与信号项 2p-2t·p′ 同时使用，t 用秒）

% gCNR（Kempski 2020）可选度量：填入 ROI 像素索引（线性索引，可用
% sub2ind([nGrid nGrid], rowVec, colVec) 生成）后启用。留空则跳过。
roiIn = [];    % 例：sub2ind([nGrid nGrid], 600:640, 1200:1240) 展开后的索引
roiOut = [];

%% ================= 主循环：逐数据文件夹完整跑一轮变体板 =================
FolderNum_str = cellstr(compose('%03d', FolderNum));
for reconround = 1:numel(FolderNum_str)
    CurrentFolderNum = FolderNum_str{reconround};
    outDir = fullfile(repoRoot, 'artifacts', 'unipolar_paper_variants_14dat', CurrentFolderNum);
    if ~exist(outDir, 'dir'), mkdir(outDir); end
    fprintf('===== 数据文件夹 %s =====\n', CurrentFolderNum);
    fprintf('  预处理滤波：HP=%d (%.1f MHz, %d 阶)  LP=%d (%.1f MHz, %d 阶)\n', ...
        preFilter.hpEnable, preFilter.hpCutoffMhz, preFilter.hpOrder, ...
        preFilter.lpEnable, preFilter.lpCutoffMhz, preFilter.lpOrder);

    % ---- 读数据（逐字移植自 harness：分段 uint16（float16 容器）→ double） ----
    FileLoadBase = cell(max(ChannelNum), 1);
    matrix_comp_cell = cell(max(ChannelNum), 1);
    Num_of_scan = 1;
    FileName_normal = {'\Card1_ChA','\Card1_ChB','\Card2_ChA','\Card2_ChB', ...
                       '\Card3_ChA','\Card3_ChB','\Card4_ChA','\Card4_ChB'};
    for cardIdx = ChannelNum
        FileloadArgc.FileNum = FileNum;
        FileloadArgc.depth   = sampDepth;
        FileloadArgc.Num_of_scan = Num_of_scan;
        FileLoadBase{cardIdx, 1} = [dataRoot CurrentFolderNum ...
                                    FileName_normal{cardIdx} FileName_custom];
        matrix_comp_cell{cardIdx, 1} = Func_8ch_DirecSample_SplitData( ...
            FileLoadBase{cardIdx, 1}, FileloadArgc);
    end
    nFiles = max(cellfun(@numel, matrix_comp_cell));
    matrix_comp = cell(max(ChannelNum), nFiles);
    for i = ChannelNum
        if ~isempty(matrix_comp_cell{i})
            matrix_comp(i, 1:numel(matrix_comp_cell{i})) = matrix_comp_cell{i};
        end
    end

    % ---- 双波长切分（逐字移植自 harness） ----
    offset = size(matrix_comp{Channelselect}, 2) - nRawColumns;
    temp = matrix_comp{Channelselect, Num_of_scan};
    temp(isnan(temp) | isinf(temp)) = 0;
    wl1 = double(temp(1:end, offset + 1:2:end));    % 532 nm
    wl2 = double(temp(1:end, offset + 2:2:end));    % 1064 nm
    if shiftWL2
        wl2 = circshift(wl2, 1, 2);
    end
    clear temp matrix_comp matrix_comp_cell

    % ---- 预处理级滤波（harness ④区；零相位 filtfilt，逐 A-line 独立） ----
    wl1 = localFrontendFilter(wl1, fs, preFilter);
    wl2 = localFrontendFilter(wl2, fs, preFilter);

    % ---- 预处理（DBR 掩膜 + DelayCut，与 harness 逐字一致） ----
    p1 = localPreprocessParams(1, sysDelay, maskLength);
    p2 = localPreprocessParams(2, sysDelay, maskLength);
    bscan1 = localPreprocess(wl1, p1);
    bscan2 = localPreprocess(wl2, p2);
    clear wl1 wl2

    % ---- 论文六步变体（逐变体开关控制；每变体单独 figure） ----
    fprintf('Reconstructing variants: step5Norm=%s, lambdaSlope=%.3g, freqComp=%d ...\n', ...
        step5Norm, lambdaSlope, enableFreqComp);
    tic;
    [imgs1, labels1, tags1] = localBuildVariants(bscan1, fs, lambdaSlope, step5Norm, ...
        enableFreqComp, freqCompFcMhz, freqCompHmax, freqCompOrder, ...
        thetaDeg, radii, x, y, das, sw);
    [imgs2, labels2, tags2] = localBuildVariants(bscan2, fs, lambdaSlope, step5Norm, ...
        enableFreqComp, freqCompFcMhz, freqCompHmax, freqCompOrder, ...
        thetaDeg, radii, x, y, das, sw);
    elapsed = toc;
    fprintf('All requested variants finished in %.2f s (%d panels per wavelength).\n', ...
        elapsed, numel(imgs1));

    % ---- 绘图：每个变体单独 figure（两波长各一套） ----
    for k = 1:numel(imgs1)
        pngPath = fullfile(outDir, sprintf('unipolar_paper_variants_%dnm_%s.png', ...
            wavelengthNm(1), tags1{k}));
        localShowVariant(x, y, imgs1{k}, ...
            sprintf('%s - %d nm', labels1{k}, wavelengthNm(1)), pngPath);
    end
    for k = 1:numel(imgs2)
        pngPath = fullfile(outDir, sprintf('unipolar_paper_variants_%dnm_%s.png', ...
            wavelengthNm(2), tags2{k}));
        localShowVariant(x, y, imgs2{k}, ...
            sprintf('%s - %d nm', labels2{k}, wavelengthNm(2)), pngPath);
    end

    % ---- 可选 gCNR ----
    if ~isempty(roiIn) && ~isempty(roiOut)
        fprintf('\ngCNR (Kempski 2020) per variant:\n');
        for k = 1:numel(labels1)
            g1 = localGCNR(imgs1{k}, roiIn, roiOut);
            g2 = localGCNR(imgs2{k}, roiIn, roiOut);
            fprintf('  %-30s %dnm: %.3f   %dnm: %.3f\n', ...
                labels1{k}, wavelengthNm(1), g1, wavelengthNm(2), g2);
        end
    end

    lambdaTimeOrigin = 'first sample after DelayCut';
    matPath = fullfile(outDir, 'unipolar_paper_variants.mat');
    save(matPath, 'imgs1', 'imgs2', 'labels1', 'labels2', 'tags1', 'tags2', 'sw', ...
        'lambdaSlope', 'step5Norm', ...
        'enableFreqComp', 'freqCompFcMhz', 'freqCompHmax', 'freqCompOrder', ...
        'lambdaTimeOrigin', 'x', 'y', 'thetaDeg', 'radii', 'fs', 'radius', ...
        'soundSpeed', 'gridSizeMm', 'sysDelay', 'maskLength', 'preFilter', ...
        'elapsed', '-v7.3');
    fprintf('Saved %s\n', matPath);
end
fprintf('全部完成。\n');

% ====================================================================
% 变体构建：论文六步 + 基线（算法部分未改动）
% ====================================================================
function [imgs, labels, tags] = localBuildVariants(bscan, fs, lambdaSlope, step5Norm, ...
    enableFreqComp, fcMhz, hMax, order, thetaDeg, radii, x, y, das, sw)

% 依赖门控：开关为 0 的变体不运行；被启用变体依赖的中间结果按需重建。
%   I1 被 initial / normI1 / sumNoNorm / sumNorm / positive / unipolar 依赖；
%   I2 被 secondary / normI2 / sumNoNorm / sumNorm / unipolar 依赖；
%   dp 被 I2（s2=-λ·p′）与 UBP（信号项 2p-2t·p′）依赖。
needI1 = sw.initial || sw.normI1 || sw.sumNoNorm || sw.sumNorm || sw.positive || sw.unipolar;
needI2 = sw.secondary || sw.normI2 || sw.sumNoNorm || sw.sumNorm || sw.unipolar;
needDp = needI2 || sw.ubp;

imgs = {}; labels = {}; tags = {};
if ~(needI1 || needI2 || sw.ubp)
    warning('所有变体开关均为 0：本波长跳过重建。');
    return;
end

% 论文第 1 步的输入是初始 PA 信号；H(f) 是项目自加项，论文忠实口径默认关闭。
if enableFreqComp && (needI1 || needDp)
    bscan = localFrequencyCompensate(bscan, fs, fcMhz, hMax, order);
end

% 论文第 1 步：I1 = DAS(p)
if needI1
    [I1, ~, ~] = recon_ring_das_multir(single(bscan), fs, thetaDeg, radii, x, y, das);
end

% 差分 dp/dt（中心差分，同 run_unipolar_das_14dat.m 口径）
if needDp
    dp = localCentralDerivative(bscan, fs);
end

% 论文第 2+3+4 步：s2 = -lambda(t).*dp/dt（lambda(t)=lambdaSlope*t，秒，G6 口径），I2 = DAS(s2)
if needI2
    [nt, ~] = size(bscan);
    lambda = lambdaSlope * (0:nt - 1)' / fs;
    s2 = -(lambda .* dp);
    [I2, ~, ~] = recon_ring_das_multir(single(s2), fs, thetaDeg, radii, x, y, das);
end

% 论文第 5 步 [G6]：两图分别归一化（规则=step5Norm；'none' 即现行信号域合并的等价像）
if sw.normI1 || sw.sumNorm || sw.unipolar
    I1n = I1 ./ localImageNorm(I1, step5Norm);
end
if sw.normI2 || sw.sumNorm || sw.unipolar
    I2n = I2 ./ localImageNorm(I2, step5Norm);
end

if sw.initial
    imgs{end+1} = I1;                                        %#ok<AGROW>
    labels{end+1} = 'I1 = DAS(p) (initial)';                 %#ok<AGROW>
    tags{end+1} = 'initial';                                 %#ok<AGROW>
end
if sw.secondary
    imgs{end+1} = I2;                                        %#ok<AGROW>
    labels{end+1} = 'I2 = DAS(-λ·p′) (secondary)';           %#ok<AGROW>
    tags{end+1} = 'secondary';                               %#ok<AGROW>
end
if sw.sumNoNorm
    imgs{end+1} = I1 + I2;                                   %#ok<AGROW>
    labels{end+1} = 'I1+I2 (no norm; current)';              %#ok<AGROW>
    tags{end+1} = 'sum_nonorm';                              %#ok<AGROW>
end
if sw.normI1
    imgs{end+1} = I1n;                                       %#ok<AGROW>
    labels{end+1} = 'norm(I1) (step5)';                      %#ok<AGROW>
    tags{end+1} = 'normI1';                                  %#ok<AGROW>
end
if sw.normI2
    imgs{end+1} = I2n;                                       %#ok<AGROW>
    labels{end+1} = 'norm(I2) (step5)';                      %#ok<AGROW>
    tags{end+1} = 'normI2';                                  %#ok<AGROW>
end
if sw.sumNorm
    imgs{end+1} = I1n + I2n;                                 %#ok<AGROW>
    labels{end+1} = 'norm(I1)+norm(I2) (step5)';             %#ok<AGROW>
    tags{end+1} = 'sum_paper';                               %#ok<AGROW>
end
if sw.unipolar
    imgs{end+1} = max(I1n + I2n, 0);                         %#ok<AGROW>
    labels{end+1} = 'max(sum,0) (paper steps 1-6)';          %#ok<AGROW>
    tags{end+1} = 'unipolar_paper';                          %#ok<AGROW>
end
if sw.positive
    imgs{end+1} = max(I1, 0);                                %#ok<AGROW>
    labels{end+1} = 'max(I1,0) (positive-only)';             %#ok<AGROW>
    tags{end+1} = 'positive_only';                           %#ok<AGROW>
end
if sw.ubp
    % UBP 基线（本仓审核 E1/G2 成对口径：权重 R*dTheta*cosAlpha/d^2 与信号项
    % 2p - 2t*dp/dt 成对；t 用秒）。信号项在 A-line 域预计算一次。
    tSec = (0:size(bscan, 1) - 1)' / fs;
    bUbp = 2 * double(bscan) - 2 * (tSec .* dp);
    [Iubp, ~, ~] = localReconRingUbp(bUbp, fs, thetaDeg, radii, x, y, das);
    imgs{end+1} = Iubp;                                      %#ok<AGROW>
    labels{end+1} = 'UBP baseline (E1-paired)';              %#ok<AGROW>
    tags{end+1} = 'ubp_baseline';                            %#ok<AGROW>
end
end

function n = localImageNorm(I, rule)
switch lower(rule)
    case 'maxabs'
        n = max(abs(I(:)));
        if n <= 0, n = 1; end
    case 'l2'
        n = sqrt(sum(double(I(:)).^2));
        if n <= 0, n = 1; end
    case 'none'
        n = 1;
    otherwise
        error('step5Norm must be ''maxabs''|''l2''|''none''.');
end
end

function dp = localCentralDerivative(p, fs)
% 与 run_unipolar_das_14dat.m localEnhancedSignal 的差分口径逐字一致
%（论文未指定离散微分方式，沿用脚本既有口径）。
dp = zeros(size(p), 'like', p);
dp(1, :) = (p(2, :) - p(1, :)) * fs;
dp(end, :) = (p(end, :) - p(end - 1, :)) * fs;
if size(p, 1) > 2
    dp(2:end-1, :) = (p(3:end, :) - p(1:end-2, :)) * (fs / 2);
end
end

function pComp = localFrequencyCompensate(p, fs, fcMhz, hMax, curveOrder)
% 【逐字移植】H(f) 低频补偿（项目自加项，非论文内容；enableFreqComp=true 时启用）：
% 奇反射填充 + 频域乘 H + 逆变换 + 裁回。H(f) = 1 + (Hmax-1)*exp(-(f/fc)^order)。
n = size(p, 1);
pad = min(n - 1, max(32, round(0.05 * n)));
left = 2 * p(1, :) - p(pad + 1:-1:2, :);
right = 2 * p(end, :) - p(end - 1:-1:end - pad, :);
ext = [left; p; right];
next = size(ext, 1);
f = (0:next - 1)' * fs / next;
f = min(f, fs - f);
fc = fcMhz * 1e6;
H = 1 + (hMax - 1) * exp(-(f / fc) .^ curveOrder);
extComp = real(ifft(fft(ext, [], 1) .* H, [], 1));
pComp = extComp(pad + 1:pad + n, :);
end

function g = localGCNR(img, roiIn, roiOut)
% gCNR = |mu1-mu0| / (|mu1-mu0| + 1.4826*(sigma1+sigma0))，Kempski et al.,
% Biomed. Opt. Express 11(7), 3684 (2020)。ROI 传像素索引列表。
v1 = double(img(roiIn));
v0 = double(img(roiOut));
mu1 = mean(v1); mu0 = mean(v0);
s1 = std(v1);   s0 = std(v0);
g = abs(mu1 - mu0) / (abs(mu1 - mu0) + 1.4826 * (s1 + s0));
end

% ====================================================================
% UBP 基线重建（E1/G2 成对口径）
% 与 recon_ring_das_multir 的 CUDA 移植同构：同一延迟/插值/越界掩膜/
% 分块 GPU 路径与显存自动分块，仅权重改为 w_UBP = wscale .* dotp ./ dsafe.^3
% （推导：dotp = -R*d*cosAlpha ==> wscC*dotp/d^3 = dTheta*apod*R*cosAlpha/d^2；
%   DAS 版为 wscC*dotp/(Rj*d^2) = dTheta*apod*cosAlpha/d）。
% 归一化同样用 Sigma|w|（几何量，双波长比值安全，审核 4.2 口径）。
% 注意（审核 4.2/D3）：fov>2*radius 时环外像素 cosAlpha 变号，w_UBP 带符号，
%   accW 用 |w| 累加——环外翻转行为按 D3 表征结果处置，此处如实保留。
% ====================================================================
function [img, acc, accW] = localReconRingUbp(bscan, fs, thetaDeg, radii, x, y, das)
[Nt, nd] = size(bscan);
thetaDeg = double(thetaDeg(:).');
radii    = double(radii(:).');
xv = single(x(:).');
yv = single(y(:));
nx = numel(xv);
ny = numel(yv);

cSingle = double(das.c);
maskOob = logical(das.MaskOutOfRange);
minDist = das.MinDistance;

th = single(thetaDeg * pi / 180);
Rj = single(radii);
detx = Rj .* cos(th);
dety = Rj .* sin(th);

% 与 C++ appendImpl 一致：角度权用相邻 A-line 的平均角步长
if nd > 1
    totalArc = sum(abs(diff(thetaDeg)));
    arc = totalArc / (nd - 1) * pi / 180;
else
    arc = 0;
end
apod = ones(1, nd, 'single');    % das.Apodization='none'（与传入参数一致）
wscale = single(-arc) .* apod;   % 1 x nd

if isempty(minDist) || (isscalar(minDist) && minDist <= 0)
    if nx > 1
        minDist = single(abs(xv(2) - xv(1)));
    else
        minDist = single(1e-6);
    end
end
minDist = max(single(minDist), single(1e-9));

% GPU 准备与显存自动分块（逐字沿用 recon_ring_das_multir 的口径）
bpG = gpuArray(single(bscan));
Xg = gpuArray(xv);                 % 1 x nx
Yg = gpuArray(yv(:));              % ny x 1
r2 = Xg .^ 2 + Yg .^ 2;            % ny x nx

chunk = 0;
if isfield(das, 'ChunkSize'), chunk = double(das.ChunkSize); end
if isempty(chunk) || chunk <= 0
    gridBytes = double(ny) * double(nx) * 4;
    g = gpuDevice;
    target = 0.35 * double(g.AvailableMemory);
    chunk = max(1, floor(target / (12 * gridBytes + 1)));
    chunk = min(chunk, 32);
end
chunk = min(max(round(chunk), 1), nd);

imgG = gpuArray.zeros(ny, nx, 'single');
sumWG = gpuArray.zeros(ny, nx, 'single');

for c0 = 1:chunk:nd
    cidx = c0:min(c0 + chunk - 1, nd);
    nc = numel(cidx);

    detxC = gpuArray(reshape(detx(cidx), 1, 1, nc));
    detyC = gpuArray(reshape(dety(cidx), 1, 1, nc));
    wscC  = gpuArray(reshape(wscale(cidx), 1, 1, nc));
    RjC   = gpuArray(reshape(Rj(cidx), 1, 1, nc));
    R2jC  = RjC .* RjC;                   % 1 x 1 x nc（禁止用 1 x nc 裸矩阵参与 3D 广播）

    proj  = Xg .* detxC + Yg .* detyC;    % ny x nx x nc
    dotp  = proj - R2jC;                  % = -R*d*cosAlpha
    dist2 = r2 - 2 * dotp - R2jC;         % |pixel-detector|^2（与 DAS 版同式）
    dist  = sqrt(max(dist2, single(0)));
    dsafe = max(dist, minDist);

    tf = dist * single(fs / cSingle);     % 飞行时间（样点数），单一声速
    i0f = floor(tf);
    frac = tf - i0f;

    if Nt * nc <= 65535
        it = 'uint16';
    else
        it = 'uint32';
    end
    i0 = cast(i0f + single(1), it);
    offs = cast(reshape((0:nc-1) * Nt, 1, 1, nc), it);

    colb = bpG(:, cidx);
    i0c = min(max(i0, cast(1, it)), cast(Nt - 1, it));
    li0 = i0c + offs;
    li1 = li0 + cast(1, it);
    v0 = colb(li0);
    v1 = colb(li1);
    vv = v0 + frac .* (v1 - v0);
    if maskOob
        valid = (i0 >= cast(1, it)) & (i0 <= cast(Nt - 1, it));
        vv = vv .* single(valid);
    end

    % UBP 权重：wscC .* dotp ./ dsafe^3 = dTheta*apod*R*cosAlpha/d^2（E1）
    w = wscC .* dotp ./ (dsafe .^ 3);

    wv = w .* vv;
    imgG = imgG + sum(wv, 3);
    sumWG = sumWG + sum(abs(w), 3);
end

tiny = single(1e-12);
accG = imgG;
imgG = accG ./ max(sumWG, tiny);
img = gather(imgG);
acc = gather(accG);
accW = gather(sumWG);
end

% ====================================================================
% 以下为 harness 逐字移植的数据读取/预处理/滤波助手 + 既有绘图助手
% ====================================================================
function [matrix_comp] = Func_8ch_DirecSample_SplitData(FileLoad, FileloadArgc)
% 【逐字移植】run_enhance_variant_compare_20260929.m 读数：
% 分段 uint16（float16 容器）→ double → 拼接
Num_of_scan = FileloadArgc.Num_of_scan;
FileNum = FileloadArgc.FileNum;
TotalDepth = FileloadArgc.depth;

cFile = [];
if isfield(FileloadArgc, 'FileInx')
    fileIndices = FileloadArgc.FileInx;
else
    fileIndices = 1:FileNum;
end
for i = fileIndices
    if i <= 10
        FileID = ['00' num2str(i-1)];
    else
        FileID = ['0' num2str(i-1)];
    end
    FolderPath = ResolveDirectSampleFilePath(FileLoad, FileID);
    if isempty(FolderPath)
        if isempty(cFile)
            error('No data file matched: %s%s.dat', FileLoad, FileID);
        else
            break;
        end
    end
    fid = fopen(FolderPath, 'r');
    if fid == -1, error('文件打开失败'); end
    cFiletemp = fread(fid, inf, 'uint16=>uint16');
    cFiletemp = halfprecision_to_double(cFiletemp);
    cFile = [cFile; single(cFiletemp)];
end

fclose('all');

if isfield(FileloadArgc, 'ScanInx')
    scanIdx = FileloadArgc.ScanInx;
else
    scanIdx = 1;
end

if isfield(FileloadArgc, 'move_Bscan')
    move_Bscan = FileloadArgc.move_Bscan;
    Inxshift1 = (scanIdx-1) * move_Bscan * TotalDepth + 1;
    NeedSamples = TotalDepth * move_Bscan * Num_of_scan;
    if size(cFile, 1) >= (Inxshift1 + NeedSamples - 1)
        cFile = cFile(Inxshift1:Inxshift1+NeedSamples-1);
    else
        if size(cFile, 1) >= Inxshift1
            cFile = cFile(Inxshift1:end);
        else
            cFile = [];
        end
        cFile = [cFile; zeros(NeedSamples-size(cFile, 1), 1)];
    end
    raw_data = reshape(cFile, TotalDepth, []);
    raw_matrix = reshape(raw_data, TotalDepth, move_Bscan, []);
else
    raw_matrix = reshape(cFile, TotalDepth, []);
end

raw_matrix = -1 .* raw_matrix;   % 相位取反（基准读数约定）

matrix_comp = cell(Num_of_scan, 1);
for i = 1:Num_of_scan
    if Num_of_scan == 1
        matrix_comp{i, 1} = raw_matrix;
    else
        matrix_comp{i, 1} = raw_matrix(:, :, i);
    end
end
end

function out = halfprecision_to_double(h)
% 【逐字移植】float16（uint16 容器）→ double 手工解码
h = uint16(h);
sign_bit = bitshift(bitand(h, hex2dec('8000')), -15);
exp_bits  = bitshift(bitand(h, hex2dec('7C00')), -10);
frac_bits = bitand(h, hex2dec('03FF'));
out = zeros(size(h), 'double');
s = 1 - 2*double(sign_bit);
idx_sub = (exp_bits == 0);
out(idx_sub) = s(idx_sub) .* 2.^(-14) .* (double(frac_bits(idx_sub)) / 1024);
idx_norm = (exp_bits > 0 & exp_bits < 31);
out(idx_norm) = s(idx_norm) .* 2.^(double(exp_bits(idx_norm)) - 15) .* ...
    (1 + double(frac_bits(idx_norm)) / 1024);
idx_inf = (exp_bits == 31 & frac_bits == 0);
out(idx_inf) = s(idx_inf) .* Inf;
idx_nan = (exp_bits == 31 & frac_bits ~= 0);
out(idx_nan) = NaN;
end

function filePath = ResolveDirectSampleFilePath(fileLoad, fileId)
% 【逐字移植】按模式匹配数据文件（多匹配时取字典序第一个并告警）
pattern = [fileLoad fileId '.dat'];
matches = dir(pattern);
if isempty(matches)
    filePath = '';
    return;
end
[~, order] = sort({matches.name});
matches = matches(order);
filePath = fullfile(matches(1).folder, matches(1).name);
if numel(matches) > 1
    warning('Multiple data files matched "%s"; using "%s".', pattern, filePath);
end
end

function out = localFrontendFilter(data, fs, preFilter)
% 【逐字移植】harness ④区：预处理级零相位高/低通滤波。
% Butterworth + filtfilt（前向+后向=零相位），对矩阵逐列滤波 = 逐 A-line 独立；
% 滤波器每条 A-line 初始状态归零，不携带跨 A-line/跨触发状态。
% 归一化截止用本数据真实采样率 fs（250e6），不用老代码 DAQ=200e6。
out = data;
if ~preFilter.hpEnable && ~preFilter.lpEnable
    return;                              % 双关 = 透传
end
if preFilter.hpEnable
    Wn = 2 * preFilter.hpCutoffMhz * 1e6 / fs;
    assert(Wn > 0 && Wn < 1, '高通截止越界：Wn=%g 必须在 (0,1) 内。', Wn);
    [B, A] = butter(preFilter.hpOrder, Wn, 'high');
    out = filtfilt(B, A, out);
end
if preFilter.lpEnable
    Wn = 2 * preFilter.lpCutoffMhz * 1e6 / fs;
    assert(Wn > 0 && Wn < 1, '低通截止越界：Wn=%g 必须在 (0,1) 内。', Wn);
    [B, A] = butter(preFilter.lpOrder, Wn, 'low');
    out = filtfilt(B, A, out);
end
end

function p = localPreprocessParams(wavelengthIndex, sysDelay, maskLength)
p = struct();
p.trigdejit = false;
p.phase_Decon = 0;
p.Gaussfil = 0;
p.gaussfil_rowstart = 1900;
p.gaussfil_rowsend = 2400;
p.filter_low = 0;
p.w_low = 0.4e6;
p.n1 = 4;
p.filter_high = 0;
p.w_high = 40e6;
p.n2 = 4;
p.med = 0;
p.arc_remove = 0;
p.DBR_sig_remove = true;
p.mask_length = maskLength;
p.singal_impair = false;
p.im_value = 2000;
p.DelayCut = true;
p.DAQ = 200e6;              % 仅历史字段，本预处理未使用（真实采样率见 fs=250e6）
p.sysDelay = sysDelay;
p.wl = wavelengthIndex;
p.system_delay = sysDelay(wavelengthIndex);
end

function bscan = localPreprocess(raw, p)
% DBR 掩膜置零 +（可选）限幅 + DelayCut。
% maskLength=0 时 wl1 不置零、wl2 置零 (sysDelay2-sysDelay1)=15 行。
bscan = raw;
zeroRows = p.mask_length + (p.wl == 2) * (p.sysDelay(2) - p.sysDelay(1));
zeroRows = min(zeroRows, size(bscan, 1));
if p.DBR_sig_remove && zeroRows > 0
    bscan(1:zeroRows, :) = 0;
end
if p.singal_impair
    bscan(bscan > p.im_value) = p.im_value;
    bscan(bscan < -p.im_value) = -p.im_value;
end
if p.DelayCut
    bscan = bscan(p.system_delay:end, :);
end
end

function p = localDasParams(soundSpeed, distanceWeightExponent, minDistance)
p = struct();
p.c = soundSpeed;
p.SoundSpeedRadii = [];
p.SoundSpeeds = [1490, 1540];
p.DistanceWeightExponent = distanceWeightExponent;
p.Interpolation = 'linear';
p.MinDistance = minDistance;
p.MaskOutOfRange = true;
p.Apodization = 'none';
p.FOVDeg = 360;
p.FOVTheta0Deg = 0;
p.ChunkSize = 8;
end

function localShowVariant(x, y, img, titleText, outputPng)
% 单变体单 figure 显示：±99.5% 分位鲁棒标度；'max(' 开头的单极化输出用 [0, scale]
fig = figure('Name', titleText, 'Color', 'w', 'Position', [80, 80, 900, 800]);
imagesc(x * 1e3, y * 1e3, img);
axis image;
set(gca, 'YDir', 'normal');
colormap(gray);
colorbar;
title(titleText, 'Interpreter', 'none');
xlabel('x (mm)');
ylabel('y (mm)');
if startsWith(titleText, 'max(')
    scale = localRobustAbsScale(max(img, 0));
    if isempty(scale) || scale <= 0, scale = 1; end
    caxis([0, scale]);
else
    scale = localRobustAbsScale(img);
    if isempty(scale) || scale <= 0, scale = 1; end
    caxis([-scale, scale]);
end
exportgraphics(fig, outputPng, 'Resolution', 150);
fprintf('Saved %s\n', outputPng);
end

function scale = localRobustAbsScale(img)
v = abs(img(isfinite(img)));
if isempty(v)
    scale = 0;
    return;
end
v = sort(v(:));
idx = max(1, min(numel(v), round(0.995 * numel(v))));
scale = v(idx);
end
