function [img, acc, accW] = recon_ring_das_multir(bscan, fs, thetaDeg, radii, x, y, p)
%RECON_RING_DAS_MULTIR 多扫描半径环形 DAS 重建（当前 CUDA 内核的 MATLAB GPU 移植）
%
% 对应 C++/CUDA 实现：
%   ring_recon_cuda.cu 的 ring_das_kernel 与 appendImpl，
%   ring_recon_cuda_append_angles_radii 路径（拼接关闭）。
% 支持逐 A-line 探测器角度与逐 A-line 重建半径。
%
% 输出与实时程序相同：
%   img = acc ./ max(accW, 1e-12)，acc = Σ(w*v)，accW = Σ|w|
%
% 输入:
%   bscan    : [Nt x nd] 预处理后的 A-line 数据（single/double）
%   fs       : 采样率 [Hz]
%   thetaDeg : [1 x nd] 逐 A-line 探测器角度 [度]
%   radii    : [1 x nd] 逐 A-line 探测器半径 [m]
%   x, y     : 重建网格坐标 [m]（与 C++ xv/yv 一致）
%   p        : 参数字段
%                c                       单声速 [m/s]
%                SoundSpeedRadii         分层声速边界半径 [m]，[]=单一声速
%                SoundSpeeds             各层声速 [m/s]
%                DistanceWeightExponent  距离权重指数（默认 1）
%                Interpolation           'linear'/'nearest' 或 0/1（0=线性）
%                MinDistance             [] 或 0 = 自动取一个网格步长
%                MaskOutOfRange          越界时间索引是否置零
%                Apodization             'none'/'hann'/'hamming' 或 0/1/2
%                FOVDeg / FOVTheta0Deg   输出扇形掩码
%                ChunkSize               GPU 批处理探测器数，0=自动
%
% 输出:
%   img : [ny x nx] single 归一化 DAS 图像
%   acc : [ny x nx] single 原始加权和（可选）
%   accW: [ny x nx] single 权重和（可选）

if nargin < 7
    error('recon_ring_das_multir:Args', '输入参数不足。');
end

