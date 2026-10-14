# RE notes: CPU fluid (PhysXCore 2.8.0.45)

Descriptions of behaviour, written in our own words. No NVIDIA code is copied here.
Addresses refer to the Ghidra project `re/ghidra/physx280`, image base 0x10000000.
[V] means read in the disassembly or decompilation; [A] means an interpretation still to be confirmed
by a harness test.

## Source layout (from assert paths) [V]
- `LowLevel/software/src/PxsFluid.cpp`, `PxsFluidDynamics.cpp`, `PxsFluidSpatialHash.cpp`: CPU SPH and collision
- `SDKs/Core/Common/src/fluids/Fluid.cpp`: shared fluid object
- `SDKs/Core/Hardware/src/fluids/Nf*`: PPU/"hardware" abstraction, including `NfFluidPPUInterfaceSwRef`
- `SDKs/Core/Gpu/src/fluids/Ng*` + `LowLevel/gpu/.../Pxg*`: CUDA path. Kernels are SASS sm_10..sm_50, with no PTX.

## SPH neighbour pass: `0x10198c80` (PxsFluidDynamics.cpp) [V]
- One function, two modes selected by the first argument: 0 = density accumulation,
  1 = force accumulation.
- Particles are stored in 64-byte records (16 floats) and grouped into **packets** (spatial cells).
  A packet's own particles interact pairwise; then it interacts with up to **26 neighbouring packets**
  (3×3×3 minus self), driven by a static offset table at `0x10326de0`.
- When both packets hold ≤100 particles, it does direct O(n·m) pair loops. Otherwise it builds a
  temporary hash (power-of-two table sized from the particle count, capped at 0x7fff) and processes
  sub-blocks (`0x1019e660` builds the hash, `0x10196c40` handles same-block pairs, `0x10197840` cross-block pairs).
- Pair test: `r² < h²`, with the kernel constants held in a parameter block (`this+0x1c..0x40`).
- **Density mode:** each particle of a pair gains `c · (k − r²·s)³`. That is a poly6-type kernel
  (Müller et al. 2003), with the constants pre-scaled (k, s, c at +0x24, +0x40, +0x28).
- **Force mode** (per pair, with r = |d| scaled):
  - **Pressure:** a term proportional to `(p_i + p_j) · (k/r − 2h + r)` along d. This is the spiky
    gradient with the pressure average folded in.
  - **Viscosity:** a term proportional to `(h − r) · (v_j − v_i)`. That's the viscosity Laplacian.
  - Each contribution is divided by the *other* particle's density and applied with opposite sign
    to the two particles, so momentum is conserved.
- Record field offsets used: position 0..2, density 3, velocity 4..6 [A], force accumulator 8..10 [A],
  pressure-ish at 14 [A].

Interpretation: the textbook Müller 2003 SPH (poly6 density, spiky pressure, viscosity Laplacian)
on a packet grid. Next: find the parameter-block setup (how RestParticleDistance, KernelRadiusMultiplier,
Stiffness, Viscosity and RestDensity become the constants), the pressure equation of state, the
integrator, and the collision passes.

## Density and force drivers [V]
- `0x10199640` `re_sphDensityPass`: each particle's density starts at the self-contribution
  (`params+0x10`, i.e. the kernel at r=0). It then visits up to **1024 packets** (10-byte packet records;
  a 0x6c-byte neighbour list per packet) and runs the pair pass in density mode.
- `0x10199700` `re_sphForcePass`: clears the force accumulators, then computes per-particle pressure
  `p = max(0, (density − restDensity) · stiffness)`, with restDensity at `params+0xc` and stiffness at
  `params+0x34`. **Pressure is clamped at zero**: no negative pressure and no attraction, a
  stability choice we must reproduce. Then it runs the pair pass in force mode and finishes with
  `0x10198b00` (post-process over all particles, likely force → acceleration scaling [A]).
- Packet setup per packet: `0x1019cc10` builds the 26-neighbour list.

## Low-level fluid init: `0x1017be80` `re_PxsFluid_initFromLLDesc` [V for formulas, A for field names]
Takes a low-level descriptor (built from NxFluidDesc in Fluid.cpp, mapping still to do) and precomputes
constants. Literal constants read from the binary: 0.001, 0.1, 315, 64π (201.0619), π, 22.5, 45.
- The simulation runs in a **normalised space**. The kernel radius is `h = 0.1 · K`, where K is a desc field
  (likely KernelRadiusMultiplier [A]). This suggests rest particle spacing maps to 0.1 in sim space [A].
- Particle mass `m = 0.001 · R` (R = desc field, likely RestDensity [A]). 0.001 = 0.1³, which is consistent
  with mass = restDensity · spacing³ at spacing 0.1.
