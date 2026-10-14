# Audit B4: shapes, actors and scene setup in MirrorsEdge.exe

Scope: what the game sets or calls for shape/actor creation, collision filtering and scene setup, i.e. the scene state that
`shim/scene_read.cpp` reads (see `docs/audit/shim-scene.md`). Static RE only, from the text exports in `re/out/MirrorsEdge/`.
Not run; no experiment. Every claim is [V] (seen in the decompile or the headers; address given) or [A] (inferred).

## How this was found

- Wrapper layer: the exe's `NxdScene`, `NxdActor` and Nxd shape wrappers have the same slot layout as the core. A slot's
  byte offset is 4 x slot. Slot numbers for 2.8.0 were derived from the 2.8.1 headers (declaration order, minus 2 for
  NxScene and NxActor, none for NxShape, NxPhysicsSDK, NxCompartment). The offsets match the ones the shim already uses
  (e.g. NxScene getGroupCollisionFlag 48, getFilterOps 59, createFluid 126; NxActor getLinearVelocity 58, readBodyFlag 91)
  [V against `shim/scene_read.cpp`].
- Shape descriptors: `NxShapeDesc` subclasses have vtables in the RTTI census (`rtti_vtables.txt`). Writes of those vtable
  addresses (constructor inlining) locate the builders. `NxShapeDesc` member offsets were checked against the builder
  stores (type +4, localPose +8..+0x37, shapeFlags +0x38, group +0x3c, density +0x44, mass +0x48, skinWidth +0x4c,
  groupsMask +0x58) [V].
- Strings anchor the rest: `User/PhysXEnhanced`, `bDisablePhysXHardwareSupport`, `PhysXGpuHeapSize`, "Attempt to create
  NxdScene with groundPlane flag set", the wrapper method names used in its guard calls.
- Ghidra prints some offsets in decimal (e.g. `+ 100))(`). Offset census counts are therefore lower bounds [V].

Main functions (addresses in `decomp.c`):

| address | role [V unless marked] |
|---|---|
| 0x00dd9400 | UE3 aggregate-geometry to `NxShapeDesc` list builder (sphere, box, capsule, convex). 2 callers, one of them 0x00bd9c20 |
| 0x00ea5320 | static actor builder for a component (body NULL, one box shape); 13 callers |
| 0x00e855a0 | ground-plane static actor builder; 4 callers |
| 0x00ac4e30 | scene creator (`createScene`, one compartment, filter, pair flags); callers 0x004b4e90, 0x00ac53a0, 0x00e855a0 |
| 0x0145e9a0 | `NxdScene` create wrapper (refuses `groundPlane` set) |
| 0x00abfb10 | SDK init: `setParameter` calls, config reads, `NxCreatePhysicsSDK` |
| 0x00f8c2b0 | "hardware PhysX present": `DAT_01ffa8a8` and `getHWVersion` (SDK slot 31) |
| 0x00bd6d00 | `URB_BodyInstance::SetFixed` core (native `execSetFixed` 0x00afa1b0) |
| 0x00bd4d00 | body velocity/pose write after SetFixed (`setLinearVelocity`) |
| 0x00f8c6b0 | builds a 4-word `NxGroupsMask` from an RB channel index |
| 0x00ede9b0 | sets or clears one bit of a component's 32-bit channel mask |
| 0x00f8c510 | vector x 0.02 (engine units to metres, [A] on the unit meaning) |

## 0. Summary

- Every shape the builders create starts from the SDK default `shapeFlags` = 0x120008 = VISUALIZATION | CLOTH_TWOWAY |
  SOFTBODY_TWOWAY. None of the builders clears or adds any other shape bit. So the game's shapes carry CLOTH_TWOWAY
  [V], which the cloth path does not read (harmless only if the game never sets NX_CLF_COLLISION_TWOWAY, see re-notes-cloth.md:636).
- The one scene-level filter the game sets is `setFilterOps(OR, OR, SWAP_AND)` with bool true. So NX_FILTEROP_SWAP_AND is
  used by the game [V values; argument order [A]]. The shim's SWAP_AND is a guess (shim-scene.md gap 13) and is now a real case.
