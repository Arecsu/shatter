# ABI: what the game uses vs the headers we have

## The game's contract
- `MirrorsEdge.exe` calls `NxCreatePhysicsSDK(0x02080000, alloc, out, desc, &err)` and
  `NxGetCookingLib(0x02080000)` (three `push 0x2080000` sites near VA `0xabfb8d..0xabfcb8`).
  The loader (`PhysXLoader.dll` 2.8.0.7, built from `e:\projects\EpicPhysX\Epic\2.8.0_FC7\`)
  resolves `<PhysXCore Path>\v2.8.0\PhysXCore.dll`. The examined reference file is **2.8.0.45** from PhysX System Software 9.13.1220.
- The game fills a **7-dword** `NxPhysicsSDKDesc` (stack `+0x1c..+0x34`):

  | off | value | 2.8.1 public header field |
  |----:|------:|---------------------------|
  | 0x00 | 0x10000 | hwPageSize |
  | 0x04 | 0x100 | hwPageMax |
  | 0x08 | 0x800 | hwConvexMax |
  | 0x0c | 0x80000000 | cookerThreadMask |
  | 0x10 | 0 | flags |
  | 0x14 | 0x80, overwritten by ini `PhysXGpuHeapSize` (32) | — *(not in 2.8.1 header)* |
  | 0x18 | 0xffffffff, overwritten by ini `PhysXMeshCacheSize` (8) | — *(not in 2.8.1 header)* |

  The names of fields 6–7 are **inferred** from the ini keys that feed them. The SDK desc in
  the game's branch is larger than in the public 2.8.1 header.

## Headers available: 2.8.1 (changelist 30725), from an unofficial GitHub mirror
`third_party/physx-sdk-2.8.1` = https://github.com/sigmaco/physx-v2.8.1 @ 687e1e9c.
They match the v2.8.1 core (verified end to end: SDK, scene, actors, step).

## Vtable comparison, v2.8.0.45 vs v2.8.1.57 (`tools/rtti_vtables.py`)
Full table: [abi/vtable-diff.txt](abi/vtable-diff.txt). Slot counts from MSVC RTTI. Equal counts are necessary but
not sufficient: slots could still be reordered.

Same slot count in both versions:
`NxFluid` 81, `NxFluidEmitter` 41, `NxCloth` 92, `NxClothMesh` 3, `NxSoftBody` 83, `NxActor` 103,
`NxCompartment` 14, `NxPhysicsSDK` 34, `NxUserAllocator` 8.

Different slot counts:
- `NxScene` 155 → 157 (NpScene 156 → 170)
- every joint interface +4 slots
- `NxFoundationSDK` 9 → 10
- force-field shape groups +1
- Internal `NfFluid`/`NgFluid` 87 → 91, `NfCloth`/`NgCloth` 122 → 124

**Conclusion:** the 2.8.1 headers are **not** a drop-in ABI for the core the game uses.
`NxScene`, which owns `createFluid`/`createCloth`/`createCompartment`, is the critical mismatch.
Descriptor struct layouts (`NxFluidDesc`, `NxClothDesc`, ...) are not covered by this check at all.

## Update (fluid desc / NxScene slots)
`docs/re-notes-fluid-mesh.md` section 3: the `NxFluidDesc`, `NxFluidEmitterDesc` (0x74) and `NxParticleData` layouts the 2.8.0
core reads are identical to the 2.8.1 headers, and the `NxScene` slots for createFluid/createCloth/createSoftBody (126..136)
are the same in both; the 155 vs 157 difference is only at slot 153+ (`setDynamicTreeRebuildRateHint`).

## Consequences
- The hello-world harness compiles against 2.8.1 and requests 2.8.1, so headers and core agree.
  Its `2.8.0-probe` mode only creates and releases the SDK through C exports.
- The oracle harnesses use 2.8.1 with matched headers. The shim independently checks the game's 2.8.0 ABI. See [NOTES.md](../NOTES.md) and [CONTRIBUTING.md](../CONTRIBUTING.md).