[Nt, nd] = size(bscan);
thetaDeg = double(thetaDeg(:).');
radii    = double(radii(:).');
if numel(thetaDeg) ~= nd || numel(radii) ~= nd
    error('recon_ring_das_multir:Size', ...
        'thetaDeg/radii 长度必须等于 A-line 数（nd=%d）。', nd);
end
if Nt < 2 || nd < 2
    error('recon_ring_das_multir:Size', 'bscan 至少需要 2x2 大小。');
end

xv = single(x(:).');
yv = single(y(:));
nx = numel(xv);
ny = numel(yv);
if nx < 2 || ny < 2
    error('recon_ring_das_multir:Grid', '网格至少需要 2x2 点。');
end

% ------------------------------------------------------------------ 选项
fovDeg      = 360;
fovTh0Deg   = 0;
ssRadii     = [];
ssSpeeds    = [];
pExp        = 1;
interpOpt   = 'linear';
minDist     = [];
maskOob     = true;
apodOpt     = 'none';
chunk       = 0;
if isfield(p, 'SoundSpeedRadii'), ssRadii = double(p.SoundSpeedRadii(:).'); end
if isfield(p, 'SoundSpeeds'),     ssSpeeds = double(p.SoundSpeeds(:).'); end
if isfield(p, 'DistanceWeightExponent'), pExp = double(p.DistanceWeightExponent); end
if isfield(p, 'MinDistance'), minDist = p.MinDistance; end
if isfield(p, 'MaskOutOfRange'), maskOob = logical(p.MaskOutOfRange); end
if isfield(p, 'FOVDeg'), fovDeg = double(p.FOVDeg); end
if isfield(p, 'FOVTheta0Deg'), fovTh0Deg = double(p.FOVTheta0Deg); end
if isfield(p, 'ChunkSize'), chunk = round(double(p.ChunkSize)); end
if isfield(p, 'Interpolation')
    iv = p.Interpolation;
    if isnumeric(iv)
        if iv == 0, interpOpt = 'linear';
        elseif iv == 1, interpOpt = 'nearest';
        else, error('recon_ring_das_multir:Interp', 'Interpolation 只能为 0 或 1。');
        end
    else
        interpOpt = lower(char(iv));
    end
end
if isfield(p, 'Apodization')
    av = p.Apodization;
    if isnumeric(av)
        if av == 0, apodOpt = 'none';
        elseif av == 1, apodOpt = 'hann';
        elseif av == 2, apodOpt = 'hamming';
        else, error('recon_ring_das_multir:Apod', 'Apodization 只能为 0/1/2。');
        end
    else
        apodOpt = lower(char(av));
    end
end
interpOpt = validatestring(interpOpt, {'linear', 'nearest'}, mfilename, 'Interpolation');
apodOpt   = validatestring(apodOpt, {'none', 'hann', 'hamming'}, mfilename, 'Apodization');

if ~isfield(p, 'c') || isempty(p.c) || p.c <= 0
    error('recon_ring_das_multir:SoundSpeed', 'p.c 必须为正的声速。');
end
cSingle = double(p.c);

% ------------------------------------------------------------ 分层声速
useLayers = false;
nBound = 0;
preOuter = single(0);
preCoeff = single(0);
rb2s = single(0);
if ~isempty(ssRadii)
    if isempty(ssSpeeds) || numel(ssSpeeds) < numel(ssRadii) + 1
        error('recon_ring_das_multir:SoundSpeed', ...
            'SoundSpeeds 元素数至少为边界数+1。');
    end
    ssSpeeds = ssSpeeds(1:numel(ssRadii) + 1);
    if any(ssRadii <= 0) || any(diff(ssRadii) <= 0)
        error('recon_ring_das_multir:SoundSpeed', ...
            'SoundSpeedRadii 必须为正且严格递增。');
    end
    if any(ssSpeeds <= 0)
        error('recon_ring_das_multir:SoundSpeed', 'SoundSpeeds 必须为正。');
    end
    useLayers = true;
    nBound = numel(ssRadii);
    invSpeeds = single(1 ./ ssSpeeds);
    preOuter  = single(fs * invSpeeds(end));
    preCoeff  = single(fs * (invSpeeds(1:end-1) - invSpeeds(2:end)));
    rb2s      = single(ssRadii .^ 2);
end
pre = single(fs / cSingle);

% ------------------------------------------------------------- 几何量
thDeg = thetaDeg;
th = single(thDeg * pi / 180);
Rj  = single(radii);
detx = Rj .* cos(th);
dety = Rj .* sin(th);

% 与 C++ appendImpl 一致：角度权用相邻 A-line 的平均角步长
if nd > 1
    totalArc = sum(abs(diff(thDeg)));
    arc = totalArc / (nd - 1) * pi / 180;
else
    arc = 0;
end

switch apodOpt
    case 'hann'
        apod = single(0.5 - 0.5 * cos(2 * pi * (0:nd-1) / nd));
    case 'hamming'
        apod = single(0.54 - 0.46 * cos(2 * pi * (0:nd-1) / nd));
    otherwise
        apod = ones(1, nd, 'single');
end
wscale = single(-arc) .* apod;   % 1 x nd

if isempty(minDist) || (isscalar(minDist) && minDist <= 0)
    if nx > 1
        minDist = single(abs(xv(2) - xv(1)));
    else
        minDist = single(1e-6);
    end
end
minDist = max(single(minDist), single(1e-9));

% ------------------------------------------------------------- GPU 准备
bpG = gpuArray(single(bscan));
Xg = gpuArray(xv);                 % 1 x nx
Yg = gpuArray(yv(:));              % ny x 1
r2 = Xg .^ 2 + Yg .^ 2;            % ny x nx

if chunk <= 0
    gridBytes = double(ny) * double(nx) * 4;
    g = gpuDevice;
    target = 0.35 * double(g.AvailableMemory);
    chunk = max(1, floor(target / (12 * gridBytes + 1)));
    chunk = min(chunk, 32);
end
chunk = min(max(round(chunk), 1), nd);

imgG = gpuArray.zeros(ny, nx, 'single');
sumWG = gpuArray.zeros(ny, nx, 'single');

% ------------------------------------------------------------- DAS 循环
for c0 = 1:chunk:nd
    cidx = c0:min(c0 + chunk - 1, nd);
    nc = numel(cidx);

    detxC = gpuArray(reshape(detx(cidx), 1, 1, nc));
    detyC = gpuArray(reshape(dety(cidx), 1, 1, nc));
    wscC  = gpuArray(reshape(wscale(cidx), 1, 1, nc));
    RjC   = gpuArray(reshape(Rj(cidx), 1, 1, nc));
    R2j   = RjC .* RjC;

    proj  = Xg .* detxC + Yg .* detyC;    % ny x nx x nc
    dotp  = proj - R2j;
    dist2 = r2 - 2 * dotp - R2j;
    dist  = sqrt(max(dist2, single(0)));
    dsafe = max(dist, minDist);

    % 飞行时间（样点数），分层声速路径与 CUDA 内核一致
    if useLayers
        tf = dist .* preOuter;
        for bi = 1:nBound
            rb2 = rb2s(bi);
            bothIn = (R2j <= rb2) & (r2 <= rb2);
            discr4 = dotp .* dotp - dist2 .* (R2j - rb2);
            cross = discr4 > single(0);
            sd = sqrt(max(discr4, single(0)));
            dist2d = max(dist2, single(1e-12));
            u1 = (-dotp - sd) ./ dist2d;
            u2 = (-dotp + sd) ./ dist2d;
            Lc = (min(max(u2, single(0)), single(1)) - ...
                  max(min(u1, single(1)), single(0))) .* dist;
            Lc = max(Lc, single(0));
            Li = dist .* single(bothIn) + Lc .* single(cross & ~bothIn);
            tf = tf + Li .* preCoeff(bi);
        end
    else
        tf = dist * pre;
    end

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
    if strcmp(interpOpt, 'linear')
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
    else
        i0n = cast(round(tf) + single(1), it);
        i0c = min(max(i0n, cast(1, it)), cast(Nt, it));
        li = i0c + offs;
        vv = colb(li);
        if maskOob
            valid = (i0n >= cast(1, it)) & (i0n <= cast(Nt, it));
            vv = vv .* single(valid);
        end
    end

    pw = pExp + 1;
    if pw == 2
        dsafeP = dsafe .* dsafe;
    elseif pw == 1
        dsafeP = dsafe;
    else
        dsafeP = dsafe .^ single(pw);
    end
    w = wscC .* dotp ./ (RjC .* dsafeP);

    wv = w .* vv;
    imgG = imgG + sum(wv, 3);
    sumWG = sumWG + sum(abs(w), 3);
end

% ------------------------------------------------------------- 归一化
tiny = single(1e-12);
accG = imgG;
imgG = accG ./ max(sumWG, tiny);

if fovDeg < 359.9999
    phi = atan2(Yg, Xg);
    dang = mod(phi - single(fovTh0Deg * pi / 180), single(2 * pi));
    fovMask = single(dang <= single(fovDeg * pi / 180));
    imgG = imgG .* fovMask;
    accG = accG .* fovMask;
    sumWG = sumWG .* fovMask;
end

img = gather(imgG);
if nargout >= 2
    acc = gather(accG);
end
if nargout >= 3
    accW = gather(sumWG);
end
end
