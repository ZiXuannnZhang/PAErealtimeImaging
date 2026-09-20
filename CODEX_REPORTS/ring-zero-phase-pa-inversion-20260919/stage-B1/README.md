# Stage B1 — 环形成像高低通零相位滤波生产实现

任务：`TASKS/阶段B1环形成像高低通零相位滤波实现_20260920-144420.md`
起点：阶段 A 已验收 `91ca68b36dac9c14ccd756c52896ea20ad794e0a`
执行回执：[stage-B1-report.md](stage-B1-report.md)

## 目录

- `stage-B1-report.md` — B1 执行/验证/交付报告（数值证据、测试、性能四态、GUI 范围、未验证项）
- `matlab/export_zpf_reference.m` — 阶段 A 参考向量 → C++ 测试紧凑文本导出脚本
  （MATLAB R2023a + Signal Processing Toolbox；非运行时依赖）

## C++ 测试参考向量（再生成）

`tests/ring_zero_phase_filter_test` 通过 `--ref` 读取
`matlab/reference_vectors/`（43 个 %.17g 文本向量）。该目录不入场（可再生成），
克隆后首次运行前执行：

```matlab
% 在本仓库检出内运行（要求：阶段 A evidence/filter_reference_vectors.mat 已在场）
matlab -batch "run('CODEX_REPORTS/ring-zero-phase-pa-inversion-20260919/stage-B1/matlab/export_zpf_reference.m')"
```

目录缺失时测试自动降级为内部一致性断言并打印 NOTE，不静默冒充参考对照。
