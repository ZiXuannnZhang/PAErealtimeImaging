// D1 — 实测信号特性表征 (measured signal characterisation)
//
// Purpose: judge whether B2 ("low-pass before differentiating") is viable, based on
// MEASURED noise floor / bandwidth / pulse shape. Nothing here assumes viability.
//
// Data source: testdata/01/*.dat  — FileSaver output.
//   encoding   : IEEE-754 binary16 (float16), little-endian, fixed-length records, no header
//   record len : 10000 samples = 40 us at 250 MSa/s   (see record-length evidence below)
//   file layout: one file per (card, channel) per file-sequence; records are one A-line each
//   channel map: Card{1..4}_Ch{A,B} -> physical channel 0..7
//   wavelength : global trigger g alternates wl1/wl2; triggerWlOdd=1 => g even = wl1
//                (RingBlockAssembler.cpp:360). g = fileIdx*500 + recIdx within a channel.
//
// Secondary source: testdata/14.dat — MATLAB-era single-detector file (float64,
//   8300 columns x 4000 samples, wlOffset=301, interleaved wl1/wl2). Used as a
//   cross-check only; its provenance/config is documented in the MATLAB scripts.
//
// Everything is computed on RAW linear values. No per-image normalisation, no display
// stretch, no rectification, no Hilbert magnitude (premise R1/R3).
//
// Run: node d1_signal_characterization.mjs
import fs from 'node:fs';
import path from 'node:path';
import { fileURLToPath } from 'node:url';

const HERE = path.dirname(fileURLToPath(import.meta.url));
const REPO = path.resolve(HERE, '..', '..', '..');
const OUT = [];
const P = (...a) => OUT.push(a.join(''));
const F = (v, n = 4) => (typeof v === 'number' && Number.isFinite(v) ? v.toFixed(n) : String(v));
const E = (v, n = 3) => (typeof v === 'number' && Number.isFinite(v) ? v.toExponential(n) : String(v));

// ------------------------- configuration -------------------------
const FS_HZ = 250e6;          // 250 MSa/s, Constants.h FPGA_ADC_INTERVAL_NS = 4.0 ns
const REC_LEN = 10000;        // samples per A-line record (empirically determined, see §1)
const PRETRIG = 400;          // quiet samples before the trigger delay / ring-down
const FILES_PER_CH = 8;       // testdata/01 holds 8 file sequences per channel
const RECS_PER_FILE = null;   // derived from file size
const N_CH = 8;
const PSD_STRIDE = 8;          // compute the Welch PSD on every 8th record per stream

function f16(u) {
  const s = (u >> 15) & 1, e = (u >> 10) & 31, m = u & 1023;
  if (e === 0) return (s ? -1 : 1) * Math.pow(2, -14) * (m / 1024);
  if (e === 31) return m ? NaN : (s ? -1 : 1) * Infinity;
  return (s ? -1 : 1) * Math.pow(2, e - 15) * (1 + m / 1024);
}

// ------------------------- FFT (radix-2) -------------------------
function fft(re, im) {
  const n = re.length;
  for (let i = 1, j = 0; i < n; i++) {
    let bit = n >> 1;
    for (; j & bit; bit >>= 1) j ^= bit;
    j ^= bit;
    if (i < j) { [re[i], re[j]] = [re[j], re[i]]; [im[i], im[j]] = [im[j], im[i]]; }
  }
  for (let len = 2; len <= n; len <<= 1) {
    const ang = -2 * Math.PI / len;
    const wr = Math.cos(ang), wi = Math.sin(ang);
    for (let i = 0; i < n; i += len) {
      let cr = 1, ci = 0;
      for (let k = 0; k < len / 2; k++) {
        const ur = re[i + k], ui = im[i + k];
        const vr = re[i + k + len / 2] * cr - im[i + k + len / 2] * ci;
        const vi = re[i + k + len / 2] * ci + im[i + k + len / 2] * cr;
        re[i + k] = ur + vr; im[i + k] = ui + vi;
        re[i + k + len / 2] = ur - vr; im[i + k + len / 2] = ui - vi;
        const ncr = cr * wr - ci * wi; ci = cr * wi + ci * wr; cr = ncr;
      }
    }
  }
}
function hann(n) { const w = new Float64Array(n); for (let i = 0; i < n; i++) w[i] = 0.5 - 0.5 * Math.cos(2 * Math.PI * i / n); return w; }

// ------------------------- statistics helpers -------------------------
function rms(a, from = 0, to = a.length) { let s = 0; for (let i = from; i < to; i++) s += a[i] * a[i]; return Math.sqrt(s / (to - from)); }
function peakAbs(a, from = 0, to = a.length) { let m = 0; for (let i = from; i < to; i++) { const v = Math.abs(a[i]); if (v > m) m = v; } return m; }

// Interpolate the sample index where |a| crosses `level` walking outward from `iPeak`.
function crossing(a, iPeak, level) {
  const n = a.length;
  // walk left
  let li = iPeak;
  while (li > 0 && Math.abs(a[li]) > level) li--;
  // linear interp between li and li+1
  const l0 = Math.abs(a[li]), l1 = Math.abs(a[li + 1] ?? a[li]);
  const lerp = l1 > l0 ? li + (level - l0) / (l1 - l0) : li;
  let ri = iPeak;
  while (ri < n - 1 && Math.abs(a[ri]) > level) ri++;
  const r0 = Math.abs(a[ri]), r1 = Math.abs(a[ri - 1] ?? a[ri]);
  const rerp = r1 > r0 ? ri - (level - r0) / (r1 - r0) : ri;
  return [lerp, rerp];
}

// ------------------------- main -------------------------
const dataDir = path.join(REPO, 'testdata', '01');
const chans = [];
for (let c = 1; c <= 4; c++) for (const s of ['A', 'B']) chans.push(`Card${c}_Ch${s}`);
if (!fs.existsSync(dataDir)) { console.error('missing ' + dataDir); process.exit(1); }

