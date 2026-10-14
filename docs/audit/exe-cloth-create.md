# MirrorsEdge.exe: cloth creation (audit B1)

Static read of the Ghidra exports in `re/out/MirrorsEdge/` (no Ghidra GUI, game not run). Addresses are exe VAs.
[V] = seen in the decompile or in the 2.8.1 headers (function and offset given). [A] = inferred. Offsets in the
"cloth data" are relative to the cloth-data block at component+0x1c8; "comp" is the owning object that calls
`FUN_00d4ba70` (a USkeletalMeshComponent-like object, class not confirmed). Scale: the game multiplies by 0.02 (UE units
to metres) in several places; that factor is [V].

## How it was found

- NxdScene::createCloth is slot 131 (0x20c), per the 2.8.1 header order (same index used by the shim, cloth_wrap.cpp:59).
  NxdScene slot 131 is `0x0145e700` [V rtti_vtables.txt]. Its body calls the real scene's slot 0x20c with the desc, wraps the
  result in an NxdCloth (constructor `0x0146d0a0`, vtable `0x1d71a6c` [V]) and appends it to the scene's cloth list. It refuses
  while the scene simulates (error text "Attempt to create cloth during simulation", NxdScene.cpp line 0x4ff).
- The only caller that builds the desc on the stack and then calls the scene's slot 0x20c, and then the cloth setup, is
  `FUN_00d4ba70` (2,894 bytes). Its cloth-mesh side is `FUN_00dab750`, the only site that calls NxCookingInterface slot 0x1c
  (NxCookClothMesh, header order index 7) and SDK slot 0x54 (createClothMesh, index 21) [V: grep of DAT_020009d4 and
  DAT_020009cc].
- Desc field offsets were checked against `third_party/physx-sdk-2.8.1/.../cloth/NxClothDesc.h`: the stack desc is 280 bytes,
  the same layout as shim/cloth_ours.cpp:74-80 [V].
- Property names: the strings file has the native names only (`intUSkeletalMeshComponentexecSetCloth*`, natives.tsv), and no
  `ClothThickness`-style property strings. The UE3 property names below therefore come from the native names and reasoning,
  and are marked [A]. The packages need the content scan to confirm them.

## Main functions

| function | address | role |
|---|---|---|
| FUN_00d4ba70 | 0x00d4ba70 | InitClothSim-like: builds NxClothDesc, createCloth, then the setup calls |
| FUN_00dab750 | 0x00dab750 | NxClothMesh per scale: cook (NxCookClothMesh) + createClothMesh, cached by a float key |
| FUN_00da79e0 | 0x00da79e0 | cloth vertex positions from the skeletal-mesh LOD (x scale x 0.02) |
| FUN_00d492d0 | 0x00d492d0 | parameter sync from the cloth data (after create; also native execUpdateClothParams 0x00d49620) |
| FUN_00d436b0 | 0x00d436b0 | external acceleration from comp+0x328 (also native execSetClothExternalForce 0x00d49650) |
| FUN_00d43700 | 0x00d43700 | NX_CLF_STATIC from comp+0x324 bit 2 (native execSetClothFrozen 0x00d49eb0) |
| FUN_00d47240 | 0x00d47240 | pins the fixed vertices to global positions (per frame via FUN_00d4af70 0x00d4af70) |
| FUN_00d48db0 | 0x00d48db0 | vertex-to-shape attachments (attachVertexToShape) |
| FUN_00d496f0 | 0x00d496f0 | base-body attach/free of the fixed vertices (native execSetAttachClothVertsToBaseBody 0x00d49e40) |
| FUN_00d18f20 | 0x00d18f20 | runtime enable: creates via FUN_00d4ba70 or releases via FUN_00d4c5d0 (native execSetEnableClothSimulation 0x00d19f40) |
| FUN_00d4c5d0 | 0x00d4c5d0 | release: NxScene slot 0x210 (releaseCloth) |
| FUN_00d48950 | 0x00d48950 | native execAttachClothToCollidingShapes: NxCloth slot 25 with flags |
| FUN_00f8c400 | 0x00f8c400 | copies the component 4x4 into the NxMat34 layout, translation x 0.02 [V] |
| FUN_00f8c510 | 0x00f8c510 | 3-vector x 0.02 [V] |
| FUN_00f8c6b0 | 0x00f8c6b0 | builds NxGroupsMask from a byte channel and a dword [V] |
| FUN_00f8c2b0 | 0x00f8c2b0 | hardware gate: SDK slot 31 (getHWVersion) != NX_HW_VERSION_NONE and a global set [V] |

