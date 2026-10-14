# RE notes: fluid vs triangle meshes, dynamic shapes / two-way coupling, desc ABI, unit scale

Written in our own words, no NVIDIA code. Addresses are the 2.8.0.45 `PhysXCore.dll` (image base 0x10000000) unless
stated. Offsets in "dword index" form (`desc[0x52]`) are 4-byte units. [V] = read in decompilation/disassembly,
[A] = interpretation not yet confirmed by a harness test. Companion to `docs/re-notes-fluid.md` (read that first).

**Oracle results (session 5, `scenarios/fluid/probe_mesh/`, harness keywords `quad` / `tri`):**
- Mesh proximity = the box rule (2·cd): `mesh_prox_{1.2,1.5,1.9}` are bit-identical to the box probes `bp2_A_*`. [V]
- One-sided: a particle moving up through the back face passes (`mesh_backface`). [V]
- Impact restores the start position (`mesh_drop` holds y = 0.0435 on the impact step). [V] **We don't copy this**
  (user decision 2026-10-08, treated as an artifact): `kMeshImpactRestoresStart = false` in `solver/fluid/fluid.cpp`
  puts the particle at hit + n·cd like the other shapes. `mesh_drop` is a golden ("differs by design") probe.
- Our solver: exact 7-region closest point; the face flag comes from the unclamped projection (s>0, t>0, s+t<=1), so a
  point on the shared diagonal of two triangles counts as face, as in the original. Brute force over triangles (no
  midphase yet).

## 0. Corrections to earlier notes (found while doing this)
1. **Pass order is dynamic first, static second** (the old note says static first). In `0x1019bec0` the first loop over the
   fluid's shape-pair list takes pairs whose flag word (`+0x20`) has bit 2 clear, then resolves with
   `0x1019b5d0(…, 1, 1)`, which uses the *dynamic* restitution/friction (`ctx[3]`,`ctx[4]`) and then clears the
   accumulators. The second loop takes pairs with bit 2 set and resolves with the *static* coefficients
   (`ctx[0]`,`ctx[1]`). Bit 2 is set in the pair constructor `0x1017df10` when the shape has no body (`shape+0xc == 0`),
   so bit 2 = **static**. It also matches the particle flag written in `0x1019b800`: bit 2 set -> internal flag 1 (static),
   clear -> flag 2 (dynamic). [V] The static pass starts from the target position and velocity left by the dynamic pass,
   so static geometry has the last word.
2. **Proximity distance is 2 x collisionDistance in the constants**: the PxsFluid init (`0x1017c06d`) stores
   `cd` at +0xe0 and `cd + cd` at +0xe4, and the collide context (`PxsFluid + 0xb4`) reads cd at ctx+0x2c and the
   proximity at ctx+0x30. [V] The probe fit (`proximity ~ cd * 1.0001`, `probe_proxdist`) disagrees; the cache filter in
   `0x1019a130` may be what makes it look like 1 cd for planes. Open: a mesh probe with the end point at 1.5 cd above a face
   settles it (see tests).
3. **`NxScene` vtable: the createFluid/createCloth/... slots are identical in 2.8.0 and 2.8.1.** (section 5) The earlier assumption that the shift affects `NxScene::createCloth` was incorrect. [V]

## 1. Fluid vs triangle mesh (shape-dispatch case 7)

### 1.1 Where things are
| address | role |
|---|---|
| `0x1019b800` | per-pair shape dispatch (cases 1 sphere, 2 plane, 3 capsule, 5 box, 6 convex, **7 triangle mesh**, **8 heightfield**) |
| `0x1019e660` + `0x1019c970` | build the per-batch particle-cell hash (case 7 only; cached for the batch in a 5-dword struct) |
| `0x101a9de0` | per hash cell: build the cell OBB, run the mesh midphase, call the per-particle loop |
| `0x1028d5d0` | midphase: OBB vs the mesh's bounding-volume tree (an Opcode-style OBBCollider) [V structure, A "Opcode" name] |
| `0x101a9910` | per particle in the cell x each candidate triangle: call the triangle test, merge results into the shape-local record |
| `0x101a91c0` | **particle vs one triangle**: closest point, collision/proximity classification, swept impact |
| `0x1019a980` | rotate the shape-local result to world and merge it into the batch record (mesh/heightfield version of `0x1019a130`) |
| `0x101aa130` | case 8 heightfield (same triangle routine `0x101a91c0`) |

### 1.2 Candidate triangles (midphase)
- The collide loop works on batches of at most **300** particles of one packet (`0x1019bec0`, first batch = N mod 300).
  For case 7 the batch's particles are first **grouped by their spatial-hash cell** (the same u16 cell triple stored in the
  particle record at +0x30/+0x32/+0x34 that the SPH grid uses).
  - **Cell size = world kernel radius h_w** (= kernelRadiusMultiplier x rest spacing): collide ctx +0x1c is a copy of
    PxsFluid +0x7c (`0x1017c025`). Cell index = floor(p / h_w) as signed 16-bit [V size, A floor and origin 0; the cell
    centre used later is `(index + 0.5) * h_w`].
  - Hash table: power of two = next pow2 above 2N (computed as `(2N | N)` smeared plus one), **capped at 512** entries.
    Entry = 10 bytes `{cx, cy, cz, count (0xffff = empty), firstIndex}`; slot =
    `(cy*0x65 + cz*0x1eef + cx) & (size-1)`, linear probing. Particle indices are then bucket-sorted by cell
    (stable, so ascending batch index inside a cell) [V].
- For every non-empty cell `0x101a9de0` builds an **oriented box in mesh-local space**:
  - centre = `invPose(cellCentreWorld)`; axes = rotation of the inverse pose; **half extent (all axes) = h_w/2 + motionLimit
    + collisionDistance + 0.001** (ctx+0x38 = PxsFluid +0xec = motion limit + cd + 0.001, set in the init, [V]).
  - The pose used is the first of the two shape poses (the one applied to target positions; the second one, applied to
    start positions, is not used for the box). For static meshes they are equal.
