# MirrorsEdge.exe: NxCloth calls after creation (audit step 2, task B2)

Static RE only (no game launch). Sources: `re/out/MirrorsEdge/` text exports, and objdump of
`re/ghidra/MirrorsEdge.exe` for the flush, the NxdScene wrappers and the jump table. Tags: [V] seen in code
(address given), [A] inferred. Slot numbers follow the 2.8.1 NxCloth.h declaration order, which the exe's own wrapper
and call sites agree with wherever checked (slot 48/52 = getPositions/getVelocities, slot 53 = getNumberOfParticles,
scene 131/132/140/141/142 = createCloth/releaseCloth/simulate/checkResults/fetchResults).

## 0. Where things are

| item | address | note |
|---|---|---|
| NxdCloth vtable | 0x1d71a6c, 92 slots | rtti_vtables.txt |
| NxdCloth ctor | 0x146d0a0 | wrapper +0x8 = real cloth, +0xc = NxdScene, +0x10 = index in scene cloth list |
| NxdCloth dtor (slot 0 body) | 0x146c5a0 (slot 0 = 0x14704a0) | frees shadows, real `scene->releaseCloth` (scene slot 132) |
| **cloth flush** (applies buffered writes) | **0x146c8c0** | replays queued records, refreshes shadows (asm-verified) |
| scene-level flush driver | 0x145d630 | calls 0x146c8c0 for every registered cloth; called from checkResults wrapper |
| registration into flush list | 0x145dda0 | list at NxdScene +0x964 (count +0x968) |
| NxdScene busy flag | NxdScene +0x14 | set by simulate wrapper 0x145c260, cleared at the flush |
| NxdScene::checkResults wrapper (slot 141) | 0x145e960 | flush runs when real checkResults is true and status bit 0 is set [V asm] |
| NxdScene::fetchResults wrapper (slot 142) | 0x145d310 | calls wrapper check, then real fetch, then 0x145c520 (deferred deletes) when status bit 0 set [V asm] |
| NxdScene::createCloth wrapper (131) | 0x145e700 | refuses while busy ("Attempt to create cloth during simulation"); else real createCloth, then wraps the result in a new NxdCloth (FUN_0146d0a0) |
| NxdScene::releaseCloth wrapper (132) | 0x145d090 | refuses while busy ("Attempt to release cloth during simulation"), then deletes the wrapper |
| guard that logs writes during simulate | 0x146c520 | logs "Attempt to write to cloth during simulation in function %s" (source tag NxdCloth.cpp) [V string] |
| engine cloth member | USkeletalMeshComponent +0x35c (NxdCloth*), +0x1c8 (cloth sim parameter block), +0x324 (flags), +0x328 (external accel vector), +0x360 (world/scene id), +0x388 (particle count, written at creation) | [V code; names [A] ] |

## 1. Flush semantics (layer a)

### 1.1 Who runs the flush and when [V]
1. Game calls `NxdScene::simulate` (wrapper 0x145c260): calls real simulate (slot 140), sets busy (+0x14) = 1.
   The shim hook `hkSimulate` runs inside the real call.
2. While busy, the wrapper's scalar setters do not call the real object. They append a 36-byte record to the cloth's
   queue (+0xa4 begin, +0xa8 end, +0xac capacity), set a dirty bit in +0x18, store the value in a wrapper field, and
   register the cloth with the scene (0x145dda0). Outside busy, the same setters call the real slot directly.
3. Game calls `checkResults(status)` (wrapper 0x145e960). If the real `checkResults` returns true and `status & 1`
   (NX_RIGID_BODY_FINISHED, NxScene.h:106), it calls the scene flush 0x145d630, then clears busy.
4. The flush 0x145d630 runs, for each registered cloth, 0x146c8c0 (section 1.2), then resets the registration index.
5. `fetchResults(status)` (wrapper 0x145d310) calls the wrapper check again (already flushed, so a no-op for the lists),
   then real `fetchResults` (shim hook `hkFetchResults`), then deferred deletes (0x145c520) if `status & 1`.

**Consequence for the shim [V]:** the replayed setters reach the real (shim) cloth between real `checkResults` and
real `fetchResults`, not at the game's call site. Writes made during busy are applied one step late, at that point.

### 1.2 Order of work inside the cloth flush 0x146c8c0 [V asm]
1. Replay the queue in recording order. Each record is 9 dwords: opcode at [0] (1..0x20), arguments after. The opcode
   is looked up in a 32-entry jump table at 0x146d01c (opcode 0x1f and 0x17 and any other value go to the no-op
   default at 0x146ce52). Opcode to real slot (decoded from the table):

