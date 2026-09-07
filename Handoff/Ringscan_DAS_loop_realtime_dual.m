clc;clearvars;
% close all;
set(0,'DefaultLineLineWidth',1.5);set(0,'DefaultAxesLineWidth',1.5);
set(0,'DefaultAxesFontSize',15);set(0,'DefaulttextFontSize',15);
set(0,'DefaultFigureWindowStyle','normal');
dualDir = fileparts(mfilename('fullpath'));                  % 本交付包目录（Handoff）
addpath(dualDir);                                         % 必要函数（simulateAcquisition/streamingReconAppend 等）
% 核心函数 das_recon_circular_gpu_v2 与本脚本同目录（自包含），无需额外 addpath
% ---- 数据文件（模拟采集源）----
datanum = [14];                    % 读取数据文件序号（testdata 下 14.dat / 11.dat）
folderPath = fullfile(fileparts(mfilename('fullpath')), '..', 'DASredo', 'testdata');   % 本地测试数据（11.dat/14.dat）；跑 D:\\zzx 数据时改回真实路径
fileName = sprintf('%02d.dat', datanum);
testDataPath = fullfile(folderPath, fileName);

% ---- 重建模式（三种可选）----
ReconMode = 'both';                % 'wl1'=仅 532nm 实时成像；'wl2'=仅 1064nm 实时成像；'both'=双波长同时重建实时显示
ShiftWL2 = 1;                      % 1064 通道 circshift(+1) 列对齐（与主脚本一致；0=不做偏移）

% ---- DAS 重建参数 ----
% sysDelay = [171, 184];             % 532nm/1064nm 系统延时（样点数）（11.dat 用 [171,184]）
sysDelay = [358, 371];             % 532nm/1064nm 系统延时（样点数）

SampDepth  = 4000;                 % 采样深度
ReconDepth = 4000;                 % 重建深度
space_n = 1;                       % A-line 抽值间隔
FrameNum = 1;                      % 数据内扫描圈数
DAQ = 200e6;                       % 采样率 [Hz]
Radius = 6.57e-3;                  % 环形阵列旋转半径 [m]（11.dat=6.57e-3；14.dat=6.48e-3）

SoundSpeedRadii = [];              % 分层声速边界 [m]（空=单一声速）
SoundSpeeds = [1490 1540];         % 各层声速 [m/s]，元素数=边界数+1

% ---- 实时采集参数（逐块到达）----
AlineRateHz = 40;                  % A-line 采集速率 [Hz]（每根 25ms；200 根=5s/块）
AlinesPerFrame = 8000;             % 每圈（每帧）双波长合计 A-line 数（单波长=4000；11.dat 用 4000）
AlinesPerBlock = 200;              % 每块（每次打包上传）双波长合计 A-line 数（单波长=100；200 根/40Hz=5s）
NumRevolutions = 1;                % 连续采集模拟圈数（每圈结束重置重建状态）
ReadMode = 'incremental';          % 数据模拟发送：'incremental'=按块读文件（推荐，不持有整帧）；'whole'=整帧切片
RawColsPerBlock = AlinesPerBlock;   % 每块原始列数 = 每块总 A-line 数（一列一根 A-line，双波长交替）

% ---- 成像网格与 DAS 选项 ----
FOV = 36e-3;                       % 成像视场直径 [m]
GridSize = 0.02e-3;                % 重建网格尺寸 [m]
CoverageDeg = 360;                 % 探头覆盖角 [deg]：决定整帧 A-line 的角度跨度（dtheta=CoverageDeg/单波长帧A-line数）。部分覆盖（如 11.dat 实际 180°）时需同时把 FOVDeg/FOVTheta0Deg 设为覆盖扇区，否则全圆显示会出现有限视角伪影
Theta0Deg = 0;                     % 覆盖起始角 [deg]
FOVDeg = 360;                      % 成像扇区角宽度
FOVTheta0Deg = 0;                  % 成像扇区起始角 [deg]
NormalizeByW = false;              % 增量模式恒按权重和归一化（等效 true，此项保留一致性）
apod_type = 'none';                % DAS 角度加窗：'none'/'hann'/'hamming'
DistanceWeightExponent = 1;        % DAS 距离权重指数
Interpolation = 'linear';          % 插值方式：'linear'/'nearest'
MinDistance = [];                  % 近场距离保护（空=自动取一个网格步长）
MaskOutOfRange = true;             % 越界时间索引置零

