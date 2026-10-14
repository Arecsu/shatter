# PhysX compatibility gap audit (historical)

The table is the initial audit snapshot, not the current implementation status. Corrections and fixes are recorded in the dated sections below; use [CONTRIBUTING.md](../../CONTRIBUTING.md) for current development guidance.

Sources: exe side `exe-setup.md`, `exe-cloth-create.md` (B1), `exe-cloth-runtime.md` (B2), `exe-fluid.md` (B3),
`exe-scene.md` (B4); shim side `shim-cloth.md` (A2), `shim-fluid.md` (A3), `shim-scene.md` (A4). Each file ends with the
independent spot-checks. This table cites those files; open them for addresses and file:line.

Severity: **W** = wrong behaviour (the game sets or calls it and we do something different), **M** = missing (feature
the game can reach, we ignore it), **C** = cosmetic / parity only / no visible effect expected, **-** = no gap.
"Game uses" = static reachability in MirrorsEdge.exe. Whether any shipped content actually turns a path on is step 2
(content scan), joined on the "UE3 source" column; whether it happens at run time is step 4 (coverage counters).

## How the game talks to PhysX (read this first)
- All engine calls go through a **statically linked double-buffering wrapper** (NxdScene/NxdCloth/NxdFluid/..., RTTI in the
  exe, slot i of NxdX = slot i of the 2.8.0 NxX). While the scene simulates, setters are stored in the wrapper and replayed
  to the real object (our shim) **in NxdScene::checkResults, just before the real fetchResults** [V]. The shim therefore sees
  most game writes as a burst right before fetchResults, plus direct calls while the scene is idle.
- The wrapper never touches the real cloth's public `userData` (+4), so the shim's private pointer there is safe [V B2].
- The wrapper **drops** setMinAdhereVelocity (never replayed) and, on a tear (particle count change), drops the game's
  setPositions for that flush. These behaviours are in the original too (they are in the exe), so they are not gaps.
- UE3 properties are bound from the packages, so the exe has few property-name strings. The "UE3 source" column gives the
  owning object and offset (e.g. `ClothData+0x1dc bit 3`, `UPS+0x84`) plus the probable property name [A]. Step 2 must map
  offsets to names via `tools/ue3pkg.py` (class property order).

## Table

Ordered: W first, then M, then C; within a group, by how likely the content is to reach it.

