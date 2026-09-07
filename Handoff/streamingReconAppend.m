function [dispImg, state] = streamingReconAppend(block_raw, state, cfg)
% STREAMINGRECONAPPEND 实际采集逐块接入 API
%
% 每次采集系统打包送达一个原始 A-line 块时调用一次：
%   原始块 -> 逐块预处理（preprocessBlock） -> 增量重建（das_recon_incremental_loop）
%
% 计数约定（与主脚本一致）：
%   cfg.AlinesPerFrame / cfg.AlinesPerBlock 均为“双波长合计”A-line 数；
%   每波长数量 = 合计/2。角度计算按单波长数量进行：
%     dtheta = CoverageDeg / (AlinesPerFrame/2)
%
% 输入:
%   block_raw : 单波长 [SampDepth x AlinesPerBlock/2]；或 cfg.Wavelengths=[1 2] 时
%               struct('wl1',[...], 'wl2',[...])，各为 [SampDepth x AlinesPerBlock/2]
%   state     : 上一次调用返回的 state（首次传 []）
%   cfg       : 配置结构体，必填字段：
%               fs / c / R / x / y / AlinesPerFrame / AlinesPerBlock /
%               CoverageDeg / Theta0Deg / pBlock（逐块预处理参数）/
%               SoundSpeedRadii / SoundSpeeds
%               可选：FOVDeg/FOVTheta0Deg/Apodization/DistanceWeightExponent/
%               Interpolation/MinDistance/MaskOutOfRange
%               逐波长：Wavelengths=[1] 或 [1 2]（存在该字段即启用逐波长 API），
%               pBlock 为结构体数组 pBlock(1)/pBlock(2)（或元胞 {pBlock1,pBlock2}）
%
% 输出:
%   单波长（无 Wavelengths 字段）：dispImg [ny x nx]，state 原累加状态
%   逐波长（有 Wavelengths 字段）：dispImg/state 均为 struct('wl1',...,'wl2',...)；
%               单波长矩阵输入（Wavelengths 为标量）时自动按该波长处理
%
% 说明:
%   - 本函数是“真实采集数据流”的接入点：收到一个块即调用一次，
%     显示刷新由外部（主脚本/网络线程）负责；
%   - 每根 A-line 只反投影一次，全部块处理完 = 全孔径 DAS；
%   - 逐波长模式两个波长共用同一块序号与角度位置，各自独立预处理/重建/累加。

% ---------------------------------------------------------------- 模式判定
if isfield(cfg, 'Wavelengths')
    wlList = cfg.Wavelengths(:)';
    dualAPI = true;
else
    wlList = 1;
    dualAPI = false;
end

% 单波长计数（双波长合计/2）
nWlFrame = cfg.AlinesPerFrame / 2;   % 每波长每帧 A-line 数
nWlBlock = cfg.AlinesPerBlock / 2;   % 每波长每块 A-line 数
if rem(nWlFrame, 1) || rem(nWlBlock, 1) || nWlBlock < 1
    error('streamingReconAppend:Count', ...
        'AlinesPerFrame/AlinesPerBlock 为双波长合计，必须为偶数且 AlinesPerBlock>=2。');
end

% 可选 DAS 选项缺省值（两种模式共用）
fovDeg = 360; fovTh0 = 0; apod = 'none'; pExp = 1;
interp = 'linear'; minD = []; maskOob = true;
if isfield(cfg, 'FOVDeg'), fovDeg = cfg.FOVDeg; end
if isfield(cfg, 'FOVTheta0Deg'), fovTh0 = cfg.FOVTheta0Deg; end
if isfield(cfg, 'Apodization'), apod = cfg.Apodization; end
if isfield(cfg, 'DistanceWeightExponent'), pExp = cfg.DistanceWeightExponent; end
if isfield(cfg, 'Interpolation'), interp = cfg.Interpolation; end
if isfield(cfg, 'MinDistance'), minD = cfg.MinDistance; end
if isfield(cfg, 'MaskOutOfRange'), maskOob = cfg.MaskOutOfRange; end

% 本块角度位置（块内 A-line 为帧内连续段，按单波长数量计算）
dtheta = cfg.CoverageDeg / nWlFrame;            % 每根（单波长）A-line 角度 [deg]

