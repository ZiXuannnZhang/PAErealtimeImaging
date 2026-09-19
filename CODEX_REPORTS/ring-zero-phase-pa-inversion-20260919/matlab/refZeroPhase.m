function y = refZeroPhase(x, sos, g, n)
% REFZEROPHASE 参考零相位滤波（阶段 A 冻结约定，阶段 B C++ 实现的对照算法）
%
% 约定：
%   - double SOS 级联（butter→zp2sos），n = 单程 Butterworth 阶数
%   - 奇对称延拓：两端各 nfact = 3n 样本，ext = 2*x(1) − x(nfact+1:-1:2)（起端）
%   - 逐节稳态初始化：DF2T 每节对常输入的稳态（首节输入 x(1)，级联传递）
%   - 前向 → 时间翻转 → 前向（同系数）→ 翻转
%   - 输入有限、线长 > nfact，否则报错（不静默单向滤波）
y = double(x(:));
Nx = numel(y);
nfact = 3 * n;
if Nx <= nfact
    error('refZeroPhase:shortWindow', ...
        '线长 %d 必须大于延拓长度 nfact=%d（3×单程阶数）', Nx, nfact);
end
if any(~isfinite(y))
    error('refZeroPhase:nonFinite', '输入含 NaN/Inf');
end
ext = [2 * y(1) - y(nfact+1:-1:2); y; 2 * y(end) - y(end-1:-1:end-nfact)];
ziC = sosZiSteady(sos, ext(1));
for k = 1:size(sos, 1)
    ext = filter(sos(k, 1:3), sos(k, 4:6), ext, ziC{k});
end
ziC = sosZiSteady(sos, ext(end));
ext = flipud(ext);
for k = 1:size(sos, 1)
    ext = filter(sos(k, 1:3), sos(k, 4:6), ext, ziC{k});
end
ext = flipud(ext);
y = g^2 * ext(nfact+1 : nfact+Nx);   % 零相位复合增益 = g^2（前后向各一次）
y = y(:);
end

function ziC = sosZiSteady(sos, x0)
% 每节对常输入的稳态初始状态（DF2T）：y=H(1)x0；s1=y−b0·x0；s2=y(1+a2)−x0(b0+b1)
% 首节输入 x0，之后每节输入为上一节的稳态输出（级联稳态）。
ziC = cell(size(sos, 1), 1);
xin = x0;
for k = 1:size(sos, 1)
    b = sos(k, 1:3);  a = sos(k, 4:6);
    yk = (sum(b) / sum(a)) * xin;
    s1 = yk - b(1) * xin;
    s2 = yk * (1 + a(2)) - xin * (b(1) + b(2));
    ziC{k} = [s1; s2];
    xin = yk;
end
end
