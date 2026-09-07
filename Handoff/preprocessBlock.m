function bscan = preprocessBlock(bscan, p)
% PREPROCESSBLOCK 逐块预处理（匹配实际逐块采集）
%
% 输入:
%   bscan : [SampDepth x nBlock] 本块原始数据（列向步骤逐块独立处理）
%   p     : 参数结构体，字段与主脚本一致：
%           trigdejit / phase_Decon / Gaussfil/gaussfil_rowstart/gaussfil_rowsend /
%           filter_high/w_high/n2 / med / arc_remove /
%           DBR_sig_remove/mask_length / singal_impair/im_value /
%           DelayCut/system_delay / DAQ / sysDelay / wl
%
% 输出:
%   bscan : 预处理后的块（延时截断后行数减少）
%
% 说明:
%   - 列向/逐元素步骤（滤波、DBR、阈值、延时截断）可直接逐块处理；
%   - 2D 或跨块步骤（触发去抖、中值、去光纤、去弧线）逐块处理会产生边界
%     差异或依赖整帧信息，实时模式下默认关闭，开启需先做滑动窗口适配；
%   - 中值滤波（med=1）按块近似应用，块边界与整帧结果略有差异。

if p.trigdejit
    % 触发去抖需要跨块参考列（refCol/Corrows），实时模式需滑动窗口适配（暂关）
end

if p.phase_Decon == 1
    % 相位去卷积按列处理，需块内满足长度约束（暂关）
end

if p.Gaussfil == 1
    % 2D 光纤信号去除：按块区域处理，块列边界与整帧略有差异
    gaussfil_pre = bscan(p.gaussfil_rowstart:p.gaussfil_rowsend, :);
    gaussfil_after = Freqfilter4FiberSignal(gaussfil_pre, p.DAQ, 1e6, 0);   % 本函数版本为 4 参
    bscan(p.gaussfil_rowstart:p.gaussfil_rowsend, :) = gaussfil_after;
end

if p.filter_low == 1
    Wn = 2*p.w_low/p.DAQ;
    [B, A] = butter(p.n1, Wn, 'high');
    bscan(301:end, :) = filtfilt(B, A, bscan(301:end, :));
end

if p.filter_high == 1
    Wn = 2*p.w_high/p.DAQ;
    [B, A] = butter(p.n2, Wn, 'low');
    bscan(p.system_delay+20:end, :) = filtfilt(B, A, bscan(p.system_delay+20:end, :));
end

if p.med == 1
    bscan = medfilt2(bscan, [3, 3]);   % 块级近似，边界与整帧略有差异
end

if p.arc_remove == 1
    % 弧线掩膜依赖整帧几何，逐块无法直接应用（暂关）
end

if p.DBR_sig_remove == 1
    bscan(1:p.mask_length+(p.wl==2)*(p.sysDelay(2)-p.sysDelay(1)), :) = 0;
end

if p.singal_impair == 1
    bscan(bscan >  p.im_value) =  p.im_value;
    bscan(bscan < -p.im_value) = -p.im_value;
end

if p.DelayCut == 1
    bscan = bscan(p.system_delay:end, :);
end
end
