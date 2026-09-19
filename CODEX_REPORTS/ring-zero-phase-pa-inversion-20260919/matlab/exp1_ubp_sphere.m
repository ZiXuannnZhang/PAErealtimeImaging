% EXP1_UBP_SPHERE  判定性实验 1：以紧支撑均匀球体验证 Xu-Wang PRE 71,016706 (2005)
% 万有反投影（UBP, Eq.(20)-(22)）的精确常数与权重实现。
%
% 背景
% ----
% Xu & Wang (PRE 2005) Eq.(20)：p0(b)(r) = (1/Omega0) ∮_S b(r0, r~) dΩ0，
%   b(r0, r~) = 2 p(r0, r~) − 2 r~ ∂p(r0, r~)/∂r~，r~ = |r − r0|（长度单位），
%   dΩ0 = dS0 (n0·(r − r0))/r~^3（n0 指向源侧），Omega0 = 4π（闭合曲面内点）。
% 该式对"完整闭合曲面 + 源在面内"严格成立；对非紧支撑本征模式不适用
% （exp1 初版的教训），因此本实验改用紧支撑均匀球体（闭式前向数据）。
%
% 3D 前向模型（均匀球体，半径 a，初始声压 f0，声速 c）：
%   球性均值 M(rho0, r) = f0 * F(rho0, r)，
%   F(rho0, r) = clamp((1 − cos(psi_c))/2, 0, 1)，
%   cos(psi_c) = (rho0^2 + r^2 − a^2)/(2 rho0 r)   [球-球相交立体角分数]
%   p(x0, t) = ∂/∂t [ t · M(|x0|, c t) ]   (3D Poisson / Kirchhoff 公式)
%
% 判据：对位于原点、半径 a 的均匀球体，重构
%   f_rec(rho) = (1/4π) ∫ b(x0, r~(theta0)) (n0·(x−x0))/r~^3 dS0
% 在 rho < a 内部应 = f0（绝对值正确），在 rho > a 应 = 0（阶跃边缘）。
% 同时输出 2× 常数变体以判定文献常数引用差异。
%
% 运行：matlab -batch "run('exp1_ubp_sphere.m')"
% 输出：evidence/exp1_ubp_sphere.json

outdir = fullfile(fileparts(mfilename('fullpath')), '..', 'evidence');
if ~exist(outdir, 'dir'), mkdir(outdir); end

c = 1490.0;             % 声速 [m/s]
Rs = 6.57e-3;           % 检测球面半径 [m]（与生产环半径一致）
f0 = 1.0;               % 初始声压 [Pa]

cases = struct('name', {}, 'a', {}, 'rho', {}, 'frec', {}, 'frecHalf', {});
alist = [2.0e-3, 0.8e-3];
rholist = [0, 0.5, 1.0, 1.5, 2.0, 3.0, 4.0] * 1e-3;

% 时间网格（覆盖最大走时并留裕量）
tMax = 1.5 * 2 * Rs / c;
Nt = 2 ^ 15;
tgrid = linspace(0, tMax, Nt);
dt = tgrid(2) - tgrid(1);

for a = alist
    % 数据 p(t) = d/dt [ t * f0 * F(Rs, c t) ]（中心差分，网格足够细）
    Mfun = @(rho0, r) ballMeanFraction(rho0, r, a);
    tM = tgrid;
    Mt = f0 * tM .* Mfun(Rs, c * tM);          % t*M(rho0, ct)
    p = gradient(Mt, dt);                       % [Pa]
    pFun = @(tq) interp1(tgrid, p, tq, 'linear', 0);

    for rho = rholist
        frec = ubpSphereAt(rho, Rs, a, f0, c, pFun, 1.0);
        frecHalf = ubpSphereAt(rho, Rs, a, f0, c, pFun, 0.5);
        cases(end+1) = struct('name', sprintf('a=%.1fmm', a * 1e3), ...
            'a', a, 'rho', rho, 'frec', frec, 'frecHalf', frecHalf); %#ok<AGROW>
    end
end

fprintf('\n=== EXP1 3D 球面 UBP 常数判定（目标：球内 frec=f0=1，球外=0）===\n');
fprintf('%-10s %8s  %10s  %10s\n', 'ball', 'rho/mm', 'frec', 'frecHalf');
for k = 1:numel(cases)
    fprintf('%-10s %8.2f  %10.6f  %10.6f\n', cases(k).name, cases(k).rho * 1e3, ...
        cases(k).frec, cases(k).frecHalf);
end

