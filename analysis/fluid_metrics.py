#!/usr/bin/env python3
"""Statistical comparison of our fluid solver against the original core.

Fluid runs are chaotic, so per-particle error is meaningless after a few dozen steps in dense scenes.
Instead both sides are run as jittered ensembles (tools/fluid_stat.sh) and ours is judged against the
oracle's own run-to-run spread:

  scalar metric m(t) (count, centroid, robust extent, speed quantiles, density, ...):
      delta(t) = |mean_ours(t) - mean_oracle(t)|,  limit(t) = max(K * std_oracle(t), floor)
  distribution metric (marginal x/y/z position, speed):
      delta(t) = median 1-Wasserstein distance over (ours_i, oracle_j) pairs
      limit(t) = max(K * median W1 over (oracle_i, oracle_j) pairs, floor)
  scalar per run (settle time): same as a scalar metric.

A metric passes if delta <= limit for at least (1 - TOL) of the steps. NaN anywhere fails.

  fluid_metrics.py NAME [NAME...] [--json-dir DIR] [-K 3] [--tol 0.05]
    NAME = scenario basename; runs are captures/fluid/NAME.j*/{oracle-1,ours}.bin
"""
import argparse
import glob
import itertools
import json
import os
import re
import sys

import numpy as np

sys.path.insert(0, os.path.dirname(__file__))
from fluidbin import read  # noqa: E402

# Reported but not judged: the oracle's contact flag partly reflects broadphase latency, not behaviour.
INFO_ONLY = ("contact_frac",)

ROOT = os.path.normpath(os.path.join(os.path.dirname(__file__), ".."))
CAP = os.path.join(ROOT, "captures", "fluid")


def scenario_params(name):
    """Pull the few parameters the floors depend on out of the scenario text."""
    p = {"restParticlesPerMeter": 50.0, "restDensity": 1000.0, "simulationMethod": "sph"}
    path = next(iter(glob.glob(os.path.join(CAP, "_variants", name + ".j*.txt"))), None)
    for line in open(path) if path else []:
        m = re.match(r"\s*fluid\.(\w+)\s+(\S+)", line.split("#")[0])
        if m and m.group(1) in p:
            p[m.group(1)] = m.group(2) if m.group(1) == "simulationMethod" else float(m.group(2))
    return p


def per_step_scalars(frames, sph):
    """frames -> dict name -> array[steps]."""
    keys = ["count"] + [f"centroid_{a}" for a in "xyz"] + [f"p01_{a}" for a in "xyz"] + \
           [f"p99_{a}" for a in "xyz"] + ["speed_mean", "speed_p50", "speed_p95", "contact_frac"]
    if sph:
        keys.append("density_mean")
    out = {k: np.full(len(frames), np.nan) for k in keys}
    nan = False
    for s, (_, f) in enumerate(frames):
        out["count"][s] = len(f)
        if not len(f):
            continue
        p, v = f["pos"].astype(np.float64), f["vel"].astype(np.float64)
        nan |= bool(np.isnan(p).any() or np.isnan(v).any())
        sp = np.linalg.norm(v, axis=1)
        q01, q99 = np.percentile(p, [1, 99], axis=0)
        for i, a in enumerate("xyz"):
            out[f"centroid_{a}"][s] = p[:, i].mean()
            out[f"p01_{a}"][s] = q01[i]
            out[f"p99_{a}"][s] = q99[i]
        out["speed_mean"][s] = sp.mean()
        out["speed_p50"][s], out["speed_p95"][s] = np.percentile(sp, [50, 95])
        out["contact_frac"][s] = np.mean((f["flags"] & 3) != 0)  # static|dynamic contact flags
        if sph:
            out["density_mean"][s] = f["density"].mean()
    return out, nan


def settle_step(speed_p95, thr):
    """First step after which the 95th-percentile speed stays below thr (len if never)."""
    above = np.nonzero(speed_p95 >= thr)[0]
    return float(above[-1] + 1) if len(above) else 0.0


