% RUN_CURRENT_DAS_14DAT
% One-click MATLAB visualization of the current ring DAS implementation.
%
% Data: testdata/14.dat
%   - 4000 samples per A-line
%   - 200 MHz sampling rate
%   - 4000 A-lines per wavelength
%   - dual-wavelength raw columns are interleaved after wlOffset
%
% The reconstruction core is RadiusCalibration/recon_ring_das_multir.m,
% which mirrors the current C++/CUDA ring_das_kernel and returns
% acc / max(accW, eps), exactly like ring_recon_cuda_snapshot.
% isplusonly controls optional positive rectification for display/output.

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
radius = 6.57e-3;       % current ring_recon default/reference setting
soundSpeed = 1490.0;
fov = 24e-3;
gridSizeMm = 0.01;      % use 0.02 for higher-resolution inspection
coverageDeg = 360;
distanceWeightExponent = 1;
isplusonly = 0;
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

nGrid = ceil(fov / (gridSizeMm * 1e-3));
x = linspace(-fov / 2, fov / 2, nGrid);
y = x;
thetaDeg = (0:nAlinesPerWavelength - 1) * (coverageDeg / nAlinesPerWavelength);
radii = radius * ones(1, nAlinesPerWavelength);

das = localDasParams(soundSpeed, distanceWeightExponent, gridSizeMm * 1e-3);
fprintf('Reconstructing current DAS: grid=%d x %d, A-lines=%d ...\n', nGrid, nGrid, nAlinesPerWavelength);

tic;
[img1, acc1, accW1] = recon_ring_das_multir(single(bscan1), fs, thetaDeg, radii, x, y, das);
[img2, acc2, accW2] = recon_ring_das_multir(single(bscan2), fs, thetaDeg, radii, x, y, das);
elapsed = toc;
imgSigned1 = img1;
imgSigned2 = img2;
if isplusonly
    img1 = max(0, img1);
    img2 = max(0, img2);
end
fprintf('Reconstruction finished in %.2f s.\n', elapsed);

fig1 = figure('Name', 'Current ring DAS - 14.dat', 'Color', 'w', 'Position', [100, 100, 1200, 560]);
% subplot(1, 2, 1);
localShowImage(x, y, img1, 'Current DAS - 532 nm', isplusonly);
% subplot(1, 2, 2);
fig2 = figure('Name', 'Current ring DAS - 14.dat', 'Color', 'w', 'Position', [100, 100, 1200, 560]);
localShowImage(x, y, img2, 'Current DAS - 1064 nm', isplusonly);

pngPath = fullfile(outputDir, 'current_das_14dat.png');
exportgraphics(fig, pngPath, 'Resolution', 150);
matPath = fullfile(outputDir, 'current_das_14dat.mat');
save(matPath, 'img1', 'img2', 'imgSigned1', 'imgSigned2', 'isplusonly', 'acc1', 'acc2', 'accW1', 'accW2', ...
    'x', 'y', 'thetaDeg', 'radii', 'fs', 'radius', 'soundSpeed', ...
    'gridSizeMm', 'sysDelay', 'elapsed', '-v7.3');
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
% Keep the preprocessing semantics of preprocessBlock / preprocess_ring_block.
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