## Desc fields (NxClothDesc, 280 bytes; offsets from the start of the desc)

Set in `FUN_00d4ba70` unless noted. "default" = the value the desc gets from the inlined setToDefault (header values).

| PhysX API item | set/called? [V/A] | address | UE3 source property/function | notes |
|---|---|---|---|---|
| NxClothDesc.clothMesh (+0) | set [V] | FUN_00d4ba70 -> FUN_00dab750 | USkeletalMesh cloth mesh, cooked per scale | NxClothMesh pointer from FUN_00dab750 (cache key = scale float) |
| NxClothDesc.globalPose rotation (+4..+39) | set [V] | FUN_00d4ba70, FUN_00f8c400 | component LocalToWorld (comp+0x70, 4x4 FMatrix, row-vector) [A on the name] | 3x3 written so that the 9 floats are the UE 3x3 transposed (row-major of A^T; equal to the column-major copy of A). The shim reads it row-major and computes M r + t (shim/cloth_ours.cpp:1714-1720), which is consistent. Rows are normalised first (scale removed) [V: FUN_004ae050 applied to the copy, [A] on its ECX] |
| NxClothDesc.globalPose translation (+40..+51) | set [V] | FUN_00f8c400 | component LocalToWorld translation x 0.02 | |
| globalPose alternative (when data+0x1ec bit 1 is set) | set [V] | FUN_00d4ba70 | cloth data +0x1ec bit 1 [A name] | identity rotation, translation = -(bounding-box centre of the particles) in metres; the mesh is then recentred |
| NxClothDesc.thickness (+52) | set [V] default 0.01 at create, then setThickness | FUN_00d492d0 | USkeletalMesh/component ClothThickness [A] (data+0x1b4) | setThickness (NxCloth slot 17) with data+0x1b4 x 0.02 [V] |
| NxClothDesc.density (+56) | set [V] | FUN_00d4ba70 | ClothDensity [A] (data+0x1b0) | no scale |
| NxClothDesc.bendingStiffness (+60) | default 1.0; setBendingStiffness (slot 3) [V] | FUN_00d492d0 | ClothBendStiffness [A] (data+0x1ac) | clamped to [0,1] |
| NxClothDesc.stretchingStiffness (+64) | default 1.0; setStretchingStiffness (slot 5) [V] | FUN_00d492d0 | ClothStretchStiffness [A] (data+0x1a8) | clamped to [0,1] |
| NxClothDesc.dampingCoefficient (+68) | default 0.5; setDampingCoefficient (slot 7) [V] | FUN_00d492d0 | ClothDamping [A] (data+0x1b8) | no clamp |
| NxClothDesc.friction (+72) | default 0.5; setFriction (slot 9) [V] | FUN_00d492d0 | ClothFriction [A] (data+0x1c0) | no clamp |
| NxClothDesc.pressure (+76) | default 1.0; setPressure (slot 11) [V] | FUN_00d492d0 | ClothPressure [A] (data+0x1c8) | max(0, x) |
| NxClothDesc.tearFactor (+80) | default 1.5; setTearFactor (slot 13) [V] | FUN_00d492d0 | ClothTearFactor [A] (data+0x200) | no clamp |
| NxClothDesc.collisionResponseCoefficient (+84) | default 0.2; setCollisionResponseCoefficient (slot 60) [V] | FUN_00d492d0 | ClothCollisionResponseCoefficient [A] (data+0x1cc) | max(0, x) |
| NxClothDesc.attachmentResponseCoefficient (+88) | default 0.2; setAttachmentResponseCoefficient (slot 62) [V] | FUN_00d492d0 | ClothAttachmentResponseCoefficient [A] (data+0x1d0) | max(0, x) |
| NxClothDesc.attachmentTearFactor (+92) | default 1.5; setAttachmentTearFactor (slot 15) [V] | FUN_00d492d0 | ClothAttachmentTearFactor [A] (data+0x1d4, or comp+0x3f0 when that is >= 0) | clamped >= 1 |
| NxClothDesc.toFluidResponseCoefficient (+96) | default 1.0 only [V] | FUN_00d4ba70 | none | never set again (no setter in the path) |
| NxClothDesc.fromFluidResponseCoefficient (+100) | default 1.0 only [V] | FUN_00d4ba70 | none | |
| NxClothDesc.minAdhereVelocity (+104) | default 1.0 only [V] | FUN_00d4ba70 | none | NX_CLF_ADHERE is never set in this path |
| NxClothDesc.solverIterations (+108) | default 5; setSolverIterations (slot 22) [V] | FUN_00d492d0 | ClothIterations [A] (data+0x1bc) | no clamp in the game (shim maps 0 to 1) |
| NxClothDesc.externalAcceleration (+112..+123) | 0 at create; setExternalAcceleration (slot 68) [V] | FUN_00d436b0 | external force vector comp+0x328 (set by execSetClothExternalForce) | applied as comp+0x328 x 0.02 |
| NxClothDesc.windAcceleration (+124..+135) | 0 [V] | FUN_00d4ba70 | none in this path | setWindAcceleration not called here |
| NxClothDesc.wakeUpCounter (+136) | NX_SLEEP_INTERVAL = 20 x 0.02 = 0.4 [V] | FUN_00d4ba70 | none | the cloth is then put to sleep, see below |
| NxClothDesc.sleepLinearVelocity (+140) | default -1.0; setSleepLinearVelocity (slot 76) [V] | FUN_00d492d0 | ClothSleepLinearVelocity [A] (data+0x1d8) | |
| NxClothDesc.meshData (+144..+211, 68 bytes) | set [V] | FUN_00d4ba70 | see the next rows | |
| meshData.verticesPosBegin (+144) | set [V] | FUN_00d4ba70 | none (runtime output buffer comp+0x364) | stride 12 at +152; positions out |
| meshData.verticesNormalBegin (+148) | set [V] | FUN_00d4ba70 | none (buffer comp+0x370) | normal stride 12 at +156 |
| meshData.maxVertices (+160) | set [V] | FUN_00d4ba70 | cloth data +0x160 vertex count (+0x204 extra when tearable, primary branch only) | |
| meshData.numVerticesPtr (+164) | set [V] | FUN_00d4ba70 | none (comp+0x388) | count written by the SDK |
| meshData.indicesBegin (+168) | set [V] | FUN_00d4ba70 | triangle indices copied from cloth data +0x18c (count +0x190) | buffer comp+0x37c; index stride 4 at +172 |
| meshData.maxIndices (+176) / numIndicesPtr (+180) | set [V] | FUN_00d4ba70 | cloth data +0x190 / comp+0x38c | |
| meshData.parentIndices (+184..+196) | set only when tearable [V] | FUN_00d4ba70 | none | buffers comp+0x390 and +0x39c; stride 4 at +188; max = +0x190 |
| meshData.dirtyBufferFlagsPtr (+200) | set [V] | FUN_00d4ba70 | none (comp+0x3c4) | dirty bits 1,2,4,8 (pos, normal, indices, parent) [V shim header] |
| meshData.flags (+204) and last word (+208) | set to 0 [V] | FUN_00d4ba70 | none | +208 is not named in the 2.8.1 NxMeshData header I read [A] |
| NxClothDesc.collisionGroup (+212, uint16) | set 0 [V] | FUN_00d4ba70 | none | |
| NxClothDesc.groupsMask (+216..+231) | set [V] | FUN_00d4ba70, FUN_00f8c6b0 | byte at comp+0x3c8 (channel) and dword at comp+0x3cc [A names] | bits0 = 1 << (byte & 31); bits1 = 0; bits2 = dword at comp+0x3cc; bits3 = 0 [V] |
| NxClothDesc.forceFieldMaterial (+232, uint16) | set 0 [V] | FUN_00d4ba70 | none | |
| NxClothDesc.validBounds (+236..+259) | set empty [V] | FUN_00d4ba70 | none | min = +FLT_MAX, max = -FLT_MAX. NX_CLF_VALIDBOUNDS is not set in this path |
| NxClothDesc.relativeGridSpacing (+260) | set [V] | FUN_00d4ba70 | cloth data +0x1c4 [A name] | no clamp (SDK isValid wants >= 0.01) |
| NxClothDesc.flags (+264) | set [V] | FUN_00d4ba70, FUN_00d492d0, FUN_00d43700 | see the flags table | creation value 0x20, plus 0x1000 and 0x2000 under conditions |
| NxClothDesc.userData (+268) | NULL [V] | FUN_00d4ba70 | none | |
| NxClothDesc.name (+272) | NULL [V] | FUN_00d4ba70 | none | |
| NxClothDesc.compartment (+276) | set only in the hardware path [V] | FUN_00d4ba70 -> FUN_00f8e1a0 | none | scene table entry (+0x10) [A]; otherwise NULL |

