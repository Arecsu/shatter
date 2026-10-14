#!/usr/bin/env python3
"""Read the shim's fluid trace (PXFT, format in shim/fluid_wrap.cpp).

  fluid_trace.py summary FILE.trace                  # record counts, fluids, steps
  fluid_trace.py dump FILE.trace [--fluid N]       # per step: API calls, particle ids/life, id buffers, packets
  fluid_trace.py check FILE.trace ORACLE.bin [--fluid N]
      # the trace's PARTICLES records of fluid N vs the oracle's PXFD frames: byte-identical particle data?
  fluid_trace.py compare A.trace B.trace [--fluid N]
      # per step: particle count, id set, created ids per add, deleted id set, max |life| and |position| diff
      # (particles matched by id), max |world bounds| diff
"""
import argparse
import collections
import struct
import sys

TAGS = {1: "CREATE", 2: "RELEASE", 3: "ADD", 4: "UPDATE", 5: "REMOVEALL", 6: "SIMULATE", 7: "FETCHED",
        8: "PARTICLES", 9: "CREATED_IDS", 10: "DELETED_IDS", 11: "PACKETS", 12: "CREATED_AFTER_ADD", 13: "WORLD_BOUNDS", 14: "WRITE_COUNT_AFTER_ADD"}
REC = 52  # pos3 vel3 density life normal3 id flags


def records(path):
    data = open(path, "rb").read()
    assert data[:4] == b"PXFT", "bad magic"
    off = 8
    while off + 12 <= len(data):
        tag, fluid, n = struct.unpack_from("<III", data, off)
        off += 12
        yield tag, fluid, data[off:off + n]
        off += n


def pxfd_frames(path):
    data = open(path, "rb").read()
    assert data[:4] == b"PXFD"
    _, steps, _ = struct.unpack_from("<III", data, 4)
    off = 20
    for _ in range(steps):
        n = struct.unpack_from("<I", data, off)[0]
        off += 12
        yield n, data[off:off + n * REC]
        off += n * REC


def summary(a):
    counts = collections.Counter()
    for tag, fluid, p in records(a.trace):
        counts[TAGS.get(tag, tag)] += 1
        if tag == 1:
            d = struct.unpack_from("<88I", p)
            f = struct.unpack_from("<88f", p)
            print(f"fluid {fluid}: max {d[0x10]} reserve {d[0x11]} restPPM {f[0x12]:g} method {d[0x2d]} "
                  f"flags {d[0x4f]:#x} compartment {d[0x52]:#x}")
    for k, v in counts.items():
        print(f"{k:12s} {v}")


def dump(a):
    step = 0
    for tag, fluid, p in records(a.trace):
        if fluid not in (a.fluid, 0xFFFFFFFF):
            continue
        if tag == 6:
            print(f"-- simulate {step} dt={struct.unpack_from('<f', p)[0]:g}")
            step += 1
        elif tag == 3:
            req, app, ret, streams = struct.unpack_from("<4I", p)
            lifes = sorted({round(struct.unpack_from("<f", p, 16 + 32 * i + 24)[0], 6) for i in range(req)})
            print(f"   add n={req} appendIds={app} -> {ret} streams={streams:#x} life={lifes}")
        elif tag == 4:
            mode, n, streams = struct.unpack_from("<3I", p)
            ups = [struct.unpack_from("<2I", p, 12 + 20 * i) for i in range(n)]
            print(f"   update mode={mode} n={n} streams={streams:#x} (id,flag)={ups[:12]}")
        elif tag == 5:
            print("   removeAll")
        elif tag == 8:
            n = struct.unpack_from("<I", p)[0]
            recs = [struct.unpack_from("<11f2I", p, 4 + REC * i) for i in range(n)]
            ids = [r[11] for r in recs]
            life = [round(r[7], 4) for r in recs]
            flags = sorted({r[12] for r in recs})
            print(f"   particles n={n} ids={ids[:24]}{'...' if n > 24 else ''} life={life[:12]} flags={[hex(f) for f in flags]}")
        elif tag in (9, 10, 12):
            n = struct.unpack_from("<I", p)[0]
            name = {9: "created", 10: "deleted", 12: "created (right after add)"}[tag]
            print(f"   {name} {list(struct.unpack_from(f'<{n}I', p, 4))}")
        elif tag == 14:
            print(f"   write buffer count right after add: {struct.unpack_from('<I', p)[0]}")
        elif tag == 13:
            b = [round(x, 4) for x in struct.unpack_from("<6f", p)]
            print(f"   worldBounds {b[:3]} - {b[3:]}")
        elif tag == 11:
            n = struct.unpack_from("<I", p)[0]
            pk = [struct.unpack_from("<6f3I", p, 4 + 36 * i) for i in range(n)]
            print(f"   packets n={n} " + " ".join(f"[id {q[8]} first {q[6]} n {q[7]}]" for q in pk[:8]))


def check(a):
    frames = [p for tag, fluid, p in records(a.trace) if tag == 8 and fluid == a.fluid]
    ref = list(pxfd_frames(a.oracle))
    bad = 0
    for i, (p, (n, blob)) in enumerate(zip(frames, ref)):
        m = struct.unpack_from("<I", p)[0]
        if m != n or p[4:] != blob:
            bad += 1
            if bad <= 5:
                print(f"step {i}: trace n={m} oracle n={n} differ")
    print(f"{len(frames)} trace frames, {len(ref)} oracle frames, {bad} differ")
    return 0 if bad == 0 and len(frames) == len(ref) else 1