| opcode | slot | method | opcode | slot | method |
|---|---|---|---|---|---|
| 0x01 | 62 | setAttachmentResponseCoefficient | 0x12 | 17 | setThickness |
| 0x02 | 15 | setAttachmentTearFactor | 0x13 | 66 | setToFluidResponseCoefficient |
| 0x03 | 3 | setBendingStiffness | 0x14 | 43 | setValidBounds |
| 0x04 | 60 | setCollisionResponseCoefficient | 0x15 | 49 | setVelocity |
| 0x05 | 7 | setDampingCoefficient | 0x16 | 72 | setWindAcceleration |
| 0x06 | 68 | setExternalAcceleration | 0x17 | none | (setMinAdhereVelocity: dropped, see 1.4) |
| 0x07 | 79 | setFlags | 0x18 | 28 | attachVertexToGlobalPosition |
| 0x08 | 9 | setFriction | 0x19 | 29 | freeVertex |
| 0x09 | 64 | setFromFluidResponseCoefficient | 0x1a | 78 | putToSleep |
| 0x0a | 37 | setGroup | 0x1b | 77 | wakeUp |
| 0x0b | 39 | setGroupsMask | 0x1c | 84 | addDirectedForceAtPos |
| 0x0c | 45 | setPosition | 0x1d | 83 | addForceAtPos |
| 0x0d | 11 | setPressure | 0x1e | 82 | addForceAtVertex |
| 0x0e | 76 | setSleepLinearVelocity | 0x20 | 35 | tearVertex |
| 0x0f | 22 | setSolverIterations | | | |
| 0x10 | 5 | setStretchingStiffness | | | |
| 0x11 | 13 | setTearFactor | | | |

2. Read the real particle count (slot 53) and compare with the cached count (+0xbc).
3. If the count changed (tear): allocate two new buffers of 12*N bytes (positions, velocities), read the real positions
   (slot 48) and velocities (slot 52) into them, then overlay the game-written shadow (old count * 12 bytes) for
   positions if dirty bit 0x4000 is set and for velocities if 0x800000 is set. Free the old buffers, store the new
   pointers and count. **No real setPositions/setVelocities call is made on this branch** [V asm 0x146cea8-0x146cf8b].
4. If the count is unchanged: if 0x4000, real setPositions(shadow positions, stride 12) (slot 46); else real
   getPositions(shadow positions) (slot 48). Then if 0x800000, real setVelocities (slot 50); else real getVelocities
   (slot 52) [V asm 0x146cf96-0x146cff8].
5. Clear the dirty word (+0x18 = 0).

The shadow buffers (+0xb4 positions, +0xb8 velocities) are therefore the game's view between flushes. Reads
(getPosition/getPositions/getVelocity(ies)/getNumberOfParticles) return them without calling the real object.

### 1.3 Which wrapper calls are buffered [V, from the wrapper bodies]
- **Always buffered** (queued and shadow updated at call time, regardless of busy): setPositions (46, memcpy into +0xb4,
  dirty 0x4000, 0x146c370) and setVelocities (50, memcpy into +0xb8, dirty 0x800000, 0x146c3f0).
- **Busy-only buffered** (direct call when not busy; shadow NOT updated on the direct path): setPosition (45),
  setVelocity (49), attachVertexToGlobalPosition (28), freeVertex (29), tearVertex (35), wakeUp (77), putToSleep (78),
  setSleepLinearVelocity (76), addForceAtPos (83), addForceAtVertex (82), addDirectedForceAtPos (84), all scalar setters
  listed in table 2 (they also cache the value when buffered).
- **Guarded direct** (log if busy, then call real, never buffered): saveToDesc (1), attachToShape (24),
  attachToCollidingShapes (25), detachFromShape (26), attachVertexToShape (27), attachToCore (34), setMeshData (41).
  Each calls 0x146c520 which logs "Attempt to write to cloth during simulation in function %s" when busy [V].
- **Forward only**: getClothMesh (2), getDensity (19), getRelativeGridSpacing (20), getWorldBounds (23), getVertexAttachment*
  (31-33), raycast (36), getMeshData (42), overlapAABBTriangles (85),
  getScene (86), getName (87), getCompartment (88), getPPUTime (89), getForceFieldMaterial (90), setForceFieldMaterial (91),
  isSleeping (74), queryShapePointers/getStateByteSize/getShapePointers/setShapePointers/save/loadState (54-59), setName (81).
