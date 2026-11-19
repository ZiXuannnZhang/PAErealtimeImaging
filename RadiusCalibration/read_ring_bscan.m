function bscan = read_ring_bscan(cfg)
%READ_RING_BSCAN Locate physical rounds, then verify and read their file pairs.
% cfg.captureId disambiguates captures. cfg.roundId selects one or more rounds.
% cfg.roundFileIndices optionally bounds discovery to explicit per-card pairs.

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

cards = unique(ceil(selectedCh / 2));
captureId = "";
if isfield(cfg, 'captureId'), captureId = lower(string(cfg.captureId)); end
if ~isscalar(captureId) || ismissing(captureId)
    error('read_ring_bscan:CaptureId', 'cfg.captureId 必须为单个采集 ID。');
end
if captureId == "" && isfield(cfg, 'allowUnverifiedLegacy') && cfg.allowUnverifiedLegacy
    [data, columns, evidence] = readLegacy(cfg, selectedCh, cards, n);
else
    [captureId, catalog] = discoverPairs(cfg, cards, captureId);
    [rounds, indices] = chooseRounds(cfg, cards, catalog, captureId, n);
    checkReadBudget(cfg.filesPerChannel, cards, indices);
    [data, provenance] = readPairs(cfg, selectedCh, indices, false);
    [valid, columns, files] = verifyCoverage(cfg, selectedCh, provenance, captureId, rounds, n);
    if ~valid
        error('read_ring_bscan:UnverifiedCoverage', '所选卡缺少共同的完整轮次，或逻辑列有重复、不连续。');
    end
    unrelated = catalog([catalog.knownRounds] & [catalog.issue] ~= "" & ...
        arrayfun(@(pair) ~any(ismember(pair.rounds, rounds)), catalog));
    evidence = struct('source', 'paired-sidecar-v2', 'softwareRounds', numel(rounds), ...
        'captureId', captureId, 'rounds', rounds, 'files', {files}, 'unrelatedFailures', {unrelated});
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

function [captureId, catalog] = discoverPairs(cfg, cards, captureId)
scanLimit = 512;
if isfield(cfg, 'scanFilesPerCard'), scanLimit = cfg.scanFilesPerCard; end
validateattributes(scanLimit, {'numeric'}, {'scalar', 'integer', 'positive'});
search = cell(1, 4);
suffix = char(cfg.fileSuffix);
if ~isempty(suffix), suffix = ['_' suffix]; end
if isfield(cfg, 'roundFileIndices') && ~isempty(cfg.roundFileIndices)
    if ~iscell(cfg.roundFileIndices) || numel(cfg.roundFileIndices) ~= 4
        error('read_ring_bscan:FileScope', 'cfg.roundFileIndices 必须是按卡号排列的四元素 cell。');
    end
    search = cfg.roundFileIndices;
    for card = cards
        validateattributes(search{card}, {'numeric'}, {'vector', 'nonempty', 'integer', 'nonnegative', 'increasing'});
    end
else
    pattern = ['^Card([1-4])_Ch[AB]', regexptranslate('escape', suffix), '_([0-9]+)\.dat(\.jsonl)?$'];
    listings = dir(fullfile(cfg.dataDir, 'Card*_Ch*.dat*'));
    for listing = listings(:).'
        if listing.isdir, continue; end
        tokens = regexp(listing.name, pattern, 'tokens', 'once');
        if isempty(tokens), continue; end
        card = str2double(tokens{1});
        if ismember(card, cards)
            sequence = str2double(tokens{2});
            if ~isfinite(sequence) || ~strcmp(tokens{2}, sprintf('%03d', sequence))
                error('read_ring_bscan:FileScope', '文件序号不符合保存命名规则：%s', listing.name);
            end
            search{card}(end+1) = sequence;
        end
    end
    for card = cards, search{card} = unique(search{card}); end
end

catalog = [];
for card = cards
    if numel(search{card}) > scanLimit
        error('read_ring_bscan:ScanLimit', ...
            '卡 %d 的扫描范围有 %d 对文件，超过扫描上限 %d。请缩小逐卡文件范围或提高扫描上限。', ...
            card, numel(search{card}), scanLimit);
    end
    for sequence = reshape(search{card}, 1, [])
        fileA = sprintf('Card%d_ChA%s_%03d.dat', card, suffix, sequence);
        fileB = sprintf('Card%d_ChB%s_%03d.dat', card, suffix, sequence);
        entry = inspect_ring_raw_pair(cfg.dataDir, card, sequence, fileA, fileB, captureId);
        if isempty(catalog), catalog = entry; else, catalog(end+1) = entry; end %#ok<AGROW>
    end
end
if isempty(catalog) || all([catalog.captureId] == "")
    error('read_ring_bscan:UnverifiedMetadata', '扫描范围内没有可核验的 V2 旁车。');
end
ids = [catalog.captureId];
candidates = unique(ids(ids ~= ""));
if captureId == ""
    if numel(candidates) ~= 1
        error('read_ring_bscan:CaptureId', '发现多个采集，请指定 cfg.captureId。');
    end
    captureId = candidates(1);
end
catalog = catalog(~[catalog.legacy] & (ids == captureId | ids == ""));
end

function [rounds, indices] = chooseRounds(cfg, cards, catalog, captureId, n)
requested = strings(0,1);
if isfield(cfg, 'roundId')
    requested = string(cfg.roundId(:));
    requested(requested == "") = [];
    if any(ismissing(requested)) || any(cellfun(@isempty, regexp(cellstr(requested), '^[0-9]+$', 'once'))) || ...
            (~cfg.useAllRounds && numel(requested) > 1)
        error('read_ring_bscan:RoundId', 'cfg.roundId 必须给出有效轮次；指定多圈时启用 useAllRounds。');
    end