% ---- 预处理开关（流程与主脚本一致）----
trigdejit = 0;                     % 触发去抖（跨块步骤，实时模式需滑动窗口适配，暂关）
phase_Decon = 0;                   % 相位去卷积
Gaussfil = 0;                      % 去除光纤/静态背景信号（块级近似）
filter_low = 0;                    % 高通滤波
filter_high = 0;                   % 低通滤波
med = 0;                           % 中值滤波（块级近似）
arc_remove = 0;                    % 去除弧线信号（依赖整帧，暂关）
DBR_sig_remove = 1;                % 扣除 DBR 光纤振荡强信号
singal_impair = 0;                 % 全图信号阈值削顶
DelayCut = 1;                      % 延时截断

% ---- 预处理子参数 ----
refCol = 1001;                     % 触发去抖参考列
Corrows = [161, 190];              % 触发去抖相关行范围
interpFactor = 10;                 % 触发去抖插值倍数
dejitcheck = 0;                    % 触发去抖是否显示检查图
Figpram.xylim = [501, 1500, 300, 900];
Figpram.clim = [-200, 200];

gaussfil_rowstart = 1900;          % 去光纤信号处理起始行
gaussfil_rowsend = 2400;           % 去光纤信号处理结束行

w_low = 0.4e6;  n1 = 4;            % 高通滤波参数
w_high = 40e6;  n2 = 4;            % 低通滤波参数

curve_num = [1, 2, 3, 4];          % 弧线编号

mask_length = 300;                 % DBR 强信号扣除行数

im_value = [2000 400];             % [532nm 1064nm]阈值削顶值

bscan_view_orig = 0;               % 是否显示原始 Bscan
b_Aline1 = 1;                      % 原始 Bscan 显示起始行
b_Aline2 = 4500;                   % 原始 Bscan 显示结束行
pa_data_view = 0;                  % 是否显示预处理后的 Bscan
c_bscan = [200 200];               % Bscan 显示 colorbar

% ---- DAS 专属预处理 ----
scan_artifact_remove = 0;          % 去扫描伪影（默认关闭：旧版为硬编码几何模型，需按当前几何重新推导后再启用）
decay = 0.05;                      % 扫描伪影衰减系数（启用时使用）
scan_N = 4000;                     % 旧版几何参数：一整圈点数
scan_K = 2280;                     % 旧版几何参数：90 度时距离
scan_dmin = 340;                   % 旧版几何参数：最近距离
scan_D_ref = 1905;                 % 旧版几何参数：直径参考值
scan_win = 50;                     % 衰减窗口长度（样点数）

% ---- 显示与存储 ----
DisplayRecon = 1;                  % 是否逐块同图框刷新显示（both 模式为左右双图）
c_recon = [100 100];          % [532nm 1064nm] 重建图像 colorbar
save1 = 0;                         % 是否保存最终结果 mat
folderPath_2_save = fullfile(folderPath, '1');

% ---- 模拟与验证 ----
SimulateTiming = 0;                % 1=按物理块到达间隔（5s）暂停；0=连续处理
RefreshPauseSec = 0.05;            % 每块刷新后暂停秒数
VerifyFinal = 0;                   % 结束后与整帧 global 重建对比

% % % %============================================模式与波长列表============================================
switch ReconMode
    case 'wl1'
        wlList = 1;                % 仅 532nm
    case 'wl2'
        wlList = 2;                % 仅 1064nm
    case 'both'
        wlList = [1 2];            % 双波长同时
    otherwise
        error('ReconMode 只能为 ''wl1''、''wl2'' 或 ''both''。');
end
waveNames = {'532nm', '1064nm'};

% % % %============================================逐块实时重建============================================
% 逐块预处理参数（每波长独立：system_delay/im_value/wl 等，与主脚本一致）
pBlock = struct();
for w = 1:2
    pBlock(w).trigdejit = trigdejit;
    pBlock(w).phase_Decon = phase_Decon;
    pBlock(w).Gaussfil = Gaussfil;
    pBlock(w).gaussfil_rowstart = gaussfil_rowstart;
    pBlock(w).gaussfil_rowsend = gaussfil_rowsend;
    pBlock(w).filter_low = filter_low; pBlock(w).w_low = w_low; pBlock(w).n1 = n1;
    pBlock(w).filter_high = filter_high; pBlock(w).w_high = w_high; pBlock(w).n2 = n2;
    pBlock(w).med = med;
    pBlock(w).arc_remove = arc_remove;
    pBlock(w).DBR_sig_remove = DBR_sig_remove; pBlock(w).mask_length = mask_length;
    pBlock(w).singal_impair = singal_impair; pBlock(w).im_value = im_value(w);
    pBlock(w).DelayCut = DelayCut;
    pBlock(w).DAQ = DAQ;
    pBlock(w).sysDelay = sysDelay;
    pBlock(w).wl = w;
    pBlock(w).system_delay = sysDelay(w);
