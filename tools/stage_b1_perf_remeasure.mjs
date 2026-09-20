#!/usr/bin/env node
// stage-B1 收口 S2：性能复测（带每次运行原始记录）。
//
// 复现 stage-B1-report §5.3 的统一口径（同 14.dat、同 build_mingw_debug
// 构建、同 avg_process_us 边界），每配置 1 次预热 + 3 次正式运行，逐次写
// run-record.json + 汇总 CSV/JSON（任务要求：汇总数无原始记录时做必要
// 小范围复测）。输出：stage-B1/perf-remeasure/。
import { execFileSync } from 'node:child_process';
import { createHash } from 'node:crypto';
import { mkdirSync, writeFileSync, appendFileSync, readFileSync } from 'node:fs';
import { join, dirname, resolve } from 'node:path';
import { fileURLToPath } from 'node:url';

const HERE = dirname(fileURLToPath(import.meta.url));           // <repo>/tools
const REPO = resolve(HERE, '..');                                // repo root
const DELIVERY = join(REPO, 'MC_410T_MultiCard', 'delivery');
const SELFTEST = join(DELIVERY, 'build', 'mingw_debug', 'bin', 'ring_svc_selftest.exe');
const SVC = join(DELIVERY, 'build', 'mingw_debug', 'bin', 'ImagingSvc.exe');
const DATA = 'D:/ChatGPT/PAERealtimeImaging/testdata/14.dat';
const OUT = join(REPO, 'CODEX_REPORTS', 'ring-zero-phase-pa-inversion-20260919', 'stage-B1', 'perf-remeasure');

// 与 §5.3 表一致的配置矩阵 + 部署默认网格
const CASES = [
  { label: 'block40_off',       block: 40,  zp: 0, gridMm: 0.4 },
  { label: 'block40_hp',        block: 40,  zp: 1, gridMm: 0.4 },
  { label: 'block40_lp',        block: 40,  zp: 2, gridMm: 0.4 },
  { label: 'block40_hplp',      block: 40,  zp: 3, gridMm: 0.4 },
  { label: 'block100_hplp',     block: 100, zp: 3, gridMm: 0.4 },
  { label: 'deploy_grid_hplp',  block: 40,  zp: 3, gridMm: 0.01 },
  { label: 'deploy_grid_off',   block: 40,  zp: 0, gridMm: 0.01 },
];

function sha256(path) {
  const h = createHash('sha256');
  h.update(readFileSync(path));
  return h.digest('hex');
}

function runOnce(kase, runKind, runIdx, gitSha) {
  const outDir = join(OUT, 'runs', `${kase.label}_${runKind}${runIdx}`);
  mkdirSync(outDir, { recursive: true });
  const args = ['--data', DATA, '--svc', SVC, '--id', '14',
                '--grid-mm', String(kase.gridMm), '--block', String(kase.block),
                '--zp', String(kase.zp), '--out', outDir];
  const t0 = new Date();
  let stdout = '', stderr = '', code = 0;
  try {
    stdout = execFileSync(SELFTEST, args, { encoding: 'utf8', timeout: 1800000,
                                            stdio: ['ignore', 'pipe', 'pipe'] });
  } catch (e) {
    code = e.status ?? 1;
    stdout = (e.stdout || '').toString();
    stderr = (e.stderr || '').toString().slice(-400);
  }
  const t1 = new Date();
  const avg = /avg_process_us=([0-9.]+)/.exec(stdout);
  const total = /total=([0-9.]+) ms/.exec(stdout);
  const record = {
    case: kase.label, runKind, runIndex: runIdx, exitCode: code,
    avg_process_us: avg ? Number(avg[1]) : null,
    wallTotalMs: total ? Number(total[1]) : null,
    startedAt: t0.toISOString(), finishedAt: t1.toISOString(),
    cmd: [SELFTEST, ...args], stderrTail: stderr,
  };
  writeFileSync(join(outDir, 'run-record.json'),
                JSON.stringify(record, null, 2), 'utf8');
  return record;
}

function git(args) {
  return execFileSync('git', args, { cwd: REPO, encoding: 'utf8' }).trim();
}

// ── main ──
mkdirSync(OUT, { recursive: true });
const gitSha = git(['rev-parse', 'HEAD']);
let trackedDirty = '';
try { trackedDirty = git(['status', '--porcelain', '--untracked-files=no']); } catch {}
const dataSha = sha256(DATA);

const formal = new Map();   // label -> [avgUs...]
const csvRows = [[
  'case','block','zpMode','gridMm','runKind','runIndex','exitCode',
  'avg_process_us','wallTotalMs','startedAt']];

for (const kase of CASES) {
  const w = runOnce(kase, 'warmup', 1, gitSha);
  console.log(`${kase.label} warmup: exit=${w.exitCode} avg=${w.avg_process_us}`);
  formal.set(kase.label, []);
  for (const i of [1, 2, 3]) {
    const r = runOnce(kase, 'formal', i, gitSha);
    console.log(`${kase.label} formal${i}: exit=${r.exitCode} avg=${r.avg_process_us}`);
    if (r.runKind === 'formal' && r.avg_process_us != null)
      formal.get(kase.label).push(r.avg_process_us);
    csvRows.push([kase.label, kase.block, kase.zp, kase.gridMm, r.runKind,
                  r.runIndex, r.exitCode, r.avg_process_us ?? '', r.wallTotalMs ?? '', r.startedAt]);
  }
}

// 中位数汇总（ms）
const median = (a) => { const s = [...a].sort((x, y) => x - y);
  const m = Math.floor(s.length / 2);
  return s.length % 2 ? s[m] : (s[m - 1] + s[m]) / 2; };
const casesSummary = {};
for (const kase of CASES) {
  const vals = formal.get(kase.label) || [];
  casesSummary[kase.label] = {
    block: kase.block, zpMode: kase.zp, gridMm: kase.gridMm,
    formalRuns: vals.length,
    medianMs: vals.length ? +(median(vals) / 1000).toFixed(2) : null,
    maxMs: vals.length ? +(Math.max(...vals) / 1000).toFixed(2) : null,
  };
  if (vals.length)
    csvRows.push([`MEDIAN:${kase.label}`, '', '', '', '', '', vals.length,
                  +(median(vals) / 1000).toFixed(2), '', '']);
}
writeFileSync(join(OUT, 'perf-remeasure-runs.csv'),
              csvRows.map(r => r.join(',')).join('\r\n') + '\r\n', 'utf8');
const summary = {
  generatedAt: new Date().toISOString(),
  gitSha, trackedDirty: trackedDirty.length > 0,
  buildType: 'mingw-debug (-g, unoptimized)',
  dataFile: DATA, dataSha256: dataSha,
  timingBoundary: 'ImagingSvc::processRingPulse entry→recordProcessDuration ' +
    '(SHM copy + per-line preprocessing incl. zero-phase filter + CUDA recon ' +
    'submit + display snapshot writeback + ZMQ notify)',
  sampDepth: 4000, fsHz: 2.5e8, channelsEnabled: 8,
  warmupsPerCase: 1, formalRunsPerCase: 3,
  cases: casesSummary,
};
writeFileSync(join(OUT, 'perf-remeasure-summary.json'),
              JSON.stringify(summary, null, 2), 'utf8');
console.log(JSON.stringify(casesSummary, null, 2));
