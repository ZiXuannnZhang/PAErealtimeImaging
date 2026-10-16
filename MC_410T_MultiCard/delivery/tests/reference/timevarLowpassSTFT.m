function [y, info] = timevarLowpassSTFT(x, Fs, fcMax, fcMin, p1, p2, vardata)
if ~ismatrix(x)
    error('输入 x 必须为二维矩阵。');
end
if ~isreal(x)
    error('当前函数只针对实数时域信号设计。');
end
if isempty(x)
    error('输入 x 不能为空。');
end
numSamples = size(x, 1);
numChannels = size(x, 2);
if Fs <= 0
    error('Fs 必须大于 0。');
end
if fcMax <= 0 || fcMax > Fs/2
    error('fcMax 必须满足 0 < fcMax <= Fs/2。');
end
if fcMin <= 0 || fcMin > fcMax
    error('fcMin 必须满足 0 < fcMin <= fcMax。');
end
if ~isscalar(p1) || p1 ~= round(p1)
    error('p1 必须为整数采样点位置。');
end
if ~isscalar(p2) || p2 ~= round(p2)
    error('p2 必须为整数采样点位置。');
end
if p1 < 1 || p1 > numSamples
    error('p1 必须位于 [1, size(x,1)] 范围内。');
end
if p2 < 1 || p2 > numSamples
    error('p2 必须位于 [1, size(x,1)] 范围内。');
end
if p2 <= p1
    error('必须满足 p2 > p1。');
end
t1 = (p1 - 1) / Fs;
t2 = (p2 - 1) / Fs;
p = inputParser;
addParameter(p, 'WindowLength', 128, ...
    @(v) isnumeric(v) && isscalar(v) && v >= 2);
addParameter(p, 'OverlapRatio', 0.75, ...
    @(v) isnumeric(v) && isscalar(v) && v >= 0 && v < 1);
addParameter(p, 'FFTLength', [], ...
    @(v) isempty(v) || ...
    (isnumeric(v) && isscalar(v) && v >= 2));
addParameter(p, 'FilterOrder', 4, ...
    @(v) isnumeric(v) && isscalar(v) && v > 0);
addParameter(p, 'Transition', 'smoothstep', ...
    @(v) ischar(v) || isstring(v));
parse(p, vardata{:});
winLength    = round(p.Results.WindowLength);
overlapRatio = p.Results.OverlapRatio;
nfft         = p.Results.FFTLength;
filterOrder  = p.Results.FilterOrder;
transition   = lower(string(p.Results.Transition));
if winLength > numSamples
    error(['WindowLength 不能大于信号的时间采样点数 ', ...
           'size(x,1)。']);
end
win = hann(winLength, 'periodic');
overlapLength = round(winLength * overlapRatio);
overlapLength = min(overlapLength, winLength - 1);
if isempty(nfft)
    nfft = 2^nextpow2(winLength);
else
    nfft = round(nfft);
    if nfft < winLength
        error('FFTLength 必须大于或等于 WindowLength。');
    end
end
[~, F, T] = stft( ...
    x(:,1), ...
    Fs, ...
    'Window', win, ...
    'OverlapLength', overlapLength, ...
    'FFTLength', nfft, ...
    'FrequencyRange', 'onesided');
numFrames = length(T);
samplePosition = T .* Fs + 1;
u = (samplePosition - p1) ./ (p2 - p1);
u = max(0, min(1, u));
switch transition
    case "smoothstep"
        q = 3 .* u.^2 - 2 .* u.^3;
    case "linear"
        q = u;
    otherwise
        error('Transition 只能设置为 smoothstep 或 linear。');
end
fc = fcMax - (fcMax - fcMin) .* q;
fc = fc(:).';
Fcol = F(:);
H = 1 ./ sqrt( ...
    1 + (Fcol ./ fc).^(2 * filterOrder) ...
    );
y = zeros(size(x), 'like', x);
tOriginal = (0:numSamples-1).' / Fs;
for ch = 1:numChannels
    S = stft( ...
        x(:,ch), ...
        Fs, ...
        'Window', win, ...
        'OverlapLength', overlapLength, ...
        'FFTLength', nfft, ...
        'FrequencyRange', 'onesided');
    Sf = S .* H;
    [yTemp, tTemp] = istft( ...
        Sf, ...
        Fs, ...
        'Window', win, ...
        'OverlapLength', overlapLength, ...
        'FFTLength', nfft, ...
        'FrequencyRange', 'onesided', ...
        'ConjugateSymmetric', true);
    yTemp = real(yTemp(:));
    tTemp = tTemp(:);
    if length(yTemp) == numSamples
        y(:,ch) = yTemp;
    else
        y(:,ch) = interp1( ...
            tTemp, ...
            yTemp, ...
            tOriginal, ...
            'linear', ...
            0);
    end
end
info.F = F;
info.T = T;
info.samplePosition = samplePosition;
info.fc = fc;
info.H = H;
info.p1 = p1;
info.p2 = p2;
info.t1 = t1;
info.t2 = t2;
info.WindowLength = winLength;
info.OverlapLength = overlapLength;
info.FFTLength = nfft;
info.FilterOrder = filterOrder;
info.Transition = transition;
info.NumSamples = numSamples;
info.NumChannels = numChannels;
end