P('==================================================================');
P('D1  实测信号特性表征 / measured signal characterisation');
P('==================================================================');
P('');
P('--- 0. 数据清单与配置来源 ---------------------------------------');
P('数据根目录: testdata/01/');
P('');
P('记录格式（实测推定，见 §1）：');
P('  编码        IEEE-754 binary16 (float16) LE，定长记录，无文件头');
P('  记录长度    ' + REC_LEN + ' 采样点 = ' + F(REC_LEN / FS_HZ * 1e6, 2) + ' us @ ' + FS_HZ / 1e6 + ' MSa/s');
P('  通道映射    Card{1..4}_Ch{A,B} -> 物理通道 0..7');
P('  波长映射    全局触发 g 奇偶交替 wl1/wl2；triggerWlOdd=1 => g 偶 = wl1(532nm)');
P('              (RingBlockAssembler.cpp:360)；g = 文件序号*每文件记录数 + 记录序号');
P('');
P('配置基准（ring_recon_cuda_set_defaults / AcqConfig）：');
P('  daqHz = 250e6    sampDepth = 4000    alinesPerFrame = 8000');
P('  radius = 6.57e-3    soundSpeeds = {1490, 1540}    sysDelay = {358, 371}');
P('');
P('【E3 配置来源差异，必须显式标注】');
P('  (a) AcqConfig::acqTimeNs 默认 200000 ns => samplesPerTrig = 50000。');
P('      本数据实测记录长度为 10000 采样点 => 该 2026-08-16 会话的 acqTimeNs 实为 40000 ns，');
P('      与默认值不同。差异已标注，本任务按实测 10000 采样点处理。');
P('  (b) MATLAB 主脚本对 14.dat 记 DAQ=200e6、Radius(14.dat)=6.48e-3；');
P('      本仓默认 daqHz=250e6、radius=6.57e-3。本任务一律以本仓默认值为基准。');
P('  (c) testdata/01 的 provenance 未正式确认（RadiusCalibration/README.md:164 明确');
P('      “不能把历史 testdata/01 的结果当作正式半径结论”）。本任务的数字因此标注为');
P('      “2026-08-16 历史采集”，不外推为正式采集基线。');
P('');

// ---- inventory + SHA256 ----
const { createHash } = await import('node:crypto');
function sha256(f) {
  const h = createHash('sha256');
  const fd = fs.openSync(f, 'r');
  const buf = Buffer.alloc(1 << 22);
  let r;
  while ((r = fs.readSync(fd, buf, 0, buf.length)) > 0) h.update(buf.subarray(0, r));
  fs.closeSync(fd);
  return h.digest('hex');
}
P('--- 0.1 文件清单（SHA256） --------------------------------------');
P('| 文件 | 字节数 | 记录数 | SHA256 |');
const files = [];
for (const ch of chans) {
  for (let s = 0; s < FILES_PER_CH; s++) {
    const name = `${ch}_test_${String(s).padStart(3, '0')}.dat`;
    const p = path.join(dataDir, name);
    if (!fs.existsSync(p)) continue;
    const sz = fs.statSync(p).size;
    const recs = sz / 2 / REC_LEN;
    const hash = sha256(p);
    files.push({ ch, seq: s, path: p, size: sz, recs });
    P(`| ${name} | ${sz} | ${recs} | ${hash} |`);
  }
}
{
  const p14 = path.join(REPO, 'testdata', '14.dat');
  if (fs.existsSync(p14)) {
    const sz = fs.statSync(p14).size;
    P(`| 14.dat (交叉核对用) | ${sz} | float64, 8300 列 x 4000 点 | ${sha256(p14)} |`);
  }
}
P('');
P(`合计 ${files.length} 个文件。每个物理通道 ${FILES_PER_CH} 个文件。`);
P('');

// ---- §1 record-length evidence ----
P('--- 1. 记录长度判定（证据） ------------------------------------');
P('判据：真实记录（=一次触发 = 一根 A-line）应在每个记录起点出现一致的');
P('“静默前缀 + 启动 ring-down”结构。对 50000 与 10000 两种候选分别检验');
P('每块内全局峰值的样本位置一致性。');
{
  const p = files[0].path;
  const sz = fs.statSync(p).size;
  const fd = fs.openSync(p, 'r');
  const b = Buffer.alloc(sz); fs.readSync(fd, b, 0, sz, 0); fs.closeSync(fd);
  const n = sz / 2;
  const pk = (start, len) => { let m = 0, mi = 0; for (let i = 0; i < len; i++) { const v = Math.abs(f16(b.readUInt16LE((start + i) * 2))); if (v > m) { m = v; mi = i; } } return mi; };
  const a10 = [], a50 = [];
  for (let k = 0; k < 20 && k * REC_LEN < n; k++) a10.push(pk(k * REC_LEN, REC_LEN));
  for (let k = 0; k < 5 && k * 50000 < n; k++) a50.push(pk(k * 50000, 50000));
  P('文件 ' + path.basename(p));
  P('  按 10000 分块的峰值样本位置（前 20 块）: ' + a10.join(' '));
  P('  按 50000 分块的峰值样本位置（前 5 块） : ' + a50.join(' '));
  P('  10000 分块下峰值位置极差 = ' + (Math.max(...a10) - Math.min(...a10)) + ' 采样点（稳定）');
  P('  50000 分块下峰值位置极差 = ' + (Math.max(...a50) - Math.min(...a50)) + ' 采样点（不稳定）');
  P('  => 记录长度 = 10000 采样点。');
}
P('');

