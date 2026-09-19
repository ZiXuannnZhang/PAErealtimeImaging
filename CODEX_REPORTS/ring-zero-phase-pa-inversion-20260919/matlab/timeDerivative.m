function dF = timeDerivative(F, dt)
% TIMEDERIVATIVE 沿时间维（dim 1，行方向）的时间导数，逐列独立（审查整改 R1）。
%
% 背景：9493962 版 exp2 使用 gradient(matrix, dt, 1)。MATLAB R2023a 中该调用
% 的第三参数被解释为 dim 2（列/A-line 方向）的间距，且单输出返回的是
% dim 2 方向梯度——对各列相同的输入返回全零，对偏心目标返回跨 A-line 差分，
% 并非时间导数。本函数是整改后唯一的矩阵时间导数入口。
%
% dF = timeDerivative(F, dt)
%   F  : Nt×nLines 矩阵（时间样本 × 线）或 Nt×1 向量；
%   dt : 采样间隔 [s]，正有限标量；
%   dF : 与 F 同尺寸，dF(i,:) = dF/dt 在 t_i 处的值 [F单位/秒]。
% 离散格式（与 MATLAB 对向量调用 gradient(v, dt) 语义一致，逐点差异 ≤ eps 量级）：
%   内部   dF(i) = (F(i+1) - F(i-1)) / (2·dt)   （中心差分）
%   端点   dF(1) = (F(2) - F(1)) / dt；dF(end) = (F(end) - F(end-1)) / dt（单侧）
% 每列独立计算，列间无耦合；不修改输入。
% 测试：test_time_derivative.m（时间线性→常数导数、时间恒定→零、解析导数
%   容差、向量/矩阵路径一致、端点约定、导出参考向量不受矩阵误用影响）。

if nargin < 2
    error('timeDerivative:missingDt', '必须显式提供采样间隔 dt [s]');
end
if ~isscalar(dt) || ~isfinite(dt) || dt <= 0
    error('timeDerivative:badDt', 'dt 必须为正有限标量，收到 %s', mat2str(dt));
end
F = double(F);
if size(F, 1) < 2
    error('timeDerivative:tooShort', '时间维（行）长度必须 ≥ 2，收到 %d', size(F, 1));
end
dF = zeros(size(F), 'like', F);
if size(F, 1) == 2
    % 长度 2：两端同为单侧差分（与 gradient 对长度 2 向量的约定一致）
    dF(1, :) = (F(2, :) - F(1, :)) / dt;
    dF(2, :) = dF(1, :);
    return;
end
dF(1, :) = (F(2, :) - F(1, :)) / dt;
dF(2:end-1, :) = (F(3:end, :) - F(1:end-2, :)) / (2 * dt);
dF(end, :) = (F(end, :) - F(end-1, :)) / dt;
end
