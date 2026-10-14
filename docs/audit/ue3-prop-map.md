# UE3 property map: MirrorsEdge.exe cloth offsets (audit step 2)

Maps the raw offsets that MirrorsEdge.exe reads for cloth to UE3 property names. "Cloth data" is the
USkeletalMesh object (USkeletalMeshComponent+0x1c8 is its `SkeletalMesh` pointer). "comp" is the
USkeletalMeshComponent. Every row is marked [V] (checked against the exe decompile or the package) or [A] (inferred).
Defaults come from `Default__SkeletalMesh` (export 19625) and `Default__SkeletalMeshComponent` (export 8636) in
`Engine.u`. A bool that is not listed there is False. Per-mesh values in the content can differ from these defaults.

## Fluid (UPhysXParticleSystem)

Historical mapping. Source: `TdGame/CookedPC/Engine.u`, export table and property export bodies, read with
`tools/ue3pkg.py` helpers (read-only). Native offsets are not serialized; they are derived from declaration order and checked
against the offsets in `docs/audit/exe-fluid.md` §2.

### Method and evidence

- Declaration order [V]: the UField `Next` link in each property export body (the class Children head is `MaxParticles`) gives
  a 27-property chain. The export table lists the same properties in reverse order. Enums and functions are linked in the same
  chain but take no storage.
- Layout [V by anchors]: the first property sits at +0x3c, which is the parent (`Object`) size on this build. Natural
  alignment, the 7 bools share dword +0x54, the Vector is 12 bytes, the `Pointer` struct is 4 bytes. Every name-to-offset
  pair below agrees with the exe offsets in exe-fluid.md §2 (MaxParticles +0x3c, CollisionDistance +0x40,
  RestitutionWithStatic +0x44, RestParticleDistance +0x70, CollisionResponse +0x84, ParticleReserve +0x88, and every other
  offset in exe-fluid.md §2 that the table uses). That is well over the 3 anchors the brief asked for.
- Defaults [V]: the CDO `Default__PhysXParticleSystem` tagged properties (Engine.u export 17108). A property missing from the
  CDO is 0 / false / null / zero vector. [A] that no C++ constructor overrides it (the exe constructor values I could check
  match the CDO: maxParticles 0x7fff, restDensity 1000, stiffness 20, viscosity 6).
- Bool bit positions [A]: 1 << declaration index within the dword. Consistent with the CDO (bDynamicCollision and
  bStaticCollision are True, so bits 0x1 and 0x4 are set by default) and with exe-fluid.md's collision default of 3 (bits 0x1 and
  0x4 both set). The names of bits 0x2, 0x8, 0x10, 0x20, 0x40 are declaration-order inferences, not read from the exe.

### Table