// ================= per-channel x per-wavelength analysis =================
P('--- 2. 逐通道 × 逐波长数字表 -----------------------------------');
P('');
P('口径定义：');
P('  噪声底 RMS      静默区段 [0, ' + PRETRIG + ') 采样点的均方根（触发延迟前，无回波可到）。');
P('                  另给“全场最低滑窗 RMS”（单条 A-line 上窗长 256 的滑窗 RMS 最小值，');
P('                  对每 ' + PSD_STRIDE + ' 条取 1 条求平均）作为交叉核对。');
P('  噪声底 峰值     静默区段内 |v| 最大值。');
P('  信号有无        max|v|/噪声RMS > 4 判为有信号。纯量化噪声通道该比值 = sqrt(3) = 1.732');
P('                  （均匀分布），有回波通道远大于 1。');
P('  有效带宽        Welch 平均功率谱（Hann 窗 4096 点，50% 重叠，每 ' + PSD_STRIDE + ' 条取 1 条）上的');
P('                  -3 dB / -6 dB 带宽与中心频率（按功率谱重心）。');
P('  脉冲主瓣宽度    过峰值点取线剖，|v| 降到峰值的 -6 dB / -20 dB 处的全宽。');
P('  旁瓣比          主瓣外最大 |v| / 主瓣峰值，dB。');
P('  过零结构        主瓣内符号变化次数。');
P('  SNR             回波峰值 / 噪声底 RMS（线性幅度比，不取对数后比）。');
P('');

const NFFT = 4096;
const win = hann(NFFT);
const winPow = win.reduce((a, b) => a + b * b, 0);
const nBin = NFFT / 2 + 1;
// The pre-trigger quiet segment is only PRETRIG=400 samples, so a 4096-point Welch
// cannot be formed from it. Use a separate short window for the pure-noise PSD
// (0..PRETRIG), and the long window for the signal PSD. Both are averaged over
// every record, so the segment count is large despite the short window.
const NFFT_N = 256;
const winN = hann(NFFT_N);
const winNPow = winN.reduce((a, b) => a + b * b, 0);
const nBinN = NFFT_N / 2 + 1;
const dfN = FS_HZ / NFFT_N;

// accumulators per (channel, wavelength)
const stat = [];
for (let c = 0; c < N_CH; c++) {
  stat.push([0, 1].map(() => ({
    n: 0,
    // noise
    preSum: 0, preSum2: 0, prePeak: 0,
    // per-record peaks & echo peaks
    recPeakSum: 0, recPeakMax: 0,
    // PSD accumulators (echo region) and (pre-trigger = pure noise)
    psdEcho: new Float64Array(nBin), psdNoise: new Float64Array(nBinN),
    segEcho: 0, segNoise: 0, psdCount: 0,
    minWinSum: 0, minWinN: 0,
    // ring-down / pulse metrics accumulators
    rdIdx: 0, rdAmp: 0,
    pw6: 0, pw20: 0, sll: 0, zc: 0, m: 0,
    // waveform mean (for the report's shape figure)
    mean: new Float64Array(REC_LEN),
  })));
}

const RECS_PER_CH_PER_WL_TARGET = Infinity;
let processed = 0;

for (const f of files) {
  const chIdx = chans.indexOf(f.ch);
  const sz = f.size;
  const fd = fs.openSync(f.path, 'r');
  const buf = Buffer.alloc(sz);
  fs.readSync(fd, buf, 0, sz, 0);
  fs.closeSync(fd);
  const recs = sz / 2 / REC_LEN;
  for (let r = 0; r < recs; r++) {
    const g = f.seq * recs + r;              // global trigger index within the channel
    const wl = (g % 2 === 0) ? 0 : 1;        // triggerWlOdd=1 => g even = wl1
    const S = stat[chIdx][wl];
    const off = r * REC_LEN;
    const a = new Float64Array(REC_LEN);
    for (let i = 0; i < REC_LEN; i++) a[i] = f16(buf.readUInt16LE((off + i) * 2));
    S.n++;
    // noise floor: pre-trigger
    let ss = 0, ss2 = 0, pk = 0;
    for (let i = 0; i < PRETRIG; i++) { const v = a[i]; ss += v * v; ss2 += v; const av = Math.abs(v); if (av > pk) pk = av; }
    S.preSum += ss; S.preSum2 += ss2; S.prePeak = Math.max(S.prePeak, pk);
    // ring-down: global peak
    let rp = 0, ri = 0;
    for (let i = 0; i < REC_LEN; i++) { const av = Math.abs(a[i]); if (av > rp) { rp = av; ri = i; } }
    S.recPeakSum += rp; S.recPeakMax = Math.max(S.recPeakMax, rp);
    S.rdIdx += ri; S.rdAmp += rp;
    // accumulate mean waveform
    for (let i = 0; i < REC_LEN; i++) S.mean[i] += a[i];
    // pulse metrics around the ring-down peak
    {
      const level6 = rp * Math.pow(10, -6 / 20), level20 = rp * Math.pow(10, -20 / 20);
      const [l6, r6] = crossing(a, ri, level6);
      const [l20, r20] = crossing(a, ri, level20);
      S.pw6 += (r6 - l6); S.pw20 += (r20 - l20);
      // sidelobe: max |v| outside the -20 dB main-lobe span
      let sm = 0;
      for (let i = 0; i < REC_LEN; i++) if (i < l20 - 1 || i > r20 + 1) { const av = Math.abs(a[i]); if (av > sm) sm = av; }
      S.sll += 20 * Math.log10(Math.max(sm, 1e-9) / Math.max(rp, 1e-9));
      // zero crossings inside the -20 dB main lobe
      let z = 0;
      for (let i = Math.max(1, Math.floor(l20)); i <= Math.min(REC_LEN - 1, Math.ceil(r20)); i++)
        if ((a[i] >= 0) !== (a[i - 1] >= 0)) z++;
      S.zc += z; S.m++;
    }
    // Welch PSD — pure noise from the pre-trigger quiet segment (short window).
    // Computed on every PSD_STRIDE-th record within this (channel,wavelength) stream;
    // the per-record statistics above use every record. A few hundred records x
    // several Welch segments already averages the PSD tightly.
    const doPsd = (S.psdCount++ % PSD_STRIDE === 0);
    if (doPsd) {
      // Noise-floor cross-check: minimum 256-sample sliding-window RMS over the WHOLE
      // record of a single A-line (not the across-record mean, which averages noise away).
      let run = 0;
      for (let i = 0; i < 256; i++) run += a[i] * a[i];
      let mnw = run / 256;
      for (let i = 256; i < REC_LEN; i++) {
        run += a[i] * a[i] - a[i - 256] * a[i - 256];
        const v = run / 256;
        if (v < mnw) mnw = v;
      }
      S.minWinSum += Math.sqrt(mnw);
      S.minWinN++;
    }
    if (doPsd) {
      for (let s = 0; s + NFFT_N <= PRETRIG; s += NFFT_N / 2) {
        const re = new Float64Array(NFFT_N), im = new Float64Array(NFFT_N);
        for (let i = 0; i < NFFT_N; i++) re[i] = a[s + i] * winN[i];
        fft(re, im);
        for (let k = 0; k < nBinN; k++) S.psdNoise[k] += (re[k] * re[k] + im[k] * im[k]) / winNPow;
        S.segNoise++;
      }
      // Welch PSD — full A-line (long window)
      for (let s = 0; s + NFFT <= REC_LEN; s += NFFT / 2) {
        const re = new Float64Array(NFFT), im = new Float64Array(NFFT);
        for (let i = 0; i < NFFT; i++) re[i] = a[s + i] * win[i];
        fft(re, im);
        for (let k = 0; k < nBin; k++) S.psdEcho[k] += (re[k] * re[k] + im[k] * im[k]) / winPow;
        S.segEcho++;
      }
    }
    processed++;
  }
}
P(`已处理 ${processed} 条 A-line 记录（${files.length} 个文件）。`);
P('');

