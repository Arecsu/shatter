#!/usr/bin/env python3
"""Minimal reader for Mirror's Edge UE3 packages (file version 536, licensee 43).

Reads the name/import/export tables (decompressing LZO/zlib chunked packages)
and decodes tagged properties of exports. Read-only.

  ue3pkg.py summary PKG                      # counts + class histogram
  ue3pkg.py exports PKG [--class REGEX]      # list exports
  ue3pkg.py props PKG --class REGEX [--json] # decode tagged properties
"""
import argparse
import json
import re
import struct
import sys
import zlib

TAG = 0x9E2A83C1
COMPRESS_ZLIB, COMPRESS_LZO, COMPRESS_LZX = 1, 2, 4


class Reader:
    def __init__(self, data, pos=0):
        self.d, self.p = data, pos

    def u8(self):
        v = self.d[self.p]; self.p += 1; return v

    def u16(self):
        v = struct.unpack_from("<H", self.d, self.p)[0]; self.p += 2; return v

    def i32(self):
        v = struct.unpack_from("<i", self.d, self.p)[0]; self.p += 4; return v

    def u32(self):
        v = struct.unpack_from("<I", self.d, self.p)[0]; self.p += 4; return v

    def u64(self):
        v = struct.unpack_from("<Q", self.d, self.p)[0]; self.p += 8; return v

    def f32(self):
        v = struct.unpack_from("<f", self.d, self.p)[0]; self.p += 4; return v

    def raw(self, n):
        v = self.d[self.p:self.p + n]; self.p += n; return v

    def fstring(self):
        n = self.i32()
        if n == 0:
            return ""
        if n > 0:
            return self.raw(n)[:-1].decode("latin-1")
        return self.raw(-n * 2)[:-2].decode("utf-16-le")


def decompress_chunks(data, flags, chunks):
    """Rebuild the uncompressed file image: header bytes + decompressed chunks."""
    end = max(c[0] + c[1] for c in chunks)
    out = bytearray(end)
    first_uoff = min(c[0] for c in chunks)
    out[:first_uoff] = data[:first_uoff]
    for uoff, usize, coff, csize in chunks:
        r = Reader(data, coff)
        tag = r.u32()
        assert tag == TAG, "bad chunk tag"
        block_size = r.u32()
        r.u32()  # total compressed
        total_u = r.u32()
        blocks = []
        while sum(b[1] for b in blocks) < total_u:
            blocks.append((r.u32(), r.u32()))
        pos = uoff
        for cs, us in blocks:
            src = r.raw(cs)
            if flags & COMPRESS_LZO:
                import lzo
                buf = lzo.decompress(src, False, us)
            elif flags & COMPRESS_ZLIB:
                buf = zlib.decompress(src)
            else:
                raise ValueError("unsupported compression %d" % flags)
            out[pos:pos + len(buf)] = buf
            pos += len(buf)
    return bytes(out)


