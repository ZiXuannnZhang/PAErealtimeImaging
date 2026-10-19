function [out, info] = pa2d_bandlimited_prefilter(data, fs, opts)
%PA2D_BANDLIMITED_PREFILTER Band-limited 2-D pressure prefilter for time-sample BP.
% data: real CPU single/double [time samples x A-lines]. See FilteredDAS_2D_notes.md.
% Enabled=false returns data unchanged, without casting or an FFT.

if nargin < 3, opts = struct(); end
validateattributes(data, {'single','double'}, {'real','2d','nonempty','finite'});
validateattributes(fs, {'numeric'}, {'real','scalar','finite','positive'});
if isa(data, 'gpuArray')
    error('pa2d_prefilter:CPUInput', 'Pass CPU data; the existing BP handles GPU transfer.');
end
cfg = struct('Enabled', true, 'Order', 0.5, 'ReferenceHz', 40e6, ...
    'PassHz', 50e6, 'StopHz', 60e6, 'PadFactor', 4, 'BatchColumns', 64);
names = fieldnames(opts);
for j = 1:numel(names)
    if ~isfield(cfg, names{j})
        error('pa2d_prefilter:Option', 'Unknown option: %s.', names{j});
    end
    cfg.(names{j}) = opts.(names{j});
end
validateattributes(cfg.Enabled, {'logical','numeric'}, {'scalar','binary'});
if ~cfg.Enabled
    out = data;
    info = struct('Config', cfg, 'Enabled', false, 'FFTLength', 0);
    return;
end
validateattributes(cfg.Order, {'numeric'}, {'real','scalar','finite','>=',0,'<=',1});
validateattributes(cfg.ReferenceHz, {'numeric'}, {'real','scalar','finite','positive'});
validateattributes(cfg.PassHz, {'numeric'}, {'real','scalar','finite','positive'});
validateattributes(cfg.StopHz, {'numeric'}, {'real','scalar','finite','>',cfg.PassHz,'<',fs/2});
validateattributes(cfg.ReferenceHz, {'numeric'}, {'<=',cfg.PassHz});
validateattributes(cfg.PadFactor, {'numeric'}, {'real','scalar','finite','>=',4});
validateattributes(cfg.BatchColumns, {'numeric'}, {'real','scalar','finite','integer','positive'});

nt = size(data, 1);
nfft = 2^nextpow2(cfg.PadFactor * nt);
left = floor((nfft - nt)/2);
k = (0:nfft-1)';
k(k > nfft/2) = k(k > nfft/2) - nfft;
f = k * (fs/nfft);
af = abs(f);
window = ones(nfft, 1);
transition = af > cfg.PassHz & af < cfg.StopHz;
window(transition) = 0.5 * (1 + cos(pi * ...
    (af(transition)-cfg.PassHz)/(cfg.StopHz-cfg.PassHz)));
window(af >= cfg.StopHz) = 0;
% Negative phase conjugates the 2-D point-source propagation phase at f > 0.
H = (af/cfg.ReferenceHz).^cfg.Order .* ...
    exp(-1i * sign(f) * pi * cfg.Order/2) .* window;
H(1) = 0;
H(nfft/2+1) = 0; % A real Nyquist bin cannot carry a fractional complex phase.

out = zeros(size(data), 'like', data);
HC = cast(H, 'like', data);
for c0 = 1:cfg.BatchColumns:size(data, 2)
    cols = c0:min(c0+cfg.BatchColumns-1, size(data, 2));
    padded = zeros(nfft, numel(cols), 'like', data);
    padded(left+(1:nt), :) = data(:, cols);
    filtered = real(ifft(fft(padded, [], 1) .* HC, [], 1));
    out(:, cols) = filtered(left+(1:nt), :);
end
if any(~isfinite(out(:)))
    error('pa2d_prefilter:Nonfinite', 'Filtering overflowed; check input scale.');
end
info = struct('Config', cfg, 'Enabled', true, 'FFTLength', nfft, ...
    'PadLeftSamples', left, 'FrequencyHz', f, 'Response', H, ...
    'PeakGain', max(abs(H)), 'InputSamples', nt);
end
