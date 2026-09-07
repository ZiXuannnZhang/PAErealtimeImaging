function bscan = read_ring_bscan(cfg)
%READ_RING_BSCAN 读取监听程序保存的环扫数据，返回每通道全孔径原始 Bscan
%
% 输入 cfg 字段：
%   dataDir              保存目录
%   channelSel           启用的通道代号数组（1..8 任意数量）
%   fileSuffix           保存文件名中的自定义字符串 z
%   filesPerChannel      单通道顺序读取的文件数量
%   triggersPerFile      单文件触发数（当前 1000）
%   sampleCount          单触发采样点数（当前 4000）
%   fullAlinesPerChannel 每通道每圈总 A-line 数量（含双波长，全孔径标定）
%   useAllRounds         true=多圈全部参与；false=只用第一圈
%
% 输出：
%   bscan : 结构体
%     wl1 : 1x8 元胞，wl1{ch} = [sampleCount x Nwl1] 通道 ch 的 532nm 原始 Bscan
%     wl2 : 1x8 元胞，wl2{ch} = [sampleCount x Nwl2] 通道 ch 的 1064nm 原始 Bscan
%
% 说明:
%   - 本函数只做数据读取与波长切分，不拼接、不对原始数据做任何数值修改；
%   - 各通道全孔径起始角度对齐由 align_channel_start_angles 估计整数列位移，
%     再通过重建角度向量完成虚拟对齐；
%   - 1064nm 不在此处做 circshift，其一列提前的角度差由重建角度向量处理。

if nargin < 1 || ~isstruct(cfg)
    error('read_ring_bscan:Args', '缺少 cfg 结构体输入。');
end

selectedCh = sort(unique(cfg.channelSel(:)'));
if isempty(selectedCh) || any(selectedCh < 1 | selectedCh > 8)
    error('read_ring_bscan:Channels', ...
        'cfg.channelSel 必须是 1..8 内的通道代号数组。');
end

savedData = cell(1, 8);
trigCount = zeros(1, 8);
for ch = selectedCh
    savedData{ch} = read_ring_saved_channel(cfg.dataDir, ch, ...
        cfg.fileSuffix, cfg.filesPerChannel, ...
        cfg.triggersPerFile, cfg.sampleCount);
    trigCount(ch) = size(savedData{ch}, 2);
end

if numel(unique(trigCount(selectedCh))) ~= 1
    error('read_ring_bscan:TriggerCount', ...
        '所选通道触发数不一致：%s', mat2str(trigCount(selectedCh)));
end
totalTrig = trigCount(selectedCh(1));

% 全孔径标定：每个通道独立扫描完整一圈
triggersPerCircle = cfg.fullAlinesPerChannel;
if rem(triggersPerCircle, 2)
    error('read_ring_bscan:FullAlines', ...
        'cfg.fullAlinesPerChannel 必须为偶数（双波长），当前为 %d。', ...
        triggersPerCircle);
end

useTrig = totalTrig;
if ~cfg.useAllRounds
    useTrig = min(totalTrig, triggersPerCircle);
end
if totalTrig < triggersPerCircle
    warning('read_ring_bscan:TriggerShortage', ...
        '每通道触发数 %d 少于一圈所需 %d，将使用全部触发。', ...
        totalTrig, triggersPerCircle);
end
if rem(useTrig, 2)
    error('read_ring_bscan:TriggerParity', ...
        '参与重建的触发数必须为偶数（双波长交替），当前为 %d。', useTrig);
end

wl1 = cell(1, 8);
wl2 = cell(1, 8);
for ch = selectedCh
    mat = savedData{ch}(:, 1:useTrig);

    % 触发奇偶即波长：第1、3、5...列=532nm，第2、4、6...列=1064nm
    wl1{ch} = mat(:, 1:2:end);
    wl2{ch} = mat(:, 2:2:end);
end
clear savedData mat;

bscan.wl1 = wl1;
bscan.wl2 = wl2;
end
