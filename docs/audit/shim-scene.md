# Audit A4: scene state honoured by the hosted cloth and fluid solvers

Scope: what `shim/scene_read.cpp` and its callers (`shim/cloth_ours.cpp`, `shim/fluid_ours.cpp`) read from the core's
NxScene, and what the solvers (`solver/cloth/cloth.cpp`, `solver/fluid/fluid.cpp`) do with it. Static audit only: the game
was not run and no experiment was made. Every claim is marked [V] (seen in code or docs; file:line given) or [A] (assumed,
not verified).

Status words: honoured / partial / ignored / n.a. (no bearing on the solver or not in the original either).

"Original" cites the RE notes (`docs/re-notes-*.md`) and the PhysX 2.8.1 headers (`third_party/physx-sdk-2.8.1/...`, values
for 2.8.0 are the same bits [A], except where a decompile mask is cited).

---

## 0. Summary

- Shapes: planes, spheres, boxes, capsules (static and dynamic), triangle meshes and heightfields are read by both solvers.
  Wheels and dynamic planes are dropped. Convex hulls are read as triangles, which is not the original's model.
- Shape flags: the fluid path honours the fluid bits (drain, fluid-disable, fluid two-way, trigger, disable, disable-response).
  The cloth path reads none of the cloth bits (`NX_SF_CLOTH_*`), but reuses the fluid gate.
- Filtering: the scene group table and the shape/fluid group and mask are honoured. The scene filter ops and the static
  masks are re-read only on a rescan (up to 600 refreshes stale).
- Cloth statics are selected once, at the cloth's first build. They never follow the cloth or the scene afterwards.
- Motion: sweeps are rebuilt from velocities, not from the original's start and end poses. Fluid runs one frame behind bodies.

---

## 1. Shape types

| Item | CLOTH | FLUID | Original behaviour | Ours (file:line) |
|---|---|---|---|---|
| Plane, static | honoured | honoured | fluid plane test [V] re-notes-fluid.md:132-133 | scene_read.cpp:278-284; cloth_ours.cpp:400-403 |
| Plane, dynamic | ignored | ignored | n.a. [A] (SDK planes are static in practice) | scene_read.cpp:475 (localGeometry returns null for planes) |
| Sphere (static, dynamic) | honoured | honoured | [V] re-notes-fluid.md:131 | scene_read.cpp:285-289; cloth_ours.cpp:404-406, 727-728 |
| Box, axis-aligned | honoured | honoured | [V] re-notes-fluid.md:244-249 (frame rule for all types) | scene_read.cpp:290-296; cloth_ours.cpp:407-409 |
| Box, rotated (oriented) | honoured | honoured | [V] re-notes-fluid.md:244-249 | scene_read.cpp:297-300; cloth_ours.cpp:539-544 (posed) |
| Capsule (upright / tilted) | honoured | honoured | fluid case 3 [V] re-notes-fluid.md:241 | scene_read.cpp:303-315 |
| Wheel | ignored | ignored | not documented [A] | scene_read.cpp:327-328 (unsupported; census line 653-657) |
| Convex | partial | partial | fluid: hull planes, one-sided by construction [V] re-notes-fluid-mesh.md:151-157; cloth case 5 [V] re-notes-cloth.md:155 | scene_read.cpp:316-321, 465-473 (triangles of the convex mesh); cloth_ours.cpp:413-419 |
| Triangle mesh | partial (two-sided, see gaps) | honoured (one-sided, as original) | fluid one-sided [V] re-notes-fluid-mesh.md:112-114; cloth one-sided [V] re-notes-cloth.md:586-588 | scene_read.cpp:158-185, 316-321; fluid.cpp:339 (front test); cloth.cpp:270-278 (two-sided) |
| Heightfield | partial | honoured | case 8 [V] re-notes-fluid-mesh.md:158; SDK format S16_TM only | scene_read.cpp:207-270 (format check 210-213), 322-326; fluid via triangles |
| Mesh flag NX_MESH_DOUBLE_SIDED (1<<1) | ignored | ignored | "no per-mesh double-sided flag read" [V] re-notes-fluid-mesh.md:112-114 | not read |
| Mesh flag NX_MESH_SMOOTH_SPHERE_COLLISIONS (1<<0) | ignored | ignored | not documented [A] | not read |
| Mesh flip normals (NX_MF_FLIPNORMALS, cooking) | ignored | ignored | original honours it [V] re-notes-cloth.md:597-599 | not visible to the shim (cloth.cpp:276-278) |
| Shape skin width | n.a. | n.a. | no evidence [A] | not read |
| Pose: global | honoured | honoured | - | scene_read.cpp:622 (statics, rescan only), 689 (dynamic actors, every refresh), 714 |
| Pose: local (dynamic shapes) | partial | partial | - | scene_read.cpp:539 (cached in buildDynActor; rebuilt 120 refreshes, 701-703) |
| Scale (heightfield height/row/column) | honoured | honoured | - | scene_read.cpp:216-218, 261-263 (cache key) |

