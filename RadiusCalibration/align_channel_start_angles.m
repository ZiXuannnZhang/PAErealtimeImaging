function shift = align_channel_start_angles(refBscan, candBscan)
%ALIGN_CHANNEL_START_ANGLES 估计两通道全孔径 Bscan 的整数列向循环位移
%
% 输入:
%   refBscan  : [Nt x N] 基准通道 Bscan（一列 = 一根 A-line）
%   candBscan : [Nt x N] 待对齐通道 Bscan（尺寸必须与 refBscan 相同）
%
% 输出:
%   shift : 整数循环位移（0..N-1），满足：
%           circshift(candBscan, shift, 2) 与 refBscan 的列向相关性最大。
%
% 说明:
%   - 只估计位移，不对任何输入数据做数值修改；
%   - 为提高速度，先把二维 Bscan 沿行方向压缩为列能量签名，
%     再对签名做一维循环互相关；
%   - 本函数只处理整数位移，亚列精度由后续重建角度向量按需处理。

if ~isequal(size(refBscan), size(candBscan))
    error('align_channel_start_angles:Size', ...
        '两通道 Bscan 尺寸必须一致。');
end
N = size(refBscan, 2);
if N < 2
    error('align_channel_start_angles:Size', 'Bscan 至少需要 2 列。');
end

% 列能量签名（去均值，消除通道间整体幅度差异）
sigRef  = sum(double(refBscan) .^ 2, 1);
sigCand = sum(double(candBscan) .^ 2, 1);
sigRef  = sigRef - mean(sigRef);
sigCand = sigCand - mean(sigCand);

% 循环互相关：cc(i) 对应滞后 i-1
cc = real(ifft(fft(sigRef) .* conj(fft(sigCand))));
[~, idx] = max(cc);

% 由滞后换算为使 candBscan 匹配 refBscan 所需的右循环位移
shift = mod(-(idx - 1), N);
end
