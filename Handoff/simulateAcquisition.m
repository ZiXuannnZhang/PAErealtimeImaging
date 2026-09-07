function [out, sim] = simulateAcquisition(action, sim, cfg)
% SIMULATEACQUISITION 数据模拟发送模块（模拟实际采集“每 AlinesPerBlock 根 A-line 打包上传”）
%
% 用法：
%   [~, sim]  = simulateAcquisition('init', [], cfg);   % 初始化
%   [blk, sim] = simulateAcquisition('next', sim);       % 取下一原始块
%   [~, sim]  = simulateAcquisition('close', sim);       % 关闭（释放文件句柄）
%
% cfg 字段：
%   ReadMode         : 'incremental'（按块 fseek 读文件，推荐，不持有整帧）
%                      / 'whole'（整帧切片对照）
%   testDataPath     : 数据文件路径
%   SampDepth        : 每条 A-line 采样点数
%   wlOffset         : 波长切分列偏移（14.dat=301，11.dat=151）
%   wl               : 1=532nm，2=1064nm；[1 2]=双波长同时输出
%   ShiftWL2         : 1=对 1064nm 做 circshift(+1,2) 列对齐（与主脚本一致，默认 0）
%   AlinesPerFrame   : 每帧（每圈）双波长合计 A-line 总数（必须为偶数，
%                      单波长数量 = AlinesPerFrame/2）
%   AlinesPerBlock   : 每次打包上传的双波长合计 A-line 总数（必须为偶数，
%                      单波长数量 = AlinesPerBlock/2；须能整除 AlinesPerFrame）
%
% 说明：
%   - 原始文件一列 = 一根 A-line，532/1064 按列交替存放；
%   - 单波长输出：[SampDepth x AlinesPerBlock/2]（本波长原始数据，未预处理）；
%     双波长输出：struct('wl1', [...], 'wl2', [...])，各为 [SampDepth x AlinesPerBlock/2]；
%   - 全部取完后 block=[] 且 sim.done=true；
%   - ShiftWL2=1 时，1064 通道按主脚本约定做 circshift(+1,2) 对齐。逐块实现为
%     “上一块末列前插 + 当前块去尾”，首块用本帧末列补 wrap（模拟可用；真实采集
%     首块缺末列时，可接受 1 列（约 0.09°）帧缝偏差，或改为延时一帧对齐）。