| # | API item | game uses it (exe) | our status (shim) | gap | UE3 source (join key for step 2) | content (step 2) | evidence |
|---|---|---|---|---|---|---|---|
| 1 | NxFluidDesc.collisionResponseCoefficient | set at create from the asset, clamp >= 0 [V] | read into the desc words only; solver fixed at 0.2 | **W** whenever an asset is not 0.2 | 0 assets != 0.2 (all 155 UPS instances inherit 0.2); no bTwoWayCollision [V content-*] | UPhysXParticleSystem+0x84 (CollisionResponseCoefficient [A]) | exe-fluid §2 + review; shim-fluid gap 6 + review |
| 2 | Cloth contact vs moving shapes (friction / normal clamp use v - v_shape) | engine moves kinematic bodies (SetFixed -> NX_BF_KINEMATIC, MoveKinActorTransform) and dynamic bodies [V B4] | world velocity used; shape velocity ignored | **W** on lifts, moving platforms, swinging bodies | n.a. (scene state; Escape_p has kinematic/dynamic bodies near cloths) | none (scene state) | shim-scene gap 2 |
| 3 | Cloth static shapes after teleport/large motion | natives SetClothPosition (setPositions), ResetClothVertsToRefPose; cloth follows moving owners [V B2] | statics collected once in a box at first build; never re-queried | **W** after teleport/reset or a cloth that travels | no Kismet/map use of SetClothPosition/ResetClothVertsToRefPose; natives only in Engine.u name table [V content-pkgs, -maps2] | USkeletalMeshComponent.SetClothPosition / ResetClothVertsToRefPose | shim-scene gap 1 |
| 4 | NX_CLF_VALIDBOUNDS + setValidBounds | native EnableClothValidBounds / SetClothValidBounds [V]; captures show flag 0x8000 and one setValidBounds per cloth | particles strictly outside the bounds are removed (spec below); **fixed 2026-10-12 (late)**, see that session | **W** (rarely visible: only cloth leaving its bounds) | bEnableValidBounds on 25 placed components in 3 maps (Cranes_Plaza_Spt, Cranes_Off_Spt, Subway_Roof-RenCo_Spt) [V]; but Escape_p traces show VALIDBOUNDS 0x8000 set at run time on most cloths (source not traced) | USkeletalMeshComponent.bEnableValidBounds / ValidBoundsMin/Max [A]; Kismet | shim-cloth gap 2 + review; exe-cloth-runtime §3.1 |
| 5 | setFlags(NX_CLF_DISABLE_COLLISION) on a live cloth | UpdateClothParams sets/clears it from component+0x324 bit 1 [V] | rebuild keeps statics, body shapes stall at the origin -> still collides | **W** when content/script sets it (traced flags so far never do) | bDisableClothCollision=True on P_Flags.SK_Flag_02 flag prefabs (P_Buildings.upk) [V]; placed copies: maps2 13 in 9 maps, maps0 says placements do not inherit it (conflict); traced runs: never at create, never toggled | USkeletalMeshComponent+0x324 bit 1 (bDisableClothCollision [A]) | exe-cloth-create flags; shim-scene gap 12 |
| 6 | Dynamic shape identity in cloth (slot per geometry pointer) | n.a. (internal) | primitive shapes get a new pointer every 120 refreshes -> parked slots accumulate; shape attachments may break | **W** [A on visibility], perf | n.a. | none | shim-scene gap 4 + review |
| 7 | NX_CLF_PRESSURE + setPressure | flag from ClothData+0x1dc bit 2, value ClothData+0x1c8 (max 0) [V]; game calls setPressure on every cloth | stored only; no pressure in the solver | **M** (W if any asset sets the bit) | 0 meshes with bEnableClothPressure [V all 4 scans] | ClothData+0x1dc bit 2 / +0x1c8 (bEnableClothPressure / ClothPressure [A]) | exe-cloth-create flags; shim-cloth gap 3 |
| 8 | NX_CLF_COLLISION_TWOWAY + setCollisionResponseCoefficient (cloth) | flag from ClothData+0x1dc bit 3, coefficient ClothData+0x1cc [V]; every game shape carries NX_SF_CLOTH_TWOWAY (SDK default 0x120008) [V] | flag logged unsupported; coefficient stored only; no contact impulses on bodies | **M** (W if any asset sets the bit) | 0 meshes with bEnableClothTwoWayCollision, coefficient 0.2 everywhere [V] | ClothData+0x1dc bit 3 / +0x1cc (bEnableClothTwoWayCollision [A] / ClothCollisionResponseCoefficient) | exe-scene §0 + review; shim-cloth gap 5; shim-scene gap 15 |
| 9 | NxCloth::attachToCore (slot 34) | called by FUN_00d49090 (vtable, buoyancy-type attach) when component+0x324 bit 0, +0x314 & 0x200 and the body has shapes; params ClothData+0x1f0..0x1f8 [V] | stub (logs, no effect) | **M** if content enables it | 0 meshes with bClothMetal / non-default ClothMetal* [V] | component+0x314 & 0x200, ClothData+0x1f0/0x1f4/0x1f8 (attach-to-core impulse/penetration/maxDist [A]) | exe-cloth-runtime §3.4; shim-cloth row 34 |
| 10 | Cloth shape flags NX_SF_CLOTH_DISABLE_COLLISION / NX_SF_CLOTH_DRAIN | not set by any shape builder (only the 0x120008 default) [V]; a later setFlag path not ruled out [A] | never read; cloth uses fluid gates instead | **M** [A] (likely unreachable) | no setFlag callers for shape bits after creation (exe-open §1) [A] | unknown (exe-scene open q. 1) | exe-scene §0; shim-scene gaps 3, 5 |
| 11 | Cloth uses fluid-only gates (FLUID_DISABLE_COLLISION, DISABLE_RESPONSE, actor fluid bit, RB-compartment rule) | the game never sets those shape bits at creation [V]; one RIGIDBODY compartment exists [V] | applied to cloth | **C** [A] while no shape carries them |  | none | shim-scene gap 5; exe-scene §3 |
| 12 | Fluid vs moving bodies timing | as 2 | bodies read one frame late (2 cm at 1 m/s) | **C/W** small |  | none | shim-scene gap 6 |
| 13 | Sweeps from velocities, rotation held during the frame (cloth), kinematic velocity assumed | engine uses kinematic bodies [V] | as described | **C/W** [A] |  | none | shim-scene gaps 7, 8 |
| 14 | Convex shapes vs fluid/cloth | engine builds convex shapes (B4 §1) | tested as one-sided triangles, not hull planes | **C/W** (particles inside a hull) |  | none | shim-scene gap 10 |
| 15 | NxFluidDesc.staticFriction (static 0.05, dynamic 0.5, constants) | set as constants [V] | ignored (no stick term) | **C** (the 2.8.0 core reportedly ignores it too [A]) |  | none | exe-fluid §2; shim-fluid rows 42/50 |
| 16 | NxFluidDesc friction naming | "FrictionWithStaticShapes" goes to dynamicFriction (0x25) [V] | read as dynamic friction | **-** (behaves as the original) |  | UPS+0x4c | exe-fluid §2 |
| 17 | NxFluid::addParticles | per tick from sprite and mesh spawn, appendIds = false [V] | 4096 cap per call (core: per step [A]); reserve ignored; no bounds check on the write buffer | **C** unless a spawn exceeds 4096 per tick |  | UParticleModuleTypeDataPhysX spawn | exe-fluid §3; shim-fluid gaps 4, 8 |
| 18 | NxFluid::updateParticles | NX_FORCE with delete flags + id buffer; NX_ACCELERATION records; wrapper replays queued forces as NX_VELOCITY_CHANGE [V] | all three implemented, delete by id | **-** |  | UE3 kill / force producers (not traced) | exe-fluid §3; shim-fluid row 17 |
| 19 | NxFluid::removeAllParticles, getWorldBounds, getParticleDeletionIdWriteData, releaseFluid | called [V] | implemented | **-** |  | | |
| 20 | NxFluidDesc fields from the asset (maxParticles, reserve + PRIORITY_MODE, restParticlesPerMeter = 1/(dist*0.02), restDensity, kernel/motion/collision multipliers, packetSize enum 4..128, stiffness, viscosity, damping, externalAcceleration, restitution S/D, dynFriction S/D, simulationMethod SPH/NOINT/MIXED, collisionMethod STATIC/DYNAMIC, DISABLE_GRAVITY, COLLISION_TWOWAY) | set [V] | all read and used | **-** |  | UPS+0x3c..+0x88, flags byte UPS+0x54, enums +0x6c/+0x6d | exe-fluid §2, 2b; shim-fluid tables 2/3 |
| 21 | Fluid emitters, packets API, reserve/limit setters, property setters after create | not called [A, B3] | createEmitter returns null; setters stored only | **-** for this game |  | | exe-fluid §3; shim-fluid gaps 1-3 |
| 22 | NX_FF_HARDWARE / NX_CLF_HARDWARE / fluid & cloth compartments | only when the SDK reports PhysX hardware and the device is not CPU [V] | CPU only (2026-10-12): getHWVersion/getNbPPUs (SDK slots 31/32) report no hardware, so the game's gate (0x00f8c2b0) never sets the hardware flags or a PPU compartment; NxPhysicsSDKDesc gets NX_SDKF_NO_HARDWARE [A: core semantics]; a hardware flag or compartment still in a fluid/cloth desc is dropped before hosting or pass-through. PXSHIM_ALLOW_GPU=1 disables all three | **-** on a CPU-only setup; check on Windows with legacy GPU PhysX (not yet verified) |  | Engine.bDisablePhysXHardwareSupport, User/PhysXEnhanced | exe-cloth-create gates; exe-scene §3 |
| 23 | Scene filter: groupsMask with setFilterOps (SWAP_AND), filter bool/constants | set once at scene creation [V; op values A] | read back live from the scene; SWAP_AND as documented [A] | **-** (verify K0/K1 and the op order in disassembly) |  | RB channel: shape/cloth/fluid groupsMask bits0 = 1<<channel, bits2 = collide-with | exe-scene §2 + review; shim-scene gap 13 |
| 24 | Cloth groupsMask (bits0 = 1 << ClothData channel byte, bits2 = component collide-with dword), collisionGroup 0 | set [V] | honoured through the scene filter | **-** |  | component+0x3c8 (RBChannel) / +0x3cc (RBCollideWithChannels) [A] | exe-cloth-create desc; shim-scene row 123 |
| 25 | Cloth globalPose (transpose of the UE 4x4, translation x 0.02; or identity + recentre when ClothData+0x1ec bit 1) | set [V] | fixed in the working tree (row-major, R*r); **HEAD is wrong (column-major)** | **-** after commit |  | component LocalToWorld | exe-cloth-create review; shim-cloth "Pose matrix" |
| 26 | Cloth desc fields: thickness (x0.02), density, bending/stretching (clamped 0..1), damping, friction, tear factor, attachment tear/response, solverIterations, sleepLinearVelocity, wakeUpCounter 0.4 then putToSleep, externalAcceleration (x0.02), relativeGridSpacing | set at create and by UpdateClothParams [V] | implemented (relativeGridSpacing stored only: internal broadphase parameter) | **-** / C |  | ClothData+0x1a8..+0x200, component+0x328 | exe-cloth-create; shim-cloth table 2 |
| 27 | NX_CLF flags GRAVITY, BENDING, BENDING_ORTHO, DAMPING, COMDAMPING, SELFCOLLISION, TEARABLE, STATIC (SetClothFrozen) | set [V] | implemented (self-collision diverges after the first fold, known) | **-** (see [cloth accuracy notes](../re-notes-cloth.md)) |  | ClothData+0x1a4 / +0x1dc / +0x1fc bits, component+0x324 bit 2 | exe-cloth-create flags; shim-cloth table 3 |
| 28 | Per-frame cloth calls: attachVertexToGlobalPosition per fixed vertex; getVelocities -> addForceAtVertex(VELOCITY_CHANGE) per vertex -> setVelocities; getNumberOfParticles | called every frame [V] | implemented (setVelocities uses a read/write heuristic that fits exactly this pattern) | **-** |  | | exe-cloth-runtime §3.4; shim-cloth rows 28, 50, 82 |
| 29 | addDirectedForceAtPos (AddImpulseAtClothPos), attachToCollidingShapes, attachVertexToShape / freeVertex (SetAttachClothVertsToBaseBody, shape attachment table), putToSleep / wakeUp, setMeshData (deferred release), releaseCloth | called [V] | implemented / partial (attach queued one step; getVertexAttachmentShape returns NULL for colliding-shape attachments) | **C** |  | natives in natives.tsv | exe-cloth-runtime §3.1-3.3; shim-cloth gaps 6, 7 |
| 30 | Mesh cook: TEARABLE mesh flag, no vertex masses/flags, welding 0 (UE3 welds itself) | [V] | only TEARABLE read; masses/welding ignored | **-** |  | ClothData+0x1fc bit 0 | exe-cloth-create mesh cook |
| 31 | Hosted objects missing from getNbCloths/getCloths/getNbFluids/getFluids/user notify | no game call found [A] (the wrapper keeps its own lists) | not hooked | **C** |  | | shim-cloth gap 8; shim-fluid gap 7 |
| 32 | setMinAdhereVelocity / NX_CLF_ADHERE | wrapper never replays it; flag never set [V] | stored only (logs "unsupported" once per cloth) | **C** (silence the log) |  | | exe-cloth-runtime §1.4 |
| 33 | Stubs: queryShapePointers, getShapePointers, getStateByteSize, saveToDesc, overlapAABBTriangles, raycast callers | no engine caller found [A] (raycast is implemented in the shim) | stub / implemented | **C** |  | | shim-cloth gap 10; exe-cloth-runtime §3.6 |