- `0x1028d5d0` returns the triangle indices whose triangle overlaps that box. The tree walk collects leaves; **a leaf is a run
  of up to 16 consecutive triangles** encoded as `(firstTri << 4) | (count - 1)`. Each triangle is then tested exactly against the
  box (separating-axis test: box faces, triangle plane, edge cross products), so the list is exact, not just bounding
  boxes [V]. Node layouts for the four tree variants (flag bits: quantised / no-leaf; 0x14, 0x18, 0x20, 0x24-byte nodes) are
  selected from the model flags; the cooked format is not needed because the shim passes the cooked mesh to the real
  core. 2.8.1 has the same dispatch (`0x1ff` cap, same call shape; its triangle test also has the 1e-4 slop) [V].
- Mesh data used by the triangle loop: vertices = 12-byte float triples at meshdata+8, triangles = **three 32-bit indices**
  (12 bytes) at meshdata+0xc [V: the loop reads dwords]. Winding: normal n = (v1 - v0) x (v2 - v0) (unnormalised).
- Cost model: every particle of a cell tests every candidate triangle of that cell's box (O(particles x triangles)); no
  per-triangle early-out other than the arithmetic below. No packet triangle cap in this software path. The message
  "Maximal number of static triangles per fluid packet exceeded. Dropping some triangles." belongs to the
  hardware mesh mirror (`MirrorManager::emitStaticMesh`, `NfFluidMeshStatic`/`NgFluidMeshStatic`, FluidTri), not to case 7.
  **The GPU/PPU path therefore handles static meshes differently** (cooked per-packet triangle lists with a limit); the
  software behaviour here is our reference [A that nothing else matters].

### 1.3 Particle vs triangle (`0x101a91c0`), all in mesh-local space
Inputs per particle: S = start position (batch rec +0x00), P = target position (rec +0x10), triangle v0, e1 = v1 - v0,
e2 = v2 - v0, n = e1 x e2 (|n| = nl), collision distance cd, proximity distance pd, flag `hit` = particle already has an
impact from an earlier shape this pass.

1. **Closest point C on the triangle to P** (barycentric s along e1, t along e2). With a=e1.e1, b=e1.e2, c=e2.e2,
   d=e1.(v0-P), e=e2.(v0-P), det=a c - b b, snum = b e - c d, tnum = b d - a e:
   - snum > 0 and tnum > 0: if snum + tnum <= det -> interior, s = snum/det, t = tnum/det, **faceFlag = true**;
     otherwise on the v1-v2 edge: s = (c + e - b - d)/(a - 2b + c), t = 1 - s.
   - snum > 0, tnum <= 0: t = 0, s = -d/a.
   - snum <= 0: s = 0, t = -e/c.
   - clamp s and t to [0,1]. Denominators are floored at FLT_MIN (1.1754944e-38). [V]
   - **This is not the exact 7-region closest point**: for snum <= 0 it never considers the v1-v2 edge, so a sliver beyond
     the v2 corner picks the v0-v2 edge instead. Recommendation (consistent with the "don't copy artifacts" rule): use the
     exact closest point; the difference is confined to that sliver.
2. D = P - C, dist2 = |D|^2, `front = n.D > 0`.
3. **collision** = front and dist2 < cd^2. **proximity** = faceFlag and front and dist2 < pd^2. (Strict compares.) Both are
   suppressed when `hit` is set. [V]
   - Collision contact: normal = n/nl if dist2 <= **1e-5** (m^2, absolute!), else D/|D|. Point = C + normal * cd.
     Edges and vertices therefore push out radially from the closest point, faces along the face normal. With
     cd < 3.16 mm (default 0.12/ppm: cd = 2.4 mm at 50 particles/m) the 1e-5 test is always true, so small-particle
     fluids always get the **face normal** even at edges. [V constant, A consequence]
   - Proximity contact: normal = n/nl, point = C + n/nl * cd, **plane distance = (S - v0).n/nl** (the signed distance of the
     start position; used to pick which two planes to keep).
   - If collision or proximity fired, **stop: no swept impact test for this triangle**. [V]
4. **Swept impact** (only if neither fired): num = n.(v0 - S), den = n.(P - S).
   - Impact needs num <= 0 (S in front of or on the plane) **and** den < num (P strictly behind the plane than S is):
     t = num/den in [0,1). Hit H = S + (P - S) t.
   - H must lie in the triangle with a **relative barycentric slop of 1e-4**: u > -1e-4, v >= -1e-4,
     u + v <= 1 + 1e-4 (u, v computed from the 2x2 system with the det of the dot products; no division until the final
     compare). [V] Degenerate triangles (det ~ 0) are not specially handled.
   - Impact output: t, normal n/nl, point H + n/nl * cd, plane distance = -num/nl (>= 0).

**One-sided**: collision and proximity require P in front of the triangle's closest point; an impact requires crossing from the
front to the back. Back-facing triangles produce nothing and a particle that starts behind a face is never stopped by it. There is
no per-mesh "double sided" flag read anywhere in these functions [V]; if the game's cooked collision meshes are double-sided
they must be so geometrically (duplicated triangles) [A]. This needs an oracle test (below).

### 1.4 Merging per particle (`0x101a9910` and `0x1019a980`)
Shape-local record (0x80 bytes per batch particle) fields, [V] from the stores:
`+0x00 S, +0x0c best t (initialised from the batch's current best t, 1.0 at the start of a pass), +0x10 P, +0x1c flags (4 impact,
8 collision, 0x10 proximity; value 2 = "already has impact", copied in), +0x20 summed collision normal, +0x30 summed
collision point, +0x40/+0x50 cache plane 0 (normal, point), +0x4c cache count, +0x5c collision count, +0x60/+0x70 cache plane 1,
+0x6c / +0x7c plane distances of the two slots`.

Per triangle result:
- **impact**: if t < record best t then record normal/point/t are replaced (earliest impact wins across all triangles). Flags
  are *assigned* 4 (this clears 8/0x10 gathered from earlier triangles), `hit = true` so later triangles' collision/proximity are
  ignored (they can still produce earlier impacts). The impact also goes into the two cache slots (below).
