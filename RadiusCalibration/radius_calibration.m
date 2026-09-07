%% radius_calibration.m
% 环形扫描多通道半径自校准主脚本（全孔径重构版）
%
% 重构思路：
%   1. 每个启用通道均做全孔径扫描，独立得到完整 360° 的原始 Bscan；
%   2. 预处理默认只做延时截断，其余步骤保留开关但默认关闭；
%   3. 用 Bscan 列向整数循环互相关估计各通道相对基准通道的起始角度偏移，
%      只输出位移列数，不对原始数据做任何数值修改，通过重建角度向量虚拟对齐；
%   4. 基准通道做多半径全孔径重建，以聚焦度量选最佳半径并生成基准图；
%   5. 其余通道做多半径全孔径重建，与基准图做归一化互相关，
%      消除通道间半径相对误差。
%
% 重建核心为当前 CUDA 代码的 MATLAB GPU 移植：
%   ring_recon_cuda.cu（ring_das_kernel / append_angles_radii 路径，拼接关闭）。
%
% 运行环境：
%   MATLAB + Parallel Computing Toolbox + 可用 CUDA GPU。

clc; clearvars; close all;

scriptDir = fileparts(mfilename('fullpath'));
addpath(scriptDir);

%% ================= 用户配置 =================
cfg = struct();

% ---- 数据来源（监听程序保存数据）----
cfg.dataDir = fullfile(fileparts(scriptDir), 'testdata', '01');
cfg.channelSel = 1:8;                   % 启用的通道代号数组，可取 1..8 任意数量
cfg.fileSuffix = 'test';                % 文件名中的自定义字符串 z
cfg.filesPerChannel = 8;                % 单通道读取文件数量（顺序读 000..N-1）
cfg.triggersPerFile = 1000;             % 单文件触发数（FileSaver 默认 1000）
cfg.sampleCount = 5000;                 % 单触发采样点数
cfg.fullAlinesPerChannel = 8000;        % 每通道每圈总 A-line 数量（含双波长，全孔径标定）
cfg.useAllRounds = false;               % true=多圈全部参与重建（时间×圈数）

% ---- 采集 / 几何参数 ----
cfg.fs = 250e6;                         % 监听程序实际采样率 [Hz]
cfg.angleStartDeg = 180;                % 基准通道全孔径起始角度（9 点钟方向）
cfg.radiusNominalMm = 6.57;             % 统一标称半径 [mm]

% ---- 预处理（默认只做延时截断，其余开关保留但默认关闭）----
cfg.externalFunctionPath = 'D:\zzx\data\SelfmadeFunction\DeconvolutionFuncPack';
cfg.trigdejit = 0;                      % 触发去抖（关闭）
cfg.phase_Decon = 0;                    % 相位去卷积（关闭）
cfg.Gaussfil = 0;                       % 光纤信号去除（关闭，开启需外部函数）
cfg.gaussfil_rowstart = 1900;
cfg.gaussfil_rowsend = 2400;
cfg.filter_low = 0;                     % 高通滤波（关闭）
cfg.w_low = 0.4e6;
cfg.n1 = 4;
cfg.filter_high = 0;                    % 低通滤波（关闭）
cfg.w_high = 40e6;
cfg.n2 = 4;
cfg.med = 0;                            % 中值滤波（关闭）
cfg.arc_remove = 0;                     % 弧线去除（关闭）
cfg.DBR_sig_remove = 0;                 % DBR 置零（默认关闭）
cfg.mask_length = 300;
cfg.singal_impair = 0;                  % 阈值削顶（关闭）
cfg.im_value = [2000 400];
cfg.DelayCut = 1;                       % 延时截断（默认开启）
cfg.sysDelay = [358 371];               % [532nm 1064nm] 系统延时

% ---- 角度对齐（整数列循环互相关，虚拟对齐）----
cfg.enableAngleAlign = true;            % 是否启用起始角度对齐
cfg.angleAlignWavelength = 1;           % 用 532nm Bscan 估计位移