## NX_CLF_* flags

Creation value = NX_CLF_GRAVITY (0x20), then the conditions below. The post-create sync (FUN_00d492d0) reads getFlags (slot 80)
and writes setFlags (slot 79) once with the bits it sets; FUN_00d43700 then toggles STATIC with its own setFlags.

| PhysX API item | set/called? [V/A] | address | UE3 source property/function | notes |
|---|---|---|---|---|
| NX_CLF_GRAVITY (1<<5) | set [V] | FUN_00d4ba70 | none (constant) | never cleared in this path |
| NX_CLF_TEARABLE (1<<12) | set [V] | FUN_00d4ba70, FUN_00d492d0 | cloth data +0x1fc bit 0 and cloth data +0x16c == 0 [A] | the mesh flag 0x100 (NX_CLOTH_MESH_TEARABLE, desc mesh flags) is set on the same condition, in FUN_00dab750 [V] |
| NX_CLF_HARDWARE (1<<13) | set only if the hardware gate passes [V] | FUN_00d4ba70, FUN_00f8c2b0 | component bit comp+0x118 & 0x80 [A name] | the gate: a PhysX HW version reported by the SDK (slot 31) and a global set [V]. With no PPU the flag is never set [V logic] |
| NX_CLF_STATIC (1<<1) | set by frozen flag [V] | FUN_00d43700, FUN_00d4ba70 | comp+0x324 bit 2 (set by execSetClothFrozen 0x00d49eb0) [A name] | comp+0x324 bit 2 also gates the create-time call |
| NX_CLF_DISABLE_COLLISION (1<<2) | set/cleared [V] | FUN_00d492d0 | comp+0x324 bit 1 [A name] | |
| NX_CLF_PRESSURE (1<<0) | set/cleared [V] | FUN_00d492d0 | cloth data +0x1dc bit 2 [A] | |
| NX_CLF_SELFCOLLISION (1<<3) | set/cleared [V] | FUN_00d492d0 | cloth data +0x1dc bit 1 [A] | |
| NX_CLF_BENDING (1<<6) | set/cleared [V] | FUN_00d492d0 | cloth data +0x1a4 bit 0, or +0x1dc bit 0 [A] | |
| NX_CLF_BENDING_ORTHO (1<<7) | set/cleared [V] | FUN_00d492d0 | cloth data +0x1dc bit 0 [A] | both bending bits are set together |
| NX_CLF_DAMPING (1<<8) | set/cleared [V] | FUN_00d492d0 | cloth data +0x1a4 bit 1 [A] | |
| NX_CLF_COLLISION_TWOWAY (1<<9) | set/cleared [V] | FUN_00d492d0 | cloth data +0x1dc bit 3 [A] | |
| NX_CLF_COMDAMPING (1<<14) | set/cleared [V] | FUN_00d492d0 | cloth data +0x1a4 bit 2 [A] | |
| NX_CLF_TRIANGLE_COLLISION, VISUALIZATION, VALIDBOUNDS, FLUID_COLLISION, DISABLE_DYNAMIC_CCD, ADHERE | not set [V] | (none in the path) | none | not written by FUN_00d4ba70, FUN_00d492d0 or FUN_00d43700. Other natives (execSetClothFlags 0x00d483f0) can set them at run time [A] |

