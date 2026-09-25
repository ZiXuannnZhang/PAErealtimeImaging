// D3 — 环外权重行为表征 (out-of-ring weight behaviour)
//
// Pure geometry/numeric analysis. No GPU, no real data, no production code touched.
// Reproduces, from the repo defaults, exactly the weight geometry implemented in
// src/RingRecon/ring_recon_cuda.cu and then characterises its spatial behaviour.
//
// Weight actually implemented (ring_recon_cuda.cu:337-345, :615-648):
//     wscale[j] = -arc * apod
//     dotp      = r.r_s - R^2
//     dist2     = |r-r_s|^2          (algebraically; see verifyIdentity)
//     dsafe     = max(dist, minDist)
//     wBase     = wscale[j] * dotp / (R * dsafe^(wExponent+1))
//   => w = dTheta*apod*(R^2 - r.r_s)/(R*d^2)
//      with n_hat = r_s/R and cos(alpha) = (R - r.n_hat)/d
//      => w  = dTheta*apod*cos(alpha)/d              [existing DAS weight]
//      => dO = R*dTheta*apod*cos(alpha)/d^2          [UBP solid-angle weight]
//      => dO/w = R/d                                 [premise E1]
//
// Run: node d3_weight_behavior.mjs
import fs from 'node:fs';
import path from 'node:path';
import { fileURLToPath } from 'node:url';

// ---- defaults, ring_recon_cuda_set_defaults (ring_recon_cuda.cu:73-151) ----
const CFG = {
  radius: 6.57e-3,                 // m
  fov: 36e-3,                      // m
  gridSize: 0.01e-3,               // m
  distanceWeightExponent: 1.0,
  apodType: 0,                     // none => apod = 1.0
  minDistance: 0.0,                // 0.0 => auto = gridSize
  maskOutOfRange: 1,
  interpolation: 0,                // linear
  enabledChannelCount: 8,
  alinesPerFrame: 8000,
  alinesPerChannelPerFrame: 500,
  sectorStartDeg: 180.0,
  sectorCcw: 1,
  spliceBlendDeg: 1.5,
};
// MainWindow.cpp:5368-5369 (production assembler configure)
const sectorWidthDeg = 360.0 / CFG.enabledChannelCount;        // 45 deg
const stepDeg = sectorWidthDeg / CFG.alinesPerChannelPerFrame; // 0.09 deg
const apod = 1.0;                                              // apodType = 0

const R = CFG.radius;
const fov = CFG.fov;
const gs = CFG.gridSize;
const nx = Math.ceil(fov / gs);                 // 3600
const minDist = CFG.minDistance > 0 ? CFG.minDistance : gs;   // 10 um

// Detector set for ONE wavelength over one full revolution:
// angle = sectorStartDeg + s*sectorWidthDeg + k*stepDeg, s=0..M-1, k=0..K-1
// (RingBlockAssembler.cpp:373-374). This is a uniform ring of M*K = 4000 points.
const M = CFG.enabledChannelCount;
const K = CFG.alinesPerChannelPerFrame;
const ND = M * K;                               // 4000
const dTheta = stepDeg * Math.PI / 180.0;       // rad per A-line (uniform)

const out = [];
const P = (...a) => out.push(a.join(' '));
const F = (v, n = 6) => (typeof v === 'number' ? v.toFixed(n) : String(v));

