function [data, evidence] = read_ring_saved_channel(dataDir, channelId, fileSuffix, fileCount, triggersPerFile, sampleCount, fileIndices, metadataOnly)
%READ_RING_SAVED_CHANNEL 读取监听程序保存的单通道连续数据文件
%
% 输入:
%   dataDir         : 保存目录（如 ...\testdata\01）
%   channelId       : 通道代号 1..8（与实时程序一致）
%   fileSuffix      : 文件名中的自定义字符串 z
%   fileCount       : 读取文件数量（序号 000 到 fileCount-1，按顺序拼接）
%   triggersPerFile : 单个文件包含的触发数（当前为 1000）
%   sampleCount     : 单触发采样点数（当前为 4000）
%   metadataOnly    : 核验文件对及旁车，跳过分配和读取波形（可选）。
%
% 输出:
%   data : [sampleCount x fileCount*triggersPerFile] single 矩阵，
%          列顺序 = 文件序号顺序、文件内触发顺序。
%   evidence : 旁车覆盖、逐列身份和采样间隔。没有旁车的旧文件按原定长规则读取。
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

validateattributes(fileCount, {'numeric'}, {'scalar', 'integer', 'positive'});
validateattributes(triggersPerFile, {'numeric'}, {'scalar', 'integer', 'positive'});
validateattributes(sampleCount, {'numeric'}, {'scalar', 'integer', 'positive'});
if nargin < 7
    fileIndices = 0:(fileCount - 1);
end
validateattributes(fileIndices, {'numeric'}, {'vector', 'integer', 'nonnegative', 'numel', fileCount});
cardId = ceil(channelId / 2);
isLaneB = mod(channelId, 2) == 0;
suffix = char(fileSuffix);
if ~isempty(suffix)
    suffix = ['_' suffix];
end

if nargin < 8, metadataOnly = false; end
data = [];
if ~metadataOnly, data = zeros(sampleCount, fileCount * triggersPerFile, 'single'); end
columns = cell(fileCount, 1);
hasSidecar = false(1, fileCount);
sampleInterval = NaN(1, fileCount);
offset = 0;

inputFiles = strings(fileCount, 2);
for k = 0:(fileCount - 1)
    fileIndex = fileIndices(k+1);
    fileA = sprintf('Card%d_ChA%s_%03d.dat', cardId, suffix, fileIndex);
    fileB = sprintf('Card%d_ChB%s_%03d.dat', cardId, suffix, fileIndex);
    names = {fileA, fileB};
    fileName = names{1 + isLaneB};
    filePath = fullfile(dataDir, fileName);
    if isempty(suffix) && ~isfile(filePath)
        % Older callers also used two underscores for an empty suffix.
        fileA = sprintf('Card%d_ChA__%03d.dat', cardId, fileIndex);
        fileB = sprintf('Card%d_ChB__%03d.dat', cardId, fileIndex);
        names = {fileA, fileB};
        fileName = names{1 + isLaneB};
        filePath = fullfile(dataDir, fileName);
    end
    if ~isfile(filePath)
        error('read_ring_saved_channel:FileNotFound', '找不到文件：%s', filePath);
    end
    inputFiles(k+1, :) = [string(fileA), string(fileB)];
    sidecar = fullfile(dataDir, [fileA '.jsonl']);
    nColumns = triggersPerFile;
    if isfile(sidecar)
        [columns{k+1}, sampleInterval(k+1)] = readSidecar( ...
            sidecar, dataDir, fileA, fileB, cardId, sampleCount);
        nColumns = height(columns{k+1});
        if nColumns > triggersPerFile
            error('read_ring_saved_channel:Size', '旁车列数超过每文件触发上限：%s', sidecar);
        end
        hasSidecar(k+1) = true;
    end

    if metadataOnly, continue; end

    fid = fopen(filePath, 'r', 'ieee-le');
    if fid < 0
        error('read_ring_saved_channel:Open', '无法打开文件：%s', filePath);
    end
    u = fread(fid, Inf, 'uint16=>uint16');
    fclose(fid);

    expectedPerFile = double(nColumns) * double(sampleCount);
    if numel(u) ~= expectedPerFile
        error('read_ring_saved_channel:Size', ...
            '文件 %s 元素数 %d 与预期 %d 不一致（triggersPerFile=%d, sampleCount=%d）。', ...
            fileName, numel(u), expectedPerFile, nColumns, sampleCount);
    end

    % float16 -> single，再按列主序恢复为 [sampleCount x triggersPerFile]
    block = half_to_single(u);
    block = reshape(block, [sampleCount, nColumns]);
    data(:, offset + (1:nColumns)) = block;
    offset = offset + nColumns;
