#!/usr/bin/env python3
"""Render PXCL cloth dumps to PNG: contact sheets (steps as columns) or single frames.

  render_cloth.py sheet OUT.png A.bin [B.bin ...] [--steps 0,40,80,..] [--labels a,b] [--scenario X.txt]
  render_cloth.py frames OUT_DIR A.bin [--every N] [--scenario X.txt]

Each input gives two rows: an oblique 3D view (painter-sorted, flat-shaded triangles) and a side view (x/y).
Static shapes from the scenario file (plane/sphere/box) are drawn as outlines when --scenario is given
(auto-detected from captures/cloth/<name>/ -> scenarios/cloth/<name>.txt).
"""
import argparse
import os
import sys

import matplotlib

matplotlib.use("Agg")
import matplotlib.pyplot as plt  # noqa: E402
import numpy as np  # noqa: E402
from matplotlib.collections import PolyCollection  # noqa: E402

sys.path.insert(0, os.path.dirname(__file__))
from clothbin import read  # noqa: E402

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


def parse_shapes(path):
    shapes = []
    if not path or not os.path.exists(path):
        return shapes
    for line in open(path):
        t = line.split("#")[0].split()
        if not t:
            continue
        try:
            v = [float(x) for x in t[1:]]
        except ValueError:
            continue
        if t[0] in ("plane", "box", "sphere", "capsule"):
            shapes.append((t[0], v))
    return shapes


def shape_lines(kind, v):
    """Wireframe polylines (Nx3 arrays) for a static shape."""
    out = []
    if kind == "sphere":
        r, c = v[0], np.array(v[1:4])
        a = np.linspace(0, 2 * np.pi, 49)
        for i, j in ((0, 1), (0, 2), (1, 2)):
            p = np.zeros((49, 3))
            p[:, i], p[:, j] = np.cos(a) * r, np.sin(a) * r
            out.append(p + c)
    elif kind == "box":
        h, c = np.array(v[0:3]), np.array(v[3:6])
        cs = np.array([[sx, sy, sz] for sx in (-1, 1) for sy in (-1, 1) for sz in (-1, 1)]) * h + c
        for a in range(8):
            for b in range(a + 1, 8):
                if bin(a ^ b).count("1") == 1:
                    out.append(np.array([cs[a], cs[b]]))
    return out


def project_oblique(p):
    """Camera: yaw 35 deg, pitch 28 deg. Returns screen xy and depth."""
    yaw, pit = np.radians(35), np.radians(28)
    x = p[..., 0] * np.cos(yaw) - p[..., 2] * np.sin(yaw)
    z = p[..., 0] * np.sin(yaw) + p[..., 2] * np.cos(yaw)
    y = p[..., 1] * np.cos(pit) + z * np.sin(pit)
    d = z * np.cos(pit) - p[..., 1] * np.sin(pit)
    return np.stack([x, y], -1), d


def draw_cloth(ax, pos, tri, view, ground=None):
    if len(tri) == 0 or len(pos) == 0:
        return
    tri = tri[(tri < len(pos)).all(axis=1)]
    P = pos[tri]
    if view == "oblique":
        xy, d = project_oblique(P)
        order = np.argsort(-d.mean(1))  # far first
    else:
        xy, order = P[..., [0, 1]], np.arange(len(tri))
    n = np.cross(P[:, 1] - P[:, 0], P[:, 2] - P[:, 0])
    n /= np.linalg.norm(n, axis=1, keepdims=True) + 1e-12
    light = np.array([0.3, 0.8, 0.5])
    light /= np.linalg.norm(light)
    sh = 0.35 + 0.6 * np.abs(n @ light)
    col = np.stack([0.20 * sh + 0.1, 0.45 * sh + 0.1, 0.85 * sh], -1).clip(0, 1)
    pc = PolyCollection(xy[order], facecolors=col[order], edgecolors=(0, 0, 0, 0.25), linewidths=0.2)
    ax.add_collection(pc)


def scene_extent(runs, steps, shapes):
    pts = [f["v"]["pos"] for r in runs for s in steps if s < len(r["frames"]) for f in [r["frames"][s]] if len(f["v"])]
    for kind, v in shapes:
        for l in shape_lines(kind, v):
            pts.append(l)
    a = np.concatenate(pts)
    return a.min(0), a.max(0)