P('==================================================================');
P('D3  环外权重行为表征 / out-of-ring weight behaviour');
P('==================================================================');
P('');
P('--- 0. 输入与几何定义（来源）-------------------------------------');
P(`radius              R        = ${F(R, 8)} m        (ring_recon_cuda_set_defaults)`);
P(`fov                        = ${F(fov, 8)} m`);
P(`gridSize                   = ${F(gs, 8)} m`);
P(`grid                  nx=ny= ${nx}            ceil(fov/gridSize)`);
P(`grid coord          x_i      = -fov/2 + (i+0.5)*gridSize`);
P(`distanceWeightExponent      = ${CFG.distanceWeightExponent}`);
P(`apodType                    = ${CFG.apodType}  => apod = ${apod}`);
P(`minDistance (raw)           = ${CFG.minDistance}  => effective minDist = ${F(minDist, 8)} m = ${(minDist * 1e6).toFixed(1)} um`);
P(`enabledChannelCount    M    = ${M}`);
P(`alinesPerChannelPerFrame K  = ${K}`);
P(`sectorStartDeg              = ${CFG.sectorStartDeg}`);
P(`sectorWidthDeg  360/M       = ${F(sectorWidthDeg, 6)}   (MainWindow.cpp:5368)`);
P(`stepDeg          width/K    = ${F(stepDeg, 8)}   (MainWindow.cpp:5369)`);
P(`A-lines per wavelength      = M*K = ${ND}   over 360 deg`);
P(`dTheta (uniform)            = ${F(stepDeg, 8)} deg = ${F(dTheta, 10)} rad = 2pi/${F(2 * Math.PI / dTheta, 3)}`);
P('');
P('【E3 提示】MATLAB 主脚本 实时重建脚本/Ringscan_DAS_loop_realtime_dual.m 对');
P('14.dat 记 DAQ=200e6、Radius 14.dat=6.48e-3，而本仓默认 daqHz=250e6、radius=6.57e-3。');
P('本任务一律以本仓默认值为基准（前提 E3），差异在此显式标注，不混用。');
P('');

// ------------------------------------------------------------------
// 1. cos(alpha) = 0 的几何轨迹 = 变号边界
// ------------------------------------------------------------------
P('--- 1. 变号边界 (cos(alpha)=0) -----------------------------------');
P('定义  n_hat = r_s/R ,  cos(alpha) = (R - r.n_hat)/d ,  d = |r - r_s|');
P('cos(alpha) = 0  <=>  r.n_hat = R');
P('对固定探测器 r_s：这是一条过 r_s、垂直于 n_hat 的直线，即环在 r_s 处的切线。');
P('半平面 r.n_hat <= R 包含环心与整个探测环，该侧 cos(alpha) > 0；');
P('另一侧 r.n_hat > R，cos(alpha) < 0（权重变号）。');
P('');
P('对像素 (rho=|r|, theta_r) 与探测器角 theta_s：');
P('  cos(alpha) < 0  <=>  rho*cos(theta_r - theta_s) > R');
P('  必要条件 rho > R；此时 |theta_r - theta_s| < acos(R/rho)');
P('  该像素被“变号探测器”命中的角占比 = acos(R/rho)/pi');
P('');
const rhoCrit = R;
P(`临界半径 rho = R = ${F(R * 1e3, 4)} mm ；FOV 为边长 ${F(fov * 1e3, 2)} mm 的正方形，`);
P(`其内切圆半径 ${F(fov / 2 * 1e3, 2)} mm、外接圆半径 ${F(Math.SQRT2 * fov / 2 * 1e3, 4)} mm。`);
P('');

// ------------------------------------------------------------------
// 2. 变号像素占比 —— 逐像素统计（在 3600^2 网格上解析计数，按径向分档）
// ------------------------------------------------------------------
P('--- 2. 变号像素占比 (FOV 内 cos(alpha)<0) -----------------------');
P('口径：对每个像素 r，统计 4000 个探测器中满足 cos(alpha)<0 的个数，');
P('“变号像素”= 至少被 1 个探测器以负权重命中的像素（等价于 rho > R）。');
P('分档：按 |r| 落入 [lo,hi) mm 统计像素数与占比。');
P('');

// Analytic: rho > R  <=>  pixel outside the circle of radius R.
// Count pixels with |r| > R in the grid, and also per radial band.
function bandOf(rho) { return Math.min(Math.floor(rho * 1e3 / 2), 17); } // 2mm bands to 34mm
const NB = 18;
const bandTotal = new Array(NB).fill(0);
const bandNeg = new Array(NB).fill(0);
const bandNegDetectorFracAcc = new Array(NB).fill(0); // mean over pixels of acos(R/rho)/pi
let totalPx = 0, negPx = 0;
let sumNegDetFrac = 0;

