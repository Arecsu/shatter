# Audit A3: what the shim implements for fluids

Scope: `shim/fluid_ours.cpp`, `shim/fluid_wrap.cpp`, `shim/fluid_internal.h`, `shim/PhysXLoader.cpp`, `shim/shim.h`,
`solver/fluid/fluid.h`, `solver/fluid/fluid.cpp`, `solver/fluid/scenario.h`. Also read: `shim/scene_read.cpp` (drain, group,
timing and filter inputs come from it; it is not in the task's list). Reference: 2.8.1 headers under
`third_party/physx-sdk-2.8.1/SDKs/Physics/include/` (semantics only). Background: `docs/re-notes-fluid*.md`,
`docs/shim-game-run.md`, `docs/abi-2.8.0-vs-2.8.1.md`.

**Marks.** Every row is [V] (checked in the cited code or header) unless it says [A]. File:line refers to the
repo as of this audit.

**Slot numbers.** The shim's tables use 2.8.1 header declaration order. This audit re-counted the `virtual`
declarations: NxFluid 81, NxFluidEmitter 41, NxScene 157 (matches the counts in
`docs/abi/vtables-PhysXCore-v2.8.0.json` and `docs/abi-2.8.0-vs-2.8.1.md`). Slot numbers for NxScene 38, 93, 95, 126-130
match `docs/re-notes-fluid-mesh.md` 3.2 [V]; 94 (getFluidUserNotify) follows header order [A]. The 2.8.0 *order* of the NxFluid slots is not checked
against the binary here: the JSON only has counts, so it rests on `docs/re-notes-fluid-mesh.md` 3.1 [A]. The
game-run call pattern agrees with the table: slots 5, 6, 17, 58, 72 are the calls seen (`docs/shim-game-run.md`
18-29) [V in the logs].

**Status legend.** implemented: does what the header says, within the measured behaviour in `docs/`.
partial: part of the behaviour; the rest is in the "missing" column. stored only: the setter writes a
copy that the solver never reads; the getter returns the copy. stub: fixed return or no-op. pass-through:
forwarded to the core (wrapper path only). not hooked: the NxScene slot is not patched, so the core handles it
and cannot see hosted fluids.

## 0. What happens when a slot is not modelled

- **Hosted fluid (default, `PXSHIM_FLUID` unset).** The game holds a `ShimFluid` whose vtable `g_hostVt` has all 81
  entries (`shim/fluid_ours.cpp:490-506`, `static_assert`). A call never reaches a missing entry. A method the
  shim does not model returns 0 or does nothing (stub), or stores the value and does not apply it (stored only).
  Logged: `stored()` (`fluid_ours.cpp:308`), `setCurrentParticleLimit` (`:317`), `createEmitter` (`:258`),
  `loadFromDesc` (`:467`). Silent: getEmitters, getNbEmitters, getPPUTime, getters. Counted per slot: addParticles,
  removeAll, updateParticles (forces), getWorldBounds (`fluid_wrap.cpp:355-364`, `:366-445`; `fluid_ours.cpp:342`).
- **Wrapped fluid (`PXSHIM_FLUID=off`, or a non-collide-only fluid under `ours`).** All 81 slots are thunks that
  forward to the core's own vtable (`shim/fluid_wrap.cpp:197-296`). Five slots are typed (createEmitter `:302`,
  addParticles `:315`, removeAllParticles `:321`, updateParticles `:326`, getWorldBounds `:331`), and the
  setParticlesWriteData thunk copies the struct first (`:208`). Everything else is pure pass-through.
- **NxFluidEmitter (wrapped only).** 41 thunks forward to the core (`fluid_wrap.cpp:268-274`, patched in place at
  `:283-300`). Hosted fluids never own emitters (see section 1b).
- **NxScene slots not hooked.** Only createFluid 126, releaseFluid 127, simulate 140, fetchResults 142 are patched
  (`shim/PhysXLoader.cpp:177-178, 218-232`), plus cloth 131/132. Every other NxScene call goes to the core
  untouched. The core does not know hosted fluids (section 1c).
- **Slot index above 80 on NxFluid.** Would read past the table (UB). Not an NxFluid method; not seen in the game
  logs [V `docs/shim-game-run.md:18-31`].

## 1a. NxFluid (81 slots)

Hosted path unless noted. Count by status: implemented 26, partial 16, stored only 31, stub 8 (total 81).