| offset | bits | UE3 property (owner) | type | CDO default | evidence | status |
|---|---|---|---|---|---|---|
| +0x3c | all (int) | MaxParticles | int | 32767 (0x7fff) | package chain; exe anchor +0x3c | [V] |
| +0x40 | all | CollisionDistance | float | 10.0 | package; exe anchor +0x40 | [V] |
| +0x44 | all | RestitutionWithStaticShapes | float | 0.5 | package; exe anchor +0x44 | [V] |
| +0x48 | all | RestitutionWithDynamicShapes | float | 0.5 | package; exe +0x48 | [V] |
| +0x4c | all | FrictionWithStaticShapes | float | 0.05 | package; exe +0x4c | [V] |
| +0x50 | all | FrictionWithDynamicShapes | float | 0.5 | package; exe +0x50 | [V] |
| +0x54 | 0x01 | bDynamicCollision | bool | True | package; CDO True; exe collision bit "dynamic" | [V] offset, [A] bit |
| +0x54 | 0x02 | bDisableGravity | bool | False | package; exe "disable gravity" bit | [V] offset, [A] bit |
| +0x54 | 0x04 | bStaticCollision | bool | True | package; CDO True; exe collision bit "static" | [V] offset, [A] bit |
| +0x54 | 0x08 | bTwoWayCollision | bool | False | package; exe "two-way" bit | [V] offset, [A] bit |
| +0x54 | 0x10 | bDestroy | bool | False | package; exe "pending, set before destroy" | [V] offset, [A] name |
| +0x54 | 0x20 | bSyncFailed | bool | False | package; exe "create failed" | [V] offset, [A] name |
| +0x54 | 0x40 | bIsInGame | bool | False | package; exe "proxy actor state" | [V] offset, [A] name |
| +0x58 | all | MaxMotionDistance | float | 64.0 | package; exe +0x58 | [V] |
| +0x5c | all | Damping | float | 0.0 (absent) | package; exe +0x5c | [V] |
| +0x60 | 12 bytes | ExternalAcceleration | struct Core.Object.Vector | (0,0,0) | package; exe +0x60 | [V] |
| +0x6c | byte | SimulationMethod | enum byte ESimulationMethod | 1 = ESM_NO_PARTICLE_INTERACTION | package; exe +0x6c | [V] |
| +0x6d | byte | PacketSizeMultiplier | enum byte EPacketSizeMultiplier | 2 = EPSM_16 | package; exe +0x6d | [V] |
| +0x70 | all | RestParticleDistance | float | 64.0 | package; exe anchor +0x70 | [V] |
| +0x74 | all | RestDensity | float | 1000.0 | package; exe +0x74 | [V] |
| +0x78 | all | KernelRadiusMultiplier | float | 2.0 | package; exe +0x78 | [V] |
| +0x7c | all | Stiffness | float | 20.0 | package; exe +0x7c | [V] |
| +0x80 | all | Viscosity | float | 6.0 | package; exe +0x80 | [V] |
| +0x84 | all | CollisionResponseCoefficient | float | 0.2 | package; exe anchor +0x84 | [V] |
| +0x88 | all (int) | ParticleReserve | int | 0 (absent) | package; exe anchor +0x88 | [V] |
| +0x8c | 4 bytes | CascadeScene | struct Core.Object.Pointer (native) | null | package; exe stores a proxy actor pointer at +0x8c | [V] offset, [A] name |
| +0x90 | 4 bytes | PSys | struct Core.Object.Pointer (native) | null | package; exe FPhys pointer at +0x90 | [V] offset and role |

The object's last field ends at +0x94 (PSys + 4) [V by this layout].

### Enums (values from the enum exports)

- ESimulationMethod (Engine.u export 17078): ESM_SPH = 0, ESM_NO_PARTICLE_INTERACTION = 1, ESM_MIXED_MODE = 2, ESM_MAX = 3 (sentinel).
  The exe's byte mapping (0 -> SPH, 1 -> no particle interaction, 2 -> mixed) agrees [V].
- EPacketSizeMultiplier (export 17079): EPSM_4 = 0, EPSM_8 = 1, EPSM_16 = 2, EPSM_32 = 3, EPSM_64 = 4, EPSM_128 = 5, EPSM_MAX = 6.
  The exe's byte mapping (0..5 -> 4..128) agrees [V].

### Notes for the content scan

- The CDO default SimulationMethod is ESM_NO_PARTICLE_INTERACTION (collide-only). An instance with no tagged SimulationMethod
  is collide-only, so the SPH content (12 smoke configs per CLAUDE.md) must carry an explicit ESM_SPH or ESM_MIXED_MODE tag.
  An instance that does not tag a bool gets the CDO value above (bDynamicCollision and bStaticCollision on by default).
- The CDO has no ParticleReserve, Damping or ExternalAcceleration tags, so their defaults are 0.

### Other UPS members

- No other UProperty exists on UPhysXParticleSystem (27 properties, 2 enums, 2 native Pointer slots) [V].
- The class export has no further own properties. Object.Pointer is a ScriptStruct with one member `Dummy` (IntProperty) [V],
  hence 4 bytes.

### Summary

