function export_ring_recon_reference(dataPath, outDir, dataId, gridSizeMm, alinesPerBlock)
% EXPORT_RING_RECON_REFERENCE
% 运行 Handoff 双波长逐块实时重建（与 C++ RingRecon 完全相同的参数），
% 将每个波长每个块的归一化显示图导出为 float32 raw（column-major），
% 以及网格 x/y（float32），供 C++ 移植结果逐块对照。
%
% 用法：
%   export_ring_recon_reference('D:\zzx\data\20260716\11.dat', 'ref11', 11, 0.1, 200)
%   export_ring_recon_reference('D:\zzx\data\20260519\14.dat', 'ref14', 14, 0.1, 200)

    dualDir = fileparts(mfilename('fullpath'));
    addpath(dualDir);

    % ---- 数据/重建参数（与 Handoff 主脚本及 C++ 验证工具一致）----
    if dataId == 11
        wlOffset       = 151;
        AlinesPerFrame = 4000;
        sysDelay       = [171, 184];
        Radius         = 6.57e-3;
        CoverageDeg    = 180;
        FOVDeg         = 180;
    else
        wlOffset       = 301;
        AlinesPerFrame = 8000;
        sysDelay       = [358, 371];
        Radius         = 6.57e-3;
        CoverageDeg    = 360;
        FOVDeg         = 360;
    end

    SampDepth  = 4000;
    DAQ        = 200e6;
    FOV        = 36e-3;
    GridSize   = gridSizeMm * 1e-3;
    AlinesPerBlock = alinesPerBlock;
    if AlinesPerBlock < 2 || rem(AlinesPerBlock, 2) || rem(AlinesPerFrame, AlinesPerBlock)
        error('AlinesPerBlock 必须为偶数且整除 AlinesPerFrame');
    end

    SoundSpeedRadii = [];
    SoundSpeeds     = [1490 1540];
    ShiftWL2        = 1;
    Theta0Deg       = 0;
    FOVTheta0Deg    = 0;
    NormalizeByW    = false;
    apod_type       = 'none';
    DistanceWeightExponent = 1;
    Interpolation   = 'linear';
    MinDistance     = [];
    MaskOutOfRange  = true;

    trigdejit    = 0;  phase_Decon = 0;  Gaussfil = 0;
    filter_low   = 0;  filter_high = 0;  med = 0;  arc_remove = 0;
    DBR_sig_remove = 1;  singal_impair = 0;  DelayCut = 1;
    mask_length  = 300;
    im_value     = [2000 400];
    w_low = 0.4e6; n1 = 4; w_high = 40e6; n2 = 4;
    gaussfil_rowstart = 1900; gaussfil_rowsend = 2400;

    % ---- 逐波长预处理参数（与主脚本一致）----
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

    x = linspace(-FOV/2, FOV/2, ceil(FOV/GridSize)); y = x;
    nBlocks = AlinesPerFrame / AlinesPerBlock;

    cfgStream = struct();
    cfgStream.fs = DAQ; cfgStream.c = SoundSpeeds(1); cfgStream.R = Radius;
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
    cfgStream.Wavelengths = [1 2];
    cfgStream.pBlock = pBlock;

    simCfg = struct();
    simCfg.ReadMode = 'incremental';
    simCfg.testDataPath = dataPath;
    simCfg.SampDepth = SampDepth;
    simCfg.wlOffset = wlOffset;
    simCfg.wl = [1 2];
    simCfg.ShiftWL2 = ShiftWL2;
    simCfg.AlinesPerFrame = AlinesPerFrame;
    simCfg.AlinesPerBlock = AlinesPerBlock;
    simCfg.RawColsPerBlock = AlinesPerBlock;

    if ~exist(outDir, 'dir'), mkdir(outDir); end

    % 网格
    fid = fopen(fullfile(outDir, 'grid_x_f32.raw'), 'w');
    fwrite(fid, single(x(:)), 'float32'); fclose(fid);
    fid = fopen(fullfile(outDir, 'grid_y_f32.raw'), 'w');
    fwrite(fid, single(y(:)), 'float32'); fclose(fid);

    % 逐块重建并导出
    [~, sim] = simulateAcquisition('init', [], simCfg);
    state = [];
    for k = 1:nBlocks
        t0 = tic;
        [block, sim] = simulateAcquisition('next', sim);
        [dispImg, state] = streamingReconAppend(block, state, cfgStream);
        for w = 1:2
            wname = sprintf('wl%d', w);
            fid = fopen(fullfile(outDir, sprintf('wl%d_%03d.raw', w, k)), 'w');
            fwrite(fid, single(dispImg.(wname)), 'float32');
            fclose(fid);
            fid = fopen(fullfile(outDir, sprintf('wl%d_acc_%03d.raw', w, k)), 'w');
            fwrite(fid, single(state.(wname).acc), 'float32');
            fclose(fid);
            fid = fopen(fullfile(outDir, sprintf('wl%d_accw_%03d.raw', w, k)), 'w');
            fwrite(fid, single(state.(wname).acc_w), 'float32');
            fclose(fid);
        end
        fprintf('block %d/%d: %.1f ms\n', k, nBlocks, toc(t0)*1e3);
    end
    simulateAcquisition('close', sim);
    fprintf('EXPORT_DONE nx=%d ny=%d\n', numel(x), numel(y));
end