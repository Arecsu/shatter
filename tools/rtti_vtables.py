#!/usr/bin/env python3
"""Dump MSVC RTTI vtables (class name -> slot count) from a 32-bit PE.

Used to check ABI compatibility between PhysXCore builds: if the public
interface implementations (NpScene, NpPhysicsSDK, NpFluid, NpCloth...) have the
same vtable sizes across versions, headers of one version are a reasonable
match for the other. Slot *count* equality is necessary, not sufficient.

Usage: rtti_vtables.py [--filter REGEX] [--json out.json] FILE
"""
import argparse
import json
import re
import struct

import pefile


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("file")
    ap.add_argument("--filter", default=".")
    ap.add_argument("--json")
    a = ap.parse_args()

    pe = pefile.PE(a.file, fast_load=True)
    base = pe.OPTIONAL_HEADER.ImageBase
    img = pe.get_memory_mapped_image()
    text = [(s.VirtualAddress, s.VirtualAddress + s.Misc_VirtualSize)
            for s in pe.sections if s.Characteristics & 0x20000000]

    def is_code(va):
        rva = va - base
        return any(lo <= rva < hi for lo, hi in text)

    def u32(rva):
        return struct.unpack_from("<I", img, rva)[0]

    # TypeDescriptor: pVFTable, spare, name ".?AV...@@"
    tds = {}
    for m in re.finditer(rb"\.\?A[VU][^\x00]{1,200}?@@\x00", img):
        tds[m.start() - 8 + base] = m.group()[:-1].decode(errors="replace")

    # CompleteObjectLocator (x86): signature=0, offset, cdOffset, pTypeDescriptor, pClassHierarchy
    cols = {}
    for rva in range(0, len(img) - 20, 4):
        sig, off, cd, ptd = struct.unpack_from("<IIII", img, rva)
        if sig == 0 and ptd in tds:
            cols[rva + base] = (tds[ptd], off)

    # vtable: [ptr to COL][slot0][slot1]...
    out = {}
    colset = set(cols)
    for rva in range(0, len(img) - 8, 4):
        p = u32(rva)
        if p in colset:
            name, off = cols[p]
            n = 0
            r = rva + 4
            while r + 4 <= len(img) and is_code(u32(r)):
                n += 1
                r += 4
            if n:
                key = "%s+%d" % (name, off)
                out.setdefault(key, []).append(n)

    flt = re.compile(a.filter)
    for k in sorted(out):
        if flt.search(k):
            print("%-60s %s" % (k, out[k]))
    if a.json:
        json.dump(out, open(a.json, "w"), indent=1, sort_keys=True)


if __name__ == "__main__":
    main()