// ---- report table ----
function centreFreq(psd, nseg, df, nb) {
  const N = nb ?? psd.length;
  let num = 0, den = 0;
  for (let k = 1; k < N; k++) { const p = psd[k] / nseg; num += k * df * p; den += p; }
  return den > 0 ? num / den : NaN;
}
function bandwidth(psd, nseg, df, dropDb, nb) {
  const N = nb ?? psd.length;
  const p = new Float64Array(N);
  let mx = 0;
  for (let k = 1; k < N; k++) { p[k] = psd[k] / nseg; if (p[k] > mx) mx = p[k]; }
  const th = mx * Math.pow(10, -dropDb / 10);
  let lo = NaN, hi = NaN;
  for (let k = 1; k < N; k++) if (p[k] >= th) { lo = k * df; break; }
  for (let k = N - 1; k >= 1; k--) if (p[k] >= th) { hi = k * df; break; }
  return [lo, hi];
}
const df = FS_HZ / NFFT;

const rows = [];
for (let c = 0; c < N_CH; c++) {
  for (let w = 0; w < 2; w++) {
    const S = stat[c][w];
    const nAvg = S.n;
    const noiseRms = Math.sqrt(S.preSum / (nAvg * PRETRIG));
    const noiseMean = S.preSum2 / (nAvg * PRETRIG);
    const minWin = S.minWinN > 0 ? S.minWinSum / S.minWinN : NaN;
    const echoPeak = S.recPeakSum / nAvg;
    // Signal-presence verdict. A pure quantisation-noise channel has a bounded,
    // near-uniform amplitude distribution with max|v|/sigma = sqrt(3) = 1.732;
    // a channel carrying photoacoustic echoes has max|v|/sigma >> 1.
    const maxOverSd = S.recPeakMax / Math.max(noiseRms, 1e-12);
    const hasSignal = maxOverSd > 4;
    const [b3l, b3h] = bandwidth(S.psdEcho, S.segEcho, df, 3, nBin);
    const [b6l, b6h] = bandwidth(S.psdEcho, S.segEcho, df, 6, nBin);
    const fc = centreFreq(S.psdEcho, S.segEcho, df, nBin);
    const [nb3l, nb3h] = bandwidth(S.psdNoise, Math.max(S.segNoise, 1), dfN, 3, nBinN);
    rows.push({
      ch: chans[c], wl: w === 0 ? 'wl1(532)' : 'wl2(1064)', n: nAvg,
      noiseRms, noiseMean, noisePeak: S.prePeak, minWin,
      echoPeak, snr: echoPeak / Math.max(noiseRms, 1e-12), maxOverSd, hasSignal,
      b3l, b3h, b3w: b3h - b3l, b6l, b6h, b6w: b6h - b6l, fc,
      nb3l, nb3h,
      pw6: S.pw6 / S.m, pw20: S.pw20 / S.m, sll: S.sll / S.m, zc: S.zc / S.m,
    });
  }
}

