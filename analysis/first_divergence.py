#!/usr/bin/env python3
"""Report, per step, the worst particle mismatch between two dumps until it exceeds a threshold."""
import sys, os
import numpy as np
sys.path.insert(0, os.path.dirname(__file__))
from fluidbin import read
A, B = read(sys.argv[1]), read(sys.argv[2])
thr = float(sys.argv[3]) if len(sys.argv) > 3 else 1e-5
for s in range(A["steps"]):
    fa = A["frames"][s][1]; fb = B["frames"][s][1]
    fa = fa[np.argsort(fa["id"])]; fb = fb[np.argsort(fb["id"])]
    d = np.linalg.norm(fa["pos"] - fb["pos"], axis=1); i = int(np.argmax(d))
    print("%3d max %.2e  n>%g: %4d  worst id %d pos %s flags A/B %x/%x" % (s, d.max(), thr, int((d > thr).sum()),
          fa["id"][i], fa["pos"][i].round(4), fa["flags"][i], fb["flags"][i]))
    if d.max() > thr * 100: break