def draw_frame(ax, r, s, view, shapes, lo, hi, title):
    f = r["frames"][s]
    draw_cloth(ax, f["v"]["pos"], f["tri"], view)
    for kind, v in shapes:
        if kind == "plane":
            if view == "side" and abs(v[1]) > 0.5:
                ax.axhline(v[3] / v[1], color="0.5", lw=1)
            continue
        for l in shape_lines(kind, v):
            xy = project_oblique(l)[0] if view == "oblique" else l[:, [0, 1]]
            ax.plot(xy[:, 0], xy[:, 1], color="tab:red", lw=0.6, alpha=0.8)
    corners = np.array([[x, y, z] for x in (lo[0], hi[0]) for y in (lo[1], hi[1]) for z in (lo[2], hi[2])])
    xy = project_oblique(corners)[0] if view == "oblique" else corners[:, [0, 1]]
    pad = 0.05 * (xy.max(0) - xy.min(0)).max() + 1e-3
    ax.set_xlim(xy[:, 0].min() - pad, xy[:, 0].max() + pad)
    ax.set_ylim(xy[:, 1].min() - pad, xy[:, 1].max() + pad)
    ax.set_aspect("equal")
    ax.set_title(title, fontsize=7)
    ax.tick_params(labelsize=5)


def auto_scenario(path):
    name = os.path.basename(os.path.dirname(os.path.abspath(path)))
    return os.path.join(ROOT, "scenarios", "cloth", name + ".txt")


def sheet(out, inputs, steps, labels, scen):
    runs = [read(p) for p in inputs]
    shapes = parse_shapes(scen or auto_scenario(inputs[0]))
    lo, hi = scene_extent(runs, steps, shapes)
    fig, axes = plt.subplots(2 * len(runs), len(steps), figsize=(2.8 * len(steps), 2.8 * 2 * len(runs)), squeeze=False)
    for ri, r in enumerate(runs):
        for ci, s in enumerate(steps):
            for vi, view in enumerate(("oblique", "side")):
                ax = axes[2 * ri + vi][ci]
                if s < len(r["frames"]):
                    nv = len(r["frames"][s]["v"])
                    draw_frame(ax, r, s, view, shapes, lo, hi, "%s  step %d  %s  n=%d" % (labels[ri], s, view, nv))
                else:
                    ax.axis("off")
    fig.tight_layout()
    fig.savefig(out, dpi=100)
    plt.close(fig)
    print(out)


def frames(outdir, path, every, scen):
    r = read(path)
    shapes = parse_shapes(scen or auto_scenario(path))
    steps = list(range(0, len(r["frames"]), every))
    lo, hi = scene_extent([r], steps, shapes)
    os.makedirs(outdir, exist_ok=True)
    for s in steps:
        fig, axes = plt.subplots(1, 2, figsize=(9, 4.5))
        for ax, view in zip(axes, ("oblique", "side")):
            draw_frame(ax, r, s, view, shapes, lo, hi, "step %d %s" % (s, view))
        fig.tight_layout()
        fig.savefig(os.path.join(outdir, "frame_%04d.png" % s), dpi=90)
        plt.close(fig)
    print(outdir, len(steps), "frames")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("cmd", choices=["sheet", "frames"])
    ap.add_argument("out")
    ap.add_argument("inputs", nargs="+")
    ap.add_argument("--steps", default="")
    ap.add_argument("--labels", default="")
    ap.add_argument("--scenario", default="")
    ap.add_argument("--every", type=int, default=10)
    a = ap.parse_args()
    if a.cmd == "frames":
        frames(a.out, a.inputs[0], a.every, a.scenario)
        return
    n = read(a.inputs[0])["steps"]
    steps = [int(s) for s in a.steps.split(",")] if a.steps else sorted({int(round(i)) for i in np.linspace(0, n - 1, 6)})
    labels = a.labels.split(",") if a.labels else [os.path.basename(os.path.dirname(p)) + "/" + os.path.basename(p)[:-4]
                                                   for p in a.inputs]
    sheet(a.out, a.inputs, steps, labels, a.scenario)


if __name__ == "__main__":
    main()