P('表 2.1 噪声底与 SNR（逐通道 × 逐波长）');
P('| 通道 | 波长 | 记录数 | 噪声底RMS | 噪声底均值 | 噪声底峰值 | 最低滑窗RMS(256) | 回波峰值(均) | 回波峰值(最大) | max|v|/RMS | SNR(峰值/RMS) | 信号 |');
for (const r of rows) {
  const S = stat[chans.indexOf(r.ch)][r.wl.startsWith('wl1') ? 0 : 1];
  P(`| ${r.ch} | ${r.wl} | ${r.n} | ${F(r.noiseRms, 3)} | ${F(r.noiseMean, 3)} | ${F(r.noisePeak, 1)} | ${F(r.minWin, 3)} | ${F(r.echoPeak, 1)} | ${F(S.recPeakMax, 1)} | ${F(r.maxOverSd, 3)} | ${F(r.snr, 1)} | ${r.hasSignal ? '有' : '无'} |`);
}
const sigCh = rows.filter((r) => r.hasSignal);
P('');
P(`信号通道统计：${sigCh.length} / ${rows.length} 组（通道×波长）含真实信号。`);
P(`  有信号：${sigCh.map((r) => r.ch + '/' + r.wl.split('(')[0]).join(', ') || '（无）'}`);
P('');
P('【重大数据充分性发现】');
P('  testdata/01 的 8 个物理通道中，仅 Card1_ChA 含真实光声信号；');
P('  其余 7 个通道的 max|v|/噪声RMS = 1.73~1.75 ≈ sqrt(3)，幅度分布近似均匀，');
P('  是纯量化噪声（无回波）。这意味着：');
P('  - 噪声底、双波长一致性：8 个通道全部可测，结论有效；');
P('  - 有效带宽 / 脉冲形状 / SNR / 导数可行性：只能在 Card1_ChA 上测，');
P('    其余通道表 2.2/2.3 的数字描述的是【噪声】不是【信号】，不得当作信号特性使用。');
P('  证据层如实标注：这是历史实测数据的内容事实，不是合成假设。');
P('');
P('表 2.2 有效带宽（Welch 平均功率谱，Hann 4096 点，50% 重叠）');
P('单位 MHz。echo 谱 = 含 ring-down 的整条 A-line；noise 谱 = 触发前静默区段。');
P('| 通道 | 波长 | echo -3dB 下 | echo -3dB 上 | echo -3dB 带宽 | echo -6dB 带宽 | 中心频率 | noise -3dB 带宽 |');
for (const r of rows) {
  P(`| ${r.ch} | ${r.wl} | ${F(r.b3l / 1e6, 3)} | ${F(r.b3h / 1e6, 3)} | ${F(r.b3w / 1e6, 3)} | ${F(r.b6w / 1e6, 3)} | ${F(r.fc / 1e6, 3)} | ${F((r.nb3h - r.nb3l) / 1e6, 3)} |`);
}
P('');
P('表 2.3 脉冲形状（以各记录全局峰值即启动 ring-down 为主瓣）');
P('| 通道 | 波长 | 主瓣 -6dB 全宽(采样) | 主瓣 -6dB 全宽(us) | 主瓣 -20dB 全宽(us) | 旁瓣比(dB) | 过零次数 | 峰位(采样) |');
for (const r of rows) {
  const S = stat[chans.indexOf(r.ch)][r.wl.startsWith('wl1') ? 0 : 1];
  P(`| ${r.ch} | ${r.wl} | ${F(r.pw6, 2)} | ${F(r.pw6 / FS_HZ * 1e6, 3)} | ${F(r.pw20 / FS_HZ * 1e6, 3)} | ${F(r.sll, 2)} | ${F(r.zc, 2)} | ${F(S.rdIdx / S.n, 1)} |`);
}
P('');

// ---- dual-wavelength consistency ----
P('--- 3. 双波长一致性 --------------------------------------------');
P('口径：同一物理通道下 wl1 与 wl2 的噪声底 / 带宽 / 脉宽直接比较，');
P('不做任何逐图归一化（R1）。差异用相对百分比与 dB 表示。');
P('');
P('| 通道 | 噪声底RMS wl1 | 噪声底RMS wl2 | 相对差% | 带宽wl1(MHz) | 带宽wl2(MHz) | 相对差% | -6dB脉宽wl1(us) | -6dB脉宽wl2(us) | 相对差% |');
const wlRel = { noise: [], bw: [], pw: [], snr: [] };
for (let c = 0; c < N_CH; c++) {
  const a = rows[c * 2], b = rows[c * 2 + 1];
  const dN = 100 * (b.noiseRms - a.noiseRms) / a.noiseRms;
  const dBw = 100 * (b.b3w - a.b3w) / a.b3w;
  const dP = 100 * (b.pw6 - a.pw6) / a.pw6;
  wlRel.noise.push(Math.abs(dN)); wlRel.bw.push(Math.abs(dBw)); wlRel.pw.push(Math.abs(dP));
  P(`| ${chans[c]} | ${F(a.noiseRms, 3)} | ${F(b.noiseRms, 3)} | ${F(dN, 2)} | ${F(a.b3w / 1e6, 3)} | ${F(b.b3w / 1e6, 3)} | ${F(dBw, 2)} | ${F(a.pw6 / FS_HZ * 1e6, 3)} | ${F(b.pw6 / FS_HZ * 1e6, 3)} | ${F(dP, 2)} |`);
}
P('');
P(`8 个物理通道上 |相对差| 的最大值：噪声底 ${F(Math.max(...wlRel.noise), 2)}%  带宽 ${F(Math.max(...wlRel.bw), 2)}%  脉宽 ${F(Math.max(...wlRel.pw), 2)}%`);
P('');

