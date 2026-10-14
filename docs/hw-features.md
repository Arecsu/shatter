# Hardware-only PhysX features: what the game may use that the CPU recipe misses

Read-only survey. Sources: 2.8.1 headers (`third_party/physx-sdk-2.8.1/SDKs/Physics/include`), game exe strings
(UTF-16 and ASCII), game INI files, UE3 name tables of `CookedPC` (via `tools/ue3pkg.py`),
`captures/assets/physx_inventory.json`, and 2.8.0 core strings (`vendor-dlls/engine/v2.8.0/PhysXCore.dll`).
No decompiled code is reproduced. Tags: [V] verified in the sources above, [A] assumed or inferred.

## 1. Hardware identifiers in the 2.8.1 headers

- `NX_FF_HARDWARE` (fluid flag, 1<<4): run this fluid on the PPU. Set at creation only. [V]
- `NX_CLF_HARDWARE` (cloth flag, 1<<13): cloth on the PPU; created in a software scene; do not change later. [V]
- `NX_SBF_HARDWARE` (softbody flag, 1<<10): same idea for soft bodies. [V]
- `NX_SOFTBODY_MESH_TEARABLE` (mesh cook flag): PPU tearing needs extra cooked space; tearing also stops at
  undisclosed hardware buffer limits. [V]
- `NX_SIMULATION_HW` / `NX_SIMULATION_SW` (`NxSceneDesc.simType`): hardware vs software master scene. [V]
- `NX_SF_RESTRICTED_SCENE` (scene flag 0x20): hardware scenes only; broadphase on hardware with an actor cap. [V]
- `NX_SDKF_NO_HARDWARE` (SDK creation flag): forbid hardware use by the app. PC SW: No. [V]
- `getHWVersion()` / `NxHWVersion` (NONE=0, ATHENA_1_0=1) and `getNbPPUs()`: hardware presence queries. [V]
- `NxPhysicsSDKDesc.hwPageSize / hwPageMax / hwConvexMax`: hardware mesh-page limits (page size 65536 only). [V]
- `NxCompartmentDesc` device codes `NX_DC_CPU`, `NX_DC_PPU_0..8`, `NX_DC_PPU_AUTO_ASSIGN`; compartment types
  `NX_SCT_FLUID`=1, `NX_SCT_CLOTH`=2, `NX_SCT_SOFTBODY`=2 (cloth and softbody share compartments). [V]
- `NX_COMPARTMENT_HW_FLUID / HW_CLOTH / HW_SOFTBODY` (shape-desc flags) and SW twins. [V]
- `NxClothDesc.compartment`: NULL plus `NX_CLF_HARDWARE` puts the cloth in the first cloth compartment. [V]
- `NxFluid::getPPUTime()`, `NxCloth::getPPUTime()`: timing queries. [V]
- `NxFluid` surface-tension and fade-in-time get/set: platform table says "PC SW: No". [V]
- `NX_CONSTANT_FLUID_MAX_PACKETS` (=925) and `NX_CONSTANT_FLUID_MAX_PARTICLES_PER_STEP` (=4096): both marked
  "PC SW: Yes". [V]
- `setParameter` / `getParameter` and some scene-flag rows: "PPU: Partial". [V]

No `NxFluidDesc` member is documented as hardware-only. `maxParticles` is a generic cap (1..32767). [V]

## 2. Strings in MirrorsEdge.exe

- `bDisablePhysXHardwareSupport` (UE3 engine key and `Engine.u` property). [V]
- `PhysXGpuHeapSize`, `PhysXMeshCacheSize` (read from `Engine.Engine`), plus `enableLocalPhysXCore`,
  `PhysXLoader.dll`, `PhysXLocal/PhysXLoader.dll`, `PhysXExtensions.dll`, `EpicLocalDLLHack`. [V]
