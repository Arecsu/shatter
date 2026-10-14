# MirrorsEdge.exe: static RE setup (audit step 1, task A1)

Historical static-analysis setup for the examined GOG executable. Decompiled exports and binary copies are local-only.

## The file
- Copy: `re/ghidra/MirrorsEdge.exe` (GOG build, `goggame-*.dll` next to it), 31,596,544 bytes,
  sha256 `58de3df21f40e9953e00be92f7749097832bad02e5b657d8485cdb61f360a3dd` [V].
- Not packed or encrypted [V]: standard sections. `.text` is 23.6 MB at entropy 6.56, `.rdata` 5.0 MB at 5.38,
  `.data` 1.3 MB at 5.62, `.rsrc` 0.6 MB at 2.19, `.reloc` 2.0 MB at 6.65. No overlay.
  38 import DLLs, 3,338 imported symbols, plus delay imports.

## Ghidra
- Project `re/ghidra/me_exe`: headless import plus auto-analysis took 800 s. The decompile export (NameVtablesAndExport.java)
  took about 32 min: 72,785 functions, 8 failed. It renamed no vtable slots, because Ghidra's RTTI pass made no
  `vftable` symbols. The RTTI data is present, though; see below.
- Auto-analysis left about 6 MB of `.text` outside any function, because that code is referenced only from data
  (UE3 native tables, vtables). Two extra passes with `re/scripts/DecompileAtList.java` (create the function if missing,
  then decompile):
  - `natives.tsv` (UE3 `int<Class>exec<Name>` string to function address, 1,791 entries, from the native registration tables
    in .data/.rdata) -> `natives_decomp.c`. Ghidra had not created 937 of these functions.
  - `vt_missing.tsv` (targets of runs of 4 or more consecutive code pointers in .rdata that were not function entries, about 48k)
    -> `vt_decomp.c`; 35,413 new functions.

## Exports in `re/out/MirrorsEdge/` (gitignored)
| file | content |
|---|---|
| decomp.c | all auto-analysed functions; header line `// ==== <addr> <name>` |
| functions.tsv | entry, name, size, callers |
| natives_decomp.c / natives.tsv | UE3 native exec functions (names are the UE3 native names) |
| vt_decomp.c / vt_missing.tsv | functions reached only from vtables/data, named `vt_<addr>` |
| rtti_vtables.txt | vtables of every `Nx*`/`Nxd*` class, recovered from the RTTI (TypeDescriptor -> COL -> vtable): `Class slot target` per line |
| strings.tsv | 104,670 strings (ascii and utf16) with address and section |
| imports.tsv | dll, symbol, IAT address, normal/delay |

## PhysX imports [V]
- `PhysXLoader.dll` (delay import, IAT 0x1f977d4-0x1f977e8): NxReleasePhysicsSDK, NxCreatePhysicsSDK (0x1f977d8),
  NxGetCookingLib, NxGetUtilLib, NxGetPhysicsSDK, NxGetPhysicsSDKAllocator.
- `PhysXExtensions.dll`: NxCreateExtension (0x1a8b98c).
- SDK init is FUN_00abfb10 (decomp.c line ~1148920). It stores the cooking lib in DAT_020009d4, the extension in DAT_020009d0
  and the SDK in DAT_020009cc (45 references).

## The Nxd wrapper layer (key finding) [V for the vtables, A for the semantics]
The exe statically links a wrapper class for every scene object: NxdScene, NxdActor, NxdCloth, NxdFluid, NxdFluidEmitter,
NxdSoftBody, Nxd*Shape, Nxd*Joint, NxdMaterial and Nxd user callbacks. RTTI strings are at 0x1f84ea4 and following. Their vtables have
exactly the 2.8.0 core's slot counts: NxdScene 155 (0x1d6ffb4), NxdActor 103 (0x1d70984), NxdCloth 92 (0x1d71a6c),
NxdFluid 81 (0x1d71ef4), NxdFluidEmitter 41 (0x1d732b4), NxdMaterial 23, NxdBoxShape 43, and so on. So engine code calls
NxdX slot i, and the wrapper calls (or buffers, then replays) NxX slot i on the real object from our shim/the core.
[A] This is PhysX 2.8's double-buffering wrapper, which lets the engine call the API while the scene simulates and
applies the buffered writes after fetchResults. That matters for when setters reach the shim.

## Anchor strings [V]
- Cloth: UE3 natives `USkeletalMeshComponent::execSetCloth*`/`execGetCloth*` (0x1c008a0-0x1c01bc8; addresses in natives.tsv,
  e.g. execSetClothPressure, execSetClothValidBounds 0xd48a40 entry from natives.tsv), `USeqAct_SetClothFrozen`,
  "Cloth Verts:%d (%d Fixed)" (stat display, decomp.c:116322), show flags CLOTH_* at 0x1bf585c.
- Fluid: `UPhysXParticleSystem`, "FPhysXParticleSystem %s failed to be created in PhysX Sdk!" (0x1c895c8),
  `FParticleSpritePhysXEmitterInstance` / `FParticleMeshPhysXEmitterInstance` (0x1cbf168, 0x1cbf080), "TotalFluidParticles",
  show flags FLUID_* at 0x1bf5920.
- Rigid bodies: `UPrimitiveComponent::execSetRBChannel` (0xd466e0), `AGameInfo::execIsPhysXEnhanced` (0xae5340).

## Printing one function
The decomp files are large (decomp.c 88 MB, vt_decomp.c 40 MB). To print the function at an address:
`cat re/out/MirrorsEdge/{decomp,natives_decomp,vt_decomp}.c | awk -v A=00d4ba70 '$0 ~ "^// ==== "A" " {p=1; next} p && /^\/\/ ==== /{exit} p'`