- Kernel constants, all multiplied by m:
  - density (poly6): `315 / (64π h⁹)`
  - pressure gradient: `22.5 / (π h⁶)` (spiky 45/(πh⁶), halved for the (pᵢ+pⱼ)/2 average)
  - viscosity: `45 / (π h⁶) · viscosity`. `setViscosity` (`PxsFluid_vf20`) recomputes only this term.
  - self density W(0)·m = `315/(64π h⁹) · h⁶ · m`
- Other derived values: K/D and its square (D = another desc field, likely rest spacing [A]), packet size
  `1 << shift` (PacketSizeMultiplier), and collision distances scaled by 1/D with a +0.001 epsilon.

## Velocity integration: `0x10198b00` `re_fluidIntegrateVelocity` [V]
Per particle, with dt the step:
1. In SPH mode, `force /= density`, then `a = force · forceScale + extAccel`. Without SPH, `a = extAccel`
   (gravity plus ExternalAcceleration, presumably pre-summed).
2. Save the old velocity into the particle record (slot +0x20, the same slot the API exposes as
   collision normal; it gets reused later).
3. `v += a · dt`
4. Damping: `v *= max(0, 1 − damping · dt)` (linear, not exponential).
5. Motion clamp: if `|v| · dt > maxMotionDistance`, rescale v so that `|v| · dt = maxMotionDistance`.
Particles are processed in chunks of 0x7fff. Position update and collision come after this, in a separate pass.

## Particle record (64 bytes), from the read-back view `PxsFluid_vf4` [V offsets, A names]
+0x00 pos (3f) · +0x0c density · +0x10 vel (3f) · +0x1c lifetime · +0x20 collision normal / old vel (3f)
· +0x2c id (u16) · +0x2e flags (u16) · +0x38 (unknown, exported as 8th stream)

## Low-level descriptor → constants (field names matched against 2.8.1 NxFluidDesc order) [V offsets, A names]
LL desc byte offsets (the decompiler shows them as ushort* indices; the byte offsets below are ×2 of those):
0x00 maxParticles (u16) · 0x04 restParticlesPerMeter (ρₚ) · 0x08 restDensity · 0x0c kernelRadiusMultiplier ·
0x10 motionLimitMultiplier · 0x14 collisionDistanceMultiplier · 0x18 packetSizeMultiplier (as shift) ·
0x1c stiffness · 0x20 viscosity · 0x24 surfaceTension? · 0x28 damping · 0x2c fadeInTime ·
0x30 externalAcceleration (3f) · 0x3c..0x54 7 collision floats (restitution / friction / … static & dynamic) ·
0x58 ? (passed to the integrator as a time value) · 0x5c simulationMethod (1 = SPH) ·
0x60 projectionPlane (n, d) · 0x70 flags (bit1 = project to plane).
Derived (with spacing `s = 1/restParticlesPerMeter`):
- kernel radius (world) = kernelRadiusMultiplier · s
- world→sim scale = 0.1 / s, so the **rest spacing becomes 0.1 in sim space**; h_sim = 0.1 · kernelRadiusMultiplier
- particle mass = restDensity · 0.1³ (sim units)
- motion limit and collision distance are their multipliers · s (plus a 0.001 epsilon)
These names come from field order and get confirmed by harness tests (set distinct values, read the effect back).

## Collision (CPU), per packet in batches of 300: `0x1019bec0` `re_fluidCollidePacket` [V]
1. motion = v · dt. If the particle has a cached contact from the last step (index ≠ 0x7fff), resolve
   it first (`0x10199aa0`). That sets particle flag bit 0 (static contact) or bit 1 (dynamic contact).
2. Target = pos + motion.
3. Collide against **static shapes** (shape flag bit 2 clear) via `0x1019b800`, then resolve, then against
   **dynamic shapes** (bit 2 set), then resolve again. Body poses are taken at the start and end of the step
   (two transforms per shape), so moving shapes sweep.
4. Clamp the displacement to the motion limit.
5. Optional projection onto a plane: if fluid flag 2 is set, project both velocity and position onto plane n·x+d=0.
6. Write back pos, vel, the collision normal into +0x20, and the contact cache for the next step.

## Contact resolution: `0x1019b5d0` `re_fluidResolveParticleContacts` [V]
- With several contacts, the normals and surface data are **averaged** (divided by contact count) and the
  normal renormalised. With a single contact, it's used directly.
- Static contacts use (restitution, friction) = params[0], params[1]; dynamic contacts use params[3], params[4].

