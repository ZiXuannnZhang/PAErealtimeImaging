function [accepted, rejects] = filterDbrConfigCheck(channels)
% FILTERDBRCONFIGCHECK 零相位滤波 × DBR × delayCut 配置前置校验参考实现
% （C2 整改，任务 §7.C2）。仅本任务新增处理定义下的前置规则；全部新功能
% 关闭时不引入新拒绝条件，保持旧行为。当前只做参考校验与测试，不接入
% 生产 UI/代码。
%
% 定义（每通道/波长独立检查；一条不满足即拒绝本组配置）：
%   filterEnabled  = highpassEnabled || lowpassEnabled
%   inversionEnabled = 光声反演开关
%   D = 该通道/波长 systemDelay（raw 一基声学零点）
%   E = 该通道/波长 A-line 实际 DBR 置零样本数（真实预处理语义）：
%       DBR 关闭时 E=0；DBR 开启但有效置零长度为 0 时 E=0（开关开启不
%       等同于存在阶跃）。E 计入该波长 dbrmaskExtra 与实际样本数边界
%       （E = min(maskLength + dbrmaskExtra, Nt)——置零长度不越过行尾），
%       不混用配置值与实际长度。
%
% 接受/拒绝表（本版冻结，不再"留待阶段 B 决定"）：
%   | 条件 | 决定 |
%   |---|---|
%   | filterEnabled=false | 不触发零相位滤波专属拒绝；仅反演保持既定
%   |     | 有效查询/导数端点规则（本函数不额外拒绝） |
%   | filterEnabled=true, E=0 | DBR 相关规则允许；仍需满足采样率、截止、
%   |     | 阶数和长度校验（refZeroPhase 既有报错，不在本表） |
%   | filterEnabled=true, E>0, delayCut=true | 仅当 E<D 允许；E>=D 明确报错 |
%   | filterEnabled=true, E>0, delayCut=false | 明确报错：不支持该组合 |
%
% 无效参数本身（maskLength<0、D<=0 等）沿用既定校验（本函数直接报错），
% 不做截断静默修复。
%
% 输入
%   channels : struct 数组，每元素字段
%       .name       通道/波长名（报告用，字符串）
%       .highpassEnabled .lowpassEnabled .inversionEnabled : 逻辑值
%       .dbrEnabled  DBR 开关（逻辑值；false → E=0）
%       .maskLength  DBR 置零基础行数（>=0 整数）
%       .dbrmaskExtra 该波长 DBR 附加行数（>=0 整数）
%       .sysDelay    该通道/波长 systemDelay D（正整数）
%       .delayCut    延时裁剪开关（逻辑值）
%       .Nt          该 A-line raw 样本数（正整数；置零长度不越过行尾）
% 输出
%   accepted : 逻辑值——本组所有通道均通过才为 true
%   rejects  : struct 数组（每拒绝通道一项；空数组 = 全通过），字段
%       .name .reason .message
%
% 错误语义（参考文案，任务 §7.C2）：
%   - 零相位滤波与未裁剪的 DBR 置零前缀不兼容（delayCut=false 且 E>0）
%   - DBR 置零末端必须早于该通道/波长的延时裁剪起点（E>=D 且 delayCut=true）
%
% 注意：上述配置允许不构成无边界误差或目标无损保证；已量化的起点/尾端
% 有限窗误差与实机 UNVERIFIED 限制不变。

if ~isstruct(channels) || isempty(channels)
    error('filterDbrConfigCheck:invalidInput', 'channels 必须为非空 struct 数组');
end