- **proximity** (only while `hit` is false): goes into the two cache slots; flag |= 0x10.
- **collision** (only while `hit` is false): normal and point are **summed**, count++, flag |= 8.
- **Two cache slots** keep the **two smallest plane distances** (the first two fill the slots; after that a new plane replaces
  the slot holding the larger distance if it is smaller than that one).

`0x1019a980` then moves the record to world space with the inverse of the dispatch pose (the dispatch's `fStack_961c`
block) and merges into the batch record:
- impact (record flag 4): if t < batch best t (batch +0x2c): batch normal = world normal, batch flags = (flags & ~1) | 2
  (single contact), **batch surface point (+0x10) := the particle's START position (batch +0x30)**, surface velocity := 0. So,
  unlike plane/sphere/box/capsule/convex (hit + n*cd), **a mesh/heightfield impact puts the particle back at its pre-step
  position** and reflects the velocity with the triangle normal (position update happens in `0x1019b5d0` single-contact path:
  target := surface point). [V: disassembly `0x1019aaec`: `[batch+0x10..0x18] = [batch+0x30..0x38]`.]
- collision (record flag 8): rotated sums are added (normal sum, point sum), batch count += record count, batch flags |= 1. The
  resolve step later divides by the count and renormalises the normal, exactly like the other shapes
  (`re_fluidResolveParticleContacts`), so a particle near an edge between two triangles averages both push-out points.
  Surface velocity of mesh and heightfield contacts is always zero (no kinematic mesh motion is transferred) [V: zero stores].
- cache planes (impact or proximity): stored as {world normal, d = normal . worldPoint, surfVel = 0, shape id, tag 1 static / 2
  dynamic}; when both batch slots are full the two planes with the smallest `normal . S - d` are kept (the third is dropped).
  Collision-only contacts at edges/vertices (no face region) create **no** cache plane.

Everything downstream (`0x1019b5d0`, restitution/friction response in `0x101998c0`, cached-plane solver `0x10199aa0`, motion limit
clamp, projection plane) is shared with the other shapes and unchanged.

### 1.5 Convex (case 6) and heightfield (case 8), cheap summary
- **Convex mesh (`0x101ab600`)**: a bounds early-out, then the same generic convex test `0x101ab240` already documented for the
  box (planes at +0xc, stride 0x24, count at hull+0x20). Details worth keeping: it is *one-sided by construction* (inside
  test via planes); "started inside" (start behind all planes, impact t = 0) pushes out through the plane with the largest
  (least negative) start distance to `cd + |dist|`; collision = end point within `cd` of all planes (polytope grown by cd,
  corners are not rounded; normal of the plane with the largest end distance, point = P + (cd - dist) * n); proximity = end point
  in front of **exactly one** plane and within pd; impact = entry slab time in (0,1) with entry <= exit. [V]
- **Heightfield (`0x101aa130`)**: for each particle it takes the AABB of S and P grown by `cd`, converts it to sample indices
  with the field's row/column scales (geometry +0x14/+0x18/+0x1c, dims at +0x20/+0x24, sign flags at +0x34/+0x38 flip the cell
  diagonal), and for each covered cell runs the **same `0x101a91c0`** on its two triangles. Merge uses the same `0x101a9910`-style
  record logic and `0x1019a980`. Partially read; hole/material handling not looked at. [A for the grid details]
- Drain: `NX_SF_FLUID_DRAIN` is rejected for triangle meshes ("not supported for triangle meshes") [V string].

## 2. Dynamic shapes and two-way coupling

### 2.1 Static vs dynamic in the step (`0x1019bec0`)
- Every fluid keeps a list of pair objects (`PxsFluidBodyContactManager`, vtable at `0x1017de50…`), one per (fluid, shape) from
  the packet/shape broadphase. Pair flag word `+0x20`: bit 0 = enabled for the loop, bit 1 = drain
  (vf8 returns it; the loop sets internal particle flag 4 when it is set), bit 2 = static (shape without body). [V]
- Order per batch of <= 300 particles: cached-plane resolve (0x10199aa0) -> **dynamic shapes** (resolve with dynamic
  coefficients, accumulators cleared) -> **static shapes** (resolve with static coefficients) -> motion-limit clamp (ctx+0x34) ->
  optional projection plane -> write back. See section 0.1.
- Each shape carries two poses (14 floats in 0x38-byte entries built in `0x1019c530` from the body handle table: first 7 floats
  = fields +0x00..0x18, second = +0x1c..0x34). In `0x1019b800` the particle **target** is transformed with the first and the
  **start** with the second, so a moving shape is handled as relative motion in its own frame (continuous). Which is "begin"
  and which "end" is [A]. Surface velocity of a dynamic contact comes from `0x1019a130` (v_lin + w x r about the centre of
  mass) as already documented; meshes and heightfields use 0.
- Per-particle flags written at the end of `0x1019b800`: internal flag |= 2 (dynamic) or |= 1 (static) when the pass produced any
  contact (collision or impact; proximity alone does not set them) [V from the code `(flags & 0xc) != 0`].