def w1(a, b):
    """1-Wasserstein distance between two 1-D samples (exact, via quantile functions)."""
    if not len(a) or not len(b):
        return np.nan
    q = np.linspace(0, 1, 257)[1:-1]
    return float(np.mean(np.abs(np.quantile(a, q) - np.quantile(b, q))))


def dist_samples(frames, s):
    f = frames[s][1]
    return {"pos_x": f["pos"][:, 0], "pos_y": f["pos"][:, 1], "pos_z": f["pos"][:, 2],
            "speed": np.linalg.norm(f["vel"], axis=1)}


def judge(delta, limit, tol):
    ok = np.isfinite(delta)
    ratio = np.where(ok, delta / limit, np.inf)
    bad = ratio > 1.0
    worst = int(np.argmax(ratio))
    return {"pass": bool(bad.mean() <= tol), "fail_frac": round(float(bad.mean()), 4),
            "max_ratio": round(float(ratio[worst]), 3), "worst_step": worst}


def analyse(name, K, tol, dist_every):
    oracle_paths = sorted(glob.glob(os.path.join(CAP, name + ".j*", "oracle-1.bin")))
    ours_paths = sorted(glob.glob(os.path.join(CAP, name + ".j*", "ours.bin")))
    if len(oracle_paths) < 2 or not ours_paths:
        raise SystemExit(f"{name}: need >=2 oracle and >=1 ours runs (have {len(oracle_paths)}, {len(ours_paths)})")
    prm = scenario_params(name)
    sph = prm["simulationMethod"] != "none"
    spacing = 1.0 / prm["restParticlesPerMeter"]
    O = [read(p) for p in oracle_paths]
    U = [read(p) for p in ours_paths]
    steps = min(r["steps"] for r in O + U)
    dt = O[0]["dt"]

    floors = {"count": None, "centroid": 0.5 * spacing, "p01": spacing, "p99": spacing, "speed": 0.05,
              "contact_frac": 0.02, "density_mean": 0.02 * prm["restDensity"], "pos": 0.5 * spacing,
              "settle_steps": 10.0}

    def floor_for(k):
        for pre, v in floors.items():
            if k.startswith(pre):
                return v
        raise KeyError(k)

    So, nan_o = zip(*(per_step_scalars(r["frames"][:steps], sph) for r in O))
    Su, nan_u = zip(*(per_step_scalars(r["frames"][:steps], sph) for r in U))
    report = {"scenario": name, "runs_oracle": len(O), "runs_ours": len(U), "steps": steps, "K": K, "tol": tol,
              "nan_oracle": any(nan_o), "nan_ours": any(nan_u), "metrics": {}}

    for k in So[0]:
        mo = np.stack([s[k] for s in So]); mu = np.stack([s[k] for s in Su])
        delta = np.abs(mu.mean(0) - mo.mean(0))
        fl = floor_for(k)
        if fl is None:  # count: exact up to 0.5 %
            fl = np.maximum(1.0, 0.005 * mo.mean(0))
        limit = np.maximum(K * mo.std(0, ddof=1), fl)
        r = judge(delta, limit, tol)
        r.update(oracle_end=round(float(mo.mean(0)[-1]), 5), ours_end=round(float(mu.mean(0)[-1]), 5),
                 spread_end=round(float(mo.std(0, ddof=1)[-1]), 5))
        report["metrics"][k] = r

    # settle time: per-run scalar
    thr = 0.05 if not sph else 0.1
    so = np.array([settle_step(s["speed_p95"], thr) for s in So])
    su = np.array([settle_step(s["speed_p95"], thr) for s in Su])
    lim = max(K * so.std(ddof=1), floors["settle_steps"])
    d = abs(su.mean() - so.mean())
    report["metrics"]["settle_steps"] = {"pass": bool(d <= lim), "oracle_mean": float(so.mean()),
                                         "ours_mean": float(su.mean()), "spread": float(so.std(ddof=1)),
                                         "max_ratio": round(float(d / lim), 3), "threshold_speed_p95": thr,
                                         "never_settled": int(steps)}

    # distribution metrics (every dist_every steps; W1 is the costly part)
    idx = list(range(0, steps, dist_every)) + ([steps - 1] if (steps - 1) % dist_every else [])
    dkeys = ["pos_x", "pos_y", "pos_z", "speed"]
    delta = {k: np.zeros(len(idx)) for k in dkeys}
    limit = {k: np.zeros(len(idx)) for k in dkeys}
    within_med = {k: np.zeros(len(idx)) for k in dkeys}
    for j, s in enumerate(idx):
        do = [dist_samples(r["frames"], s) for r in O]
        du = [dist_samples(r["frames"], s) for r in U]
        for k in dkeys:
            within = [w1(a[k], b[k]) for a, b in itertools.combinations(do, 2)]
            cross = [w1(a[k], b[k]) for a in du for b in do]
            fl = floors["speed"] if k == "speed" else floors["pos"]
            delta[k][j] = np.median(cross)
            within_med[k][j] = np.median(within)
            limit[k][j] = max(K * within_med[k][j], fl)
    for k in dkeys:
        r = judge(delta[k], limit[k], tol)
        r["worst_step"] = idx[r["worst_step"]]
        r.update(w1_end=round(float(delta[k][-1]), 5), w1_oracle_end=round(float(within_med[k][-1]), 5))
        report["metrics"]["dist_" + k] = r

    # timing (report only; oracle runs under Wine, single thread)
    t_o = np.median([np.median([t for t, _ in r["frames"][:steps]]) for r in O])
    t_u = np.median([np.median([t for t, _ in r["frames"][:steps]]) for r in U])
    report["timing_ms"] = {"oracle_median_step": round(1e3 * t_o, 3), "ours_median_step": round(1e3 * t_u, 3)}
    for k in INFO_ONLY:
        if k in report["metrics"]:
            report["metrics"][k]["info_only"] = True
    report["pass"] = (not report["nan_ours"]) and all(m["pass"] for m in report["metrics"].values() if not m.get("info_only"))
    report["dt"] = dt
    return report


