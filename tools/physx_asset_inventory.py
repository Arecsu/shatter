#!/usr/bin/env python3
"""Scan every cooked package of the game and inventory PhysX content.

Finds, per package:
  - PhysXParticleSystem (NxFluid parameters), merged over the class defaults
  - ParticleSystems whose emitters use ParticleModuleTypeData{,Mesh}PhysX
  - SkeletalMesh assets with cloth parameters; (Td)SkeletalMeshComponents with cloth enabled after resolving
    the archetype chain (own tags, then archetypes in this or other packages, then the class default object);
    "placed" = under PersistentLevel, otherwise a template copy (e.g. a cooked prefab archetype)
  - FracturedStaticMesh / FracturedStaticMeshActor counts (glass & breakables)

Output: JSON (default captures/assets/physx_inventory.json) + a short text summary.
Game data stays under captures/ (gitignored).
"""
import argparse
import functools
import glob
import json
import os
import sys
import multiprocessing
from concurrent.futures import ProcessPoolExecutor

sys.path.insert(0, os.path.dirname(__file__))
from ue3pkg import Package  # noqa: E402

DEFAULT_ROOT = os.path.join(os.environ.get("GAME_PREFIX", ""), "drive_c/Mirror's Edge/TdGame/CookedPC")
TYPEDATA = ("ParticleModuleTypeDataPhysX", "ParticleModuleTypeDataMeshPhysX")


def flatten(props):
    """props list -> {name or name[i]: value}; arrays reduced to element counts."""
    out = {}
    for p in props:
        key = p["name"] if p["index"] == 0 else "%s[%d]" % (p["name"], p["index"])
        if p["type"] == "ArrayProperty":
            raw = bytes.fromhex(p.get("raw", ""))
            out[key] = {"array_len": int.from_bytes(raw[:4], "little", signed=True) if len(raw) >= 4 else 0}
        elif isinstance(p.get("value"), list) and p["value"] and isinstance(p["value"][0], dict):
            out[key] = flatten(p["value"])
        else:
            out[key] = p.get("value", p.get("raw"))
    return out


PKG_FILES = {}  # lower-case package name -> path, filled in main() before the workers fork


@functools.lru_cache(None)
def load_package(name):
    path = PKG_FILES.get(name.lower())
    return Package(path) if path else None


def find_export(pkg, path):
    want = ".".join(path).lower()
    for e in pkg.exports:
        if pkg.full_name(e["index"]).lower() == want:
            return e
    return None


def effective_props(pkg, e, cls):
    """Own tags merged over the archetype chain and the class default; (props, list of chain links)."""
    eff, links = {}, []
    for _ in range(32):
        links.append("%s:%s" % (os.path.basename(pkg.path), pkg.full_name(e["index"])))
        for k, v in flatten(pkg.export_props(e)).items():
            eff.setdefault(k, v)
        a = e["archetype"]
        if a > 0:
            e = pkg.exports[a - 1]
            continue
        if a < 0:
            parts, i = [], a
            while i:
                o = pkg.imports[-i - 1] if i < 0 else pkg.exports[i - 1]
                parts.append(o["name"])
                i = o["outer"]
            parts.reverse()
            p2 = load_package(parts[0])
            e2 = find_export(p2, parts[1:]) if p2 else None
            if e2 is None:
                links.append("UNRESOLVED " + ".".join(parts))
                break
            pkg, e = p2, e2
            continue
        for script in ("Engine", "TdGame"):
            p2 = load_package(script)
            e2 = find_export(p2, ["Default__" + cls]) if p2 else None
            if e2 is not None:
                links.append("%s.u:Default__%s" % (script, cls))
                for k, v in flatten(p2.export_props(e2)).items():
                    eff.setdefault(k, v)
                break
        break
    return eff, links


