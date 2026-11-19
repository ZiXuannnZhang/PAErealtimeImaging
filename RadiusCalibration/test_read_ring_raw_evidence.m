function test_read_ring_raw_evidence
root = tempname;
mkdir(root);
cleanup = onCleanup(@() rmdir(root, 's')); %#ok<NASGU>
captureA = "12345678-1234-4321-a123-1234567890ab";
captureB = "12345678-1234-4321-b123-1234567890ab";
cfg = struct('dataDir', root, 'channelSel', [1 2 3], 'fileSuffix', 'fixture', ...
    'filesPerChannel', 1, 'triggersPerFile', 8, 'sampleCount', 2, ...
    'fullAlinesPerChannel', 4, 'useAllRounds', false, 'fs', 250e6, 'captureId', captureA);

testDefaultRoundFallback(root, captureA, cfg);

writePair(root, 1, 0, captureA, 0:3, [65534 65535 0 1], true, true);
writePair(root, 2, 0, captureA, 0:3, [65534 65535 0 1], true, true);
bscan = read_ring_bscan(cfg);
assert(isequal(size(bscan.wl1{1}), [2 2]));
assert(strcmp(bscan.captureEvidence.physicalPhase, 'unverified'));

writePair(root, 1, 0, captureA, 1:4, 20:23, true, true);
writePair(root, 2, 0, captureA, 1:4, 20:23, true, true);
expectError(@() read_ring_bscan(cfg), 'read_ring_bscan:UnverifiedCoverage');
writePair(root, 1, 0, captureA, 0:3, [20 21 23 24], true, true);
writePair(root, 2, 0, captureA, 0:3, [20 21 23 24], true, true);
expectError(@() read_ring_bscan(cfg), 'read_ring_bscan:UnverifiedCoverage');
writePair(root, 2, 0, captureA, 0:3, [20 21 23 24], false, true);
expectError(@() read_ring_bscan(cfg), 'read_ring_bscan:UnverifiedCoverage');
writePair(root, 2, 0, captureA, 0:3, 20:23, true, false);
expectError(@() read_ring_bscan(cfg), 'read_ring_bscan:UncertainRound');
% The restarted measurement reused trigger counters and suffix. Card 1 skipped
% sequence zero, while card 2 was free to use it.
writePair(root, 1, 0, captureA, 0:3, 20:23, true, true);
writePair(root, 1, 1, captureB, 0:3, 20:23, true, true);
writePair(root, 2, 0, captureB, 0:3, 20:23, true, true);
expectError(@() read_ring_bscan(cfg), 'read_ring_bscan:UnverifiedCoverage');
cfg.captureId = "";
expectError(@() read_ring_bscan(cfg), 'read_ring_bscan:CaptureId');
cfg.captureId = captureB;
bscan = read_ring_bscan(cfg);
assert(bscan.captureEvidence.captureId == captureB);
assert(contains(bscan.captureEvidence.files{1}(1), '001.dat'));

writePair(root, 1, 1, captureB, [0:3 0:3], 20:27, true, true);
writePair(root, 2, 0, captureB, [0:3 0:1], 20:25, true, true);
bscan = read_ring_bscan(cfg);
assert(bscan.captureEvidence.rounds == "0");
cfg.useAllRounds = true;
expectError(@() read_ring_bscan(cfg), 'read_ring_bscan:UnverifiedCoverage');
cfg.useAllRounds = false;

phaseEvidence = bscan.captureEvidence;
record = struct('captureId', captureB, 'roundId', "0", 'reference', "fixture", ...
    'direction', "CCW", 'startAngleDeg', 180, 'firstWavelengthNm', 1064, 'secondOffsetDeg', 20);
phase = ring_calibration_phase(phaseEvidence, record);
assert(phase.firstLane == 2 && isequal(phase.startDeg, [160 180]));
assert(isequal(mod(phase.startDeg(2) + phase.stepSign * 90 * (0:3), 360), [180 90 0 270]));
phaseEvidence.rounds = ["0"; "1"];
record(2) = record(1);
record(2).roundId = "1";
record(2).direction = "CW";
expectError(@() ring_calibration_phase(phaseEvidence, record), 'radius_calibration:IncompatibleRounds');