for (let iy = 0; iy < nx; iy++) {
  const y = -fov / 2 + (iy + 0.5) * gs;
  for (let ix = 0; ix < nx; ix++) {
    const x = -fov / 2 + (ix + 0.5) * gs;
    const rho = Math.hypot(x, y);
    const b = bandOf(rho);
    bandTotal[b]++;
    totalPx++;
    if (rho > R) {
      negPx++;
      bandNeg[b]++;
      const frac = Math.acos(R / rho) / Math.PI;
      bandNegDetectorFracAcc[b] += frac;
      sumNegDetFrac += frac;
    }
  }
}
P(`总像素数                 = ${totalPx}  (${nx} x ${nx})`);
P(`rho > R 的像素数         = ${negPx}`);
P(`占比                     = ${F(100 * negPx / totalPx, 4)} %`);
P(`rho <= R 的像素数        = ${totalPx - negPx}  (${F(100 * (totalPx - negPx) / totalPx, 4)} %)`);
P('');
P('| 径向分档 [mm) | 像素数 | rho>R 像素数 | 该档变号占比% | 该档“变号探测器”平均角占比 |');
for (let b = 0; b < NB; b++) {
  if (bandTotal[b] === 0) continue;
  const avgF = bandNeg[b] > 0 ? bandNegDetectorFracAcc[b] / bandNeg[b] : 0;
  P(`| [${String(b * 2).padStart(2)},${String(b * 2 + 2).padStart(2)}) | ${String(bandTotal[b]).padStart(8)} | ${String(bandNeg[b]).padStart(8)} | ${F(100 * bandNeg[b] / bandTotal[b], 4).padStart(12)} | ${F(avgF, 6).padStart(24)} |`);
}
P('');
P('全 FOV 平均“变号探测器”角占比（仅对 rho>R 像素平均）= ' + F(sumNegDetFrac / Math.max(negPx, 1), 6));
P('');

// ------------------------------------------------------------------
// 3. 权重空间行为 + R/d 比值验证
// ------------------------------------------------------------------
P('--- 3. 权重数值与 R/d 比值验证 ----------------------------------');
P('沿代表性射线取点（射线方向 theta_r = 0），探测器取 theta_s = 0（正对射线）');
P('与 theta_s = 180 deg（背对射线）。');
P('');

function weights(rho, thetaR, thetaS) {
  const x = rho * Math.cos(thetaR), y = rho * Math.sin(thetaR);
  const xs = R * Math.cos(thetaS), ys = R * Math.sin(thetaS);
  const dotp = x * xs + y * ys - R * R;
  const dist2 = (x * x + y * y - R * R) - 2 * dotp;
  const dist = Math.sqrt(Math.max(dist2, 0));
  const dsafe = Math.max(dist, minDist);
  const pw = CFG.distanceWeightExponent + 1.0;
  const dsafeP = pw === 2 ? dsafe * dsafe : pw === 1 ? dsafe : Math.pow(dsafe, pw);
  // exact implemented form (with the clamped dsafe, as in the CUDA kernel)
  const wImpl = (-dTheta * apod) * dotp / (R * dsafeP);
  // closed forms using the true d (not clamped) — used for the R/d identity check
  const nHatx = xs / R, nHaty = ys / R;
  const cosAlpha = (R - (x * nHatx + y * nHaty)) / (dist || 1e-300);
  const wClosed = dTheta * apod * cosAlpha / dist;
  const dOmega = R * dTheta * apod * cosAlpha / (dist * dist);
  return { dotp, dist, dsafe, wImpl, wClosed, dOmega, cosAlpha };
}

P('射线 theta_r=0，探测器 theta_s=0（cos(alpha) 恒为正）：');
P('| rho[mm] | d[mm] | cos(alpha) | w_现有 | dOmega | dOmega/w | R/d |');
const rayRhos = [0, 1, 2, 3, 4, 5, 6, 6.569, 6.571, 7, 8, 10, 12, 15, 18, 20, 24, 25.4558];
for (const mm of rayRhos) {
  const rho = mm * 1e-3;
  const w = weights(rho, 0, 0);
  const ratio = w.dist > 0 ? w.dOmega / w.wClosed : NaN;
  P(`| ${F(mm, 3).padStart(7)} | ${F(w.dist * 1e3, 5).padStart(7)} | ${F(w.cosAlpha, 6).padStart(10)} | ${F(w.wClosed, 8).padStart(10)} | ${F(w.dOmega, 8).padStart(10)} | ${F(ratio, 8).padStart(9)} | ${F(R / w.dist, 8).padStart(8)} |`);
}
P('注：rho=6.570 恰为探测器位置（d=0），已用 6.569 / 6.571 两个邻点代替展示奇异两侧。');
P('');
P('射线 theta_r=0，探测器 theta_s=180 deg（穿过环心，环外 cos(alpha) 变号）：');
P('| rho[mm] | d[mm] | cos(alpha) | w_现有 | dOmega | dOmega/w | R/d |');
for (const mm of rayRhos) {
  const rho = mm * 1e-3;
  const w = weights(rho, 0, Math.PI);
  const ratio = w.dist > 0 ? w.dOmega / w.wClosed : NaN;
  P(`| ${F(mm, 3).padStart(7)} | ${F(w.dist * 1e3, 5).padStart(7)} | ${F(w.cosAlpha, 6).padStart(10)} | ${F(w.wClosed, 8).padStart(10)} | ${F(w.dOmega, 8).padStart(10)} | ${F(ratio, 8).padStart(9)} | ${F(R / w.dist, 8).padStart(8)} |`);
}
P('');

