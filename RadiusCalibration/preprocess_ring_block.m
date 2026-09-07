function bscan = preprocess_ring_block(bscan, p)
%PREPROCESS_RING_BLOCK 预处理（移植自 Ringscan_DAS_loop_realtime_dual / preprocessBlock.m）
%
% 输入:
%   bscan : [SampDepth x nBlock] 原始 Bscan（一列 = 一根 A-line）
%   p     : 参数结构体，字段与参考脚本一致：
%           trigdejit / phase_Decon / Gaussfil / gaussfil_rowstart /
%           gaussfil_rowsend / filter_high / w_high / n2 / med /
%           arc_remove / DBR_sig_remove / mask_length /
%           singal_impair / im_value / DelayCut / system_delay /
%           DAQ / sysDelay / wl
%
% 输出:
%   bscan : 预处理后的 Bscan（DelayCut 后行数减少）
%
% 说明:
%   - 步骤顺序与 Handoff/preprocessBlock.m 完全一致；
%   - Gaussfil=1 时调用外部函数 Freqfilter4FiberSignal，调用方式与
%     Ringscan_DAS_loop_realtime_dual 相同（调用前需把外部函数目录加入路径）；
%   - trigdejit/phase_Decon/arc_remove 在参考脚本中为占位，保留开关与注释。

if p.trigdejit
    % 触发去抖需要跨块参考列（refCol/Corrows），实时模式下暂关（占位）
end

if p.phase_Decon == 1
    % 相位去卷积按列处理，需块内满足长度约束（占位）
end

if p.Gaussfil == 1
    % 二维光纤信号去除：按区域处理
    gaussfil_pre = bscan(p.gaussfil_rowstart:p.gaussfil_rowsend, :);
    gaussfil_after = Freqfilter4FiberSignal(gaussfil_pre, p.DAQ, 1e6, 0);
    bscan(p.gaussfil_rowstart:p.gaussfil_rowsend, :) = gaussfil_after;
end

if p.filter_low == 1
    % 高通滤波
    Wn = 2 * p.w_low / p.DAQ;
    [B, A] = butter(p.n1, Wn, 'high');
    bscan(301:end, :) = filtfilt(B, A, bscan(301:end, :));
end

if p.filter_high == 1
    % 低通滤波
    Wn = 2 * p.w_high / p.DAQ;
    [B, A] = butter(p.n2, Wn, 'low');
    bscan(p.system_delay + 20:end, :) = filtfilt(B, A, bscan(p.system_delay + 20:end, :));
end

if p.med == 1
    % 中值滤波（按 Bscan 整体近似，与参考脚本一致）
    bscan = medfilt2(bscan, [3, 3]);
end

if p.arc_remove == 1
    % 弧线掩膜依赖整帧几何，暂关（占位）
end

if p.DBR_sig_remove == 1
    % 扣除 DBR 光纤振荡强信号
    bscan(1:p.mask_length + (p.wl == 2) * (p.sysDelay(2) - p.sysDelay(1)), :) = 0;
end

if p.singal_impair == 1
    % 全图信号阈值削顶
    bscan(bscan >  p.im_value) =  p.im_value;
    bscan(bscan < -p.im_value) = -p.im_value;
end

if p.DelayCut == 1
    % 延时截断
    bscan = bscan(p.system_delay:end, :);
end
end