- 27 UProperties on UPhysXParticleSystem, +0x3c..+0x93. Every offset that exe-fluid.md §2 uses (+0x3c..+0x90, including the
  flags byte +0x54) maps to exactly one property. Two of the exe's [A] names are now confirmed by the package: SimulationMethod (+0x6c) and ExternalAcceleration (+0x60).
- The flags byte +0x54 holds 7 bools (0x01 bDynamicCollision, 0x02 bDisableGravity, 0x04 bStaticCollision, 0x08 bTwoWayCollision,
  0x10 bDestroy, 0x20 bSyncFailed, 0x40 bIsInGame). The bit names are [A] by declaration order.
- The +0x8c and +0x90 native pointer slots are named CascadeScene and PSys in UE3. The exe uses them as the proxy actor and the
  FPhys pointer, so the UE3 names do not describe their use.

### Open questions

1. Bit names for +0x54 (0x02, 0x08, 0x10, 0x20, 0x40) rest on declaration order. I found no bitmask field in the bool export
   bodies (the candidate word is 0 in every bool export), so only the exe's use of the bits can confirm them.
2. NxFluidDesc constants: exe-fluid.md writes 0.05 to desc 0x26 (staticFrictionForStaticShapes) and 0.5 to desc 0x2a
   (staticFrictionForDynamicShapes). Those equal the CDO defaults of FrictionWithStaticShapes (0.05) and
   FrictionWithDynamicShapes (0.5). Check whether these two desc slots should read the UPS friction properties instead of
   constants. Not verified here.
3. Whether any native constructor overrides a CDO default that is absent from the tagged properties (see Method). Checked only for
   MaxParticles, restDensity, stiffness and viscosity, where the exe constructor agrees with the CDO.
4. UObject size 0x3c is inferred from the exe anchors, not read from the Object class body (Core.u, not decoded here).

Independent spot-check: the CDO `Default__PhysXParticleSystem` decoded with tools/ue3pkg.py lists the tags in this
layout order with the defaults above (CollisionResponseCoefficient 0.2, SimulationMethod ESM_NO_PARTICLE_INTERACTION) [V].

## Cloth

### Method

1. **Layout from the package.** Every UProperty export in `Engine.u` has a serialized body of 11 dwords plus a
   type-specific tail. Word 0 is the `UField::Next` link, word 5 is `ArrayDim`, words 6-7 are `PropertyFlags`, word 8 is
   the `Category` FName, and the tail holds the referenced object (struct, class, inner or enum). Only the exports whose
   outer is the class belong to that class (function parameters share the same `Next` chain). Offsets follow chain order,
   with 4-byte alignment (1 for bytes) and bool bits packed into one dword in chain order. Sizes come from the type:
   float/int/object/component/pointer 4, name 8, array 12, byte 1, vector 12, Matrix 64, BoxSphereBounds 28, Color 4,
   RBCollisionChannelContainer 4.
2. **Base offsets.** The component's first own property is `SkeletalMesh` at +0x1c8, which fixes the component base.
   The SkeletalMesh cloth block starts at +0xa0 relative to the chain start. That value is the sum of the parent UObject
   part and the three members whose size the tool could not compute (BoxSphereBounds `Bounds`, the `NameIndexMap` map,
   and the `LODModels` indirect array). The anchors below fix the sum, so the exact split is not needed.