// global R/d identity verification over a random pixel/detector sample
let maxRelErr = 0, worst = null, nCheck = 0;
for (let t = 0; t < 4000; t++) {
  const rho = Math.random() * Math.SQRT2 * fov / 2;
  const th = Math.random() * 2 * Math.PI;
  const ths = Math.random() * 2 * Math.PI;
  const w = weights(rho, th, ths);
  if (w.dist < 1e-9) continue;
  const err = Math.abs(w.dOmega / w.wClosed - R / w.dist) / (R / w.dist);
  nCheck++;
  if (err > maxRelErr) { maxRelErr = err; worst = { rho, th, ths, d: w.dist }; }
}
P(`R/d 比值验证：随机采样 ${nCheck} 组 (像素, 探测器)`);
P(`  max |dOmega/w - R/d| / (R/d) = ${maxRelErr.toExponential(3)}`);
P(`  结论：恒等式 dOmega = (R/d) * w_现有 在数值精度内成立（前提 E1 得到独立复核）。`);
P('');

// algebraic identity check on the implemented (clamped) form
P('实现形态核对（含 minDist 钳位）：wImpl = (-dTheta*apod)*dotp/(R*dsafe^2)');
P('  代数展开 dotp = r.r_s - R^2 , dist2 = |r|^2 - R^2 - 2*dotp = |r - r_s|^2');
P('  => wImpl = dTheta*apod*(R - r.n_hat)/dsafe^2 = dTheta*apod*cos(alpha_true)*d/dsafe^2');
P('  当 d >= minDist 时 dsafe == d，与闭式 w = dTheta*apod*cos(alpha)/d 完全一致。');
{
  let maxErr = 0;
  for (let t = 0; t < 2000; t++) {
    const rho = Math.random() * Math.SQRT2 * fov / 2;
    const th = Math.random() * 2 * Math.PI, ths = Math.random() * 2 * Math.PI;
    const w = weights(rho, th, ths);
    if (w.dist < minDist || w.dist < 1e-12) continue;
    const e = Math.abs(w.wImpl - w.wClosed);
    if (e > maxErr) maxErr = e;
  }
  P(`  d >= minDist 样本上 max|wImpl - wClosed| = ${maxErr.toExponential(3)} (float64 舍入级)`);
}
P('');

