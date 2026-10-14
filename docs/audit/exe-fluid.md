# MirrorsEdge.exe: fluids (audit B3, static RE)

Scope: how the game builds an NxFluidDesc from UPhysXParticleSystem, which NxFluid / NxdFluid / NxFluidEmitter methods it
calls at runtime, and how the Nxd wrapper buffers writes. Source: text exports in `re/out/MirrorsEdge/` (see
`docs/audit/exe-setup.md`). No Ghidra GUI was used. Addresses are image VAs (base 0x400000).

Marks: [V] seen in the export code or header (address given), [A] inferred. Header numbers are the 2.8.1 headers under
`third_party/physx-sdk-2.8.1/`. Enum and constant values in the binary (NX_FORCE/ACCELERATION numbering, NX_DC_CPU =
0xffff0000) agree with those headers. The 2.8.0 slot order of NxFluid and NxScene is the header order (NxScene: the
index is the header index minus 2, which the shim's createFluid = 126 confirms), so the slot names below are [A] unless a
call site or the wrapper code confirms them.

## 1. Key addresses [V]

| role | address | note |
|---|---|---|
| anchor string "FPhysXParticleSystem %s failed to be created in PhysX Sdk!" | 0x1c895c8 | referenced only at 0xe87bb1, inside FUN_00e87b00 |
| UPS property handler (name dispatch, clamps) | FUN_00e83d20 | FName compares against the UProperty name, clamp; after each clamp it calls FUN_01101970 (role not identified) |
| UPS create (checks flag 0x20, allocates FPhys, logs failure) | FUN_00e87b00 | caller FUN_00f8f990 |
| FPhys constructor: allocates buffers, validates desc, calls createFluid | FUN_00e86f20 | desc is 0x160 bytes on the stack |
| NxFluidDesc fill (defaults first, then the UPS values) | FUN_00e86690 | |
| FPhys particle-set / actor setup (creates a static proxy actor, not the fluid) | FUN_00e855a0 | calls scene slot 7; stores the actor at UPS+0x8c |
| FPhys release (releaseFluid via NxdFluid getScene) | FUN_00e863b0 | |
| FPhys reset / destroy paths | FUN_00e86640 (removeAll), FUN_00e86e30 (destroy), FUN_00e86ea0, FUN_00e87a80 | |
| per-tick emitter update of one particle system | FUN_00e85a70 | pending deletes, pending accelerations, emitter spawn |
| emitter spawn that calls addParticles (sprite / mesh) | FUN_010af0f0 (sprite), FUN_010adea0 (mesh) | both call FUN_00e85d70 |
| addParticles wrapper (creation-id mapping) | FUN_00e85d70 | |
| pending delete flush (updateParticles, flags only) | FUN_00e83190 | |
| pending acceleration flush (updateParticles, NX_ACCELERATION) | FUN_00e82e30 | |
| bounds readers (getWorldBounds) | FUN_00e84a50 (from FUN_00f8eec0), FUN_00e84d60 (from FUN_010ae7f0) | |
| readback of deletion ids and particle copies | FUN_00e8fa30 | |
| fluid manager: create / destroy pending lists, then per-PS tick | FUN_00f8f990 | called from FUN_00abf650 (physics tick) and FUN_00ac1700 |
| physics tick: pending queue, then scene simulate (slot 140), fetchResults (slot 142) | FUN_00abf650 | the simulate call passes 0 |
| NxdScene::createFluid (slot 126) | FUN_0145e5d0 | wraps the real fluid (0x194-byte NxdFluid) |
| NxdScene::releaseFluid (slot 127) | vt_0145cf70 | |
| NxdScene::checkResults (slot 141) with buffered flush | vt_0145e960 -> FUN_0145d630 | |
| NxdScene::fetchResults (slot 142) | FUN_0145d310 | calls slot 141 first, then the real fetch |
| NxdFluid buffered-write flush (per fluid) | FUN_01471ab0; property bits FUN_014712d0; emitters FUN_0147b220 | |
| Nxd write guard ("Attempt to write to fluid during simulation in function %s") | FUN_014715e0 | string at 0x1d71c08 |
| NxdFluid updateParticles wrapper (force scaling, queueing) | FUN_01471cd0 (slot 17), FUN_01471650 (scale) | |
| UE3 "TotalFluidParticles" / "ActiveFluidParticles" string refs | 0xf8d36b / 0xf8d3ce | reader not identified, see open questions |

FPhysXParticleSystem (FPhys, pointer at UPS+0x90). Fields read or written in the constructor and the functions above [V]:

| FPhys offset | content | size per particle or count |
|---|---|---|
| +0x00 | NxFluid* (the Nxd wrapper returned by createFluid) | |
| +0x08 | particle write buffer, 32-byte records: pos at +0, vel at +0xc, id at +0x18 | MaxParticles x 32 |
| +0x0c | fluid id -> owner table, 8 bytes per id (filled in FUN_00e85d70) | MaxParticles x 8 |
| +0x10 | pending acceleration records {vec3, id at +0xc} (flushed by FUN_00e82e30) | MaxParticles x 16 |
| +0x14 | u16 index map for the pending records (memset 0xff) | MaxParticles x 2 |
| +0x18 | 12 bytes per particle, use not traced | MaxParticles x 12 |
| +0x1c | pending delete records {id, flag} stride 8 (flushed by FUN_00e83190) | MaxParticles x 8 |
| +0x20 | particle collision normal buffer | MaxParticles x 12 |
| +0x24 | particle creation id buffer | MaxParticles x 4 |
| +0x28 | particle deletion id buffer | MaxParticles x 4 |
| +0x2c | packet buffer (NxFluidPacket, 36 bytes) | packets x 36 |
| +0x30 | particle count cell (numParticlesPtr) | |
| +0x34 | creation count cell | |
| +0x38 | deletion count cell | |
| +0x3c | packet count cell | |
| +0x44 / +0x48 | pending acceleration count / pending delete count | |
| +0x5c | 16 bytes per packet, internal (not in the desc) | packets x 16 |
| +0x70 | UE3-side copy of positions (12 bytes), used by readback | |

packets = ROUND(SDK getParameter(78)) = NX_CONSTANT_FLUID_MAX_PACKETS = 78 (Nxp.h:1974) [V value, A that the call is getParameter].

## 2. Creation: UPS -> NxFluidDesc (FUN_00e86690, then FUN_00e86f20)

Order inside FUN_00e86690 [V]: a block of defaults is written first (the NxFluidDesc constructor values, for example
maxParticles 0x7fff, restDensity 1000, stiffness 20, viscosity 6), then the same fields are overwritten from the UPS. Desc
fields not written by the second block keep the defaults. The desc is memset to 0x160 bytes first.

Desc dword index = byte offset / 4 (NxFluidDescBase is 0x54 dwords = 336 bytes, the emitter NxArray follows at 0x54).

| PhysX API item | set/called? | address | UE3 source property (owner) | notes |
|---|---|---|---|---|
| NxFluidDesc.initialParticleData (dw 0x00-0x0f) | zero [V] | FUN_00e86690 memset | none | no initial particles |
| NxFluidDesc.maxParticles (0x10) | set [V] | FUN_00e86690 | UPhysXParticleSystem.MaxParticles (UPS+0x3c, int) | clamped 1..0x7fff in FUN_00e83d20; also sizes FPhys buffers |
| NxFluidDesc.numReserveParticles (0x11) | set [V] | FUN_00e86690 | UPhysXParticleSystem.ParticleReserve (UPS+0x88) | clamped 0..MaxParticles (FUN_0040a2e0 call in FUN_00e83d20); non-zero also sets PRIORITY_MODE |
| NxFluidDesc.restParticlesPerMeter (0x12) | set [V] | FUN_00e86690 | derived from RestParticleDistance (UPS+0x70) | value = 1 / (RestParticleDistance x 0.02) [V formula]; the 0.02 unit factor is [A] |
| NxFluidDesc.restDensity (0x13) | set [V] | FUN_00e86690 | UPhysXParticleSystem.RestDensity (UPS+0x74) | clamp >= 1e-4 |
| NxFluidDesc.kernelRadiusMultiplier (0x14) | set [V] | FUN_00e86690 | UPhysXParticleSystem.KernelRadiusMultiplier (UPS+0x78) | clamp >= 0.5 |
| NxFluidDesc.motionLimitMultiplier (0x15) | set [V] | FUN_00e86690 | MaxMotionDistance (UPS+0x58) / RestParticleDistance (UPS+0x70) | derived, division by RestParticleDistance |
| NxFluidDesc.collisionDistanceMultiplier (0x16) | set [V] | FUN_00e86690 | CollisionDistance (UPS+0x40) / RestParticleDistance (UPS+0x70) | derived |
| NxFluidDesc.packetSizeMultiplier (0x17) | set [V] | FUN_00e86690 | PacketSizeMultiplier (enum byte UPS+0x6d) | byte 0..5 -> 4, 8, 16, 32, 64, 128; default 16 |
| NxFluidDesc.stiffness (0x18) | set [V] | FUN_00e86690 | UPhysXParticleSystem.Stiffness (UPS+0x7c) | clamp >= 1e-4 |
| NxFluidDesc.viscosity (0x19) | set [V] | FUN_00e86690 | UPhysXParticleSystem.Viscosity (UPS+0x80) | clamp >= 1e-4 |
| NxFluidDesc.surfaceTension (0x1a) | constant 0 [V] | FUN_00e86690 | none | |
| NxFluidDesc.damping (0x1b) | set [V] | FUN_00e86690 | UPhysXParticleSystem.Damping (UPS+0x5c) | clamp >= 0 |
| NxFluidDesc.fadeInTime (0x1c) | constant 0 [V] | FUN_00e86690 | none | |
| NxFluidDesc.externalAcceleration (0x1d-0x1f) | set [V] | FUN_00e86690, vector copy via FUN_00f8c510 | vector at UPS+0x60 (name not in the exe strings) [A: ExternalAcceleration or similar] | |
| NxFluidDesc.projectionPlane (0x20-0x23) | constant (0, 0, 1, 0) [V] | FUN_00e86690 | none | NX_FF_PROJECT_TO_PLANE stays clear |
| NxFluidDesc.restitutionForStaticShapes (0x24) | set [V] | FUN_00e86690 | RestitutionWithStaticShapes (UPS+0x44) | clamp 0..1 |
| NxFluidDesc.dynamicFrictionForStaticShapes (0x25) | set [V] | FUN_00e86690 | FrictionWithStaticShapes (UPS+0x4c) | the UE3 name says "static"; the desc slot is the dynamic friction [V slot, A on intent]; clamp 0..1 |
| NxFluidDesc.staticFrictionForStaticShapes (0x26) | constant 0.05 [V] | FUN_00e86690 | none | not set from UE3 |
| NxFluidDesc.attractionForStaticShapes (0x27) | constant 0 [V] | FUN_00e86690 | none | |
| NxFluidDesc.restitutionForDynamicShapes (0x28) | set [V] | FUN_00e86690 | RestitutionWithDynamicShapes (UPS+0x48) | clamp 0..1 |
| NxFluidDesc.dynamicFrictionForDynamicShapes (0x29) | set [V] | FUN_00e86690 | FrictionWithDynamicShapes (UPS+0x50) | clamp 0..1 |
| NxFluidDesc.staticFrictionForDynamicShapes (0x2a) | constant 0.5 [V] | FUN_00e86690 | none | |
| NxFluidDesc.attractionForDynamicShapes (0x2b) | constant 0 [V] | FUN_00e86690 | none | |
| NxFluidDesc.collisionResponseCoefficient (0x2c) | set [V] | FUN_00e86690 | CollisionResponseCoefficient (UPS+0x84) | clamp >= 0 |
| NxFluidDesc.simulationMethod (0x2d) | set [V] | FUN_00e86690 | SimulationMethod (enum byte UPS+0x6c, not in exe strings) [A on name] | 0 -> NX_F_SPH (1), 1 -> NX_F_NO_PARTICLE_INTERACTION (2), 2 -> NX_F_MIXED_MODE (4); default 1 |
| NxFluidDesc.collisionMethod (0x2e) | set [V] | FUN_00e86690 | UPS flags byte +0x54 (bits 0x4 and 0x1) [A on UE3 bool names] | bit 0x4 -> NX_F_STATIC (1); bit 0x1 -> NX_F_DYNAMIC (2); default 3 |
| NxFluidDesc.collisionGroup (0x2f, u16) | 0 [V] | FUN_00e86690 | none | |
| NxFluidDesc.groupsMask (0x30-0x33) | set [V] (value A) | FUN_00e86690 via FUN_00ede9b0 and FUN_00f8c6b0 | UE3 collision channel container [A] | bits 0, 2, 5, 12 of a 32-bit channel word, passed through FUN_00f8c6b0 with argument 6 |
| NxFluidDesc.forceFieldMaterial (0x34, u16) | 0 [V] | FUN_00e86690 | none | |
| NxFluidDesc.particlesWriteData (0x35-0x44, NxParticleData) | set [V] | FUN_00e86690 (copy from a 16-dword local block) | none (FPhys buffers) | see 2a |
| NxFluidDesc.particleDeletionIdWriteData (0x45-0x48) | set [V] | FUN_00e86690 | none | count FPhys+0x38, buffer FPhys+0x28, stride 4, name 0 |
| NxFluidDesc.particleCreationIdWriteData (0x49-0x4c) | set [V] | FUN_00e86690 | none | count FPhys+0x34, buffer FPhys+0x24, stride 4, name 0 |
| NxFluidDesc.fluidPacketData (0x4d-0x4e) | set [V] | FUN_00e86690 | none | buffer FPhys+0x2c (36 bytes per packet), count cell FPhys+0x3c |
| NxFluidDesc.flags (0x4f) | set [V] | FUN_00e86690 | see 2b | |
| NxFluidDesc.userData (0x50) | 0 [V] | FUN_00e86690 | none | |
| NxFluidDesc.name (0x51) | 0 [V] | FUN_00e86690 | none | the game passes no debug name |
| NxFluidDesc.compartment (0x52) | set [V] | FUN_00e86f20 -> FUN_00f8e160, passed as FUN_00e86690 argument | none | non-null is required (FUN_00e86f20). Lookup table DAT_020009e0, key FPhys+0x70 = 0 [A] |
| NxFluidDesc.emitters (NxArray, 0x54-0x56) | empty [V] | FUN_00e86690 | none | no descriptor emitters. Emitters are not created by the game (section 3) |

Particle data buffers, as written into the desc [V]:
- particlesWriteData: numParticlesPtr = FPhys+0x30; bufferPos = FPhys+0x08 (stride 0x20); bufferVel = FPhys+0x08 + 0xc (stride 0x20); bufferId = FPhys+0x08 + 0x18 (stride 0x20); bufferCollisionNormal = FPhys+0x20 (stride 12); bufferLife, bufferDensity, bufferFlag = NULL; name = NULL (the 16th dword).
- The desc passed to createFluid is the whole 0x160-byte block (352 bytes, the NxFluidDesc with emitters), not the 336-byte base [V size, A that the core reads only the base].

### 2a. Validation before createFluid (FUN_00e86f20) [V structure, A mapping]
Before the create call the constructor requires: the scene pointer is non-null, the scene reports writable (scene virtual +0x22c, NxScene slot 139 by the shim's numbering, `isWritable` in the header, [A]), the compartment is non-null, and the desc passes a chain of checks. The chain requires: packet size a power of two above 3; several floats in [0, 1] (the restitution and friction values); a non-zero vector; the initial particle data empty or consistent. A failed check leaves the fluid pointer at zero and FUN_00e87b00 logs the anchor string. Creation failure sets UPS flag 0x20, so there is no retry [V].

### 2b. Flags word (desc 0x4f) [V]
| flag | value | source |
|---|---|---|
| NX_FF_VISUALIZATION | 0x1 | always set (base value 9 = bits 0 and 3) |
| NX_FF_DISABLE_GRAVITY | 0x2 | UPS flags byte +0x54 bit 0x2 |
| NX_FF_COLLISION_TWOWAY | 0x4 | UPS flags byte +0x54 bit 0x8 |
| NX_FF_ENABLED | 0x8 | always set |
| NX_FF_HARDWARE | 0x10 | set when the SDK reports hardware (FUN_00f8c2b0: SDK virtual +0x7c, [A] getHWVersion) AND the compartment's device code is not NX_DC_CPU (0xffff0000, NxCompartmentDesc.h:35) via compartment virtual +4 [A] |
| NX_FF_PRIORITY_MODE | 0x20 | set iff ParticleReserve (desc 0x11) != 0 |
| NX_FF_PROJECT_TO_PLANE | 0x40 | never |
| NX_FF_FORCE_STRICT_COOKING_FORMAT | 0x80 | never |

UPS flags byte +0x54 (as read by the code): 0x1 dynamic collision, 0x2 disable gravity, 0x4 static collision, 0x8 two-way collision,
0x10 pending state (set before destroy, cleared on create) [A], 0x20 create failed [V], 0x40 proxy actor state [A]. The UE3 names of the
bool bits are not in the exe strings (UE3 binds them from the package), so they need the package scan.

### 2c. Scene and object bookkeeping [V]
- Create: FUN_00e87b00 -> FUN_00e86f20 -> scene slot 126 = NxdScene::createFluid (FUN_0145e5d0), which calls the real createFluid
  (NxScene slot 126, [V] shim numbering) and wraps the result in a 0x194-byte NxdFluid (FUN_01471990), registered in the scene list
  (scene +0x940 / +0x944). The scene-level message "Attempt to create fluid during simulation" (0x1d6f8e8) is in this path.
- Release: FUN_00e863b0 -> NxdFluid slot 58 (getScene, returns the NxdScene wrapper) -> NxdScene slot 127 (vt_0145cf70), which checks
  writability, logs "Attempt to release fluid during simulation" (0x1d6f92c) if busy, and removes the entry from the scene list.
- Create and destroy are queued: FUN_00f8f990 runs the pending-create list (+0x20/+0x24, FUN_00e87b00 on each), then the
  pending-destroy list (+0x14/+0x18, FUN_00e86e30 on each), and then, for each entry of the +0x18 list, FUN_00e863a0, which runs the
  per-tick update FUN_00e85a70. FUN_00abf650 calls FUN_00f8f990 before scene simulate (slot 140) and fetchResults (slot 142) when
  its queue DAT_02000a68 is non-empty [V path; queue meaning A].

## 3. Runtime calls (game -> NxdFluid -> real fluid)

Frequency column: per tick = each physics step that enters FUN_00abf650's queue branch (before simulate); on event = create,
destroy, reset, component reset.

| PhysX API item | set/called? | address | UE3 source function (owner) | notes, frequency |
|---|---|---|---|---|
| NxScene::createFluid (scene slot 126) | called [V] | FUN_00e86f20 -> FUN_0145e5d0 | UPhysXParticleSystem create (FUN_00e87b00) | on event (activation), via the pending list |
| NxScene::releaseFluid (slot 127) | called [V] | FUN_00e863b0 -> vt_0145cf70 | UPS destroy (FUN_00e86e30, FUN_00e86df0) | on event |
| NxFluid::getScene (slot 58) | called [V] | FUN_00e863b0 | UPS release | on event |
| NxFluid::addParticles (slot 5, NxParticleData, appendIds=false) | called [V] | FUN_00e85d70 (called from FUN_010af0f0 sprite and FUN_010adea0 mesh spawn; both from FUN_00e85a70) | UPhysXParticleSystem emitter spawn | per tick, per emitter with spawn count > 0 [V path]. NxParticleData: numParticlesPtr = emitter+0x15c (count); bufferPos = UE3 particle record +0x10 (stride = emitter+0xc0); bufferVel = record +0x30 (same stride); bufferLife = array of 9999.0 (stride 4) when a type-data count is non-zero, else NULL [A on the condition]; all others NULL. Returns the number added (FPhys+0x34). Creation ids are mapped to the owner table at FPhys+0x0c (8 bytes per id) |
| NxFluid::removeAllParticles (slot 6) | called [V] | FUN_00e86640 (from FUN_00e86ea0 and FUN_00e87a80: UPS reset / destroy). FUN_00e89590 (slot 6 on the fluid list) from FUN_00b82210 (component reset) | UPS reset | on event; also before the destroy |
| NxFluid::updateParticles, NX_FORCE, flags only (slot 17) | called [V] | FUN_00e83190 | UE3 kill path (pending deletes at FPhys+0x48, records FPhys+0x1c) | per tick when FPhys+0x68 is set. bufferForce NULL, bufferFlag = record+4 (stride 8), bufferId = record+0 (stride 8). Only bit 0 (NX_FP_DELETE) is used |
| NxFluid::updateParticles, NX_ACCELERATION (5), force records (slot 17) | called [V] | FUN_00e82e30 | UE3 force / acceleration source (not traced) | per tick, same caller. Records are 16 bytes {vec3, id at +0xc}, stride 0x10 for force and id, no flag buffer. Zero-vector records are compacted out first |
| NxFluid::getWorldBounds (slot 72) | called [V] | FUN_00e84d60 (from FUN_010ae7f0), FUN_00e84a50 (from FUN_00f8eec0) | emitter / UPS bounds | per tick or per query [A] |
| NxFluid::getParticleDeletionIdWriteData (slot 10) | called [V] | FUN_00e8fa30 (readback; from FUN_00e90770, FUN_00b848a0) | UE3 readback of deleted ids | per frame readback [A on frequency] |
| NxFluid::getParticlesWriteData (slot 8) | not called [A] | none in the FPhys cluster | | the game reads its own buffers at FPhys+0x08 |
| NxFluid::setParticlesWriteData (slot 7) | not called [A] | (scene slot 7 is createActor in FUN_00e855a0) | | |
| NxFluid::setParticleDeletionIdWriteData (9), setParticleCreationIdWriteData (11), getParticleCreationIdWriteData (12) | not called [A] | | | |
| NxFluid::setFluidPacketData (18), getFluidPacketData (19) | not called [A] | | | packets are only in the desc |
| NxFluid::getNumReserveParticles (13), setNumReserveParticles (14) | not called [A] | | | reserve is set only through the desc |
| NxFluid::getCurrentParticleLimit (15), setCurrentParticleLimit (16) | not called [A] | | | |
| NxFluid property setters and getters, slots 20-57, 68-71, 76-77 | not called after create [A] | | | the only UE3 code using fluid vtables in the cluster is listed above |
| NxFluid::getMaxParticles (59), getKernelRadiusMultiplier (60), ..., getParticleMass (67), getRestParticlesPerMeter (64), getRestParticleDistance (66) | getParticleMass called from the wrapper only [V] (FUN_01471650) | | | |
| NxFluid::createEmitter (slot 1), releaseEmitter (slot 2), getNbEmitters (3), getEmitters (4) on a fluid | not called [A] | | | calls at vtable +4 in the cluster are GetClass or the compartment type call, not emitters |
| NxFluidEmitter (41 virtuals, 0x1d7320c / 0x1d732b4) | no game call found [A] | | | the game's emitters are UE3 emitter instances that feed addParticles |
| NxScene::isWritable (scene slot 139) | called [V] | wrappers, FUN_00e86f20 | | guard for create, release, and property writes |
| NxScene::getTiming (slot 83) | called by the wrapper [V] | FUN_01471650 | | returns the substep count used for force scaling |
| "TotalFluidParticles", "ActiveFluidParticles" stats | strings referenced [V] | 0xf8d36b, 0xf8d3ce | reader not identified | see open questions |

## 4. NxdFluid wrapper vtable 0x1d71ef4 (81 slots) [V]

The wrapper holds the real NxFluid pointer at +8 and the NxdScene at +0xc. Slot i calls real slot i. "F" = forwards straight
to the real object (no guard, no buffer). "G" = write guard: if the scene is busy (scene slot 139 returns false), log the error
"Attempt to write to fluid during simulation in function %s" (FUN_014715e0, file NxdFluid.cpp line 0x6a) and drop the call.
"B" = buffered: if the scene is idle, forward; if busy, store the value in the wrapper, set a bit in the word at +0x34, and add
the fluid to the scene's dirty list (FUN_0145dd60, list at scene +0x94c). Getters "B-read" return the stored value while the bit is set.
Flush: FUN_01471ab0 runs per dirty fluid. It sends queued force records first (see below), then applies each set bit by calling
the real setter with the stored value (FUN_014712d0), then clears the bit.

| slot | method (header order) | wrapper | class | bit (+0x34) / buffer | notes |
|---|---|---|---|---|---|
| 0 | ~NxFluid | FUN_014726d0 | W | | frees the wrapper (allocator) |
| 1 | createEmitter | FUN_01471fd0 | W | | real call, wraps the result in an NxdFluidEmitter (0x94 bytes) on the wrapper's list (+0x28/+0x2c) |
| 2 | releaseEmitter | FUN_01471c50 | W | | removes from the list, real release, deletes the wrapper |
| 3 | getNbEmitters | vt_01471cb0 | W | | returns wrapper count |
| 4 | getEmitters | vt_01471cc0 | W | | returns wrapper list |
| 5 | addParticles | vt_01470660 | F | | |
| 6 | removeAllParticles | vt_01470680 | F | | |
| 7 | setParticlesWriteData | vt_01471750 | G | | message "setParticlesWriteData" |
| 8 | getParticlesWriteData | vt_014706a0 | F | | |
| 9 | setParticleDeletionIdWriteData | vt_01471770 | G | | |
| 10 | getParticleDeletionIdWriteData | vt_014706c0 | F | | |
| 11 | setParticleCreationIdWriteData | vt_01471790 | G | | |
| 12 | getParticleCreationIdWriteData | vt_014706e0 | F | | |
| 13 | getNumReserveParticles | vt_01470720 | B-read | 0x800000 / +0x7c | |
| 14 | setNumReserveParticles | FUN_01470740 | B | 0x800000 / +0x7c | |
| 15 | getCurrentParticleLimit | vt_01470930 | B-read | 0x8000000 / +0x190 | |
| 16 | setCurrentParticleLimit | FUN_01470950 | B | 0x8000000 / +0x190 | |
| 17 | updateParticles | FUN_01471cd0 | G + B | queue (no bit) | busy with bufferId NULL: error "updateParticles, index based (NxParticleUpdateData::bufferId == NULL)". Busy: forces scaled by FUN_01471650 and appended to the queue (records {x,y,z scaled, flag, id}, stride 0x14). Idle: forwarded unchanged |
| 18 | setFluidPacketData | vt_014717b0 | G | | |
| 19 | getFluidPacketData | vt_01470700 | F | | |
| 20 | getSimulationMethod | vt_01470780 | B-read | 0x4000 / +0xec | |
| 21 | setSimulationMethod | FUN_014707a0 | B | 0x4000 / +0xec | |
| 22 | getStiffness | vt_014707e0 | B-read | 0x1 / +0x98 | |
| 23 | setStiffness | FUN_01470800 | B | 0x1 / +0x98 | |
| 24 | getViscosity | vt_01470850 | B-read | 0x2 / +0x9c | |
| 25 | setViscosity | FUN_01470870 | B | 0x2 / +0x9c | |
| 26 | getSurfaceTension | vt_014708c0 | B-read | 0x4000000 / +0xa0 | |
| 27 | setSurfaceTension | FUN_014708e0 | B | 0x4000000 / +0xa0 | |
| 28 | getDamping | vt_01470990 | B-read | 0x4 / +0xa4 | |
| 29 | setDamping | FUN_014709b0 | B | 0x4 / +0xa4 | |
| 30 | getFadeInTime | vt_01470a00 | B-read | 0x8 / +0xa8 | |
| 31 | setFadeInTime | FUN_01470a20 | B | 0x8 / +0xa8 | |
| 32 | getExternalAcceleration | vt_01470a70 | B-read | 0x10 / +0xac (vec3) | |
| 33 | setExternalAcceleration | FUN_01470ab0 | B | 0x10 / +0xac | |
| 34 | getProjectionPlane | vt_01470b10 | B-read | 0x2000000 / +0xb8 (4 floats) | |
| 35 | setProjectionPlane | FUN_014717d0 | B | 0x2000000 / +0xb8 | |
| 36 | getCollisionMethod | vt_01470b60 | B-read | 0x8000 / +0xf0 | |
| 37 | setCollisionMethod | FUN_01470b80 | B | 0x8000 / +0xf0 | |
| 38 | getRestitutionForStaticShapes | vt_01470bc0 | B-read | 0x20 / +0xc8 | |
| 39 | setRestitutionForStaticShapes | FUN_01470be0 | B | 0x20 / +0xc8 | |
| 40 | getDynamicFrictionForStaticShapes | vt_01470c30 | B-read | 0x40 / +0xcc | |
| 41 | setDynamicFrictionForStaticShapes | FUN_01470c50 | B | 0x40 / +0xcc | |
| 42 | getStaticFrictionForStaticShapes | vt_01470ca0 | B-read | 0x80 / +0xd0 | |
| 43 | setStaticFrictionForStaticShapes | FUN_01470cc0 | B | 0x80 / +0xd0 | |
| 44 | getAttractionForStaticShapes | vt_01470d10 | B-read | 0x100 / +0xd4 | |
| 45 | setAttractionForStaticShapes | FUN_01470d30 | B | 0x100 / +0xd4 | |
| 46 | getRestitutionForDynamicShapes | vt_01470d80 | B-read | 0x200 / +0xd8 | |
| 47 | setRestitutionForDynamicShapes | FUN_01470da0 | B | 0x200 / +0xd8 | |
| 48 | getDynamicFrictionForDynamicShapes | vt_01470df0 | B-read | 0x400 / +0xdc | |
| 49 | setDynamicFrictionForDynamicShapes | FUN_01470e10 | B | 0x400 / +0xdc | |
| 50 | getStaticFrictionForDynamicShapes | vt_01470e60 | B-read | 0x800 / +0xe0 | |
| 51 | setStaticFrictionForDynamicShapes | FUN_01470e80 | B | 0x800 / +0xe0 | |
| 52 | getAttractionForDynamicShapes | vt_01470ed0 | B-read | 0x1000 / +0xe4 | |
| 53 | setAttractionForDynamicShapes | FUN_01470ef0 | B | 0x1000 / +0xe4 | |
| 54 | setCollisionResponseCoefficient | FUN_01470f40 | B | 0x2000 / +0xe8 | |
| 55 | getCollisionResponseCoefficient | vt_01470f90 | B-read | 0x2000 / +0xe8 | |
| 56 | setFlag | FUN_01471130 | B | 0x400000 / masks +0x188 (set), +0x18c (clear) | |
| 57 | getFlag | vt_014711b0 | B-read | 0x400000 / +0x188 | returns only the pending set mask while buffered [V] |
| 58 | getScene | vt_01470650 | F (W) | | returns the NxdScene wrapper (+0xc) |
| 59 | getMaxParticles | vt_014711e0 | F | | |
| 60 | getKernelRadiusMultiplier | vt_014711f0 | F | | |
| 61 | getMotionLimitMultiplier | vt_01471200 | F | | |
| 62 | getCollisionDistanceMultiplier | vt_01471210 | F | | |
| 63 | getPacketSizeMultiplier | vt_01471220 | F | | |
| 64 | getRestParticlesPerMeter | vt_01471230 | F | | |
| 65 | getRestDensity | vt_01471240 | F | | |
| 66 | getRestParticleDistance | vt_01471250 | F | | |
| 67 | getParticleMass | vt_01471260 | F | | |
| 68 | setGroup | vt_01470fb0 / FUN_01470fb0 | B | 0x10000 / +0xf4 (u16) | |
| 69 | getGroup | vt_01471000 | B-read | 0x10000 / +0xf4 | |
| 70 | setGroupsMask | FUN_01471020 | B | 0x20000 / +0xf8 (4 dwords) | |
| 71 | getGroupsMask | vt_01471080 | B-read | 0x20000 / +0xf8 | |
| 72 | getWorldBounds | vt_01471280 | F | | |
| 73 | loadFromDesc | vt_01471860 | G | | message "loadFromDesc"; the real loadFromDesc is a stub in the shim |
| 74 | saveToDesc | vt_014712b0 | F | | |
| 75 | getPPUTime | vt_014712c0 | F | | |
| 76 | setName | vt_014710d0 / FUN_014710d0 | B | 0x1000000 / +0x17c | |
| 77 | getName | vt_01471110 | B-read | 0x1000000 / +0x17c | |
| 78 | getCompartment | vt_01471270 | F | | |
| 79 | getForceFieldMaterial | vt_01471290 | F | | returns 0 without a real fluid |
| 80 | setForceFieldMaterial | vt_01471830 | G | | message "setForceFieldMaterial" |

Wrapper flush of queued updateParticles records [V]: the queue is passed to the real fluid as NX_VELOCITY_CHANGE (forceMode 2) with
the flag and id buffers at +0xc and +0x10 of each 20-byte record. Scaling done in FUN_01471650 at queue time, per force mode:
NX_FORCE = F x T / m, NX_IMPULSE = F / m, NX_VELOCITY_CHANGE = F, NX_ACCELERATION = F x T, with m = real getParticleMass (slot 67)
and T from the scene timing (slot 83) and the substep count [A on the exact T]. Modes 3 and 4 fall into the NX_FORCE case.

### When buffered writes reach the real fluid [V]
1. Busy window: between the scene's simulate (slot 140) and the point where the scene is writable again. In this window a
   "B" setter stores its value and marks the fluid dirty; a "G" call logs an error and is dropped; a "F" call goes straight to the
   real fluid (no check).
2. Flush: NxdScene::checkResults (slot 141, vt_0145e960) calls the real checkResults and, when that returns true and the status
   argument has bit 0 set, runs FUN_0145d630. That walks the dirty fluids (FUN_01471ab0: queued forces, then the emitters, then
   the property bits) and three other dirty lists at scene +0x2c, +0x968 and +0x980 (not identified). The scene's own flag at +0x14
   is cleared after it. The bit test uses an argument Ghidra could not recover [A].
3. NxdScene::fetchResults (slot 142, FUN_0145d310) calls slot 141 first (so the flush runs there as well), then the real fetchResults,
   then FUN_0145c520 post-fetch work.
Idle writes (scene writable) are forwarded at once and never buffered.

## 5. Other UE3 facts [V unless marked]
- Show flags FLUID_PACKETS, FLUID_POSITION, FLUID_MESH_PACKETS, FLUID_PACKET_DATA, FLUID_BOUNDS, FLUID_VELOCITY, FLUID_KERNEL_RADIUS,
  FLUID_DRAINS, FLUID_EMITTERS are strings at 0x1bf5920-0x1bf5a28 (debug rendering; no API calls traced).
- FUN_00f8d100 (the function the stat string lookup landed in) builds a box from the bounds of an object at +0x35c with a
  scale of 50 [V constant]. It reads bounds through a virtual of that object, which is not identified. It is not the stat reader.
- UE3 classes: UPhysXParticleSystem (0x1c89598); particle module types UParticleModuleTypeDataPhysX (0x1ca86c0) and
  UParticleModuleTypeDataMeshPhysX (0x1ca8630); default content path strings "EngineResources.DefaultPhysXParSys" (0x1ca8678, 0x1ca8700).

## Open questions
1. Who fills the pending acceleration records (FPhys+0x10, count +0x44) and the pending delete records (FPhys+0x1c, count +0x48)?
   Those are the game's force and kill producers, not traced. The callers of FUN_00e85a70 and of FUN_010ae7f0 are the next step.
2. Exact NxParticleData for the sprite emitter (FUN_010af0f0): only the mesh spawn (FUN_010adea0) was read in full. The 9999.0 life
   and the stride fields come from the mesh path.
3. UE3 names of the bool bits (UPS+0x54), the enum bytes (+0x6c, +0x6d) and the vector at +0x60 are not in the exe. They need the
   package scan (`tools/ue3pkg.py`). The desc names in section 2 are the shim's (2.8.1) names, not the game's.
4. The stat reader for "TotalFluidParticles" / "ActiveFluidParticles" (refs at 0xf8d36b, 0xf8d3ce): the function that starts at
   0xf8d100 does not contain them in the export, so the boundary is probably wrong.
5. The compartment lookup (FUN_00f8e160, table DAT_020009e0) and the scene lookup (FUN_00f8e0e0) use key FPhys+0x70 = 0 at create
   time, so the fluid always goes to the first entry [A].
6. HARDWARE flag: the SDK virtual at +0x7c and the compartment virtual at +4 (compared to NX_DC_CPU) are [A] on their names.
7. The desc validation chain in FUN_00e86f20 (section 2a) is not mapped field by field; only its shape is recorded.
8. Frequency of addParticles per emitter and of the acceleration flush depends on FUN_00abf650's queue (DAT_02000a68), which is not
   identified.
9. The NxFluid 2.8.0 slot order is taken from the header order plus the shim numbering; the wrapper code confirms the index
   mapping (slot i -> real slot i) and the calls at slots 5, 6, 7, 10, 14, 17, 58, 72, 126, 127, but not every name.

## Independent review (spot-checks)
- Desc conversion FUN_00e86690: confirmed stores from the UPS object (comp+0x54) into desc dwords 0x10-0x19, 0x1b, 0x1d-0x1f,
  0x24, 0x25, 0x28, 0x29, 0x2c, and the packetSize switch on the UPS byte +0x6d (0->4, 1->8, ... power of two) [V].
  restParticlesPerMeter = 1/(UPS+0x70 * 0.02); motionLimit and collisionDistance multipliers are UPS+0x58 and UPS+0x40 divided by
  UPS+0x70 [V]. Dword 0x26 (staticFriction for static shapes) keeps the default 0.05 (0x3d4ccccd) [V].
- Dword 0x2c is collisionResponseCoefficient in the shim's layout (fluid_ours.cpp:77-79, dResponse). The game sets it from UPS+0x84.
  Our solver ignores that field (shim-fluid.md gap 6), so this is a confirmed "game sets, shim ignores" item.