3. **Anchors (all match):**
   - Component: bool group +0x324 gives bEnableClothSimulation bit0, bDisableClothCollision bit1, bClothFrozen bit2,
     bAttachClothVertsToBaseBody bit5 (matches FUN_00d4af70, FUN_00d4b930, FUN_00d43700, FUN_00d496f0, FUN_00d47240).
     ClothSim (Pointer) at +0x35c, SceneIndex at +0x360, ClothExternalForce at +0x328, ClothRBChannel at +0x3c8,
     ClothRBCollideWithChannels at +0x3cc, ClothForceScale at +0x3d0, ClothAttachmentTearFactor at +0x3f0,
     NumClothMeshVerts at +0x388, NumClothMeshIndices at +0x38c, ClothMeshPosData at +0x364, ClothMeshNormalData at +0x370,
     ClothMeshIndexData at +0x37c, ValidBoundsMin at +0x3d8 (FUN_00d4b6f0 reads +0x3d8), bool group +0x314 bit 0x200
     (FUN_00d49090 gate).
   - Mesh: FUN_00d492d0 reads +0x1a4 bits 0/1/2, +0x1a8 .. +0x1d8 floats/ints, +0x1dc bits 0..3, +0x1fc bit0, +0x200,
     +0x16c, and the offsets listed in the table below.
     FUN_00d4ba70 reads +0x160 (vertex count), +0x18c/+0x190 (index buffer ptr/count), +0x174/+0x178/+0x17c (welded
     path), +0x1b0 (density), +0x204 (tear reserve). FUN_00d48db0 reads the attachment table +0x1e0/+0x1e4, with
     `ClothSpecialBoneInfo` = {BoneName @0 (8 bytes), BoneType @8 (byte), AttachedVertexIndices @0xc (TArray<int>)}, size
     24. This matches the exe's "two names, byte at +8, int array at +0xc" usage [V]. FUN_00d49090 reads +0x1f0/+0x1f4/+0x1f8.
4. **Name-only check.** Names come from the package. The exe has no property strings for them. The exe's functions are
   consistent with these names, but the names themselves are [A] unless a row says otherwise.

### USkeletalMesh cloth data (object at comp+0x1c8)

Offset is from the start of the SkeletalMesh object. Bool bits are masks within the dword shown.