---

## 2. Shape flags (NX_SF_*, bit values from 2.8.1 `Nxp.h` lines 414-508)

Bits from 2.8.1 headers. For 2.8.0 the bits are the same where cited: fluid bits 4, 8, 10, 12 from the 2.8.0 decompile
masks [V] re-notes-fluid-drain.md:10-41; cloth drain 0x8000 [V] re-notes-cloth.md:163; cloth dynamic CCD 0x20000 [V]
re-notes-cloth.md:530. Others [A].

| Flag (bit) | CLOTH | FLUID | Original behaviour | Ours (file:line) |
|---|---|---|---|---|
| TRIGGER_ON_ENTER/LEAVE/STAY (bits 0-2, ENABLE = 7) | honoured (any bit excludes) | honoured | cloth: triggers filtered [V] re-notes-cloth.md:152; fluid: no collision [V] re-notes-fluid-drain.md:28-32 | fluid.h:172, 182; scene_read.cpp:53, 483-484 |
| NX_SF_VISUALIZATION (1<<3) | n.a. | n.a. | no collision effect [A] | not read |
| NX_SF_DISABLE_COLLISION (1<<4) | honoured (shared gate) | honoured | fluid pass-through [V] re-notes-fluid-drain.md:39 | fluid.h:168, 182; scene_read.cpp:485-487 |
| NX_SF_FEATURE_INDICES (1<<5) | n.a. | n.a. | rigid-body report | not read |
| NX_SF_DISABLE_RAYCASTING (1<<6) | n.a. | n.a. | queries only | not read |
| NX_SF_POINT_CONTACT_FORCE (1<<7) | n.a. | n.a. | rigid-body report | not read |
| NX_SF_FLUID_DRAIN (1<<8) | n.a. (cloth uses bit 15) | honoured | drain on proximity or contact [V] re-notes-fluid-drain.md:10-26 | fluid.h:169; scene_read.cpp:515, 543; fluid.cpp:1601, 1640 |
| NX_SF_FLUID_DISABLE_COLLISION (1<<10) | gated by shared rule [A: original cloth may not gate on it] | honoured | pass-through [V] re-notes-fluid-drain.md:40 | fluid.h:170, 182; scene_read.cpp:486 |
| NX_SF_FLUID_TWOWAY (1<<11) | n.a. | honoured (dynamic, non-kinematic only) | selects body reaction, not a gate [V] re-notes-fluid-drain.md:58-59 | scene_read.cpp:422, 545, 716; fluid.cpp:1621 |
| NX_SF_DISABLE_RESPONSE (1<<12) | gated by shared rule [A] | honoured (pair gate) | pass-through, also gates the pair [V] re-notes-fluid-drain.md:41 | fluid.h:171, 182; scene_read.cpp:486 |
| NX_SF_DYNAMIC_DYNAMIC_CCD (1<<13) | n.a. | n.a. | rigid-body CCD | not read |
| NX_SF_DISABLE_SCENE_QUERIES (1<<14) | n.a. | n.a. | queries only | not read |
| NX_SF_CLOTH_DRAIN (1<<15) | ignored | n.a. | removes touching particles [V] re-notes-cloth.md:163 | not read (scene_read.cpp:485-487); no drain in solver/cloth (grep) |
| NX_SF_CLOTH_DISABLE_COLLISION (1<<16) | ignored | n.a. | excluded from cloth contact [V] re-notes-cloth.md:152 | not read |
| NX_SF_CLOTH_TWOWAY (1<<17) | ignored | n.a. | with NX_CLF_COLLISION_TWOWAY, impulse to body [V] re-notes-cloth.md:111-113 | not read; cloth collision two-way not implemented [V] re-notes-cloth.md:550, logged at cloth_ours.cpp:1730 |
| NX_SF_SOFTBODY_DRAIN / _DISABLE_COLLISION / _TWOWAY (1<<18, 19, 20) | n.a. | n.a. | no soft-body solver | not read |