def scan(path):
    rel = path
    try:
        pkg = Package(path)
    except Exception as e:  # report and continue
        return {"package": rel, "error": repr(e)}
    res = {"package": rel, "fluids": [], "physx_emitters": [], "cloth_meshes": [], "cloth_components": [],
           "fractured_meshes": 0, "fractured_actors": 0}
    for i, e in enumerate(pkg.exports, 1):
        cls = pkg.class_name(e)
        name = pkg.full_name(i)
        if name.split(".")[-1].startswith("Default__"):
            continue
        try:
            if cls == "PhysXParticleSystem":
                res["fluids"].append({"object": name, "props": flatten(pkg.export_props(e))})
            elif cls in TYPEDATA:
                props = flatten(pkg.export_props(e))
                res["physx_emitters"].append({"object": name, "type": cls, "props": props})
            elif cls == "SkeletalMesh":
                props = flatten(pkg.export_props(e))
                cloth = {k: v for k, v in props.items() if "Cloth" in k or k.startswith("bEnableCloth")}
                bones = props.get("ClothBones", {}).get("array_len", 0) if isinstance(props.get("ClothBones"), dict) else 0
                if bones or any(k.startswith("bEnableCloth") for k in cloth) or cloth.get("bClothMetal"):
                    res["cloth_meshes"].append({"object": name, "cloth_props": cloth})
            elif cls.endswith("SkeletalMeshComponent"):
                props, links = effective_props(pkg, e, cls)
                if props.get("bEnableClothSimulation"):
                    res["cloth_components"].append({
                        "object": name, "class": cls, "placed": ".PersistentLevel." in name, "archetypes": links[1:],
                        "props": {k: v for k, v in props.items()
                                  if "Cloth" in k or "ValidBounds" in k or k in ("SkeletalMesh", "bEnableClothSimulation")}})
            elif cls == "FracturedStaticMesh":
                res["fractured_meshes"] += 1
            elif cls == "FracturedStaticMeshActor":
                res["fractured_actors"] += 1
        except Exception as ex:
            res.setdefault("prop_errors", []).append("%s: %r" % (name, ex))
    return res


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--root", default=DEFAULT_ROOT)
    ap.add_argument("--out", default="captures/assets/physx_inventory.json")
    ap.add_argument("-j", type=int, default=os.cpu_count())
    a = ap.parse_args()
    files = sorted(f for ext in ("upk", "u", "me1") for f in glob.glob(os.path.join(a.root, "**", "*." + ext), recursive=True))
    for f in files:
        PKG_FILES.setdefault(os.path.splitext(os.path.basename(f))[0].lower(), f)
    defaults = {}
    eng = Package(os.path.join(a.root, "Engine.u"))
    for i, e in enumerate(eng.exports, 1):
        n = e["name"]
        if n in ("Default__PhysXParticleSystem", "Default__SkeletalMesh", "Default__SkeletalMeshComponent") + tuple(
                "Default__" + t for t in TYPEDATA):
            props = flatten(eng.export_props(e))
            if n == "Default__SkeletalMesh" or n == "Default__SkeletalMeshComponent":
                props = {k: v for k, v in props.items() if "Cloth" in k}
            defaults[n[len("Default__"):]] = props
    with ProcessPoolExecutor(a.j, mp_context=multiprocessing.get_context("fork")) as ex:
        results = list(ex.map(scan, files, chunksize=4))
    for r in results:
        r["package"] = os.path.relpath(r["package"], a.root)
    hits = [r for r in results if r.get("error") or r["fluids"] or r["physx_emitters"] or r["cloth_meshes"]
            or r["cloth_components"] or r["fractured_actors"] or r["fractured_meshes"]]
    os.makedirs(os.path.dirname(a.out), exist_ok=True)
    json.dump({"root": a.root, "packages_scanned": len(files), "class_defaults": defaults, "packages": hits},
              open(a.out, "w"), indent=1)

    comps = [dict(c, package=r["package"]) for r in hits if not r.get("error") for c in r["cloth_components"]]
    for placed in (True, False):
        cs = [c for c in comps if c["placed"] == placed]
        print("%s cloth components: %d (packages %d); bDisableClothCollision %d, bEnableValidBounds %d, unresolved chains %d" % (
            "placed" if placed else "template", len(cs), len({c["package"] for c in cs}),
            sum(bool(c["props"].get("bDisableClothCollision")) for c in cs),
            sum(bool(c["props"].get("bEnableValidBounds")) for c in cs),
            sum(any(l.startswith("UNRESOLVED") for l in c["archetypes"]) for c in cs)))
    errs = [r for r in results if r.get("error")]
    print("scanned %d packages, %d with PhysX content, %d parse errors" % (len(files), len(hits) - len(errs), len(errs)))
    for r in errs[:10]:
        print("  ERROR", r["package"], r["error"])
    print("%-45s %6s %8s %6s %6s %6s" % ("package", "fluids", "emitters", "clothM", "clothC", "fract"))
    for r in hits:
        if r.get("error"):
            continue
        print("%-45s %6d %8d %6d %6d %6d" % (r["package"][:45], len(r["fluids"]), len(r["physx_emitters"]),
                                             len(r["cloth_meshes"]), len(r["cloth_components"]),
                                             r["fractured_actors"]))


if __name__ == "__main__":
    main()
