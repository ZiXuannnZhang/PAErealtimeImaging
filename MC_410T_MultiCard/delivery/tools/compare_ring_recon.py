#!/usr/bin/env python3
"""逐块对比 MATLAB 参考与 C++ RingRecon 输出。

对比三类数据（每波长每块）：
  - acc   原始累加图（最接近算法等价性）
  - accw  权重累加图
  - img   归一化显示图（会放大弱信号/探测器奇异点的浮点噪声）

用法:
  python compare_ring_recon.py <ref_dir> <out_dir> <nx> <ny> <nblocks>
      [--dataset 11|14] [--grid-mm 0.1] [--block 200] [--tol 1e-4]
"""
import argparse
import math
import os
import sys

import numpy as np


def load(path, n):
    a = np.fromfile(path, dtype=np.float32)
    if a.size != n:
        raise SystemExit(f"{path}: expected {n} floats, got {a.size}")
    return a


def singular_mask(dataset, block_k, nx, ny, grid_mm, alines_per_block, xv, yv,
                  mask_mult=10.0):
    """排除距离任一已出现探测器 < mask_mult*网格步长的像素。

    DAS 权重含 1/dist^2，探测器附近的浮点舍入会被放大；
    该掩膜覆盖第 1..block_k 块的所有探测器位置。
    """
    if dataset not in (11, 14):
        return np.zeros((nx, ny), dtype=bool)
    R = 6.57e-3
    coverage = 180.0 if dataset == 11 else 360.0
    nwl_frame = 2000 if dataset == 11 else 4000
    nwl_block = alines_per_block // 2
    dtheta = coverage / nwl_frame
    min_dist = grid_mm * 1e-3
    thr = mask_mult * min_dist
    X = xv[:, None]           # nx x 1
    Y = yv[None, :]           # 1 x ny
    m = np.zeros((nx, ny), dtype=bool)
    for b in range(1, block_k + 1):
        start = (b - 1) * nwl_block * dtheta
        j = np.arange(nwl_block)
        th = (start + j * dtheta) * math.pi / 180.0
        detx = R * np.cos(th)
        dety = R * np.sin(th)
        d = np.sqrt((X[:, :, None] - detx[None, None, :]) ** 2 +
                    (Y[:, :, None] - dety[None, None, :]) ** 2)
        m |= d.min(axis=2) < thr
    return m  # 与文件布局一致：索引 ix*ny+iy -> mask[ix, iy]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("ref_dir")
    ap.add_argument("out_dir")
    ap.add_argument("nx", type=int)
    ap.add_argument("ny", type=int)
    ap.add_argument("nblocks", type=int)
    ap.add_argument("--dataset", type=int, default=0)
    ap.add_argument("--grid-mm", type=float, default=0.1)
    ap.add_argument("--block", type=int, default=200)
    ap.add_argument("--tol", type=float, default=1e-3)
    ap.add_argument("--mask-mult", type=float, default=10.0)
    args = ap.parse_args()

    n = args.nx * args.ny
    xv = load(os.path.join(args.ref_dir, "grid_x_f32.raw"), args.nx)
    yv = load(os.path.join(args.ref_dir, "grid_y_f32.raw"), args.ny)

    worst = {"acc": (0.0, ""), "accw": (0.0, ""), "img": (0.0, ""), "img_masked": (0.0, "")}
    sing_count = 0
    for wl in (1, 2):
        for k in range(1, args.nblocks + 1):
            name = f"wl{wl}_{k:03d}.raw"
            acc_name = f"wl{wl}_acc_{k:03d}.raw"
            accw_name = f"wl{wl}_accw_{k:03d}.raw"
            r = load(os.path.join(args.ref_dir, name), n)
            o = load(os.path.join(args.out_dir, name), n)
            ra = load(os.path.join(args.ref_dir, acc_name), n)
            oa = load(os.path.join(args.out_dir, acc_name), n)
            rw = load(os.path.join(args.ref_dir, accw_name), n)
            ow = load(os.path.join(args.out_dir, accw_name), n)

            m = singular_mask(args.dataset, k, args.nx, args.ny,
                              args.grid_mm, args.block, xv, yv)
            flat = m.reshape(-1)
            sing_count += int(flat.sum())

            use_mask = args.dataset and np.any(~flat)
            for key, a, b in (("acc", ra, oa), ("accw", rw, ow), ("img", r, o)):
                if use_mask:
                    a = a[~flat]
                    b = b[~flat]
                refmax = float(np.max(np.abs(a))) or 1e-30
                rel = float(np.max(np.abs(a - b))) / refmax
                if rel > worst[key][0]:
                    worst[key] = (rel, f"{name}/{key}")

            if args.dataset:
                d = np.abs(r - o)
                refmax = float(np.max(np.abs(r))) or 1e-30
                rel = float(np.max(d[~flat])) / refmax if np.any(~flat) else 0.0
                if rel > worst["img_masked"][0]:
                    worst["img_masked"] = (rel, f"{name}/img(masked)")
            else:
                worst["img_masked"] = worst["img"]

    print(f"singular pixels excluded (dataset={args.dataset}): {sing_count}")
    for key in ("acc", "accw", "img_masked", "img"):
        print(f"worst {key:11s}: rel={worst[key][0]:.6g}  at {worst[key][1]}")

    ok = worst["acc"][0] <= args.tol and worst["accw"][0] <= args.tol and \
         worst["img_masked"][0] <= args.tol
    print("PASS" if ok else "FAIL")
    sys.exit(0 if ok else 1)


if __name__ == "__main__":
    main()