writePair(root, 2, 0, captureB, 0:3, 20:23, true, true);
delete(fullfile(root, '*.jsonl'));
cfg.triggersPerFile = 4;
cfg.captureId = "";
expectError(@() read_ring_bscan(cfg), 'read_ring_bscan:UnverifiedMetadata');
cfg.allowUnverifiedLegacy = true;
expectError(@() read_ring_bscan(cfg), 'read_ring_bscan:UnverifiedMetadata');
cfg.legacyFileIndices = 0;
bscan = read_ring_bscan(cfg);
assert(strcmp(bscan.captureEvidence.source, 'legacy-explicit'));
testRoundDiscovery(root, captureA, cfg);
disp('PASS capture identity, scoped round discovery, paired waveforms, historical selection and physical direction');
end

function testDefaultRoundFallback(root, captureId, cfg)
cfg.dataDir = fullfile(root, 'fallback');
mkdir(cfg.dataDir);
writePair(cfg.dataDir, 1, 0, captureId, 0:3, 20:23, true, false, 0, true);
writePair(cfg.dataDir, 2, 3, captureId, 0:3, 20:23, true, true, 0, true);
writePair(cfg.dataDir, 1, 5, captureId, 0:3, 24:27, true, true, 1, true, [15360 16384 16896 17408]);
writePair(cfg.dataDir, 2, 7, captureId, 0:3, 24:27, true, true, 1, true, [17408 16896 16384 15360]);
bscan = read_ring_bscan(cfg);
assert(bscan.captureEvidence.rounds == "1" && ...
       bscan.captureEvidence.unrelatedFailures.issue == "unsealed_sidecar");
assert(isequal(bscan.wl1{1}(1,:), single([1 3])) && isequal(bscan.wl2{3}(1,:), single([3 1])));
wrong = cfg;
wrong.fs = 125e6;
expectError(@() read_ring_bscan(wrong), 'read_ring_bscan:SamplingRate');
wrong = cfg;
wrong.sampleCount = 4;
expectError(@() read_ring_bscan(wrong), 'read_ring_saved_channel:UnverifiedMetadata');
cfg.roundId = "0";
expectError(@() read_ring_bscan(cfg), 'read_ring_saved_channel:UnverifiedMetadata');
cfg.roundId = "1";
picked = read_ring_bscan(cfg);
assert(picked.captureEvidence.rounds == "1");
cfg.useAllRounds = true;
cfg.roundId = ["0"; "1"];
cfg.filesPerChannel = 2;
expectError(@() read_ring_bscan(cfg), 'read_ring_saved_channel:UnverifiedMetadata');
cfg.filesPerChannel = 1;
cfg.useAllRounds = false;
cfg.roundId = "";

% A damaged old V2 pair has exactly the two identified rounds. Neither
% is eligible, even if the other card has a complete copy of each round.
writePair(cfg.dataDir, 1, 0, captureId, [0:3 0:3], 20:27, true, true);
fid = fopen(fullfile(cfg.dataDir, 'Card1_ChB_fixture_000.dat'), 'a');
assert(fid >= 0);
fwrite(fid, uint8(1));
fclose(fid);
for card = 1:2
    writePair(cfg.dataDir, card, 9+card, captureId, 0:3, 28:31, true, true, 2, true);
end
picked = read_ring_bscan(cfg);
assert(picked.captureEvidence.rounds == "2");
cfg.roundFileIndices = {0, [3 7], [], []};
expectError(@() read_ring_bscan(cfg), 'read_ring_bscan:UnverifiedCoverage');
cfg.roundFileIndices = {};

% A bad strict-parser field survives discovery, but can be associated with
% its proven pair. Default skips it. Explicit selection keeps its error ID.
writePair(cfg.dataDir, 1, 0, captureId, 0:3, 20:23, true, true, 0, true);
sidecar = fullfile(cfg.dataDir, 'Card1_ChA_fixture_000.dat.jsonl');
changeSidecar(sidecar, '"triggerSeq":20', '"triggerSeq":65536');
bscan = read_ring_bscan(cfg);
assert(bscan.captureEvidence.rounds == "1" && ...
       bscan.captureEvidence.unrelatedFailures.issue == "invalid_trigger_identity");
cfg.roundId = "0";
expectError(@() read_ring_bscan(cfg), 'read_ring_saved_channel:UnverifiedMetadata');
cfg.roundId = "";
changeSidecar(sidecar, '"fileRound":"0"', '"fileRound":"4"');
expectError(@() read_ring_bscan(cfg), 'read_ring_bscan:UncertainRound');
cfg.roundFileIndices = {5, 7, [], []};
picked = read_ring_bscan(cfg);
assert(picked.captureEvidence.rounds == "1");
end

