#!/usr/bin/env python3
"""Read PXFD fluid dumps (harness/src/fluid_oracle.cpp format) and compute metrics.

  fluidbin.py summary FILE.bin                 # per-step metrics table
  fluidbin.py compare A.bin B.bin [--json]     # per-step differences (matched by particle id)
"""
import argparse
import json
import struct
import sys

import numpy as np

PDT = np.dtype([("pos", "<f4", 3), ("vel", "<f4", 3), ("density", "<f4"), ("life", "<f4"),
                ("normal", "<f4", 3), ("id", "<u4"), ("flags", "<u4")])


def read(path):
    data = open(path, "rb").read()
    assert data[:4] == b"PXFD", "bad magic"
    version, steps, maxp = struct.unpack_from("<III", data, 4)
    dt = struct.unpack_from("<f", data, 16)[0]
    off = 20
    frames = []
    for _ in range(steps):
        n = struct.unpack_from("<I", data, off)[0]
        t = struct.unpack_from("<d", data, off + 4)[0]
        off += 12
        arr = np.frombuffer(data, PDT, n, off).copy()
        off += n * PDT.itemsize
        frames.append((t, arr))
    return {"version": version, "steps": steps, "max": maxp, "dt": dt, "frames": frames}


def frame_metrics(arr):
    if len(arr) == 0:
        return {"count": 0}
    p, v = arr["pos"], arr["vel"]
    speed = np.linalg.norm(v, axis=1)
    return {
        "count": int(len(arr)),
        "centroid": p.mean(0).round(5).tolist(),
        "bbox_min": p.min(0).round(5).tolist(),
        "bbox_max": p.max(0).round(5).tolist(),
        "speed_mean": float(speed.mean()),
        "speed_p95": float(np.percentile(speed, 95)),
        "density_mean": float(arr["density"].mean()),
        "nan": bool(np.isnan(p).any() or np.isnan(v).any()),
        "flags_hist": {hex(int(k)): int(c) for k, c in zip(*np.unique(arr["flags"], return_counts=True))},
    }


def compare(a, b):
    out = []
    for s, ((_, fa), (_, fb)) in enumerate(zip(a["frames"], b["frames"])):
        ia = np.argsort(fa["id"]); ib = np.argsort(fb["id"])
        fa, fb = fa[ia], fb[ib]
        common = np.intersect1d(fa["id"], fb["id"])
        ma = np.isin(fa["id"], common); mb = np.isin(fb["id"], common)
        d = np.linalg.norm(fa["pos"][ma] - fb["pos"][mb], axis=1) if common.size else np.zeros(1)
        out.append({"step": s, "count_a": int(len(fa)), "count_b": int(len(fb)),
                    "pos_err_max": float(d.max()), "pos_err_rms": float(np.sqrt((d ** 2).mean()))})
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("cmd", choices=["summary", "compare"])
    ap.add_argument("files", nargs="+")
    ap.add_argument("--json", action="store_true")
    ap.add_argument("--every", type=int, default=30)
    a = ap.parse_args()
    if a.cmd == "summary":
        r = read(a.files[0])
        rows = [dict(step=i, sim_ms=t * 1e3, **frame_metrics(f)) for i, (t, f) in enumerate(r["frames"])]
        if a.json:
            json.dump(rows, sys.stdout, indent=1)
            return
        for row in rows:
            if row["step"] % a.every == 0 or row["step"] == len(rows) - 1:
                print("step %4d n=%5d  centroid=%s  min=%s  max=%s  |v|=%.3f p95=%.3f rho=%.1f ms=%.2f flags=%s" % (
                    row["step"], row["count"], row.get("centroid"), row.get("bbox_min"), row.get("bbox_max"),
                    row.get("speed_mean", 0), row.get("speed_p95", 0), row.get("density_mean", 0),
                    row["sim_ms"], row.get("flags_hist")))
    else:
        rows = compare(read(a.files[0]), read(a.files[1]))
        if a.json:
            json.dump(rows, sys.stdout, indent=1)
            return
        for row in rows:
            if row["step"] % a.every == 0 or row["step"] == len(rows) - 1:
                print(row)
        print("max over run: pos_err_max=%.3g" % max(r["pos_err_max"] for r in rows))


if __name__ == "__main__":
    main()