---

## 3. Actor flags (NX_AF_*) and body flags (NX_BF_*)

| Flag | CLOTH | FLUID | Original behaviour | Ours (file:line) |
|---|---|---|---|---|
| NX_AF_DISABLE_COLLISION (1<<0) | gated by shared rule [A] | honoured | pass-through [V] re-notes-fluid-drain.md:42 | fluid.h:173, 184; scene_read.cpp:494, 704 (near dynamic), 507-513 (static, via readStaticFlags) |
| NX_AF_DISABLE_RESPONSE (1<<1) | gated by shared rule [A] | honoured | pass-through [V] re-notes-fluid-drain.md:43 | fluid.h:174, 184; scene_read.cpp:494 |
| NX_AF_LOCK_COM (1<<2) | n.a. | n.a. | rigid-body | not read |
| NX_AF_FLUID_DISABLE_COLLISION (1<<3) | gated by shared rule [A] | honoured | pass-through [V] re-notes-fluid-drain.md:44 | fluid.h:175, 184; scene_read.cpp:494 |
| NX_AF_CONTACT_MODIFICATION (1<<4), FORCE_CONE_FRICTION (1<<5), USER_ACTOR_PAIR_FILTERING (1<<6) | ignored | ignored | user callbacks; not hooked [A] | not read |
| NX_BF_KINEMATIC (1<<7) | honoured (no `dynamic`, no two-way) | honoured (no two-way) | kinematic is a body, not static [A on details] | scene_read.cpp:709, 716; cloth_ours.cpp:700, 737-738 |
| NX_BF_DISABLE_GRAVITY (1<<0) | n.a. | n.a. | motion integration only | not read |
| NX_BF_FROZEN_* (1<<1..6) | ignored | ignored | velocities used as read [A] | not read |
| NX_BF_VISUALIZATION, FILTER_SLEEP_VEL, ENERGY_SLEEP_TEST (1<<8, 10, 11) | n.a. | n.a. | - | not read |
| Sleeping state (`isSleeping`) of bodies | ignored | ignored | awake or asleep, the body still collides [A] | not read in scene_read.cpp |
| Static vs dynamic (`isDynamic`, slot 29) | honoured | honoured | - | scene_read.cpp:573-590 (classified on first sighting, 578) |

---

## 4. Static, dynamic, kinematic; velocity; previous pose

