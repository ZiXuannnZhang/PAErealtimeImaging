function pair = inspect_ring_raw_pair(dataDir, card, sequence, fileA, fileB, captureFilter)
% Discover round membership from the sidecar without reading waveform samples.
pair = struct('card', card, 'sequence', sequence, 'fileA', string(fileA), 'fileB', string(fileB), ...
    'captureId', "", 'legacy', false, 'rounds', strings(0, 1), ...
    'knownRounds', false, 'sampleCount', NaN, 'sampleIntervalNs', NaN, 'issue', "");
path = fullfile(dataDir, [fileA '.jsonl']);
fid = fopen(path, 'rt');
if fid < 0, return; end
guard = onCleanup(@() fclose(fid)); %#ok<NASGU>
layout = decode(fgetl(fid));
if ~all(isfield(layout, {'type','version','card','fileA','fileB'})) || ...
        ~isequal(layout.type, 'pae-raw-pair') || ~isequal(layout.card, card) || ...
        ~isequal(layout.fileA, fileA) || ~isequal(layout.fileB, fileB)
    return;
end
if isequal(layout.version, 1)
    pair.legacy = true;
    return;
end
if ~isequal(layout.version, 2) || ~isfield(layout, 'captureId') || ~ischar(layout.captureId) || ...
        isempty(regexpi(layout.captureId, '^[0-9a-f]{8}(-[0-9a-f]{4}){3}-[0-9a-f]{12}$', 'once'))
    return;
end
pair.captureId = lower(string(layout.captureId));
if captureFilter ~= "" && pair.captureId ~= captureFilter, return; end
if isfield(layout, 'sampleCount') && validPositive(layout.sampleCount) && mod(layout.sampleCount, 1) == 0
    pair.sampleCount = layout.sampleCount;
end
if isfield(layout, 'sampleIntervalNs') && validPositive(layout.sampleIntervalNs)
    pair.sampleIntervalNs = layout.sampleIntervalNs;
end
[declared, fileSession, fileRound] = ring_raw_pair_round(layout);
rowsValid = true;
contradiction = false;
count = 0;
ending = [];
endSeen = false;
while true
    line = fgetl(fid);
    if ~ischar(line), break; end
    if isempty(strtrim(line)), continue; end
    row = decode(line);
    if endSeen
        rowsValid = false; % An end record has to be the final record.
    end
    if isfield(row, 'type') && isequal(row.type, 'end')
        endSeen = true;
        if isfield(row, 'confirmedColumns'), ending = row.confirmedColumns; end
        continue;
    end
    count = count + 1;
    if ~isfield(row, 'type') || ~isequal(row.type, 'column')
        rowsValid = false;
        continue;
    end
    hasRound = isfield(row, 'round') && isDecimalId(row.round);
    hasSession = isfield(row, 'session') && isDecimalId(row.session);
    if hasRound
        id = string(row.round);
        if ~any(pair.rounds == id), pair.rounds(end+1, 1) = id; end
    end
    if fileRound ~= ""
        contradiction = contradiction || (hasRound && string(row.round) ~= fileRound) || ...
            (hasSession && string(row.session) ~= fileSession);
    end
    rowsValid = rowsValid && hasRound && hasSession && isfield(row, 'column') && isequal(row.column, count);
end
if ~feof(fid)
    pair.issue = "unreadable_sidecar";
    return;
end
sealed = rowsValid && count > 0 && isequal(ending, count);
if fileRound ~= "" && ~contradiction
    % The writer splits pairs on a round change. The declaration identifies
    % even an interrupted pair but does not confirm a single waveform column.
    pair.rounds = fileRound;
    pair.knownRounds = true;
elseif sealed && ~declared
    pair.knownRounds = true;
end
if contradiction || (declared && fileRound == "")
    pair.issue = "invalid_round_declaration";
elseif ~sealed
    pair.issue = "unsealed_sidecar";
elseif ~isnan(pair.sampleCount) && ...
        (knownLengthMismatch(dataDir, fileA, 2 * pair.sampleCount * count) || ...
         knownLengthMismatch(dataDir, fileB, 2 * pair.sampleCount * count))
    pair.issue = "paired_length_mismatch";
end
end

function record = decode(line)
record = struct();
if ~ischar(line), return; end
try
    candidate = jsondecode(line);
    if isstruct(candidate) && isscalar(candidate), record = candidate; end
catch
    % A partially written line provides no identity evidence.
end
end

function valid = isDecimalId(value)
valid = ischar(value) && ~isempty(regexp(value, '^[0-9]+$', 'once'));
end

function valid = validPositive(value)
valid = isnumeric(value) && isscalar(value) && isreal(value) && isfinite(value) && value > 0;
end

function mismatch = knownLengthMismatch(dataDir, name, expected)
file = dir(fullfile(dataDir, name));
mismatch = isscalar(file) && ~file.isdir && file.bytes ~= expected;
end