### 2.2 Force on dynamic actors (`0x100ff580`, called from the fluid post-step `0x10102e00` before the phase flip)
**Oracle (session 5, `scenarios/fluid/probe_dyn/`, body states in `captures/fluid/dyn_*/bodies-1.bin`) [V]:** the total
impulse matches (v_before - v_after) * collisionResponseCoefficient * particleMass (0.994x over 60 steps, floating 1 kg box,
vertical and horizontal), but it is **spread over many steps**: about 0.1 of it on the impact step, then each step's
share ~0.9x the previous one (geometric). Where the smoothing lives (force buffer in the body, or in this routine) is
not traced yet [A]. Particle mass = restDensity / restParticlesPerMeter^3. Other probe results: dynamic restitution is used
for dynamic shapes; at a static/dynamic seam the static response wins (static resolves last); no box response without
both the fluid flag and the shape flag; kinematic boxes are not pushed and the particle leaves at v_box + e(v_box - v_p).
Applied only when **all** hold [V]:
- fluid flag `NX_FF_COLLISION_TWOWAY` (fluid object +0x3b8 bit 2) and `collisionMethod & NX_F_DYNAMIC` (NfFluid slot 49 -> +0x14c);
- a scene flag word bit 0 (`scene+0x41c`, set at construction and never cleared in the places searched) [A: always true];
- per particle (iterating the read-back arrays): shape id != 0, internal flag 4 clear (not drained), flag 2 set (dynamic contact);
- the shape (handle table lookup, `0x101705b0`) has `NX_SF_FLUID_TWOWAY` (`shape+0x15c & 0x800`) and a body (`shape+0x134`);
- body is not kinematic (`body flags & 0x80`), unless a fluid-level byte at fluid +0x79 is non-zero [A: that byte is a "also push
  kinematic bodies" setting; not located].

Then, for particles whose stored vector is non-zero:

    impulse = storedVector * collisionResponseCoefficient * particleMass     applied with NX_IMPULSE at the particle position

- `collisionResponseCoefficient` = NfFluid slot 60 (+0x12c) (NxFluid set/getCollisionResponseCoefficient = NpFluid slots 54/55). [V]
- `particleMass` = NfFluid slot 77 = NxFluid::getParticleMass (NpFluid slot 67) = (restDensity * rest spacing^3) [V that
  the slot is the mass getter, A for the formula; the init uses restDensity * 0.1^3 in simulation space].
- The call is the body interface's vtable +0x28 with arguments (vector*, position*, mode 1), i.e. an impulse at a point
  (not a continuous force; `NX_IMPULSE` = 1). The position is the particle's end-of-step position.
- **What the stored vector is**: in two-way mode, instead of the collision normal, the +0x20 slot of the particle record holds
  `v_before - v_after` of the *dynamic pass only* (`0x1019b5d0`, dynamic call; cached-plane path: displacement change x 1/dt),
  and +0x38 holds the contact shape id. The momentum lost by the particle is given to the body. Static contacts store neither.
  That is why NxParticleData collision normals are unavailable when TWOWAY is set.
- The **drag/force-transfer flags** that matter: fluid flag TWOWAY (4), fluid `collisionMethod` DYNAMIC (2), shape flag
  `NX_SF_FLUID_TWOWAY` (1<<11), coefficient (default 0.2). `NX_SF_FLUID_DISABLE_COLLISION` (1<<10) and actor flag
  `NX_AF_FLUID_DISABLE_COLLISION` (1<<3) remove the pair; `NX_SF_FLUID_DRAIN` (1<<8) deletes particles that touch the shape.
- Mirror's Edge's PhysXParticleSystem sets TWOWAY from its own bit 3 (see 4), so two-way coupling is available to content,
  but whether any asset enables it is not checked (see tests). Rigid bodies stay on the original CPU path, so the shim has
  to keep calling this impulse path itself if it replaces the fluid solver: the same vtable +0x28 call on the real actor.
- **Why the impulse is spread over steps (static read, session 6; no runs) [V unless marked]:**
  - The call at `0x100ff714-0x100ff749` is `mov eax,[ecx]; call [eax+0x28]` with (force, pos, **1 = NX_IMPULSE**). NX_SMOOTH_IMPULSE is 3 (`third_party/physx-sdk-2.8.1/SDKs/Physics/include/Nxp.h:313`).
  - Body vtable 102dc9cc slot 10 (`0x10002d30`) turns the point impulse into linear + torque ((p - COM) x F) and calls slot 11 (`0x10003a80`) with the same mode.
  - `0x10003a80` case 1 (NX_IMPULSE) changes velocity at once: v += F * invMass (+0x214), angular part via inverse inertia, then queues the body (`0x10095230`). Case 3 (smooth) fills separate accumulators (+0x254..+0x268) instead; the fluid does not use it. The consumer of those accumulators is not located.
  - The body side does not smooth the impulse: no 0.1 or 0.9 constant exists on that path (grep of float literals). The only 0.9/0.1 filter is `0x10006e40` (sleep energy, flag 0x400).
  - So the 10%/0.9 split comes from the particle side. `0x1019b5d0(rec,1,1)` divides the stored vector and displacement by an integer count at record+0x1c (`param_1[7]`). What that count is, and where the geometric share comes from, is not traced [A].
  - For the shim [A]: apply each step's impulse at once through the same NxActor call (addForceAtPos(F, p, NX_IMPULSE)), with no body-side smoothing. Reproduce the split from the per-step stored vector and count, then compare with `bodies-1.bin`. Use NX_SMOOTH_IMPULSE only if that fails.

## 3. ABI: NxFluidDesc / NxParticleData / NxFluid for the shim

### 3.1 NxFluidDesc is the same layout in 2.8.0 and 2.8.1 [V]
Verified from `NpFluid::loadFromDesc` (`0x1016afc0`), `NpScene::createFluid` (`0x1014d7d0`) and the 2.8.1 equivalents (same dword
indices in both: 0x10..0x2d, 0x35..0x4e, 0x53..0x55). Compiled 2.8.1 header layout (clang, i686 MSVC): `NxFluidDescBase` = 336
bytes (84 dwords), `NxFluidDesc` = 352 bytes (adds `NxArray<NxFluidEmitterDesc>` at dword 84: begin, end, ...),
`NxFluidEmitterDesc` = 0x74 bytes (the stride 0x74 is hard-coded in the 2.8.0 core), `NxParticleData` = 64 bytes (8 pointers,
7 strides, name), `NxParticleIdData` = 16, `NxFluidPacketData` = 8.

| dword | field |
|---|---|
| 0x00..0x0f | initialParticleData (numParticlesPtr, bufferPos, Vel, Life, Density, Id, Flag, CollisionNormal; 7 strides at 0x08..0x0e; name 0x0f) |
| 0x10 / 0x11 | maxParticles (1..0xffff), numReserveParticles (< max) |
| 0x12..0x17 | restParticlesPerMeter, restDensity, kernelRadiusMultiplier (>=1), motionLimitMultiplier, collisionDistanceMultiplier, packetSizeMultiplier (pow2 >= 4; motionLimit and collisionDistance <= packetSize * KRM) |
| 0x18..0x1c | stiffness, viscosity (>0), surfaceTension, damping, fadeInTime (>=0) |
| 0x1d..0x1f / 0x20..0x23 | externalAcceleration / projectionPlane (normal must be non-zero) |
| 0x24..0x2c | restS, dynFrS, statFrS, attrS, restD, dynFrD, statFrD, attrD, collisionResponseCoefficient (all validated, 0..1 for rest/friction) |
| 0x2d / 0x2e / 0x2f | simulationMethod (exactly one of SPH=1 / NO_INTERACTION=2 / MIXED=4), collisionMethod, collisionGroup (u16, < 32) |
| 0x30..0x33 / 0x34 | groupsMask / forceFieldMaterial (u16; not validated in 2.8.0) |
| 0x35..0x44 | particlesWriteData (NxParticleData) |
| 0x45..0x48 / 0x49..0x4c | particleDeletionIdWriteData / particleCreationIdWriteData (numIds*, bufferId*, stride, name) |
| 0x4d / 0x4e | fluidPacketData (bufferFluidPackets, numFluidPacketsPtr: buffer first [V, shim trace 2026-10-09]) |
| 0x4f | flags (NxFluidFlag) [A: not read in the code paths looked at; same slot as in the 2.8.1 header] |
| 0x50 / 0x51 / 0x52 / 0x53 | userData / name (passed to setName, vtable +0x130) / compartment (NxCompartment*; its type field must be 1 = fluid, checked in `0x1010f1b0`) / type (0 = default NxArray, 1 = custom allocator) [V] |
| 0x54 / 0x55 | emitters.begin / .end (NxFluidEmitterDesc, 0x74 each, validated < 0x10000) |

So **the 2.8.1 `NxFluidDesc`, `NxFluidEmitterDesc` and `NxParticleData` headers can be used to read what the game passes to
`createFluid`** [V on every dword the 2.8.0 core touches; the only 2.8.0-vs-2.8.1 difference seen is that 2.8.0 also
range-checks the two static-friction fields, 2.8.1 skips them]. The game never sets static friction or attraction (the builder
leaves those defaults, see 4).
- Defaults substituted by the core's `loadFromDesc` for the LL descriptor are the usual ones (0x42480000 = 50 particles/m,
  0x447a0000 = 1000 density, 0x3f99999a = 1.2 KRM, 0x40666667 = 3.6 motion limit, 0x3df5c290 = 0.12 cd, 16 packet, 6 stiffness).
