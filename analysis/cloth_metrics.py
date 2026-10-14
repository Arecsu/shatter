#!/usr/bin/env python3
"""Compare our cloth solver against the oracle, per scenario.

  cloth_metrics.py [NAME ...] [--ours ours-1.bin] [--oracle oracle-1.bin] [--json]

For each captures/cloth/<name>: vertex-wise position error (rms / max) at early, mid and final steps, final centroid
and bbox deltas, mean/max edge strain (oracle vs ours), mean speed at the end, vertex counts. The cloth is
deterministic, so unlike fluid a vertex-wise comparison is meaningful; the max over the run is also reported.
"""
import argparse
import glob
import json
import os
import sys

import numpy as np

sys.path.insert(0, os.path.dirname(__file__))
from clothbin import frame_metrics, read, rest_lengths  # noqa: E402

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


def pos_err(fa, fb):
    n = min(len(fa["v"]), len(fb["v"]))
    d = np.linalg.norm(fa["v"]["pos"][:n] - fb["v"]["pos"][:n], axis=1)
    return float(np.sqrt((d ** 2).mean())), float(d.max())


def compare_scenario(name, ours="ours-1.bin", oracle="oracle-1.bin"):
    d = os.path.join(ROOT, "captures", "cloth", name)
    a, b = read(os.path.join(d, oracle)), read(os.path.join(d, ours))
    e, rest = rest_lengths(a)
    n = min(len(a["frames"]), len(b["frames"]))
    errs = [pos_err(a["frames"][s], b["frames"][s]) for s in range(n)]
    last = n - 1
    mid = n // 2
    ma, mb = frame_metrics(a["frames"][last], e, rest), frame_metrics(b["frames"][last], e, rest)
    sca = np.array(ma["centroid"]); scb = np.array(mb["centroid"])
    bba = np.array([ma["bbox_min"], ma["bbox_max"]]); bbb = np.array([mb["bbox_min"], mb["bbox_max"]])
    # extent scale for normalising
    ext = float(np.linalg.norm(bba[1] - bba[0])) + 1e-9
    cen = [np.linalg.norm(np.array(frame_metrics(a["frames"][i], e, rest).get("centroid", [0, 0, 0])) -
                          np.array(frame_metrics(b["frames"][i], e, rest).get("centroid", [0, 0, 0]))) for i in range(n)]
    return {
        "scenario": name, "steps": n, "centroid_delta_max": float(max(cen)), "centroid_delta_mean": float(np.mean(cen)),
        "n_oracle": ma["count"], "n_ours": mb["count"],
        "rms@1": errs[min(1, last)][0], "rms@mid": errs[mid][0], "rms@end": errs[last][0],
        "max@end": errs[last][1], "rms_worst": max(x[0] for x in errs),
        "centroid_delta_end": float(np.linalg.norm(sca - scb)),
        "bbox_delta_end": float(np.abs(bba - bbb).max()),
        "extent": ext,
        "strain_mean_oracle": ma["strain_mean"], "strain_mean_ours": mb["strain_mean"],
        "strain_max_oracle": ma["strain_max"], "strain_max_ours": mb["strain_max"],
        "speed_oracle": ma["speed_mean"], "speed_ours": mb["speed_mean"],
        "min_y_oracle": ma["bbox_min"][1], "min_y_ours": mb["bbox_min"][1],
    }


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("names", nargs="*")
    ap.add_argument("--ours", default="ours-1.bin")
    ap.add_argument("--oracle", default="oracle-1.bin")
    ap.add_argument("--json", action="store_true")
    a = ap.parse_args()
    names = a.names or sorted(os.path.basename(p) for p in glob.glob(os.path.join(ROOT, "captures/cloth/*"))
                              if os.path.exists(os.path.join(p, a.ours)) and os.path.exists(os.path.join(p, a.oracle)))
    rows = [compare_scenario(n, a.ours, a.oracle) for n in names]
    if a.json:
        json.dump(rows, sys.stdout, indent=1)
        return
    print("%-20s %5s %9s %9s %9s %9s %9s %9s | strain mean o/u | strain max o/u | |v| o/u | minY o/u" % (
        "scenario", "steps", "rms@1", "rms@mid", "rms@end", "max@end", "rmsWorst", "dCentr"))
    print("(dCentr = final centroid distance; per-run mean/max of the centroid distance are in --json)")
    for r in rows:
        print("%-20s %5d %9.2e %9.2e %9.2e %9.2e %9.2e %9.2e | %.4f/%.4f | %.4f/%.4f | %.3f/%.3f | %.3f/%.3f%s" % (
            r["scenario"], r["steps"], r["rms@1"], r["rms@mid"], r["rms@end"], r["max@end"], r["rms_worst"],
            r["centroid_delta_end"], r["strain_mean_oracle"], r["strain_mean_ours"], r["strain_max_oracle"],
            r["strain_max_ours"], r["speed_oracle"], r["speed_ours"], r["min_y_oracle"], r["min_y_ours"],
            "" if r["n_oracle"] == r["n_ours"] else "  VERTEX COUNT %d vs %d" % (r["n_oracle"], r["n_ours"])))


if __name__ == "__main__":
    main()