| slot | method | status | missing / differs | file:line |
|---|---|---|---|---|
| 0 | ~NxFluid | stub | vtable destructor ignored, returns this | fluid_ours.cpp:253-256 |
| 1 | createEmitter | stub | returns 0 on hosted fluids; wrapped: pass-through and emitter hooks | fluid_ours.cpp:257-260; fluid_wrap.cpp:302-311 |
| 2 | releaseEmitter | stub | no-op | fluid_ours.cpp:261 |
| 3 | getNbEmitters | stub | always 0; emitters in the descriptor are dropped | fluid_ours.cpp:262 |
| 4 | getEmitters | stub | always null | fluid_ours.cpp:263 |
| 5 | addParticles | partial | 4096 cap per call, not per step (header NxFluid.h:148; [A] the core counts per step). Capacity is max - size; the reserve is ignored (fluid.cpp:913) [A vs core]. Write buffer appended without a bounds check (fluid_ours.cpp:154-171). Input id/flag/density/normal ignored (`:272-276`). Lowest-free ids and appendIds: implemented (`:141-150`, fluid.cpp:924) | fluid_ours.cpp:265-284 |
| 6 | removeAllParticles | implemented | removed at the end of the next step, bounds keep them for that step (fluid.cpp:1754-1755, 1783-1787). [A] particles added after this call and before the next step are removed too | fluid_ours.cpp:286-290 |
| 7 | setParticlesWriteData | implemented | struct copy; `name` ignored | fluid_ours.cpp:292 |
| 8 | getParticlesWriteData | implemented | returns the stored struct | fluid_ours.cpp:293-296 |
| 9 | setParticleDeletionIdWriteData | implemented | used at fetch | fluid_ours.cpp:297 |
| 10 | getParticleDeletionIdWriteData | implemented | | fluid_ours.cpp:298-301 |
| 11 | setParticleCreationIdWriteData | implemented | used at fetch and by addParticles | fluid_ours.cpp:302 |
| 12 | getParticleCreationIdWriteData | implemented | | fluid_ours.cpp:303-306 |
| 13 | getNumReserveParticles | partial | returns the create-time or last-set value; the solver's priority eviction uses the create-time value only (fluid.cpp:1040) | fluid_ours.cpp:310 |
| 14 | setNumReserveParticles | stored only | solver never sees the new value | fluid_ours.cpp:311-314 |
| 15 | getCurrentParticleLimit | partial | returns maxParticles, not the limit set by the setter (header NxFluid.h:292) | fluid_ours.cpp:315 |
| 16 | setCurrentParticleLimit | stub | ignored, logged. Header NxFluid.h:292-302: lowering it deletes the oldest particles in PRIORITY mode | fluid_ours.cpp:316-318 |
| 17 | updateParticles | partial | Forces: FORCE, IMPULSE, VELOCITY_CHANGE, ACCELERATION implemented; SMOOTH_IMPULSE/SMOOTH_VELOCITY_CHANGE run as FORCE with no log (fluid.cpp:974). Flags: only bit 0 (DELETE), only with an id buffer; index-based flags ignored (`:349`). DELETE applied at the next step (fluid.cpp:949) | fluid_ours.cpp:345-357; queueForces `:323-343` |
| 18 | setFluidPacketData | implemented | packet table written at fetch; packetID is a hash guess [A] (`:230`) | fluid_ours.cpp:359; writeOutputs `:214-236` |
| 19 | getFluidPacketData | implemented | | fluid_ours.cpp:360-363 |
| 20 | getSimulationMethod | partial | desc value; solver method fixed at create (fluid.cpp:1760) | fluid_ours.cpp:380 (GET_U) |
| 21 | setSimulationMethod | stored only | solver method fixed at create | fluid_ours.cpp:380 (SET_U) |
| 22 | getStiffness | partial | create-time value used by solver | fluid_ours.cpp:381 |
| 23 | setStiffness | stored only | pressure constant fixed in ctor (fluid.cpp:896-899) | fluid_ours.cpp:381 |
| 24 | getViscosity | partial | create-time value | fluid_ours.cpp:382 |
| 25 | setViscosity | stored only | viscC_ fixed in ctor (fluid.cpp:900) | fluid_ours.cpp:382 |
| 26 | getSurfaceTension | stored only | solver has no surface-tension term (fluid.h:32-65) | fluid_ours.cpp:383 |
| 27 | setSurfaceTension | stored only | as 26 | fluid_ours.cpp:383 |
| 28 | getDamping | partial | create-time value | fluid_ours.cpp:384 |
| 29 | setDamping | stored only | integrate reads P_.damping (fluid.cpp:1434) | fluid_ours.cpp:384 |
| 30 | getFadeInTime | stored only | no fade-in in solver | fluid_ours.cpp:385 |
| 31 | setFadeInTime | stored only | as 30 | fluid_ours.cpp:385 |
| 32 | getExternalAcceleration | partial | create-time value | fluid_ours.cpp:410-413 |
| 33 | setExternalAcceleration | stored only | P_.externalAcceleration fixed (fluid.cpp:1433) | fluid_ours.cpp:414-417 |
| 34 | getProjectionPlane | stored only | no projection in solver | fluid_ours.cpp:418-421 |
| 35 | setProjectionPlane | stored only | NX_FF_PROJECT_TO_PLANE not implemented | fluid_ours.cpp:422-425 |
| 36 | getCollisionMethod | implemented | | fluid_ours.cpp:386 |
| 37 | setCollisionMethod | implemented | include static/dynamic flags of the filter set after hostSync; next step uses them (scene_read.cpp:749, 766) | fluid_ours.cpp:387-392 |
| 38 | getRestitutionForStaticShapes | partial | create-time value | fluid_ours.cpp:393 |
| 39 | setRestitutionForStaticShapes | stored only | P_.restitutionStatic fixed (fluid.cpp:1657) | fluid_ours.cpp:393 |
| 40 | getDynamicFrictionForStaticShapes | partial | solver uses it as frictionStatic (`fluid_ours.cpp:115`, used at fluid.cpp:1656) | fluid_ours.cpp:394 |
| 41 | setDynamicFrictionForStaticShapes | stored only | as 39 | fluid_ours.cpp:394 |
| 42 | getStaticFrictionForStaticShapes | stored only | solver has no stick term; the 2.8.0 core stubs it to 0 [A, re-notes-fluid.md 218-227 vs mesh 3.1], so round trip differs from core | fluid_ours.cpp:395 |
| 43 | setStaticFrictionForStaticShapes | stored only | as 42 (core: no-op) | fluid_ours.cpp:395 |
| 44 | getAttractionForStaticShapes | stored only | no attraction in solver; core stub 0 [A] | fluid_ours.cpp:396 |
| 45 | setAttractionForStaticShapes | stored only | as 44 | fluid_ours.cpp:396 |
| 46 | getRestitutionForDynamicShapes | partial | create-time value | fluid_ours.cpp:397 |
| 47 | setRestitutionForDynamicShapes | stored only | P_.restitutionDynamic fixed | fluid_ours.cpp:397 |
| 48 | getDynamicFrictionForDynamicShapes | partial | create-time value | fluid_ours.cpp:398 |
| 49 | setDynamicFrictionForDynamicShapes | stored only | P_.frictionDynamic fixed (fluid.cpp:1617) | fluid_ours.cpp:398 |
| 50 | getStaticFrictionForDynamicShapes | stored only | none in solver; core stub [A] | fluid_ours.cpp:399 |
| 51 | setStaticFrictionForDynamicShapes | stored only | as 50 | fluid_ours.cpp:399 |
| 52 | getAttractionForDynamicShapes | stored only | as 44 | fluid_ours.cpp:400 |
| 53 | setAttractionForDynamicShapes | stored only | as 44 | fluid_ours.cpp:400 |
| 54 | setCollisionResponseCoefficient | stored only | solver uses fixed 0.2 (fluid.h:51); paramsFromDesc never reads desc 0x2c (`fluid_ours.cpp:95-131`) | fluid_ours.cpp:401 |
| 55 | getCollisionResponseCoefficient | stored only | returns stored value; solver value stays 0.2 | fluid_ours.cpp:401 |
| 56 | setFlag | stored only | DISABLE_GRAVITY, COLLISION_TWOWAY, PRIORITY_MODE are read at create only (`:121-123`) | fluid_ours.cpp:426-430 |
| 57 | getFlag | partial | returns stored bit, any flag | fluid_ours.cpp:431 |
| 58 | getScene | implemented | scene pointer at create | fluid_ours.cpp:432 |
| 59 | getMaxParticles | implemented | | fluid_ours.cpp:402 |
| 60 | getKernelRadiusMultiplier | implemented | | fluid_ours.cpp:403 |
| 61 | getMotionLimitMultiplier | implemented | | fluid_ours.cpp:403 |
| 62 | getCollisionDistanceMultiplier | implemented | | fluid_ours.cpp:403 |
| 63 | getPacketSizeMultiplier | implemented | | fluid_ours.cpp:403 |
| 64 | getRestParticlesPerMeter | implemented | | fluid_ours.cpp:404 |
| 65 | getRestDensity | implemented | | fluid_ours.cpp:404 |
| 66 | getRestParticleDistance | implemented | 1/ppm (header NxFluid.h:1009-1019) | fluid_ours.cpp:433 |
| 67 | getParticleMass | implemented | restDensity / ppm^3 = solver particleMass (fluid.cpp:965-967) | fluid_ours.cpp:434-437 |
| 68 | setGroup | implemented | filter group (scene_read.cpp:752) after hostSync | fluid_ours.cpp:438-442 |
| 69 | getGroup | implemented | | fluid_ours.cpp:443 |
| 70 | setGroupsMask | partial | the mask is used by the scene filter equation (scene_read.cpp:149-153, 757, 771), on by default (`fluid_ours.cpp:570`). See section 5, item 6 | fluid_ours.cpp:444-448 |
| 71 | getGroupsMask | implemented | | fluid_ours.cpp:449-452 |
| 72 | getWorldBounds | implemented | solver bounds; during an async step, the bounds saved at job start (`:455-465`, `:878-880`); empty = +/-FLT_MAX (fluid.cpp:1081-1088) | fluid_ours.cpp:455-465 |
| 73 | loadFromDesc | stub | returns 0, logged | fluid_ours.cpp:466-469 |
| 74 | saveToDesc | partial | copies the stored base (336 bytes), streams and userData; emitter array not written; always returns 1 | fluid_ours.cpp:470-478 |
| 75 | getPPUTime | stub | always 0 | fluid_ours.cpp:479 |
| 76 | setName | stored only | pointer only, not copied; no log | fluid_ours.cpp:480 |
| 77 | getName | implemented | pointer round trip | fluid_ours.cpp:481 |
| 78 | getCompartment | stored only | pointer returned; compartment type not checked; no effect on fluid (only RB-compartment rule on statics, scene_read.cpp:500-502) | fluid_ours.cpp:482 |
| 79 | getForceFieldMaterial | stored only | no force fields in shim or solver (grep: none) | fluid_ours.cpp:483 |
| 80 | setForceFieldMaterial | stored only | as 79 | fluid_ours.cpp:484 |

