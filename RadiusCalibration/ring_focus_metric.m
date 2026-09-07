function m = ring_focus_metric(img, mask, metricType)
%RING_FOCUS_METRIC 半径标定使用的聚焦度量
%
% 输入:
%   img        : [ny x nx] 归一化 DAS 图像（single/double）
%   mask       : 逻辑掩码 [ny x nx]，仅在掩码内计算（[] = 全图）
%   metricType : 'tenengrad'       梯度能量
%                'tenengrad_norm'  归一化梯度能量（对整体亮度不敏感，默认推荐）
%                'brenner'         Brenner 梯度度量
%
% 输出:
%   m : 标量聚焦度量（越大越清晰）

if nargin < 3 || isempty(metricType)
    metricType = 'tenengrad_norm';
end

img = double(img);
if nargin < 2 || isempty(mask)
    mask = true(size(img));
else
    mask = logical(mask);
end
if ~isequal(size(mask), size(img))
    error('ring_focus_metric:Size', '掩码尺寸必须与图像一致。');
end

if strcmpi(metricType, 'brenner')
    % Brenner：Σ( I(x+2) - I(x) )^2，仅水平方向
    d = img(1:end-2, :) - img(3:end, :);
    msk = mask(1:end-2, :);
    g2 = d .* d;
    m = sum(g2(msk));
    denom = max(sum(img(mask) .^ 2), eps);
    m = m / denom;
    return;
end

[gx, gy] = gradient(img);
g2 = gx .* gx + gy .* gy;
m = sum(g2(mask));

if strcmpi(metricType, 'tenengrad_norm')
    denom = max(sum(img(mask) .^ 2), eps);
    m = m / denom;
elseif ~strcmpi(metricType, 'tenengrad')
    warning('ring_focus_metric:Type', ...
        '未知度量类型 "%s"，改用 tenengrad。', metricType);
end
end
