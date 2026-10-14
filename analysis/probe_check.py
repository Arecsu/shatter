#!/usr/bin/env python3
"""Bit-exactness regression for the single-particle probes: ours.bin vs oracle-1.bin per probe.

  probe_check.py NAME...        # names are captures/fluid/<NAME>/
Prints max position error over the run and the first step above TOL. Probes listed in KNOWN measure
original artifacts we deliberately don't copy, so the oracle can't judge them; they are checked instead
against our own accepted output in scenarios/fluid/probe_golden.json (per-step position and velocity).
  probe_check.py --update-golden NAME...   # accept the current ours.bin for the KNOWN probes
PROBE_OURS=<dir> probe_check.py NAME...  # read ours from <dir>/NAME/ours.bin (default captures/fluid/)
"""
import json
import os
import sys

import numpy as np

sys.path.insert(0, __file__.rsplit("/", 1)[0])
import fluidbin  # noqa: E402

TOL = 1e-5
# Original artifacts (see docs/re-notes-fluid.md): fresh-particle broadphase latency and tunnelling,
# plus static friction (2.8.0 has none; the 2.8.1 oracle does).
KNOWN = {
    # fresh-particle latency: closer to (or exact with) the oracle under PXF_AGE_LATENCY=2 (newParticleShapeLatency); the
    # goldens are the default (0) outputs
    "prox_": "fresh-particle latency (exact at PXF_AGE_LATENCY=2)", "proxbox_": "fresh-particle latency (exact at PXF_AGE_LATENCY=2)",
    "rebound_0.020": "fresh-particle latency (max 1.7e-3 at PXF_AGE_LATENCY=2)", "rebound_0.024": "fresh-particle latency (max 1.7e-3 at PXF_AGE_LATENCY=2)",
    "rebound_0.028": "fresh-particle latency (max 1.7e-3 at PXF_AGE_LATENCY=2)", "rebound_0.032": "fresh-particle latency (exact at PXF_AGE_LATENCY=2)",
    "a_full": "tunnelling", "b_noplane": "tunnelling", "c_nolimit": "tunnelling", "d_slower": "tunnelling",
    "stick_": "static friction (2.8.1 only)", "slide_sf0.05": "static friction (2.8.1 only)",
    "slide_sf0.5": "static friction (2.8.1 only)", "slide_sf1.0": "static friction (2.8.1 only)",
    "mesh_drop$": "mesh impact restores start position",
    "dyn_kin$": "static plane-cache retention fit (step 101, away from the box)",
    "dyn_rest$": "resting box micro-jitter not in the replayed trajectory (1e-5 drift)",
    "dyn_twoway_off$": "resting box micro-jitter not in the replayed trajectory (1e-5 drift)",
    "dyn_twoway_noshapeflag$": "resting box micro-jitter not in the replayed trajectory (1e-5 drift)",
    "dyn_twoway$": "second contact with a body moving from fluid impulses (oracle impulse/rigid ordering)",
    # probe_drain: the oracle's plane impact for a 16-particle block is one step earlier than ours at step 46 (pre-existing,
    # HEAD gives the same output; plane16_ctrl). Pass-through probes are checked against that control: ours and the
    # oracle's outputs are bit-identical to plane16_ctrl's (docs/re-notes-fluid-drain.md).
    "plane16_ctrl$": "16-particle block plane impact one step earlier in the oracle (pre-existing)",
    "shape_disable_coll$": "pass-through: same as plane16_ctrl (plane impact timing)",
    "shape_disable_resp$": "pass-through: same as plane16_ctrl (plane impact timing)",
    "shape_fluid_disable_coll$": "pass-through: same as plane16_ctrl (plane impact timing)",
    "actor_disable_coll$": "pass-through: same as plane16_ctrl (plane impact timing)",
    "actor_disable_resp$": "pass-through: same as plane16_ctrl (plane impact timing)",
    "actor_fluid_disable_coll$": "pass-through: same as plane16_ctrl (plane impact timing)",
    "comp_box_rb$": "pass-through: same as plane16_ctrl (plane impact timing)",
    "trigger_on$": "pass-through: same as plane16_ctrl (plane impact timing)",
    "drain_trigger$": "pass-through: same as plane16_ctrl (plane impact timing)",
    "grp_scene_off$": "pass-through: same as plane16_ctrl (plane impact timing)",
    # fresh particle inside collision distance: the oracle does not push it out in its first 2 steps (fresh-particle latency)
    "drain_prox_0.001$": "fresh-particle latency: no push-out in steps 0-1 (exact at PXF_AGE_LATENCY=2)",
    "drain_prox_0.005$": "fresh-particle latency: no push-out in steps 0-1 (exact at PXF_AGE_LATENCY=2)",
}


def known(name):
    # a key ending in "$" matches the whole name, any other key is a prefix
    return next((why for k, why in KNOWN.items()
                 if (name == k[:-1] if k.endswith("$") else name.startswith(k))), None)


def golden_rows(r):
    return [[f["pos"].round(7).tolist(), f["vel"].round(7).tolist()]
            for _, f in ((t, f[np.argsort(f["id"])]) for t, f in r["frames"])]


def golden_err(rows, ref):
    if len(rows) != len(ref):
        return float("inf")
    e = 0.0
    for (p, v), (rp, rv) in zip(rows, ref):
        if len(p) != len(rp):
            return float("inf")
        e = max(e, float(np.abs(np.array(p) - rp).max(initial=0)), float(np.abs(np.array(v) - rv).max(initial=0)))
    return e


def main():
    base = __file__.rsplit("/", 2)[0]
    root = base + "/captures/fluid/"
    gpath = base + "/scenarios/fluid/probe_golden.json"
    update = "--update-golden" in sys.argv
    names = [a for a in sys.argv[1:] if not a.startswith("--")]
    try:
        golden = json.load(open(gpath))
    except FileNotFoundError:
        golden = {}
    bad = 0
    for name in names:
        try:
            ours_dir = os.environ.get("PROBE_OURS", root.rstrip("/"))
            a = fluidbin.read(os.path.join(ours_dir, name, "ours.bin"))
            b = fluidbin.read(root + name + "/oracle-1.bin")
        except FileNotFoundError as e:
            print(f"{name:28s} MISSING {e.filename}")
            bad += 1
            continue
        rows = fluidbin.compare(a, b)
        err = np.array([r["pos_err_max"] for r in rows])
        cnt = any(r["count_a"] != r["count_b"] for r in rows)
        first = int(np.argmax(err > TOL)) if (err > TOL).any() else None
        ok = first is None and not cnt
        why = known(name)
        tag = "ok" if ok else "FAIL"
        if why and not ok:  # oracle disagrees as expected: judge against our accepted output instead
            rows = golden_rows(a)
            if update:
                golden[name] = rows
            ge = golden_err(rows, golden[name]) if name in golden else None
            tag = (f"golden ok ({why})" if ge is not None and ge <= TOL
                   else f"GOLDEN {'MISSING' if ge is None else 'FAIL %.1e' % ge}")
            ok = ge is not None and ge <= TOL
        if not ok:
            bad += 1
        print(f"{name:28s} {tag:38s} max {err.max():.2e}  first>tol {first}  count_mismatch {cnt}")
    if update:
        json.dump(golden, open(gpath, "w"), separators=(",", ":"))
        print(f"wrote {len(golden)} golden probes to {gpath}")
    print(f"{len(names)} probes, {bad} unexpected failures")
    sys.exit(1 if bad else 0)


if __name__ == "__main__":
    main()
