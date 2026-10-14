#!/usr/bin/env python3
# usage: cloth_isect.py frames.bin [every]  (frames.bin from cloth_replay --frames or cloth_replay_oracle; for game
# recordings cloth_replay --isect n counts the same without a dump)
# Count self-intersections in a cloth_replay --frames dump: edges of the cloth that pierce one of its own triangles
# (edge and triangle share no vertex). Prints per sampled frame: step index, alive triangles, piercing edge-triangle pairs.
import sys, struct, numpy as np

def frames(path):
    f = open(path, "rb")
    magic, ver, n, _, nrest, ntri = struct.unpack("<6I", f.read(24))
    f.read(4)
    tri = np.frombuffer(f.read(ntri * 12), np.uint32).reshape(-1, 3)
    f.read(nrest * 12)
    for k in range(n):
        nv, nt = struct.unpack("<2I", f.read(8)); f.read(8); has, = struct.unpack("<I", f.read(4))
        if has: tri = np.frombuffer(f.read(nt * 12), np.uint32).reshape(-1, 3)
        pv = np.frombuffer(f.read(nv * 24), np.float32).reshape(-1, 6)[:, :3].astype(np.float64)
        yield k, tri, pv

def count(tri, p):
    tri = tri[(tri[:, 0] != tri[:, 1]) | (tri[:, 1] != tri[:, 2])]  # dead triangles are (a,a,a)
    if len(tri) == 0: return 0, 0
    e = np.concatenate([tri[:, [0, 1]], tri[:, [1, 2]], tri[:, [2, 0]]]); e.sort(1); e = np.unique(e, axis=0)
    A, B, C = p[tri[:, 0]], p[tri[:, 1]], p[tri[:, 2]]
    tlo, thi = np.minimum(np.minimum(A, B), C), np.maximum(np.maximum(A, B), C)
    P, Q = p[e[:, 0]], p[e[:, 1]]
    elo, ehi = np.minimum(P, Q), np.maximum(P, Q)
    cell = max(np.median(np.linalg.norm(Q - P, axis=1)) * 2, 1e-4)
    from collections import defaultdict
    grid = defaultdict(list)
    lo = np.floor(tlo / cell).astype(int); hi = np.floor(thi / cell).astype(int)
    for t in range(len(tri)):
        for x in range(lo[t, 0], hi[t, 0] + 1):
            for y in range(lo[t, 1], hi[t, 1] + 1):
                for z in range(lo[t, 2], hi[t, 2] + 1): grid[(x, y, z)].append(t)
    pe, pt = [], []
    elo_c = np.floor(elo / cell).astype(int); ehi_c = np.floor(ehi / cell).astype(int)
    for i in range(len(e)):
        cand = set()
        for x in range(elo_c[i, 0], ehi_c[i, 0] + 1):
            for y in range(elo_c[i, 1], ehi_c[i, 1] + 1):
                for z in range(elo_c[i, 2], ehi_c[i, 2] + 1): cand.update(grid.get((x, y, z), ()))
        for t in cand: pe.append(i); pt.append(t)
    if not pe: return len(tri), 0
    pe, pt = np.array(pe), np.array(pt)
    tv = tri[pt]; ev = e[pe]
    share = (tv == ev[:, :1]).any(1) | (tv == ev[:, 1:]).any(1)
    pe, pt = pe[~share], pt[~share]
    a, b, c = A[pt], B[pt], C[pt]; s, d = P[pe], Q[pe] - P[pe]
    e1, e2 = b - a, c - a
    h = np.cross(d, e2); det = (e1 * h).sum(1)
    ok = np.abs(det) > 1e-12; inv = np.where(ok, 1 / np.where(ok, det, 1), 0)
    sv = s - a; u = (sv * h).sum(1) * inv
    qv = np.cross(sv, e1); v = (d * qv).sum(1) * inv; w = (e2 * qv).sum(1) * inv
    hit = ok & (u >= 0) & (v >= 0) & (u + v <= 1) & (w > 0) & (w < 1)
    return len(tri), int(hit.sum())

every = int(sys.argv[2]) if len(sys.argv) > 2 else 50
for k, tri, p in frames(sys.argv[1]):
    if k % every == 0:
        nt, n = count(tri, p)
        print(k, nt, n)