- **Cached getters**: return the wrapper field when the dirty bit is set, else forward. Bit to field: 0x2 +0x1c (attachment
  response, 63 [A bit]), 0x4 +0x20 (attachment tear, 16), 0x8 +0x24 (bending, 4), 0x10 +0x28 (collision response, 61),
  0x20 +0x2c (damping, 8), 0x40 +0x30..0x38 (external accel, 69), 0x80 +0x3c (flags, 80), 0x100 +0x40 (friction, 10),
  0x200 +0x44 (from-fluid, 65), 0x400 +0x48 (group, 38), 0x800 +0x4c..0x58 (groups mask, 40), 0x2000000 +0x9c (min adhere, 71),
  0x1000000 +0x90..0x98 (wind, 73), 0x10000 +0x60 (sleep lin vel, 75), 0x20000 +0x64 (solver iterations, 21),
  0x40000 +0x68 (stretching, 6), 0x80000 +0x6c (tear, 14), 0x100000 +0x70 (thickness, 18), 0x200000 +0x74 (to-fluid, 67),
  0x400000 +0x78..0x84 (valid bounds, 44), 0x8000 +0x5c (pressure, 12), 0x4000 / 0x800000 are the array buffers above.

### 1.4 Findings from the flush
- [V] **setMinAdhereVelocity (70) is never replayed.** The wrapper queues opcode 0x17, which the table maps to the no-op
  default (0x146ce52). The cached getter (71) still returns the value. The shim also logs it as unsupported.
- [V] **Count change drops game-written positions from the solver.** On the tear branch (1.2 step 3) the overlaid positions
  reach only the shadow. The refresh on a later flush reads the solver's positions back, so the write disappears [A on timing].
  Compare shim behaviour: setPosition after a tear is dropped (shim-cloth.md row 45).
- [V] Per-vertex reads (getPosition 47, getVelocity 51) see the shadow, which for busy-only setters is only updated by a flush.
  A direct setPosition outside the busy window is not copied into the shadow. The shadow is refreshed only for clouths
  in the flush list (registered by a buffered write in the same frame), so such a write may stay invisible to getPositions
  until the next registration [A on the registration pattern].
- [V] The per-frame tick reaches the flush every frame through setVelocities (50), which always buffers and registers (see 2.2).

## 2. NxdCloth wrapper table (layer a, 92 slots)

Slot names: 2.8.1 NxCloth.h order. Wrapper address from rtti_vtables.txt. "Class" uses the classes in section 1.3.
"Engine caller" = section 3 (natives and engine functions that reach the slot). Only items the engine or flush touch are
discussed further; the rest are forwards.