## Calls after createCloth (in order, FUN_00d4ba70)

| PhysX API item | set/called? [V/A] | address | UE3 source property/function | notes |
|---|---|---|---|---|
| NxScene::isWritable (slot 139, 0x22c) and checkResults(NX_RIGID_BODY_FINISHED, block) (slot 141, 0x234) | called [V] | FUN_00f8c2d0 | none | runs before createCloth and before each attach; waits for the simulation to finish |
| NxScene::createCloth (slot 131, 0x20c) via NxdScene 0x0145e700 | called [V] | FUN_00d4ba70 | none | result stored in comp+0x35c (NxdCloth*) |
| NxCloth::getNumberOfParticles (slot 53, 0xd4) | called [V] | FUN_00d4ba70 | none | comp+0x388 |
| comp+0x360 = owner key (param+0x70) | set [V] | FUN_00d4ba70 | none | used later to find the scene (FUN_00d4c5d0) |
| FUN_00d492d0 parameter sync | called [V] | FUN_00d492d0 | cloth data | see the desc table |
| setExternalAcceleration (slot 68) | called [V] | FUN_00d436b0 | comp+0x328 | |
| setFlags (79) with STATIC bit from FUN_00d43700 | called [V] | FUN_00d43700 | comp+0x324 bit 2 | |
| attachVertexToGlobalPosition (slot 28, 0x70) for vertices [cloth data+0x188, cloth data+0x160) | called [V] | FUN_00d47240 | fixed-vertex range from cloth data +0x188 [A] | skipped if comp+0x2c0 >= 1, comp+0x324 bit 0x20 set, or comp+0x238 == 0 [V]. Position: comp LocalToWorld applied to the skinned vertex (bone matrices at comp+0x240, 64 bytes each), x 0.02 [V] |
| getPositions (slot 48, 0xc0) with stride 12 | called [V] | FUN_00d48db0 | none | reads positions for the shape-local offsets |
| attachVertexToShape (slot 27, 0x6c) for each entry of the attachment table | called [V] | FUN_00d48db0 | attachment table at cloth data +0x1e0 (count +0x1e4) [A] | gated by comp+0x228 and comp+0x22c != 0. Each entry: two 32-bit names (FUN_00d19d90 resolves them to a body index, FUN_00bd8090 to a shape, FUN_00bd4fd0 gives the physics object [A]), a byte kind at +8 (only 0 or 1 used), an int array of vertex indices at +0xc and its count at +0x10 [V]. Flags: 0 for the first vertex of an entry, 2 (NX_CLOTH_ATTACHMENT_TEARABLE, 1<<1) for the others [V] |
| putToSleep (slot 78, 0x138) | called [V] | FUN_00d4ba70 | none | the cloth is created asleep; wakeUpCounter 0.4 [V] |
| attachToCollidingShapes (slot 25, 0x64) | called from the native only [V] | FUN_00d48950 | native execAttachClothToCollidingShapes [V] | flags 1 (TWOWAY) or 3 when the script bool is set [A on the flag mapping]. Not called in the creation path |
| attachToShape (24), attachToCore (34), detachFromShape (26), tearVertex (35) | not called in creation [V] | none | none | |
| freeVertex (slot 29, 0x74) and attachVertexToShape (27) to the base body | called from the native only [V] | FUN_00d496f0 | execSetAttachClothVertsToBaseBody [V] | on: free each fixed vertex, attach it to the base-body shape with flags 0, set comp+0x324 bit 0x20. Off: free each, clear the bit [V] |
| releaseCloth (NxScene slot 132, 0x210) | called [V] | FUN_00d4c5d0 | none | comp+0x35c cleared |