def collect(path, fluid):
    """Per step s (the s-th SIMULATE, 0-based): particles {id: (pos, life)}, deleted set, bounds; adds by step."""
    steps = collections.defaultdict(dict)
    adds = collections.defaultdict(list)  # key s: adds made after s simulates (before step s)
    sim = 0
    for tag, fl, p in records(path):
        if fl not in (fluid, 0xFFFFFFFF):
            continue
        if tag == 6:
            sim += 1
        elif tag == 12:
            n = struct.unpack_from("<I", p)[0]
            adds[sim].append(tuple(struct.unpack_from(f"<{n}I", p, 4)))
        elif tag == 8 and sim >= 1:
            n = struct.unpack_from("<I", p)[0]
            steps[sim - 1]["P"] = {r[11]: (r[0:3], r[7]) for r in
                                   (struct.unpack_from("<11f2I", p, 4 + REC * i) for i in range(n))}
        elif tag == 10 and sim >= 1:
            n = struct.unpack_from("<I", p)[0]
            steps[sim - 1]["D"] = set(struct.unpack_from(f"<{n}I", p, 4))
        elif tag == 13 and sim >= 1:
            steps[sim - 1]["B"] = struct.unpack_from("<6f", p)
    return steps, adds, sim


def compare(a):
    sa, aa, na = collect(a.trace, a.fluid)
    sb, ab, nb = collect(a.other, a.fluid)
    nsteps = max(na, nb)
    print(f"A: {a.trace} ({na} steps)\nB: {a.other} ({nb} steps)")
    print(f"{'step':>4} {'nA':>5} {'nB':>5} {'count':>5} {'ids':>4} {'created':>7} {'deleted':>7} "
          f"{'life_max':>10} {'pos_max':>10} {'bounds_max':>10}")
    bad = collections.Counter()
    tot = dict(life=0.0, pos=0.0, bnd=0.0)
    for s in range(nsteps):
        PA = sa[s].get("P", {}); PB = sb[s].get("P", {})
        ca = aa.get(s, []); cb = ab.get(s, [])
        count_eq = len(PA) == len(PB)
        ids_eq = set(PA) == set(PB)
        created_eq = ca == cb
        deleted_eq = sa[s].get("D", set()) == sb[s].get("D", set())
        common = set(PA) & set(PB)
        life = max((abs(PA[i][1] - PB[i][1]) for i in common), default=0.0)
        pos = max((abs(x - y) for i in common for x, y in zip(PA[i][0], PB[i][0])), default=0.0)
        if "B" in sa[s] and "B" in sb[s]:
            bnd = max(abs(x - y) for x, y in zip(sa[s]["B"], sb[s]["B"]))
        else:
            bnd = float("nan")
        for key, ok in (("count", count_eq), ("ids", ids_eq), ("created", created_eq), ("deleted", deleted_eq)):
            if not ok:
                bad[key] += 1
        tot["life"] = max(tot["life"], life); tot["pos"] = max(tot["pos"], pos)
        if bnd == bnd:
            tot["bnd"] = max(tot["bnd"], bnd)
        yn = lambda ok: "ok" if ok else "DIFF"
        print(f"{s:>4} {len(PA):>5} {len(PB):>5} {yn(count_eq):>5} {yn(ids_eq):>4} {yn(created_eq):>7} "
              f"{yn(deleted_eq):>7} {life:>10.3g} {pos:>10.3g} {bnd:>10.3g}")
    print(f"adds: A {sum(len(v) for v in aa.values())}, B {sum(len(v) for v in ab.values())}")
    for name, st, ad in (("A", sa, aa), ("B", sb, ab)):
        missing = [k for k, ok in (("CREATED_AFTER_ADD", bool(ad)),
                                   ("WORLD_BOUNDS", any("B" in v for v in st.values()))) if not ok]
        if missing:
            print(f"note: {name} has no {', '.join(missing)} records (not compared)")
    print("steps with a difference: " + (", ".join(f"{k} {v}" for k, v in bad.items()) or "none"))
    bnd_txt = f"{tot['bnd']:.3g}" if any("B" in v for v in sa.values()) and any("B" in v for v in sb.values()) else "n/a"
    print(f"max |life| diff {tot['life']:.3g}, max |pos| diff {tot['pos']:.3g}, max |bounds| diff {bnd_txt}")
    return 0 if not bad else 1


def main():
    ap = argparse.ArgumentParser()
    sub = ap.add_subparsers(dest="cmd", required=True)
    s = sub.add_parser("summary"); s.add_argument("trace")
    d = sub.add_parser("dump"); d.add_argument("trace"); d.add_argument("--fluid", type=int, default=0)
    c = sub.add_parser("check"); c.add_argument("trace"); c.add_argument("oracle"); c.add_argument("--fluid", type=int, default=0)
    cp = sub.add_parser("compare"); cp.add_argument("trace"); cp.add_argument("other"); cp.add_argument("--fluid", type=int, default=0)
    a = ap.parse_args()
    sys.exit({"summary": summary, "dump": dump, "check": check, "compare": compare}[a.cmd](a))


if __name__ == "__main__":
    main()
