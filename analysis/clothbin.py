#!/usr/bin/env python3
"""Read PXCL cloth dumps (harness/src/cloth_oracle.cpp format) and compute metrics.

  clothbin.py summary FILE.bin [--every N] [--json]   # per-step metrics table
  clothbin.py compare A.bin B.bin [--every N] [--json] # per-step differences (matched by vertex index)

Per-step metrics: vertex/triangle count, centroid, bbox, mean/p95 speed, edge strain (current length / rest length
of the step-0-mesh edges: max and mean |strain-1|), and NaN.
"""
import argparse
import json
import struct
import sys

import numpy as np

VDT = np.dtype([("pos", "<f4", 3), ("vel", "<f4", 3)])


def read(path):
    data = open(path, "rb").read()
    assert data[:4] == b"PXCL", "bad magic"
    version, steps, maxv, nv0, nt0 = struct.unpack_from("<5I", data, 4)
    dt = struct.unpack_from("<f", data, 24)[0]
    off = 28
    tri0 = np.frombuffer(data, "<u4", nt0 * 3, off).reshape(-1, 3).copy()
    off += nt0 * 12
    rest_pos = np.frombuffer(data, "<f4", nv0 * 3, off).reshape(-1, 3).copy()
    off += nv0 * 12
    frames = []
    idx = tri0
    for _ in range(steps):
        if off >= len(data):
            break
        nv, nt = struct.unpack_from("<II", data, off)
        t = struct.unpack_from("<d", data, off + 8)[0]
        has_idx = struct.unpack_from("<I", data, off + 16)[0]
        off += 20
        if has_idx:
            idx = np.frombuffer(data, "<u4", nt * 3, off).reshape(-1, 3).copy()
            off += nt * 12
        arr = np.frombuffer(data, VDT, nv, off).copy()
        off += nv * VDT.itemsize
        frames.append({"t": t, "v": arr, "tri": idx})
    return {"version": version, "steps": steps, "max": maxv, "nv0": nv0, "nt0": nt0, "dt": dt,
            "tri0": tri0, "rest_pos": rest_pos, "frames": frames}


def edges(tri):
    e = np.concatenate([tri[:, [0, 1]], tri[:, [1, 2]], tri[:, [2, 0]]])
    e.sort(axis=1)
    return np.unique(e, axis=0)


def frame_metrics(fr, e, rest):
    p, v = fr["v"]["pos"], fr["v"]["vel"]
    if len(p) == 0:
        return {"count": 0}
    speed = np.linalg.norm(v, axis=1)
    ok = (e < len(p)).all(axis=1)
    L = np.linalg.norm(p[e[ok, 1]] - p[e[ok, 0]], axis=1)
    strain = L / rest[ok] - 1.0
    return {
        "count": int(len(p)),
        "triangles": int(len(fr["tri"])),
        "centroid": p.mean(0).round(5).tolist(),
        "bbox_min": p.min(0).round(5).tolist(),
        "bbox_max": p.max(0).round(5).tolist(),
        "speed_mean": float(speed.mean()),
        "speed_p95": float(np.percentile(speed, 95)),
        "strain_max": float(np.abs(strain).max()) if strain.size else 0.0,
        "strain_mean": float(np.abs(strain).mean()) if strain.size else 0.0,
        "nan": bool(np.isnan(p).any() or np.isnan(v).any()),
    }


def rest_lengths(r):
    e = edges(r["tri0"])
    p0 = r["rest_pos"]
    return e, np.linalg.norm(p0[e[:, 1]] - p0[e[:, 0]], axis=1)


def summary_rows(r):
    e, rest = rest_lengths(r)
    return [dict(step=i, sim_ms=fr["t"] * 1e3, **frame_metrics(fr, e, rest)) for i, fr in enumerate(r["frames"])]


def compare(a, b):
    out = []
    for s, (fa, fb) in enumerate(zip(a["frames"], b["frames"])):
        n = min(len(fa["v"]), len(fb["v"]))
        d = np.linalg.norm(fa["v"]["pos"][:n] - fb["v"]["pos"][:n], axis=1) if n else np.zeros(1)
        dv = np.linalg.norm(fa["v"]["vel"][:n] - fb["v"]["vel"][:n], axis=1) if n else np.zeros(1)
        out.append({"step": s, "count_a": int(len(fa["v"])), "count_b": int(len(fb["v"])),
                    "pos_err_max": float(d.max()), "pos_err_rms": float(np.sqrt((d ** 2).mean())),
                    "vel_err_max": float(dv.max()),
                    "bitwise_equal": bool(n == len(fa["v"]) == len(fb["v"]) and
                                          fa["v"].tobytes() == fb["v"].tobytes())})
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("cmd", choices=["summary", "compare"])
    ap.add_argument("files", nargs="+")
    ap.add_argument("--json", action="store_true")
    ap.add_argument("--every", type=int, default=30)
    a = ap.parse_args()
    if a.cmd == "summary":
        rows = summary_rows(read(a.files[0]))
        if a.json:
            json.dump(rows, sys.stdout, indent=1)
            return
        for row in rows:
            if row["step"] % a.every == 0 or row["step"] == len(rows) - 1:
                print("step %4d n=%5d tri=%5d centroid=%s min=%s max=%s |v|=%.3f p95=%.3f strain max=%.4f mean=%.4f "
                      "ms=%.2f%s" % (row["step"], row["count"], row.get("triangles", 0), row.get("centroid"),
                                     row.get("bbox_min"), row.get("bbox_max"), row.get("speed_mean", 0),
                                     row.get("speed_p95", 0), row.get("strain_max", 0), row.get("strain_mean", 0),
                                     row["sim_ms"], " NaN" if row.get("nan") else ""))
    else:
        rows = compare(read(a.files[0]), read(a.files[1]))
        if a.json:
            json.dump(rows, sys.stdout, indent=1)
            return
        for row in rows:
            if row["step"] % a.every == 0 or row["step"] == len(rows) - 1:
                print(row)
        print("max over run: pos_err_max=%.3g  all steps bitwise equal: %s" % (
            max(r["pos_err_max"] for r in rows), all(r["bitwise_equal"] for r in rows)))


if __name__ == "__main__":
    main()