end
if isempty(requested)
    if isempty(catalog)
        rounds = strings(0,1);
    else
        rounds = unique(vertcat(catalog.rounds));
    end
else
    rounds = unique(requested);
end
[~, order] = sortrows(table(strlength(rounds), rounds)); % uint64 decimal IDs without double rounding
rounds = rounds(order);
if isempty(rounds) && ~isempty(catalog) && any(~[catalog.knownRounds])
    reportUncertain(catalog(find(~[catalog.knownRounds], 1)));
end
if ~isempty(rounds)
    if ~isempty(requested) || cfg.useAllRounds
        indices = collectPairs(catalog, rounds);
        if all(~cellfun(@isempty, indices(cards))), return; end
    else
        for round = rounds.'
            indices = collectPairs(catalog, round);
            if any(cellfun(@isempty, indices(cards))), continue; end
            refChannels = 2 * cards - 1;
            [~, info] = readPairs(cfg, refChannels, indices, true);
            if verifyCoverage(cfg, refChannels, info, captureId, round, n)
                rounds = round;
                return;
            end
        end
    end
end
error('read_ring_bscan:UnverifiedCoverage', '选定扫描范围内没有各卡共同的完整轮次。');
end

function indices = collectPairs(catalog, rounds)
indices = cell(1,4);
for pair = catalog
    if any(ismember(pair.rounds, rounds))
        % Even an unsealed contributing pair has to pass the strict reader.
        indices{pair.card}(end+1) = pair.sequence;
    elseif ~pair.knownRounds
        reportUncertain(pair);
    end
end
end

function checkReadBudget(limit, cards, indices)
for card = cards
    if numel(indices{card}) > limit
        error('read_ring_bscan:ReadLimit', ...
            '卡 %d 的所选轮次需要读取 %d 对文件，超过读取上限 %d。', ...
            card, numel(indices{card}), limit);
    end
end
end

function reportUncertain(pair)
error('read_ring_bscan:UncertainRound', ...
    '无法确认 %s 是否包含所选轮次。请在 cfg.roundFileIndices 中指定有证据的逐卡文件范围。', ...
    pair.fileA);
end

function [data, provenance] = readPairs(cfg, channels, indices, metadataOnly)
data = cell(1, 8);
provenance = cell(1, 8);
for ch = channels
    files = indices{ceil(ch / 2)};
    [data{ch}, provenance{ch}] = read_ring_saved_channel(cfg.dataDir, ch, cfg.fileSuffix, ...
        numel(files), cfg.triggersPerFile, cfg.sampleCount, files, metadataOnly);
end
end

function [data, columns, evidence] = readLegacy(cfg, channels, cards, n)
if ~isfield(cfg, 'legacyFileIndices') || isempty(cfg.legacyFileIndices)
    error('read_ring_bscan:UnverifiedMetadata', '历史读法要求列出旧文件的确切序号。');
end
if cfg.useAllRounds
    error('read_ring_bscan:UnverifiedCoverage', '历史资料只允许显式选择单圈。');
end
oldIndices = cfg.legacyFileIndices;
validateattributes(oldIndices, {'numeric'}, {'vector', 'integer', 'nonnegative', 'increasing'});
indices = cell(1,4);
for card = cards, indices{card} = oldIndices; end
[data, provenance] = readPairs(cfg, channels, indices, false);
counts = cellfun(@(samples) size(samples, 2), data(channels));
if numel(unique(counts)) ~= 1
    error('read_ring_bscan:TriggerCount', '所选历史通道触发数不一致。');
end
count = min(counts(1), n);
if count == 0 || rem(count, 2)
    error('read_ring_bscan:TriggerParity', '所选历史数据需要非零偶数触发。');
end
columns = cell(1, 8);
inputFiles = cell(1, 8);
for ch = channels
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

function [valid, columns, files] = verifyCoverage(cfg, channels, provenance, captureId, rounds, n)
ref = provenance{channels(1)};
if ~ref.hasSidecar
    error('read_ring_bscan:UnverifiedMetadata', '选定采集缺少旁车。');
end
if isfield(cfg, 'fs')
    validateattributes(cfg.fs, {'numeric'}, {'scalar', 'real', 'finite', 'positive'});
    if abs(ref.sampleIntervalNs * cfg.fs / 1e9 - 1) > 1e-9
        error('read_ring_bscan:SamplingRate', 'cfg.fs 与采集旁车中的采样间隔不一致。');
    end
end
columns = cell(1, 8);
files = cell(1, 8);
valid = false;
for ch = channels
    info = provenance{ch};
    if ~info.hasSidecar || any(info.columns.captureId ~= captureId) || ...
            info.sampleIntervalNs ~= ref.sampleIntervalNs
        error('read_ring_bscan:UnverifiedCoverage', '所选卡的采集身份或采样参数不一致。');
    end
end
fields = {'session', 'index', 'trigger'};
for round = rounds.'
    reference = table();
    for ch = channels
        rows = provenance{ch}.columns;
        rowIndices = find(rows.round == round);
        [~, order] = sort(rows.index(rowIndices));
        rowIndices = rowIndices(order);
        block = rows(rowIndices, :);
        if ~completeRound(block, n), return; end
        if isempty(reference), reference = block(:, fields); end
        if ~isequal(block(:, fields), reference), return; end
        columns{ch} = [columns{ch}; rowIndices];
    end
end
valid = true;
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