end

nBlocks = AlinesPerFrame / AlinesPerBlock;
if AlinesPerBlock < 2 || nBlocks ~= round(nBlocks) || nBlocks < 1
    error('AlinesPerBlock 必须 >=2 且能整除 AlinesPerFrame。');
end
nBlocks = round(nBlocks);
nWlPerFrame = AlinesPerFrame / 2;     % 每波长每帧 A-line 数
nWlPerBlock = AlinesPerBlock / 2;     % 每波长每块 A-line 数
dtheta = CoverageDeg / nWlPerFrame;        % 每根（单波长）A-line 角度 [deg]
blockInterval = AlinesPerBlock / AlineRateHz;     % 物理块到达间隔 [s]（200/40=5s）
revDuration   = AlinesPerFrame / AlineRateHz;     % 整圈采集时长 [s]（14.dat：8000/40=200s；11.dat：4000/40=100s）

x = linspace(-FOV/2, FOV/2, ceil(FOV/GridSize)); y = x;
c = SoundSpeeds(1);   % 分层时核心取 SoundSpeeds 前 k+1 个

% 逐块接入配置（streamingReconAppend / 模拟发送模块使用）
cfgStream = struct();
cfgStream.fs = DAQ; cfgStream.c = c; cfgStream.R = Radius;
cfgStream.x = x; cfgStream.y = y;
cfgStream.AlinesPerFrame = AlinesPerFrame;
cfgStream.AlinesPerBlock = AlinesPerBlock;
cfgStream.CoverageDeg = CoverageDeg;
cfgStream.Theta0Deg = Theta0Deg;
cfgStream.FOVDeg = FOVDeg;
cfgStream.FOVTheta0Deg = FOVTheta0Deg;
cfgStream.Apodization = apod_type;
cfgStream.DistanceWeightExponent = DistanceWeightExponent;
cfgStream.Interpolation = Interpolation;
cfgStream.MinDistance = MinDistance;
cfgStream.MaskOutOfRange = MaskOutOfRange;
cfgStream.SoundSpeedRadii = SoundSpeedRadii;
cfgStream.SoundSpeeds = SoundSpeeds;
cfgStream.Wavelengths = wlList;      % 启用逐波长接入 API（单/双波长统一）
cfgStream.pBlock = pBlock;           % 结构体数组，按波长索引

% 数据模拟发送模块配置
simCfg = struct();
simCfg.ReadMode = ReadMode;
simCfg.testDataPath = testDataPath;
simCfg.SampDepth = SampDepth;
simCfg.wlOffset = 151;               % 11.dat=151；14.dat=301
simCfg.wl = wlList;                  % 按模式输出单波长或双波长块
simCfg.ShiftWL2 = ShiftWL2;          % 1064 列对齐（与主脚本 circshift 一致）
simCfg.AlinesPerFrame = AlinesPerFrame;
simCfg.AlinesPerBlock = AlinesPerBlock;
simCfg.RawColsPerBlock = RawColsPerBlock;

% ================================================= 显示初始化
hIm = struct(); ax = struct();       % DisplayRecon=0 时未使用
if DisplayRecon
    nW = numel(wlList);
    fig = figure('Name', '实时逐块重建（双波长模拟采集）');
    for idx = 1:nW
        w = wlList(idx);
        wname = sprintf('wl%d', w);
        if nW == 2
            ax.(wname) = subplot(1, 2, idx);
        else
            ax.(wname) = subplot(1, 1, 1);
        end
        hIm.(wname) = imagesc(ax.(wname), x*1e3, y*1e3, zeros(numel(y), numel(x)));
        axis(ax.(wname), 'image'); colormap(ax.(wname), 'gray'); colorbar(ax.(wname));
        clim(ax.(wname), [-c_recon(w), c_recon(w)]);
        title(ax.(wname), sprintf('%s 实时重建：第1圈 0/0 块', waveNames{w}), 'FontName', '黑体');
    end
    drawnow;
end

fprintf('模拟连续采集：%d 圈 x %d 块 x %d A-line（双波长合计，每波长 %d/块），A-line 速率 %dHz，读取模式 %s，重建模式 %s\n', ...
    NumRevolutions, nBlocks, AlinesPerBlock, nWlPerBlock, AlineRateHz, ReadMode, ReconMode);
fprintf('物理时序：块间隔 %.2fs，整圈采集时长 %.1fs\n', blockInterval, revDuration);