## Triggers and callers of the creation (FUN_00d4ba70)

| caller | address | how it is reached [V/A] | notes |
|---|---|---|---|
| execSetEnableClothSimulation -> FUN_00d18f20 | 0x00d19f40 -> 0x00d18f20 | native, UE3 script [V] | creates the cloth when enabled and none exists; releases when disabled; sets comp+0x324 bit 0 [V] |
| vt_00d4d160 | 0x00d4d160 | virtual method of the component (class not confirmed) [A] | creates or releases depending on a flag bit (comp+0x314 bit 12) and on the scene key [V logic] |
| FUN_00d4af70 (per frame) | 0x00d4af70 | virtual tick [A] | if comp+0x324 bit 0 is set, calls FUN_00d47240 every frame (pins the fixed vertices) [V] |
| FUN_004a6150, FUN_006a1820 | 0x004a6150, 0x006a1820 | editor code (AnimSetViewer, wx "ToggleCloth" checkbox) [V] | the shipped exe carries the editor code; these are not gameplay paths |
| FUN_00d24b60 | 0x00d24b60 | calls FUN_00d43700 (frozen toggle) [V] | not analysed further |

## Conditions and gates

| item | [V/A] | address | notes |
|---|---|---|---|
| Cloth data block present (comp+0x1c8 != 0), no cloth yet (comp+0x35c == 0), owner key non-null | [V] | FUN_00d4ba70 | |
| 3 scale components within 1e-4 of each other (test on 3 floats at ECX) | [V test] / [A operand] | FUN_007bc220 (eps 0x38d1b717) | the build is skipped when the test fails. Which 3 floats it reads is not clear from the decompile |
| Mesh from FUN_00dab750 non-null | [V] | FUN_00dab750 | |
| Primary vs alternate source: cloth data +0x16c == 0 selects the primary arrays (+0x160 count, +0x18c indices, +0x190 index count); otherwise the alternate ones (+0x174 count, +0x178 indices, +0x17c index count) | [V] | FUN_00d4ba70, FUN_00dab750 | the meaning of +0x16c is not identified [A] |
| Hardware: SDK HW version != NX_HW_VERSION_NONE (0, Nxp.h:2133) and a global set (DAT_01ffa8a8) | [V] | FUN_00f8c2b0 | `AGameInfo::execIsPhysXEnhanced` (0x00ae5340) is not read on this path [V: not referenced in the functions above] |
| Compartment: only when the HW gate passes and comp+0x118 & 0x80; value from scene table entry | [V] / [A] | FUN_00f8e1a0 | the table is DAT_020009e0 with 0x24-byte entries [A] |
| Scene lookup: key = owner param+0x70, table DAT_020009e0 entry +4 | [A] | FUN_00f8e0e0, FUN_00f8e0a0, FUN_00abe5f0 | the exact owner object (DAT_01ffbca4 +0x160) is not identified |
| Scene is not simulating (else error text, NxdScene.cpp:0x4ff) | [V] | NxdScene wrapper 0x0145e700 | |