// ------------------------------------------------------------------
// 4. minDistance 保护的实际覆盖
// ------------------------------------------------------------------
P('--- 4. minDistance 保护的实际覆盖 -------------------------------');
P(`minDist = ${F(minDist * 1e6, 1)} um (自动 = gridSize)。`);
P('考察网格像素到 4000 个探测器的最小距离 d_min，以及钳位是否生效。');
P('');
// For each pixel, find min distance to any detector. Full 3600^2 x 4000 is too big;
// exploit the near-uniform detector ring: nearest detector is the one whose angle is
// closest to the pixel angle. Verify on a subset that this holds.
function nearestDetectorDist(x, y) {
  const rho = Math.hypot(x, y);
  const th = Math.atan2(y, x);
  // detector angles: sectorStart + s*sectorWidth + k*step  ==  sectorStart + n*step, n=0..ND-1
  // (uniform ring, see header). Map to nearest n.
  let a = (th - CFG.sectorStartDeg * Math.PI / 180.0) / dTheta;
  const n0 = Math.round(a);
  let best = Infinity;
  for (const n of [n0 - 1, n0, n0 + 1]) {
    const ths = (CFG.sectorStartDeg * Math.PI / 180.0) + ((n % ND) + ND) % ND * dTheta;
    const xs = R * Math.cos(ths), ys = R * Math.sin(ths);
    const d = Math.hypot(x - xs, y - ys);
    if (d < best) best = d;
  }
  return best;
}
// verify the nearest-angle shortcut against brute force on random pixels
{
  let bad = 0, worstRatio = 1;
  for (let t = 0; t < 200; t++) {
    const x = (Math.random() - 0.5) * fov, y = (Math.random() - 0.5) * fov;
    const dFast = nearestDetectorDist(x, y);
    let dBrute = Infinity;
    for (let n = 0; n < ND; n++) {
      const ths = CFG.sectorStartDeg * Math.PI / 180.0 + n * dTheta;
      const d = Math.hypot(x - R * Math.cos(ths), y - R * Math.sin(ths));
      if (d < dBrute) dBrute = d;
    }
    const ratio = dFast / dBrute;
    if (ratio > worstRatio) worstRatio = ratio;
    if (Math.abs(dFast - dBrute) > 1e-12) bad++;
  }
  P(`最近探测器快捷算法核对（200 随机像素 vs 暴力 4000 探测器）：不一致 ${bad} 个，最大比值 ${F(worstRatio, 12)}`);
}
// scan the grid for the global min d and the count of clamped pixels
let dMinGlobal = Infinity, clampedPx = 0, pxWithin = new Array(21).fill(0);
const probeStep = 1;  // full grid
for (let iy = 0; iy < nx; iy += probeStep) {
  const y = -fov / 2 + (iy + 0.5) * gs;
  for (let ix = 0; ix < nx; ix += probeStep) {
    const x = -fov / 2 + (ix + 0.5) * gs;
    const d = nearestDetectorDist(x, y);
    if (d < dMinGlobal) dMinGlobal = d;
    if (d <= minDist) clampedPx++;
    const idx = Math.min(20, Math.floor(d * 1e6 / 1));  // 1 um buckets to 20 um
    pxWithin[idx]++;
  }
}
P(`网格像素到探测器的全局最小距离 d_min = ${F(dMinGlobal * 1e6, 6)} um`);
P(`d <= minDist(${F(minDist * 1e6, 1)} um) 的像素数 = ${clampedPx} (${F(100 * clampedPx / totalPx, 6)} %)`);
P('| d 区间 [um) | 像素数 |');
for (let i = 0; i <= 20; i++) P(`| [${i},${i + 1}) | ${pxWithin[i]} |`);
P('');
P('结论（minDistance）：');
P('  环半径 6.57 mm 与 minDist=10 um 无关——minDist 约束的是“像素到探测器”的距离 d，');
P('  而 d 可以趋近 0（网格步长 10 um，探测器共 4000 个，网格 3600^2）。');
P(`  实测 d_min = ${F(dMinGlobal * 1e6, 4)} um，${clampedPx} 个像素的 d 落在钳位半径内。`);
P('  因此 minDistance 在本几何下【会生效】，能阻止 d->0 的严格奇异，');
P('  但它只是把奇点截断在 10 um，权重幅值仍被放大到 ~dTheta/minDist 量级；');
P('  对 UBP 形态（1/d^2）放大更剧烈（~R*dTheta/minDist^2）。它【不是】平滑窗，');
P('  只是硬钳位，会在环上产生局部长条状高权重。');
P('');
P('权重幅值放大定量（apod=1，取 |cos(alpha)|=1 的最坏方向）：');
P(`  |w_现有|_max  ~ dTheta/minDist            = ${F(dTheta / minDist, 4)}   (参考：环心处 |w| = dTheta/R = ${F(dTheta / R, 6)})`);
P(`  |dOmega|_max  ~ R*dTheta/minDist^2        = ${F(R * dTheta / (minDist * minDist), 4)}   (参考：环心处 |dOmega| = ${F(dTheta / R, 6)})`);
P(`  放大倍数（相对环心）= R/minDist            = ${F(R / minDist, 2)}  ×  （w_现有）`);
P(`  放大倍数（相对环心）= (R/minDist)^2        = ${F((R / minDist) * (R / minDist), 2)}  ×  （UBP dOmega 形态）`);
P(`  即 minDist=10 um 让环上局部 |w| 比环心高约 ${F(R / minDist, 0)} 倍；UBP 形态（1/d^2）则高约 ${F((R / minDist) * (R / minDist), 0)} 倍。`);
P('');