Wrapped path (`PXSHIM_FLUID=off`): every row is pass-through except 1, 5, 6, 7, 17 and 72 (typed; count and trace only).

## 1b. NxFluidEmitter (41 slots)

No hosted fluid has emitters (fluid_ours.cpp:257-263 and 472-478 drop them). Rows apply only to wrapped fluids, where
the core runs the emitter. Status: pass-through for all 41 (`fluid_wrap.cpp:268-274, 283-300`). Note getFluid (slot 1)
returns the core's NxFluid, not the ShimFluid, so a game call on it bypasses the wrapper [V by code].
Shim names match the header order (count verified). Game use of emitters: none in two logged runs
(`docs/shim-game-run.md:29-31, 74-76`) [V in logs].

| slot | method | status | missing / differs | file:line |
|---|---|---|---|---|
| 0 | ~NxFluidEmitter | pass-through |  | fluid_wrap.cpp:268-274 |
| 1 | getFluid | pass-through | returns the core's NxFluid, not the ShimFluid | fluid_wrap.cpp:268-274 |
| 2 | setGlobalPose | pass-through |  | fluid_wrap.cpp:268-274 |
| 3 | setGlobalPosition | pass-through |  | fluid_wrap.cpp:268-274 |
| 4 | setGlobalOrientation | pass-through |  | fluid_wrap.cpp:268-274 |
| 5 | getGlobalPoseVal | pass-through |  | fluid_wrap.cpp:268-274 |
| 6 | getGlobalPositionVal | pass-through |  | fluid_wrap.cpp:268-274 |
| 7 | getGlobalOrientationVal | pass-through |  | fluid_wrap.cpp:268-274 |
| 8 | setLocalPose | pass-through |  | fluid_wrap.cpp:268-274 |
| 9 | setLocalPosition | pass-through |  | fluid_wrap.cpp:268-274 |
| 10 | setLocalOrientation | pass-through |  | fluid_wrap.cpp:268-274 |
| 11 | getLocalPoseVal | pass-through |  | fluid_wrap.cpp:268-274 |
| 12 | getLocalPositionVal | pass-through |  | fluid_wrap.cpp:268-274 |
| 13 | getLocalOrientationVal | pass-through |  | fluid_wrap.cpp:268-274 |
| 14 | setFrameShape | pass-through | frame shape not read by the shim | fluid_wrap.cpp:268-274 |
| 15 | getFrameShape | pass-through |  | fluid_wrap.cpp:268-274 |
| 16 | getDimensionX | pass-through |  | fluid_wrap.cpp:268-274 |
| 17 | getDimensionY | pass-through |  | fluid_wrap.cpp:268-274 |
| 18 | setRandomPos | pass-through |  | fluid_wrap.cpp:268-274 |
| 19 | getRandomPos | pass-through |  | fluid_wrap.cpp:268-274 |
| 20 | setRandomAngle | pass-through |  | fluid_wrap.cpp:268-274 |
| 21 | getRandomAngle | pass-through |  | fluid_wrap.cpp:268-274 |
| 22 | setFluidVelocityMagnitude | pass-through |  | fluid_wrap.cpp:268-274 |
| 23 | getFluidVelocityMagnitude | pass-through |  | fluid_wrap.cpp:268-274 |
| 24 | setRate | pass-through |  | fluid_wrap.cpp:268-274 |
| 25 | getRate | pass-through |  | fluid_wrap.cpp:268-274 |
| 26 | setParticleLifetime | pass-through |  | fluid_wrap.cpp:268-274 |
| 27 | getParticleLifetime | pass-through |  | fluid_wrap.cpp:268-274 |
| 28 | setRepulsionCoefficient | pass-through |  | fluid_wrap.cpp:268-274 |
| 29 | getRepulsionCoefficient | pass-through |  | fluid_wrap.cpp:268-274 |
| 30 | resetEmission | pass-through |  | fluid_wrap.cpp:268-274 |
| 31 | getMaxParticles | pass-through |  | fluid_wrap.cpp:268-274 |
| 32 | getNbParticlesEmitted | pass-through |  | fluid_wrap.cpp:268-274 |
| 33 | setFlag | pass-through |  | fluid_wrap.cpp:268-274 |
| 34 | getFlag | pass-through |  | fluid_wrap.cpp:268-274 |
| 35 | getShape | pass-through |  | fluid_wrap.cpp:268-274 |
| 36 | getType | pass-through |  | fluid_wrap.cpp:268-274 |
| 37 | loadFromDesc | pass-through |  | fluid_wrap.cpp:268-274 |
| 38 | saveToDesc | pass-through |  | fluid_wrap.cpp:268-274 |
| 39 | setName | pass-through |  | fluid_wrap.cpp:268-274 |
| 40 | getName | pass-through |  | fluid_wrap.cpp:268-274 |

