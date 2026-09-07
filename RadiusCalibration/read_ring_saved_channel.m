function data = read_ring_saved_channel(dataDir, channelId, fileSuffix, fileCount, triggersPerFile, sampleCount)
%READ_RING_SAVED_CHANNEL 读取监听程序保存的单通道连续数据文件
%
% 输入:
%   dataDir         : 保存目录（如 ...\testdata\01）
%   channelId       : 通道代号 1..8（与实时程序一致）
%   fileSuffix      : 文件名中的自定义字符串 z
%   fileCount       : 读取文件数量（序号 000 到 fileCount-1，按顺序拼接）
%   triggersPerFile : 单个文件包含的触发数（当前为 1000）
%   sampleCount     : 单触发采样点数（当前为 4000）
%
% 输出:
%   data : [sampleCount x fileCount*triggersPerFile] single 矩阵，
%          列顺序 = 文件序号顺序、文件内触发顺序（与采集顺序一致）
%
% 命名规则（对应 FileSaver::generateFileName）:
%   Card{N}_Ch{A|B}_{z}_{seq:03d}.dat
%   N = 采集卡编号（1..4），每卡两个通道 A/B；
%   物理通道代号 = (N-1)*2 + (A=0, B=1)，UI 显示为通道1..8。
%   文件内容 = 无文件头、float16、按触发顺序追加的时域信号。

if nargin < 6
    error('read_ring_saved_channel:Args', '输入参数不足。');
end
if channelId < 1 || channelId > 8
    error('read_ring_saved_channel:Channel', '通道代号必须为 1..8，当前为 %d。', channelId);
end

cardId = ceil(channelId / 2);
chLetter = 'A';
if mod(channelId, 2) == 0
    chLetter = 'B';
end

expectedPerFile = double(triggersPerFile) * double(sampleCount);
data = zeros(sampleCount, fileCount * triggersPerFile, 'single');

for k = 0:(fileCount - 1)
    fileName = sprintf('Card%d_Ch%s_%s_%03d.dat', ...
        cardId, chLetter, fileSuffix, k);
    filePath = fullfile(dataDir, fileName);
    if ~exist(filePath, 'file')
        error('read_ring_saved_channel:FileNotFound', ...
            '找不到文件：%s', filePath);
    end

    fid = fopen(filePath, 'r', 'ieee-le');
    if fid < 0
        error('read_ring_saved_channel:Open', '无法打开文件：%s', filePath);
    end
    u = fread(fid, Inf, 'uint16=>uint16');
    fclose(fid);

    if numel(u) ~= expectedPerFile
        error('read_ring_saved_channel:Size', ...
            '文件 %s 元素数 %d 与预期 %d 不一致（triggersPerFile=%d, sampleCount=%d）。', ...
            fileName, numel(u), expectedPerFile, triggersPerFile, sampleCount);
    end

    % float16 -> single，再按列主序恢复为 [sampleCount x triggersPerFile]
    block = half_to_single(u);
    block = reshape(block, [sampleCount, triggersPerFile]);

    % 严格按文件序号顺序写入预分配缓冲区，不改变采集顺序
    cols = (k * triggersPerFile + 1) : ((k + 1) * triggersPerFile);
    data(:, cols) = block;
end
end
