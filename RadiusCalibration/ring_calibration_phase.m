function phase = ring_calibration_phase(evidence, records)
% Each selected physical round needs a measured origin, direction and wavelength phase.
roundIds = string(evidence.rounds(:));
required = {'captureId', 'roundId', 'reference', 'direction', ...
            'startAngleDeg', 'firstWavelengthNm', 'secondOffsetDeg'};
if ~isstruct(records) || isempty(records) || ~all(isfield(records, required))
    error('radius_calibration:PhysicalPhase', '请提供逐圈的起点、方向、波长相位和设备证据引用。');
end
ids = string({records.roundId});
starts = zeros(numel(roundIds), 2);
steps = zeros(numel(roundIds), 1);
first = zeros(numel(roundIds), 1);
selected = zeros(numel(roundIds), 1);
for k = 1:numel(roundIds)
    index = find(ids == roundIds(k));
    if ~isscalar(index)
        error('radius_calibration:PhysicalPhase', '物理轮 %s 必须有且只有一条证据。', roundIds(k));
    end
    record = records(index);
    selected(k) = index;
    if ~strcmpi(record.captureId, evidence.captureId) || strlength(strtrim(string(record.reference))) == 0
        error('radius_calibration:PhysicalPhase', '物理轮 %s 的证据缺少来源或对应采集 ID。', roundIds(k));
    end
    if strcmpi(record.direction, 'CW')
        steps(k) = 1;
    elseif strcmpi(record.direction, 'CCW')
        steps(k) = -1;
    else
        error('radius_calibration:PhysicalPhase', '物理轮 %s 需指定 CW 或 CCW。', roundIds(k));
    end
    validateattributes(record.startAngleDeg, {'numeric'}, {'real', 'finite', 'scalar'});
    validateattributes(record.secondOffsetDeg, {'numeric'}, {'real', 'finite', 'scalar', '>=', 0, '<', 360});
    if isequal(record.firstWavelengthNm, 532)
        first(k) = 1;
    elseif isequal(record.firstWavelengthNm, 1064)
        first(k) = 2;
    else
        error('radius_calibration:PhysicalPhase', '首触发波长必须为 532 或 1064 nm。');
    end
    starts(k, first(k)) = mod(record.startAngleDeg, 360);
    starts(k, 3 - first(k)) = mod(record.startAngleDeg + steps(k) * record.secondOffsetDeg, 360);
end
if any(steps ~= steps(1)) || any(first ~= first(1)) || any(any(starts ~= starts(1, :)))
    error('radius_calibration:IncompatibleRounds', '多圈方向或波长相位不兼容，请逐圈标定。');
end
phase = struct('stepSign', steps(1), 'firstLane', first(1), ...
    'startDeg', starts(1, :), 'records', records(selected));
end