function changeSidecar(file, from, to)
body = strrep(fileread(file), from, to);
fid = fopen(file, 'w');
assert(fid >= 0);
fprintf(fid, '%s', body);
fclose(fid);
end

function testRoundDiscovery(root, captureId, cfg)
cfg.dataDir = fullfile(root, 'discover');
mkdir(cfg.dataDir);
cfg.captureId = captureId;
cfg.filesPerChannel = 1;
cfg.triggersPerFile = 8;
cfg.roundId = "0";
cfg.useAllRounds = false;
writePair(cfg.dataDir, 1, 0, captureId, 0:3, 20:23, true, true);
writePair(cfg.dataDir, 2, 3, captureId, 0:3, 20:23, true, true);
writePair(cfg.dataDir, 1, 5, captureId, 0:3, 24:27, true, true, 1);
writePair(cfg.dataDir, 2, 7, captureId, 0:3, 24:27, true, true, 1);
fid = fopen(fullfile(cfg.dataDir, 'Card1_ChB_fixture_005.dat'), 'a');
assert(fid >= 0);
fwrite(fid, uint8(1));
fclose(fid);
bscan = read_ring_bscan(cfg);
assert(bscan.captureEvidence.rounds == "0");
assert(numel(bscan.captureEvidence.unrelatedFailures) == 1 && ...
       bscan.captureEvidence.unrelatedFailures.issue == "paired_length_mismatch");
cfg.roundId = "1";
expectError(@() read_ring_bscan(cfg), 'read_ring_saved_channel:UnverifiedMetadata');

writePair(cfg.dataDir, 1, 5, captureId, 0:3, 24:27, true, false, 1, true);
cfg.roundId = "0";
bscan = read_ring_bscan(cfg);
assert(bscan.captureEvidence.unrelatedFailures.issue == "unsealed_sidecar");
cfg.roundId = "1";
expectError(@() read_ring_bscan(cfg), 'read_ring_saved_channel:UnverifiedMetadata');

writePair(cfg.dataDir, 1, 5, captureId, 0:3, 24:27, true, false, 1);
cfg.roundId = "0";
expectError(@() read_ring_bscan(cfg), 'read_ring_bscan:UncertainRound');
cfg.roundFileIndices = {0, 3, [], []};
bscan = read_ring_bscan(cfg);
assert(bscan.captureEvidence.rounds == "0");
cfg.roundFileIndices = {5, 7, [], []};
cfg.roundId = "1";
expectError(@() read_ring_bscan(cfg), 'read_ring_saved_channel:UnverifiedMetadata');
cfg.roundFileIndices = {};
cfg.scanFilesPerCard = 1;
expectError(@() read_ring_bscan(cfg), 'read_ring_bscan:ScanLimit');
cfg.scanFilesPerCard = 512;
writePair(cfg.dataDir, 1, 5, captureId, 0:3, 24:27, true, true, 1, true);
bscan = read_ring_bscan(cfg);
assert(bscan.captureEvidence.rounds == "1" && ...
    contains(bscan.captureEvidence.files{1}(1), '005.dat') && ...
    contains(bscan.captureEvidence.files{3}(1), '007.dat'));

% Even a sealed V2 header must agree with the readable column identities.
sidecar = fullfile(cfg.dataDir, 'Card1_ChA_fixture_005.dat.jsonl');
text = strrep(fileread(sidecar), '"fileRound":"1"', '"fileRound":"0"');
fid = fopen(sidecar, 'w');
assert(fid >= 0);
fprintf(fid, '%s', text);
fclose(fid);
expectError(@() read_ring_bscan(cfg), 'read_ring_saved_channel:UnverifiedMetadata');
cfg.roundId = "0";
expectError(@() read_ring_bscan(cfg), 'read_ring_bscan:UncertainRound');
fid = fopen(sidecar, 'w');
assert(fid >= 0);
fprintf(fid, '%s', strrep(text, '"session":"10",', ''));
fclose(fid);
expectError(@() read_ring_bscan(cfg), 'read_ring_bscan:UncertainRound');

