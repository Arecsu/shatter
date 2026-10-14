#!/usr/bin/env python3
"""Read movelog.bin from the PXSHIM_MOVELOG=1 logger (shim/game_hooks.cpp).

  tools/movelog.py <movelog.bin>                     summary: pawns, frame rate, record counts
  tools/movelog.py <movelog.bin> --player-csv out    player per top-level physics call: t, dt, phys, speed, vel, loc,
                                                     the last acceleration request (inputs and result), move pointer
  tools/movelog.py <movelog.bin> --aim-csv out        per aim-bot update: t, dt, owner, dispersion x/y/z
"""
import argparse
import collections
import csv
import math
import struct

REC = struct.Struct("<HHId40f")
PHYS, WALK, SPRINT, DISP, FLAG = 1, 2, 3, 4, 5


def u32(f):
    return struct.unpack("<I", struct.pack("<f", f))[0]


def records(path):
    data = open(path, "rb").read()
    for off in range(0, len(data) - REC.size + 1, REC.size):
        typ, it, self, t, *a = REC.unpack_from(data, off)
        yield typ, it, self, t, a


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("log")
    ap.add_argument("--player-csv")
    ap.add_argument("--aim-csv")
    ap.add_argument("--flags", action="store_true", help="slide-steering flag changes (PXSHIM_MOVEWATCH=1)")
    args = ap.parse_args()
    recs = list(records(args.log))
    counts = collections.Counter((r[0], r[2]) for r in recs)
    accel_pawns = {r[2] for r in recs if r[0] in (WALK, SPRINT)}
    print(f"{len(recs)} records, {recs[-1][3] - recs[0][3]:.1f} s" if recs else "empty")
    for self in sorted({r[2] for r in recs if r[0] == PHYS}):
        top = [r for r in recs if r[0] == PHYS and r[2] == self and r[1] == 0]
        dts = sorted(r[4][0] for r in top)
        med = dts[len(dts) // 2] if dts else 0
        tag = " (player: accel requests)" if self in accel_pawns else ""
        print(f"pawn {self:08X} vt#{u32(top[0][4][17]) if top else '?'}: {len(top)} physics calls, "
              f"median dt {med * 1000:.2f} ms ({1 / med if med else 0:.0f} Hz), "
              f"substeps {counts[(PHYS, self)] - len(top)}{tag}")
    for self in sorted(accel_pawns):  # hits on the player: health drops
        hp = [(r[3], int(r[4][18])) for r in recs if r[0] == PHYS and r[2] == self and r[1] == 0]
        drops = [(t, b - a) for (_, b), (t, a) in zip(hp, hp[1:]) if a < b]
        print(f"player {self:08X}: {len(drops)} health drops, total {sum(d for _, d in drops)}")
    bots = collections.Counter(r[2] for r in recs if r[0] == DISP)
    for b, n in bots.items():
        print(f"aim bot {b:08X}: {n} dispersion updates")

    flags = [r for r in recs if r[0] == FLAG]
    if flags:
        on = sum(1 for r in flags if r[4][0] == 1)
        print(f"slide-steering flag: {len(flags)} changes ({on} on)")
    if args.flags:
        for typ, it, self, t, a in flags:
            stack = " ".join(f"{u32(x):08X}" for x in a[2:12] if u32(x))
            print(f"  {t:9.4f} {'ON ' if a[0] else 'off'} {'(in physics)' if it else '            '} eip {u32(a[1]):08X}  {stack}")
    if args.player_csv:
        player = max(accel_pawns, key=lambda p: counts[(PHYS, p)], default=None)
        last_accel = [0.0] * 20
        with open(args.player_csv, "w", newline="") as f:
            w = csv.writer(f)
            w.writerow(["t", "dt", "iter", "phys", "speed2d", "vx", "vy", "vz", "x", "y", "z", "move",
                        "fwd", "strafe", "drot", "accel_dt", "ax", "ay", "az", "f6b4", "f6c4", "f6d0", "f6f4",
                        "health", "since_damage", "accx", "accy", "accz", "accel_rate", "air_control", "air_speed",
                        "walkable_z", "slide_flag", "move_state", "move_vtable", "slide_correction",
                        "slide_side", "move_strength", "move_cap", "flag_source", "touch_nx", "touch_ny", "touch_nz"])
            for typ, it, self, t, a in recs:
                if self != player:
                    continue
                if typ in (WALK, SPRINT):
                    last_accel = a
                elif typ == PHYS:
                    vx, vy, vz = a[8:11]
                    w.writerow([f"{t:.6f}", a[0], it, int(a[14]), math.hypot(vx, vy), vx, vy, vz, *a[11:14],
                                f"{u32(a[15]):08X}", *last_accel[0:4], *last_accel[4:7], *last_accel[15:19],
                                int(a[18]), a[19], *a[20:27], int(a[27]), int(a[28]), f"{u32(a[29]):08X}", a[30],
                                int(a[31]), a[32], a[33], int(a[34]), *a[35:38]])
        print(f"player {player:08X} -> {args.player_csv}" if player else "no player found")
    if args.aim_csv:
        with open(args.aim_csv, "w", newline="") as f:
            w = csv.writer(f)
            w.writerow(["t", "dt", "bot", "owner", "disp_x", "disp_y", "disp_z"])
            for typ, it, self, t, a in recs:
                if typ == DISP:
                    blk = a[1:]  # the bot's bytes from +0
                    w.writerow([f"{t:.6f}", a[0], f"{self:08X}", f"{u32(blk[0x28 // 4]):08X}",
                                blk[0x7C // 4], blk[0x80 // 4], blk[0x84 // 4]])
        print(f"aim -> {args.aim_csv}")


if __name__ == "__main__":
    main()