## Session 2026-10-12 (day): steps 2, 4 and 5 (not committed)
Property names: `ue3-prop-map.md` (ClothData = the USkeletalMesh itself, comp+0x1c8 = SkeletalMesh; UPS +0x3c..+0x93).
Content: `content-pkgs.md`, `content-maps0/1/2.md` (all 2424 packages parsed). **tools/ue3pkg.py export_props mis-decodes
component exports** (UComponent prefix: TemplateOwnerClass, plus TemplateName on templates); the scans used their own
decoder (scratch `content-pkgs/scan.py: decode()`), so `physx_asset_inventory.py` under-counts cloth components.
Exe loose ends: `exe-open.md` (filter ops OR/OR/SWAP_AND, K0 = K1 = 0 [V]; setTiming every advance FIXED [V]; setGravity
every tick; no post-create shape setFlag found [A]; kinematic skinned bodies use setGlobalPose).
Coverage counters: `shim/coverage.{h,cpp}`, one or more per row, logged as `coverage (...)` lines at each releaseScene
(per level) and at detach (totals).

Fixes (each regression-checked: bit-identical on every scenario where not expected to change):
| row | fix | check |
|---|---|---|
| 1 | NxFluidDesc.collisionResponseCoefficient reaches the solver (two-way impulses only) | 42 fluid scenes identical; new probe `dyn_twoway_float_coef05`: body velocity -0.14914 = oracle (HEAD -0.05966) |
| 2 | cloth friction relative to the contact body's point velocity (lin + ang x (x - com), 0x100e5e80/0x100e2d40 [V]); dynamic bodies tested without CCD count as at rest [A, fitted] | 54 static scenarios identical; vs oracle: dyn_sweep_v20 0.945 -> 9.7e-5, dyn_capsule_sweep 0.268 -> 0.053, dyn_sphere_push 0.319 -> 0.265, v20n unchanged 8e-5 |
| 3 | statics re-collected when the cloth comes within 1 m of its statics box (teleport, falling, travelling); attachments to statics follow their shape; torn cloths keep theirs | all 59 identical (self_jitter* re-query and stay identical) |
| 6 | body slots keyed on the NxShape, not scene_read's geometry pointer (no new slot per geometry rebuild) | all 59 identical |
| 5 | NX_CLF_DISABLE_COLLISION as a live solver flag (no contacts, shapes and body poses kept for attachments); turning it on/off no longer rebuilds | native 59 identical; final shim build: cloth 59/59 and fluid 39/39 identical to the previous step |
Not done: row 4 (valid bounds; RE below), rows 7-9 (no content), 10/11 (no caller found).
Row 4 RE [V 2.8.0]: bounds = component origin + ValidBoundsMin/Max (FUN_00d4b6f0 in the exe); a particle outside gets 0x400
(0x100e7820) and from then on: no prediction (frozen, v = 0), no contacts, edges touching it disabled by the split fix-up
(0x100e13f0), left out of the cloth bounds (0x100e23b0) and of COM damping. Implement only if the sweep's
`cloth_outside_validbounds` is non-zero.

