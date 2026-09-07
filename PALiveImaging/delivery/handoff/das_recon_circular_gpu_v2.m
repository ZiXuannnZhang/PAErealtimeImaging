function [img, img_raw, weight_sum] = das_recon_circular_gpu_v2(bp, fs, c, R, varargin)
% DAS_RECON_CIRCULAR_GPU_V2 Delay-and-sum (DAS) photoacoustic reconstruction
% for circular/ring-array data, accelerated on GPU.
%
% Two grid syntaxes are supported:
%   img = das_recon_circular_gpu_v2(bp, fs, c, R, x1, x2, xd, y1, y2, yd, ...)
%   img = das_recon_circular_gpu_v2(bp, fs, c, R, xv, yv, ...)
%
% Inputs:
%   bp   [Nt x nd] transducer time series. For universal back-projection use
%        the integrand bp = 2*p - 2*t*dp/dt.
%   fs   sampling rate [Hz], c  sound speed [m/s], R  ring radius [m].
%
% Name-Value options:
%   'CoverageDeg'            detector angular coverage, deg (default 360)
%   'Theta0Deg'              coverage start angle, deg (default 0)
%   'FOVDeg'                 imaging sector width, deg, starting at
%                            FOVTheta0Deg; [] or >=360 means full circle
%                            (default [])
%   'FOVTheta0Deg'           imaging sector start angle, deg (default 0).
%                            When FOVTheta0Deg=0 and Theta0Deg=0, FOV and
%                            detector coverage start at the same angle.
%   'SoundSpeedRadii'        boundary radii of concentric sound-speed layers
%                            [m], ascending (default [] = single speed)
%   'SoundSpeeds'            sound speed list [m/s] (any length). When
%                            SoundSpeedRadii is empty, SoundSpeeds(1) is used
%                            as the single speed; otherwise the first
%                            numel(SoundSpeedRadii)+1 elements are used as the
%                            layer speeds (default [])
%   'Interpolation'          'linear' (default) or 'nearest'
%   'DistanceWeightExponent' spreading exponent p in w ~ cos(theta)/dist^p
%                            (default 1; 2 reproduces the original behavior;
%                            total amplitude decay is 1/dist^(p+1))
%   'NormalizeByW'           divide by accumulated abs(weight) (default true;
%                            original divided by signed weight sum)
%   'Apodization'            'none' (default), 'hann' or 'hamming'
%   'UseModulation'          multiply by exp(-|angle|) (default false)
%   'UseCF'                  coherence factor weighting (default false)
%   'CFPower'                CF exponent (default 1)
%   'CFMix'                  CF mix factor, 1 = full CF (default 1)
%   'UseEnvelope'            return |analytic signal| (default false)
%   'EnvelopeDim'            Hilbert envelope dimension (default 1)
%   'MaskOutOfRange'         zero pixels with delay outside data (default true)
%   'MinDistance'            clamp distance in weights (default one grid step)
%   'ChunkSize'              detectors per GPU batch; 0 = auto (default 0)
%
% Output:
%   img     [ny x nx] single precision image (CF-mixed when UseCF is on).
%   img_raw [ny x nx] raw DAS image (same post-processing except CF);
%            only returned when two outputs are requested.
%   weight_sum [ny x nx] accumulated abs(weight) map; only returned when
%               three outputs are requested (for unified normalization).

% ------------------------------------------------------------------ parse grid
if nargin < 5
    error('das_recon_circular_gpu_v2:Args', 'Not enough input arguments.');
end
if numel(varargin) >= 6 && ...
        all(cellfun(@(v) isnumeric(v) && isscalar(v), varargin(1:6)))
    xv = single(varargin{1}):single(varargin{3}):single(varargin{2});
    yv = single(varargin{4}):single(varargin{6}):single(varargin{5});
    optargs = varargin(7:end);
