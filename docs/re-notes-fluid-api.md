# NxFluid API semantics the game relies on (session 7, 2026-10-09)

What the stand-in NxFluid has to reproduce for Mirror's Edge's call pattern (docs/shim-game-run.md). The
behaviour was measured on the 2.8.1 core with the oracle running through the pass-through shim
(`tools/run_fluid_shim.sh`, `PXSHIM_TRACE=2`, `analysis/fluid_trace.py dump`). Scenarios:
`scenarios/fluid/api/` (`at S add|delete|deleteEvery|removeAll` directives, `fluid.writeIds`, `fluid.compartment`).
Static RE (2.8.0 core) is marked as such; most of it is [A] and the trace results below supersede it.

## Pass-through check [V]
The wrapper the game holds (ShimFluid, `shim/fluid_wrap.cpp`) forwards all 81 slots. Oracle output through it is
identical to the plain oracle apart from the clock field, and the trace's particle stream is byte-identical to the
oracle's frames: debris_bounce, sph_dambreak, steam_ceiling, mesh_drop, api_life, api_life_comp, api_priority.

## Layout correction [V]
`NxFluidPacketData` is { bufferFluidPackets, numFluidPacketsPtr }: the buffer comes first (desc dwords 0x4d, 0x4e).
The older note had them swapped. The packet record is 36 bytes: aabb min3 max3, firstParticleIndex, numParticles, packetID.

## Compartment [V by trace, details A]
- Fluid in an `NX_SCT_FLUID` CPU compartment vs no compartment: **identical output** (api_life vs api_life_comp).
  The write buffers are filled in `fetchResults(NX_RIGID_BODY_FINISHED, true)` of the same frame, so there's no
  extra latency. The stand-in can ignore the compartment pointer (but must return it from getCompartment).
- Static RE (2.8.0): NxCompartmentDesc is 7 dwords like the 2.8.1 header (type, deviceCode, gridHashCellSize,
  gridHashTablePower, flags, threadMask, timeScale); a SW compartment is an async scene with its own thread, kicked in
  simulate with the scene dt (timing INHERIT by default) and waited for in fetchResults [A].

## addParticles [V]
- Reads pos, vel and life (when given); returns the number added. Ids are the **lowest free ids** (0, 1, 2, ...;
  freed ids are reused lowest first).
- **The creation id buffer is written by addParticles itself**: right after the call it holds exactly the ids of that
  call (overwritten per call, not accumulated). After the next fetchResults it's empty (count 0). The game passes a
  creation id buffer and appendIds = false, so this is most likely how it maps its UE3 particles to fluid ids.
- **addParticles also appends the new particles to the user's particle write buffer at once**: the count grows by
  the number added before any simulate (api_life: 9, 13, 15) [V]. The header says the same for packets [A].
- `appendIds = true` appends the ids to the creation id buffer instead of overwriting it (header text) [A, the game
  passes false].
- New particles then show up in the next fetch's write buffers, already stepped once.
- At most 4096 particles per addParticles call/step (header, "PC SW: Yes") [A: the stand-in caps each call].

## Lifetime [V]
- The life write buffer holds the **remaining** lifetime after the step (life - steps*dt).
- A particle is removed in the step where its remaining life reaches <= 0 (life 0.1 at dt 1/60: present after 5
  steps, gone in the 6th) and its id is reported in the deletion id buffer at that fetch.
- Life 0 (or no life buffer) = never expires; the life buffer reads 0.

## updateParticles / removeAllParticles [V]
- `NX_FP_DELETE` (flag 1) by id: the particle is gone after the next simulate and its id is reported in the deletion
  buffer at that fetch. Unknown ids (e.g. 300) are ignored. The game uses an interleaved id/flag buffer, stride 8.
- removeAllParticles: all particles are gone after the next simulate and reported as deleted. The packets and
  world bounds of that one step still include them (they are stepped once more and removed at the end of the step).
- Deletion id order = reverse write-buffer order (the core removes by swapping from the end) [V, not needed].

## PRIORITY_MODE (flags 0x20, the game's collide-only fluids) [V]
**Deliberate difference: Shatter removes just enough** (exactly the excess, least remaining life first, ties oldest
first, infinite-life last). The timing stays the original's: the decision uses the count at the end of the previous
step, so a burst may sit above max - reserve for one step (that is what the reserve is for). api_priority differs
from the original on purpose (it removes 9 where 3 are needed); api_priority2 matches.

Original behaviour: at the next simulate, if count > maxParticles - numReserveParticles, the particles with the **least remaining life**
are removed until count = max - reserve, and reported as deleted. Particles with the same expiry time go as a group
(9 equal-life particles all removed when 3 were needed, api_priority), so the core removes whole expiry buckets [V
behaviour, A mechanism]. Infinite-life particles were never chosen while finite ones existed [A, not isolated].

