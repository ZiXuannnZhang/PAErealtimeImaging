function bscan = read_ring_bscan(cfg)
%READ_RING_BSCAN 以 V2 旁车核对各张卡共有的采集轮次，并按奇偶逻辑槽取样。
% cfg.captureId 可选，目录里有多个采集时必须指定。cfg.roundId 可选。
% 历史读法要求显式设置 allowUnverifiedLegacy 与 legacyFileIndices。

selectedCh = sort(unique(cfg.channelSel(:)'));
if isempty(selectedCh) || any(selectedCh < 1 | selectedCh > 8 | mod(selectedCh, 1) ~= 0)
    error('read_ring_bscan:Channels', 'cfg.channelSel 必须是 1..8 内的通道代号数组。');
end
n = cfg.fullAlinesPerChannel;
validateattributes(n, {'numeric'}, {'scalar', 'integer', '>=', 2});
if rem(n, 2)
    error('read_ring_bscan:FullAlines', '每圈触发数必须为偶数。');
end
validateattributes(cfg.filesPerChannel, {'numeric'}, {'scalar', 'integer', 'positive'});

[captureId, indices] = selectFiles(cfg, selectedCh);
data = cell(1, 8);
provenance = cell(1, 8);
for ch = selectedCh
    files = indices{ceil(ch / 2)};
    [data{ch}, provenance{ch}] = read_ring_saved_channel(cfg.dataDir, ch, cfg.fileSuffix, ...
        numel(files), cfg.triggersPerFile, cfg.sampleCount, files);
end

if strlength(captureId) > 0
    [columns, rounds, inputFiles] = verifiedRounds(cfg, selectedCh, provenance, captureId, n);
    evidence = struct('source', 'paired-sidecar-v2', 'softwareRounds', numel(rounds), ...
        'captureId', captureId, 'rounds', rounds, 'files', {inputFiles});
else
    if cfg.useAllRounds
        error('read_ring_bscan:UnverifiedCoverage', '历史资料只允许显式选择单圈。');
    end
    counts = cellfun(@(samples) size(samples, 2), data(selectedCh));
    if numel(unique(counts)) ~= 1
        error('read_ring_bscan:TriggerCount', '所选历史通道触发数不一致。');
    end
    count = counts(1);
    if ~cfg.useAllRounds, count = min(count, n); end
    if count == 0 || rem(count, 2)
        error('read_ring_bscan:TriggerParity', '所选历史数据需要非零偶数触发。');
    end
    columns = cell(1, 8);
    inputFiles = cell(1, 8);
    for ch = selectedCh
        rows = provenance{ch}.columns;
        if ~isempty(rows) && any(strlength(rows.captureId) > 0)
            error('read_ring_bscan:UnverifiedMetadata', '历史读法包含 V2 采集文件。');
        end
        columns{ch} = 1:count;
        inputFiles{ch} = provenance{ch}.inputFiles;
    end
    evidence = struct('source', 'legacy-explicit', 'softwareRounds', NaN, ...
        'captureId', "", 'rounds', "legacy", 'files', {inputFiles});
end

bscan.wl1 = cell(1, 8);
bscan.wl2 = cell(1, 8);
for ch = selectedCh
    mat = data{ch}(:, columns{ch});
    bscan.wl1{ch} = mat(:, 1:2:end);
    bscan.wl2{ch} = mat(:, 2:2:end);
end
evidence.physicalPhase = 'unverified';
bscan.captureEvidence = evidence;
end

function [captureId, indices] = selectFiles(cfg, selectedCh)
cards = unique(ceil(selectedCh / 2));
indices = cell(1, 4);
captureId = "";
if isfield(cfg, 'captureId'), captureId = lower(string(cfg.captureId)); end
if ~isscalar(captureId) || ismissing(captureId)
    error('read_ring_bscan:CaptureId', 'cfg.captureId 必须为单个采集 ID。');
end
if isfield(cfg, 'allowUnverifiedLegacy') && cfg.allowUnverifiedLegacy && strlength(captureId) == 0
    if ~isfield(cfg, 'legacyFileIndices') || isempty(cfg.legacyFileIndices)
        error('read_ring_bscan:UnverifiedMetadata', '历史读法要求列出旧文件的确切序号。');
    end
    oldIndices = cfg.legacyFileIndices;
    validateattributes(oldIndices, {'numeric'}, {'vector', 'integer', 'nonnegative', 'increasing'});
    for card = cards, indices{card} = oldIndices; end
    return;
end

suffix = char(cfg.fileSuffix);
if ~isempty(suffix), suffix = ['_' suffix]; end
pattern = ['^Card([1-4])_ChA', regexptranslate('escape', suffix), '_([0-9]+)\.dat\.jsonl$'];
files = dir(fullfile(cfg.dataDir, 'Card*_ChA*.dat.jsonl'));
pairCard = zeros(numel(files), 1);
pairSequence = zeros(numel(files), 1);
pairId = strings(numel(files), 1);
for k = 1:numel(files)
    tokens = regexp(files(k).name, pattern, 'tokens', 'once');
    if isempty(tokens), continue; end
    card = str2double(tokens{1});
    if ~ismember(card, cards), continue; end
    id = readCaptureId(fullfile(files(k).folder, files(k).name));
    if strlength(id) == 0, continue; end
    pairId(k) = id;
    pairCard(k) = card;
    pairSequence(k) = str2double(tokens{2});
end
candidates = unique(pairId(strlength(pairId) > 0));
if strlength(captureId) == 0
    if isempty(candidates)
        error('read_ring_bscan:UnverifiedMetadata', '未找到可核验的 V2 旁车。');
    elseif numel(candidates) ~= 1
        error('read_ring_bscan:CaptureId', '发现多个采集，请指定 cfg.captureId。');
    end
    captureId = candidates(1);
end
for card = cards
    seq = sort(pairSequence(pairId == captureId & pairCard == card));
    if isempty(seq) || numel(unique(seq)) ~= numel(seq)
        error('read_ring_bscan:UnverifiedCoverage', '卡 %d 的指定采集缺少唯一的文件对。', card);
    end
    indices{card} = seq(1:min(end, cfg.filesPerChannel));
end
end

function id = readCaptureId(path)
id = "";
fid = fopen(path, 'rt');
if fid < 0, return; end
guard = onCleanup(@() fclose(fid)); %#ok<NASGU>
try
    layout = jsondecode(fgetl(fid));
    if isequal(layout.version, 2) && strcmp(layout.type, 'pae-raw-pair')
        candidate = lower(string(layout.captureId));
        if isscalar(candidate) && ~ismissing(candidate) && ~isempty(regexp(char(candidate), ...
                '^[0-9a-f]{8}(-[0-9a-f]{4}){3}-[0-9a-f]{12}$', 'once'))
            id = candidate;
        end
    end
catch
    % A missing/invalid header provides no candidate capture identity.
end
end

function [columns, chosen, files] = verifiedRounds(cfg, channels, provenance, captureId, n)
ref = provenance{channels(1)};
if ~ref.hasSidecar
    error('read_ring_bscan:UnverifiedMetadata', '选定采集缺少旁车。');
end
if isfield(cfg, 'fs') && abs(ref.sampleIntervalNs * cfg.fs / 1e9 - 1) > 1e-9
    error('read_ring_bscan:SamplingRate', 'cfg.fs 与采集旁车中的采样间隔不一致。');
end
rounds = unique(ref.columns.round, 'stable');
for ch = channels
    info = provenance{ch};
    if ~info.hasSidecar || any(info.columns.captureId ~= captureId) || ...
            info.sampleIntervalNs ~= ref.sampleIntervalNs || ...
            (cfg.useAllRounds && ~isequal(unique(info.columns.round, 'stable'), rounds))
        error('read_ring_bscan:UnverifiedCoverage', '所选卡的采集身份、采样参数或轮次不一致。');
    end
end
if isfield(cfg, 'roundId') && strlength(string(cfg.roundId)) > 0
    rounds = rounds(rounds == string(cfg.roundId));
end
columns = cell(1, 8);
files = cell(1, 8);
chosen = strings(0, 1);
for round = rounds.'
    reference = ref.columns(ref.columns.round == round, :);
    valid = completeRound(reference, n);
    selected = cell(1, 8);
    for ch = channels
        rows = provenance{ch}.columns;
        rowIndices = find(rows.round == round);
        block = rows(rowIndices, :);
        valid = valid && completeRound(block, n) && ...
            isequal(block(:, {'session', 'index', 'trigger'}), ...
                    reference(:, {'session', 'index', 'trigger'}));
        selected{ch} = rowIndices;
    end
    if ~valid
        if cfg.useAllRounds
            error('read_ring_bscan:UnverifiedCoverage', '多圈输入包含不完整或不匹配的轮次。');
        end
        continue;
    end
    for ch = channels
        columns{ch} = [columns{ch}; selected{ch}];
    end
    chosen(end+1, 1) = round;
    if ~cfg.useAllRounds, break; end
end
if isempty(chosen)
    error('read_ring_bscan:UnverifiedCoverage', '选定采集中没有各卡共同的完整轮次。');
end
for ch = channels
    inputs = provenance{ch}.columns(columns{ch}, {'fileA', 'fileB'});
    files{ch} = unique([inputs.fileA, inputs.fileB], 'rows', 'stable');
end
end

function result = completeRound(rows, n)
result = height(rows) == n && all(rows.complete) && rows.session(1) ~= "0" && ...
    all(rows.session == rows.session(1)) && isequal(rows.index, (0:(n-1)).') && ...
    all(mod(diff(rows.trigger), 65536) == 1);
end