% ---- DAS 参数 ----
cfg.fov = 36e-3;                        % 成像视场 [m]
cfg.calibGridSize = 0.02e-3;             % 标定网格 [m]
cfg.validateGridSize = 0.02e-3;          % 验证网格 [m]
cfg.soundSpeedRadii = [];
cfg.soundSpeeds = [1490 1540];
cfg.distanceWeightExponent = 1;
cfg.interpolation = 'linear';
cfg.minDistance = [];
cfg.maskOutOfRange = true;
cfg.apodization = 'none';

% ---- 标定搜索 ----
cfg.calibWavelengths = 1;               % 1=仅532nm；[1 2]=双波长分别标定
cfg.metric = 'tenengrad_norm';          % 参考通道聚焦度量类型
cfg.searchMm = 0.10;                    % 粗扫半范围 [mm]
cfg.coarseStepMm = 0.01;                % 粗扫步长 [mm]
cfg.refine = true;                      % 是否细扫
cfg.fineSpanMm = 0.012;                 % 细扫半范围 [mm]
cfg.fineStepMm = 0.002;                 % 细扫步长 [mm]

% ---- 可视化 / 输出 ----
cfg.doFigures = true;
cfg.saveResults = true;
cfg.outDir = fullfile(scriptDir, 'calib_out');
cfg.previewChannel = 4;

%% ================= 读取原始数据 =================
bscanRaw = read_ring_bscan(cfg);

