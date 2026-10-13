% RUN_UNIPOLAR_DAS_14DAT
% One-click MATLAB visualization of current ring DAS plus:
%   - optional fixed frequency-domain low-frequency compensation
%   - lambda(t)=lambdaSlope*t derivative/inversion enhancement
%   - optional positive rectification through isplusonly
%
% Data: testdata/14.dat
%   - 4000 samples per A-line
%   - 200 MHz sampling rate
%   - 4000 A-lines per wavelength
%   - dual-wavelength raw columns are interleaved after wlOffset
%
% Enhancement chain:
%   p -> H(f) low-frequency compensation -> p_c
%   g = p_c - lambdaSlope*t*d(p_c)/dt
%   I = DAS(g)

clear; clc;
% close all;

scriptDir = fileparts(mfilename('fullpath'));
repoRoot = fileparts(fileparts(scriptDir));
addpath(fullfile(repoRoot, 'RadiusCalibration'));

dataPath = fullfile(repoRoot, 'testdata', '14.dat');
outputDir = fullfile(repoRoot, 'artifacts', 'unipolar_das_14dat');
if ~exist(outputDir, 'dir'), mkdir(outputDir); end

% Data and geometry parameters for 14.dat.
sampDepth = 4000;
fs = 200e6;
wlOffset = 301;
nRawColumns = 8300;
nAlinesPerWavelength = 4000;
sysDelay = [358, 371];
maskLength = 300;
shiftWL2 = true;
radius = 6.57e-3;
soundSpeed = 1490.0;
fov = 24e-3;
gridSizeMm = 0.01;
coverageDeg = 360;
distanceWeightExponent = 1;
isplusonly = 0;

% Frequency compensation:
%   H(f) = 1 + (Hmax - 1) * exp(-(f/fc)^order)
enableFreqComp = 1;
freqCompFcMhz = 10.0;
freqCompHmax = 2.0;
freqCompOrder = 2.0;
visualizeFreqComp = 1;
freqPlotAline = 1001;
freqPlotWavelength = 1;

% lambda(t)=lambdaSlope*t; keep the slope available for later tuning.
lambdaSlope = 1.0;

if exist(dataPath, 'file') ~= 2
    error('Data file not found: %s', dataPath);
end
info = dir(dataPath);
expectedBytes = sampDepth * nRawColumns * 8;
if info.bytes ~= expectedBytes
    error('Unexpected 14.dat size: got %d bytes, expected %d.', info.bytes, expectedBytes);
end

fprintf('Reading %s ...\n', dataPath);
[wl1, wl2] = localReadDualWavelength(dataPath, sampDepth, nRawColumns, wlOffset, shiftWL2);

p1 = localPreprocessParams(1, sysDelay, maskLength);
p2 = localPreprocessParams(2, sysDelay, maskLength);
bscan1 = localPreprocess(wl1, p1);
bscan2 = localPreprocess(wl2, p2);
clear wl1 wl2

if visualizeFreqComp
    if freqPlotWavelength == 1
        freqPlotSource = bscan1;
    else
        freqPlotSource = bscan2;
    end
    freqPlotPng = fullfile(outputDir, sprintf('frequency_compensation_aline%04d.png', freqPlotAline));
    localPlotFrequencyCompensation(freqPlotSource, fs, freqPlotAline, ...
        freqCompFcMhz, freqCompHmax, freqCompOrder, freqPlotPng);
end
bscan1 = localEnhancedSignal(bscan1, fs, lambdaSlope, enableFreqComp, ...
    freqCompFcMhz, freqCompHmax, freqCompOrder);
bscan2 = localEnhancedSignal(bscan2, fs, lambdaSlope, enableFreqComp, ...
    freqCompFcMhz, freqCompHmax, freqCompOrder);

nGrid = ceil(fov / (gridSizeMm * 1e-3));
x = linspace(-fov / 2, fov / 2, nGrid);
y = x;
thetaDeg = (0:nAlinesPerWavelength - 1) * (coverageDeg / nAlinesPerWavelength);
radii = radius * ones(1, nAlinesPerWavelength);