def print_report(r):
    print(f"== {r['scenario']}: {'PASS' if r['pass'] else 'FAIL'}  (oracle x{r['runs_oracle']}, ours x{r['runs_ours']},"
          f" {r['steps']} steps, K={r['K']}, tol={r['tol']}, nan ours={r['nan_ours']})")
    print(f"   step time ms: oracle {r['timing_ms']['oracle_median_step']}  ours {r['timing_ms']['ours_median_step']}")
    for k, m in r["metrics"].items():
        extra = ""
        if "oracle_end" in m:
            extra = f"end oracle {m['oracle_end']:.4g} ±{m['spread_end']:.2g}  ours {m['ours_end']:.4g}"
        elif "w1_end" in m:
            extra = f"end W1 ours-oracle {m['w1_end']:.3g}  oracle-oracle {m['w1_oracle_end']:.3g}"
        elif k == "settle_steps":
            extra = f"oracle {m['oracle_mean']:.1f} ±{m['spread']:.1f}  ours {m['ours_mean']:.1f}"
        ff = f"fail {m['fail_frac']:.0%} worst@{m['worst_step']:<4d}" if "fail_frac" in m else " " * 21
        tag = "-- " if m.get("info_only") else ("ok " if m["pass"] else "BAD")
        print(f"   {tag} {k:16s} max_ratio {m['max_ratio']:7.2f}  {ff}  {extra}")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("names", nargs="+")
    ap.add_argument("-K", type=float, default=3.0)
    ap.add_argument("--tol", type=float, default=0.05)
    ap.add_argument("--dist-every", type=int, default=5)
    ap.add_argument("--json-dir", default=os.path.join(CAP, "reports"))
    a = ap.parse_args()
    os.makedirs(a.json_dir, exist_ok=True)
    ok = True
    for n in a.names:
        r = analyse(n, a.K, a.tol, a.dist_every)
        print_report(r)
        json.dump(r, open(os.path.join(a.json_dir, n + ".json"), "w"), indent=1)
        ok &= r["pass"]
    sys.exit(0 if ok else 1)


if __name__ == "__main__":
    main()