| offset | bits | UE3 class.property | type | default | evidence | tag |
|---|---|---|---|---|---|---|
| +0x160 | | SkeletalMesh.ClothToGraphicsVertMap (ArrayNum, TArray at +0x15c) | int | 0 (empty) | FUN_00d4ba70 uses +0x160 as the particle count (adds +0x204 when tearable); FUN_00d47240 loop bound | [V] offset, [A] name |
| +0x16c | | SkeletalMesh.ClothWeldingMap (ArrayNum, TArray at +0x168) | int | 0 | FUN_00d4ba70 and FUN_00dab750 gate the primary path and TEARABLE on +0x16c == 0 | [V] offset, [A] name |
| +0x174 | | SkeletalMesh.ClothWeldingDomain | int | 0 | FUN_00d4ba70 uses +0x174 as the vertex count on the alternate (welded) path | [V] offset, [A] name |
| +0x178 | | SkeletalMesh.ClothWeldedIndices (data ptr; num at +0x17c) | TArray<int> | empty | FUN_00d4ba70 reads the index pointer +0x178 and count +0x17c on the alternate path | [V] |
| +0x184 | bit0 | SkeletalMesh.bForceNoWelding | bool | False | not read in the functions checked | [A] |
| +0x188 | | SkeletalMesh.NumFreeClothVerts | int | 0 | FUN_00d47240 and FUN_00d496f0 start the fixed-vertex loop at +0x188 and stop at +0x160 (free verts first) | [V] usage, [A] name |
| +0x18c | | SkeletalMesh.ClothIndexBuffer (data ptr; num at +0x190) | TArray<int> | empty | FUN_00d4ba70 copies triangle indices from +0x18c, count +0x190 | [V] |
| +0x1a4 | bit0 (0x1) | SkeletalMesh.bEnableClothBendConstraints | bool | False | FUN_00d492d0: bit0 sets NX bending (0x40) unless the ortho bit is set (0xc0) | [V] logic, [A] name |
| +0x1a4 | bit1 (0x2) | SkeletalMesh.bEnableClothDamping | bool | False | FUN_00d492d0: bit1 sets NX damping (0x100) | [V] logic, [A] name |
| +0x1a4 | bit2 (0x4) | SkeletalMesh.bUseClothCOMDamping | bool | False | FUN_00d492d0: bit2 sets NX COM damping (0x4000) | [V] logic, [A] name |
| +0x1a8 | | SkeletalMesh.ClothStretchStiffness | float | 1.0 | FUN_00d492d0 reads it into the stretching setter | [V] |
| +0x1ac | | SkeletalMesh.ClothBendStiffness | float | 1.0 | FUN_00d492d0 reads it into the bending setter | [V] |
| +0x1b0 | | SkeletalMesh.ClothDensity | float | 1.0 | FUN_00d4ba70 reads it (passed on at creation, not sent by FUN_00d492d0) | [V] usage, [A] name |
| +0x1b4 | | SkeletalMesh.ClothThickness | float | 0.5 | FUN_00d492d0 sends +0x1b4 x 0.02 to the thickness setter (vtable +0x44) | [V] |
| +0x1b8 | | SkeletalMesh.ClothDamping | float | 0.5 | FUN_00d492d0 to vtable +0x1c | [V] |
| +0x1bc | | SkeletalMesh.ClothIterations | int | 5 | FUN_00d492d0 to vtable +0x58 (solver iterations) | [V] |
| +0x1c0 | | SkeletalMesh.ClothFriction | float | 0.5 | FUN_00d492d0 to vtable +0x24 | [V] |
| +0x1c4 | | SkeletalMesh.ClothRelativeGridSpacing | float | 1.0 | layout only; not read by the functions checked | [A] |
| +0x1c8 | | SkeletalMesh.ClothPressure | float | 1.0 | FUN_00d492d0 reads it | [V] |
| +0x1cc | | SkeletalMesh.ClothCollisionResponseCoefficient | float | 0.2 | FUN_00d492d0 reads it | [V] |
| +0x1d0 | | SkeletalMesh.ClothAttachmentResponseCoefficient | float | 0.2 | FUN_00d492d0 reads it | [V] |
| +0x1d4 | | SkeletalMesh.ClothAttachmentTearFactor | float | 1.5 | FUN_00d492d0 reads it (the docs say it is used when comp+0x3f0 is < 0) | [V] |
| +0x1d8 | | SkeletalMesh.ClothSleepLinearVelocity | float | -1.0 | FUN_00d492d0 to vtable +0x130 | [V] |
| +0x1dc | bit0 (0x1) | SkeletalMesh.bEnableClothOrthoBendConstraints | bool | False | FUN_00d492d0: bit0 sets NX ortho bending (0x80, with 0x40) | [V] logic, [A] name |
| +0x1dc | bit1 (0x2) | SkeletalMesh.bEnableClothSelfCollision | bool | False | FUN_00d492d0: bit1 sets NX self-collision (0x8) | [V] logic, [A] name |
| +0x1dc | bit2 (0x4) | SkeletalMesh.bEnableClothPressure | bool | False | FUN_00d492d0: bit2 sets NX pressure (0x1); pressure value is +0x1c8 | [V] logic, [A] name |
| +0x1dc | bit3 (0x8) | SkeletalMesh.bEnableClothTwoWayCollision | bool | False | FUN_00d492d0: bit3 sets NX two-way collision (0x200) | [V] logic, [A] name |
| +0x1e0 | | SkeletalMesh.ClothSpecialBones (data ptr; num at +0x1e4) | TArray<ClothSpecialBoneInfo> | empty | FUN_00d48db0 walks +0x1e0 for +0x1e4 entries (entry: name @0, type byte @8, vertex indices @0xc) | [V] layout and struct, [A] name |
| +0x1ec | bit0 (0x1) | SkeletalMesh.bEnableClothLineChecks | bool | False | not read in the functions checked | [A] |
| +0x1ec | bit1 (0x2) | SkeletalMesh.bClothMetal | bool | False | FUN_00d4b930 gates the metal path on bit1 (with comp+0x324 bit0, not frozen, comp+0x3d0 > 0); the create docs also read bit1 as the alternative global pose | [V] offset/bit, [A] name |
| +0x1f0 | | SkeletalMesh.ClothMetalImpulseThreshold | float | 10.0 | FUN_00d49090 passes +0x1f0 to attachToCore | [V] usage, [A] name |
| +0x1f4 | | SkeletalMesh.ClothMetalPenetrationDepth | float | 0.0 | FUN_00d49090 passes +0x1f4 to attachToCore | [V] usage, [A] name |
| +0x1f8 | | SkeletalMesh.ClothMetalMaxDeformationDistance | float | 0.0 | FUN_00d49090 passes +0x1f8 to attachToCore | [V] usage, [A] name |
| +0x1fc | bit0 (0x1) | SkeletalMesh.bEnableClothTearing | bool | False | FUN_00d492d0, FUN_00d4ba70 and FUN_00dab750 test bit0 (TEARABLE 0x1000 only when +0x16c == 0) | [V] logic, [A] name |
| +0x200 | | SkeletalMesh.ClothTearFactor | float | 3.5 | FUN_00d492d0 to vtable +0x34 (tear factor) | [V] |
| +0x204 | | SkeletalMesh.ClothTearReserve | int | 128 | FUN_00d4ba70 adds it to the vertex count when tearable and +0x16c == 0 | [V] usage, [A] name |