| slot | PhysX API item | wrapper addr | class / buffer (dirty bit, field, opcode) | engine caller | notes |
|---|---|---|---|---|---|
| 0 | ~NxCloth (dtor) | 0x14704a0 | dtor -> 0x146c5a0 | scene releaseCloth (132) | frees shadows; real `scene->releaseCloth` [V] |
| 1 | saveToDesc | 0x146c880 | guarded forward | none | logs when busy [V] |
| 2 | getClothMesh | 0x146be80 | forward | none | [V] |
| 3 | setBendingStiffness | 0x146d4a0 | busy-buffered (0x8, +0x24, op 3) | native SetClothBendingStiffness; UpdateClothParams | [V] |
| 4 | getBendingStiffness | 0x146be60 | cached (0x8, +0x24) | native GetClothBendingStiffness | [V] |
| 5 | setStretchingStiffness | 0x146e7b0 | busy-buffered (0x40000, +0x68, op 0x10) | native Set/UpdateClothParams | [V] |
| 6 | getStretchingStiffness | 0x146c190 | cached (0x40000, +0x68) | native Get | [V] |
| 7 | setDampingCoefficient | 0x146d770 | busy-buffered (0x20, +0x2c, op 5) | native Set/Update | [V] |
| 8 | getDampingCoefficient | 0x146bec0 | cached (0x20, +0x2c) | native Get | [V] |
| 9 | setFriction | 0x146dbe0 | busy-buffered (0x100, +0x40, op 8) | native Set/Update | [V] |
| 10 | getFriction | 0x146bf60 | cached (0x100, +0x40) | native Get | [V] |
| 11 | setPressure | 0x146e360 | busy-buffered (0x8000, +0x5c, op 0xd) | native Set/Update | stored only in the shim; real core ignores it [A] |
| 12 | getPressure | 0x146c110 | cached (0x8000, +0x5c) | native Get | [V] |
| 13 | setTearFactor | 0x146e920 | busy-buffered (0x80000, +0x6c, op 0x11) | native Set/Update (via +0x200) | [V] |
| 14 | getTearFactor | 0x146c1b0 | cached (0x80000, +0x6c) | native Get | [V] |
| 15 | setAttachmentTearFactor | 0x146d340 | busy-buffered (0x4, +0x20, op 2) | native Set/Update | [V] |
| 16 | getAttachmentTearFactor | 0x146be40 | cached (0x4, +0x20) | native Get | [V] |
| 17 | setThickness | 0x146ea90 | busy-buffered (0x100000, +0x70, op 0x12) | native Set; UpdateClothParams (thickness*0.02) | [V] |
| 18 | getThickness | 0x146c1d0 | cached (0x100000, +0x70) | native Get | [V] |
| 19 | getDensity | 0x146bee0 | forward | none found | [V] |
| 20 | getRelativeGridSpacing | 0x146c130 | forward | none found | [V] |
| 21 | getSolverIterations | 0x146c170 | cached (0x20000, +0x64) | native Get | [V] |
| 22 | setSolverIterations | 0x146e640 | busy-buffered (0x20000, +0x64, op 0xf) | UpdateClothParams | [V] |
| 23 | getWorldBounds | 0x146c310 | forward | FUN_00d4c6d0 (via FUN_00d1fb60) [A] | |
| 24 | attachToShape | 0x146c730 | guarded direct, shape converted by 0x1474180 [A] | none found | |
| 25 | attachToCollidingShapes | 0x146c6d0 | guarded direct | native AttachClothToCollidingShapes (00d48950, vt+0x64) | [V] |
| 26 | detachFromShape | 0x146c7b0 | guarded direct | none found | |
| 27 | attachVertexToShape | 0x146c770 | guarded direct | FUN_00d496f0 (SetAttachClothVertsToBaseBody) | [V] |
| 28 | attachVertexToGlobalPosition | 0x146f3e0 | busy-buffered (op 0x18) | FUN_00d47240 (per fixed vertex) | [V] |
| 29 | freeVertex | 0x146f550 | busy-buffered (op 0x19) | FUN_00d496f0, FUN_00d4b930 | [V] |
| 30 | dominateVertex | 0x146fe60 | busy-buffered, no flush case | none found | replay dropped [V] |
| 31 | getVertexAttachmentStatus | 0x146c040 | forward | none found | |
| 32 | getVertexAttachmentShape | 0x146c030 | forward | none found | |
| 33 | getVertexAttachmentPosition | 0x146c010 | forward | none found | |
| 34 | attachToCore | 0x146c6f0 | guarded direct, passes the actor's real pointer (+8) | FUN_00d49090 (buoyancy-type attach) | [V] |
| 35 | tearVertex | 0x146ffe0 | busy-buffered (op 0x20) | none confirmed (FUN_013cab96 matched a scan, unverified) | |
| 36 | raycast | 0x146c4a0 | forward | none found | open |
| 37 | setGroup | 0x146dec0 | busy-buffered (0x400, +0x48, op 0xa) | none found | |
| 38 | getGroup | 0x146bfa0 | cached (0x400, +0x48) | none found | |
| 39 | setGroupsMask | 0x146e030 | busy-buffered (0x800, +0x4c..0x58, op 0xb) | none found | |
| 40 | getGroupsMask | 0x146bfc0 | cached (0x800, +0x4c..0x58) | none found | 16-byte struct copy [V] |
| 41 | setMeshData | 0x146c690 | guarded direct | FUN_00d4c5d0 (zeroed NxMeshData before parking, see 3.3) | [V] |
| 42 | getMeshData | 0x146c320 | forward | none found | |
| 43 | setValidBounds | 0x146ed70 | busy-buffered (0x400000, +0x78..0x84, op 0x14) | native SetClothValidBounds (FUN_00d4b6f0) | [V] |
| 44 | getValidBounds | 0x146c4d0 | cached (0x400000, +0x78..0x84) | FUN_00d4b6f0 [A] | |
| 45 | setPosition | 0x146e1c0 | busy-buffered (op 0xc), direct otherwise (no shadow update) | none found | see 1.4 |
| 46 | setPositions | 0x146c370 | always buffered: memcpy to +0xb4, dirty 0x4000 | FUN_00d47800 (SetClothPosition), FUN_00d4b3b0 (RefPose) | [V] |
| 47 | getPosition | 0x146c0e0 | shadow read (+0xb4) | none found | |
| 48 | getPositions | 0x146c060 | shadow read (+0xb4) | FUN_00d47800, FUN_00d48db0 | [V] |
| 49 | setVelocity | 0x146ef30 | busy-buffered (op 0x15), direct otherwise | native SetClothVelocity uses 50 instead | |
| 50 | setVelocities | 0x146c3f0 | always buffered: memcpy to +0xb8, dirty 0x800000 | FUN_00d4afa0 (per frame), FUN_00d479f0, FUN_00d4b3b0 | [V] |
| 51 | getVelocity | 0x146c210 | shadow read (+0xb8) | none found | |
| 52 | getVelocities | 0x146c240 | shadow read (+0xb8) | FUN_00d4afa0, FUN_00d479f0 | [V] |
| 53 | getNumberOfParticles | 0x146c050 | shadow read (+0xbc) | FUN_00d4ba70, FUN_00d4afa0, FUN_00d48db0 | count changes only in the flush [V] |
| 54-59 | queryShapePointers / getStateByteSize / getShapePointers / setShapePointers / saveStateToStream / loadStateFromStream | 0x146c840, 0x146c800, 0x146c7e0, 0x146c8a0, 0x146c860, 0x146c820 | forward | none found | |
| 60 | setCollisionResponseCoefficient | 0x146d600 | busy-buffered (0x10, +0x28, op 4) | FUN_00d492d0 | [V] |
| 61 | getCollisionResponseCoefficient | 0x146be90 | cached (0x10, +0x28) | native Get | [V] |
| 62 | setAttachmentResponseCoefficient | 0x146d1d0 | busy-buffered (+0x1c, op 1) | FUN_00d492d0 | [V] |
| 63 | getAttachmentResponseCoefficient | 0x146be20 | cached (+0x1c) | native Get | bit [A] |
| 64 | setFromFluidResponseCoefficient | 0x146dd50 | busy-buffered (0x200, +0x44, op 9) | none found | no cloth-fluid coupling [A] |
| 65 | getFromFluidResponseCoefficient | 0x146bf80 | cached (0x200, +0x44) | none found | |
| 66 | setToFluidResponseCoefficient | 0x146ec00 | busy-buffered (0x200000, +0x74, op 0x13) | none found | |
| 67 | getToFluidResponseCoefficient | 0x146c1f0 | cached (0x200000, +0x74) | none found | |
| 68 | setExternalAcceleration | 0x146d8d0 | busy-buffered (0x40, +0x30..0x38, op 6) | FUN_00d436b0 (creation), FUN_00d49650 (native SetClothExternalForce) | [V] |
| 69 | getExternalAcceleration | 0x146bef0 | cached (0x40) | none found | |
| 70 | setMinAdhereVelocity | 0x146f270 | busy-buffered (0x2000000, +0x9c, op 0x17) | none found | replay dropped, see 1.4 [V] |
| 71 | getMinAdhereVelocity | 0x146c340 | cached (0x2000000, +0x9c) | none found | |
| 72 | setWindAcceleration | 0x146f0d0 | busy-buffered (0x1000000, +0x90..0x98, op 0x16) | none found | |
| 73 | getWindAcceleration | 0x146c2c0 | cached (0x1000000) | none found | |
| 74 | isSleeping | 0x146c470 | forward | FUN_00ea3e00 [A] | |
| 75 | getSleepLinearVelocity | 0x146c150 | cached (0x10000, +0x60) | native Get | [V] |
| 76 | setSleepLinearVelocity | 0x146e4d0 | busy-buffered (0x10000, +0x60, op 0xe) | UpdateClothParams | [V] |
| 77 | wakeUp | 0x146f810 | busy-buffered (op 0x1b) | native SetClothSleep (false) | [V] |
| 78 | putToSleep | 0x146f6b0 | busy-buffered (op 0x1a) | native SetClothSleep (true); FUN_00d4ba70 at creation | [V] |
| 79 | setFlags | 0x146da70 | busy-buffered (0x80, +0x3c, op 7) | FUN_00d492d0, FUN_00d43580, FUN_00d43700 | [V] |
| 80 | getFlags | 0x146bf30 | cached (0x80, +0x3c) | FUN_00d492d0, FUN_00d43580, FUN_00d43700, native Get | [V] |
| 81 | setName | 0x146c6b0 | forward | none found | |
| 82 | addForceAtVertex | 0x146fce0 | busy-buffered (op 0x1e) | FUN_00d4afa0 (NX_VELOCITY_CHANGE per vertex) | [V] |
| 83 | addForceAtPos | 0x146fb40 | busy-buffered (op 0x1d) | none found | |
| 84 | addDirectedForceAtPos | 0x146f990 | busy-buffered (op 0x1c) | native AddImpulseAtClothPos (00d47720) | [V] |
| 85 | overlapAABBTriangles | 0x146c490 | forward | none found | open |
| 86 | getScene | 0x146c140 | forward | many (FUN_00d4c5d0, FUN_00d496f0, FUN_00d4afa0, FUN_00d48db0) | [V] |
| 87 | getName | 0x146c000 | forward | none found | |
| 88 | getCompartment | 0x146beb0 | forward | none found | |
| 89 | getPPUTime | 0x146c360 | forward | none found | |
| 90 | getForceFieldMaterial | 0x146bf50 | forward | none found | |
| 91 | setForceFieldMaterial | 0x146c670 | forward | none found | |