## Velocity response: `0x101998c0` `re_fluidContactVelocityResponse` [V]
With n the contact normal, v the particle velocity and vₛ the surface velocity, vr = v − vₛ and vn = n·vr:
- if vn ≥ 0 (separating): unchanged
- else: `v' = vₛ + (1 − friction)·(vr − n·vn) − restitution·vn·n`
That's the standard reflect-plus-tangential-damping model; friction is applied as a direct scale on the tangential part.

## Shape tests: `0x1019b800` `re_fluidCollideShape_dispatch` [V structure, A type names]
- Each particle's old position and target position are transformed into the shape's local frame, using
  both the start-of-step and end-of-step poses. That's continuous collision against moving shapes.
- Then it dispatches on an internal geometry type code: 1 → `re_fluidVsSphere` (0x101ac350),
  2 → `re_fluidVsPlane` (0x101ac060, local x is the normal), 3 → 0x101abb80 (box [A]),
  5 → 0x101ab730 / 6 → 0x101ab600 (capsule / convex? [A]), 7 → 0x101a9de0 (triangle mesh: particles are first
  hashed into ≤512 buckets for the query) [A], 8 → 0x101aa130 (657 lines, heightfield or convex [A]).
- Every test receives two distances from the fluid: **collisionDistance** (≈ multiplier · spacing) and a
  larger **proximity distance**. It classifies each particle into contact flags:
  - **4 = impact**: the swept segment old→target crosses the surface at time t∈[0,1], or the particle started
    inside. The contact point is offset outward by collisionDistance.
  - **8 = collision**: the target ends within collisionDistance of the surface. It gets pushed out to surface + collisionDistance.
  - **0x10 = proximity**: within the proximity distance. Only remembered, used for the next step's cache.
- Sphere: inside → radial push to r + collisionDistance (t = 0); else analytic segment/sphere root.
- Plane: the sign of the start and end distances decides impact; the end distance decides collision/proximity.