% ---- 基本参数合法性（沿用既定校验语义：非法即报错，不静默修复） ----
for k = 1:numel(channels)
    ch = channels(k);
    reqFields = {'highpassEnabled', 'lowpassEnabled', 'inversionEnabled', ...
        'dbrEnabled', 'maskLength', 'dbrmaskExtra', 'sysDelay', 'delayCut', 'Nt'};
    for rf = 1:numel(reqFields)
        if ~isfield(ch, reqFields{rf})
            error('filterDbrConfigCheck:missingField', ...
                '通道 %s 缺少字段 %s', chanName(ch), reqFields{rf});
        end
    end
    if ~islogical(ch.dbrEnabled) || ~islogical(ch.delayCut)
        error('filterDbrConfigCheck:invalidFlag', ...
            '通道 %s：dbrEnabled/delayCut 必须为逻辑值', chanName(ch));
    end
    if any(~islogical([ch.highpassEnabled, ch.lowpassEnabled, ch.inversionEnabled]))
        error('filterDbrConfigCheck:invalidFlag', ...
            '通道 %s：highpassEnabled/lowpassEnabled/inversionEnabled 必须为逻辑值', chanName(ch));
    end
    if ~isnumeric(ch.maskLength) || ch.maskLength < 0 || ch.maskLength ~= round(ch.maskLength)
        error('filterDbrConfigCheck:invalidMaskLength', ...
            '通道 %s：maskLength 必须为 >=0 整数', chanName(ch));
    end
    if ~isnumeric(ch.dbrmaskExtra) || ch.dbrmaskExtra < 0 || ch.dbrmaskExtra ~= round(ch.dbrmaskExtra)
        error('filterDbrConfigCheck:invalidExtra', ...
            '通道 %s：dbrmaskExtra 必须为 >=0 整数', chanName(ch));
    end
    if ~isnumeric(ch.sysDelay) || ch.sysDelay <= 0 || ch.sysDelay ~= round(ch.sysDelay)
        error('filterDbrConfigCheck:invalidSysDelay', ...
            '通道 %s：sysDelay 必须为正整数', chanName(ch));
    end
    if ~isnumeric(ch.Nt) || ch.Nt <= 0 || ch.Nt ~= round(ch.Nt)
        error('filterDbrConfigCheck:invalidNt', ...
            '通道 %s：Nt 必须为正整数', chanName(ch));
    end
end

% ---- 逐通道规则检查（一条不满足即拒绝本组） ----
rejects = struct('name', {}, 'reason', {}, 'message', {});
for k = 1:numel(channels)
    ch = channels(k);
    filterEnabled = ch.highpassEnabled || ch.lowpassEnabled;
    % E = 实际 DBR 置零样本数（真实预处理语义）
    if ~ch.dbrEnabled
        E = 0;                                        % DBR 关闭 → E=0（开关语义）
    else
        E = min(ch.maskLength + ch.dbrmaskExtra, ch.Nt);  % 计入 extra 与行尾边界
        if E <= 0, E = 0; end                          % 开启但有效长度 0 → E=0
    end
    if ~filterEnabled
        continue;                                     % 不触发零相位滤波专属拒绝
    end
    % filterEnabled=true 以下三支
    if E == 0
        continue;                                     % DBR 规则允许（采样率/截止/阶数/长度由 refZeroPhase 既有校验承担）
    end
    if ch.delayCut
        if E >= ch.sysDelay
            rejects(end+1) = struct('name', chanName(ch), ...
                'reason', 'DBR_END_NOT_BEFORE_DELAYCUT_START', ...
                'message', sprintf( ...
                ['DBR 置零末端必须早于该通道/波长的延时裁剪起点。', ...
                 '（%s：实际置零 E=%d >= sysDelay D=%d，且零相位滤波启用、delayCut=true）'], ...
                chanName(ch), E, ch.sysDelay)); %#ok<AGROW>
        end
    else
        rejects(end+1) = struct('name', chanName(ch), ...
            'reason', 'FILTER_UNCUT_DBR_UNSUPPORTED', ...
            'message', sprintf( ...
            ['零相位滤波与未裁剪的 DBR 置零前缀不兼容，请启用延时裁剪或取消 DBR 置零。', ...
             '（%s：filterEnabled=true、实际置零 E=%d>0、delayCut=false——该组合本版明确不支持，', ...
             '不是待阶段 B 决定项）'], ...
            chanName(ch), E)); %#ok<AGROW>
    end
end
accepted = isempty(rejects);
end

function s = chanName(ch)
if isfield(ch, 'name') && ischar(ch.name)
    s = ch.name;
elseif isfield(ch, 'name') && isstring(ch.name)
    s = char(ch.name);
else
    s = 'unnamed-channel';
end
end