Note: the shim's emitter name list (`fluid_wrap.cpp:73-81`) follows the header order (count verified; the index is the
thunk index, `fluid_wrap.cpp:268-274`).

## 1c. NxScene fluid-related methods (and the hooks that drive fluids)

| slot | method | status | missing / differs | file:line |
|---|---|---|---|---|
| 38 | createCompartment | not hooked | core creates it; hosted fluids use only the RB rule on statics | PhysXLoader.cpp:218-232 (not patched) |
| 39 | getNbCompartments | not hooked | | |
| 40 | getCompartmentArray | not hooked | | |
| 93 | setFluidUserNotify | not hooked | core keeps the callback; hosted fluids never generate its event (NX_FET_NO_PARTICLES_LEFT, NxFluidUserNotify.h:24-27), so it never fires for them | |
| 94 | getFluidUserNotify | not hooked | | |
| 126 | createFluid | implemented | hosted: no descriptor validation (the core refuses invalid ones; [V doc] `docs/re-notes-fluid-mesh.md:226-231`); emitters dropped; desc read as 352 bytes (`memcpy` at fluid_ours.cpp:556) while NxFluidDescBase is 336 bytes [A that the game passes a full NxFluidDesc] | PhysXLoader.cpp:227; hkCreateFluid :254-257; fluid_wrap.cpp:469-521 |
| 127 | releaseFluid | implemented | hosted: hostDestroy, core not called (so the core count is unchanged) | PhysXLoader.cpp:228; hkReleaseFluid :259-262; fluid_wrap.cpp:523-543; fluid_ours.cpp:586-591 |
| 128 | getNbFluids | not hooked | returns core fluids only; hosted fluids missing | |
| 129 | getFluids | not hooked | returns core NxFluid pointers only; hosted fluids missing | |
| 130 | cookFluidMeshHotspot | not hooked | core cooks; hosted fluids read scene geometry directly, so no effect | |
| 131-132 | createCloth / releaseCloth | out of scope | cloth wrapper | PhysXLoader.cpp:231 |
| 140 | simulate | implemented | hosted steps before the core simulate; async on the fluid thread unless two-way | PhysXLoader.cpp:229, 264-280; fluid_wrap.cpp:559-569; fluid_ours.cpp:809-893 |
| 142 | fetchResults | implemented | outputs written only when fetch returned true; waits for the job | PhysXLoader.cpp:230, 282-314; fluid_wrap.cpp:571-584; fluid_ours.cpp:902-907 |
| 48 | getGroupCollisionFlag | read by shim | group pair table, read at refresh | scene_read.cpp:30, 661 |
| 59-62 | getFilterOps / Bool / Constant0 / Constant1 | read by shim | live filter state for the groupsMask equation | scene_read.cpp:338-342 |
| 83 | getTiming | read by shim | maxTimestep, maxIter, method | scene_read.cpp:828-838; fluid_ours.cpp:812-813 |

## 2. Descriptors

Line numbers without a file name in sections 2a-2c are in `shim/fluid_ours.cpp`. Byte offset = dword index * 4. NxFluidDescBase (0x00-0x53) is 336 bytes [V `docs/re-notes-fluid-mesh.md` 3.1];
NxFluidDesc adds the emitter array at 0x54/0x55. Shim offsets are the `fluid_internal.h`/`fluid_ours.cpp` constants.
"Read" means read from the descriptor at create (or copied). "Used by solver" names the solver field.

### 2a. NxFluidDesc (shim: fluid_ours.cpp:75-81 constants; paramsFromDesc `:95-131`)