## Contact accumulation per particle: `0x1019a130` `re_fluidAccumulateShapeContact` [V]
- Shape-local results are rotated back to world space. For dynamic shapes, the surface velocity is
  `v_lin + ω × r` (r from the body's centre of mass).
- **Impact:** keep only the **earliest** impact (smallest t). It's marked "single contact" (bit 2) and overrides averaging.
- **Collision:** **sum** normal, surface point and surface velocity, and increment the count (bit 1). The resolve
  step later divides by the count (see `re_fluidResolveParticleContacts`).
- **Impact or proximity:** store up to **2 contact planes** (normal, plane distance, surface velocity, static/dynamic tag)
  as the persistent cache for the next step. When both slots are full, keep the two planes nearest the current position.

## Emitters (Common FluidEmitter*, 0x10106a00–0x1010a000) [V]
- The visible virtual functions are a debug draw (`FluidEmitter_vf0`), setGlobalPose (`vf4`) and destructors. The work is non-virtual.
- **Rate emitter step** `0x101093c0` `re_emitterRate_step(dt)`:
  - `acc += rate · dt`, then emit `round(acc)` particles; each one emitted does `acc −= 1`.
  - Spawn slots form a grid on the emitter plane, nx × ny with spacing (sx, sy). **Every odd column is offset
    by sx/2** (hex packing). Elliptical emitters reject slots outside the ellipse.
  - If more particles are due than there are slots, they're emitted in **layers**, each pushed forward along
    the emitter Z axis by `dt · fluidVelocityMagnitude / layers`.
  - Slot order is a random permutation (shuffle when emitting more than a quarter of the slots, otherwise random picks).
  - Position jitter is uniform in ±randomPos per axis.
  - Random values use the **CRT `rand()` · (1/32767)**: global, unseeded state. Oracle runs need
    `srand()` control (or must avoid emitters) to be reproducible.
- **Velocity** `0x10108300` `re_emitter_randomVelocity`: azimuth φ = U·2π [A: constant], polar θ = U·randomAngle.
  The direction is `cosθ·Z + sinθ·(cosφ·X + sinφ·Y)`, scaled by fluidVelocityMagnitude.
  - Flag 0x8: add the attached body's point velocity.
  - Flag 0x4: accumulate a reaction force/torque on the body.
- **Queue** `0x101085e0` `re_emitter_queueParticle`: appends {pos, vel, lifetime} to a creation list. It stops at the
  emitter's maxParticles (and raises an "emitter empty" event) or at the fluid's free capacity.
- Pressure emitter step: `0x10108be0` (not read yet).
- **[A] Possibly irrelevant to Mirror's Edge.** UE3 Cascade PhysX particles probably spawn with
  `NxFluid::addParticles` from the particle module rather than NxFluidEmitter. Confirm in MirrorsEdge.exe.

## SPH/integrator parameter block = PxsFluid+0x64 [V: offsets line up between init, SPH pass and integrator]
With s = 1/restParticlesPerMeter (world rest spacing) and σ = 0.1/s (world→sim scale):
| block off | PxsFluid off | value |
|---|---|---|
| +0x00..08 | 0x64 | external acceleration (world) |
| +0x0c | 0x70 | restDensity ρ₀ |
| +0x10 | 0x74 | self density W(0)·m |
| +0x14 | 0x78 | mass m = ρ₀·0.001 |
| +0x18 / +0x1c | 0x7c / 0x80 | world kernel radius h_w = KRM·s, and h_w² (**pair test is r²_world < h_w²**) |
| +0x20 / +0x24 | 0x84 / 0x88 | sim kernel radius h = 0.1·KRM, and h² |
| +0x28 | 0x8c | poly6 · m |
| +0x2c | 0x90 | 22.5/(πh⁶) · m |
| +0x30 | 0x94 | 45/(πh⁶) · m · viscosity |
| +0x34 | 0x98 | stiffness |
| +0x38 | 0x9c | damping |
| +0x3c / +0x40 / +0x44 | 0xa0 / 0xa4 / 0xa8 | σ, σ², 1/σ |
| +0x4c | 0xb0 | motion limit = motionLimitMultiplier · s |
Fluid-level collision distance = collisionDistanceMultiplier · s (0xe0); proximity = 2× that (0xe4) [A];
query range = motion limit + collision distance + 0.001 (0xec).

### Exact per-pair formulas (positions and velocities in world units; d = xᵢ − xⱼ, r = |d|·σ)
- density: ρᵢ = W(0)m + Σⱼ poly6m · (h² − |d|²σ²)³
- pressure: pᵢ = max(0, (ρᵢ − ρ₀) · stiffness)
- pair term: T = (pᵢ+pⱼ)·(h²/r − 2h + r)·C_p · σ·d + (h − r)·C_v · σ·(vⱼ − vᵢ)
  - Fᵢ += T / ρⱼ
  - Fⱼ −= T / ρᵢ
- integrator: aᵢ = Fᵢ/ρᵢ · (1/σ) + a_ext

### Verified against the oracle (2.8.1 core, `scenarios/fluid/*`) [V]
- Resting particles sit exactly collisionDistance above a plane (0.0024 at 50 particles/m with multiplier 0.12;
  0.006 at 20 particles/m).
- Oracle runs are **bit-identical** across repeats (single-threaded software scene).
- Particle flags in the output: 0x1 static contact, 0x4 separated (no neighbours), 0x8 motion limit reached.

## Solver v0 vs oracle (first comparison) [V]
- `solver/fluid` v0 (native build, `build/native/fluid_run`) reproduces **free fall and the first impact bounces exactly**:
  on `debris_bounce`, particle 55 matches bit-for-bit through two impacts (position, velocity, normal).
- The divergence starts at the **second consecutive contact frame**: the oracle sets velocity to exactly 0 (the particle sticks).
- ~~The SPH scenario differs by 4e-4 m already at step 0.~~ Superseded (2026-10-08): with the current solver
  sph_dambreak matches to float rounding (≤2e-7 m) for the first 7 steps, until the first plane contact.

## Probe results: friction (oracle = 2.8.1 core, single particle on a plane) [V]
`scenarios/fluid/probe*/`
- staticFrictionForStaticShapes **> 0, even 1e-4**: the particle stops completely (v = 0) on the 2nd consecutive
  contact frame, at 1, 10 or 30 m/s alike. So in practice it's a boolean "stick on resting contact".
- staticFriction = 0: no sticking. Each contact frame scales tangential speed by (1 − dynamicFriction).
  With dynamicFriction = 0 the speed still drops ×0.999 per contact frame: **zero friction behaves as 0.001**
  [V behaviourally, all `slide_sf0.0_df0.0_*` probes become exact with friction = max(f, 0.001); where the clamp
  lives in the binary is not located, A: desc validation/setter].
- Restitution bounces use the **pre-integration velocity** when deciding the normal response (cached-contact path
  `0x10199aa0` → `re_fluidContactVelocityResponse` gets the old velocity as its 3rd argument).
- The CRT rand scale constant is 1/32768 (3.0517578e-5), not 1/32767.
- **Version caveat:** these probes ran on the 2.8.1 core. The 2.8.0 LL descriptor has only 7 collision floats,
  versus 8 friction/restitution/attraction fields in the 2.8.1 API, so static friction may not exist in the game's 2.8.0.
  Being resolved by diffing against the 2.8.1 core (Ghidra project `physx281`).

## 2.8.0 vs 2.8.1 fluid differences [V]
- Low-level fluid (PxsFluid init, cached-contact solver, contact resolve, velocity response) is **identical**.
  Only PxsFluid member offsets shift by +8.
- **2.8.0 has no static friction and no attraction.** `NpFluid` vtable slots 42–44 and 50–52 (the 2.8.1
  static-friction/attraction get/set) are stubs in 2.8.0: getters return 0, setters do nothing. The 2.8.0 core has
  no `setStaticFriction*` strings at all. **Game behaviour = static friction 0** (no sticking), so oracle scenarios
  must set `staticFriction*ForStaticShapes/DynamicShapes 0` on the 2.8.1 core.
- `NxFluid` vtable layout is the same in both (81 slots, stubs filling the 2.8.1-only entries), so the 2.8.1
  header's NxFluid interface is ABI-compatible with 2.8.0.

## Collision fidelity status (solver vs 2.8.1 oracle, static friction 0) [V unless marked]
- Exact, bit-for-bit: free fall, the integrator, SPH density/force on the first step, impact response
  (bounce velocity, contact point = hit + n·collisionDistance), plane and box swept impact time, and
  single-particle plane drops for 12+ steps.
- Rules found by probes (`scenarios/fluid/probe_*`):
  - Ending a step within collision distance **without crossing** the surface gives no response (no push-out,
    no velocity change).
  - Proximity alone does not create a cached plane for a slow particle (`probe_prox`, `probe_prox_box`).
  - Cached planes come from impacts; the next step resolves against them using the **previous step's velocity**
    for the restitution test (particle 198 in sph_dambreak, particle 28 in debris_bounce).
- Shape tests (2.8.0 `re_fluidCollideShape_dispatch` cases): 1 sphere, 2 plane, **3 capsule** (`0x101abb80`, axis = local x,
  radius at +0x1c, half-height at +0x20), **5 box** (`0x101ab730`, six face planes fed to the generic convex-plane test
  `0x101ab240`: entry/exit slab times, single-positive-plane proximity, max-end-distance collision, inside → nearest face).
- **Shape frame, all shape types** [V: Ghidra `re_fluidCollideShape_dispatch` 0x1019b800, session 9]: before the case switch every particle's target is
  rotated by the shape's inverse pose (a quaternion: v·(2w²−1) + 2w·(q×v) + 2q(q·v), then a precomputed inverse
  translation added) and its start by the begin pose the same way; the box test 0x101ab730 then runs against six
  axis planes (±1, 0, 0, −h) in that frame. Results go back with the conjugate quaternion and the shape position.
  A static rotated box therefore keeps the box rules (we used to turn it into 12 triangles). For an identity rotation
  the transform is exactly x − c, which is why axis-aligned boxes are bit-exact; for rotated shapes the last bits
  depend on the core's internal quaternion, which we do not reproduce (we use the NxMat34 from getGlobalPose).
