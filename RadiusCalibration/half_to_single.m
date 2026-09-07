function f = half_to_single(h)
%HALF_TO_SINGLE IEEE 754 半精度（float16）转单精度（float32）
%
% 输入:
%   h : uint16 标量/向量/矩阵，每一位为一个半精度浮点值
% 输出:
%   f : 与 h 同尺寸的 single 矩阵
%
% 说明:
%   监听程序 FileSaver 将 float32 时域信号转换为 float16 后写入 .dat 文件，
%   本函数是 FileSaver::float32ToFloat16 的逆操作（不含舍入，逐位还原）。

h = uint16(h);
f = zeros(size(h), 'single');

sign = bitget(h, 16) ~= 0;
exp  = double(bitand(bitshift(h, -10), uint16(31)));
frac = double(bitand(h, uint16(1023)));

% 次正规数（指数 0、尾数非 0）
sub = (exp == 0) & (frac ~= 0);
f(sub) = single(frac(sub) * 2^-24);

% 正规数
nor = (exp > 0) & (exp < 31);
f(nor) = single((1 + frac(nor) / 1024) .* 2.^(exp(nor) - 15));

% 无穷与 NaN
infMask = (exp == 31) & (frac == 0);
nanMask = (exp == 31) & (frac ~= 0);
f(infMask) = single(inf);
f(nanMask) = single(nan);

% 符号位
f(sign) = -f(sign);
end