## Original follow-up plan (superseded by the findings below)
**Step 2 (content scan of the 679 maps)**: for every cloth (USkeletalMesh with cloth data) and every UPhysXParticleSystem,
dump the properties behind these join keys and count how many assets turn on a W/M path:
ClothData+0x1dc bits 2 and 3 (pressure, two-way) with their values (+0x1c8, +0x1cc); component+0x324 bits 0/1 and
+0x314 & 0x200 with ClothData+0x1f0..0x1f8 (attachToCore); valid bounds settings; UPS+0x84 (fluid collision response)
values != 0.2; Kismet use of SetClothValidBounds/EnableClothValidBounds/SetClothFrozen/SetClothFlags/SetClothPosition/
ResetClothVertsToRefPose/AttachClothToCollidingShapes. First map the offsets to property names (class property order in
the packages; the `[A]` names in the table are guesses).
**Step 4 (coverage counters)**: one counter per row above in the shim (flag seen, setter called with a non-default value,
stub hit, attachToCore called, live DISABLE_COLLISION toggle, statics re-query needed, slot churn count), dumped at exit;
run a sweep over levels.
**Step 5 (implementation), likely order**: fluid collisionResponseCoefficient (1, small); cloth contact shape velocity (2);
statics re-query on teleport/large motion (3); stable dynamic slot identity (6); live DISABLE_COLLISION (5); valid bounds (4);
then, only if step 2 finds content for them: pressure (7), two-way cloth contacts (8), attachToCore (9).
**Still open on the exe side**: later shape setFlag paths (exe-scene open q. 1), setTiming/setGravity callers, the
moveGlobalPose per-frame path, setFilterOps argument order in disassembly, the fluid kill/force producers.