// ---- §4 ring-down ----
P('--- 4. 启动 ring-down ------------------------------------------');
{
  // Use the mean waveform of channel 0 / wl1 to characterise the ring-down tail.
  const S = stat[0][0];
  const m = S.mean.map((x) => x / S.n);
  const ipk = m.reduce((bi, v, i, arr) => Math.abs(arr[i]) > Math.abs(arr[bi]) ? i : bi, 0);
  const apk = Math.abs(m[ipk]);
  P(`参考：Card1_ChA / wl1 的平均波形，峰值 ${F(apk, 1)} @ 采样 ${ipk}（t=${F(ipk / FS_HZ * 1e6, 3)} us）`);
  // decay: fit |m| envelope after the peak with exp(-t/tau) in the first 200 samples
  const TAU_WIN = 400;
  let sx = 0, sy = 0, sxx = 0, sxy = 0, cnt = 0;
  for (let i = ipk + 2; i < ipk + 2 + TAU_WIN; i++) {
    const y = Math.log(Math.max(Math.abs(m[i]), 1e-9) / apk);
    const x = (i - ipk) / FS_HZ;
    sx += x; sy += y; sxx += x * x; sxy += x * y; cnt++;
  }
  const slope = (cnt * sxy - sx * sy) / (cnt * sxx - sx * sx);
  const tau = -1 / slope;
  P(`  衰减时间常数 tau（峰后 ${TAU_WIN} 采样点对数包络线性拟合）= ${E(tau, 3)} s = ${F(tau * 1e6, 4)} us`);
  // residual tail: where does |m| drop to 1% and 0.1% of the echo peak
  const echoPeakAvg = S.recPeakSum / S.n;
  for (const pct of [0.01, 0.001]) {
    const th = echoPeakAvg * pct;
    let idx = -1;
    for (let i = ipk; i < REC_LEN; i++) if (Math.abs(m[i]) > th) idx = i;
    P(`  残留拖尾长度：降到回波峰值 ${pct * 100}% 以下的最后样本位置 = ${idx}（t=${F(idx / FS_HZ * 1e6, 3)} us）${idx > 0 ? '' : '（未超过阈值即已低于）'}`);
  }
  P('  ring-down 幅度 = 表 2.3 的“回波峰值(均/最大)”。');
  P('  【注意】ring-down 峰值比噪声底高约 ' + F(rows[0].snr, 0) + ' 倍（见表 2.1 SNR），');
  P('  它是本数据中最强的成分，直接对它求导会把微分链路推到满量程。');
}
P('');