| offset | field | read / used by solver / ignored | missing / differs | file:line |
|---|---|---|---|---|
| 0x00-0x3f | initialParticleData (NxParticleData) | read; added at create if count and pos buffer set (`:573-578`) | other buffers used only through the add path | fluid_ours.cpp:573-578 |
| 0x40 | maxParticles | read; solver cap | | :102 |
| 0x44 | numReserveParticles | read; priority target only | addParticles ignores it (fluid.cpp:913) [A] | :103 |
| 0x48 | restParticlesPerMeter | read; spacing | | :104 |
| 0x4c | restDensity | read; mass, pressure | | :105 |
| 0x50 | kernelRadiusMultiplier | read | | :106 |
| 0x54 | motionLimitMultiplier | read | | :107 |
| 0x58 | collisionDistanceMultiplier | read | | :108 |
| 0x5c | packetSizeMultiplier | read; packet cell size (`:179`) | packetID hash [A] | :109 |
| 0x60 | stiffness | read | | :110 |
| 0x64 | viscosity | read | | :111 |
| 0x68 | surfaceTension | ignored | no solver term | (no read) |
| 0x6c | damping | read; integrate (fluid.cpp:1434) | | :112 |
| 0x70 | fadeInTime | ignored | no fade-in | (no read) |
| 0x74 | externalAcceleration (3 floats) | read; fluid.cpp:1433 | | :113 |
| 0x80 | projectionPlane (4 floats) | ignored | NX_FF_PROJECT_TO_PLANE not implemented | (no read) |
| 0x90 | restitutionForStaticShapes | read; fluid.cpp:1657 | | :114 |
| 0x94 | dynamicFrictionForStaticShapes | read as frictionStatic; fluid.cpp:1656, 1464 | | :115 |
| 0x98 | staticFrictionForStaticShapes | ignored | no stick term; core 2.8.0 has none [A] | (no read) |
| 0x9c | attractionForStaticShapes | ignored | | (no read) |
| 0xa0 | restitutionForDynamicShapes | read; fluid.cpp:1617, 1545 | | :116 |
| 0xa4 | dynamicFrictionForDynamicShapes | read; fluid.cpp:1617 | | :117 |
| 0xa8 | staticFrictionForDynamicShapes | ignored | | (no read) |
| 0xac | attractionForDynamicShapes | ignored | | (no read) |
| 0xb0 | collisionResponseCoefficient | ignored; solver uses 0.2 | desc value not used; game value is 0.2 (`docs/shim-game-run.md:44`) | fluid.h:51; fluid.cpp:1582, 1622 |
| 0xb4 | simulationMethod | read; SPH / NoInteraction / Mixed (`:118-120`) | unknown values silently SPH | fluid_ours.cpp:118-120 |
| 0xb8 | collisionMethod | read; include static/dynamic filter (`:571-572`) | | |
| 0xbc | collisionGroup (u16) | read; filter group (`:566`) | | |
| 0xc0 | groupsMask (4 dwords) | read; scene filter equation (`:567`) | see section 5, item 6 | scene_read.cpp:757, 771 |
| 0xd0 | forceFieldMaterial (u16) | stored only | no force fields | fluid_ours.cpp:483-484 |
| 0xd4 | particlesWriteData (NxParticleData) | read; write-buffer target | see 2b | fluid_ours.cpp:154-212 |
| 0x114 | particleDeletionIdWriteData | read; fetch | | fluid_ours.cpp:238-244 |
| 0x124 | particleCreationIdWriteData | read; add and reset at fetch | | fluid_ours.cpp:141-150, 245 |
| 0x134 | fluidPacketData | read; written at fetch | | fluid_ours.cpp:214-236 |
| 0x13c | flags (NxFluidFlag) | partial: 3 bits read (section 3a) | | fluid_ours.cpp:121-123 |
| 0x140 | userData | read; copied to NxFluid::userData | | fluid_wrap.cpp:490 |
| 0x144 | name | stored only (pointer) | not used | fluid_ours.cpp:480 |
| 0x148 | compartment | stored only; returned by getCompartment | type not checked | fluid_ours.cpp:482 |
| 0x14c | type (NxFluidDescType) | ignored | | |
| 0x150 | emitters.begin (NxFluidDesc only) | ignored (dumped to log) | emitters dropped in hosted mode | fluid_wrap.cpp:472-478 |
| 0x154 | emitters.end | ignored (dumped to log) | | fluid_wrap.cpp:472-478 |

Wrapped fluids get the descriptor unchanged: the core reads every field, emitters included (fluid_wrap.cpp:483).

### 2b. Buffers used by the fluid API

| struct (size) | field (offset) | read / used / ignored | missing / differs | file:line |
|---|---|---|---|---|
| NxParticleData (64) | numParticlesPtr (+0x00) | read on add (`:268`); written at fetch (`:212`) | | fluid_internal.h:18-23; fluid_ours.cpp:268, 212 |
| | bufferPos (+0x04), bufferVel (+0x08) | read on add (`:273-274`); written at fetch (`:204-205`) | | |
| | bufferLife (+0x0c) | read on add (`:275`); written at fetch as remaining life (`:206`) | | |
| | bufferDensity (+0x10) | written at fetch (`:207`); zero on add (`:165`); input ignored | | |
| | bufferId (+0x14) | written at fetch and add; input ignored on add | | fluid_ours.cpp:166, 208 |
| | bufferFlag (+0x18) | written at fetch (`:209`); zero on add | | |
| | bufferCollisionNormal (+0x1c) | written at fetch (`:210`); zero on add | | |
| | strides (+0x20..+0x38, 7) | used for addressing | | fluid_internal.h:18-23 |
| | name (+0x3c) | ignored | | |
| NxParticleIdData (16) | numIdsPtr, bufferId, stride, name | used in creation/deletion writes | | fluid_internal.h:26-28; fluid_ours.cpp:141-150, 238-244 |
| NxFluidPacketData (8) | bufferFluidPackets (+0), numFluidPacketsPtr (+4) | written at fetch | | fluid_internal.h:29-31; fluid_ours.cpp:214-236 |
| NxFluidPacket (36) | aabb (0..23), firstParticleIndex (24), numParticles (28), packetID (32) | written; packetID is a guessed hash [A] | packet list is one per occupied cell (matches header, [A] that the game does not read them) | fluid_ours.cpp:214-236 |
| NxParticleUpdateData (32) | forceMode (+0) | read | | fluid_internal.h:33-35; fluid_ours.cpp:326 |
| | numUpdates (+4) | ignored for index-based updates (`:334-341`), used as loop bound with ids | | fluid_ours.cpp:327-340, 352 |
| | bufferForce, strideForce | read (forces) | no bounds check on the user's force buffer | fluid_ours.cpp:323-343 |
| | bufferFlag, strideFlag, bufferId, strideId | read (delete by id) | index-based flag updates ignored | fluid_ours.cpp:349-356 |

