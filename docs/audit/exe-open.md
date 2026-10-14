# MirrorsEdge.exe: the five "still open" items (historical audit)

Static RE only, from the text exports in `re/out/MirrorsEdge/` (see `exe-setup.md`). Not run. [V] = checked in the
export or header; [A] = inferred. Each item: findings, addresses, impact on the shim (one line).

Status: items 1 to 5 written (1, 3 and 5 partial; see each section's [A] lines).

## 1. Later shape setFlag paths

Result: **no engine caller of NxShape::setFlag with a NX_SF_* bit was found after creation** (bounded search, below).
So DISABLE_COLLISION, CLOTH_*, FLUID_* and DRAIN bits are, as far as found, set only by the builders (default 0x120008
in B4). Treat as [A] "unreachable" until the dynamic counter (GAPS step 4) confirms it.

Slot and wrapper [V]:
- NxShape setFlag = slot 5 (0x14). Header index 5 of the 39 virtuals of NxShape.h (= RTTI slot count 39); getFlag = 6.
- The Nxd shape wrappers at slot 5 are stubs (e.g. NxdBoxShape slot 5 at 0x0147f460, NxdSphereShape 0x0147ef10, the
  same pattern for all Nxd*Shape). Each calls the common `FUN_01473c80(flag, value)` (0x01473c80) [V].
- `FUN_01473c80` keeps per-shape pending masks (wrapper +0x10 set, +0x14 clear). If the real shape exists and a
  virtual query at +0x22c (on the object at the wrapper's `*this + 0xc`) returns true, it forwards to the real shape's
  slot 5 at once; otherwise it records the bit and calls `FUN_014636f0` to queue the replay [V for the code]. The query
  is probably "scene is simulating" [A]. So setFlag from the game reaches the shim in checkResults, like the other
  setters (exe-scene note).

Search for callers (all by enclosing function; the decompile loses the receiver type, so this is bounded):
- 1,010 functions contain a `+0x14` vtable call (slot 5 of some object). Too many to classify; slot 5 is also
  NxActor::setGlobalPose, NxScene::setGravity, and many UE3 interfaces.
- Narrowed to calls with two arguments and a strong NX_SF constant (0x100, 0x400, 0x800, 0x1000, 0x4000, 0x8000,
  0x10000, 0x20000, 0x40000, 0x80000, 0x100000) in the same function: 60 functions. Checked by hand: 0x0130c010
  (constant 0x10000 in a `-(param_5 != 0) & 0x10000` expression, the only slot-5 call has a different first argument),
  0x00f73f30 (0x8000 is an argument to a different call), 0x00d9b820 / 0x00d9bd10 (0x800 tests a flags word of
  `*param_3`, and slot 0x14 there is on `in_ECX` with 4 arguments), 0x00ec48e0 (receiver is the global `DAT_020186f8`, probably not a shape [A]),
  0x00d492d0 (cloth). None is a shape setFlag [A].
- The other candidates in that list (0x00d21f00, 0x00b69ff0, 0x0118e490, 0x013e34b6 with six calls, and the 0x0146xxxx
  cluster) were not resolved; 0x013e34b6 is the one to check next [A].

Non-shape bits the game does set on its actors (verified; this is the flag path that is actually used):
- `URB_BodyInstance::execEnableCollisionResponse` (0x00afa740) -> `FUN_00bd4fe0` (0x00bd4fe0): with the body's NxActor
  at body+0x60, reads actor flag 2 (readActorFlag, slot 27), then **raiseActorFlag (slot 25, 0x64) with NX_AF_DISABLE_RESPONSE
  (2)** when response is turned off, clearActorFlag (slot 26, 0x68) when it is turned on [V]. It also sets the body
  bit 0x80 of +0x6c. This is the actor-level DISABLE_RESPONSE that B4 found on static actors (0x00ea5320).
- `UPrimitiveComponent::execSetActorCollision` (0x00d32520): writes bit 0x8 of the component word +0x10c and calls
  `FUN_00c169a0` only when bit 0x4 changes (it requests a re-add of the body) [V]. No shape flag and no setFlag call.
- `UPrimitiveComponent::execSetNotifyRigidBodyCollision` (0x00d45530): virtual slot 0x1b0 on the component, not a
  shape flag [V].
- The native `ARB_ConstraintActor::execSetDisableCollision` (0x00af9240) is a joint flag, not a shape flag [A].
- Show-flag names exist: `FLUID_DRAINS`, `FLUID_EMITTERS`, `FORCEFIELDS` (strings at 0x01bf5a0c..0x01bf5a48). No
  class named like a drain or a force field is found in the strings, and no code references the drain name
  (the names are in a show-flag table) [A]: the engine probably has no fluid-drain shapes, so NX_SF_FLUID_DRAIN /
  NX_SF_CLOTH_DRAIN are unused.

Shim impact: no shape-flag path after creation was found, so the stored-only handling of these bits is consistent
with the exe. Add the dynamic counter (GAPS step 4) for setFlag on shapes with a non-default bit, to close it.

## 2. NxScene::setTiming and setGravity callers

Slots: setTiming = slot 82 (0x148), setGravity = slot 5 (0x14), both from the 2.8.1 header index minus 2 (notes
say the same). The object that receives them is the per-scene table entry returned by `FUN_00f8e0e0` (reads
`DAT_020009e0 + id*0x24`, +4) [V]. Whether that entry is the NxScene or the NxCompartment that the scene creator
gets from createCompartment (piVar4 in `FUN_00ac4e30`) is [A]; NxCompartment.h (2.8.1) also has setTiming, at index 6,
so the slot must be checked on the object type before using it as a shim fact.

### setTiming (0x148) [V for the call, A for the object]
- One call site: `FUN_00ac1700(this, dt, maxTimestep, mode)` (0x00ac1700, 1019 bytes). The call instruction is at
  0x00ac181e-0x00ac1824 (`mov eax,[edx+0x148]; call eax`) [V].
- Arguments [V from the disassembly]: arg0 = float maxTimestep (fstp into [esp]); arg1 = maxIter (ebx); arg2 = 0 =
  NX_TIMESTEP_FIXED (NxSceneDesc.h:26-27) (pushed as a literal). A fourth dword (ebp) is pushed too, and there is no
  caller cleanup after the call; the decompile shows 3 args. Not resolved [A] (see open questions).
- Values, from `FUN_00ac1700`: in mode 0 (the fixed-step mode of the caller) the substep count is
  `n = ceil(dt / maxTimestep)`, clamped to [1, cfg+0xdb0], then `dt' = dt / n` with a floor of 0.0025 s (400 Hz); the
  passed maxTimestep is `dt` itself when dt <= maxTimestep, so the core gets one substep when the frame is short. In
  mode != 0 the count is cfg+0xdb0 and dt is passed as-is (variable step) [V].
  So **maxIter = the game's own substep count per tick, not the SDK default 8**, and **method is always FIXED** [V].
- Callers of `FUN_00ac1700` (4 sites):
  1. `FUN_00ac2da0(dt)` (0x00ac2da0): the per-tick physics advance. Called from `FUN_00b5b830` (0x00b5b830, the world
     tick; dt is zeroed when the flag 0x400 at +0x1d0 of an object it holds is set) [V]. Arguments: dt clamped to cfg+0xda4; maxTimestep =
     cfg+0xdac (float); mode = cfg+0xda8 bit 0. cfg = `FUN_00b822c0()` (not resolved further) [V].
  2. `FUN_00f8f260(dt)` (0x00f8f260): for each scene in its list (+0x14, count +0x18): maxTimestep = **0.016 s**
     (float 0x3c83126f), mode 0. Called from `FUN_010ae7f0` and `FUN_010afb70` (not traced) [V].
  3. `FUN_006964a0(dt)` (0x006964a0): scene at obj+0x38c, maxTimestep = cfg+0xdac, mode 0; then `FUN_00abdca0` (not
     traced). Called from `vt_004a0830` with dt times the level's +0x3a0 factor [V]; it is also the tick that calls
     the skeletal update `FUN_00d235c0` (item 3).
  4. `vt_004b46c0` (0x004b46c0): contains the assert string `...UnrealEd\Src\PhATMain.cpp`, so it is the PhAT
     (physics asset editor) tick. It is in the shipped exe, but a game run is [A] not reached.
- When: every world tick (1) and every tick of the level scene list (2). Not init-only [V].

### setGravity (0x14) [V for the calls, A for the source]
Three call sites:
1. `FUN_00ac2da0` (per tick, see above). The vector is `(0, 0, gz)` with x and y = 0 and gz from `FUN_00b82300`
   (wrapper of `FUN_00ef90c0`: result of world vtable slot 0x134 times the float at object +0xcd8). The vector goes
   through `FUN_00f8c510` (multiply by 0.02, 0x00f8c510 [V]) and then to slot 5 [V]. The whole body runs only when
   the scene object's +0x160 is non-null [V]. So the scene gravity is **reset every world tick** to (0, 0, 0.02 * gz)
   [V]. The magnitude of gz (for example -980) and the object behind
   `ECX` at the `FUN_00b82300` call are [A].
2. `FUN_00abd9e0` (0x00abd9e0): sets (scaled by 0.02) the vector passed on the stack; guarded by the world object at
   `DAT_01ffbca4` having +0x160 != 0. Its caller is `FUN_00ae66b0`, which is the native
   `intAWorldInfoexecSetLevelRBGravity` (0x00ae66b0), so Kismet / script `SetLevelRBGravity` [V]. Because
   the next world tick (site 1) overwrites it, the script call only lasts until the next tick, unless the world
   field read by slot 0x134 is changed elsewhere [A].
3. `FUN_00abc980` (0x00abc980): scales its argument and calls slot 5; its only caller is `vt_004b46c0` (the PhAT tick)
   [V]. So gravity is set from the editor path only [A].
- Gravity z in the PhAT path: `gz = (float at *(world+0x950)+0x40c)+0x60) * (world+0xcd8) * (world+0xcd0)` [V
  arithmetic; the names of the terms are [A]]. The same product explains `FUN_00ef90c0`.

Shim impact: the shim reads gravity from the scene and receives it every tick, so it is correct to keep reading
it; no separate gravity event is needed. The shim should accept maxIter as a per-tick substep count (not a fixed 8) and
should not assume NX_TIMESTEP_VARIABLE.

## 3. Per-frame kinematic moves (moveGlobalPose / moveGlobalPosition / setGlobalPose)

Slots (NxActor, header index minus 2): setGlobalPose 5 (0x14), setGlobalPosition 6 (0x18), setGlobalOrientation 7
(0x1c), getGlobalPose 9 (0x24), moveGlobalPose 13 (0x34), moveGlobalPosition 14 (0x38), moveGlobalOrientation 15
(0x3c) [V from the header order; the 0x14/0x34 slots match B4]. "Body NxActor" is `body+0x60` (as in `FUN_00bd4fe0`)
[V]; `+0xa0` in the constraint path (see 3.3) [V].

Call sites found, in the order of how often they run:

**3.1 Skinned bodies, every frame: setGlobalPose, not moveGlobalPose [V for the chain, A for the frequency]**
- `FUN_006964a0` (0x006964a0, the world physics tick; dt = world dt * `+0x3a0` time dilation, from `vt_004a0830`)
  calls `FUN_00d235c0(0, 1)` (0x00d235c0) twice (lines 444006 and 444009). That function is the skeletal-mesh
  body tick; its branch `local_7c` (from component flag bits, not resolved) decides whether bones are updated [A].
- Inside it: `FUN_00dc3f60(comp+0x1c, 1, 1)` (0x00d235c0 line 319) = UpdateRBBones from the component's bones. Its
  third argument goes to `FUN_00bdc1e0(pose, flag)`.
- `FUN_00bdc1e0` (0x00bdc1e0): if the new pose differs from the current one (`FUN_00f8c580`, eps 0x38d1b717 =
  1.0e-4, see 3.4) then **`flag == 0` -> moveGlobalPose (slot 13); `flag != 0` -> setGlobalPose (slot 5)** [V]. The
  per-frame skeletal path passes flag = 1, so **it is a teleport (setGlobalPose), not a kinematic move** [V]. Children
  of the body (`+0x6c & 8`) get setGlobalPosition (slot 6, 0x18) with the child offset scaled by 0.02 [V].
- `FUN_00bdc1e0` does **not** test the kinematic flag itself [V]. Its other callers: native
  `URB_BodyInstance::execSetBoneSpringTarget` (0x00afa5a0, flag from script) [V], `FUN_00bdcbd0` (unresolved) [A],
  and the PhAT path `vt_004b46c0` (flag 0, moves) [V].
- Also in `FUN_00d235c0`, `FUN_00dc3f60(...)` runs only with `local_7c != 0` [V] (bit 3 of a component word and a
  second condition, not resolved) [A on meaning].

**3.2 Kinematic bodies with a moveGlobalPose: `FUN_00d46230` (0x00d46230) [V]**
- Gate: body flag NX_BF_KINEMATIC (0x80, readBodyFlag slot 91 = 0x16c) is set AND readBodyFlag(0x7e) is 0 (none of the
  other bits in 0x7e, i.e. not frozen/ no sleep-filter bits) [V]. Only then: getGlobalPose (slot 9) into a local,
  the determinant-style check (> 1e-4), `FUN_00f8c580` changed check, and **moveGlobalPose (slot 13) with the new
  pose** [V]. This is the one per-frame kinematic path that uses the move API.
- Caller: `FUN_00d4af70` (0x00d4af70), a component method (no direct caller found, so it is reached through a
  vtable) [V for the body]. It calls `FUN_00d46230` when component+0x2fc is non-null, and `FUN_00d47240` (cloth
  attach path, see GAPS row 9) when component+0x324 bit 0 is set [V]. Frequency: the component tick, not resolved [A].

**3.3 Constraint kinematic actors: `execMoveKinActorTransform` -> `FUN_00dbf850` [V]**
- Native `intURB_ConstraintInstanceexecMoveKinActorTransform` (0x00afb170, natives.tsv) reads a transform and calls
  `FUN_00dbf850` (0x00dbf850; its only caller is that native). It uses the NxActor at `constraint+0xa0`, calls
  getGlobalPose (slot 9), and calls **moveGlobalPose (slot 13) only when the pose changed** (eps as 3.4) [V].
- The per-frame rate is whatever the UnrealScript caller does (a script call, not a tick in the exe) [A].

**3.4 Eps and conversion**: `FUN_00f8c580(pose, new, eps)` returns 1 when the 3x3 rotation and translation are all
within eps (0x38d1b717 = 1.0e-4) and 0 otherwise, so "changed" means "not within 1e-4" [V]. The UE->PhysX conversion
is `FUN_00f8c510` (x0.02) and `FUN_00f8c400` (matrix conversion) [V].

**3.5 moveGlobalPosition / moveGlobalOrientation (slots 14, 15) and setGlobalOrientation (slot 7)**: no caller
located in the exe [A]; the scans show only slot 6 (setGlobalPosition) for child bodies (3.1).

**Answer to the three questions**
- Who calls it every frame: the world tick (`FUN_006964a0`) -> skeletal tick (`FUN_00d235c0`) -> `FUN_00bdc1e0`
  -> **setGlobalPose** (teleport) for changed bones [V]. moveGlobalPose per frame: only via `FUN_00d46230`, whose
  caller is not found [A].
- Full pose: yes, both paths pass a full NxMat34 (rotation and translation) [V].
- Kinematic at that point: the moveGlobalPose path (3.2) checks NX_BF_KINEMATIC; the setGlobalPose path (3.1) does
  not, so it is a teleport for dynamic bodies too [V].

Shim impact: the shim should treat setGlobalPose on a body as a teleport (no velocity, the static/dynamic
collision state reset), and only moveGlobalPose on a kinematic actor as a kinematic target with velocity. Check the
shim's setGlobalPose handling against this (gap 2 and 13 of GAPS.md).

## 4. setFilterOps argument order and filter constants

Resolved from the disassembly of the scene creator (`FUN_00ac4e30`, 0x00ac5255-0x00ac5294), not from the decompile.
Slots: NxScene vtable offsets 0xdc / 0xe0 / 0xe4 / 0xe8 = setFilterOps / setFilterBool / setFilterConstant0 /
setFilterConstant1 (header index minus 2 = slots 55 to 58) [V].

- setFilterOps(op0, op1, op2): the code pushes 6, 1, 1 in that order and calls `[vtbl+0xdc]` on the scene. With
  thiscall the last push is the first argument, so **op0 = OR (1), op1 = OR (1), op2 = SWAP_AND (6)** [V]. Enum values
  from Nxp.h:2085-2092 (AND 0, OR 1, XOR 2, NAND 3, NOR 4, NXOR 5, SWAP_AND 6) [V].
- setFilterBool(1) at `[vtbl+0xe0]`, pushed value 1 [V].
- setFilterConstant0 and setFilterConstant1 take the same 4-dword buffer at [esp+0x50..0x5c] (NxGroupsMask). The
  buffer is zeroed right before the first call (`mov [esp+0x50..0x5c], edi`, edi = 0) and not written again, so
  **K0 = K1 = {0,0,0,0}** [V]. This resolves the open K1 value in exe-scene.md and B4 (the decompile showed a stale
  stack slot, 0x84, that is an allocator argument, not K1; do not trust that reading).
- Shim impact: the game's filter is "bool true, both constants zero, ops (OR, OR, SWAP_AND)". Confirm the shim
  reads back the same four values in a scene (scene_read.cpp:143, SWAP_AND as a & b with dwords swapped) [A] (a
  check, not a gap).

## 5. Fluid kill / force producers

Partly resolved. The consumers and the per-tick order are [V]; the content that fills the lists is only partly traced.

**Per-tick order (`FUN_00e85a70`, the UE3 particle system tick of one FPhysXParticleSystem, 0x00e85a70)** [V]:
- If the system's flag +0x68 is set: `FUN_00e83190` (flush pending deletes, NX_FORCE with delete flags) and
  `FUN_00e82e30` (flush pending accelerations, NX_ACCELERATION), then the acceleration buffer (+0x10, count +0x30 x 16
  bytes) is zeroed and +0x60 is reset.
- Then the emitter instances: each in the list at +0x78 is ticked by `FUN_010acfb0` (tiny, 12 lines, not traced);
  each sprite instance (list +0x80, count +0x84) by `FUN_010af0f0`, each mesh instance by `FUN_010adea0`.

**Producers of pending deletes**
- `FUN_010af0f0` (sprite emitter tick, 0x010af0f0) and `FUN_010adea0` (mesh emitter tick, 0x010adea0) both append
  one record per removed particle through `FUN_010acfc0` (0x010acfc0). `FUN_010acfc0` is a generic growable-array
  append (record size = list+4, count +0x10, capacity +0xc, data +0x8) [V]. The sprite tick first compacts the active
  index array (+0x34, count +0xc4) by swap-with-last for each removed particle, then pushes a 16-byte record per
  removed particle: {a running slot counter, the particle word at +0x18, the particle word at +0x1c (the PhysX
  particle id, per the flush's use of the same field) [A], 0} [V for the stores, A for the field meanings].
- Which list the sprite records end in is [A]. The flush `FUN_00e83190` reads FPhys+0x1c as stride-8 {id, flag}
  records [V, exe-fluid.md]; the sprite record passed to the append is 16 bytes (four dwords), and the list's record size
  (+4) was not read, so a conversion step may sit between them [A]. It was not traced.
- How many particles are removed per tick: the count at emitter+0x13c. The pass `FUN_010aed60` (0x010aed60) builds
  it: it accumulates a spawn/rate value (emitter +0x144; rate +0x158, factor +0x15c through `_CIpow` and a
  `floor`), then, while floor(acc) < +0x13c, removes particles from the active array and decrements +0x13c [V]. So
  the removal count is driven by the emitter's rate/count budget (spawn-overflow style), and the lifetime test itself
  was not located in this pass [A]. The writers of +0x13c in the other emitter code (FUN_00432830, FUN_0047da70,
  FUN_004cbbb0 etc.) are not classified [A].
- Kill volumes and force fields: no kill-volume or force-field class is referenced from these functions [A]; the
  `FORCEFIELDS` and `FLUID_DRAINS` show-flag names exist (item 1) but I found no producer for them [A].

**Producers of acceleration records (FPhys+0x10, count +0x44)**
- The mesh emitter tick builds a per-particle record (0x20 bytes, from particle +0x40/+0x44/+0x48 and +0x1c clamped to
  a minimum, with a rotation matrix from `FUN_010adb20` when emitter flag +0x140 is set, then `FUN_010acfc0`) [V for
  the data movement, A for the meaning]. The flush `FUN_00e82e30` expects 16-byte {vec3, id} records, so this record is
  not the one flushed as is [A]. The producer of the 16-byte acceleration records was not located.
- So the acceleration source is the particle data (UE3 particle modules, e.g. velocity or rotation-driven), not an
  external force API; the exact module is [A].

**Shim impact**: the shim already implements NX_FORCE delete-by-id and NX_ACCELERATION (GAPS row 18). The producer
side is particle-driven, so the set of deleted ids depends on the emitter's spawn budget; check that the shim's
removal and force rates are the same on the content (the 4096 cap, GAPS row 17, is the other limit).

## Summary

- **Shape flags after creation (1)**: no caller of NxShape::setFlag with an NX_SF bit was found. The only non-shape
  bit path is the actor DISABLE_RESPONSE (`FUN_00bd4fe0`, raise/clear actor flag slots 25/26). Shim: nothing to change;
  add a counter for shape setFlag with non-default bits to confirm.
- **Timing and gravity (2)**: setTiming (slot 82) is called once per physics advance from `FUN_00ac1700`, with
  maxTimestep (0.016 s in the level-list tick, or the config value in the world tick), maxIter = the game's own substep
  count (ceil(dt/maxTimestep) clamped to a config cap), method = NX_TIMESTEP_FIXED [V]. setGravity (slot 5) is reset
  every world tick to (0, 0, 0.02 * gz) from the world's gravity getter [V for the chain; gz and the object [A]]. The
  script native SetLevelRBGravity is overwritten by the next tick [A].
- **Kinematic moves (3)**: the per-frame skinned path uses **setGlobalPose** (teleport, flag 1) when the pose changed by
  more than 1e-4; **moveGlobalPose** is used only in `FUN_00d46230` (NX_BF_KINEMATIC set and no other 0x7e bits, its
  caller is a component method) and in the constraint native `execMoveKinActorTransform` [V].
- **Filter (4)**: setFilterOps(OR, OR, SWAP_AND) with setFilterBool(true) and K0 = K1 = 0, verified in the disassembly
  of `FUN_00ac4e30` [V]. The open K1 value is resolved (zero).
- **Fluid producers (5)**: removed particles come from the sprite and mesh emitter ticks, appended through
  `FUN_010acfc0`; the removal count is set by the emitter spawn/rate pass `FUN_010aed60` (+0x13c) [V for the flow, A
  for the meaning]. Acceleration producers and the lifetime test were not located [A].

## Open questions

1. setTiming has a fourth pushed dword (ebp) and no caller cleanup at `0x00ac181e-0x00ac1824`, but the decompile shows
   3 arguments. Check the slot-82 callee's signature (or the object type: NxScene or NxCompartment, which has setTiming at
   index 6). This decides whether the shim's setTiming read-back is right.
2. The NxScene slot numbers from the 2.8.1 header differ by one from the notes for createFluid (header index minus 2
   gives 127, exe-scene.md says 126) and createCloth (132, shim says 131). Slots 5, 55-58, 82 (used here) agree with
   the disassembly; the names above 120 need one header check against the 2.8.0 core before use.
3. Is the object in `FUN_00f8e0e0` (table DAT_020009e0 + id*0x24, +4) the NxScene or the NxCompartment that the
   creator gets from createCompartment (`FUN_00ac4e30`, piVar4)? The game calls the filter and the group-pair setters
   on that object, so it behaves as a scene for the slots used.
4. Who calls `FUN_00d4af70` (component method, reached only by vtable so far) and how often `FUN_00d235c0` runs for each
   skeletal component (its gate `local_7c` is not resolved).
5. Shape setFlag census: 1,010 functions use slot 5; the two-argument, flag-constant subset (60) is not fully checked.
   A dynamic counter (GAPS step 4) is the faster way to close it.
6. Fluid: the callee that copies the sprite/mesh emitter records into FPhys+0x1c (stride 8) and the acceleration
   producer (16-byte records for FPhys+0x10) are not located; the lifetime test is not located.
7. The gravity getter `FUN_00ef90c0` (world slot 0x134 times +0xcd8) and the scale +0xcd0 need names from the package
   (`tools/ue3pkg.py`) before gz can be quoted as a value.