class Package:
    def __init__(self, path):
        self.path = path
        data = open(path, "rb").read()
        r = Reader(data)
        assert r.u32() == TAG, "not a UE3 package"
        self.version, self.licensee = r.u16(), r.u16()
        self.header_size = r.i32()
        self.folder = r.fstring()
        self.flags = r.u32()
        self.name_count, self.name_offset = r.i32(), r.i32()
        self.export_count, self.export_offset = r.i32(), r.i32()
        self.import_count, self.import_offset = r.i32(), r.i32()
        self.depends_offset = r.i32()
        self.guid = r.raw(16)
        gens = r.i32()
        for _ in range(gens):
            r.raw(12)
        self.engine_version, self.cooker_version = r.i32(), r.i32()
        self.compression = r.u32()
        nchunks = r.i32()
        chunks = [(r.i32(), r.i32(), r.i32(), r.i32()) for _ in range(nchunks)]
        self.data = decompress_chunks(data, self.compression, chunks) if nchunks else data
        self._read_tables()

    def _read_tables(self):
        r = Reader(self.data, self.name_offset)
        self.names = []
        for _ in range(self.name_count):
            self.names.append(r.fstring())
            r.u64()  # flags
        r.p = self.import_offset
        self.imports = []
        for _ in range(self.import_count):
            cpkg, cname, outer, oname = self.nameref(r), self.nameref(r), r.i32(), self.nameref(r)
            self.imports.append({"class_package": cpkg, "class": cname, "outer": outer, "name": oname})
        r.p = self.export_offset
        self.exports = []
        for _ in range(self.export_count):
            e = {"class_index": r.i32(), "super": r.i32(), "outer": r.i32(), "name": self.nameref(r),
                 "archetype": r.i32(), "object_flags": r.u64(), "serial_size": r.i32()}
            e["serial_offset"] = r.i32() if e["serial_size"] > 0 else 0
            # v < 543: ComponentMap TMap<FName, int> precedes ExportFlags
            e["component_map"] = {self.nameref(r): r.i32() for _ in range(r.i32())}
            e["export_flags"] = r.u32()
            nnet = r.i32()
            r.raw(4 * nnet)
            r.raw(16)  # package guid
            e["package_flags"] = r.u32()
            e["index"] = len(self.exports) + 1
            self.exports.append(e)

    def nameref(self, r):
        idx, num = r.i32(), r.i32()
        n = self.names[idx] if 0 <= idx < len(self.names) else "?%d" % idx
        return n if num == 0 else "%s_%d" % (n, num - 1)

    def obj_name(self, index):
        if index > 0:
            return self.exports[index - 1]["name"]
        if index < 0:
            return self.imports[-index - 1]["name"]
        return "None"

    def class_name(self, e):
        return "Class" if e["class_index"] == 0 else self.obj_name(e["class_index"])

    def full_name(self, index):
        parts = []
        while index:
            parts.append(self.obj_name(index))
            index = (self.exports[index - 1]["outer"] if index > 0 else self.imports[-index - 1]["outer"])
        return ".".join(reversed(parts))

    # ---- tagged properties -------------------------------------------------
    def read_props(self, r, end, depth=0):
        props = []
        while r.p < end:
            name = self.nameref(r)
            if name == "None":
                break
            ptype = self.nameref(r)
            size, aidx = r.i32(), r.i32()
            p = {"name": name, "type": ptype, "index": aidx}
            if ptype == "StructProperty":
                p["struct"] = self.nameref(r)
            start = r.p
            if ptype == "BoolProperty":
                p["value"] = bool(r.u32())  # v536: 4-byte bool after the tag
                start = r.p
                size = 0
            elif ptype == "IntProperty":
                p["value"] = r.i32()
            elif ptype == "FloatProperty":
                p["value"] = r.f32()
            elif ptype in ("NameProperty",):
                p["value"] = self.nameref(r)
            elif ptype == "ByteProperty":
                p["value"] = r.u8() if size == 1 else self.nameref(r)
            elif ptype in ("ObjectProperty", "ClassProperty", "ComponentProperty", "InterfaceProperty"):
                p["value"] = self.full_name(r.i32())
            elif ptype in ("StrProperty",):
                p["value"] = r.fstring()
            elif ptype == "StructProperty":
                p["value"] = self.read_struct(r, p["struct"], size, depth)
            else:
                p["raw"] = r.raw(size).hex()
            r.p = start + size
            props.append(p)
        return props

    def read_struct(self, r, sname, size, depth):
        fixed = {"Vector": "fff", "Rotator": "iii", "Color": "BBBB", "LinearColor": "ffff",
                 "Vector2D": "ff", "Guid": "IIII", "Box": None}
        if sname in fixed and fixed[sname]:
            fmt = "<" + fixed[sname]
            return list(struct.unpack_from(fmt, r.d, r.p))
        # tagged sub-struct (most script structs) - try to decode, fall back to raw
        save = r.p
        try:
            sub = self.read_props(r, save + size, depth + 1)
            if r.p <= save + size:
                return sub
        except Exception:
            pass
        r.p = save
        return {"raw": r.d[save:save + size].hex()}

    KNOWN_TYPES = {"None", "BoolProperty", "IntProperty", "FloatProperty", "NameProperty", "ByteProperty",
                   "ObjectProperty", "ClassProperty", "ComponentProperty", "InterfaceProperty", "StrProperty",
                   "StructProperty", "ArrayProperty", "MapProperty", "DelegateProperty"}

    def is_component(self, e):
        # UComponent subclasses: *Component classes and the Distribution* objects (no class tree here).
        c = self.class_name(e)
        return c.endswith("Component") or c.startswith("Distribution")

    def is_cdo_template(self, index):
        # UComponent::IsTemplate(RF_ClassDefaultObject): the object or one of its outers is a class default object.
        while index > 0:
            e = self.exports[index - 1]
            if e["name"].startswith("Default__"):
                return True
            index = e["outer"]
        return False

    def _tags_plausible(self, pos, end):
        if pos + 8 > end:
            return False
        r = Reader(self.data, pos)
        idx, num = r.i32(), r.i32()
        if not (0 <= idx < len(self.names)) or num < 0:
            return False
        if self.names[idx] == "None":
            return True
        if pos + 16 > end:
            return False
        tidx = r.i32()
        return 0 <= tidx < len(self.names) and self.names[tidx] in self.KNOWN_TYPES

    def export_props(self, e):
        if e["serial_size"] <= 0:
            return []
        r = Reader(self.data, e["serial_offset"])
        end = e["serial_offset"] + e["serial_size"]
        if self.is_component(e) and not e["object_flags"] & 0x200:  # RF_ClassDefaultObject: no prefix
            # UComponent::Serialize writes TemplateOwnerClass (object ref), and TemplateName (FName) when the
            # component sits under a class default object, before UObject's NetIndex and the tags.
            r.i32()  # TemplateOwnerClass
            if self.is_cdo_template(e["index"]):
                r.raw(8)  # TemplateName
        r.i32()  # NetIndex
        if e["object_flags"] & 0x0200000000000000:  # RF_HasStack
            r.raw(22)
        if not self._tags_plausible(r.p, end):
            raise ValueError("no tagged properties at +%d in %s" % (r.p - e["serial_offset"], e["name"]))
        return self.read_props(r, end)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("cmd", choices=["summary", "exports", "props"])
    ap.add_argument("pkg")
    ap.add_argument("--class", dest="cls", default=".")
    ap.add_argument("--json", action="store_true")
    a = ap.parse_args()
    pkg = Package(a.pkg)
    flt = re.compile(a.cls)
    if a.cmd == "summary":
        from collections import Counter
        c = Counter(pkg.class_name(e) for e in pkg.exports)
        print("version %d/%d engine %d cooker %d compression %d names %d imports %d exports %d" % (
            pkg.version, pkg.licensee, pkg.engine_version, pkg.cooker_version, pkg.compression,
            len(pkg.names), len(pkg.imports), len(pkg.exports)))
        for k, v in c.most_common():
            if flt.search(k):
                print("%6d %s" % (v, k))
    elif a.cmd == "exports":
        for i, e in enumerate(pkg.exports, 1):
            if flt.search(pkg.class_name(e)):
                print("%-30s %-60s size=%d" % (pkg.class_name(e), pkg.full_name(i), e["serial_size"]))
    else:
        out = []
        for i, e in enumerate(pkg.exports, 1):
            if flt.search(pkg.class_name(e)):
                out.append({"class": pkg.class_name(e), "object": pkg.full_name(i), "props": pkg.export_props(e)})
        if a.json:
            json.dump(out, sys.stdout, indent=1)
        else:
            for o in out:
                print("== %s %s" % (o["class"], o["object"]))
                for p in o["props"]:
                    print("   %s[%d] %s = %s" % (p["name"], p["index"], p["type"], p.get("value", p.get("raw"))))


if __name__ == "__main__":
    main()