end
data(:, offset+1:end) = [];

if any(hasSidecar) && ~all(hasSidecar)
    error('read_ring_saved_channel:UnverifiedMetadata', '文件中混有缺少旁车的数据。');
end
evidence = struct('hasSidecar', all(hasSidecar), 'columns', table(), ...
    'sampleIntervalNs', NaN, 'inputFiles', inputFiles);
if all(hasSidecar)
    if any(sampleInterval ~= sampleInterval(1))
        error('read_ring_saved_channel:UnverifiedMetadata', '所选文件的采样间隔不一致。');
    end
    evidence.columns = vertcat(columns{:});
    evidence.sampleIntervalNs = sampleInterval(1);
end
end

function [columns, sampleInterval] = readSidecar(path, dataDir, fileA, fileB, card, sampleCount)
try
    lines = splitlines(string(fileread(path)));
    lines(strlength(strtrim(lines)) == 0) = [];
    layout = jsondecode(char(lines(1)));
    ending = jsondecode(char(lines(end)));
    count = numel(lines) - 2;
    if count < 1 || ~strcmp(layout.type, 'pae-raw-pair') || ~ismember(layout.version, [1 2]) || ...
            layout.card ~= card || layout.sampleCount ~= sampleCount || ...
            ~strcmp(layout.fileA, fileA) || ~strcmp(layout.fileB, fileB) || ...
            ~strcmp(layout.packetAnchor, 'first-arrival') || ...
            ~strcmp(layout.physicalPhase, 'unverified') || ...
            ~strcmp(ending.type, 'end') || ending.confirmedColumns ~= count
        error('旁车布局、文件名或封尾列数不匹配。');
    end
    captureId = "";
    if layout.version == 2
        captureId = lower(string(layout.captureId));
        if ~isscalar(captureId) || isempty(regexp(char(captureId), ...
                '^[0-9a-f]{8}(-[0-9a-f]{4}){3}-[0-9a-f]{12}$', 'once'))
            error('旁车缺少有效的采集 ID。');
        end
    end
    sampleInterval = layout.sampleIntervalNs;
    if ~isscalar(sampleInterval) || ~isfinite(sampleInterval) || sampleInterval <= 0
        error('缺少有效采样间隔。');
    end
    for name = {fileA, fileB}
        lane = dir(fullfile(dataDir, name{1}));
        if ~isscalar(lane) || lane.isdir || lane.bytes ~= 2 * sampleCount * count
            error('双通道文件长度与确认列数不匹配。');
        end
    end
    decoded = arrayfun(@(line) jsondecode(char(line)), lines(2:end-1), 'UniformOutput', false);
    rows = vertcat(decoded{:});
    if any(~strcmp({rows.type}, 'column')) || ~isequal([rows.column], 1:count)
        error('旁车列记录不连续。');
    end
    if any(cellfun(@isempty, regexp({rows.session}, '^[0-9]+$', 'once'))) || ...
            any(cellfun(@isempty, regexp({rows.round}, '^[0-9]+$', 'once'))) || ...
            any([rows.triggerSeq] < 0 | [rows.triggerSeq] > 65535 | mod([rows.triggerSeq], 1) ~= 0)
        error('旁车触发身份无效。');
    end
    if layout.version == 2
        [declared, fileSession, fileRound] = ring_raw_pair_round(layout);
        if declared && (fileRound == "" || any(string({rows.session}) ~= fileSession) || ...
                any(string({rows.round}) ~= fileRound))
            error('文件轮次声明与逐列身份不一致。');
        end
    end
    complete = [rows.complete] & [rows.expectedSlots] > 0 & ...
        [rows.receivedSlots] == [rows.expectedSlots] & ~[rows.late] & [rows.logicalIndex] >= 0;
    if layout.version == 1
        complete = complete & ~[rows.tail];
    end
    columns = table(repmat(captureId, count, 1), string({rows.session}).', ...
        string({rows.round}).', [rows.logicalIndex].', [rows.triggerSeq].', complete.', ...
        repmat(string(fileA), count, 1), repmat(string(fileB), count, 1), ...
        'VariableNames', {'captureId', 'session', 'round', 'index', 'trigger', 'complete', 'fileA', 'fileB'});
catch exception
    error('read_ring_saved_channel:UnverifiedMetadata', ...
        '无法验证原始波形旁车 %s：%s', path, exception.message);
end
end
