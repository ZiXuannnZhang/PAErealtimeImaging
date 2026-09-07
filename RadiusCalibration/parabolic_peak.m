function xp = parabolic_peak(x, y)
%PARABOLIC_PEAK 三点抛物线顶点拟合
%
% 输入:
%   x : [1x3] 采样位置
%   y : [1x3] 对应的度量值
% 输出:
%   xp: 抛物线顶点横坐标

if numel(x) ~= 3 || numel(y) ~= 3
    error('parabolic_peak:Size', 'x 与 y 必须各包含 3 个点。');
end
x = double(x(:));
y = double(y(:));

den = (x(1) - x(2)) * (x(1) - x(3)) * (x(2) - x(3));
if abs(den) < eps
    xp = x(2);
    return;
end
a = (x(3) * (y(2) - y(1)) + x(2) * (y(1) - y(3)) + x(1) * (y(3) - y(2))) / den;
b = (x(1)^2 * (y(2) - y(3)) + x(3)^2 * (y(1) - y(2)) + x(2)^2 * (y(3) - y(1))) / den;
if abs(a) < eps
    xp = x(2);
else
    xp = -b / (2 * a);
end
end