### 2c. NxFluidEmitterDesc (0x74 bytes)

All fields ignored: hosted fluids create no emitters (`fluid_ours.cpp:257-263`), and the shim only dumps the
descriptor (`fluid_wrap.cpp:306`, `:472-478`). Wrapped fluids pass it to the core. Offsets (header order, 2.8.1
NxFluidEmitterDesc.h:85-195): relPose +0x00 (NxMat34, 48 bytes), frameShape +0x30, type +0x34, maxParticles +0x38,
shape +0x3c, dimensionX +0x40, dimensionY +0x44, randomPos +0x48 (3 floats), randomAngle +0x54,
fluidVelocityMagnitude +0x58, rate +0x5c, particleLifetime +0x60, repulsionCoefficient +0x64, flags +0x68,
userData +0x6c, name +0x70. Total 0x74 [V, sizes add up].

## 3. Flags, enums, modes

### 3a. NxFluidFlag (NX_FF_*, header NxFluidDesc.h:81-160)

| flag | value | status | what it does in the shim | file:line |
|---|---|---|---|---|
| NX_FF_VISUALIZATION | 1<<0 | ignored | no effect on sim [A: header says debug draw] | |
| NX_FF_DISABLE_GRAVITY | 1<<1 | read | disableGravity; gravity term skipped (fluid.cpp:1433) | fluid_ours.cpp:121 |
| NX_FF_COLLISION_TWOWAY | 1<<2 | read | P_.twoWay (fluid.cpp:1582, 1622, 1770); also needs includeDynamic (fluid_ours.cpp:844) and NX_SF_FLUID_TWOWAY on a non-kinematic body (scene_read.cpp:545, 716) | fluid_ours.cpp:123 |
| NX_FF_ENABLED | 1<<3 | ignored | simulation always runs (header: enable/disable execution) | |
| NX_FF_HARDWARE | 1<<4 | ignored | SW path only; the game's flags 0x29 do not set it (`docs/shim-game-run.md:44-47`) | |
| NX_FF_PRIORITY_MODE | 1<<5 | read | evictPriority keeps max - reserve (fluid.cpp:1037-1059); setCurrentParticleLimit not applied | fluid_ours.cpp:122 |
| NX_FF_PROJECT_TO_PLANE | 1<<6 | ignored | projectionPlane stored only | fluid_ours.cpp:422-425 |
| NX_FF_FORCE_STRICT_COOKING_FORMAT | 1<<7 | ignored | header says "not implemented" | |

### 3b. Simulation and collision methods

| enum | value | status | what it does | file:line |
|---|---|---|---|---|
| NX_F_SPH | 1<<0 | read | SPH with pair search (fluid.cpp:1760) | fluid_ours.cpp:118-120 |
| NX_F_NO_PARTICLE_INTERACTION | 1<<1 | read | no SPH; flags SEPARATED (fluid.cpp:1762-1763); newParticleShapeLatency 0 (`fluid_ours.cpp:128`) | fluid_ours.cpp:118-120, 128 |
| NX_F_MIXED_MODE | 1<<2 | read | alternates SPH and no-interaction each step (fluid.cpp:1760, 1771); latency 2 | fluid_ours.cpp:118-120 |
| other values | - | read as SPH | silent; the core would reject (header: exactly one of the three) [A] | fluid_ours.cpp:118-120 |
| NX_F_STATIC | 1<<0 | read | static shapes selected (scene_read.cpp:749) | fluid_ours.cpp:571 |
| NX_F_DYNAMIC | 1<<1 | read | dynamic shapes selected (scene_read.cpp:766); PXSHIM_FLUID_NODYN turns it off | fluid_ours.cpp:572 |

### 3c. Emitter flags and types (NX_FEF_*, NX_FE_*)

All ignored: no hosted emitters. Values for reference (NxFluidEmitterDesc.h:19-66): NX_FEF_VISUALIZATION 1,
NX_FEF_FORCE_ON_BODY 4, NX_FEF_ADD_BODY_VELOCITY 8, NX_FEF_ENABLED 16; NX_FE_RECTANGULAR 1, NX_FE_ELLIPSE 2;
NX_FE_CONSTANT_PRESSURE 1, NX_FE_CONSTANT_FLOW_RATE 2. Wrapped fluids: core runs them (fluid_wrap.cpp:268-300).

### 3d. Particle flags written to the flag buffer (NX_FP_*, NxParticleData.h:18-23)

Solver values match the header (fluid.h:67-72). Written at fetch (`fluid_ours.cpp:209`).

| flag | value | status | where set | file:line |
|---|---|---|---|---|
| NX_FP_COLLISION_WITH_STATIC | 1 | written | static contact (cached: fluid.cpp:1552; fresh: 1659) | |
| NX_FP_COLLISION_WITH_DYNAMIC | 2 | written | dynamic contact or proximity (fluid.cpp:1552, 1624) | |
| NX_FP_SEPARATED | 4 | written | no neighbour (fluid.cpp:1332, 1410); whole step for no-interaction (fluid.cpp:1763) | |
| NX_FP_MOTION_LIMIT_REACHED | 8 | written | motion limit hit (fluid.cpp:1697) | |

Input flag buffer on add is ignored (`fluid_ours.cpp:272-276`).

### 3e. Update flags and force modes (NxParticleUpdateData.h; Nxp.h:313-320)