## Mesh cook (FUN_00dab750, cache keyed by the scale float)

| item | [V/A] | notes |
|---|---|---|
| Cache: array of NxClothMesh* at comp+0x144, count +0x148, keys (floats) at +0x150; match within 1e-4 | [V] | |
| Vertex positions: every skeletal-mesh vertex of the LOD, in the order of the sections, x scale x 0.02 | [V] | count = sum of the two section counts per section (FUN_00da79e0) |
| NxClothMeshDesc.numVertices (+0), numTriangles (+4) = index count / 3 | [V] | |
| pointStrideBytes (+8) = 12, triangleStrideBytes (+12) = 12 | [V] | triangles are 3 x 32-bit indices |
| points (+16), triangles (+20) | [V] | triangles from cloth data +0x18c (primary) or +0x178 (alternate) |
| flags (+24) = 0x100 (NX_CLOTH_MESH_TEARABLE; same bit as shim/cloth_ours.cpp:93) | [V] | only when cloth data +0x1fc bit 0 and +0x16c == 0 |
| vertexMassStrideBytes (+28) = 4, vertexFlagStrideBytes (+32) = 4 | [V] | vertexMasses (+36) and vertexFlags (+40) = NULL |
| weldingDistance (+44) = 0 | [V] | welding is done by FUN_00da79e0 (its own logic), not by the SDK cook. [A] that the UE3 ClothWeldingMap / bForceNoWelding are applied there |
| NxCookingInterface::NxCookClothMesh (vtable slot 7 = 0x1c, header order) | [V slot by header, A for the 2.8.0 lib] | desc + stream |
| NxPhysicsSDK::createClothMesh (slot 21, 0x54) on DAT_020009cc | [V] | stream in, NxClothMesh* out |
| Post-cook: per-vertex flags table at comp+0x244 from the section index triples (vertex-to-flag map) | [A] | FUN_00dab530; purpose probably the graphics-to-cloth vertex map (ClothToGraphicsVertMap [A]) |