- `NxFluid` interface: 81 slots in both, slot order identical (NpFluid vtable 82 entries incl. the destructor).
  2.8.0 stubs (getters return 0, setters ignore) for NxFluid slots 42-45, 50-53 (static friction / attraction). So a wrapper
  must **store** those values itself if it wants `getStaticFriction…` to round-trip; the real core won't. [V from the earlier
  notes, slots as `NxFluid.h` indices.]
- `NxFluid::addParticles(const NxParticleData&, bool appendIds)` (slot 5), `getParticlesWriteData()` (slot 8, returns the 64-byte
  struct by hidden pointer) and the `NxParticleData` member order are the same, so the shim can forward them.

### 3.2 NxScene::createFluid and the SDK desc
- `NxScene::createFluid(const NxFluidDescBase&)` is **vtable slot 126** in 2.8.0 *and* 2.8.1 (releaseFluid 127, getNbFluids 128,
  getFluids 129, cookFluidMeshHotspot 130, createCloth 131, releaseCloth 132, getNbCloths 133, getCloths 134, createSoftBody
  135, releaseSoftBody 136; also createMaterial 36, createCompartment 38, setFluidUserNotify 93, setClothUserNotify 95).
  Mapped by the `NpScene::` strings in both decompilations. [V]
- The NxScene tables **diverge only at the end**: 2.8.0 `setSolverBatchSize` is slot 153; 2.8.1 inserts
  `setDynamicTreeRebuildRateHint` at 153 (+ its getter), pushing `setSolverBatchSize` to 155 and growing the vtable 155 -> 157.
  Slots 0..152 are identical. So a wrapper or a vtable hook for fluid/cloth is unaffected by the shift; a full `NxScene`
  wrapper written from the 2.8.1 header must not forward slots >= 153 by number.
- The `NxScene` entry used by the game goes through Epic's statically linked `Nxd*` wrapper layer inside MirrorsEdge.exe
  (`NxdScene.cpp`, `NxdFluid.cpp`: "Attempt to create fluid during simulation", ...), which calls the loader-supplied NxScene.
  The shim sees the real interface calls, with `createFluid` taking the 336-byte `NxFluidDescBase` plus emitters.
- `NxPhysicsSDKDesc` (not covered by this task beyond the earlier note): the game's 7-dword block is consumed by the loader/core
  creation path; the shim should forward it byte-for-byte rather than type it as the 2.8.1 5-dword struct. The meaning of
  dwords 5 and 6 (GpuHeapSize, MeshCacheSize) is [A].
- The fluid LL step reads `NfFluid +0x104` for a parameter struct (mass at +0xe0); nothing a shim needs.

## 4. Mirror's Edge: UE3 units -> PhysX metres (read-only, MirrorsEdge.exe at `Binaries/MirrorsEdge.exe`)
- UE3 `U2PScale = 0.02f` (1 uu = 0.02 m) and `P2UScale = 50.0f` exist as paired float constants in .rdata, one copy per
  translation unit (the pairs at 0x1c5c23c/0x1c5c240, 0x1c69284/88, 0x1c6a454/58, 0x1c898b4/b8, 0x1ca1f10/14, ...). [V constants]
