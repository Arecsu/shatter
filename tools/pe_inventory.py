#!/usr/bin/env python3
"""Inventory PE files: machine, version resources, sha256, imports/exports of interest.

Usage: pe_inventory.py [--json out.json] FILE...
Read-only; never modifies inputs.
"""
import argparse
import hashlib
import json
import sys

import pefile

MACHINES = {0x14C: "i386", 0x8664: "x86_64"}


def version_info(pe):
    out = {}
    if getattr(pe, "VS_FIXEDFILEINFO", None):
        f = pe.VS_FIXEDFILEINFO[0]
        out["FileVersionFixed"] = "%d.%d.%d.%d" % (
            f.FileVersionMS >> 16, f.FileVersionMS & 0xFFFF,
            f.FileVersionLS >> 16, f.FileVersionLS & 0xFFFF)
    for fi in getattr(pe, "FileInfo", []) or []:
        for entry in fi:
            for st in getattr(entry, "StringTable", []) or []:
                for k, v in st.entries.items():
                    out[k.decode(errors="replace")] = v.decode(errors="replace")
    return out


def inspect(path):
    with open(path, "rb") as fh:
        data = fh.read()
    pe = pefile.PE(data=data, fast_load=True)
    pe.parse_data_directories(directories=[
        pefile.DIRECTORY_ENTRY["IMAGE_DIRECTORY_ENTRY_RESOURCE"],
        pefile.DIRECTORY_ENTRY["IMAGE_DIRECTORY_ENTRY_IMPORT"],
        pefile.DIRECTORY_ENTRY["IMAGE_DIRECTORY_ENTRY_EXPORT"],
    ])
    rec = {
        "path": path,
        "size": len(data),
        "sha256": hashlib.sha256(data).hexdigest(),
        "machine": MACHINES.get(pe.FILE_HEADER.Machine, hex(pe.FILE_HEADER.Machine)),
        "timestamp": pe.FILE_HEADER.TimeDateStamp,
        "version": version_info(pe),
        "imports": sorted({i.dll.decode() for i in getattr(pe, "DIRECTORY_ENTRY_IMPORT", [])}),
        "exports": [e.name.decode() if e.name else "#%d" % e.ordinal
                    for e in (pe.DIRECTORY_ENTRY_EXPORT.symbols
                              if hasattr(pe, "DIRECTORY_ENTRY_EXPORT") else [])],
    }
    return rec


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--json")
    ap.add_argument("files", nargs="+")
    a = ap.parse_args()
    recs = []
    for f in a.files:
        try:
            recs.append(inspect(f))
        except Exception as e:  # keep going on odd files
            recs.append({"path": f, "error": str(e)})
    for r in recs:
        if "error" in r:
            print("%s: ERROR %s" % (r["path"], r["error"]))
            continue
        v = r["version"]
        print("%-90s %-6s %-14s %s  %s" % (
            r["path"][-90:], r["machine"], v.get("FileVersion", v.get("FileVersionFixed", "?")),
            r["sha256"][:16], v.get("FileDescription", "")))
    if a.json:
        with open(a.json, "w") as fh:
            json.dump(recs, fh, indent=1)


if __name__ == "__main__":
    sys.exit(main())