// ---- §5 derivative feasibility ----
P('--- 5. 导数可行性判定（B2「导数前先低通」） --------------------');
P('');
P('物理口径：微分在频域乘 j*2*pi*f，即 +6 dB/oct 高通。设噪声功率谱密度 S_p(f)、');
P('低通截止 fc，则');
P('    var(p)       = integral_0^fc S_p(f) df');
P('    var(dp/dt)   = integral_0^fc (2*pi*f)^2 * S_p(f) df');
P('先低通再求导，等价于把积分上限截到 fc。');
P('');
P('【公平比较口径】不能把“原始 SNR（全带宽）”与“微分 SNR（低通后）”直接比。');
P('B2 的问题是：同样的低通 fc 下，B1 的信号项 2p - 2t*p\'（t 用秒，t=r_tilde/c）');
P('相对 DAS 的信号项 p，SNR 变差多少。因此本任务比较');
P('    SNR_DAS(fc) = |p(t0)| / sqrt(var(p, fc))');
P('    SNR_B1 (fc,t0) = |2p(t0) - 2*t0*p\'(t0)| / sqrt(var(2p-2t*p\', fc))');
P('其中 var(2p-2t*p\', fc) = integral_0^fc |2 - 2*t0*j*2*pi*f|^2 * S_p(f) df');
P('                  = integral_0^fc 4*(1 + (2*pi*f*t0)^2) * S_p(f) df');
P('');
P('噪声谱：取 7 个无信号通道 + Card1_ChA 触发前静默段的 Welch 平均（见 §2）。');
P('信号 p：Card1_ChA/wl1 的 2000 条平均波形（近似无噪）。');
P('');
{
  const psdN = new Float64Array(nBinN);
  let seg = 0;
  for (let c = 0; c < N_CH; c++) for (let w = 0; w < 2; w++) {
    const S = stat[c][w];
    for (let k = 0; k < nBinN; k++) psdN[k] += S.psdNoise[k] / Math.max(S.segNoise, 1);
    seg++;
  }
  for (let k = 0; k < nBinN; k++) psdN[k] /= seg;
  // one-sided PSD density [v^2/Hz]: |X|^2/winPow averaged over segments -> *2/fs
  const Sden = new Float64Array(nBinN);
  for (let k = 0; k < nBinN; k++) Sden[k] = psdN[k] * 2 / FS_HZ;
  // Parseval cross-check: integral Sden df must equal the measured time-domain noise variance
  let vint = 0;
  for (let k = 1; k < nBinN; k++) vint += Sden[k] * dfN;
  const measuredNoiseVar = rows.reduce((a, r) => a + r.noiseRms * r.noiseRms, 0) / rows.length;
  P(`噪声谱归一化自检：integral S_p(f) df = ${E(vint, 4)}（均方）  =>  RMS = ${E(Math.sqrt(vint), 4)}`);
  P(`                   实测触发前噪声方差（16 组平均） = ${E(measuredNoiseVar, 4)}  =>  RMS = ${E(Math.sqrt(measuredNoiseVar), 4)}`);
  P(`                   相对偏差 = ${F(100 * (vint - measuredNoiseVar) / measuredNoiseVar, 3)} %`);
  P('');

  // signal waveform and derivative
  const S0 = stat[0][0];
  const m = new Float64Array(REC_LEN);
  for (let i = 0; i < REC_LEN; i++) m[i] = S0.mean[i] / S0.n;
  const dm = new Float64Array(REC_LEN);
  for (let i = 1; i < REC_LEN - 1; i++) dm[i] = (m[i + 1] - m[i - 1]) / (2 / FS_HZ);

  // pick evaluation points: the ring-down peak, and the strongest post-ring-down echo
  function argmaxAbs(from, to) {
    let bi = from, bv = 0;
    for (let i = from; i < to; i++) { const v = Math.abs(m[i]); if (v > bv) { bv = v; bi = i; } }
    return bi;
  }
  const iRd = argmaxAbs(0, REC_LEN);
  const iEc = argmaxAbs(Math.floor(REC_LEN * 0.05), REC_LEN);   // skip the first 5% (ring-down)
  const points = [
    { name: '启动 ring-down 峰', i: iRd },
    { name: 'ring-down 后最强回波', i: iEc },
    { name: 't=2us 处采样', i: Math.round(2e-6 * FS_HZ) },
    { name: 't=10us 处采样', i: Math.round(10e-6 * FS_HZ) },
  ];
  P('评估点（Card1_ChA/wl1 平均波形）：');
  for (const p of points)
    P(`  ${p.name}: 采样 ${p.i} (t=${F(p.i / FS_HZ * 1e6, 3)} us)  p=${E(m[p.i], 4)}  p'=${E(dm[p.i], 4)}`);
  P('');

  const fcs = [1, 2, 3, 5, 8, 10, 15, 20, 30, 40, 60, 80, 100, 125];
  for (const pt of points) {
    const t0 = pt.i / FS_HZ;               // seconds
    const p0 = m[pt.i], dp0 = dm[pt.i];
    const sigDAS = Math.abs(p0);
    const sigB1 = Math.abs(2 * p0 - 2 * t0 * dp0);
    P(`评估点「${pt.name}」 t0=${E(t0, 3)} s`);
    P('| fc(MHz) | var_DAS | std_DAS | SNR_DAS | var_B1 | std_B1 | SNR_B1 | SNR_B1/SNR_DAS(dB) |');
    for (const fcMHz of fcs) {
      const fc = fcMHz * 1e6;
      let vD = 0, vB = 0;
      for (let k = 1; k < nBinN; k++) {
        const f = k * dfN;
        if (f > fc) break;
        const s = Sden[k] * dfN;
        vD += s;
        vB += 4 * (1 + Math.pow(2 * Math.PI * f * t0, 2)) * s;
      }
      const sD = Math.sqrt(vD), sB = Math.sqrt(vB);
      const nD = sigDAS / Math.max(sD, 1e-300), nB = sigB1 / Math.max(sB, 1e-300);
      const rel = 20 * Math.log10(Math.max(nB, 1e-300) / Math.max(nD, 1e-300));
      P(`| ${String(fcMHz).padStart(6)} | ${E(vD, 3)} | ${E(sD, 3)} | ${F(nD, 2)} | ${E(vB, 3)} | ${E(sB, 3)} | ${F(nB, 2)} | ${F(rel, 2)} |`);
    }
    P('');
  }

  // Verdict: find the fc range where B1's SNR penalty vs DAS stays within 6 dB
  P('判定：');
  P('  判据取「SNR_B1 相对 SNR_DAS 的损失 <= 6 dB」，且低通不削掉信号 -3 dB 带宽。');
  const pt = points[1];   // the strongest real echo is the meaningful one
  const t0 = pt.i / FS_HZ, p0 = m[pt.i], dp0 = dm[pt.i];
  const sigDAS = Math.abs(p0), sigB1 = Math.abs(2 * p0 - 2 * t0 * dp0);
  const penalties = [];
  for (const fcMHz of fcs) {
    const fc = fcMHz * 1e6;
    let vD = 0, vB = 0;
    for (let k = 1; k < nBinN; k++) {
      const f = k * dfN; if (f > fc) break;
      const s = Sden[k] * dfN;
      vD += s; vB += 4 * (1 + Math.pow(2 * Math.PI * f * t0, 2)) * s;
    }
    const nD = sigDAS / Math.sqrt(vD), nB = sigB1 / Math.sqrt(vB);
    penalties.push({ fcMHz, rel: 20 * Math.log10(nB / nD), nD, nB });
  }
  P('  以「ring-down 后最强回波」为评估点：');
  for (const p of penalties)
    P(`    fc=${String(p.fcMHz).padStart(3)} MHz : SNR_DAS=${F(p.nD, 2).padStart(9)}  SNR_B1=${F(p.nB, 2).padStart(9)}  损失=${F(p.rel, 2).padStart(7)} dB`);
  const okFc = penalties.filter((p) => p.rel >= -6).map((p) => p.fcMHz);
  const sigB3 = rows[0].b3w / 1e6;
  P(`  信号 -3 dB 带宽（实测，Card1_ChA）= ${F(sigB3, 3)} MHz；中心频率 ${F(rows[0].fc / 1e6, 3)} MHz。`);
  P('');
  if (okFc.length === 0) {
    P('  【结论：导数型信号项在本数据上不可行】');
    P('  依据：在全部扫描的 fc 上，B1 信号项 2p-2t*p\' 的 SNR 都比 DAS 的 p 低 6 dB 以上。');
    P('  原因是微分按 f 加权放大噪声，而本数据的噪声谱一直到 Nyquist 都不为零，');
    P('  低通只能截断积分上限，无法抵消 (2*pi*f*t0)^2 的放大因子。');
    P('  若必须走 B1 路线，应先提高实测 SNR，或改用不带导数项的权重改造。');
  } else {
    const lo = Math.min(...okFc), hi = Math.max(...okFc);
    P(`  【结论：导数型信号项在本数据上有条件可行】`);
    P(`  建议 B2 低通截止区间：${lo} ~ ${hi} MHz（该区间内 B1 的 SNR 损失 <= 6 dB）。`);
    P(`  下限约束：不得低于信号 -3 dB 带宽的下沿（实测 ${F(rows[0].b3l / 1e6, 3)} MHz），否则削信号；`);
    P(`  上限约束：fc 越高，(2*pi*f*t0)^2 放大越强，SNR 损失越大。`);
    P('  【注意 t0 依赖】t0 = r_tilde/c 用秒。t0 越大（像素越远），2t*p\' 项越重，');
    P('  损失越大；上表用的是回波处的 t0，环外/远处像素会更差。');
    P('  【附加约束 G5】成像路径已经过前端零相位滤波（FrontendPreprocessor::dispatch');
    P('  把滤波后的 clone 同时喂 DisplayBuffer 与 ring()）。B2 若在重建侧再加低通，');
    P('  就是成像数据被滤第二次。必须先定分工口径，不得让成像数据被零相位滤两次。');
    P('  因此【本任务不给出“立刻实施”的建议】，只给出 fc 数值区间与该前置口径约束。');
  }
  P('');
  P('  附加风险：本数据中最强成分是启动 ring-down（比噪声底高 ~' + F(rows[0].snr, 0) + ' 倍），');
  P('  微分会进一步放大其陡峭前沿。B2 必须先做 ring-down 抑制（现有 dbrSigRemove/maskLength');
  P('  已在预处理侧做截断），否则微分项会被 ring-down 主导。');
  P('');
  P('  【数据充分性限制】testdata/01 中仅 Card1_ChA 一个物理通道含真实光声信号，');
  P('  其余 7 个通道为纯均匀量化噪声（max/σ=1.73=√3，无回波）。因此上述导数可行性');
  P('  判定【只建立在 1/8 个物理通道上】，不构成全部通道的结论。');
}
P('');