Also in the wrapper: NxdCloth+0x4 (the wrapper's own field) is zeroed by the ctor (0x146d0cc) and the base ctor (0x146bdd8);
nothing writes it afterwards in cloth code [V].

## 3. Engine side (layer b)

Natives: the UE3 thunks in `natives.tsv` read script parameters and call the implementation. Cloth pointer = component +0x35c.
Almost every setter/getter is gated on `component+0x1c8 != 0 && component+0x35c != 0`; the getter then returns a constant when
gated off (DAT_01a9970c) [V].

### 3.1 Natives to slots

| PhysX API item | set/called? | address (native / impl) | UE3 source (native) | notes |
|---|---|---|---|---|
| NxCloth::setBendingStiffness (3) | called [V] | 0x00d48270 | USkeletalMeshComponent.SetClothBendingStiffness | param to slot 3 |
| getBendingStiffness (4) | called [V] | 0x00d47cc0 | ...GetClothBendingStiffness | |
| setStretchingStiffness (5) / get (6) | called [V] | 0x00d48670 / 0x00d48020 | ...SetClothStretchingStiffness | |
| setDampingCoefficient (7) / get (8) | called [V] | 0x00d48370 / 0x00d47da0 | ...SetClothDampingCoefficient | |
| setFriction (9) / get (10) | called [V] | 0x00d48470 / 0x00d47e70 | ...SetClothFriction | |
| setPressure (11) / get (12) | called [V] | 0x00d484f0 / 0x00d47ee0 | ...SetClothPressure | |
| setTearFactor (13) / get (14) | called [V] | 0x00d486f0 / 0x00d48090 | ...SetClothTearFactor | |
| setAttachmentTearFactor (15) / get (16) | called [V] | 0x00d481f0 / 0x00d47c50 | ...SetClothAttachmentTearFactor | |
| setThickness (17) / get (18) | called [V] | 0x00d48770 / 0x00d48100 | ...SetClothThickness | |
| setSolverIterations (22) / get (21) | called [V] | 0x00d485f0 / 0x00d47fc0 | ...SetClothSolverIterations | |
| setSleepLinearVelocity (76) / get (75) | called [V] | 0x00d48570 / 0x00d47f50 | ...SetClothSleepLinearVelocity | get returns constant via 0x35c gate; slot 75 call not matched by the regex [A] |
| setFlags (79) / getFlags (80) | called [V] | 0x00d483f0 / 0x00d47e10 | ...SetClothFlags | |
| setCollisionResponseCoefficient (60) / get (61) | called [V] | 0x00d482f0 / 0x00d47d30 | ...SetClothCollisionResponseCoefficient | |
| setAttachmentResponseCoefficient (62) / get (63) | called [V] | 0x00d48170 / 0x00d47be0 | ...SetClothAttachmentResponseCoefficient | |
| putToSleep (78) / wakeUp (77) | called [V] | 0x00d487f0 (SetClothSleep) | ...SetClothSleep(bool) | true -> putToSleep; false -> wakeUp with constant DAT_01c370e4 [A: NX_SLEEP_INTERVAL] |
| addDirectedForceAtPos (84) | called [V] | 0x00d47720 | ...AddImpulseAtClothPos | position and force vectors built from the script; mode and radius [A] |
| setPositions (46) + getPositions (48) | called [V] | 0x00d48890 -> FUN_00d47800 | ...SetClothPosition | translate all vertices by one vector: read, add, write back |
| setVelocities (50) + getVelocities (52) | called [V] | 0x00d488f0 -> FUN_00d479f0 | ...SetClothVelocity | same pattern for velocities |
| attachToCollidingShapes (25) | called [V] | 0x00d48950 | ...AttachClothToCollidingShapes | vtable +0x64 with a flags arg [V] |
| setValidBounds (43) | called [V] | 0x00d4b8b0 -> FUN_00d4b6f0 | ...SetClothValidBounds | |
| setFlags/getFlags (NX_CLF_VALIDBOUNDS 0x8000) | called [V] | 0x00d48a40 -> FUN_00d43580 | ...EnableClothValidBounds | flag only; the shim stores bounds, no effect |
| setFlags/getFlags (NX_CLF_STATIC 0x2) | called [V] | 0x00d49eb0 -> FUN_00d43700 | ...SetClothFrozen | also sets component +0x324 bit 4 |
| freeVertex (29) + attachVertexToShape (27) | called [V] | 0x00d49e40 -> FUN_00d496f0 | ...SetAttachClothVertsToBaseBody(bool) | loop over fixed vertices (+0x188..+0x160); attaches to the base body shape |
| setExternalAcceleration (68) | called [V] | 0x00d49650 | ...SetClothExternalForce | stores vector at component +0x328 |
| many setters (3..80) | called [V] | 0x00d492d0 (FUN_00d492d0) | ...UpdateClothParams | copies the cloth sim parameter block (section 3.4) into the cloth, flags last |
| setPositions (46) + getPositions (48) + setVelocities (50) | called [V] | 0x00d4b6c0 -> FUN_00d4b3b0 | ...ResetClothVertsToRefPose | count taken from +0x388 (written at creation) [A on other writes] |
| setEnableClothSimulation | called [V] | 0x00d19f40 -> FUN_00d18f20 | ...SetEnableClothSimulation(bool) | true: create (FUN_00d4ba70); false: release (FUN_00d4c5d0(0)) |

Not found: a native for getNormals (no such NxCloth method in 2.8.1) and a native for raycast/overlap.

### 3.2 Creation (FUN_00d4ba70) [V]
- Fills an NxClothDesc on the stack; the flags are kept in a local and ORed with NX_CLF_TEARABLE (1<<12) when sim+0x1fc bit 0
  and +0x16c == 0, and NX_CLF_HARDWARE (1<<13) when FUN_00f8c2b0() and component+0x118 bit 7 [V flag values; the desc layout is not checked].
- `scene(FUN_00f8e0e0()) -> createCloth(desc)` (scene slot 131 = 0x20c). Result stored at +0x35c.
- Then: getNumberOfParticles -> +0x388; +0x360 = world id (param+0x70); UpdateClothParams (FUN_00d492d0); external accel
  (FUN_00d436b0 -> slot 68); frozen flag (FUN_00d43700 -> setFlags STATIC bit); fixed vertices (FUN_00d47240 -> slot 28 per
  vertex); FUN_00d48db0 (getPositions and getScene, role [A]); then **putToSleep (slot 78) unconditionally** [V].

### 3.3 Release (FUN_00d4c5d0) [V]
- If the cloth exists and the scene lookup (FUN_00f8e0a0 on +0x360) succeeds:
  - param != 0 and param+0x78 == 0: `scene->releaseCloth(cloth)` (scene slot 132) immediately, clear +0x35c.
  - otherwise: a 68-byte zeroed NxMeshData is passed to **setMeshData (slot 41)**, the cloth pointer is parked in a table at
    DAT_02000a64 (index from FUN_0040adc0), and +0x35c is cleared. The parked object is released later by code not traced [A].
- Callers: SetEnableClothSimulation(false) with param 0 (deferred path), FUN_00d4d2d0 (vt, detach) [A], FUN_006a1820 (UI), FUN_004a6150 [A].
- Release of the wrapper calls scene slot 132 (wrapper refuses while busy; the dtor then calls the real release) [V].

### 3.4 Per-frame and parameter block [V unless marked]
- **Parameter block**: component+0x1c8 (sim struct). Offsets used by UpdateClothParams: +0x1a4 flag bits (bit0 bending,
  bit1 damping, bit2 comdamping), +0x1a8 stretching (clamped 0..1), +0x1ac bending (0..1), +0x1b0 (not sent), +0x1b4 thickness
  (x0.02), +0x1b8 damping, +0x1bc solver iterations, +0x1c0 friction, +0x1c8 pressure (>=0), +0x1cc collision response (>=0),
  +0x1d0 attachment response (>=0), +0x1d4 attachment tear (>=1 with component +0x3f0), +0x1d8 sleep velocity, +0x1dc flag bits
  (bit0 bending-ortho path, bit1 selfcollision 0x8, bit2 pressure 0x1, bit3 two-way collision 0x200), +0x1fc bit0 (tearable),
  +0x200 tear factor, +0x16c (tear gate). Component +0x324 bit1 -> NX_CLF_DISABLE_COLLISION (1<<2). Names are [A].
- **UpdateClothParams flags**: reads getFlags (80), changes bits BENDING(1<<6)/BENDING_ORTHO(1<<7), DAMPING(1<<8),
  COMDAMPING(1<<14), PRESSURE(1<<0), SELFCOLLISION(1<<3), COLLISION_TWOWAY(1<<9), TEARABLE(1<<12), DISABLE_COLLISION(1<<2),
  then one setFlags (79). Order: setters first, then flags. [V]
- **FUN_00d24b60 (component per-frame update, name [A], 728 bytes, reached by vtable)**: under a flag test calls
  FUN_00d43700 (frozen sync). When component+0xc9 bit 2 is clear, calls FUN_00d4afa0 [V].
- **FUN_00d4afa0 (per frame) [V]**: getNumberOfParticles (53, shadow), getVelocities (52, shadow), getScene (86), then for each
  vertex addForceAtVertex (82) with NX_VELOCITY_CHANGE (2), then **setVelocities (50)**, which always buffers and registers the
  cloth for the flush. So every frame that runs this path gets one flush.
- **FUN_00d4af70 (36 bytes, vtable)** -> FUN_00d47240: for each fixed vertex, a world position from FUN_00d471b0 (reference
  transform), then **attachVertexToGlobalPosition (28)**. Per-frame [A].
- **FUN_00d4b930 (vtable, 311 bytes)**: freeVertex (29) under component+0x324 bit 0 / sim flag checks. [A on frequency]
- **FUN_00d49090 (vtable, buoyancy-type attach) [V]**: when component+0x324 bit 0, flag +0x314 & 0x200, body +0x22c with a count,
  sums a volume over the body's shapes; if > 0 calls **attachToCore (slot 34)** with the body's actor and three sim
  parameters (+0x1f0, +0x1f4, +0x1f8). Only attachToCore caller found; the shim stub (shim-cloth.md row 34) ignores it.
- **Rendering reads**: positions for drawing are not located. getPositions (48) callers found: FUN_00d47800 and FUN_00d48db0.
  getNormals does not exist in 2.8.1.
- **Tearing reads**: getNumberOfParticles (53) is read in FUN_00d4afa0 every frame, so a count change is seen there
  (the shadow was already resized in the flush). [V]
- **Sleep reads**: isSleeping (74) called from FUN_00ea3e00 [A]; the native SetClothSleep sends putToSleep/wakeUp.

### 3.5 Termination
- releaseCloth: via FUN_00d4c5d0 (section 3.3). Scene slot 132 (wrapper) -> wrapper dtor -> real release (shim oursRelease).
- releaseClothMesh (NxPhysicsSDK slot 22): not called from the cloth paths above [A: not searched].

### 3.6 Raycast, overlap, bounds
- getWorldBounds (23): forward; FUN_00d4c6d0 reads it (430 bytes, called from FUN_00d1fb60) [A].
- raycast (36) and overlapAABBTriangles (85): no caller found in the cloth paths [open].

## 4. The userData question [V]

- The engine only ever holds the **NxdCloth** pointer (wrapper). Its +0x4 is the wrapper's own field, zeroed in the ctor
  (0x146d0cc) and the base ctor (0x146bdd8). Nothing in the cloth wrapper code writes it later.
- The wrapper **does not touch the real cloth's +0x4** in any path checked: all 92 slot bodies, the ctor (0x146d0a0), dtor
  (0x146c5a0), flush (0x146c8c0), createCloth/releaseCloth wrappers (0x145e700, 0x145d090). The only real-object access at
  offset 4 is `saveToDesc` through vtable +4 (slot 1), which is a call, not a field access. Confirmed by a disassembly
  search of 0x146bd00-0x1470800 for stores at [reg+4]: the hits are all wrapper, queue or other-class buffers.
- So the shim's private pointer at real +4 is never read or written by the engine, and the engine would read 0 at its own
  NxdCloth+4. Nothing found in the engine that reads `cloth+4` [A: not exhaustively searched].

## 5. Open questions

1. Render path: which function reads positions for drawing (and whether it reads through slot 48 on the wrapper). Not found.
2. raycast (36), overlapAABBTriangles (85), detachFromShape (26), attachToShape (24), getGroup/setGroup (37-40), setShapePointers
   callers: none found in the cloth paths.
3. Who drains the parked table DAT_02000a64 (deferred release from 3.3), and whether it ever calls scene slot 132.
4. dominateVertex (slot 30) buffers with no flush case: confirm the opcode it pushes (the regex did not capture it).
5. Component +0x388 (particle count) is written at creation; check whether any other write exists (ResetClothVertsToRefPose uses it).
6. Named vtable functions FUN_00d24b60 (tick), vt_00d4d160 (attach to world, calls create), vt_00d4d2d0 (detach, calls release)
   are inferred from their bodies; the owning class is not confirmed.
7. setPressure (11) and setFromFluidResponseCoefficient (64): the engine writes them (UpdateClothParams), but the core ignores
   pressure [A]. Confirm NX_CLF_PRESSURE handling in the core, not here.
8. The "setFlags(bit)" call in vt_00d4d160 writes the value of one bit (0 or 1) as the whole flags word; check whether it is
   intended (it may overwrite other flags) [A].
9. FUN_00d43580/00d43700 are called from natives with `unaff_EDI` for the bool: the decompiler lost the argument register,
   so the bool-to-branch mapping is read from the natives' structure, not from the argument [A].

## Independent review (spot-checks)
- Flush timing: confirmed. NxdScene slot 141 (checkResults wrapper, 0x145e960) calls the real checkResults (offset 0x234 =
  slot 141). If that returns true and status bit 1 is set, it runs the buffered-write flush 0x145d630 [V]. NxdScene slot 142
  (fetchResults wrapper, 0x145d310) first calls this wrapper's own checkResults (so the flush runs), then the real
  fetchResults (0x238 = slot 142), then 0x145c520 [V]. So every buffered cloth/fluid write reaches the shim
  in a burst just before the real fetchResults, while our async cloth job may still be running. The shim handles a
  cloth call during a job by waiting for the job (cloth_ours.cpp:929), so this is correct; the cost is that the
  "waits forced by" counter is mostly this flush. The wait would happen at fetchResults anyway.