- **Broadphase latency artifact (original engine):** a lone fast particle tunnels into a box for a step
  (`probe_p28`: y goes 0.5031 → 0.4438 inside the box, then is pushed out on the next step). In a dense run the same
  layer of particles gets a contact a step earlier [A: the packet's shape pairs come from the scene broadphase,
  using packet bounds from before the step]. Faithful replication needs the packet/broadphase pairing model;
  open decision whether to copy this artifact.

## Contact cache and proximity rules, settled 2026-10-08 [V unless marked]
Read in Ghidra (2.8.0): `re_fluidVsPlane` 0x101ac060, `re_fluidAccumulateShapeContact` 0x1019a130, cached-contact
solver 0x10199aa0, resolve 0x1019b5d0, packet loop write-back in 0x1019bec0. Constants read from the DLL:
1.01 (single-plane push factor), 1.0001 and 0.999999 (two-plane case), −FLT_MAX ("no crossing" t sentinel), 0.5·1e-4.
- **Shape-test classification of the end point when the segment does not cross the surface** (plane test, the other
  shapes follow the same pattern): if the particle has no impact from an earlier shape in this pass, then
  `d1 ≤ collisionDistance` → *collision* (flag 8: summed normal/point, averaged in the resolve, push-out to
  collisionDistance, normal velocity response with restitution on the **current** velocity), and
  `0 < d1 ≤ proximityDistance` → *proximity* (flag 0x10), which only stores a cache plane for the next step.
  The plane point is pushed out to collisionDistance, so the cached plane sits at surface + collisionDistance.
- **Superseded by "Proximity distance per shape" below:** for the plane shape proximityDistance behaves like
  collisionDistance [V: `scenarios/fluid/probe_proxdist`, a particle ending 1.005·cd above the plane gets no cache
  plane; resting contacts need d1 = cd to qualify]; the solver uses cd·1.0001 for planes (the epsilon was fitted on
  `probe/slide_sf0.0_df0.15_e0.5`). All other shapes use 2·cd. Context fields of the collide context
  (`this` of 0x1019bec0): +0x2c collision distance, +0x30 proximity distance, +0x34 motion limit, +0x3c dt, +0x40 1/dt.
  Where +0x30 is filled is still unfound [A: from PxsFluid +0xe0].