| Item | CLOTH | FLUID | Original behaviour | Ours (file:line) |
|---|---|---|---|---|
| Static actors | honoured (selected once at first build) | honoured (rescan on set change) | - | scene_read.cpp:611-651; cloth_ours.cpp:510-550 |
| Dynamic actors, near only | honoured (each simulate) | honoured (each refresh) | - | scene_read.cpp:678-727; cloth_ours.cpp:688-705 |
| Kinematic actors | honoured as shape with zero inverse mass, two-way off | honoured, two-way off | kinematic: current pose used for contacts [A] re-notes-cloth.md:534-537 | scene_read.cpp:709, 716; cloth_ours.cpp:737 |
| Linear velocity of body | honoured (`lin`, from getLinearVelocity) | honoured | - | scene_read.cpp:706; cloth_ours.cpp:740 |
| Angular velocity of body | ignored | honoured (surface velocity lin + w x (x - com)) | surface velocity lin + w x r [V] re-notes-fluid.md:135-136 | scene_read.cpp:707; fluid.h:155, 161; fluid.cpp:854 |
| Centre of mass | n.a. (cloth uses the geometry centre) | honoured | - | scene_read.cpp:708; fluid.cpp:854 |
| Previous pose for the sweep | reconstructed: linear extrapolation with acceleration from consecutive velocities; first sighting assumes gravity [A]; rotation held at the current pose | reconstructed: current pose integrated back with lin and ang velocity (semi-implicit Euler) [A]; first sighting: no sweep | original keeps the previous pose per shape [V] re-notes-cloth.md:529-532; fluid takes start and end poses [V] re-notes-fluid.md:100-102 | cloth_ours.cpp:741-760; scene_read.cpp:434-440, 721, 774-778 |
| Surface velocity used in contacts | ignored: friction and normal clamp are against world velocity, not v - v_s | honoured (fluid.cpp:640-655, 1545-1547, 1617) | v = v_s + n max(v_n,0) + (1-f) v_t [V] re-notes-cloth.md:107-109 | cloth.cpp:1622-1628 (Contact has no v_s, cloth.h:230) |
| Frame lag, bodies vs solver | n.a. (cloth steps after bodies' pose at simulate) [A] | one frame behind bodies by design [A]; dyn_kin 2 cm off [V, historical oracle probe] | original steps bodies and fluid together [A] | scene_read.h:39-41 |

---

## 5. Collision filtering

| Item | CLOTH | FLUID | Original behaviour | Ours (file:line) |
|---|---|---|---|---|
| Scene group table (NxScene::getGroupCollisionFlag, 32x32) | honoured (per refresh, 1024 reads) | honoured (passed to solver each step) | "fluid group applies to all statics", groups pair-gated [V] re-notes-fluid-drain.md:49-51 | scene_read.cpp:659-661, 737-744, 752-756, 770; fluid_ours.cpp:711-713; cloth_ours.cpp:530, 693 |
| Shape group (NxShape::getGroup) | honoured (statics: rescan and every 30 refreshes; dynamic: cached at build) | honoured (same) | - | scene_read.cpp:510 (statics, 664-672 recheck), 540 (dynamic, cached) |
| Fluid / cloth own collisionGroup | honoured live (fluid); live setter, statics frozen after build (cloth) | honoured live (fluid_ours.cpp:438-441) | - | cloth_ours.cpp:1244-1247, 1700, 492-494; fluid_ours.cpp:566, 441 |
| Own groupsMask (fluid and cloth) | applied through the scene filter | applied through the scene filter (default on) [A] | cloth: group mask filtered [V] re-notes-cloth.md:152; fluid: no effect for bits0 under the default filter [V] re-notes-fluid-drain.md:52-53, not tested under other filter ops [A] | cloth_ours.cpp:495-496; fluid_ours.cpp:447, 567-570 (default on); scene_read.h:22 comment says false (stale) |
| Shape groupsMask (NxShape::getGroupsMask) | partial: statics read only at rescan (up to 600 refreshes); dynamic cached at build | partial (same) | - | scene_read.cpp:541, 634 (static rescan only) |
| Scene filter (ops, bool, K0, K1) | partial: read at rescan only (up to 600 refreshes) | partial (same) | equation "(G0 op0 K0) op2 (G1 op1 K1) == b" [V] NxScene.h (setFilterOps doc, Nxp.h 2060-2068); (X != 0) == b matches scene_read.cpp:149-153 | scene_read.cpp:337-343 (read), 609 (rescan only), 605 (every 600), 757, 771 |
| NX_FILTEROP_SWAP_AND (Ubisoft op, undocumented) | guessed [A] | guessed [A] | UE3 may use it | scene_read.cpp:143 (swap of 64-bit halves) |
| Dominance groups (setDominanceGroupPair) | n.a. | n.a. | rigid-body pairs only [A] | not read |
| Rigid-body compartment, static actor | honoured (statics) [A for cloth] | honoured [V] re-notes-fluid-drain.md:54-56 | invisible to the fluid [V]; mechanism not located [A] | scene_read.cpp:500-503, 509, 513 |
| Rigid-body compartment, dynamic actor | not excluded [A] | not excluded [A] | unknown | scene_read.cpp:544 (comment says the rule does not apply to dynamics) |
| Fluid compartment (NX_SCT_FLUID) | n.a. | n.a. | no effect [V] re-notes-fluid-api.md:19-21 | fluid_ours.cpp:482 (returned, not used) |
| Trigger / disabled shapes, cloth | shared gate, see section 2 | - | - | cloth_ours.cpp:513-530, 690-705 |

---

## 6. Materials, CCD, user data

| Item | CLOTH | FLUID | Original behaviour | Ours (file:line) |
|---|---|---|---|---|
| Shape material (NxMaterial via NxShape) | not read | not read | fluid uses its own desc floats [V] re-notes-fluid.md:86 | none (no material call in scene_read.cpp) |
| Restitution | cloth: none in solver (no restitution term) | honoured: restitutionForStatic/DynamicShapes from desc | fluid desc floats [V] re-notes-fluid.md:86 | fluid_ours.cpp:114, 116; fluid.cpp:662-667, 1547, 1617 |
| Friction | cloth: desc friction (cloth_ours.cpp:989, 1042), but relative to world velocity (see section 4) | honoured: dynamic friction from desc; static friction mapped to dynamicFrictionForStaticShapes | 2.8.0 has no static friction [V] re-notes-fluid.md:224-227 | fluid_ours.cpp:115, 117; fluid.cpp:884-887 (floor 0.001, [A, fitted]), 1656 |
| Attraction (attractionFor*Shapes) | n.a. | n.a. | 2.8.0 has none [V] re-notes-fluid.md:224-226 | not read |
| collisionResponseCoefficient | stored, unused (two-way off) | honoured (two-way impulses) | [V] re-notes-cloth.md:550 (cloth two-way not implemented) | cloth_ours.cpp:105-107, 1379-1383; fluid.cpp:1582 |
| CCD: NX_CLF_DISABLE_DYNAMIC_CCD | honoured (sweep on/off) | n.a. (fluid always sweeps) | [V] re-notes-cloth.md:529-531 | cloth_ours.cpp:327; cloth.cpp:562, 720 |
| NX_SF_DYNAMIC_DYNAMIC_CCD | n.a. | n.a. | rigid-body | not read |
| User data (shape, actor) | n.a. | n.a. | - | not read; cloth keeps NxCloth userData only (cloth_ours.cpp:152, 1702) |

---

## 7. Snapshot and caching (how often each item is re-read)

Scene geometry is refreshed once per simulate, on the game thread, before the core's simulate
(fluid: fluid_ours.cpp:858; cloth: cloth_ours.cpp:1925). Fluid and cloth each keep their own SceneGeometry per scene
(fluid_ours.cpp:623-627; cloth_ours.cpp:484-487), so the scene is read twice per frame when both are present [V].

| Data | Re-read | Where |
|---|---|---|
| Actor list, dynamic/static kind | every refresh; kind fixed at first sighting | scene_read.cpp:569-600 (578) |
| Group table 32x32 | every refresh | scene_read.cpp:659-661 |
| Static shape set, pose, mask, type | rescan when static actor set changes, every 600 refreshes (605), census | scene_read.cpp:604-651 |
| Static flags (shape bits, actor bits, RB compartment, group) | rescan, plus every 30 refreshes | scene_read.cpp:507-517, 664-672 (kStaticFlagRecheck, line 58) |
| Scene filter | rescan only (up to 600 refreshes) | scene_read.cpp:609 |
| Shape added to an existing static actor | not detected (signature counts actors, not shapes) | scene_read.cpp:584-587 |
| Dynamic actor pose, near test | every refresh | scene_read.cpp:689-699 |
| Dynamic actor velocities, COM, kinematic flag, actor bits | every refresh while near a fluid or cloth box | scene_read.cpp:704-709 |
| Dynamic shape list, local pose, shape flags, group, mask, geometry | at first sighting, on shape count or pointer change while near, or every 120 refreshes while near | scene_read.cpp:528-553, 680-681, 701-703 |
| Cloth static shapes | once, at first build (box from the particles then, + 2 m + diagonal) | cloth_ours.cpp:509-530, 561-562 |
| Cloth dynamic shapes | every simulate (near the cloth's box) | cloth_ours.cpp:688-705, 711-767 |
| Cloth group, mask | live setter; dynamic filter per step; statics frozen | cloth_ours.cpp:1244-1258, 492-498 |
| Fluid collision method, group, mask | live setter, joins the fluid thread first | fluid_ours.cpp:387-392, 438-448 |
| Fluid group table into solver | every substep | fluid_ours.cpp:711-713 |

Scene-level setters (setGroupCollisionFlag, setFilter*, actor and shape flag setters, setGlobalPose) are not hooked
[V]: the only hooks are the NxScene simulate/fetch slots and create/release for fluids and cloths
(PhysXLoader.cpp:177-231). Scene and actor changes are picked up by polling only (see the table above).

---

## Gaps vs original (ranked, wrong behaviour first)

Severity: H = likely visible in game, M = plausible, L = minor or unlikely in game. Game usage of these flags is not
known from the docs; where it matters it is said.

1. **H, cloth: static shapes are frozen at the first build.** Statics are selected in a box around the particles at that
   time (+2 m + diagonal), and `collectStatics` runs once (`staticsDone`). Setting positions (teleport, respawn) rebuilds
   the solver but does not re-collect (cloth_ours.cpp:1289-1295, `markRebuild` only). Statics outside the box never
   collide with the cloth. The original re-queries the broadphase every step [V] re-notes-cloth.md:85-86. Static meshes
   are clipped to that same box [V] re-notes-cloth.md:607-608, so a cloth that falls or moves far keeps missing them.
   Planes are unaffected.
2. **H, cloth: contact friction and the normal clamp ignore the contact shape's velocity.** Ours uses world velocity
   (cloth.cpp:1622-1628); the contact record has no shape velocity (cloth.h:230). The original uses v - v_s [V]
   re-notes-cloth.md:107-109. Moving platforms, lifts and swinging bodies are affected. Fluid does it right (fluid.cpp:640-655).
3. **H (if the game sets them), cloth: NX_SF_CLOTH_DISABLE_COLLISION (1<<16), NX_SF_CLOTH_DRAIN (1<<15),
   NX_SF_CLOTH_TWOWAY (1<<17) are never read.** The cloth path uses the fluid bits only (scene_read.cpp:485-487).
   The original excludes cloth-disabled shapes from contacts [V] re-notes-cloth.md:152, removes particles on cloth
   drains [V] re-notes-cloth.md:163, and sends two-way impulses on NX_SF_CLOTH_TWOWAY [V] re-notes-cloth.md:112-113.
   Game usage unknown [A].
4. **M-H (code only, magnitude [A]), cloth: dynamic primitive shapes get a new key every rebuild.** buildDynActor creates a
   new geometry object for each box, sphere and capsule (scene_read.cpp:474-476) and rebuilds near actors every 120
   refreshes (scene_read.cpp:701-703). The cloth looks shapes up by that pointer (cloth_ours.cpp:719-731) and adds a new
   solver slot on a miss. The old slot is parked, not removed (cloth_ours.cpp:765-766), so slots accumulate. Attachments
   to such shapes (`findShapeGeometry`, scene_read.cpp:810-824) fail after a rebuild. Needs a measurement.
5. **M, cloth: fluid-only gates reused for cloth.** Cloth uses shapeCollidesWithFluid (fluid.h:180-185), so
   FLUID_DISABLE_COLLISION, DISABLE_RESPONSE and the actor fluid bit exclude shapes from cloth. The original cloth gate is
   described as group mask, CLOTH_DISABLE, triggers and "disabled shapes" [V] re-notes-cloth.md:152, which does not name
   the fluid bits [A]. The rigid-body compartment rule (scene_read.cpp:509, 513) is also applied to cloth without evidence [A].
6. **M, fluid: sees moving bodies one frame late.** Refresh runs before the core's simulate (scene_read.h:39-41). The
   historical dyn_kin oracle probe shows a 2 cm error on a 1 m/s kinematic box [V]. The original steps bodies and fluid together
   and uses start and end poses [V] re-notes-fluid.md:100-102. The rationale for accepting this lag is unverified [A].
7. **M, both: sweeps are rebuilt from velocities, not poses.** Fluid integrates the current pose back with lin and ang
   velocity (scene_read.cpp:434-440, 777-778). Cloth extrapolates linearly with an acceleration estimate and holds rotation
   during the frame (cloth_ours.cpp:749-760). A body seen for the first time is taken as free-falling (cloth_ours.cpp:745)
   and gets no sweep in the fluid (scene_read.cpp:774-776). Original keeps the previous pose per shape [V] re-notes-cloth.md:529-532.
8. **M [A], both: kinematic velocity assumed.** Sweeps and surface velocity use getLinearVelocity/getAngularVelocity
   (scene_read.cpp:706-707; cloth_ours.cpp:740). Kinematic bodies moved by moveGlobalPose may report zero, so no sweep and
   no surface velocity. Not checked against the oracle's `kinematic`/`move` scenarios [A] re-notes-cloth.md:534-537.
9. **M, fluid: group mask gate on by default with the scene filter.** fluid_ours.cpp:567-570 sets useGroupsMask = true.
   The drain notes say the mask has no effect on pairs under the default filter, and "not used in the pair gate [A]"
   (re-notes-fluid-drain.md:52-53). The shim keeps the mask on for UE3's own filter ops; that rationale was untested at audit time. The header
   comment (scene_read.h:22) still says false. The scene filter itself is re-read only at rescan (scene_read.cpp:605, 609),
   so filter changes lag up to 600 refreshes.
10. **M, fluid and cloth: convex shapes are tested as triangles.** The original uses the hull's planes with an inside
    test [V] re-notes-fluid-mesh.md:151-157. Ours uses the triangles with the one-sided front test (fluid.cpp:339), so
    a particle inside a convex may be missed or pushed out the wrong face (scene_read.cpp:316-321, 465-473).
11. **M, both: cached dynamic shape data.** Shape flags, groups, masks and local poses of a dynamic actor are read only
    at build (scene_read.cpp:539-545), i.e. at first sighting or every 120 refreshes while near. A DISABLE_COLLISION toggle
    on a moving shard takes up to 2 s to apply. Actor-level bits are live (scene_read.cpp:704).
12. **M, cloth: setFlags(DISABLE_COLLISION) on a live cloth is wrong.** The rebuild keeps all static slots and adds them
    to the new solver (cloth_ours.cpp:593), and syncShapes returns early, so body slots are neither moved nor parked
    (cloth_ours.cpp:714, 763-766). Body slots then sit at the solver's identity pose (cloth.h:175), i.e. the origin.
    The game's traced flags (0x1020, 0x9020, 0x9022, re-notes-cloth.md:636) never set bit 2, so likely not hit [A].
13. **M [A], both: filter op SWAP_AND guessed.** The undocumented Ubisoft op (Nxp.h:2091-2092) is implemented as a swap of
    the 64-bit halves of the second operand (scene_read.cpp:143). Wrong if UE3 uses it.
14. **L-M, both: static pose, mask, and shape-count changes lag.** Static poses and masks are re-read only at rescan
    (scene_read.cpp:626-634; rescan every 600 refreshes, 605). A shape added to an existing static actor is not seen
    (scene_read.cpp:584-587). Cloth statics also never follow static actor moves (cloth_ours.cpp:539-544, 713).
15. **L, cloth: flags with no effect.** NX_CLF_TRIANGLE_COLLISION and NX_CLF_COLLISION_TWOWAY are logged as unsupported
    (cloth_ours.cpp:1729-1730) and skipped. Game flags do not set them [V] re-notes-cloth.md:636.
16. **L, both: dynamic planes and wheels dropped.** scene_read.cpp:475, 327-328. Dynamic planes are unusual in the SDK [A].
17. **L [A], fluid: dynamic bodies in a rigid-body compartment still collide.** scene_read.cpp:544 (the compartment rule
    is applied to statics only; docs establish the rule for statics only, re-notes-fluid-drain.md:54-56).
18. **L, both: sleeping and frozen body state not read.** Sleeping bodies are treated as moving; frozen bodies use the
    velocity as reported [A].
19. **L, both (deliberate deviation): cloth meshes are two-sided.** cloth.cpp:270-278 and re-notes-cloth.md:597-602 record
    this as an improvement, because flipped UE3 meshes gave wrong fronts. Original is one-sided [V] re-notes-cloth.md:586-588.
    Fluid stays one-sided [V] re-notes-fluid-mesh.md:112-114. Document, do not change blindly.
20. **L, efficiency only:** two SceneGeometry instances per scene, and the 1024-entry group table read every refresh
    (scene_read.cpp:660-661; fluid_ours.cpp:623-627; cloth_ours.cpp:484-487).
21. **L:** heightfield formats other than S16_TM are rejected and logged (scene_read.cpp:210-213). The SDK comment says only
    S16_TM is created [A].

## Independent review (spot-checks)
- Gap 3 (cloth never reads CLOTH_DISABLE_COLLISION/CLOTH_DRAIN/CLOTH_TWOWAY): confirmed, `readShapeBits` (scene_read.cpp:480-489) reads only trigger, DISABLE_COLLISION, FLUID_DRAIN, FLUID_DISABLE_COLLISION, DISABLE_RESPONSE.
- Gap 4 (dynamic slot churn): confirmed that non-mesh/convex dynamic shapes get a fresh `make_shared` geometry on every rebuild (scene_read.cpp:474-476, rebuild every 120 refreshes at :701-703) and that cloth keys body slots by that pointer (cloth_ours.cpp:719); the old slot is parked, not reused (cloth_ours.cpp:763-766). So parked slots grow over time [V]. That attachments to the shape break is [A], not checked.
- Gap 9 (fluid groupsMask): not a gap in the code. The use of the mask is deliberate (fluid_ours.cpp:567-570, PXSHIM_FLUID_NOMASK switch); only the comment in scene_read.h:22 is stale. Downgraded to cosmetic.