if dualAPI
    % ================================================ 逐波长接入（单/双波长统一入口）
    if ~isstruct(block_raw)
        if numel(wlList) ~= 1
            error('streamingReconAppend:BlockStruct', ...
                'cfg.Wavelengths 含多个波长时 block_raw 必须为 struct(wl1/wl2)。');
        end
        % 单波长矩阵输入兼容：按 wlList(1) 处理
    end
    if isempty(state)
        state = struct();
    end

    % 当前块序号（两个波长共用，由任一已存在子状态推导）
    k = 1;
    if isfield(state, 'wl1') && ~isempty(state.wl1)
        k = state.wl1.nBlock + 1;
    elseif isfield(state, 'wl2') && ~isempty(state.wl2)
        k = state.wl2.nBlock + 1;
    end

    startDeg = cfg.Theta0Deg + (k-1)*nWlBlock*dtheta;
    spanDeg  = (nWlBlock-1)*dtheta;              % 首末 A-line 角度差

    for w = wlList
        wname = sprintf('wl%d', w);
        if isstruct(block_raw) && ~isfield(block_raw, wname)
            error('streamingReconAppend:MissingWL', 'block_raw 缺少字段 %s。', wname);
        end
        if isstruct(block_raw)
            blk = block_raw.(wname);
        else
            blk = block_raw;
        end
        if size(blk, 2) ~= nWlBlock
            error('streamingReconAppend:BlockSize', ...
                '波长 %d 本块 A-line 数 %d 与 AlinesPerBlock/2=%d 不一致。', ...
                w, size(blk,2), nWlBlock);
        end

        % 逐块预处理（每波长独立参数：system_delay/im_value/wl 等）
        if iscell(cfg.pBlock)
            pb = cfg.pBlock{w};
        else
            pb = cfg.pBlock(w);
        end
        bscan = preprocessBlock(blk, pb);

        % 本波长增量重建（状态独立跨调用保存）
        subState = [];
        if isfield(state, wname), subState = state.(wname); end
        [img, subState] = das_recon_incremental_loop(single(bscan), ...
            cfg.fs, cfg.c, cfg.R, cfg.x, cfg.y, subState, ...
            'BlockStartDeg', startDeg, 'BlockSpanDeg', spanDeg, ...
            'FOVDeg', fovDeg, 'FOVTheta0Deg', fovTh0, ...
            'Apodization', apod, 'DistanceWeightExponent', pExp, ...
            'Interpolation', interp, 'MinDistance', minD, ...
            'MaskOutOfRange', maskOob, ...
            'SoundSpeedRadii', cfg.SoundSpeedRadii, 'SoundSpeeds', cfg.SoundSpeeds);
        state.(wname) = subState;
        dispImg.(wname) = img;
    end
else
    % ================================================ 旧单波长接口（向后兼容）
    if size(block_raw, 2) ~= nWlBlock
        error('streamingReconAppend:BlockSize', ...
            '本块 A-line 数 %d 与 AlinesPerBlock/2=%d 不一致。', ...
            size(block_raw,2), nWlBlock);
    end

    % 当前块序号（由状态推导）
    k = 1;
    if ~isempty(state)
        k = state.nBlock + 1;
    end

    % 逐块预处理
    bscan = preprocessBlock(block_raw, cfg.pBlock);

    % 本块角度位置
    startDeg = cfg.Theta0Deg + (k-1)*nWlBlock*dtheta;
    spanDeg  = (nWlBlock-1)*dtheta;              % 首末 A-line 角度差

    % 增量重建（本块只反投影一次）
    [dispImg, state] = das_recon_incremental_loop(single(bscan), ...
        cfg.fs, cfg.c, cfg.R, cfg.x, cfg.y, state, ...
        'BlockStartDeg', startDeg, 'BlockSpanDeg', spanDeg, ...
        'FOVDeg', fovDeg, 'FOVTheta0Deg', fovTh0, ...
        'Apodization', apod, 'DistanceWeightExponent', pExp, ...
        'Interpolation', interp, 'MinDistance', minD, ...
        'MaskOutOfRange', maskOob, ...
        'SoundSpeedRadii', cfg.SoundSpeedRadii, 'SoundSpeeds', cfg.SoundSpeeds);
end
end