switch action
    case 'init'
        sim = struct();
        sim.ReadMode = cfg.ReadMode;
        sim.SampDepth = cfg.SampDepth;
        sim.wlOffset = cfg.wlOffset;
        sim.wl = cfg.wl(:)';
        sim.wlBoth = numel(sim.wl) == 2;
        if ~sim.wlBoth && ~any(sim.wl == [1 2])
            error('simulateAcquisition:WL', 'cfg.wl 只能为 1、2 或 [1 2]。');
        end
        if isfield(cfg, 'ShiftWL2')
            sim.ShiftWL2 = cfg.ShiftWL2;
        else
            sim.ShiftWL2 = 0;
        end

        % 双波长合计计数：单波长数量 = 合计/2
        sim.AlinesPerFrame = cfg.AlinesPerFrame;
        sim.AlinesPerBlock = cfg.AlinesPerBlock;
        if rem(sim.AlinesPerFrame, 2) || rem(sim.AlinesPerBlock, 2)
            error('simulateAcquisition:Even', ...
                'AlinesPerFrame/AlinesPerBlock 为双波长合计，必须为偶数。');
        end
        sim.nWlPerFrame = sim.AlinesPerFrame / 2;   % 每波长每帧 A-line 数
        sim.nWlPerBlock = sim.AlinesPerBlock / 2;   % 每波长每块 A-line 数
        sim.nBlocks = sim.AlinesPerFrame / sim.AlinesPerBlock;
        if sim.nBlocks ~= round(sim.nBlocks) || sim.nBlocks < 1
            error('simulateAcquisition:Blocks', ...
                'AlinesPerBlock 必须能整除 AlinesPerFrame。');
        end
        % 每块原始列数 = 每块总 A-line 数（一列一根 A-line，双波长交替）
        sim.RawColsPerBlock = sim.AlinesPerBlock;
        if isfield(cfg, 'RawColsPerBlock') && cfg.RawColsPerBlock ~= sim.AlinesPerBlock
            error('simulateAcquisition:RawCols', ...
                'RawColsPerBlock 已由 AlinesPerBlock 决定（每块一列一根 A-line），当前应为 %d。', ...
                sim.AlinesPerBlock);
        end

        sim.k = 0;
        sim.done = false;
        sim.fid = [];
        sim.channel1 = [];
        sim.channel2 = [];
        sim.prevWL2Col = [];   % 增量模式下 1064 列对齐的上一块原始末列

        if strcmpi(sim.ReadMode, 'incremental')
            sim.fid = fopen(cfg.testDataPath, 'r');
            if sim.fid < 0
                error('simulateAcquisition:Open', '无法打开数据文件：%s', cfg.testDataPath);
            end
        else
            fid = fopen(cfg.testDataPath, 'r');
            if fid < 0
                error('simulateAcquisition:Open', '无法打开数据文件：%s', cfg.testDataPath);
            end
            raw = fread(fid, Inf, 'double'); fclose(fid);
            data_m = reshape(raw, sim.SampDepth, []);
            sim.channel1 = data_m(:, sim.wlOffset : 2 : size(data_m,2));
            sim.channel2 = data_m(:, sim.wlOffset + 1 : 2 : size(data_m,2));
            if size(sim.channel1,2) ~= sim.nWlPerFrame || ...
                    size(sim.channel2,2) ~= sim.nWlPerFrame
                error('simulateAcquisition:Frame', ...
                    '单波长通道 A-line 数 %d/%d 与 AlinesPerFrame/2=%d 不一致。', ...
                    size(sim.channel1,2), size(sim.channel2,2), sim.nWlPerFrame);
            end
            if sim.ShiftWL2
                sim.channel2 = circshift(sim.channel2, 1, 2);
            end
        end
        out = [];

    case 'next'
        sim.k = sim.k + 1;
        if sim.k > sim.nBlocks
            sim.done = true;
            out = [];
            return;
        end
        if strcmpi(sim.ReadMode, 'incremental')
            % 每块读 AlinesPerBlock 列（= 双波长合计 A-line 数），再按奇偶列解交织
            colStart = sim.wlOffset + (sim.k-1)*sim.AlinesPerBlock;
            fseek(sim.fid, (colStart-1)*sim.SampDepth*8, 'bof');
            blockRaw = fread(sim.fid, sim.SampDepth*sim.RawColsPerBlock, 'double');
            blockRaw = reshape(blockRaw, sim.SampDepth, sim.RawColsPerBlock);
            wl1 = blockRaw(:, 1:2:end);   % 每波长 AlinesPerBlock/2 根
            wl2 = blockRaw(:, 2:2:end);

            % 1064 列对齐：上一块原始末列（首块用本帧末列）前插，去当前块末列
            if sim.ShiftWL2
                rawLastWL2 = wl2(:, end);   % 先保存原始末列供下一块使用
                if sim.k == 1
                    lastCol = sim.wlOffset + sim.AlinesPerFrame - 1;
                    fseek(sim.fid, (lastCol-1)*sim.SampDepth*8, 'bof');
                    prevCol = fread(sim.fid, sim.SampDepth, 'double');
                else
                    prevCol = sim.prevWL2Col;
                end
                wl2 = [prevCol, wl2(:, 1:end-1)];
                sim.prevWL2Col = rawLastWL2;
            end

            if sim.wlBoth
                out = struct('wl1', wl1, 'wl2', wl2);
            elseif sim.wl == 1
                out = wl1;
            else
                out = wl2;
            end
        else
            cols = (sim.k-1)*sim.nWlPerBlock+1 : sim.k*sim.nWlPerBlock;
            wl1 = sim.channel1(:, cols);
            wl2 = sim.channel2(:, cols);
            if sim.wlBoth
                out = struct('wl1', wl1, 'wl2', wl2);
            elseif sim.wl == 1
                out = wl1;
            else
                out = wl2;
            end
        end

    case 'close'
        if ~isempty(sim) && ~isempty(sim.fid) && sim.fid > 0
            fclose(sim.fid);
            sim.fid = [];
        end
        out = [];

    otherwise
        error('simulateAcquisition:Action', '未知动作：%s', action);
end
end
