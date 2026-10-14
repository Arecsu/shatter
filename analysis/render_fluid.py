#!/usr/bin/env python3
"""Render fluid dumps to a PNG contact sheet: side view (x/y) and top view (x/z) at chosen steps.

  render_fluid.py OUT.png A.bin [B.bin ...] [--steps 0,30,60,120] [--labels oracle,ours]

Each input becomes a pair of rows (side, top); columns are steps. Points are coloured by speed.
"""
import argparse
import os
import sys

import matplotlib

matplotlib.use("Agg")
import matplotlib.pyplot as plt  # noqa: E402
import numpy as np  # noqa: E402

sys.path.insert(0, os.path.dirname(__file__))
from fluidbin import read  # noqa: E402


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("out")
    ap.add_argument("inputs", nargs="+")
    ap.add_argument("--steps", default="0,20,40,80,179")
    ap.add_argument("--labels", default="")
    a = ap.parse_args()
    steps = [int(s) for s in a.steps.split(",")]
    labels = a.labels.split(",") if a.labels else [os.path.basename(os.path.dirname(p)) + "/" + os.path.basename(p)
                                                   for p in a.inputs]
    runs = [read(p) for p in a.inputs]
    allpos = np.concatenate([f["pos"] for r in runs for s in steps if s < r["steps"]
                             for f in [r["frames"][s][1]] if len(f)])
    lo, hi = allpos.min(0), allpos.max(0)
    pad = 0.05 * (hi - lo).max() + 1e-3
    vmax = max(float(np.percentile(np.linalg.norm(r["frames"][s][1]["vel"], axis=1), 99))
               for r in runs for s in steps if s < r["steps"] and len(r["frames"][s][1])) or 1.0

    fig, axes = plt.subplots(2 * len(runs), len(steps), figsize=(3.2 * len(steps), 3.0 * 2 * len(runs)),
                             squeeze=False)
    for ri, r in enumerate(runs):
        for ci, s in enumerate(steps):
            for vi, (ax_i, ax_j, name) in enumerate([(0, 1, "side x/y"), (0, 2, "top x/z")]):
                ax = axes[2 * ri + vi][ci]
                if s < r["steps"]:
                    f = r["frames"][s][1]
                    sp = np.linalg.norm(f["vel"], axis=1)
                    ax.scatter(f["pos"][:, ax_i], f["pos"][:, ax_j], s=1.5, c=sp, cmap="viridis",
                               vmin=0, vmax=vmax, linewidths=0)
                    ax.set_title("%s  step %d  %s  n=%d" % (labels[ri], s, name, len(f)), fontsize=7)
                ax.set_xlim(lo[ax_i] - pad, hi[ax_i] + pad)
                ax.set_ylim(lo[ax_j] - pad, hi[ax_j] + pad)
                ax.set_aspect("equal")
                ax.tick_params(labelsize=6)
    fig.tight_layout()
    fig.savefig(a.out, dpi=110)
    print(a.out)


if __name__ == "__main__":
    main()