selectedCh = sort(unique(cfg.channelSel(:)'));
if isempty(selectedCh) || any(selectedCh < 1 | selectedCh > 8)
    error('radius_calibration:Channels', ...
        'cfg.channelSel 必须是 1..8 内的通道代号数组。');
end
M = numel(selectedCh);
if ~ismember(cfg.previewChannel, selectedCh)
    warning('radius_calibration:PreviewChannel', ...
        'cfg.previewChannel=%d 不在所选通道内，预览改用通道%d。', ...
        cfg.previewChannel, selectedCh(1));
    cfg.previewChannel = selectedCh(1);
end

% 每通道每波长 A-line 数（所有选中通道应一致）
nWlPerChannel = size(bscanRaw.wl1{selectedCh(1)}, 2);
for ch = selectedCh
    if size(bscanRaw.wl1{ch}, 2) ~= nWlPerChannel || ...
       size(bscanRaw.wl2{ch}, 2) ~= nWlPerChannel
        error('radius_calibration:BscanSize', ...
            '通道 %d 的 Bscan 列数与其他通道不一致。', ch);
    end
end
K = cfg.fullAlinesPerChannel / 2;       % 每通道每波长每圈 A-line 数
if rem(cfg.fullAlinesPerChannel, 2) || K < 2
    error('radius_calibration:FullAlines', ...
        'cfg.fullAlinesPerChannel 必须为偶数且不小于 4。');
end
stepDeg = 360 / K;                      % 每根 A-line 角度步长

%% ================= 预处理（默认仅延时截断）=================
if ~isempty(cfg.externalFunctionPath) && exist(cfg.externalFunctionPath, 'dir')
    addpath(cfg.externalFunctionPath);
end

pBlock = repmat(struct(), 1, 2);
for w = 1:2
    pBlock(w).trigdejit = cfg.trigdejit;
    pBlock(w).phase_Decon = cfg.phase_Decon;
    pBlock(w).Gaussfil = cfg.Gaussfil;
    pBlock(w).gaussfil_rowstart = cfg.gaussfil_rowstart;
    pBlock(w).gaussfil_rowsend = cfg.gaussfil_rowsend;
    pBlock(w).filter_low = cfg.filter_low;
    pBlock(w).w_low = cfg.w_low;
    pBlock(w).n1 = cfg.n1;
    pBlock(w).filter_high = cfg.filter_high;
    pBlock(w).w_high = cfg.w_high;
    pBlock(w).n2 = cfg.n2;
    pBlock(w).med = cfg.med;
    pBlock(w).arc_remove = cfg.arc_remove;
    pBlock(w).DBR_sig_remove = cfg.DBR_sig_remove;
    pBlock(w).mask_length = cfg.mask_length;
    pBlock(w).singal_impair = cfg.singal_impair;
    pBlock(w).im_value = cfg.im_value(w);
    pBlock(w).DelayCut = cfg.DelayCut;
    pBlock(w).DAQ = cfg.fs;
    pBlock(w).sysDelay = cfg.sysDelay;
    pBlock(w).wl = w;
    pBlock(w).system_delay = cfg.sysDelay(w);
end

bpWl = cell(1, 2);                      % bpWl{w}{ch} = 预处理后单通道 Bscan
bpWl{1} = cell(1, 8);
bpWl{2} = cell(1, 8);
for w = 1:2
    for ch = selectedCh
        if w == 1
            bpWl{w}{ch} = preprocess_ring_block(bscanRaw.wl1{ch}, pBlock(w));
        else
            bpWl{w}{ch} = preprocess_ring_block(bscanRaw.wl2{ch}, pBlock(w));
        end
    end
end

%% ================= 起始角度对齐（整数列循环互相关，虚拟对齐）=================
refCh = selectedCh(1);                  % 角度基准通道 = 首个启用通道
angleShift = zeros(1, 8);               % 每个通道相对基准的整数列位移
if cfg.enableAngleAlign
    aw = cfg.angleAlignWavelength;
    for ch = selectedCh(2:end)
        angleShift(ch) = align_channel_start_angles( ...
            bpWl{aw}{refCh}, bpWl{aw}{ch});
        fprintf('角度对齐：通道%d 相对通道%d 位移 %d 列（%.2f°）\n', ...
            ch, refCh, angleShift(ch), angleShift(ch) * stepDeg);
    end
end

% 生成虚拟对齐后的角度向量（不修改任何 Bscan 数据）
thetaWl = cell(1, 2);
thetaWl{1} = cell(1, 8);
thetaWl{2} = cell(1, 8);
for ch = selectedCh
    offsetDeg = -angleShift(ch) * stepDeg;   % 等价于把 Bscan 循环右移 shift 列
    idx = 0:(nWlPerChannel - 1);
    % 每圈角度按 K 回绕；wl2 原始列相对 wl1 提前一列，故 (idx+1)
    thetaWl{1}{ch} = cfg.angleStartDeg + offsetDeg + mod(idx, K) * stepDeg;
    thetaWl{2}{ch} = cfg.angleStartDeg + offsetDeg + mod(idx + 1, K) * stepDeg;
end

fprintf('每通道全孔径：每波长 A-line=%d，步长=%.4f°，基准通道=%d\n', ...
    nWlPerChannel, stepDeg, refCh);
fprintf('预处理后 Bscan 行数：wl1=%d，wl2=%d\n', ...
    size(bpWl{1}{refCh}, 1), size(bpWl{2}{refCh}, 1));

%% ================= 重建网格与参数 =================
nCal = ceil(cfg.fov / cfg.calibGridSize);
xCal = linspace(-cfg.fov / 2, cfg.fov / 2, nCal);
yCal = xCal;

nVal = ceil(cfg.fov / cfg.validateGridSize);
xVal = linspace(-cfg.fov / 2, cfg.fov / 2, nVal);
yVal = xVal;

p = struct();
p.c = cfg.soundSpeeds(1);
p.SoundSpeedRadii = cfg.soundSpeedRadii;
p.SoundSpeeds = cfg.soundSpeeds;
p.DistanceWeightExponent = cfg.distanceWeightExponent;
p.Interpolation = cfg.interpolation;
p.MinDistance = cfg.minDistance;
p.MaskOutOfRange = cfg.maskOutOfRange;
p.Apodization = cfg.apodization;
p.FOVDeg = 360;
p.FOVTheta0Deg = 0;
p.ChunkSize = 0;

%% ================= 半径标定 =================
wlList = cfg.calibWavelengths(:)';
nWl = numel(wlList);

coarseRmm = cfg.radiusNominalMm + (-cfg.searchMm:cfg.coarseStepMm:cfg.searchMm);
coarseRmm = coarseRmm(:).';

radiusBest = zeros(8, nWl);             % 每通道最佳半径（mm）
corrPeak = ones(8, nWl);                % 每通道与基准图的相关峰值
curves = cell(8, nWl);                  % 标定曲线（参考通道为聚焦曲线，其余为相关曲线）
imgRefWl = cell(1, nWl);                % 各波长基准图（参考通道最佳半径）
imgNomWl = cell(1, nWl);                % 各波长基准图（标称半径，用于对比）

for wi = 1:nWl
    w = wlList(wi);
    fprintf('=== 标定波长 %d ===\n', w);

    % ---------- 参考通道：聚焦度量定绝对半径 ----------
    bRef = bpWl{w}{refCh};
    thRef = thetaWl{w}{refCh};
    metric = zeros(size(coarseRmm));
    for i = 1:numel(coarseRmm)
        radii = repmat(single(coarseRmm(i) * 1e-3), 1, numel(thRef));
        img = recon_ring_das_multir(bRef, cfg.fs, thRef, radii, xCal, yCal, p);
        metric(i) = ring_focus_metric(img, [], cfg.metric);
        fprintf('  参考通道%d r=%.3f mm focus=%.6g\n', ...
            refCh, coarseRmm(i), metric(i));
    end
    [~, imax] = max(metric);
    if imax == 1 || imax == numel(coarseRmm)
        rRef = coarseRmm(imax);
        warning('radius_calibration:SearchEdge', ...
            '参考通道粗扫最优值在搜索边缘，请增大 cfg.searchMm。');
    else
        rRef = parabolic_peak(coarseRmm(imax-1:imax+1), metric(imax-1:imax+1));
    end
    if cfg.refine
        fineRmm = rRef + (-cfg.fineSpanMm:cfg.fineStepMm:cfg.fineSpanMm);
        fmetric = zeros(size(fineRmm));
        for i = 1:numel(fineRmm)
            radii = repmat(single(fineRmm(i) * 1e-3), 1, numel(thRef));
            img = recon_ring_das_multir(bRef, cfg.fs, thRef, radii, xCal, yCal, p);
            fmetric(i) = ring_focus_metric(img, [], cfg.metric);
        end
        [~, fimax] = max(fmetric);
        if fimax == 1 || fimax == numel(fineRmm)
            rRef = fineRmm(fimax);
        else
            rRef = parabolic_peak(fineRmm(fimax-1:fimax+1), ...
                                  fmetric(fimax-1:fimax+1));
        end
    end
    rRef = round(rRef * 1e4) / 1e4;
    radiusBest(refCh, wi) = rRef;
    corrPeak(refCh, wi) = 1;
    curves{refCh, wi} = struct('radiusMm', coarseRmm, 'metric', metric, ...
                               'bestMm', rRef, 'type', 'focus');

    radiiRef = repmat(single(rRef * 1e-3), 1, numel(thRef));
    imgRefWl{wi} = recon_ring_das_multir(bRef, cfg.fs, thRef, radiiRef, ...
                                         xCal, yCal, p);
    radiiNom = repmat(single(cfg.radiusNominalMm * 1e-3), 1, numel(thRef));
    imgNomWl{wi} = recon_ring_das_multir(bRef, cfg.fs, thRef, radiiNom, ...
                                         xCal, yCal, p);
    fprintf('  -> 参考通道%d 最佳半径 = %.4f mm\n', refCh, rRef);

    % ---------- 其余通道：与基准图互相关定相对半径 ----------
    for ch = selectedCh(2:end)
        bCh = bpWl{w}{ch};
        thCh = thetaWl{w}{ch};
        corr = zeros(size(coarseRmm));
        for i = 1:numel(coarseRmm)
            radii = repmat(single(coarseRmm(i) * 1e-3), 1, numel(thCh));
            img = recon_ring_das_multir(bCh, cfg.fs, thCh, radii, xCal, yCal, p);
            corr(i) = norm_corr2(img, imgRefWl{wi});
            fprintf('  通道%d r=%.3f mm corr=%.6f\n', ch, coarseRmm(i), corr(i));
        end
        [~, imax] = max(corr);
        if imax == 1 || imax == numel(coarseRmm)
            rCh = coarseRmm(imax);
            warning('radius_calibration:SearchEdge', ...
                '通道%d 粗扫最优值在搜索边缘，请增大 cfg.searchMm。', ch);
        else
            rCh = parabolic_peak(coarseRmm(imax-1:imax+1), corr(imax-1:imax+1));
        end
        if cfg.refine
            fineRmm = rCh + (-cfg.fineSpanMm:cfg.fineStepMm:cfg.fineSpanMm);
            fcorr = zeros(size(fineRmm));
            for i = 1:numel(fineRmm)
                radii = repmat(single(fineRmm(i) * 1e-3), 1, numel(thCh));
                img = recon_ring_das_multir(bCh, cfg.fs, thCh, radii, xCal, yCal, p);
                fcorr(i) = norm_corr2(img, imgRefWl{wi});
            end
            [~, fimax] = max(fcorr);
            if fimax == 1 || fimax == numel(fineRmm)
                rCh = fineRmm(fimax);
            else
                rCh = parabolic_peak(fineRmm(fimax-1:fimax+1), ...
                                     fcorr(fimax-1:fimax+1));
            end
        end
        rCh = round(rCh * 1e4) / 1e4;
        radiusBest(ch, wi) = rCh;
        corrPeak(ch, wi) = max(corr);
        curves{ch, wi} = struct('radiusMm', coarseRmm, 'metric', corr, ...
                                'bestMm', rCh, 'type', 'corr');
        fprintf('  -> 通道%d 最佳半径 = %.4f mm（相关峰值 %.4f）\n', ...
            ch, rCh, max(corr));
    end
end

% 多波长结果取平均（机械半径与波长无关）
if nWl == 1
    radiusMm = radiusBest(:, 1).';
else
    radiusMm = mean(radiusBest, 2).';
end
radiusMm = round(radiusMm * 1e4) / 1e4;
radiusMm(~ismember(1:8, selectedCh)) = cfg.radiusNominalMm;

% 角度偏移输出（度）
angleOffsetDeg = -angleShift * stepDeg;
angleOffsetDeg(~ismember(1:8, selectedCh)) = 0;

fprintf('\n=== 标定结果 ===\n');
fprintf('radiusPerChannel (mm):\n  %.4f', radiusMm);
fprintf('\nselftest 参数：\n  --radius-per-ch "%s"\n', radius_csv(radiusMm));
fprintf('角度起始偏移（度，相对通道%d）:\n  ', refCh);
fprintf('%.4f ', angleOffsetDeg);
fprintf('\n整数列位移:\n  ');
fprintf('%d ', angleShift);
fprintf('\n');

%% ================= 验证 =================
fprintf('\n=== 验证（标称 vs 标定半径）===\n');
for ch = selectedCh
    for wi = 1:nWl
        w = wlList(wi);
        thCh = thetaWl{w}{ch};
        radiiNom = repmat(single(cfg.radiusNominalMm * 1e-3), 1, numel(thCh));
        radiiCal = repmat(single(radiusMm(ch) * 1e-3), 1, numel(thCh));
        imgN = recon_ring_das_multir(bpWl{w}{ch}, cfg.fs, thCh, radiiNom, ...
                                     xVal, yVal, p);
        imgC = recon_ring_das_multir(bpWl{w}{ch}, cfg.fs, thCh, radiiCal, ...
                                     xVal, yVal, p);
        cN = norm_corr2(imgN, imgRefWl{wi});
        cC = norm_corr2(imgC, imgRefWl{wi});
        fprintf('波长%d 通道%d：与基准相关 标称=%.4f 标定=%.4f\n', ...
            w, ch, cN, cC);
    end
end

%% ================= 可视化检查 =================
if cfg.doFigures
    % 图1：预处理检查（原始/预处理后 Bscan）
    figure('Name', '预处理检查', 'Color', 'w');
    for w = 1:2
        subplot(2, 2, w);
        if w == 1
            raw = bscanRaw.wl1{cfg.previewChannel};
        else
            raw = bscanRaw.wl2{cfg.previewChannel};
        end
        imagesc(raw);
        axis image; colorbar;
        title(sprintf('原始Bscan 波长%d 通道%d', w, cfg.previewChannel));
        subplot(2, 2, 2 + w);
        imagesc(bpWl{w}{cfg.previewChannel});
        axis image; colorbar;
        title(sprintf('预处理后Bscan 波长%d 通道%d', w, cfg.previewChannel));
    end

    % 图2：各通道标定曲线（参考通道为聚焦曲线，其余为相关曲线）
    figure('Name', '半径标定曲线', 'Color', 'w');
    for s = 1:M
        ch = selectedCh(s);
        subplot(ceil(M / 2), 2, s);
        hold on;
        for wi = 1:nWl
            c = curves{ch, wi};
            plot(c.radiusMm - cfg.radiusNominalMm, c.metric, '-o', ...
                'DisplayName', sprintf('波长%d', wlList(wi)));
        end
        yl = get(gca, 'YLim');
        plot([radiusMm(ch) - cfg.radiusNominalMm, ...
              radiusMm(ch) - cfg.radiusNominalMm], yl, 'r--');
        hold off;
        xlabel('半径偏移 (mm)');
        ylabel(c.type);
        title(sprintf('通道%d -> %.4f mm', ch, radiusMm(ch)));
        grid on;
        legend('Location', 'best');
    end

    % 图3：参考通道标称/标定重建图（wl1）
    figure('Name', '参考通道标定前后', 'Color', 'w');
    subplot(1, 2, 1);
    imagesc(xVal * 1e3, yVal * 1e3, imgNomWl{1});
    axis image; colorbar;
    title(sprintf('参考通道%d 标称半径', refCh));
    subplot(1, 2, 2);
    imagesc(xVal * 1e3, yVal * 1e3, imgRefWl{1});
    axis image; colorbar;
    title(sprintf('参考通道%d 标定半径 %.4f mm', refCh, radiusMm(refCh)));
    drawnow;
end

%% ================= 保存结果 =================
if cfg.saveResults
    if ~exist(cfg.outDir, 'dir')
        mkdir(cfg.outDir);
    end
    stamp = char(datetime('now', 'Format', 'yyyyMMdd_HHmmss'));
    matPath = fullfile(cfg.outDir, sprintf('radius_calib_%s.mat', stamp));
    save(matPath, 'radiusMm', 'radiusBest', 'corrPeak', 'curves', ...
         'angleShift', 'angleOffsetDeg', 'cfg', 'selectedCh', ...
         'imgRefWl', 'imgNomWl', 'xVal', 'yVal');

    txtPath = fullfile(cfg.outDir, 'radiusPerChannel_mm.txt');
    fid = fopen(txtPath, 'w');
    if fid < 0
        error('radius_calibration:Save', '无法打开输出文件：%s', txtPath);
    end
    fprintf(fid, '%s\n', radius_csv(radiusMm));
    fclose(fid);

    fprintf('\n已保存：%s\n已保存：%s\n', matPath, txtPath);
end

fprintf('\nRADIUS_CALIBRATION_DONE\n');

%% ================= 局部函数 =================
function s = radius_csv(radiusMm)
% 逗号分隔字符串，保留 4 位小数
s = sprintf('%.4f,', radiusMm);
s = s(1:end-1);
end