// ------------------------------------------------------------------
// 5. 环外符号处理候选（仅结构性后果，不做优劣预判）
// ------------------------------------------------------------------
P('--- 5. 环外符号处理候选（仅结构性后果，不预判优劣）-------------');
P('| 候选 | 操作 | 对 R1 双波长比值守卫 | 对 R3 线性性 | 其它结构性后果 |');
P('| --- | --- | --- | --- | --- |');
P('| A | 保持符号累加（acc += w*v，accW += w） | 安全：w 与波长无关，两波长同除 accW | 保持线性 | accW 可能接近 0（正负抵消），归一化失去意义；环外扇区权重相互抵消 |');
P('| B | 取绝对值累加（acc += w*v，accW += |w|）—— 现行实现 | 安全：accW 为几何量，两波长相同 | acc 仍线性，accW 为非线性但波长无关 | acc/accW 不再是“平均权重”，而是带符号加权和除以绝对权重和；环外能量被系统性低估 |');
P('| C | 掩膜截断（cos(alpha)<0 的命中不累加） | 安全：掩膜由几何决定，与波长无关 | 保持线性 | 相当于把有效孔径限制在“半平面内”，环外只剩每像素 acos(R/rho)/pi 的探测器贡献，视角进一步受限 |');
P('');
P('共同结构性事实：三种候选下 w 均只含几何量（R, dTheta, apod, cos(alpha), d），');
P('与波长无关 ⇒ 两波长的 accW 完全相同 ⇒ R1 的比值守卫均安全。');
P('差别只出现在“单幅图的绝对标度与伪影形态”上，属标度/伪影决策，不影响比值。');
P('');

// ------------------------------------------------------------------
// 6. 拼接 / 全局 两模式的 Σ|w| 量级对比
// ------------------------------------------------------------------
P('--- 6. 拼接(spliceMode=1) vs 全局(spliceMode=0) 的 Σ|w| 量级 -----');
P('拼接模式（ImagingSvc.cpp:648）：sectorWidthDeg = 360/M = ' + F(sectorWidthDeg, 3) + ' deg，');
P('每像素仅被“扇区起点落在该像素视角内”的 A-line 命中，');
P('即每像素命中数约为全局模式的 1/M = ' + F(1 / M, 4) + '（边缘有羽化 spliceBlendDeg=' + CFG.spliceBlendDeg + ' deg）。');
P('');
// Numeric: for sample pixels, compute sum|w| over all detectors vs over only the
// detector sector that contains the pixel's angle.
function sumAbsW(x, y, sectorMode) {
  const th = Math.atan2(y, x);
  let s = 0;
  for (let n = 0; n < ND; n++) {
    const ths = CFG.sectorStartDeg * Math.PI / 180.0 + n * dTheta;
    if (sectorMode) {
      // A-line belongs to sector s = floor((angle - sectorStart)/sectorWidth) mod M
      const rel = (((ths - CFG.sectorStartDeg * Math.PI / 180.0) / (sectorWidthDeg * Math.PI / 180.0)) % M + M) % M;
      const sectorOfLine = Math.floor(rel + 1e-9);
      const relPix = (((th - CFG.sectorStartDeg * Math.PI / 180.0) / (sectorWidthDeg * Math.PI / 180.0)) % M + M) % M;
      const sectorOfPix = Math.floor(relPix + 1e-9);
      if (sectorOfLine !== sectorOfPix) continue;
    }
    const w = weights(Math.hypot(x, y), th, ths);
    s += Math.abs(w.wClosed);
  }
  return s;
}
P('| 像素位置 (x,y) [mm] | rho[mm] | Σ|w| 全局 | Σ|w| 拼接 | 拼接/全局 |');
const probePts = [
  [0, 0], [3, 0], [6, 0], [6.569, 0], [8, 0], [12, 0], [18, 0],
  [0, 10], [10, 10], [-14, 5], [15, 15],
];
const ratios = [];
for (const [x, y] of probePts) {
  const g = sumAbsW(x * 1e-3, y * 1e-3, false);
  const s = sumAbsW(x * 1e-3, y * 1e-3, true);
  const rr = s / g;
  if (Number.isFinite(rr)) ratios.push(rr);
  P(`| (${F(x, 3).padStart(7)},${F(y, 2).padStart(6)}) | ${F(Math.hypot(x, y), 3).padStart(7)} | ${F(g, 6).padStart(10)} | ${F(s, 6).padStart(10)} | ${F(rr, 6).padStart(10)} |`);
}
P('');
P(`比值 Σ|w|_拼接 / Σ|w|_全局  在取样点上（有限值）：min=${F(Math.min(...ratios), 6)}  max=${F(Math.max(...ratios), 6)}  mean=${F(ratios.reduce((a, b) => a + b, 0) / ratios.length, 6)}`);
P('几何期望下界 1/M = ' + F(1 / M, 6) + '（若各探测器 |w| 均匀则恰为 1/M）。');
P('实测比值【系统性高于 1/M】：拼接模式保留的是与像素同扇区（即角距最近）的探测器，');
P('它们的 d 更小、|w| 更大，因此 Σ|w| 的保留比例高于命中数比例。');
P('');
P('对归一化选择的影响（仅陈述事实）：');
P('  - 两种模式下 accW 都是纯几何量、两波长相同 ⇒ R1 比值守卫都安全。');
P('  - 拼接模式的 Σ|w| 为全局模式的约 ' + F(Math.min(...ratios), 3) + '~' + F(Math.max(...ratios), 3) +
  '（中心恰为 1/M=' + F(1 / M, 4) + '），故常数 Ω0=4π 归一化在两种模式间会产生');