- The fluid desc is built by `FPhysXParticleSystem` code at **0x00e8xxxx** (the builder that writes the NxFluidDesc in place,
  `0xe86a99` region). From it [V from disassembly; UE property names from the class's reflection strings, A for the exact pairing]:
  - `restParticlesPerMeter = 1.0 / (RestParticleDistance * 0.02)` (`mulss … [0x1c898b8]` = 0.02, `divss` of 1.0). So
    `RestParticleDistance` is in **uu**.
  - `motionLimitMultiplier = MaxMotionDistance / RestParticleDistance` and `collisionDistanceMultiplier = CollisionDistance /
    RestParticleDistance` (both uu, so the scale cancels; defaults in the struct are 0.12 and 3.6 before being overwritten).
  - `externalAcceleration = u2PV(vector)` with `0xf8c510` = vector x 0.02 (copy at 0x1ca1f14). Acceleration property is in uu/s^2.
  - Direct copies: restDensity, kernelRadiusMultiplier, stiffness, viscosity, damping, collisionResponseCoefficient,
    restitution/dynamic-friction for static and dynamic shapes, maxParticles, numReserve (priority mode bit when > 0).
    Static friction and attraction stay at the descriptor default (0).
  - simulationMethod: property 0 -> SPH, 1 -> NO_INTERACTION, 2 -> MIXED. packetSizeMultiplier: enum 0..5 -> 4, 8, 16, 32, 64, 128.
  - flags: VISUALIZATION | ENABLED always, TWOWAY when property bit 3, DISABLE_GRAVITY when bit 1, PRIORITY_MODE when reserve > 0,
    HARDWARE when a "PhysX hardware available" helper (0xf8c2b0) and a compartment condition hold. collisionMethod: STATIC when
    bit 2, DYNAMIC when bit 0.
- So positions and level geometry must also be at 0.02 m/uu for consistency [A: not yet traced in the particle add/read
  path or the mesh cooking path]; the earlier solver assumption `1 uu = 0.02 m` is **confirmed for the descriptor**.
  Replace the 1 uu = 0.02 assumption in the scenario converter with `RestParticleDistance_uu * 0.02`.

## 5. Confidence summary
| item | confidence |
|---|---|
| mesh pipeline structure (cell hash, OBB per cell, triangle list, per-triangle test, merging) | high [V] |
| exact triangle test formulas, thresholds (1e-5, 1e-4, strict compares, face-only proximity) | high [V], untested against an oracle |
| impact puts the particle at its start position (mesh/heightfield) | high from disassembly, **needs an oracle check** |
| one-sided behaviour | high for the code, [A] for what the game's meshes look like |
| dynamic-first, static-second | high [V] |
| two-way impulse formula (Δv x coefficient x mass, NX_IMPULSE at the particle) | high structure, medium on "mass" being exactly rho s^3 |
| NxFluidDesc layout identical to the 2.8.1 headers | high for all fields the core reads |
| NxScene createFluid/createCloth slots identical | high |
| unit scale (0.02) in the descriptor | high; in the geometry path [A] |
| proximity = 2 cd vs ~cd | open (conflict) |

## 6. Tests the solver implementer should write
Oracle: extend `harness/src/fluid_oracle.cpp` with a **triangle mesh shape** (cooking lib 2.8.1 is available, as for the cloth grids)
and run with static friction 0 as before. Then, as probes (single particle unless stated):
1. Free fall onto a large horizontal quad (two triangles): matches the plane probes (collision band, cache plane, rest height cd).
2. Same, with the start behind the face (particle below an upward-facing mesh): must pass through unaffected (one-sidedness);
   and a mesh with flipped winding: particle collides from the other side only.
3. Fast particle (step > cd..motionLimit) crossing a face interior: check impact t, post-step **position = start position**,
   velocity reflected about n (restitution/friction as for the plane), cache plane at face + cd.
4. Hit exactly near a triangle edge: inside/outside the 1e-4 barycentric slop (just inside, just outside by 2e-4); check which
   triangle of a shared edge reports the impact.
5. End point within cd of an **edge** and of a **vertex** (outside the face region) with cd < 3.16 mm and again with cd > 3.16 mm
   (larger kernel): face normal vs radial normal; push-out distance; no cache plane for the edge/vertex case.
6. End point at 1.2, 1.5, 1.9 x cd above the face interior (prox/cd question): is a cache plane created? Settles the 2 cd
   vs 1 cd conflict for meshes.
7. Particle near the shared edge of two coplanar triangles: averaged push-out equals the single-triangle result; and a convex
   fold (two triangles at 90 deg): averaged normal after normalisation, count = 2.
8. Many particles in several cells of a big mesh with distinct triangles: confirm the cell box margin: a particle at the cell
   corner moving at the motion limit toward a triangle just outside the cell still impacts (box half-extent h_w/2 + ml + cd).
9. Cell hash: a batch with > 256 distinct cells (table cap 512) and > 300 particles in one packet (batching) must give the same
   result as particle-by-particle (no cross-talk).
10. Statistical: a SPH jet hitting a mesh staircase with `fluid_metrics.py` (centroid, extent, settle time).
11. Two-way: a dynamic box (NX_SF_FLUID_TWOWAY) hit by a particle stream; compare the box's momentum change per step with
    `sum(dv x m x coef)`; test the gating conditions one by one (no TWOWAY flag, no shape flag, kinematic body, DYNAMIC off) and
    that static contacts transfer nothing; check the odd/even step behaviour in mixed mode (the post-step call sits before the
    phase flip but is not gated by the phase).
12. Dynamic vs static order: a particle squeezed between a moving dynamic box and a static plane; result differs if the order is
    swapped (static last).
13. Unit-conversion regression: feed UE3 values (RestParticleDistance 5 uu -> 10 particles/m) through the scenario converter.
14. Descriptor ABI: build a `NxFluidDesc` from the 2.8.1 header, pass it to a core-2.8.0 `createFluid` through the shim, read it
    back via `saveToDesc`; round-trip every dword incl. emitters; verify NX_FDT_DEFAULT and the invalid-desc rejection list in 3.1.

## 7. Bumpy mesh contacts (session 7; probes `scenarios/fluid/probe_meshedge/`, captures `captures/fluid/hfield_tri_A`, `me_*`)

Setup: oracle = 2.8.1 core with matched headers (as elsewhere). Ours = `solver/fluid/fluid.cpp` compiled natively
(a local `fluid_run` test build), plus a copy with `kMeshImpactRestoresStart = true` (`fluid_run_restore`). All runs
single particle unless stated, hfield_drop settings (restitution 0.2, dynamic friction 0.15, static friction 0, damping 0.05,
cd = 0.0024 m, dt 1/60). Scale: `hfield_tri_A.txt` is hfield_drop's heightfield as 3042 `tri` lines (2 per cell,
diagonal by `(r+c)&1`, normal +Y). Its oracle run equals `captures/fluid/hfield_meshref` exactly through step 138.
Caveat: `quadgrid` does not exist in the oracle; the grid is written as `tri` lines.

