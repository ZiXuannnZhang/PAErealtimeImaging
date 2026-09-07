function c = norm_corr2(a, b)
%NORM_CORR2 零位移归一化相关系数（自实现，不依赖工具箱）
%
% 输入:
%   a, b : 同尺寸矩阵
% 输出:
%   c : 归一化相关系数 [-1, 1]
%
% 公式:
%   c = <a-mean(a), b-mean(b)> / (||a-mean(a)|| * ||b-mean(b)||)

if ~isequal(size(a), size(b))
    error('norm_corr2:Size', '两矩阵尺寸必须一致。');
end

a = double(a(:));
b = double(b(:));
a = a - mean(a);
b = b - mean(b);

den = sqrt(dot(a, a) * dot(b, b));
if den < eps
    c = 0;
else
    c = dot(a, b) / den;
end
end