P('    数倍的系统性幅值偏差（前提 §4.2）；Σ|w| 归一化不随模式改变口径，比值安全。');
P('');

// ------------------------------------------------------------------
// 7. 环上变号像素的空间形状（FOV 内 cos(alpha)=0 轨迹的可视化摘要）
// ------------------------------------------------------------------
P('--- 7. 变号边界在 FOV 内的形状 ---------------------------------');
P('对每个探测器，变号边界是环在其处的切线。全部 4000 条切线的包络就是环本身；');
P('其并集把 FOV 分成：环内（|r|<=R，所有探测器 cos(alpha)>=0）与环外（|r|>R）。');
P('环外每个像素被 acos(R/rho)/pi 比例的探测器以负权重命中，该比例随 rho 增大单调下降：');
P('| rho[mm] | acos(R/rho)/pi |');
for (const mm of [6.57, 7, 8, 9, 10, 12, 14, 16, 18, 20, 22, 25.46]) {
  const rho = mm * 1e-3;
  if (rho <= R) { P(`| ${F(mm, 2).padStart(7)} |  0.000000 (rho<=R) |`); continue; }
  P(`| ${F(mm, 2).padStart(7)} | ${F(Math.acos(R / rho) / Math.PI, 6).padStart(16)} |`);
}
P('');

// ------------------------------------------------------------------
// 8. 未验证项 / 限制
// ------------------------------------------------------------------
P('--- 8. 未验证项 / 限制 ------------------------------------------');
P('* 本任务为纯几何/数值分析，不涉及实机数据（证据层 real hardware validation = 不适用）。');
P('* apodType=0 (none)。若启用 hann/hamming，w 与 dOmega 同乘 apod(j)，R/d 比值不变。');
P('* distanceWeightExponent=1.0。若改为其他值，w 的距离幂次改变，E1 的 R/d 关系不再成立，');
P('  需重新推导（本任务未覆盖）。');
P('* spliceBlendDeg=1.5 deg 的羽化只在拼接模式扇区边界生效，本任务的 Σ|w| 对比用硬切，');
P('  未计入羽化（羽化对 Σ|w| 的影响是 O(blend/sector) 量级）。');
P('* 多半径配准 (radiusPerChannel) 未覆盖：本任务用统一 R=6.57e-3。');
P('* 分层声速只影响走时 tf，不影响权重几何，故本任务未涉及。');
P('');

const outPath = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '..', 'out_d3_weight_behavior.txt');
fs.writeFileSync(outPath, out.join('\n') + '\n');
console.log(out.join('\n'));
console.error(`[written] ${outPath}`);