## getWorldBounds [V]
The tight AABB of the particle positions after the last step (no padding). An empty fluid returns min = +FLT_MAX,
max = -FLT_MAX. It lags by one step after removeAll (see above).

## Write buffers and packets [V]
- Particle order in the write buffer is the core's packet/cell order, not id or creation order. The game also asks
  for ids, so it maps by id [A]; the stand-in may use its own order.
- Packets: one per occupied packet cell, packetID = cell hash (e.g. 0 and 593), firstParticleIndex/numParticles index
  the write buffer. The game passes a packet buffer, but nothing shows it reads it [A].
- Particle flags were 0x4 (SEPARATED) for free particles.

## Unresolved static RE notes [A]
- addParticles in 2.8.0 (NpFluid slot 5, 0x1016db70) has two paths depending on a scene byte (+0x4d8); the capacity
  is min(request, max - live - reserve term).
- updateParticles honours only bit 0 of the flag (NgFluid 0x10208e40); forces scale per forceMode.
- The particle record layout in re-notes-fluid.md ("Particle record") conflicts with the examined record writer
  (id at +0x1c, expiry at +0x30, flags at +0x2e). Not needed for the stand-in.

## updateParticles force buffer (NX_FORCE and the other modes) [V unless marked]
Sources: decompile (NgFluid_vf69 / FUN_102088d0 index path, FUN_10208cb0 id path, FUN_10208190 scale, FUN_1020ad90
simulate), SDK header (NxParticleUpdateData: supported modes FORCE, IMPULSE, VELOCITY_CHANGE, ACCELERATION; numUpdates
ignored for index based updates), oracle probes scenarios/fluid/probe_force/ (single particles, one or two per case).
- Entries with bufferId add into a per-particle accumulator; ids that are not alive are ignored; only listed ids move.
  bufferId NULL: every particle, in order (count = the particle count, numUpdates ignored). Flag bit 0 stays the delete.
- Scale, fixed at the call (before the next simulate): mass m = restDensity / restParticlesPerMeter^3 (0.008 with the
  defaults, oracle particle_mass). T = max(1, N) * sub, where N and sub are the substep count and length of the last
  simulate; T = 0 before the first simulate. dv = F * T/m (NX_FORCE), F/m (NX_IMPULSE), F (NX_VELOCITY_CHANGE),
  F * T (NX_ACCELERATION). So an acceleration integrates over the time the last simulate covered, not over the frame
  dt when substeps round it [V: sub_leftover, zero_substep, vardt]. Forces sent before the first simulate do nothing
  for FORCE and ACCELERATION (probes m0, m5) [V].
- NX_SMOOTH_IMPULSE (3) and NX_SMOOTH_VELOCITY_CHANGE (4) are not implemented for fluids: the core logs "mode not
  supported" and uses NX_FORCE [V: decompile default case; probes m3, m4, late_m3, late_m4; sdk_warnings +1].
- Several entries (or calls) before one simulate add up [V: twice, persist].
- Application: the first simulate that runs at least one substep adds the accumulated change to the velocity, once,
  before that substep's motion. It is then cleared. A simulate with no substep keeps it for the next one [V: sub4_m*,
  zero_sub_accel3, big_sub_m0]. The whole change acts in that substep, not spread over the substeps: a displacement of
  dv * frame dt [V: late_sub4_m0].
- Order inside a step: the change is what an initial velocity would be. It comes before gravity (additive), SPH
  (a neighbour responds in the same step, sph_pair) and collision (collide_now == collide_now_v) [V]. The motion-limit
  clamp applies after it and also scales the gravity part (limit_grav) [V]. Damping applies to the change (damp_vc) [V].
- Oracle substep counts differ from our fixed-carry rule at exact multiples (for example dt 0.01, substep 0.004: the
  oracle gives 2, 3, 2, 3, 2, 2 substeps, the carry rule 2, 2, 3, 2, 3, 2). A total-time rule matches every timing
  probe: T = float sum of frame dts, n = floor(T * 250) - previous n, where 250 = 1/0.004 is taken from the decimal, not
  from the float [V on those probes, A on the rule]. Not applied to the shim's stepping: it changes runs without forces.
- Not seen yet [A]: variable timestep mode (sub = frame dt, one substep), and the scale when a substep is longer than the
  frame and no other simulate ran (big_sub_m0 shows T = substep length, consistent with the rule).

Implementation: solver/fluid (ForceMode, particleMass, forceVelocityChange, Fluid::addVelocityChange applied at the start
of step()); shim/fluid_ours.cpp queueForces (accumulates in the hosted fluid; handed to the solver at the next simulate);
the oracle's "at S force" and "at S dt" and "timing" directives and the runner's matching code.