- **Cache retention** [V behaviour, A mechanism]: a cached plane that was *not hit* this step stays in the cache while
  the particle's end point is within collisionDistance of it. A plane that *was* hit is consumed (only new contacts
  re-create it). The visible write-back only stores this step's new candidates, so the mechanism sits somewhere not yet
  read (probably the packet or broadphase layer). The rule reproduces `probe_rebound` exactly (a particle that bounced
  and comes back down is stopped dead: vy = 0 without push-out, because restitution uses the previous, upward velocity)
  and sph_dambreak particle 198.
- **Earlier "rules" that were broadphase latency for fresh particles, not contact rules:** "ending within collision
  distance without crossing gives no response" and "proximity alone does not create a cached plane"
  (`probe_prox*`, the first frames of `probe_rebound_0.020..0.032`). A newly added particle has no packet/shape pair
  on its first step. `probe_late` shows the same configuration later in a run *does* respond. We don't copy this latency.
- The static-contact output flag (bit 0) partly reflects this latency or read-back timing (e.g. it's set on a frame
  where the particle moves away from the cache plane), so analysis treats contact fractions as information only.

## Mixed mode (ESM_MIXED_MODE = NX_F_MIXED_MODE = 4): alternates SPH and no-interaction every step [V]
- Low-level method codes: 1 = SPH, 2 = no interaction. The LL step `0x10199810` runs SPH density and force when
  method == 1, otherwise only `re_fluidIntegrateVelocity` with the "no SPH" flag (0x101995f0).
- The phase byte is at full-object +0x41c (= subobject +0x3a8), first written by the Fluid constructor (0x10104ae0,
  4th arg). Scene fluid creation (around 0x10095420–0x100954a2) counts the existing mixed fluids by phase and gives the
  new fluid the minority phase, so with several mixed fluids about half run SPH on any given step (load balancing).
- Per scene substep (`0x10093190`): `0x1008e6c0` calls the Fluid pre-step `0x101028f0` on every fluid, then the LL
  fluid tasks run, then `0x1008e790` calls the Fluid post-step `0x10102e00` on every fluid.
  - Pre-step: for method 4 it sets LL method (param 0x16) = 1 if phase == 0, else 2. `Fluid_vf34` does the same
    when the method is set through the API.
  - Post-step: step counter +0x418 += 1, then `phase = 1 - phase` (a byte write, which is why the earlier
    `or/and dword` search missed it).
- So a mixed fluid runs SPH on even steps and no interaction on odd steps (counting from its own creation, starting
  in its assigned phase). On no-interaction steps density and forces aren't recomputed.
- Implemented in `solver/fluid` (`mixedPhase_`, a lone fluid starts in phase 0). `dustmotes_mixed` now passes the
  statistical comparison. Multi-fluid phase assignment isn't modelled yet (the runner has one fluid).

## Proximity distance per shape, box face semantics, cache retention (steam_ceiling root cause, 2026-10-08)
Found by a first-divergence trace of the unjittered `steam_ceiling` run (cd = 2·spacing = 0.2 m, so cd-sized effects are
large and visible; every earlier scenario had cd = 0.12·spacing).
- **Collide context +0x30 = PxsFluid +0xe4 = 2·collisionDistance** [V: `re_PxsFluid_initFromLLDesc` stores 0xe0 = cd and
  0xe4 = cd+cd; the context lives at PxsFluid+0xb4, passed as `this` to the collide driver 0x1019c530; the dispatcher
  0x1019b800 hands (+0x2c, +0x30) = (cd, prox) to every shape test].
- **Box, sphere, capsule cache a contact plane out to 2·cd** [V behaviour: `scenarios/fluid/probe_prox2box`,
  `probe_prox2shape`, bit-exact against the oracle for end distances 1.2/1.5/1.9·cd, with three parameter sets incl.
  cd > motion limit]. A fresh particle that ends a step at cd < d ≤ 2·cd and then crosses the plane next step is
  resolved through the cached plane (restitution from the previous velocity). The **plane shape does not** (`probe_prox2`,
  `probe_proxdist`: same test, no cache) although `re_fluidVsPlane` reads the same two distances. [A: reason unknown;
  the solver special-cases it with prox = cd·1.0001.]
