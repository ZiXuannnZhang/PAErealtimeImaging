function export_timevar_fixtures(out)
% Independent oracle: MATLAB + Signal Processing Toolbox, never C++ generated.
if ~exist(out,'dir'), mkdir(out); end
rng(20261016,'twister');
meta.matlab = version; meta.signal = ver('signal');
meta.seed = 20261016; meta.cases = {};
specs = [2030 128 0 .75;5000 128 0 .75;7500 128 0 .75;2031 128 256 .75;...
    128 128 192 .75;129 127 129 .75;257 129 192 .6;256 128 256 .5;...
    128 128 128 0;128 128 128 .01;2 2 0 .75;131 3 3 .75];
for k=1:size(specs,1)
    N=specs(k,1); W=specs(k,2); fftlen=specs(k,3); ov=specs(k,4);
    p1=min(780,max(1,floor(N/3))); p2=min(2000,N);
    if N>=2030, p1=780; end
    t=(0:N-1)'/250e6;
    x=[zeros(N,1),ones(N,1),sin(2*pi*3e6*t)+.4*cos(2*pi*70e6*t),randn(N,1)];
    for pos=unique([1,N,max(1,p1-1),p1,min(N,p1+1),p2])
        z=zeros(N,1); z(pos)=1; x(:,end+1)=z;
    end
    for mode=1:3
        c.enabled=true; c.fcMaxMhz=45; c.fcMinMhz=1; c.p1=p1; c.p2=p2;
        c.windowLength=W; c.overlapRatio=ov; c.fftLength=fftlen;
        c.filterOrder=4; c.transition='smoothstep';
        if mode>=2, c.transition='linear'; c.filterOrder=2.5; end
        if mode==2, c.fcMinMhz=45; end
        args={'WindowLength',W,'OverlapRatio',ov,'FilterOrder',c.filterOrder,'Transition',c.transition};
        if fftlen>0, args=[args,{'FFTLength',fftlen}]; end
        name=sprintf('case_%02d_%d',k,mode); entry.name=name; entry.config=c;
        try
            [y,info]=timevarLowpassSTFT(x,250e6,45e6,c.fcMinMhz*1e6,p1,p2,args);
            win=hann(W,'periodic');
            S=stft(x(:,4),250e6,'Window',win,'OverlapLength',info.OverlapLength,...
                'FFTLength',info.FFTLength,'FrequencyRange','onesided');
            [yt,tt]=istft(S.*info.H,250e6,'Window',win,'OverlapLength',info.OverlapLength,...
                'FFTLength',info.FFTLength,'FrequencyRange','onesided','ConjugateSymmetric',true);
            entry.finite=all(isfinite(y(:))) && all(isfinite(yt(:)));
            entry.N=N; entry.columns=size(x,2); entry.frames=numel(info.T);
            entry.yTempLength=numel(yt); entry.tFirst=tt(1); entry.tLast=tt(end);
            writematrix(x,fullfile(out,[name '_x.csv']));
            writematrix(y,fullfile(out,[name '_y.csv']));
            writematrix([real(yt(:)),tt(:)],fullfile(out,[name '_temp.csv']));
            writematrix(info.F(:),fullfile(out,[name '_F.csv']));
            writematrix([info.T(:),info.samplePosition(:),info.fc(:)],fullfile(out,[name '_T.csv']));
            writematrix(info.H,fullfile(out,[name '_H.csv']));
            fprintf('%s N=%d W=%d hop=%d fft=%d frames=%d temp=%d t=[%.17g %.17g] finite=%d\n',...
                name,N,W,W-info.OverlapLength,info.FFTLength,numel(info.T),numel(yt),tt(1),tt(end),entry.finite);
        catch ex
            entry.error=ex.message; fprintf('%s REJECT %s\n',name,ex.message);
        end
        meta.cases{end+1}=entry;
    end
end
f=fopen(fullfile(out,'manifest.json'),'w'); fwrite(f,jsonencode(meta),'char'); fclose(f);
end
