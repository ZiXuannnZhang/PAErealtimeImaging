function test_read_ring_raw_evidence
root = tempname;
mkdir(root);
cleanup = onCleanup(@() rmdir(root, 's')); %#ok<NASGU>
captureA = "12345678-1234-4321-a123-1234567890ab";
captureB = "12345678-1234-4321-b123-1234567890ab";
cfg = struct('dataDir', root, 'channelSel', [1 2 3], 'fileSuffix', 'fixture', ...
    'filesPerChannel', 1, 'triggersPerFile', 8, 'sampleCount', 2, ...
    'fullAlinesPerChannel', 4, 'useAllRounds', false, 'fs', 250e6, 'captureId', captureA);

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
expectError(@() read_ring_bscan(cfg), 'read_ring_saved_channel:UnverifiedMetadata');
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
disp('PASS capture identity, common complete round, historical selection and physical direction');
end

function writePair(root, card, fileIndex, captureId, indices, seq, complete, sealed)
fileA = sprintf('Card%d_ChA_fixture_%03d.dat', card, fileIndex);
fileB = sprintf('Card%d_ChB_fixture_%03d.dat', card, fileIndex);
for name = {fileA, fileB}
    fid = fopen(fullfile(root, name{1}), 'w', 'ieee-le');
    assert(fid >= 0);
    fwrite(fid, uint16(15360 * ones(2, numel(indices))), 'uint16');
    fclose(fid);
end
fid = fopen(fullfile(root, [fileA '.jsonl']), 'w');
assert(fid >= 0);
closing = onCleanup(@() fclose(fid)); %#ok<NASGU>
layout = struct('type', 'pae-raw-pair', 'version', 2, 'card', card, 'captureId', captureId, ...
    'fileA', fileA, 'fileB', fileB, 'sampleCount', 2, 'sampleIntervalNs', 4, ...
    'packetAnchor', 'first-arrival', 'physicalPhase', 'unverified');
fprintf(fid, '%s\n', jsonencode(layout));
for n = 1:numel(indices)
    slotComplete = complete || n ~= 2;
    row = struct('type', 'column', 'column', n, 'session', '10', 'round', num2str(floor((n-1) / 4)), ...
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