- **Box end-point classification is per face, not Euclidean** [V: decompiled generic convex test 0x101ab240 plus the
  steam_ceiling trace]. With d_k the signed distance of the end point to face plane k (positive = outside):
  - any d_k > prox: neither proximity nor collision;
  - proximity (flag 0x10, cache plane): exactly one face with d_k > 0 (a second positive face cancels it, so no
    proximity near edges and corners); the plane normal/point come from that face;
  - collision (flag 8): every positive d_k ≤ cd. That is a box expanded by cd with **square** edges/corners. The face
    with the largest signed distance gives normal and push-out point (`end + n·(cd − d)`).
  The old Euclidean-distance model gave rounded corners and missed e.g. particles beside a box edge that are within cd of
  the bottom face and 0.5·cd from the side face.
- **Cache retention is plane-shape only** [A, fitted]. The write-back (0x1019bec0) only stores this step's candidates
  and 0x10199aa0 has no retention path, so the earlier "un-hit planes survive while within cd of the plane" rule was
  describing something else. It is still needed for `probe_rebound` (plane), but applying it to box planes keeps stale
  planes alive that the original drops (steam_ceiling, particle beside the box edge). Solver: retention only for planes
  produced by the plane shape. The unexplained plane-vs-others difference probably has one cause (see above).
- Cached planes carry a shape id; 0x10199aa0 rejects a cached plane whose id is set in a scene bitset (+0xa8, probably
  "shape changed this step"). Irrelevant for static shapes, but it matters for dynamic shapes later.
- Remaining differences are float-rounding chaos: after the fixes steam_ceiling matches the oracle to < 1e-5 m until
  step 30, then diverges exponentially as SPH amplifies rounding. The statistical comparison passes with every metric
  ≤ 0.33 of its limit.
- **glass_shards' speed blip at steps 69-77 was the same cause** (box/sphere proximity planes and box edge cases), not the
  broadphase-latency artifact. It passes now (worst ratio 1.67 on speed_p50 at 5% tolerance).
- Side effects: `debris_bounce` and `dustmotes_mixed` are now essentially exact (metric ratios 0.00). Goldens of the
  artifact probes `proxbox_*`, `a_full`, `b_noplane` changed (box proximity) and were re-accepted.

## Plane proximity epsilon (mesh_drop_plane second contact, 2026-10-08) [V behaviour, A constant]
- A plane-shape end point at d1 = 1.000012·cd (0.00600007 m for cd = 0.006) is **not** proximity: the original makes no cache plane there, so the next step is a plain swept impact against the static plane (hit point, p = hit + n·cd, restitution on the integrated velocity). Ours cached it (epsilon 1.0001) and resolved the next step through the cache path instead, a 1.7e-2 m error from step 53.
- Plane proximity is therefore cd·(1+ε) with ε in [5e-7, 1e-5): 1.0 breaks probe/slide_* (rounding of resting contacts), 1.00001 and above breaks mesh_drop_plane. Solver uses ε = 3e-6 (supersedes the 1.0001 value above).
- This case does not exercise the cached-path velocity rule (`prevVel_` for the restitution test), which is unchanged; the "pre-integration velocity" note above is still only fitted on sph_dambreak 198 and debris_bounce 28.

## Particles starting inside the collision band (probes `scenarios/fluid/probe_band/`, 2026-10-10) [V unless marked]
Oracle = 2.8.1 core, single particle, `simulationMethod none`, 10 steps, `block` at step 0 unless `late`.
Gravity: use `gravity 0 0 0` for force-free probes. The header's "DISABLE_GRAVITY (default 1)" does not disable scene
gravity in these runs (the first batch had gravity, visible as dv = g·dt per step). The `late` probes add the particle
with `at 2 add` (oracle only: our runner writes no output for `at` lines). Frame k = state after simulate step k; the
dump's position is `p_k = p_{k-1} + v_k·dt` (semi-implicit) when nothing collides, which is how a response is detected.

**1. Age latency (analytic shapes only).** A particle's first two simulate steps after it is created get no shape test at
all: pure ballistic motion, whatever its position relative to the surface. Verified for plane, box top, box side, sphere,
capsule side (block and `at 2 add`: the late particle is latent at frames 2 and 3 and responds at frame 4). Triangle
mesh has no latency (responds on the first step). Probably the packet/shape pairs are built from the previous step's
bounds and the particle has none for its first two steps [A: from `re_fluidCollidePacket`, packet build 0x1019c530 loops
shapes and builds pair lists before any contact pass]. Does not depend on restitution, depth or velocity.