- Collision channels become shape `groupsMask` bit 0 (1 << channel) [V]. Static actors get actor group 2 [V].
- One rigid-body compartment only (CPU, cell size 20, table power 8). No fluid or cloth compartment is created in the scene
  creator [V for that function; a full census is open].
- Kinematic is `NX_BF_KINEMATIC` (0x80) raised on a dynamic actor by `SetFixed` (raiseBodyFlag slot 89) [V].
- Scene-wide `NX_SKIN_WIDTH` = 0.1 (metres) is set once in SDK init [V]. Shapes use skinWidth = -1 (take the SDK value) [V].
- Primitive sizes carry a fixed +0.025 margin (sphere radius, box half extents) [V].

## 1. Shape and actor creation

| PhysX API item | set/called? | address | UE3 source property / function | notes |
|---|---|---|---|---|
| NxShapeDesc.shapeFlags default = NX_SF_VISUALIZATION (1<<3) \| NX_SF_CLOTH_TWOWAY (1<<17) \| NX_SF_SOFTBODY_TWOWAY (1<<20) = 0x120008 | set (default, never changed) [V] | 0x00dd9400 (+0x38 store), 0x00ea5320 (local_180), 0x00e855a0 (local_60), 0x010a46b0 | none (default inherited by every builder) | the four builders and the box-from-convex path all store 0x120008 and nothing else |
| NX_SF_DISABLE_COLLISION (1<<4), NX_SF_FLUID_* (1<<8, 1<<10, 1<<11, 1<<12), NX_SF_CLOTH_* (1<<15..17), NX_SF_SOFTBODY_* (1<<18..20), NX_SF_TRIGGER_* | not set by any builder [V] | builders above | UE3 per-element or per-component flags, not located | open: must be set elsewhere (setFlag, see section 7) |
| NX_SF_VISUALIZATION (1<<3) | set, default [V] | as row 1 | none | no collision effect |
| NxShapeDesc.skinWidth | -1 (SDK default) [V] | 0x00dd9400, 0x00ea5320 | none | SDK NX_SKIN_WIDTH applies (row 24) |
| NxShapeDesc.density / mass | 1.0 / -1.0 [V] | 0x00dd9400, 0x00ea5320 | none | mass from density |
| NxShapeDesc.group | 0 [V] | 0x00dd9400 | none | |
| NxShapeDesc.materialIndex | 0 [V] | 0x00dd9400 | UE3 physical material not mapped | |
| NxShapeDesc.ccdSkeleton | NULL [V] | 0x00dd9400 | none | |
| NxShapeDesc.userData, name | NULL [V] | 0x00dd9400 | none | |
| NxShapeDesc.groupsMask bits0 = 1 << channel; words 1..3 = 0; word 2 optional | set [V] | 0x00f8c6b0 (called from 0x00ea5320 with channel 2 → word0 = 0x4) | RB channel of the primitive (UE3 `RBChannel`) [A] | 3rd argument of 0x00f8c6b0 (local_238) comes from an unknown source [A] |
| NxShapeDesc.nonInteractingCompartmentTypes | 0 [V] | 0x00dd9400 | none | |
| NxSphereShapeDesc (vtable 0x1d70f54, type 1): radius = element radius x scale + 0.025 | set [V] | 0x00dd9400 (sphere loop) | UE3 sphere element (AggGeom) [A] | margin 0.025 m [V] |
| NxBoxShapeDesc (vtable 0x1d74204, type 2): half extents = element size x scale x 0.5 + 0.025 | set [V] | 0x00dd9400 (box loop) | UE3 box element [A] | |
| NxCapsuleShapeDesc (vtable 0x1d73e80, type 3) | set [V] (type and vtable); radius/height fields partly unresolved | 0x00dd9400 (capsule loop, the 0x1b/0x1c stores are not mapped) | UE3 sphyl element [A] | [A] on field meaning |
| NxConvexShapeDesc (vtable 0x1d73a20, type 5): convex mesh from the extension's cook call (`DAT_020009d0` slot 0x18) | set [V] | 0x00dd9400 (convex loop) | UE3 convex element | cooked at load time [A] |
| convex with 8 vertices and 12 triangles becomes a box desc (vtable 0x1d74204) | set [V] (test); purpose [A] | 0x010a46b0 | UE3 convex-to-box shortcut [A] | |
| per-element disable bit, skipped at build: sphere (+0x44 bit 0), box (+0x4c bit 0), convex (+0x48 bit 0) | read [V] | 0x00dd9400 | element "no collision" flag [A] name | the element is not created at all |
| NxPlaneShapeDesc (vtable 0x1d743c0, ground) with a normal and offset from world data | set [V] (plane desc and actor); normal and offset values [A] | 0x00e855a0 (desc at local_98 region; `FUN_010fc960` world data +0xcd0/+0xcd8) | world bounds / KillZ-like plane [A] | the plane lives in a static actor; the NxdScene wrapper forbids `groundPlane` [V] |
| NxTriangleMeshShapeDesc (vtable 0x1d73860) | set [V] (vtable store) | 0x00ac4440, 0x00d4d360 (7 callers), 0x00d4ee50 | UE3 static mesh / BSP collision [A] | builders and callers not fully traced |
| NxHeightFieldShapeDesc (vtable 0x1d7360c) | set [V] (vtable store) | 0x00d4e650 (one caller) | UE3 terrain (`ATerrain`) [A] | |
| NxWheelShapeDesc (vtable 0x1d73c34) | set [V] (vtable store, 0x00fb7a70 with 4 callers); callers unknown | 0x00fb7a70, 0x00fb5630 (no callers) | none found [A] | likely unused in practice [A] |
| NxBoxShapeDesc vs NxSphereShapeDesc: the other builders 0x00bb52e0, 0x00f8db90, 0x010a46b0 use the same vtables | set [V] | as listed | | not separately analysed |
| debug draw helpers for each primitive when the `param_3` argument is set | set [V] (code) | 0x010a2d90, 0x010a3180, 0x010a38d0, 0x010a3f90 | UE3 physics debug draw [A] | the debug path, not gameplay |
| NxActorDesc.body = NULL (static actor) | set [V] | 0x00ea5320 (body pointer slot = 0) | static mesh / component bounds actor | |
| NxActorDesc.flags \|= NX_AF_DISABLE_RESPONSE (1<<1) | set [V] | 0x00ea5320 (flags slot = 2 after an OR) | UE3 component with no response [A] | the only actor flag found in the census; NX_AF_DISABLE_COLLISION (1<<0) not found |
| NxActorDesc.density = 1.0 | set [V] | 0x00ea5320 | none | |
| NxActorDesc.compartment | NULL (default compartment) [V] | 0x00ea5320 | none | rigid-body compartment by default [A] |
| NxActor::setGroup(2) on the new static actor (slot 21, 0x54) | set [V] | 0x00ea5320 | UE3 static world actor group [A] | group 2 is a constant, not a channel |
| createActor (NxScene slot 7, 0x1c) for static components and for the ground plane | called [V] | 0x00ea5320, 0x00e855a0 (via 0x00f8e0e0) | none | the created actor pointer is stored on the component (+0x3bc) [V] |
| NX_AF_LOCK_COM, NX_AF_FORCE_CONE_FRICTION, NX_AF_USER_ACTOR_PAIR_FILTERING, NX_AF_FLUID_DISABLE_COLLISION (1<<3) | not found [V for the builders; open for other paths] | none | | open |
| NX_BF_KINEMATIC (1<<7) on a dynamic body | set/clear [V] | 0x00bd6d00 (SetFixed core): isDynamic (slot 29) then raiseBodyFlag (slot 89, 0x164) when fixed; clearBodyFlag (slot 90, 0x168) when unfixed; readBodyFlag (slot 91, 0x16c) | URB_BodyInstance.SetFixed; natives execSetFixed 0x00afa1b0; UPhysicsAssetInstance SetNamedBodiesFixed 0x00afe010, SetAllBodiesFixed 0x00af9dc0 [A on the link] | callers 0x00bd7590, 0x00bd7650, 0x00bd8780, 0x00d48ca0, vt 0x00d42e40 |
| body velocity written on the fixed path (setLinearVelocity, slot 56, 0xe0) | called [V] | 0x00bd4d00 (callers 0x00d42bd0, 0x00d46af0, 0x00d46be0, 0x00ea1e90, 0x00fb6360) | body reposition / velocity reset [A] | |
| getLinearVelocity (slot 58, 0xe8) | called [V] | 0x00d42bd0 | | |
| NX_BF_DISABLE_GRAVITY, NX_BF_FROZEN_*, NX_BF_FILTER_SLEEP_VEL, NX_BF_ENERGY_SLEEP_TEST | not found [V for the SetFixed path]; open | | | open |
| NxBodyDesc (no vtable): body created for dynamic components | not located | | UE3 dynamic rigid body path (FRBPhysScene-like) | open |
| UE3 KActor warnings: "BlockRigidBody set to FALSE and no joints", "StaticMesh BodySetup is NULL" | read [V] (bit 0x40 of body instance +0x10c) | 0x00ea4990 | `BlockRigidBody` (`Engine.PrimitiveComponent.BlockRigidBody`, string 0x01b137a8) | editor-side check only |

