function [dispImg, state] = das_recon_incremental_loop(block_data, fs, c, R, x, y, state, varargin)
% DAS_RECON_INCREMENTAL_LOOP 增量流式反投影（匹配实际逐块采集）
%
% 每个新到达的 A-line 块全图反投影一次并累加（原始加权和 + 权重和），
% 任意时刻输出归一化后的因果最优图；全部块处理完 = 全孔径 DAS。
% 与 das_recon_stream_loop 的 global 模式数学等价，但状态可跨调用保存，
% 每块只反投影一次（总计算量 = 一次全量 DAS）。
%
% 输入:
%   block_data : [Nt x nBlock] 本块预处理后的 A-line 数据
%   fs/c/R/x/y : 与 das_recon_circular_gpu_v2 一致
%   state      : 上一步返回的 state 结构体（首次传 []）
% Name-Value:
%   'BlockStartDeg'  本块起始角度 [deg]（默认 0）
%   'BlockSpanDeg'   本块首末 A-line 角度差 [deg]（必填，=(nBlock-1)*dtheta）
%   'FOVDeg'/'FOVTheta0Deg' 输出图像扇区（默认 360/0=全图）
%   其余透传核心：Apodization/DistanceWeightExponent/Interpolation/
%   MinDistance/MaskOutOfRange/SoundSpeedRadii/SoundSpeeds
%
% 输出:
%   dispImg : 当前归一化显示图 [ny x nx]
%   state   : 累加状态（acc/acc_w/nBlock），下一次调用传入

p = inputParser;
p.addParameter('BlockStartDeg', 0);
p.addParameter('BlockSpanDeg', []);
p.addParameter('FOVDeg', 360);
p.addParameter('FOVTheta0Deg', 0);
p.addParameter('Apodization', 'none');
p.addParameter('DistanceWeightExponent', 1);
p.addParameter('Interpolation', 'linear');
p.addParameter('MinDistance', []);
p.addParameter('MaskOutOfRange', true);
p.addParameter('SoundSpeedRadii', []);
p.addParameter('SoundSpeeds', []);
p.parse(varargin{:});

block_start = double(p.Results.BlockStartDeg);
block_span  = double(p.Results.BlockSpanDeg);
if isempty(block_span)
    error('das_recon_incremental_loop:Span', '必须提供 BlockSpanDeg（首末 A-line 角度差）。');
end
fov_deg  = double(p.Results.FOVDeg);
fov_th0  = double(p.Results.FOVTheta0Deg);
apod_opt = p.Results.Apodization;
p_exp    = double(p.Results.DistanceWeightExponent);
interp_opt = p.Results.Interpolation;
min_dist = p.Results.MinDistance;
mask_oob = logical(p.Results.MaskOutOfRange);
ss_radii = p.Results.SoundSpeedRadii;
ss_speeds = p.Results.SoundSpeeds;

% 初始化累加状态
if isempty(state)
    ny = numel(y); nx = numel(x);
    state.acc   = single(zeros(ny, nx));
    state.acc_w = single(zeros(ny, nx));
    state.nBlock = 0;
end

% 本块全图反投影（原始加权和 + 权重和）
[img_k, ~, wsum_k] = das_recon_circular_gpu_v2(block_data, fs, c, R, x, y, ...
    'CoverageDeg', block_span, 'Theta0Deg', block_start, ...
    'FOVDeg', fov_deg, 'FOVTheta0Deg', fov_th0, ...
    'NormalizeByW', false, 'Apodization', apod_opt, ...
    'DistanceWeightExponent', p_exp, 'Interpolation', interp_opt, ...
    'MinDistance', min_dist, 'MaskOutOfRange', mask_oob, ...
    'SoundSpeedRadii', ss_radii, 'SoundSpeeds', ss_speeds);

state.acc   = state.acc + img_k;
state.acc_w = state.acc_w + wsum_k;
state.nBlock = state.nBlock + 1;

% 归一化显示图
dispImg = state.acc ./ max(state.acc_w, single(1e-12));
end