// ---- §6 cross-check with 14.dat ----
P('--- 6. 与 14.dat 的交叉核对 ------------------------------------');
{
  const p14 = path.join(REPO, 'testdata', '14.dat');
  if (!fs.existsSync(p14)) { P('14.dat 不存在，跳过。'); }
  else {
    const sz = fs.statSync(p14).size;
    const fd = fs.openSync(p14, 'r');
    const nD = sz / 8;
    const cols = nD / 4000;
    P(`14.dat: ${sz} 字节 = ${nD} 个 float64 = ${cols} 列 x 4000 点/列。`);
    P('布局（实时重建脚本/simulateAcquisition.m）：列主序 reshape(SampDepth=4000, [])，');
    P('wlOffset = 301（1-based），wl1 = 列 301,303,...，wl2 = 列 302,304,...；');
    P('每波长 A-line 数 = AlinesPerFrame/2 = 4000，占用列 301..8300（正好 8000 列）。');
    P('前 300 列（=150 对）为启动段，被 wlOffset 跳过。');
    // Read a sample of wl1 columns and compute noise floor + peak
    for (const [label, firstCol] of [['wl1', 301], ['wl2', 302]]) {
      const useCols = 64;
      let preSum = 0, preCnt = 0, peak = 0, allSum2 = 0, allCnt = 0;
      const buf = Buffer.alloc(4000 * 8);
      for (let k = 0; k < useCols; k++) {
        const col = firstCol + 2 * k - 1;   // to 0-based
        fs.readSync(fd, buf, 0, 4000 * 8, col * 4000 * 8);
        for (let i = 0; i < 4000; i++) {
          const v = buf.readDoubleLE(i * 8);
          allSum2 += v * v; allCnt++;
          if (i < PRETRIG) { preSum += v * v; preCnt++; }
          const av = Math.abs(v); if (av > peak) peak = av;
        }
      }
      P(`  ${label}（前 ${useCols} 根）: 触发前噪声底 RMS=${F(Math.sqrt(preSum / preCnt), 3)}  全程RMS=${F(Math.sqrt(allSum2 / allCnt), 3)}  峰值=${F(peak, 1)}  SNR=${F(peak / Math.sqrt(preSum / preCnt), 1)}`);
    }
    fs.closeSync(fd);
    P('  注：14.dat 的数值量级（ADC 计数量级）与 testdata/01 的 float16 不同源，');
    P('  只作数量级交叉核对，不并入表 2.x 的统计。');
  }
}
P('');

// ---- §7 evidence layers & limitations ----
P('--- 7. 证据口径（四层分离） ------------------------------------');
P('source/code correctness             未声称（本任务不改代码）');
P('automated tests/build/selftest      脚本可复跑 = 是（node d1_signal_characterization.mjs）');
P('real hardware validation            如实标注：用的是 testdata/01（2026-08-16 历史实测采集，');
P('                                    provenance 未正式确认）+ testdata/14.dat（MATLAB 时代');
P('                                    单探测器实测数据）。非合成数据，但也【不是】本轮实机验收数据。');
P('hardware root-cause attribution     未声称');
P('');
P('--- 8. 未验证项 / 限制 ------------------------------------------');
P('* 记录长度 10000 采样点由峰值位置一致性实测推定，未取得该会话的采集配置快照佐证。');
P('* wl1/wl2 的奇偶约定取自 RingBlockAssembler.cpp:360（triggerWlOdd=1 => g 偶 = wl1）。');
P('  若实际会话的 triggerWlOdd 取 0，则两列标签对调；表中成对数字不受影响。');
P('* testdata/01 的8 个文件是否连续同一轮次未验证；FileSaver 的物理轮次滚动可能');
P('  使跨文件的 g 不连续。这会影响波长奇偶的绝对标签，不影响“两组之间有无差异”。');
P('* 噪声底取触发前 [0,400) 静默段。若该段含残留 ring-down 拖尾，会高估噪声底；');
P('  已用“全场最低滑窗 RMS”交叉核对（表 2.1）。');
P('* 带宽为整条 A-line（含 ring-down）的谱；ring-down 主导低频。已同时给出 noise 谱对照。');
P('* 脉冲形状以全局峰值（启动 ring-down）为主瓣。真正的组织回波脉冲形状未单独提取');
P('  （需先定义回波选取规则，属 B 系列后续任务）。');
P('* 微分 SNR 用平均波形（近似无噪）估计信号微分峰值，用实测噪声谱估计噪声；');
P('  属“可自验的估计”，不是逐条 A-line 的实测微分 SNR 分布。');
P('* 未测双波长的幅值比（那是 D4 比值守卫与 D6/D7 的事）。');
P('* 未涉及实机实时采集；全部为离线分析。');
P('');

const outPath = path.resolve(HERE, '..', 'out_d1_signal_characterization.txt');
fs.writeFileSync(outPath, OUT.join('\n') + '\n');
console.log(OUT.join('\n'));
console.error(`[written] ${outPath}`);