## 2. Collision channels, groups and filtering

| PhysX API item | set/called? | address | UE3 source property / function | notes |
|---|---|---|---|---|
| RB channel index (byte) | set [V] | native `execSetRBChannel` 0x00d466e0 writes the component's byte at +0x44 and calls virtual slot 0x1d8 | `UPrimitiveComponent.RBChannel` / `SetRBChannel` | channel index is 0..31 [A] |
| RB collide-with mask (32-bit) | set [V] | native `execSetRBCollidesWithChannel` 0x00d46630 → 0x00ede9b0 (set or clear bit channel) | `RBCollideWithChannels` / `SetRBCollidesWithChannel` | |
| default collide-with bits of a new static component: channels 0, 5, 6 enabled | set [V] | 0x00ea5320 (0x00ede9b0 with (0,1), (6,1), (5,1)) | defaults [A on names] | |
| BlockRigidBody | set [V] | native `execSetBlockRigidBody` 0x00d454b0 → virtual slot 0x1ac | `PrimitiveComponent.BlockRigidBody` | |
| RB dominance group | set [V] | native `execSetRBDominanceGroup` 0x00d45620 → virtual slot 0x1b4 | `PrimitiveComponent.RBDominanceGroup` (string "Dominance" at 0x01be0ff0) | |
| NxScene::setFilterOps(OR, OR, SWAP_AND) | called [V values; argument order [A]] | 0x00ac4e30 (slot 55, 0xdc) | scene-wide filter | NX_FILTEROP values: OR = 1, SWAP_AND = 6 (Nxp.h:2085-2092) |
| NxScene::setFilterBool(true) | called [V] | 0x00ac4e30 (slot 56, 0xe0) | | |
| NxScene::setFilterConstant0(0) | called [V] | 0x00ac4e30 (slot 57, 0xe4) | | |
| NxScene::setFilterConstant1(K1) | called [V]; K1 value [A] | 0x00ac4e30 (slot 58, 0xe8) | | K1 comes from a stack slot reused during the call; value not resolved |
| NxScene::setActorGroupPairFlags (slot 51, 0xcc), 5 calls | called [V]; group numbers [A] | 0x00ac4e30 | UE3 pair notify setup | flags 0x8a = START_TOUCH (1<<1) \| TOUCH (1<<3) \| FORCES (1<<7); 0x1008a adds CONTACT_MODIFICATION (1<<16); 0x10000 = CONTACT_MODIFICATION only (NxUserContactReport.h:29-42) |
| NX_NOTIFY_CONTACT_MODIFICATION used | set [V] (flags); the contact-modify callback is not hooked by the shim | 0x00ac4e30 | UE3 contact modify (none in the shim) | rigid-body only [A] |
| channel bit to mask word (groupsMask) | set [V] | 0x00f8c6b0 | | |
| NxScene::setGroupCollisionFlag (slot 47, 0xbc) | not called in the scene creator; census open | | | open |
| NxScene::setDominanceGroupPair (slot 49, 0xc4) | not called in the scene creator; census open | | | open |
| NxScene::setUserActorPairFiltering (slot 105, 0x1a4), setUserTriggerReport (0x194), setFluidUserNotify, setClothUserNotify | wrapper names only; not located as calls | | | open |