## Session 2026-10-12 (evening): step 3 done, row 4 RE complete (not implemented, not committed)
**tools/ue3pkg.py fixed [V]**: UComponent exports carry TemplateOwnerClass (4 bytes) before NetIndex, plus TemplateName
(8) when an outer is a class default object; a component CDO itself (flag 0x200) has neither. All 804,101 component
exports in the 2424 packages decode (0 failures, every property type known). `physx_asset_inventory.py` now resolves the
archetype chain (own tags, then archetypes across packages, then the class default) for (Td)SkeletalMeshComponents and marks
placed (under PersistentLevel) versus template: cloth components 0 -> 263; fluids/emitters/meshes unchanged (155/2010/219).
**maps0 vs maps2 reconciled [V]**: 212 placed cloth components in 28 maps, **none** with bDisableClothCollision in effect;
the flag sits only on 44 template copies of the P_Buildings flag prefabs (40 cooked into 32 maps), referenced only by their
own archetype actors (no placed actor, no Kismet object points at them). maps2 counted the copies. Row 5 is not reached by
map content (fix stays, harmless). bEnableValidBounds: 25 placed, 3 maps (unchanged).
**Row 4 is live at run time [V]**: traced run 20261011-010124 (Escape_p) shows setFlags turning on 0x8000 for most cloths
(66 transitions) although no Escape component tags bEnableValidBounds; class default ValidBounds = +-500 UU (+-10 m after
x0.02) around the component origin. So the original removes any particle more than 10 m from its component's origin
(e.g. a torn piece falling off a roof). Sweep counter `cloth_outside_validbounds` will say how often.
**Row 4 RE, complete [V 2.8.0]** (spec for the implementation):
- Test in 0x100e7820, after friction, before tearing (IntegrateTimeStep 0x100f0b50): flag 0x8000 set and x strictly outside
  (x < min or x > max per axis) -> particle flag 0x400, sticky (nothing clears it). Default bounds from the constructor
  (0x100e39d0): empty box (min +FLT_MAX, max -FLT_MAX), i.e. flag on without setValidBounds removes everything.
