function export_fbp_calibration(path,fs,angleOffsetDeg,delayOffsetSamples,X,startSample,prefixPerWavelength)
% Export RING calibration only. angles: 8 physical channels, delays: 8 x 2.
% X optional: samples x positions x 8 channels x 2 wavelengths, at the SAME
% frontend-filter stage/configuration as ImagingSvc input (before DBR/DelayCut).
% Radius values remain in the existing per-channel radius UI; never map linear ys/zs.
assert(isscalar(fs)&&isfinite(fs)&&fs>0);
assert(numel(angleOffsetDeg)==8&&all(isfinite(angleOffsetDeg(:))));
assert(isequal(size(delayOffsetSamples),[8 2])&&all(isfinite(delayOffsetSamples(:))));
o=struct('formatVersion',1,'geometry','concentric-ring','daqHz',fs, ...
    'angleOffsetDeg',reshape(angleOffsetDeg,1,8),'delayOffsetSamples',delayOffsetSamples, ...
    'created',datestr(now,31));
if nargin>=5&&~isempty(X)
    assert(nargin==7&&size(X,3)==8&&size(X,4)==2&&prefixPerWavelength>=8&&prefixPerWavelength<=256);
    assert(startSample>=1&&startSample+400<=size(X,1));
    templates=cell(8,2);
    for c=1:8,for w=1:2
        a=double(X(startSample:startSample+400,:,c,w));
        templates{c,w}=median(a-median(a,1),2)';
    end,end
    % Explicit nested cell lists preserve [channel][wavelength][sample] JSON axes.
    rows=cell(1,8);for c=1:8,rows{c}={templates{c,1},templates{c,2}};end
    o.artifact=struct('startSample',startSample,'prefixPerWavelength',prefixPerWavelength,'templates',{rows});
end
fid=fopen(path,'w');assert(fid>=0);guard=onCleanup(@()fclose(fid));fprintf(fid,'%s',jsonencode(o));
end