## 3. Scene setup

| PhysX API item | set/called? | address | UE3 source property / function | notes |
|---|---|---|---|---|
| NxPhysicsSDK::setParameter(NX_SKIN_WIDTH = 1, 0.1) | set [V] | 0x00abfb10 (slot 2, 0x08) | none | scene-wide default skin width 0.1 m |
| NxPhysicsSDK::setParameter(NX_ASYNCHRONOUS_MESH_CREATION = 96, 1.0) | set [V] | 0x00abfb10 | none | enum value from Nxp.h:2003 |
| NxPhysicsSDK::setParameter(NX_ADAPTIVE_FORCE = 68, 0.0) | set [V] | 0x00abfb10 | none | |
| PhysXGpuHeapSize, PhysXMeshCacheSize | read [V] when `DAT_01ffa8a8` is set | 0x00abfb10 | `Engine.Engine` config keys | |
| NxCreatePhysicsSDK, NxCreateExtension, NxGetCookingLib | called [V] | 0x00abfb10 | none | loader from PhysXLoader.dll (imports) |
| hardware check (getHWVersion, slot 31) | called [V] | 0x00f8c2b0 (gated by `DAT_01ffa8a8`) | `bDisablePhysXHardwareSupport` (`Engine.Engine`, read at 0x0129e050) | |
| PhysXEnhanced (game setting) | read [V] | 0x0040f750, 0x00410530, 0x00b313d0, 0x00b32bd0, 0x00b333e0; native `AGameInfo::execIsPhysXEnhanced` 0x00ae5340 | `User/PhysXEnhanced` ini key; GameInfo function | which of these gate the scene is not resolved [A] |
| NxPhysicsSDK::createScene (slot 4, 0x10) | called [V] | 0x00ac4e30 (the wrapper 0x0145e9a0 is used first when present) | level physics scene | the desc is a local (not NxSceneDesc 2.8.1 layout; not mapped in full) |
| NxSceneDesc.groundPlane | must be 0 [V] | 0x0145e9a0 (checks the desc at +0x44) | none | the ground is an explicit actor (row 0x00e855a0) |
| createCompartment (scene slot 38, 0x98), one call | called [V] | 0x00ac4e30 | UE3 physics compartment | see next row |
| NxCompartmentDesc: type 0 (NX_SCT_RIGIDBODY), deviceCode 0xffff0000 (NX_DC_CPU), gridHashCellSize 20.0 (0x41a00000), gridHashTablePower 8, flags 8 (NX_CF_INHERIT_SETTINGS), threadMask 0, timeScale 1.0 | set [V] | 0x00ac4e30 | none | cell size is in the scene's distance units [A] |
| device code choice after the call: NX_DC_PPU_AUTO_ASSIGN (0xffff0001) when a hardware PhysX is present | set [V] (value); use [A] | 0x00f8c2b0 used at 0x00ac4e30 | none | the first compartment is always CPU at creation [V] |
| fluid compartment (NX_SCT_FLUID) or cloth compartment (NX_SCT_CLOTH) | not created in the scene creator [V] | 0x00ac4e30 | none | open: census of createCompartment callers |
| the compartment object exposes scene-type slots (setActorGroupPairFlags, setFilter*) | called [V] | 0x00ac4e30 | none | the compartment is treated as a scene here [A] |
| NxScene::setGravity (slot 5, 0x14) | not called in the scene creator [V] | | gravity from world info: `AWorldInfo.SetLevelRBGravity` native 0x00ae66b0; "gravity" string lookups 0x01509a70, 0x0150cde0 | scene desc gravity source [A]; open |
| NxScene::setTiming (slot 82, 0x148) | not located in the scene creator; open | | | timing is the core default unless set elsewhere [A] |
| NxSceneDesc flags (NX_SF_*), max actors/shapes, timestep | not mapped | | | open |
| NxScene::createCompartment for hardware cloth or fluid | not found | | | open |

