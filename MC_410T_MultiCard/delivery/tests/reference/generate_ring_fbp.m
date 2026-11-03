function generate_ring_fbp(outdir)
% Independent CPU oracle copied from the accepted MATLAB discretization.
% No GPU/PTX or production C++ math is used by this fixture generator.
if ~exist(outdir,'dir'),mkdir(outdir);end
n=256;fs=10e6;dt=1/fs;tau=(0:n-1)'*dt;Tm=max(tau',.25*dt);
F0=@(x,T)acosh(max(x./T,1));F1=@(x,T)sqrt(max(x.^2-T.^2,0));
lo=max(tau-dt,Tm);hi=max(tau,Tm);
A=((F1(hi,Tm)-F1(lo,Tm))-(tau-dt).*(F0(hi,Tm)-F0(lo,Tm)))/dt;
lo=max(tau,Tm);hi=max(tau+dt,Tm);
A=A+((tau+dt).*(F0(hi,Tm)-F0(lo,Tm))-(F1(hi,Tm)-F1(lo,Tm)))/dt;
D=(circshift(A,-1,2)-circshift(A,1,2))/(2*dt);
D(:,1)=(A(:,2)-A(:,1))/dt;D(:,end)=(A(:,end)-A(:,end-1))/dt;K=single(D');
angles=single((0:15)*22.5);P=single(sin((0:n-1)'*.27+(1:16)*.3).*exp(-((0:n-1)'-80).^2/1000));
Q=K*P;R=.004;c=1490;fov=.02;nx=20;x=linspace(-fov/2,fov/2,nx);[Y,X]=ndgrid(x,x);
im=zeros(nx,nx);
for j=1:numel(angles)
    th=double(angles(j))*pi/180;dist=hypot(X-R*cos(th),Y-R*sin(th));
    f=dist/c*fs;ii=floor(f);u=f-ii;valid=ii>=0&ii<n-2;
    ii=min(max(ii,0),n-2);q=reshape(double(Q(ii(:)+1,j)),size(X)).*(1-u)+reshape(double(Q(ii(:)+2,j)),size(X)).*u;
    weight=-(X*cos(th)+Y*sin(th)-R)./(dist*c*pi)*R*(22.5*pi/180);
    im=im+weight.*q.*valid;
end
write('P',P);write('K',K);write('image',single(im));
% Fractional signal correction and spatial median/DC reference.
p=single(sin((0:255)'*.3).*exp(-((0:255)'-80).^2/900));nn=2^nextpow2(numel(p)+64);
f=[0:nn/2,-nn/2+1:-1]'/nn;q=single(real(ifft(fft(double(p),nn).*exp(1i*2*pi*f*1.375))));
write('shift_input',p);write('shift_expected',q(1:256));
fprintf('FBP CPU MATLAB fixtures generated in %s\n',outdir);
    function write(name,v)
        fid=fopen(fullfile(outdir,[name '.f32']),'wb');assert(fid>=0);fwrite(fid,v,'single');fclose(fid);
    end
end