**2. Response after the latency** (plane family, gravity off, cd = collision distance, s = signed distance along the
outward normal n, vn = n·v, e = restitution for static shapes):
- Let s1 = s0 + vn·dt be the ballistic end distance.
- **crossing** (s0 > 0, s1 < 0, vn < 0): swept impact as already implemented (matches the oracle at age ≥ 2).
- **collision** (s1 ≤ cd, not crossing, any s0 including s0 ≤ 0 and any sign of vn): the end point is pushed out along n
  to distance cd. Velocity: if vn < 0 then vn' = −e·vn (tangential part unchanged for zero friction); if vn ≥ 0 unchanged.
  Observed: plane tow from s0 = −0.5cd and s0 = 0 and s0 = 0.25cd, rest, slow, parallel, all s0 ≤ 0.9cd, all end at exactly
  1.000 cd with vn → +0.5·|vn| (e = 0.5) or 0 (e = 0). Box top, box side, capsule, sphere behave the same.
- **proximity** (cd < s1 ≤ prox, prox = cd for plane, 2·cd for box/sphere/capsule): only the cache as before.
- **otherwise** (s1 > cd and not crossing): ballistic, no change. This is the rule that rejects started-inside particles moving
  away: plane and capsule with s0 = −0.5cd and vn = +1 m/s at cd 0.006 m (s1 = 2.3cd) are never touched (oracle, 10 steps).
  At cd 1.2 m (game) the same particle has s1 < cd, so it IS pushed (collision rule above, velocity unchanged since vn > 0).
  So the started-inside case needs no separate branch: it is the ordinary end-point test.
- Decompiled plane test `re_fluidVsPlane` (0x101ac060, FUN_101ac060 in re/out/PhysXCore-280/decomp.c) has an
  else-branch for s0 ≤ 0 that sets an impact at t = 0 with a push to cd, regardless of the end point and the velocity [A].
  The oracle does not do that for separating particles, so the else-branch's effect must be gated somewhere else (not
  located). Do not implement it as written.
- **Motion limit applies to the push** [V]: in cfg B (cd 1.2 m, motion limit 0.3 m = 0.25 cd) a particle at −0.5 cd
  moves to cd in steps of 0.25 cd: age 2 → −0.25, then 0, +0.25, … The oracle has one pause (age 5 or 6, the displacement
  is then ballistic for one step) and then continues; mechanism unknown [A: cached-plane rebuild every other step?].
  The flag output shows 0x5 (contact) at each pushed step, never 0x8 (motion limit), so the clamp is not reported.

**3. Mesh** (triangle mesh, `quad`, cfg A) [V]: no latency; no push for s0 < 0 at all (rest, tow, slow from −0.5cd and
0; a particle below a flat quad stays put even at rest). Particles above the quad with s1 ≤ cd are pushed at step 0.
Keep the mesh path separate from the analytic rule above.

**4. Output flags:** 0x4 on steps without a response, 0x5 when a contact push happened (0x1 = contact bit). Latent steps
report 0x4 as well.

### Our solver vs the oracle on the same probes [V]
- `solver/fluid/fluid.cpp:483-489`: the `d0 <= 0` branch (started inside) always sets an impact at t = 0 and pushes the
  projection of the start point to cd, regardless of vn and s1. That gives the bug in heli_smoke: at step 0 every particle
  with s0 ≤ 0 is snapped to cd (and velocities reflected), and particles moving away are pushed too. In the original the
  push waits for age 2, then only happens if s1 ≤ cd, moving at most the motion limit per step.
- No age latency: the analytic shape tests run from the first step (the solver tracks `born_` at line 932 for drain
  latency only, `fluid.cpp:1372`). Mesh is also not latent in the original, so the latency belongs to the analytic path.
- Motion limit clamp matches (`fluid.cpp:1544`), so the 0.25 cd per-step progression comes out right once the latency is in.
- Minor, mesh: at s0 = 0 exactly (quad, tow) the oracle gives no response, ours gives one at step 0.

### Proposed solver changes at probe time (historical)

The age-latency comparison is recorded in [sph-game-validation.md](sph-game-validation.md). This proposal is not the current release checklist.
1. In the plane/box/sphere/capsule path (`sd` lambda, ~line 483), delete the `d0 <= 0` impact branch. Let the end-point
   collision test (s1 ≤ cd → push to cd, vn' = −e·vn only if vn < 0, push clamped to the motion limit) handle started-inside
   particles. Keep the swept impact only for s0 > 0 with a crossing.
2. Add a per-particle age gate: skip all analytic shape tests (not mesh) while `steps_ − born_[i] < 2`, ballistic motion only.
   `born_` already exists; the gate belongs in the collide loop that calls the shape tests, not in the integrator.
3. Check after the change: `probe_band/band_plane_A_*` and `band_plane_B_*`, plus the existing probe goldens (`tools/fluid_probe_check.sh`)
   and `heli_smoke` at steps 0-30. Expect the goldens for started-inside probes to change; re-accept only with this rule.