% 量化判定：球内（rho<a）偏差
inErr1 = 0; inErr2 = 0;
for k = 1:numel(cases)
    if cases(k).rho < cases(k).a * 0.98
        inErr1 = max(inErr1, abs(cases(k).frec - 1));
        inErr2 = max(inErr2, abs(cases(k).frecHalf - 1));
    end
end
fprintf('球内最大偏差：C=1/Omega0 变体 %.3g；C=1/(2*Omega0) 变体 %.3g\n', inErr1, inErr2);

% 收敛性检验：球外 rho=4mm 伪影应随求积分辨率下降（证明为数值伪影而非公式错误）
a = 2.0e-3;
Mfun = @(rho0, r) ballMeanFraction(rho0, r, a);
convRes = struct('nTheta', {}, 'Nt', {}, 'frec', {});
for cfg = [struct('nTheta', 4096, 'Nt', 2^15), struct('nTheta', 16384, 'Nt', 2^17), ...
           struct('nTheta', 65536, 'Nt', 2^19)]
    tg = linspace(0, tMax, cfg.Nt);
    Mt = f0 * tg .* Mfun(Rs, c * tg);
    pC = gradient(Mt, tg(2) - tg(1));
    pFunC = @(tq) interp1(tg, pC, tq, 'linear', 0);
    fr = ubpSphereAtN(4.0e-3, Rs, c, pFunC, 1.0, cfg.nTheta);
    convRes(end+1) = struct('nTheta', cfg.nTheta, 'Nt', cfg.Nt, 'frec', fr); %#ok<AGROW>
end
fprintf('收敛性（a=2mm, rho=4mm）：\n');
for k = 1:numel(convRes)
    fprintf('  nTheta=%6d Nt=%6d -> frec=%.5f\n', convRes(k).nTheta, convRes(k).Nt, convRes(k).frec);
end

out = struct();
out.description = 'Validation of Xu-Wang PRE2005 UBP constants on compact uniform ball';
out.c = c; out.Rs = Rs; out.f0 = f0;
out.cases = cases;
out.interiorMaxErr_C1 = inErr1;
out.interiorMaxErr_Chalf = inErr2;
out.convergence = convRes;
out.matlabVersion = version;
fid = fopen(fullfile(outdir, 'exp1_ubp_sphere.json'), 'w');
fwrite(fid, jsonencode(out, 'PrettyPrint', true)); fclose(fid);
fprintf('EXP1_DONE -> %s\n', fullfile(outdir, 'exp1_ubp_sphere.json'));

% ------------------------------------------------------------------
function f = ubpSphereAt(rho, Rs, a, f0, c, pFun, constScale) %#ok<INUSD>
f = ubpSphereAtN(rho, Rs, c, pFun, constScale, 4096);
end

function f = ubpSphereAtN(rho, Rs, c, pFun, constScale, nTheta)
% 在距球心 rho 处评估 UBP（b = 2p − 2t~ p'，dΩ = dS0 (n0·(x−x0))/r~^3）
% constScale=1 → C=1/Omega0；0.5 → C=1/(2*Omega0)。
if rho >= Rs
    f = NaN;
    return;
end
th = linspace(0, pi, nTheta);           % 极角（相对 x 方向）
dS0 = Rs^2 .* sin(th);                  % |dS0/dtheta| per dphi
rtilde = sqrt(Rs^2 + rho^2 - 2 * Rs * rho * cos(th));
cosN = (Rs - rho * cos(th)) ./ rtilde;  % n0(指向源)·(x−x0)/r~ = 正
tq = rtilde / c;                        % [s]
% 数值导数 dp/dt~ = (1/c) dp/dt：对 p(t) 网格直接用差分句柄
% b = 2p − 2 t~ dp/dt~；t~ = r~（米）。
pd = gradFun(pFun, tq, c);
b = 2 * pFun(tq) - 2 * rtilde .* pd;
integrand = b .* cosN ./ rtilde.^2 .* dS0;   % dS0*(n0·(x−x0))/r~^3 = cosN/r~^2*dS0
I = trapz(th, integrand) * 2 * pi;      % dphi 积分
f = constScale * I / (4 * pi);
end

function dpdt = gradFun(pFun, tq, c)
% dp/dt~ = (1/c) * dp/dt，中心差分（步长取时间网格量级）
h = 1e-10;
dpdt = (pFun(tq + h) - pFun(tq - h)) / (2 * h) / c;
end

function F = ballMeanFraction(rho0, r, a)
% 均匀球体（半径 a）对中心距 rho0、半径 r 的球面所张的面积分数
if rho0 == 0
    F = double(r <= a);
    return;
end
cosPsi = (rho0.^2 + r.^2 - a^2) ./ (2 * rho0 * r);
F = (1 - cosPsi) / 2;
F = min(max(F, 0), 1);
end