elseif numel(varargin) >= 2 && isnumeric(varargin{1}) && isnumeric(varargin{2}) ...
        && ~isscalar(varargin{1})
    xv = single(varargin{1}(:).');
    yv = single(varargin{2}(:).');
    optargs = varargin(3:end);
else
    error('das_recon_circular_gpu_v2:GridSpec', ...
        'Unrecognized grid. Use (...,x1,x2,xd,y1,y2,yd,...) or (...,xv,yv,...).');
end

% ------------------------------------------------------------------ options
p = inputParser;
p.addParameter('CoverageDeg', 360);
p.addParameter('Theta0Deg', 0);
p.addParameter('FOVDeg', []);
p.addParameter('FOVTheta0Deg', 0);
p.addParameter('SoundSpeedRadii', []);
p.addParameter('SoundSpeeds', []);
p.addParameter('Interpolation', 'linear');
p.addParameter('DistanceWeightExponent', 1);
p.addParameter('NormalizeByW', true);
p.addParameter('Apodization', 'none');
p.addParameter('UseModulation', false);
p.addParameter('UseCF', false);
p.addParameter('CFPower', 1);
p.addParameter('CFMix', 1);
p.addParameter('UseEnvelope', false);
p.addParameter('EnvelopeDim', 1);
p.addParameter('MaskOutOfRange', true);
p.addParameter('MinDistance', []);
p.addParameter('ChunkSize', 0);
p.parse(optargs{:});

coverage_deg = double(p.Results.CoverageDeg);
theta0_deg   = double(p.Results.Theta0Deg);
fov_deg      = double(p.Results.FOVDeg);
if isempty(fov_deg), fov_deg = coverage_deg; end
fov_theta0_deg = double(p.Results.FOVTheta0Deg);

iv = p.Results.Interpolation;
if isnumeric(iv)
    if isequal(iv, 1)
        iv = 'linear';
    elseif isequal(iv, 0)
        iv = 'nearest';
    else
        error('das_recon_circular_gpu_v2:Interp', 'Interpolation must be 0/1 or ''nearest''/''linear''.');
    end
end
interp_opt = validatestring(lower(char(iv)), {'linear','nearest'}, ...
    mfilename, 'Interpolation');

av = p.Results.Apodization;
apod_opt = validatestring(lower(char(av)), {'none','hann','hamming'}, ...
    mfilename, 'Apodization');

norm_by_w = logical(p.Results.NormalizeByW);
use_mod   = logical(p.Results.UseModulation);
use_cf    = logical(p.Results.UseCF);
use_env   = logical(p.Results.UseEnvelope);
mask_oob  = logical(p.Results.MaskOutOfRange);
cf_power  = double(p.Results.CFPower);
cf_mix    = double(p.Results.CFMix);
env_dim   = double(p.Results.EnvelopeDim);
p_exp     = double(p.Results.DistanceWeightExponent);
chunk     = round(double(p.Results.ChunkSize));

% 声速统一由 SoundSpeeds 控制：
%  - SoundSpeedRadii 为空：单声速 = SoundSpeeds(1)（SoundSpeeds 也为空时回退输入 c）
%  - SoundSpeedRadii 非空（k 个边界）：启用 SoundSpeeds 前 k+1 个元素分层
ss_radii  = double(p.Results.SoundSpeedRadii);
ss_speeds = double(p.Results.SoundSpeeds);
if isempty(ss_radii)
    if ~isempty(ss_speeds)
        c = ss_speeds(1);          % 单声速由 SoundSpeeds 第一个元素决定
    end
    use_layers = false;
else
    if isempty(ss_speeds)
        error('das_recon_circular_gpu_v2:SoundSpeed', ...
            'SoundSpeedRadii 非空时必须提供 SoundSpeeds。');
    end
    if numel(ss_speeds) < numel(ss_radii) + 1
        error('das_recon_circular_gpu_v2:SoundSpeed', ...
            'SoundSpeeds 的元素数至少为边界数+1（当前 %d < %d）。', ...
            numel(ss_speeds), numel(ss_radii) + 1);
    end
    ss_speeds = ss_speeds(1:numel(ss_radii) + 1);   % 只启用前 k+1 个
    use_layers = true;
end
if use_layers
    if any(ss_radii <= 0) || any(diff(ss_radii) <= 0)
        error('das_recon_circular_gpu_v2:SoundSpeed', ...
            'SoundSpeedRadii 必须为正且严格递增。');
    end
end
if ~isempty(ss_speeds) && any(ss_speeds <= 0)
    error('das_recon_circular_gpu_v2:SoundSpeed', 'SoundSpeeds 必须为正。');
end

% ------------------------------------------------------------------ geometry
[Nt, nd] = size(bp);
if Nt < 2
    error('das_recon_circular_gpu_v2:Nt', 'bp must have at least 2 time samples.');
end

xv = single(xv(:).');
yv = single(yv(:).');
nx = numel(xv);
ny = numel(yv);
if nx < 2 || ny < 2
    error('das_recon_circular_gpu_v2:Grid', 'Grid must have at least 2x2 points.');
end

dxs = min(diff(xv));
dys = min(diff(yv));
min_dist = p.Results.MinDistance;
if isempty(min_dist)
    min_dist = min(dxs, dys);
end
min_dist = max(single(min_dist), single(1e-9));

if coverage_deg >= 359.999
    th_deg = theta0_deg + (0:nd-1)*(coverage_deg/nd);
    arc    = coverage_deg/nd*pi/180;
else
    th_deg = theta0_deg + (0:nd-1)*(coverage_deg/max(nd-1,1));
    arc    = coverage_deg/max(nd-1,1)*pi/180;
end
th = single(th_deg*pi/180);
detx_h = single(R)*cos(th);
dety_h = single(R)*sin(th);

switch apod_opt
    case 'hann'
        apod_h = single(0.5 - 0.5*cos(2*pi*(0:nd-1)/nd));
    case 'hamming'
        apod_h = single(0.54 - 0.46*cos(2*pi*(0:nd-1)/nd));
    otherwise
        apod_h = ones(1, nd, 'single');
end
wscale = -single(arc) .* apod_h;                 % 1 x nd

% -------------------------------------------------------------------- GPU data
bp_g = gpuArray(single(bp));
Xg   = gpuArray(xv);                             % 1 x nx
Yg   = gpuArray(yv(:));                          % ny x 1
r2   = Xg.^2 + Yg.^2;                            % ny x nx
r2mR2 = r2 - single(R*R);                        % precomputed once

pre  = single(fs/c);                             % samples per meter
R2s  = single(R*R);

% 分层声速预计算：t = d/c_outer + sum(L_i * (1/c_{i-1} - 1/c_i))
if use_layers
    inv_speeds = single(1 ./ ss_speeds);
    pre_outer  = single(fs * inv_speeds(end));
    pre_coeff  = single(fs * (inv_speeds(1:end-1) - inv_speeds(2:end)));
    rb2s       = single(ss_radii.^2);
    n_bound    = numel(ss_radii);
else
    pre_outer = single(0);
    pre_coeff = single(0);
    rb2s      = single(0);
    n_bound   = 0;
end

if chunk <= 0
    grid_bytes = double(ny)*double(nx)*4;
    g = gpuDevice;
    target = 0.35 * double(g.AvailableMemory);   % keep per-chunk temp arrays bounded
    chunk = max(1, floor(target / (12*grid_bytes + 1)));
    chunk = min(chunk, 32);
end
chunk = min(max(round(chunk), 1), nd);

img_g = gpuArray.zeros(ny, nx, 'single');
need_sum_w = norm_by_w || use_cf || (nargout >= 3);   % 第三输出需要权重和（统一归一化）
if need_sum_w
    sum_w_g = gpuArray.zeros(ny, nx, 'single');
end
if use_cf
    pwr_g    = gpuArray.zeros(ny, nx, 'single');
    sum_w2_g = gpuArray.zeros(ny, nx, 'single');
end

% --------------------------------------------------------- main DAS loop
for c0 = 1:chunk:nd
    cidx  = c0:min(c0+chunk-1, nd);
    nc    = numel(cidx);
    detx_c = reshape(detx_h(cidx), 1, 1, nc);
    dety_c = reshape(dety_h(cidx), 1, 1, nc);
    wsc_c  = reshape(wscale(cidx), 1, 1, nc);

    proj = Xg.*detx_c + Yg.*dety_c;              % ny x nx x nc
    dotp = proj - R2s;
    dist2 = r2mR2 - 2*dotp;
    dist  = sqrt(max(dist2, single(0)));
    dsafe = max(dist, min_dist);

    % 飞行时间（样点数）：单声速直接 dist*fs/c；
    % 分层声速按直线射线与同心圆边界求交，分段累计 1/c
    if use_layers
        tf = dist .* pre_outer;
        for bi = 1:n_bound
            rb2 = rb2s(bi);
            s_in = R2s <= rb2;                       % 传感器在边界内？
            both_in = s_in & (r2 <= rb2);            % 两端都在边界内
            discr4 = dotp.^2 - dist2 .* (R2s - rb2); % 判别式/4
            cross = discr4 > single(0);
            sd = sqrt(max(discr4, single(0)));
            dist2d = max(dist2, single(1e-12));      % 防除零
            u1 = (-dotp - sd) ./ dist2d;
            u2 = (-dotp + sd) ./ dist2d;
            Lc = (min(max(u2, single(0)), single(1)) - ...
                  max(min(u1, single(1)), single(0))) .* dist;
            Lc = max(Lc, single(0));
            Li = dist .* single(both_in) + ...
                 Lc .* single(cross & ~both_in);
            tf = tf + Li .* pre_coeff(bi);
        end
    else
        tf = dist*pre;
    end
    i0f   = floor(tf);
    frac  = tf - i0f;

    if Nt*nc <= 65535
        it = 'uint16';
    else
        it = 'uint32';
    end
    i0   = cast(i0f + single(1), it);
    offs = cast(reshape((0:nc-1)*Nt, 1, 1, nc), it);

    if strcmp(interp_opt, 'linear')
        i0c = min(max(i0, cast(1, it)), cast(Nt-1, it));
        li0 = i0c + offs;
        li1 = li0 + cast(1, it);
        colb = bp_g(:, cidx);
        v0 = colb(li0);
        v1 = colb(li1);
        vv = v0 + frac.*(v1 - v0);
        if mask_oob
            valid = (i0 >= cast(1, it)) & (i0 <= cast(Nt-1, it));
            vv = vv .* single(valid);
        end
    else
        i0n = cast(round(tf) + single(1), it);
        i0c = min(max(i0n, cast(1, it)), cast(Nt, it));
        li = i0c + offs;
        colb = bp_g(:, cidx);
        vv = colb(li);
        if mask_oob
            valid = (i0n >= cast(1, it)) & (i0n <= cast(Nt, it));
            vv = vv .* single(valid);
        end
    end

    pw = p_exp + 1;                     % total 1/dist power (incl. obliquity)
    if pw == 2
        dsafe_p = dsafe.*dsafe;
    elseif pw == 1
        dsafe_p = dsafe;
    else
        dsafe_p = dsafe.^single(pw);
    end
    w = wsc_c .* dotp ./ (single(R) .* dsafe_p);

    if use_mod
        cosf = -dotp ./ (dsafe .* single(R));
        cosf = min(max(cosf, single(-1)), single(1));
        w = w .* exp(-abs(acos(cosf)));
    end

    wv = w .* vv;
    img_g = img_g + sum(wv, 3);
    if need_sum_w
        sum_w_g = sum_w_g + sum(abs(w), 3);
    end
    if use_cf
        pwr_g    = pwr_g + sum(wv.*wv, 3);
        sum_w2_g = sum_w2_g + sum(w.*w, 3);
    end
end

% ------------------------------------------------------------- post-process
tiny = single(1e-12);
want_raw = (nargout >= 2);
img_raw_g = img_g;
if use_cf
    neff = sum_w_g.^2 ./ max(sum_w2_g, tiny);
    cf   = img_g.^2 ./ max(neff .* pwr_g, tiny);
    cf   = min(max(cf, single(0)), single(1));
    cf   = cf.^single(cf_power);
    if norm_by_w
        img_raw_g = img_g ./ max(sum_w_g, tiny);
    else
        img_raw_g = img_g;
    end
    img_cf = img_raw_g .* cf;
    img_g = single(cf_mix) .* img_cf + single(1-cf_mix) .* img_raw_g;
elseif norm_by_w
    img_raw_g = img_g ./ max(sum_w_g, tiny);
    img_g = img_raw_g;
end

if use_env
    img_g = gpuHilbertAbs(img_g, env_dim);
    if want_raw
        img_raw_g = gpuHilbertAbs(img_raw_g, env_dim);
    end
end

if fov_deg < 359.9999
    fov_th0_rad = single(fov_theta0_deg*pi/180);
    phi  = atan2(Yg, Xg);
    dang = mod(phi - fov_th0_rad, single(2*pi));     % 相对 FOV 起点的角度 [0,2pi)
    fovmask = single(dang <= single(fov_deg*pi/180));
    img_g = img_g .* fovmask;
    if want_raw
        img_raw_g = img_raw_g .* fovmask;
    end
    if nargout >= 3
        sum_w_g = sum_w_g .* fovmask;
    end
end

img = gather(img_g);
if want_raw
    img_raw = gather(img_raw_g);   % only gathered when two outputs are requested
end
if nargout >= 3
    weight_sum = gather(sum_w_g);
end
end

% ------------------------------------------------------------- local helpers
function h = gpuHilbertAbs(g, dim)
n = size(g, dim);
if rem(n, 2) == 0
    resp = [1; 2*ones(n/2-1, 1); 1; zeros(n/2-1, 1)];
else
    resp = [1; 2*ones((n-1)/2, 1); zeros((n-1)/2, 1)];
end
sz = ones(1, max(ndims(g), 2));
sz(dim) = n;
resp = gpuArray(single(reshape(resp, sz)));
h = abs(ifft(fft(g, [], dim) .* resp, [], dim));
end