das = localDasParams(soundSpeed, distanceWeightExponent, gridSizeMm * 1e-3);
fprintf('Reconstructing enhanced DAS: freqComp=%d, lambdaSlope=%.3g ...\n', ...
    enableFreqComp, lambdaSlope);

tic;
[imgSigned1, acc1, accW1] = recon_ring_das_multir(single(bscan1), fs, thetaDeg, radii, x, y, das);
[imgSigned2, acc2, accW2] = recon_ring_das_multir(single(bscan2), fs, thetaDeg, radii, x, y, das);
elapsed = toc;

imgOutput1 = imgSigned1;
imgOutput2 = imgSigned2;
if isplusonly
    imgOutput1 = max(0, imgOutput1);
    imgOutput2 = max(0, imgOutput2);
end
fprintf('Reconstruction finished in %.2f s.\n', elapsed);

fig1 = figure('Name', 'Enhanced ring DAS - 14.dat', 'Color', 'w', 'Position', [100, 100, 1200, 560]);
% subplot(1, 2, 1);
localShowImage(x, y, imgOutput1, 'Enhanced DAS - 532 nm', isplusonly);
fig2 = figure('Name', 'Enhanced ring DAS - 14.dat', 'Color', 'w', 'Position', [100, 100, 1200, 560]);
% subplot(1, 2, 2);
localShowImage(x, y, imgOutput2, 'Enhanced DAS - 1064 nm', isplusonly);

pngPath = fullfile(outputDir, 'unipolar_das_14dat.png');
exportgraphics(fig1, pngPath, 'Resolution', 150);
matPath = fullfile(outputDir, 'unipolar_das_14dat.mat');
lambdaTimeOrigin = 'first sample after DelayCut';
save(matPath, 'imgSigned1', 'imgSigned2', 'imgOutput1', 'imgOutput2', ...
    'isplusonly', 'enableFreqComp', 'freqCompFcMhz', 'freqCompHmax', ...
    'freqCompOrder', 'visualizeFreqComp', 'freqPlotAline', ...
    'freqPlotWavelength', 'freqPlotPng', 'lambdaSlope', 'lambdaTimeOrigin', ...
    'acc1', 'acc2', 'accW1', 'accW2', 'x', 'y', 'thetaDeg', 'radii', ...
    'fs', 'radius', 'soundSpeed', 'gridSizeMm', 'sysDelay', ...
    'elapsed', '-v7.3');
fprintf('Saved %s\nSaved %s\n', pngPath, matPath);

function [wl1, wl2] = localReadDualWavelength(dataPath, sampDepth, nRawColumns, wlOffset, shiftWL2)
fid = fopen(dataPath, 'r', 'ieee-le');
if fid < 0, error('Cannot open %s', dataPath); end
cleanup = onCleanup(@() fclose(fid));
raw = fread(fid, [sampDepth, nRawColumns], 'double=>single');
if numel(raw) ~= sampDepth * nRawColumns
    error('Unexpected number of samples in %s.', dataPath);
end
wl1 = raw(:, wlOffset:2:nRawColumns);
wl2 = raw(:, wlOffset + 1:2:nRawColumns);
if size(wl1, 2) ~= 4000 || size(wl2, 2) ~= 4000
    error('Unexpected wavelength column counts: %d/%d.', size(wl1, 2), size(wl2, 2));
end
if shiftWL2
    wl2 = circshift(wl2, 1, 2);
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
p.DAQ = 200e6;
p.sysDelay = sysDelay;
p.wl = wavelengthIndex;
p.system_delay = sysDelay(wavelengthIndex);
end

function bscan = localPreprocess(raw, p)
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

function localPlotFrequencyCompensation(p, fs, lineIndex, fcMhz, hMax, curveOrder, outputPng)
if lineIndex < 1 || lineIndex > size(p, 2)
    error('freqPlotAline is outside the A-line range.');
end
x = double(p(:, lineIndex));
n = numel(x);
if n < 2
    error('Frequency-compensation plot requires at least two samples.');
