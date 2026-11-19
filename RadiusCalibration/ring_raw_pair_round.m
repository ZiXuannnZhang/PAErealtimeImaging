function [declared, session, round] = ring_raw_pair_round(layout)
% A V2 pair can declare that every column belongs to one physical round.
declared = isfield(layout, 'fileSession') || isfield(layout, 'fileRound');
session = "";
round = "";
if isfield(layout, 'fileSession') && isfield(layout, 'fileRound') && ...
        ischar(layout.fileSession) && ischar(layout.fileRound) && ...
        ~isempty(regexp(layout.fileSession, '^[1-9][0-9]*$', 'once')) && ...
        ~isempty(regexp(layout.fileRound, '^(0|[1-9][0-9]*)$', 'once'))
    session = string(layout.fileSession);
    round = string(layout.fileRound);
end
end
