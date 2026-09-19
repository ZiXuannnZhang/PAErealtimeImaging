% RUN_ALL_REMEDIATION  审查整改统一复跑入口（追加任务 §7.5/§10）。
%
% 运行顺序：R1/R2 自动测试 → exp1..exp6。任一脚本报错或任一关键断言失败，
% 最终 error() → matlab -batch 非零退出。
% 未在本轮修改的 exp1/exp4/exp6 也一并重跑，其 JSON pass 字段由此入口复核
% （exp1 无 pass 字段，按其判定阈值复核：球内 C=1/Ω₀ 变体误差 <1e-5、
%   半常数变体偏差 >0.3）。
%
% 运行：matlab -batch "run_all_remediation"   （工作目录 = matlab/）

here = fileparts(mfilename('fullpath'));
evd = fullfile(here, '..', 'evidence');

items = { ...
    'test_time_derivative.m', 'R1 导数自动测试'; ...
    'test_fair_comparison.m', 'R2 公平对照自动测试'; ...
    'test_boundary_reference.m', '二轮B1 边界参考结构/收敛/负例测试'; ...
    'exp1_ubp_sphere.m',      'exp1 UBP 常数（未改码，重跑复核）'; ...
    'exp2_ring_ubp_2d.m',     'exp2 修复后完整重跑'; ...
    'exp3_time_axis.m',       'exp3 时间轴 + 两态反演（重跑）'; ...
    'exp4_dual_layer.m',      'exp4 分层走时（未改码，重跑复核）'; ...
    'exp5_order_endpoints.m', 'exp5 顺序/边界策略（重跑）'; ...
    'exp6_filter_reference.m','exp6 滤波参考（未改码，重跑复核）'};

fail = {};
% 注意：run() 在本工作区执行脚本，实验脚本内的循环变量（如 i/j/k）会污染
% 本脚本的同名变量——循环变量一律使用实验脚本不会用到的 itemIdx，
% 且 name/label 在 run() 之前捕获。
for itemIdx = 1:size(items, 1)
    name = items{itemIdx, 1};
    lbl = items{itemIdx, 2};
    t0 = tic;
    try
        run(fullfile(here, name)); %#ok<RUNRES>
        el = toc(t0);
        fprintf('[RUN_ALL] %s (%s) OK，耗时 %.1fs\n', name, lbl, el);
    catch err
        fail{end+1} = sprintf('%s: %s', name, err.message); %#ok<AGROW>
        fprintf('[RUN_ALL] %s (%s) FAILED: %s\n', name, lbl, err.message);
    end
end

% ---- 证据 JSON 断言复核（含无内建门的 exp1/exp4/exp6） ----
j1 = fullfile(evd, 'exp1_ubp_sphere.json');
if exist(j1, 'file')
    d1 = jsondecode(fileread(j1));
    if ~(d1.interiorMaxErr_C1 < 1e-5 && d1.interiorMaxErr_Chalf > 0.3)
        fail{end+1} = 'exp1_ubp_sphere.json: 判定阈值未满足'; %#ok<AGROW>
    end
else
    fail{end+1} = 'exp1_ubp_sphere.json 不存在'; %#ok<AGROW>
end

for nm = {'exp4_dual_layer', 'exp6_filter_reference'}
    jf = fullfile(evd, sprintf('%s.json', nm{1}));
    if exist(jf, 'file')
        dj = jsondecode(fileread(jf));
        if ~isfield(dj, 'pass') || ~allPass(dj.pass)
            fail{end+1} = sprintf('%s.json: pass 字段未全部为真', nm{1}); %#ok<AGROW>
        end
    else
        fail{end+1} = sprintf('%s.json 不存在', nm{1}); %#ok<AGROW>
    end
end

fprintf('\n[RUN_ALL] 汇总：%d 项失败\n', numel(fail));
for k = 1:numel(fail)
    fprintf('[RUN_ALL] FAIL %d: %s\n', k, fail{k});
end
if ~isempty(fail)
    error('run_all_remediation:failed', ...
        '整改复跑存在失败项（共 %d），见上方清单；运行非零退出', numel(fail));
end
fprintf('[RUN_ALL] 全部通过\n');

function ok = allPass(p)
% 递归检查 pass 结构全部字段为真（逻辑真或非零数值）
ok = true;
fn = fieldnames(p);
for k = 1:numel(fn)
    v = p.(fn{k});
    if isstruct(v)
        ok = ok && allPass(v);
    elseif islogical(v)
        ok = ok && all(v(:));
    elseif isnumeric(v)
        ok = ok && all(v(:) ~= 0);
    end
end
end