cfg.dataDir = fullfile(root, 'split');
mkdir(cfg.dataDir);
for card = 1:2
    % Physical file sequence is independent of logical order and of the other card.
    writePair(cfg.dataDir, card, card*2, captureId, 2:3, [0 1], true, true, 2, true, [16896 17408]);
    writePair(cfg.dataDir, card, 15-card, captureId, 0:1, [65534 65535], true, true, 2, true, [15360 16384]);
    writePair(cfg.dataDir, card, 20+card, captureId, 0:3, 2:5, true, true, 3, true);
end
cfg.roundId = "2";
expectError(@() read_ring_bscan(cfg), 'read_ring_bscan:ReadLimit');
cfg.roundId = "";
expectError(@() read_ring_bscan(cfg), 'read_ring_bscan:ReadLimit');
cfg.roundId = "2";
cfg.filesPerChannel = 2;
bscan = read_ring_bscan(cfg);
assert(isequal(bscan.wl1{1}(1,:), single([1 3])) && ...
       isequal(bscan.wl2{3}(1,:), single([2 4])));
cfg.useAllRounds = true;
cfg.filesPerChannel = 3;
cfg.roundId = ["3"; "2"];
bscan = read_ring_bscan(cfg);
assert(isequal(bscan.captureEvidence.rounds, ["2"; "3"]) && size(bscan.wl1{2}, 2) == 4);
record = struct('captureId', captureId, 'roundId', "2", 'reference', "fixture", ...
    'direction', "CCW", 'startAngleDeg', 180, 'firstWavelengthNm', 1064, 'secondOffsetDeg', 20);
record(2) = record(1);
record(2).roundId = "3";
phase = ring_calibration_phase(bscan.captureEvidence, record);
assert(phase.firstLane == 2);
cfg.roundId = ["2"; "4"];
expectError(@() read_ring_bscan(cfg), 'read_ring_bscan:UnverifiedCoverage');
cfg.roundId = "2";
cfg.useAllRounds = false;
writePair(cfg.dataDir, 1, 17, captureId, 1, 65535, true, true, 2, true);
expectError(@() read_ring_bscan(cfg), 'read_ring_bscan:UnverifiedCoverage');
end

function writePair(root, card, fileIndex, captureId, indices, seq, complete, sealed, rounds, declareRound, halfValues)
if nargin < 9, rounds = floor((0:numel(indices)-1) / 4); end
if isscalar(rounds), rounds = repmat(rounds, size(indices)); end
if nargin < 10, declareRound = false; end
if nargin < 11, halfValues = 15360 * ones(size(indices)); end
fileA = sprintf('Card%d_ChA_fixture_%03d.dat', card, fileIndex);
fileB = sprintf('Card%d_ChB_fixture_%03d.dat', card, fileIndex);
for name = {fileA, fileB}
    fid = fopen(fullfile(root, name{1}), 'w', 'ieee-le');
    assert(fid >= 0);
    fwrite(fid, uint16(repmat(halfValues, 2, 1)), 'uint16');
    fclose(fid);
end
fid = fopen(fullfile(root, [fileA '.jsonl']), 'w');
assert(fid >= 0);
closing = onCleanup(@() fclose(fid)); %#ok<NASGU>
layout = struct('type', 'pae-raw-pair', 'version', 2, 'card', card, 'captureId', captureId, ...
    'fileA', fileA, 'fileB', fileB, 'sampleCount', 2, 'sampleIntervalNs', 4, ...
    'packetAnchor', 'first-arrival', 'physicalPhase', 'unverified');
if declareRound
    assert(numel(unique(rounds)) == 1);
    layout.fileSession = '10';
    layout.fileRound = num2str(rounds(1));
end
fprintf(fid, '%s\n', jsonencode(layout));
for n = 1:numel(indices)
    slotComplete = complete || n ~= 2;
    row = struct('type', 'column', 'column', n, 'session', '10', 'round', num2str(rounds(n)), ...
        'logicalIndex', indices(n), 'triggerSeq', seq(n), 'complete', slotComplete, ...
        'expectedSlots', 2, 'receivedSlots', 1 + slotComplete, ...
        'late', false, 'tail', false);
    fprintf(fid, '%s\n', jsonencode(row));
end
if sealed
    fprintf(fid, '%s\n', jsonencode(struct('type', 'end', 'confirmedColumns', numel(indices))));
end
end

function expectError(action, identifier)
try
    action();
catch exception
    assert(strcmp(exception.identifier, identifier), '%s', exception.message);
    return
end
error('test_read_ring_raw_evidence:ExpectedError', 'Missing expected error %s', identifier);
end
