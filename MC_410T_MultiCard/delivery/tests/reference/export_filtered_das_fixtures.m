function export_filtered_das_fixtures(out)
% Independent MATLAB oracle; defaults are the entry script's, never helper fallback.
if ~exist(out,'dir'), mkdir(out); end
rng(20261019,'twister');
meta.matlab=version; meta.signal=ver('signal'); meta.seed=20261019; meta.cases={};
fs=250e6;
lengths=[4643,4630,7143,7130,4000,2031,17,18,5000,7500,7053,7037];
for k=1:numel(lengths)
    nt=lengths(k); t=(0:nt-1)'/fs;
    x=[zeros(nt,1),ones(nt,1),randn(nt,1)];
    for pos=unique([1,ceil(nt/2),nt])
        z=zeros(nt,1);z(pos)=1;x(:,end+1)=z;
    end
    for f=[2e6,5e6,20e6,50e6,70e6]
        x(:,end+1)=sin(2*pi*f*t);
    end
    x(:,end+1)=sin(2*pi*3e6*t)+.4*cos(2*pi*70e6*t);
    x=[x,x(:,3),x(:,2)]; % repeated/interleaved lines
    for mode=1:3
        opts=struct('Enabled',true,'Order',.5,'ReferenceHz',5e6,...
            'PassHz',40e6,'StopHz',60e6,'PadFactor',4,'BatchColumns',64);
        if mode==2, opts.Order=0;opts.PadFactor=8;opts.BatchColumns=7; end
        if mode==3
            opts.Order=1;opts.ReferenceHz=3e6;opts.PassHz=25e6;opts.StopHz=55e6;
            opts.PadFactor=4.25;opts.BatchColumns=1;
        end
        for type={'single','double'}
            kind=type{1}; input=cast(x,kind);
            [y,info]=pa2d_bandlimited_prefilter(input,fs,opts);
            assert(isa(y,kind)&&isequal(size(y),size(input))&&all(isfinite(y(:))));
            name=sprintf('case_%02d_%d_%s',k,mode,kind);
            writebin(fullfile(out,[name '_x.bin']),input,kind);
            writebin(fullfile(out,[name '_y.bin']),y,kind);
            writebin(fullfile(out,[name '_H.bin']),[real(info.Response),imag(info.Response)],'double');
            writebin(fullfile(out,[name '_F.bin']),info.FrequencyHz,'double');
            entry=struct('name',name,'kind',kind,'nt',nt,'columns',size(x,2),...
                'nfft',info.FFTLength,'left',info.PadLeftSamples,'opts',opts);
            meta.cases{end+1}=entry;
            fprintf('%s nt=%d cols=%d fft=%d left=%d\n',name,nt,size(x,2),info.FFTLength,info.PadLeftSamples);
        end
    end
end
% Production-chain oracle: identical full float input -> optional old STFT -> DBR/clip/cut -> new single.
% Delays vary across channels/wavelengths; do not concatenate their time axes.
for tv=0:1
    N=5000; cols=6; channels=8;
    raw=zeros(N,cols*channels,'single');
    for j=0:cols-1
        for c=0:channels-1
            i=(0:N-1)';raw(:,j*channels+c+1)=single(sin(i*.071+c*.2+j*.11)+.4*cos(i*1.7+j));
        end
    end
    writebin(fullfile(out,sprintf('service_%d_raw.bin',tv)),raw,'single');
    for c=0:channels-1
        for w=0:1
            delay=358+13*w+3*c; nt=N-delay+1;
            input=zeros(nt,cols/2,'single');
            for j=w:2:cols-1
                d=double(raw(:,j*channels+c+1));
                if tv
                    d=timevarLowpassSTFT(d,fs,45e6,1e6,780,2000,...
                        {'WindowLength',128,'OverlapRatio',.75,'FilterOrder',4,'Transition','smoothstep'});
                end
                d(1:300+13*w)=0;
                limit=double(single(.3+.1*w));d=max(-limit,min(limit,d));
                input(:,floor(j/2)+1)=single(d(delay:end));
            end
            writebin(fullfile(out,sprintf('service_%d_c%d_w%d_before.bin',tv,c,w)),input,'single');
            opts=struct('Enabled',true,'Order',.5,'ReferenceHz',5e6,'PassHz',40e6,...
                'StopHz',60e6,'PadFactor',4,'BatchColumns',64);
            y=pa2d_bandlimited_prefilter(input,fs,opts);
            writebin(fullfile(out,sprintf('service_%d_c%d_w%d_after.bin',tv,c,w)),y,'single');
        end
    end
end
f=fopen(fullfile(out,'manifest.json'),'w');fwrite(f,jsonencode(meta),'char');fclose(f);
end
function writebin(path,x,kind)
f=fopen(path,'w','ieee-le');assert(f>=0);fwrite(f,x,kind);fclose(f);
end