| item | value | status | what it does | file:line |
|---|---|---|---|---|
| NX_FP_DELETE | 1 | read | delete by id at the next step (fluid.cpp:949, 1027-1035); only bit 0 looked at | fluid_ours.cpp:352, 356 |
| NX_FORCE | 0 | implemented | dv = F * simTime / mass (fluid.cpp:974) | fluid_ours.cpp:331, 339 |
| NX_IMPULSE | 1 | implemented | dv = F / mass (fluid.cpp:971) | |
| NX_VELOCITY_CHANGE | 2 | implemented | dv = F (fluid.cpp:972) | |
| NX_SMOOTH_IMPULSE | 3 | as FORCE | core: "mode not supported", uses NX_FORCE [V doc `docs/re-notes-fluid-api.md`]; the shim logs nothing | fluid.cpp:974; fluid.h:28-29 |
| NX_SMOOTH_VELOCITY_CHANGE | 4 | as FORCE | as 3 | fluid.cpp:974 |
| NX_ACCELERATION | 5 | implemented | dv = F * simTime (fluid.cpp:973) | |

The change is queued and added at the start of the next step that runs a substep (fluid.cpp:953-963, 1757); the
scale uses the last simulate's substep time (`fluid_ours.cpp:846`). Matches the rules in `docs/re-notes-fluid-api.md`.

### 3f. Shape and actor flags that matter to fluid (Nxp.h:442-508, 903-929)

| flag | value | status | what it does | file:line |
|---|---|---|---|---|
| NX_SF_DISABLE_COLLISION | 1<<4 | read, excluded | static: `shapeCollidesWithFluid` (`scene_read.cpp:513`); dynamic: `:544` | fluid.h:168, 182 |
| NX_SF_FLUID_DRAIN | 1<<8 | read, drain | static `scene_read.cpp:515`; dynamic `:543`; solver fluid.cpp:1600-1601, 1640, 1700, 1768-1769. Removed at the next step, after its response [V probes, `docs/re-notes-fluid-drain.md`]. Dynamic drain untested | fluid.h:169; fluid.h:116, 160 |
| NX_SF_FLUID_DISABLE_COLLISION | 1<<10 | read, excluded | | fluid.h:170, 182 |
| NX_SF_FLUID_TWOWAY | 1<<11 | read on dynamic shapes | static: n/a | scene_read.cpp:422, 545, 716 |
| NX_SF_DISABLE_RESPONSE | 1<<12 | read, excluded | also gates the pair [V drain notes] | fluid.h:171, 182 |
| NX_TRIGGER_ENABLE | bits 0-2 | read, excluded | triggers never collide; drain on a trigger does nothing [V drain notes] | fluid.h:172, 182; scene_read.cpp:53 |
| NX_AF_DISABLE_COLLISION | 1<<0 | read, excluded | static `scene_read.cpp:508-513`; dynamic `:704` | fluid.h:173, 182 |
| NX_AF_DISABLE_RESPONSE | 1<<1 | read, excluded | as above | fluid.h:174 |
| NX_AF_FLUID_DISABLE_COLLISION | 1<<3 | read, excluded | as above | fluid.h:175 |
| NX_SCT_RIGIDBODY | 0 | read, static only | static actor in an RB compartment is invisible to fluid [V drain notes]. Dynamic actors: rule not applied (`scene_read.cpp:544`). The actor-compartment getter (slot 100) and the compartment type getter (slot 0) are [A] (`scene_read.cpp:56`) | scene_read.cpp:500-502 |
| NX_SCT_FLUID | 1 | no effect | fluid compartment does not change collisions [V drain notes] | |
| NX_FET_NO_PARTICLES_LEFT | 0 | never generated | NxFluidUserNotify::onEvent (NxFluidUserNotify.h:24-27) | |
| NX_FEET_EMITTER_EMPTY | 0 | never generated | no emitters | NxFluidUserNotify.h:34-37 |

### 3g. Timestep method (NX_TIMESTEP_*, NxSceneDesc.h:25-30)

FIXED (0): the shim steps `floor(accum / maxTimestep)` substeps of `maxTimestep`, capped at `maxIter`; time past
that is dropped [A] (`fluid_ours.cpp:825-832`). Anything else: one step of `elapsed` (`:823-824`). INHERIT (2) is
compartment-only; not seen here [A].

## 4. Modes the shim supports