- `Software\AGEIA Technologies\` (registry key; probably the driver install check). [A]
- `PhysXEnhanced` (video option, `[SystemSettings]` key, AppCompat key), `IsPhysXEnhanced` (GameInfo script
  function), `GetPhysXSupported` (video settings UI function), `PhysXSupport` (localized option tag). [V]
- `FPhysXParticleSystem %s failed to be created in PhysX Sdk!` (log message). [V]
- Epic `Nxd*` wrapper sources (`NxdScene`, `NxdFluid`, `NxdFluidEmitter`, `NxdCloth`, `NxdSoftBody`) and RTTI for
  `NxSoftBody`/`NxdSoftBody`. Their messages cover threading and lifetime, not hardware. [V]
- Not found: "HWVersion", "getNbPPUs", "PhysX not found", "disable fluids", any PPU/GPU fallback text. The ASCII
  "PPU"/"GPU" hits are machine-code noise. [V absence]

## 3. Hardware flags in the assets

- `physx_inventory.json` (249 packages with fluids, emitters or cloth): no hardware field or flag; leaf properties
  are simulation, cloth and emitter parameters only. [V]
- Fluid `SimulationMethod`: `ESM_SPH` 29, `ESM_MIXED_MODE` 10. `MaxParticles` runs up to 10000; 30 entries exceed
  4096 (the per-step cap). The inventory's 155 fluid entries are not reconciled with the 43-config figure. [V]
- Name-table scan of all 1745 cooked `.upk`/`.u`/`.umap`: soft-body names occur only in `Engine.u`
  (`CompartmentRunList.SoftBody`, `CompartmentTimingSoftBody`, `PHYS_SoftBody`). No package uses a soft-body asset. [V]
- `bInHardware` exists on `RB_ConstraintInstance`, `RB_Handle` and `RB_Spring` in `Engine.u`. Meaning [A].

## 4. What the game does without hardware

- `BaseEngine.ini` sets `bDisablePhysXHardwareSupport=True`; the user `TdEngine.ini` (Documents folder under the
  game prefix) sets `False`, so hardware is requested on that machine. Override order [A]. [V values]
- Per the existing RE note (`docs/re-notes-fluid-mesh.md` section 4), the descriptor sets `NX_FF_HARDWARE` only when
  a "PhysX hardware available" helper and a compartment condition hold. Without hardware the fluid is a software
  fluid, not an error. [V from RE note; helper not re-read]
- Core errors `No hardware connection: creating fluid failed.` and `... hardware scene when PhysX hardware not
  accessible` exist, so the conditional flag is the guard. [V strings; causation [A]]
- `PhysXEnhanced` is false in AppCompat buckets 1-3 and true in 4-5 (`TdGame/Config/DefaultCompat.ini`); the user
  file sets `PhysXEnhanced=True`. What it gates is not located. [V values; gating [A]]
- No string says fluids are disabled without PPU/GPU; reduced particle counts are not visible in strings. [V absence]

## 5. 2.8.0 core strings about hardware

Build name `PhysXCore_FC45_GPU` (wide string). [V]
- Source paths: `SDKs/Core/Hardware/src/{fluids,cloth}/Nf*` (PPU), `SDKs/Core/Gpu/src/{fluids,cloth}/Ng*` (CUDA),
  `LowLevel/ppu/host/Pxf*`, `LowLevel/software/Pxs*` (CPU). [V]
- CUDA: `cudart32_60.dll`, `cuda*` kernels for fluid and deformable (cloth/softbody). Warnings: `CUDA not available`,
  `NVAPI not available`, `PhysXDevice.dll not found`. [V]
- HW-only restrictions: `getShapePairFlags: ... not supported in hardware!`; `hardware and software actors may not be
  jointed together`; `Software cooked triangle mesh instantiated in hardware scene`; `Attempt to create HW cloth in SW
  compartment!`; `Hardware heap ran out of memory ... fluid mesh data`. [V]
- Fluid limits: `PPU Fluids do not support more than %d particles ... clamped`; `Maximal number of static triangles per
  fluid packet exceeded. Dropping some triangles.`; `Per fluid packet limit reached ... NX_CONSTANT_FLUID_MAX_PACKETS`.
  The clamp value is not extracted yet. [V strings]
- Hardware mesh path: `NfFluidMeshDynamic`, `NfFluidMeshStatic`, `cookFluidMeshHotspot`, `NgFluidMeshStatic` heap
  stats. Cloth/softbody: `Could not tear cloth/softbody; buffer full!`. [V]

## Risks for our CPU recipe

1. **Reference path.** On a PhysX-hardware machine the game's fluids run on the PPU/CUDA path with different limits
   (packet triangle drop, PPU particle clamp, packet cap). Our recipe is the software path. Decide the reference. [A]
2. **Per-step cap.** `NX_CONSTANT_FLUID_MAX_PARTICLES_PER_STEP` = 4096 is "PC SW: Yes", but whether the SW core
   enforces it is unknown. 30 fluid entries exceed 4096 `MaxParticles`. [V header; enforcement [A]]
3. **Packet cap.** `NX_CONSTANT_FLUID_MAX_PACKETS` = 925 bounds the packet buffer; we do not model packets. [V header; effect [A]]
4. **Static-triangle drop.** The hardware mirror drops triangles past a per-packet limit, a visible original behaviour
   we would not reproduce. Our RE notes say the SW case has no cap. [V strings; impact [A]]
5. **Surface tension and fade-in time.** PPU-only in the header and absent from the game's descriptor copy list
   (`re-notes-fluid-mesh.md` section 4). Confirm no runtime setter is called. [V; call sites [A]]
6. **Performance hint.** `NX_SF_FLUID_PERFORMANCE_HINT` (static collision one step late) is not HW-only; check the
   scene-flag word the game sets. [A]
7. **Cloth/softbody HW flags.** The game may set `NX_CLF_HARDWARE` or `NX_SBF_HARDWARE` on some objects. No asset
   shows it and the code is not checked. [A]
8. **Soft bodies.** Engine has `SoftBody` compartments and `PHYS_SoftBody`, and the exe has `NxdSoftBody`, but no
   cooked asset uses them. Treat as unused. [V assets; runtime [A]]
9. **Emitter LOD budgets.** `WorldInfo.PhysXVerticalProperties` and `PhysXEmitterVerticalProperties` (`ParticlesLodMax`,
   `PacketsPerPhysXParticleSystemMax`, `bDisableLod`) live in map WorldInfo, which the inventory does not scan. They
   may throttle particles. [V names; values [A]]
10. **`bInHardware` on RB constraints.** Rigid bodies stay on CPU, but check the flag does not change constraint
    behaviour. [A]
11. **PhysXEnhanced buckets.** Low buckets disable it. The reference must match the tier we target. [V values; gating [A]]
12. **GPU heap / mesh cache sizes.** `PhysXGpuHeapSize=32`, `PhysXMeshCacheSize=8` are the only HW sizing visible;
    meaning in the SDK descriptor [A]. Harmless on CPU if not forwarded.

## Follow-ups

- Extract the `PPU Fluids` clamp value and the packet triangle limit from `PhysXCore.dll` (Ghidra, read-only).
- Trace the flags word in the game's fluid and cloth creation (`NX_FF_HARDWARE`, `NX_CLF_HARDWARE`, `NX_SBF_HARDWARE`).
- Find what `PhysXEnhanced` and `IsPhysXEnhanced` gate in `TdGame.u` bytecode.
- Read default `PhysXVerticalProperties` values from map WorldInfo.
- Oracle probe: does the 2.8.1 SW core enforce `NX_CONSTANT_FLUID_MAX_PARTICLES_PER_STEP`?
- Confirm `bInHardware` meaning in `Engine.u`.
