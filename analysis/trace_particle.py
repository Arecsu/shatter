#!/usr/bin/env python3
"""Print one particle's trajectory from two dumps side by side.
  trace_particle.py A.bin B.bin [--id N | --worst-at STEP] [--from S --to E]"""
import argparse, os, sys
import numpy as np
sys.path.insert(0, os.path.dirname(__file__))
from fluidbin import read

ap = argparse.ArgumentParser()
ap.add_argument("a"); ap.add_argument("b")
ap.add_argument("--id", type=int); ap.add_argument("--worst-at", type=int)
ap.add_argument("--from", dest="s0", type=int, default=0); ap.add_argument("--to", dest="s1", type=int, default=60)
x = ap.parse_args()
A, B = read(x.a), read(x.b)
def get(R, s, pid):
    f = R["frames"][s][1]; m = f["id"] == pid
    return f[m][0] if m.any() else None
pid = x.id
if pid is None:
    s = x.worst_at
    fa, fb = A["frames"][s][1], B["frames"][s][1]
    fa = fa[np.argsort(fa["id"])]; fb = fb[np.argsort(fb["id"])]
    pid = int(fa["id"][np.argmax(np.linalg.norm(fa["pos"] - fb["pos"], axis=1))])
print("particle", pid)
for s in range(x.s0, min(x.s1, A["steps"])):
    a, b = get(A, s, pid), get(B, s, pid)
    fmt = lambda r: "pos=(%9.5f %9.5f %9.5f) vel=(%8.4f %8.4f %8.4f) n=(%5.2f %5.2f %5.2f) f=%x" % (*r["pos"], *r["vel"], *r["normal"], r["flags"])
    print("%3d A %s\n    B %s" % (s, fmt(a), fmt(b)))