### 7.1 What diverges, and why [V]
1. **The impact position is the decisive term.** Our default places a mesh impact at hit + n·cd (line
   `solver/fluid/fluid.cpp:79`, switch used at `:436` and `:378`). The original puts it at the start position (section 1.4).
   With the switch on, the speed gap closes. Six jittered seeds (`hfield_tri.j1..j6`, `jitter 1e-5 s`), speed_p95 at the last
   step: oracle 3.164-3.165, ours default 1.918-1.952, ours with restore 3.164-3.165. Mean over steps is 3.38 for the oracle
   and 3.34 for ours default, so the gap is in the late phase (settled particles on the slope), not the free fall.
2. **Single-particle probes**, first divergence (0-based step), default -> restore:
   - `me_ridge_crest_drop` (drop on convex crest, shared edge): 18 -> identical for 60 steps.
   - `me_valley_drop` (concave valley): 27 -> identical. `me_apex_drop` (vertex of a 4-triangle pyramid): 18 -> identical.
   - `me_flat_diag_drop` (on the shared diagonal of two coplanar triangles): 27 -> identical.
   - `me_ridge_crest_slow` (start 1 mm above the crest, inside cd of both faces; one swept impact at step 0): 0 -> identical.
   - `me_ridge_under_up` (up through the ridge from below): identical in both: one-sidedness holds. [V]
   - `me_ridge_crest_slide` (slides along +x on the left face, 1 mm above): default matches to step 18, restore differs at 19
     (1.3 mm). `me_ridge_face_drop` (left face, slides down slope): restore differs at 53 (1.0 mm). Both are cases **after**
     the first impact, see 7.2.
3. **Tilted-plane retention, not mesh-specific [V].** `me_plane_tilt_ref` is the left face as a static plane. The oracle
   and ours agree up to the first impact (step 22), then differ at step 32 (3.5 mm). At step 31 the particle is at
   0.002402 m from the face = 1.0008 cd. The oracle keeps the contact (its dump shows the contact flags/normal); ours
   does not. Ours keeps a cached plane that was not hit only while `n·target - d <= collDist` (`fluid.cpp:1516`, the
   retention block; `keepCache_` = true by default, `fluid.h:312`). The original's band is wider than 1.0 cd here. Earlier notes
   (`mesh_drop_plane` needs < 1.2e-5 cd) are about *new* planes, not retained ones, so both can be true. [A on the exact band]
4. **Exact-edge tie decides a different triangle [V].** In `hfield_tri_A`, particle 970 (the one divergent particle at step 138)
   lies exactly on the edge x = -5.5 shared by cell (8,9) (normal (0, 0.929, -0.371)) and cell (9,9) (normal
   (-0.348, 0.870, -0.348)). Both accept it (the edge is inside the 1e-4 slop) and their float t values differ in the 8th
   digit (0.91933316 vs 0.91933334 when computed in float with the same formula). The oracle reflects off the cell (9,9)
   normal, ours (first in index order, strict `<` at `fluid.cpp:377`) off cell (8,9). Changing our dot-product order to the
   decompiled order (y,z,x for num; x,z,y for den) does **not** remove it, so the tie is not an operation-order artifact.
   Candidate: the cooked tree visits (9,9) first and the oracle keeps the first triangle on exact ties of its own t. Lattice
   particles on grid lines are the only case seen; not a general rule yet. [A]

### 7.2 Rules (what the original does), with the status of our solver
| question | original (section 1, re-checked) | ours | status |
|---|---|---|---|
| several triangles in contact (collision) | normal and point **summed**, count++; resolved as one averaged contact | `fluid.cpp:337-346` same | matches (`me_ridge_crest_*`, `me_flat_diag_drop`) [V] |
| edge / vertex push-out normal | radial D/|D| unless dist² <= 1e-5 m² (absolute); with cd < 3.16 mm always the face normal | `fluid.cpp:342` same | matches [V] (the 1e-5 test always fires at cd = 2.4 mm) |
| impact, several triangles | earliest t wins (strict `<`), flags reset to impact, collisions cleared, cache planes kept | `fluid.cpp:351-381` same | matches except the exact-tie choice (7.1.4) |
| impact position | **start position** (`old`), velocity reflected by the winning normal | hit + n·cd (`:79`, default) | **mismatch: the cause of the speed gap** (7.1.1) |
| restitution / friction with merged normals | same response as planes; cache planes give the contact on the next step | same | matches on the probes that do not hit the retention band (7.1.3) |
| triangles that count (one-sided) | front side only for collision and proximity; impacts only from front to back | `fluid.cpp:330-336, 351` same | matches (`me_ridge_under_up`) [V] |
| closest point | the decompiled test does **not** consider the v1-v2 edge in one branch (section 1.3) | exact closest point `closestOnTriangle` | differs in a sliver; not probed here [V code, A effect] |
| proximity (flag 0x10) | cached plane = n·cd, face region only | `fluid.cpp:346-347` | not probed in this session |

### 7.3 Implementation-ready change (for the solver owner; needs the user's decision on 1)
1. **Set `kMeshImpactRestoresStart = true`** (`solver/fluid/fluid.cpp:79`). The 2026-10-08 "artifact" decision was taken before
   this evidence. With it, all impact probes match the oracle, and the six-seed speed gap closes (3.16 vs 1.95). Recommend
   the user revisit that decision. The probes show the restore is what the original does on every impact seen here, so
   with the switch off the solver differs from the original on each mesh impact.
