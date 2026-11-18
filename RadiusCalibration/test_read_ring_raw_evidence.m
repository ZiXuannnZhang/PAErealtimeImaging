function test_read_ring_raw_evidence
root = tempname;
mkdir(root);
cleanup = onCleanup(@() rmdir(root, 's')); %#ok<NASGU>
cfg = struct('dataDir', root, 'channelSel', [1 2 3], 'fileSuffix', 'fixture', ...
    'filesPerChannel', 1, 'triggersPerFile', 8, 'sampleCount', 2, ...
    'fullAlinesPerChannel', 4, 'useAllRounds', false, 'fs', 250e6);

writePair(root, 1, 0:3, [65534 65535 0 1], true, true);
writePair(root, 2, 0:3, [65534 65535 0 1], true, true);
bscan = read_ring_bscan(cfg);
assert(isequal(size(bscan.wl1{1}), [2 2]));
assert(strcmp(bscan.captureEvidence.physicalPhase, 'unverified'));

writePair(root, 1, 1:4, 20:23, true, true);
writePair(root, 2, 1:4, 20:23, true, true);
expectError(@() read_ring_bscan(cfg), 'read_ring_bscan:UnverifiedCoverage');
writePair(root, 1, 0:3, [20 21 23 24], true, true);
writePair(root, 2, 0:3, [20 21 23 24], true, true);
expectError(@() read_ring_bscan(cfg), 'read_ring_bscan:UnverifiedCoverage');
writePair(root, 2, 0:3, [20 21 23 24], false, true);
expectError(@() read_ring_bscan(cfg), 'read_ring_bscan:UnverifiedCoverage');
writePair(root, 2, 0:3, 20:23, true, false);
expectError(@() read_ring_bscan(cfg), 'read_ring_saved_channel:UnverifiedMetadata');
delete(fullfile(root, '*.jsonl'));
cfg.triggersPerFile = 4;
bscan = read_ring_bscan(cfg);
assert(strcmp(bscan.captureEvidence.source, 'legacy'));
disp('PASS raw metadata, rollover count, partial/missing trigger and legacy files');
end

function writePair(root, card, indices, seq, complete, sealed)
fileA = sprintf('Card%d_ChA_fixture_000.dat', card);
fileB = sprintf('Card%d_ChB_fixture_000.dat', card);
for name = {fileA, fileB}
    fid = fopen(fullfile(root, name{1}), 'w', 'ieee-le');
    assert(fid >= 0);
    fwrite(fid, uint16(15360 * ones(2, numel(indices))), 'uint16');
    fclose(fid);
end
fid = fopen(fullfile(root, [fileA '.jsonl']), 'w');
assert(fid >= 0);
closing = onCleanup(@() fclose(fid)); %#ok<NASGU>
layout = struct('type', 'pae-raw-pair', 'version', 1, 'card', card, ...
    'fileA', fileA, 'fileB', fileB, 'sampleCount', 2, 'sampleIntervalNs', 4, ...
    'packetAnchor', 'first-arrival', 'physicalPhase', 'unverified');
fprintf(fid, '%s\n', jsonencode(layout));
for n = 1:numel(indices)
    slotComplete = complete || n ~= 2;
    row = struct('type', 'column', 'column', n, 'session', '10', 'round', '0', ...
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