Notes on the mesh rows:
- ClothSpecialBones is the attachment table. Its element struct is `SkeletalMesh.ClothSpecialBoneInfo` (size 24).
- +0x1c4 (ClothRelativeGridSpacing) is the only cloth-data name in the requested range that the checked functions do not read.
- Any SkeletalMesh property at +0x208 or later (ClothTornTriMap, GraphicsIndexIsCloth) is outside the requested range and is not mapped.

### USkeletalMeshComponent (comp)

| offset | bits | UE3 class.property | type | default | evidence | tag |
|---|---|---|---|---|---|---|
| +0x1c8 | | USkeletalMeshComponent.SkeletalMesh | USkeletalMesh* | none | the cloth data base; FUN_00d4ba70 and natives gate on it (+0x1c8 != 0) | [V] |
| +0x238 | | USkeletalMeshComponent.MeshObject | Pointer (native, 4 bytes) | 0 | FUN_00d47240 skips the pinning when +0x238 == 0 | [V] usage, [A] name |
| +0x2c0 | | USkeletalMeshComponent.PredictedLODLevel | int | 0 | FUN_00d47240 skips the pinning when +0x2c0 >= 1 | [V] usage, [A] name |
| +0x314 | 0x200 | USkeletalMeshComponent.bHasPhysicsAssetInstance | bool | False | FUN_00d49090 requires (+0x314 & 0x200) for attach-to-core | [V] bit, [A] name |
| +0x314 | 0x1000 | USkeletalMeshComponent.bSkelCompFixed | bool | False | create/release path (vt_00d4d160) tests bit 12 per the create docs (not re-checked here) | [V] bit (from docs), [A] name |
| +0x324 | bit0 (0x1) | USkeletalMeshComponent.bEnableClothSimulation | bool | False | FUN_00d4af70 pins every frame when set; execSetEnableClothSimulation / FUN_00d18f20 creates or releases | [V] |
| +0x324 | bit1 (0x2) | USkeletalMeshComponent.bDisableClothCollision | bool | False | UpdateClothParams (FUN_00d492d0) maps it to NX_CLF_DISABLE_COLLISION (0x4) | [V] logic, [A] name |
| +0x324 | bit2 (0x4) | USkeletalMeshComponent.bClothFrozen | bool | False | FUN_00d43700 writes this bit and sets NX_CLF_STATIC (0x2) | [V] logic, [A] name |
| +0x324 | bit3 (0x8) | USkeletalMeshComponent.bAutoFreezeClothWhenNotRendered | bool | False | layout only | [A] |
| +0x324 | bit4 (0x10) | USkeletalMeshComponent.bClothBaseVelClamp | bool | False | layout only | [A] |
| +0x324 | bit5 (0x20) | USkeletalMeshComponent.bAttachClothVertsToBaseBody | bool | False | FUN_00d496f0 sets/clears it; FUN_00d47240 skips the pinning when set | [V] logic, [A] name |
| +0x324 | bit6 (0x40) | USkeletalMeshComponent.bCacheAnimSequenceNodes | bool | True | layout only (outside the requested range) | [A] |
| +0x324 | bit7 (0x80) | USkeletalMeshComponent.bForceMeshObjectUpdates | bool | False | layout only (outside the requested range) | [A] |
| +0x328 | | USkeletalMeshComponent.ClothExternalForce | vector (12) | 0 | FUN_00d436b0 stores the vector at +0x328..+0x330 and pushes it to the cloth | [V] |
| +0x334 | | USkeletalMeshComponent.ClothWind | vector (12) | 0 | layout only | [A] |
| +0x340 | | USkeletalMeshComponent.ClothBaseVelClampRange | vector (12) | 0 | layout only | [A] |
| +0x34c | | USkeletalMeshComponent.ClothBlendWeight | float | 1.0 | FUN_00d4ba70 copies +0x34c to +0x350 at creation | [V] |
| +0x350 | | USkeletalMeshComponent.ClothDynamicBlendWeight | float | 0 | written from +0x34c at creation | [V] |
| +0x354 | | USkeletalMeshComponent.ClothMinBlendDistance | float | 0 | layout only | [A] |
| +0x358 | | USkeletalMeshComponent.ClothMaxBlendDistance | float | -1.0 | layout only | [A] |
| +0x35c | | USkeletalMeshComponent.ClothSim | Pointer (native, NxdCloth*) | 0 | the cloth object; created by FUN_00d4ba70, released by FUN_00d4c5d0; all natives read it | [V] |
| +0x360 | | USkeletalMeshComponent.SceneIndex | int | 0 | FUN_00d4ba70 writes the owner key here (docs) | [V] offset, [A] name |
| +0x364 | | USkeletalMeshComponent.ClothMeshPosData | TArray (12) | empty | output buffer (FUN_00d4ba70, FUN_00da79e0) | [V] |
| +0x370 | | USkeletalMeshComponent.ClothMeshNormalData | TArray (12) | empty | output buffer | [V] |
| +0x37c | | USkeletalMeshComponent.ClothMeshIndexData | TArray (12) | empty | output buffer (copied from the mesh's ClothIndexBuffer) | [V] |
| +0x388 | | USkeletalMeshComponent.NumClothMeshVerts | int | 0 | FUN_00d4ba70 writes the particle count here after creation | [V] |
| +0x38c | | USkeletalMeshComponent.NumClothMeshIndices | int | 0 | FUN_00d4ba70 writes it here | [V] |
| +0x390 | | USkeletalMeshComponent.ClothMeshParentData | TArray (12) | empty | output buffer | [V] |
| +0x39c | | USkeletalMeshComponent.NumClothMeshParentIndices | int | 0 | output count (FUN_00d4ba70) | [V] |
| +0x3a0 | | USkeletalMeshComponent.ClothMeshWeldedPosData | TArray (12) | empty | output buffer | [V] |
| +0x3ac | | USkeletalMeshComponent.ClothMeshWeldedNormalData | TArray (12) | empty | output buffer | [V] |
| +0x3b8 | | USkeletalMeshComponent.ClothMeshWeldedIndexData | TArray (12) | empty | output buffer | [V] |
| +0x3c4 | | USkeletalMeshComponent.ClothDirtyBufferFlag | int | 0 | FUN_00d4ba70 passes it as an output pointer | [V] usage, [A] name |
| +0x3c8 | | USkeletalMeshComponent.ClothRBChannel | byte (RBCollisionChannel enum) | RBCC_Cloth | FUN_00f8c6b0 builds the group mask from this byte (bit = 1 << (byte & 31)) | [V] |
| +0x3cc | | USkeletalMeshComponent.ClothRBCollideWithChannels | RBCollisionChannelContainer (struct, 4) | 0 | FUN_00f8c6b0 takes it as the second mask dword | [V] |
| +0x3d0 | | USkeletalMeshComponent.ClothForceScale | float | 0 | FUN_00d4b930 applies it in the metal path when > 0 | [V] usage, [A] name |
| +0x3d4 | bit0 (0x1) | USkeletalMeshComponent.bEnableValidBounds | bool | False | the native execEnableClothValidBounds sets the NX validbounds flag (0x8000) and stores no bit | [A] |
| +0x3d8 | | USkeletalMeshComponent.ValidBoundsMin | vector (12) | (-500, -500, -500) | FUN_00d4b6f0 reads the vector at +0x3d8 (execSetClothValidBounds) | [V] usage, [A] name |
| +0x3e4 | | USkeletalMeshComponent.ValidBoundsMax | vector (12) | (500, 500, 500) | layout only (second vector of the same native) | [A] |
| +0x3f0 | | USkeletalMeshComponent.ClothAttachmentTearFactor | float | -1.0 | FUN_00d492d0 reads it next to the mesh's +0x1d4 | [V] usage, [A] name |
| +0x3f4 | | USkeletalMeshComponent.LimitMaterial | Object | none | layout only | [A] |

### Corrections to the earlier audit docs

- The runtime doc (`exe-cloth-runtime.md`, native table) says `execSetClothFrozen` "also sets component +0x324 bit 4".
  The decompile of FUN_00d43700 writes mask 0x4, which is bit 2 (bClothFrozen). The doc's "bit 4" looks like a
  mis-stated mask (value 4 written as "bit 4"). [V]
- GAPS row 38 and the create doc give comp+0x1f0..0x1f8 as attach-to-core values. Their names are now the
  ClothMetalImpulseThreshold / ClothMetalPenetrationDepth / ClothMetalMaxDeformationDistance triple. [A on the names]
- The create doc's "comp+0x3f0 when >= 0" is the component's own ClothAttachmentTearFactor. Its default is -1.0, so the
  mesh value is used until a component override is set. [V default, A usage]

### Summary

- Every requested cloth-data and component offset now has a property name. The best-verified set is the
  bool bits (+0x1a4 bits 0..2, +0x1dc bits 0..3, +0x324 bits 0..2 and 5, +0x314 bits 0x200 and 0x1000), the float and int
  parameters +0x1a8 to +0x1d8, +0x200, +0x204, and the mesh arrays +0x160/+0x16c/+0x178/+0x18c/+0x1e0.
- Two name guesses from the request are confirmed by the package: ClothMetal* are the +0x1f0..+0x1f8 triple, and
  ClothRBChannel / ClothRBCollideWithChannels are at comp+0x3c8 / +0x3cc. Also confirmed: comp+0x3f0 is
  ClothAttachmentTearFactor (component default -1.0), and comp+0x1c8 is the SkeletalMesh object pointer.
- The +0x1c8 "cloth data" is the USkeletalMesh object, as the request guessed. Its cloth properties start at +0xa0.

### Open questions

1. bClothMetal at +0x1ec bit1 is the only bit-level name with a [A] and two uses (the metal path in FUN_00d4b930 and the
   global-pose alternative in the create docs). Confirm the name in the content (the metal-cloth meshes) or the
   behaviour under the other bit's gate.
2. bEnableValidBounds (comp+0x3d4 bit0) is not read anywhere in the exe. Its value only feeds the NX flag by the
   native's argument. Check whether the component ever writes or reads +0x3d4.
3. The ClothWeldingDomain / ClothWeldedIndices pair at +0x174..+0x17c is the alternate (welded) mesh path. Its names are
   [A]. The real counts need a welded mesh in the content.
4. Per-mesh values in the content (for example the tuned ClothTearFactor and ClothMetalImpulseThreshold on each mesh)
   can differ from the class defaults above. A content scan of the SkeletalMesh exports in TdGame would confirm the
   values used in practice.
5. The mesh offsets are anchored, not built up from the Bounds / NameIndexMap / LODModels sizes. A check of those three
   sizes (or of UObject's size) would make the base exact instead of anchored.