- Removed particle: prediction p* = x (0x100f0780), so v = 0 from the next step; skipped by contacts (0x100e17b0), the
  collision-plane projection, attachment projection (0x100e0d90), COM damping (all 3 loops, 0x100df6e0), edge damping
  (0x100df490 skips edges with an end flagged 0x402), friction, self-collision, cloth bounds (0x100e23b0) and tearing
  (0x100edb90 skips edges with a removed end).
- Fix-up 0x100e13f0 (runs when the removed count changed; also once on the first step, dirty byte set by the constructor):
  stretch of edge (i,j) off if i or j removed or both opposite vertices removed/absent; bending off if any of i,j,k,l
  removed or l absent; particles referenced by no active constraint get 0x400; triangles touching a removed particle
  are collapsed to (a,a,a) in the output index buffer (+ dirty flag). Equivalent for us: rebuild edges from the
  triangles whose three vertices are alive; a vertex in no alive triangle is removed; masses unchanged.
- Only difference of that equivalence: an edge with both ends alive but both triangles dead still damps in the original.
- Implementation plan (solver): ClothParams useValidBounds + lo/hi (recorded, appended), removed_ mask carried across
  shim rebuilds (new rec op), filters in prediction / contacts / pins+attachProject / COM damping / wind normals /
  self-collision prepass / selfContactFind (dead triangle = degenerate) / mesh sweep / tearStep; bounds test before
  tearStep; fix-up after it (full rebuildEdges when a split happened on a cloth with removed particles). Shim: collapse
  dead triangles in writeResults (normals from the collapsed list), exclude removed from getWorldBounds/boundsAtStep.
  Probe: cloth_oracle `validbounds` keyword (check whether it exists), a falling sheet crossing a bound.