## 4. Kinematic and pose

| PhysX API item | set/called? | address | UE3 source property / function | notes |
|---|---|---|---|---|
| NxActor::moveGlobalPose (slot 13, 0x34) | not located | | `MoveKinActorTransform` native 0x00afb170 (calls a UE3 function at 0x00dbf850; not traced) | per-frame kinematic moves: open |
| NxActor::setGlobalPose (slot 5, 0x14) | not located as a call; many generic hits | | | open |
| body fixed (kinematic) toggle | set [V] | 0x00bd6d00 | `SetFixed` | see section 1 |

## 5. Open questions

1. NX_SF_* bits other than the default: which path sets FLUID_DRAIN, FLUID_DISABLE_COLLISION, FLUID_TWOWAY, DISABLE_RESPONSE or
   CLOTH_* on a shape after creation? Candidate: NxShape setFlag (slot 5, 0x14). A census by slot is too noisy (Ghidra mixes
   decimal and hex). Needed for gap 3 in shim-scene.md.
2. NX_BF_* and NX_AF_* beyond KINEMATIC and DISABLE_RESPONSE: the dynamic body path (creation of NxBodyDesc) is not yet found.
3. Compartments: whether fluid and cloth get their own compartment (the cloth desc has a compartment field, shim
   `kOffCompartment`). The cloth/fluid creation sites were not traced here.