2. **Retention band**: for the plane shape, the cached plane should survive while `n·target - d` is below about 1.0008 cd
   (exact value unknown). Find it with a probe: after an impact on a tilted plane (`me_plane_tilt_ref`), end points at
   1.0, 1.0005, 1.001, 1.002, 1.005, 1.01, 1.1, 1.5 cd above the face; the first distance that is dropped gives the band.
   Test candidate `<= 2·cd` (the box/sphere/capsule proximity distance) against `probe_rebound` and `mesh_drop_plane` before
   accepting; do not fit it to one probe.
3. **Ties**: leave as is for now (index order, strict `<`). Do not change until the cooked tree order is checked (it may be
   the mesh's cooked triangle order; the oracle keeps the cell (9,9) triangle).

### 7.4 Still open
- The restore probes after the first impact (`me_ridge_crest_slide`, `me_ridge_face_drop`) differ by 1 mm, and item 7.3.2 is
  their likely cause [A, not yet run with the band fixed].
- Crest slide across the crease was not reached in 60 steps (the particle stayed on the left face); the crease-crossing
  probe needs a higher start or a longer run. No proximity probes on meshes this session.
- Oracle note: impact dumps have normal 0 (the oracle's normal field is not the impact normal), so compare velocities for
  impact normals, not the dump's normal field.

## 8. Hybrid landing (session 8, 2026-10-10): tested, not adopted

Experiment goal: keep landing at hit + n·cd (no hover), but stop the bounce energy loss that the hovering
original does not have. Result: **the hybrid cannot match the 6-seed hfield_tri ensemble with a local rule, and the
rules that would close part of the gap break oracle-exact probes.** Production `solver/fluid/fluid.cpp` is unchanged by
this session. Instrumented test copies were local-only, not production code.

### 8.1 Where the speed goes after a landing [V, traces of particle 973 in hfield_tri.j1]
- The impact itself is identical to the oracle: same velocity after the bounce (speeds equal at steps 138-141).
  The only difference is position: ours sits cd above the face, the oracle 5.3 cm higher (its restored start).
- The 5.3 cm offset changes when the particle touches again. Ours bounces with e·|vn| = 0.19 m/s, gravity removes it
  within one step, so it re-touches at step 147 (its normal velocity is -0.15 m/s again) and then stays in tiny
  contact hops (flag 5 at 147-156). Each contact frame scales the tangential speed by (1 - 0.15), as the oracle's
  sliding probes show. The oracle's copy of the particle is in free flight in those steps and touches again only at 152.
- So the deficit is the *number of contact frames*, caused by the hover geometry, not by an extra friction law.

### 8.2 Candidate rules, tested on the 6-seed hfield_tri ensemble (speed_p95 oracle 3.164, ours default 1.935)
| variant | speed_p95 | speed_p50 (oracle 0.211) | verdict |
|---|---|---|---|
| default (current solver) | 1.935 | 0.19 | baseline |
| friction/restitution gated on pre-integration vn >= 0 (all particles) | 3.064 | 0.79 | rejected: resting particles lose all friction |
| no friction on the first re-contact after a fresh landing (not in contact last step, separating) | 2.18 | 0.19 | rejected: changes oracle-exact probes (below) |
| same, held for W steps after a real impact (W = 3 / 6 / 12 / 30) | 2.18 / 2.49 / 2.87 / 3.05 | 0.19 / 0.20 / 0.24 / 0.74 | rejected: W is a fitted knob; p50 breaks at W >= 30 |
| restitution gated on pre-integration vn (swept impacts) | no change at the tilted-plane divergence | - | rejected |

Probe check of the first-landing gate (vs the baseline solver, same binary otherwise): changed outputs include the
oracle-exact `slide_sf*_df*_e0.5` (8 probes, dpos up to 1.2e-2), `rebound_0.020-0.048` (8), `mesh_drop_plane`
(1.2e-3), `me_ridge_crest_drop` (2e-2) and `dyn_kin` (3e-4). The oracle shows touchdown friction on these, so the
"no friction on touchdown while separating" rule is wrong for single particles. [V]

### 8.3 Tilted-plane retention threshold (me_plane_tilt_ref; oracle run captured this session)
- Retention band sweep (retained if the end point is within X·cd of the face, X = 1.0 ... 2.0, with and without
  keeping planes that were hit this step): no change in the first divergence (step 32 always, or 35 with hit planes).
  Retention is **not** the cause of this probe's divergence. [V]
- The divergence at step 32 is a grazing decision: both sides end at the same distance (1.00001 cd from the face
  with the scenario's plane offset), but the oracle's velocity after that step has no normal bounce (vn = 0) and its
  position is at old + 0.89·motion, while ours takes an impact at t ≈ 0.996 with a 0.2·|vn| bounce. Making the
  restitution depend on pre-integration vn does not remove the 3.5 mm position offset. [V] Mechanism of the 0.89
  fraction: not found [A: a clamp of the step's motion at contact, not located].
- The "1.0008 cd" in 7.1.3 is not reproduced with this probe's plane offset (I get 1.00001 cd at step 31); the
  retention band value in 7.3.2 is therefore **not established**. Do not set a retention constant from this probe.

### 8.4 Decision (deliberate deviation from the user's hybrid, to be confirmed)
- Not implemented: no change to `kMeshImpactRestoresStart` (still false, `fluid.cpp:79`) and no retention constant.
- Ensemble with the current solver: speed_p95 1.94 vs 3.16 (fails). Restoring the start position gives 3.16 vs 3.16
  (passes) [V, section 7.1]. The original's ensemble therefore depends on the hover; a local bounce rule does not reproduce it.
- If a landing on the surface is still required, the remaining option is a fuller contact model for resting
  and hopping particles (the oracle's 0.89-fraction contact step, section 8.3), which is an open RE item.