end

f = (0:n - 1)' * fs / n;
f = min(f, fs - f);
half = floor(n / 2) + 1;
fPlot = f(1:half) / 1e6;

pre = fft(x);
preDb = 20 * log10(max(abs(pre(1:half)) / max(abs(pre)), eps));
H = 1 + (hMax - 1) * exp(-((f(1:half) * 1e6) / (fcMhz * 1e6)) .^ curveOrder);
xComp = localFrequencyCompensate(x, fs, fcMhz, hMax, curveOrder);
post = fft(xComp);
postDb = 20 * log10(max(abs(post(1:half)) / max(abs(pre)), eps));

fig = figure('Name', sprintf('Frequency compensation - A-line %d', lineIndex), ...
    'Color', 'w', 'Position', [100, 100, 900, 800]);

subplot(3, 1, 1);
plot(fPlot, preDb, 'LineWidth', 1.1);
grid on;
xlim([0, fs / 2e6]);
ylim([-100, 20]);
xlabel('Frequency (MHz)');
ylabel('Magnitude (dB)');
title('Before H(f)');

subplot(3, 1, 2);
plot(fPlot, H, 'LineWidth', 1.1);
grid on;
xlim([0, fs / 2e6]);
ylim([1, max(1.05, hMax * 1.05)]);
xlabel('Frequency (MHz)');
ylabel('H(f)');
title('H(f): monotonic low-frequency gain, H(f) -> 1');
yline(1, '--', 'High-frequency reference', 'LabelVerticalAlignment', 'bottom');

subplot(3, 1, 3);
plot(fPlot, postDb, 'LineWidth', 1.1);
grid on;
xlim([0, fs / 2e6]);
ylim([-100, 20]);
xlabel('Frequency (MHz)');
ylabel('Magnitude (dB)');
title('After H(f)');

exportgraphics(fig, outputPng, 'Resolution', 150);
fprintf('Saved frequency-compensation plot: %s\n', outputPng);
end
function g = localEnhancedSignal(p, fs, lambdaSlope, enableFreqComp, ...
    freqCompFcMhz, freqCompHmax, freqCompOrder)
if ~isscalar(lambdaSlope) || ~isfinite(lambdaSlope) || lambdaSlope <= 0
    error('lambdaSlope must be a positive finite scalar.');
end
if enableFreqComp
    p = localFrequencyCompensate(p, fs, freqCompFcMhz, freqCompHmax, freqCompOrder);
end

dp = zeros(size(p), 'like', p);
dp(1, :) = (p(2, :) - p(1, :)) * fs;
dp(end, :) = (p(end, :) - p(end - 1, :)) * fs;
if size(p, 1) > 2
    dp(2:end-1, :) = (p(3:end, :) - p(1:end-2, :)) * (fs / 2);
end
lambda = lambdaSlope * (0:size(p, 1) - 1)' / fs;
g = p - lambda .* dp;
end

function pComp = localFrequencyCompensate(p, fs, fcMhz, hMax, curveOrder)
if ~isscalar(fcMhz) || ~isfinite(fcMhz) || fcMhz <= 0
    error('freqCompFcMhz must be positive.');
end
if ~isscalar(hMax) || ~isfinite(hMax) || hMax < 1
    error('freqCompHmax must be finite and >= 1.');
end
if ~isscalar(curveOrder) || ~isfinite(curveOrder) || curveOrder <= 0
    error('freqCompOrder must be positive.');
end

% Odd reflection padding reduces FFT boundary ringing before DelayCut.
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

function localShowImage(x, y, img, titleText, isplusonly)
imagesc(x * 1e3, y * 1e3, img);
axis image;
set(gca, 'YDir', 'normal');
colormap(gray);
colorbar;
title(titleText);
xlabel('x (mm)');
ylabel('y (mm)');
scale = localRobustAbsScale(img);
if isempty(scale) || scale <= 0
    scale = 1;
end
if isplusonly
    caxis([0, scale]);
else
    caxis([-scale, scale]);
end
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