## Output buffers (meshData targets, comp+0x364 .. comp+0x3c4)

| buffer | offset | notes |
|---|---|---|
| positions | comp+0x364 (count comp+0x368) | written by the SDK each step; read for the bounding box in the alternate pose path |
| normals | comp+0x370 | |
| indices | comp+0x37c | copied from cloth data +0x18c |
| vertex count, index count | comp+0x388, comp+0x38c | |
| parent indices (tearable) | comp+0x390, comp+0x39c | |
| dirty flags | comp+0x3c4 | |
| external force | comp+0x328..+0x330 | UE units; x 0.02 when sent |
| component flag word | comp+0x324 | bit 0 cloth enabled, bit 1 disable collision, bit 2 static/frozen, bit 5 base-body mode [A names] |
| attach override | comp+0x3f0 | attachment tear factor override when >= 0 [A name] |

## Open questions

1. UE3 names. The exe has no property strings for the cloth data. The mapping of cloth data offsets (+0x1a4..+0x200,
   +0x1dc, +0x1fc, +0x1ec) to USkeletalMesh/Component properties needs the content scan (package property tables).
   Owner class of comp+0x1c8 and of DAT_01ffbca4 (+0x160) are not identified.
2. Which 3 floats FUN_007bc220 reads (ECX at the call), and which vector FUN_004ae050 normalises (the copy of comp+0x70 was
   assumed from the call pattern) [A].
3. Matrix layout: the game writes the rotation as the transpose of the UE row-vector 3x3, row-major (desc+4 holds column 0 of the
   UE matrix). The shim reads it the same way and works, so this is consistent with NxMat34 in the 2.8.x layout. The Foundation
   header does not state the storage default in the part read; confirm with the SDK source if it matters.
4. The 2.8.0 core's cooking and scene vtable order is assumed equal to the 2.8.1 header order (cooking slot 7 = NxCookClothMesh
   fits the 0x1c call with desc and stream). The 2.8.0 header is not on hand.
5. Vertex-to-bone mapping (FUN_00d471b0) and the attachment name lookups (FUN_00d19d90, FUN_00d2d970, FUN_00bd8090,
   FUN_00bd4fd0) were not read. The UE3 meaning of the attachment table (+0x1e0) is a guess.
6. meshData last word (+208) and the groupsMask source byte (comp+0x3c8) and dword (comp+0x3cc) need names.
7. Whether AGameInfo::IsPhysXEnhanced gates any cloth path outside the functions read here (not seen in this path).
8. Who calls the vtable slots that lead to vt_00d4d160 and FUN_00d4af70 (virtual, class not confirmed).

## Independent review (spot-checks)
- createCloth = NxdScene slot 131 (wrapper target 0x145e700 in rtti_vtables.txt), which matches the shim's slot 131 (cloth_wrap.cpp:4) [V].
- globalPose: FUN_00f8c400 copies the UE 4x4 into an NxMat34 with out[r][c] = in[c][r] for the 3x3 (a transpose), and the
  translation is in[3][0..2] * 0.02 (UU -> m). FUN_00d4ba70 calls it on comp+0x70 when cloth-data +0x1ec bit 1 is clear [V].
  NxMat34 stores M row-major, so the desc's row i is UE's column i. That is consistent with the shim's current (uncommitted)
  reading: row-major 3x3, R*r (shim-cloth.md "Pose matrix"). The committed HEAD's column-major reading was wrong.