4. Scene timing and gravity: setTiming and setGravity are not in the scene creator. Locate the callers (slot 82, 0x148; slot 5, 0x14).
5. The actor-group pair flags (5 calls): group numbers are not resolved, and whether the contact-modify flag implies a
   callback that the game installs (setUserContactModify) is not checked.
6. Filter constant K1: the value is in a stack slot reused during the call; the setFilterOps argument order is inferred.
7. The 0.025 margin: confirm from the packages (primitive size vs collision size) that UE3 does not also add it.
8. kinematic move path: where moveGlobalPose is called per frame (0x00dbf850 and the physics tick).
9. Heightfield, triangle mesh and wheel builders: callers and per-element flags not traced.
10. Element flag bits (+0x44, +0x48, +0x4c bit 0): name and owner class.

## Independent review (spot-checks)
- 0x120008 in the shape builders: confirmed at 0xdd9400 (two stores of 0x120008 to the shape desc). Note that this value is
  exactly the NxShapeDesc::setToDefault value (NX_SF_VISUALIZATION|NX_SF_CLOTH_TWOWAY|NX_SF_SOFTBODY_TWOWAY, NxShapeDesc.h:304),
  so the engine keeps the SDK default and adds nothing. CLOTH_TWOWAY on every shape only matters for cloths with
  NX_CLF_COLLISION_TWOWAY.
- Filter setup at 0xac4e30: confirmed calls on the new scene at vtable offsets 0xdc/0xe0/0xe4/0xe8 = NxScene slots 55-58
  (setFilterOps, setFilterBool(1), setFilterConstant0, setFilterConstant1; the getters are 59-62 in shim/scene_read.cpp:30-31).
  The decompiler lost setFilterOps' arguments; the (OR, OR, SWAP_AND) values are [A] until checked in disassembly. The shim
  reads the ops back live from the scene, so it follows whatever the game sets. Its SWAP_AND (scene_read.cpp:143, a & b
  with dwords 0/1 swapped against 2/3) matches the SDK documentation's definition [A]. This is the standard UE3 RB channel
  filter: bits0 = own channel, bits2 = collide-with channels.