## Session 2026-10-12 (late): row 4 implemented (not committed)
| row | fix | check |
|---|---|---|
| 4 | NX_CLF_VALIDBOUNDS removal per the evening spec: `Cloth::validBoundsStep` (strictly outside -> removed, sticky; this step's v and friction kept, prediction p* = x from the next step) before tearStep; `removedFixup` rebuilds edges from alive triangles, orphans removed, masses kept, `tri_` intact; filters in prediction, contacts, pins/attachProject, COM damping, wind normals, self contact, mesh sweep, tearing. Params appended to the recording, new rec op `kRecRemoved`. Shim: setValidBounds/setFlags live, applyDeferredParams also carries disableCollision + bounds, mask kept across rebuilds, dead triangles written as (a,a,a) with normals from that list, removed particles out of getWorldBounds/boundsAtStep, `cloth_outside_validbounds` counts removals | byte-exact base vs new through the shim: cloth 59/59, fluid 39/39 identical (3 bench scenes do not parse in the harness, both sides; docs/audit/row4-regress.md). New probes vb_fall / vb_swing (docs/audit/row4-probe.md), ours vs oracle, native and shim (REF_HZ=0): same freeze step for all 121 particles, index buffers identical every step (collapses 29..38 x20 / 7,8,9,11,14), max pos diff 5.7e-7, vel 2.1e-5 [V] |

Known differences left [A, not probed]: an edge with both ends alive but both triangles dead stops damping (the original keeps it);
rebuilt edges take their rest values from the rest pose, not the cooked stream; tear arcs still count dead triangles.
Harness: cloth_oracle and run_scenario take `bounds x0 y0 z0 x1 y1 z1` (with `flag validbounds 1`).

## Session 2026-10-12 (late 2): CPU only on every system (not committed)
The game asks getHWVersion only in its hardware gate 0x00f8c2b0 (callers: scene creator 0x00ac4e30, fluid 0x00e86690,
cloth 0x00d4ba70) [V: call graph]; PhysXEnhanced (the extra effects) does not go through it, and this CPU-only machine
already runs the full effects with PhysXEnhanced=True. Hosting never depended on the core build (only the cooked-mesh
hook needs the v2.8.0/v2.8.1 path, and the loader always loads v2.8.0 for this game). Checks: cloth 59/59 and fluid
39/39 byte-identical to the committed build; a cloth created with NX_CLF_HARDWARE through the shim is logged, stripped
and identical to the same cloth without the flag (harness flag `hardware`). Open: confirm in game that the shim log shows
"cpu only: NxPhysicsSDKDesc.flags" and on Windows with an NVIDIA GPU and legacy PhysX installed.