`PXSHIM_FLUID` is read at each createFluid (`shim/fluid_ours.cpp:547-552`). Unset or any value other than the
ones below = `ours-all`: every fluid runs on our solver (default; `fluid_ours.cpp:3-5, 551`). `ours` = only
NO_PARTICLE_INTERACTION fluids are hosted (the game's collide-only debris, glass, dust); others stay with the core
(`:550`, the test reads the descriptor's simulationMethod at create only). `off` or `0` = wrapper only, the core
runs everything (`:549`; `fluid_wrap.cpp:1-7`). Wrapped fluids go through `fluidCreate` with the descriptor
unchanged; a core refusal returns 0 (`fluid_wrap.cpp:482-487`). Other switches: `PXSHIM_FLUID_ASYNC` (default on;
off with two-way, `fluid_ours.cpp:868-869`), `PXSHIM_THREADS` (`:523-527`), `PXSHIM_FLUID_LATENCY` (`:127-128`),
`PXSHIM_FLUID_NOMASK` (`:570`), `PXSHIM_FLUID_NODYN` (`:572`), `PXSHIM_REF_HZ` (default 62 Hz, `PhysXLoader.cpp:62-70`),
`PXSHIM_TRACE` (`fluid_wrap.cpp:454`), `PXSHIM_GEOM_CENSUS` (`:509`, `fluid_ours.cpp:854`), `PXSHIM_DUMP_MAX`,
`PXSHIM_DUMP_EVERY`, `PXSHIM_DUMP_MS` (`fluid_ours.cpp:617-621, 694, 724`).
Default at audit time: hosted fluids, as documented in README.md. Test runners explicitly select pass-through when observing the original core.
Test scripts default to `off` (`tools/run_fluid_shim.sh:20`, `tools/run_cloth_shim.sh:23`).

## 5. Known differences (comments, TODOs, and gaps found by this audit)

The files listed contain no TODO, FIXME or XXX markers [V grep]. Markers found:

1. Setters not applied to the running solver: "stored, not applied to the running solver" (`shim/fluid_ours.cpp:10`, `:308`); the values the solver reads are fixed at create (`fluid_ours.cpp:95-131`; `solver/fluid/fluid.cpp:883-904`). Affects rows 14, 21, 23, 25, 29, 33, 39, 41, 47, 49, 54, 56. The game never calls setters in the logged runs (`docs/shim-game-run.md:31-33`) [V in logs].
2. setCurrentParticleLimit ignored: `shim/fluid_ours.cpp:316-318` (logs "ignored").
3. createEmitter not supported: `fluid_ours.cpp:258`; emitters in the descriptor dropped (`fluid_wrap.cpp:472-478`, dump only).
4. loadFromDesc not supported: `fluid_ours.cpp:466-469`.
5. Two-way impulses from an async job dropped: `fluid_ours.cpp:793` "not expected". Unreachable as long as two-way scenes step inline (`fluid_ours.cpp:868-869`).
6. Scene filter equation applied to fluid masks by default: `scene_read.cpp:149-153, 757, 771`; default on at `fluid_ours.cpp:570` (switch `PXSHIM_FLUID_NOMASK`). Comment `fluid_ours.cpp:568-570`: a probe showed no effect "only under the default filter state", and UE3 sets its own filter ops. The solver's oracle parser ignores fluid.groupsMask (`solver/fluid/scenario.h:199`). The result depends on the scene's live ops and constants: with zero constants the masks drop out of the equation. The game's filter state (ops, constants) is not probed in the repo [A]. This is the most likely pair-gating divergence and needs a probe through the shim.
7. Friction floor: `solver/fluid/fluid.cpp:883-887` "[A, fitted]": zero friction behaves as 0.001.
8. Latency split by method: `fluid_ours.cpp:124-128` (SPH/mixed 2 steps, collide-only 0). Fitted to game smoke, per the comment.
9. Step-rate independence: `solver/fluid/fluid.h:60-63`, `shim/PhysXLoader.cpp:62-70` (refDt 1/62 s). Deliberate: not in the original.
10. Drain removal: solver removes the drained particle one step after the response (`fluid.cpp:1766-1769`); probes confirm the timing (`docs/re-notes-fluid-drain.md`).
11. Packet ID hash: `fluid_ours.cpp:230` "[A]". Not verified against the core.
12. Per-call 4096 cap: `fluid_ours.cpp:86, 269` (header says per step; header NxFluid.h:148).
13. Capacity ignores the reserve: `solver/fluid/fluid.cpp:913` [A vs core, re-notes-fluid-api "min(request, max - live - reserve term)" listed as open].
14. Unbounded user-buffer writes: `fluid_ours.cpp:154-171` (add), `:194-212`, `:240-242`. Writes `count + i` and `i < n` with no capacity check. Safe only if the game's buffers are sized to maxParticles [A].
15. Descriptor validation absent on the hosted path: the core rejects bad descriptors (`docs/re-notes-fluid-mesh.md:226-231`); `fluid_wrap.cpp:469-506` passes hosted descriptors through unchecked and `fluid_ours.cpp:554-584` never fails. Wrapped fluids keep the core's check.
16. Dead branch: `fluid_wrap.cpp:499-506` "our fluid refused the descriptor" cannot fire, since hostCreate always returns a non-null pointer (`fluid_ours.cpp:554-584`).
17. Smooth force modes run as FORCE without a log (`fluid.cpp:974`; core logs "mode not supported" [V doc]).
18. saveToDesc omits the emitter array and always returns 1 (`fluid_ours.cpp:470-478`).
19. Wrapped trace shadows go stale: creation/deletion/packet pointers are copied at create only (`fluid_wrap.cpp:495-498`), but the setters go to the core. Trace-only.
20. Hosted NX_FF_ENABLED, PROJECT_TO_PLANE, FADE_IN, surfaceTension and collisionResponseCoefficient from the desc are not read (`fluid_ours.cpp:95-131`).
21. Shape and actor vtable slots are [A]: `scene_read.cpp:3-7` ("[A] for the rest"); `scene_read.cpp:56` (actor compartment slot 100, compartment type slot 0 [A]). A wrong slot would silently change which statics the fluid sees.

## 6. Documentation conflicts found

- `docs/re-notes-fluid.md:81-88, 218-227`: says the 2.8.0 LL descriptor has 7 collision floats and may not have static friction. `docs/re-notes-fluid-mesh.md:226-231, 256-261`: says the same 8-field layout for 2.8.0 and 2.8.1. The shim follows the mesh note (`fluid_ours.cpp:113-117`). The game's logged friction values (`docs/shim-game-run.md:44-46`) fit either reading.
- `solver/fluid/scenario.h:199` says the groupsMask has no effect on fluid pairs; `fluid_ours.cpp:570` keeps the mask on by default (item 6 above).
- `docs/re-notes-fluid-mesh.md:257-261`: getters on stub slots return 0 in 2.8.0; the shim returns stored values (rows 42-53). Round trip differs from the core.


## Independent review (spot-checks)
- Gap 2 (hosted fluids have no emitters): confirmed, hCreateEmitter logs and returns null; getNbEmitters/getEmitters return 0 (fluid_ours.cpp:257-263). The cited fluid_wrap.cpp:472-478 only dumps descriptor emitters in the wrap path's log; that descriptor emitters are dropped on the hosted path is [A] until the createFluid conversion is checked.
- Gap 3 (setCurrentParticleLimit ignored): confirmed (fluid_ours.cpp:315-318).
- Gap 6 (collisionResponseCoefficient): confirmed, the param conversion sets restitution/friction (fluid_ours.cpp:114-116) but never the solver's collisionResponseCoefficient, which stays at 0.2 (fluid.h:51). The getter/setter only touch the stored desc word (fluid_ours.cpp:401).