tBlock = zeros(nBlocks, 1);
accRealtime = [];
for rev = 1:NumRevolutions
    % 初始化数据模拟发送（每圈重新开始）
    [~, sim] = simulateAcquisition('init', [], simCfg);
    state = [];
    for k = 1:nBlocks
        % 数据模拟发送：取下一原始块（单波长矩阵 / 双波长 struct）
        [block, sim] = simulateAcquisition('next', sim);

        % 逐块接入：每波长独立预处理 + 增量重建
        t0 = tic;
        [dispImg, state] = streamingReconAppend(block, state, cfgStream);
        tBlock(k) = toc(t0);

        % ---- 实时显示刷新（双波长模式左右双图同帧刷新）----
        if DisplayRecon
            startDeg = Theta0Deg + (k-1)*nWlPerBlock*dtheta;
            for w = wlList
                wname = sprintf('wl%d', w);
                set(hIm.(wname), 'CData', dispImg.(wname));
                title(ax.(wname), sprintf('%s 实时重建：第%d圈 %d/%d 块（%.1f-%.1f°），重建 %.1fms', ...
                    waveNames{w}, rev, k, nBlocks, startDeg, startDeg+(nWlPerBlock-1)*dtheta, ...
                    tBlock(k)*1e3), 'FontName', '黑体');
            end
            drawnow;
            pause(RefreshPauseSec);
        end

        if SimulateTiming
            pause(max(blockInterval - tBlock(k), 0));
        end
    end
    [~, sim] = simulateAcquisition('close', sim);
    accRealtime = dispImg;
    fprintf('第 %d 圈完成：%d 块，单块平均 %.1fms（块间隔 %.2fs，整圈采集 %.1fs），模式 %s\n', ...
        rev, nBlocks, mean(tBlock)*1e3, blockInterval, revDuration, ReconMode);
end

% ---- 验证：逐块增量结果 vs 整帧 global 重建（逐波长）----
if VerifyFinal
    [~, simF] = simulateAcquisition('init', [], simCfg);
    frames = struct();
    kk = 0;
    while true
        [blk, simF] = simulateAcquisition('next', simF);
        if isempty(blk), break; end
        kk = kk + 1;
        cols = (kk-1)*nWlPerBlock+1 : kk*nWlPerBlock;
        for w = wlList
            wname = sprintf('wl%d', w);
            if isstruct(blk)
                b0 = blk.(wname);
            else
                b0 = blk;
            end
            bp = preprocessBlock(b0, pBlock(w));
            if kk == 1
                frames.(wname) = zeros(size(bp,1), nWlPerFrame, 'like', bp);
            end
            frames.(wname)(:, cols) = bp;
        end
    end
    [~, simF] = simulateAcquisition('close', simF);
    for w = wlList
        wname = sprintf('wl%d', w);
        % 整帧 global 重建作为参考（与逐块路径同源、同预处理；核心函数全帧反投影 = global 模式等价）
        [img_k, ~, wsum_k] = das_recon_circular_gpu_v2(single(frames.(wname)), DAQ, c, Radius, x, y, ...
            'CoverageDeg', CoverageDeg, 'Theta0Deg', Theta0Deg, ...
            'FOVDeg', FOVDeg, 'FOVTheta0Deg', FOVTheta0Deg, ...
            'NormalizeByW', false, 'Apodization', apod_type, ...
            'DistanceWeightExponent', DistanceWeightExponent, ...
            'Interpolation', Interpolation, 'MinDistance', MinDistance, ...
            'MaskOutOfRange', MaskOutOfRange, ...
            'SoundSpeedRadii', SoundSpeedRadii, 'SoundSpeeds', SoundSpeeds);
        refAcc = img_k ./ max(wsum_k, single(1e-12));
        if isstruct(accRealtime)
            accW = accRealtime.(wname);
        else
            accW = accRealtime;
        end
        d = max(abs(accW(:) - refAcc(:)));
        fprintf('%s 与整帧 global 重建差异：%.3g（相对 %.3g）\n', ...
            waveNames{w}, d, d/max(abs(refAcc(:))));
    end
end

if save1 == 1
    currentSec = char(datetime('now', 'Format', 'HHmmss'));
    save(fullfile(folderPath_2_save, ...
        ['realtime_dual_', num2str(AlinesPerBlock), '_', currentSec, '.mat']), ...
        'accRealtime', 'x', 'y', 'state', 'tBlock', 'ReconMode', 'wlList');
end

fprintf('REALTIME_DUAL_DONE\n');
