# RE notes: CPU cloth (PhysXCore 2.8.0.45)

Descriptions of behaviour, written in our own words. No NVIDIA code is copied here.
Addresses refer to the Ghidra project `re/ghidra/physx280` (image base 0x10000000) unless marked 2.8.1
(`re/out/PhysXCore-281`). [V] = read in the disassembly/decompilation or measured with the oracle;
[A] = interpretation still to confirm.


## Quick map (2.8.0) [V]
- Software cloth = class `Cloth` (vtable 0x102e5304, 122 slots), code 0x100db270..0x100fe9d0, source
  `SDKs/Core/Common/src/cloth/Cloth.cpp`. `NfCloth` (PPU) and `NgCloth` (CUDA) are sibling implementations of the
  same internal interface, not subclasses. The same `Cloth` class also runs **soft bodies**: member +0x48 is the
  object kind (1 = cloth, 2 = soft body); solver/debug-draw branch on it.
- API wrapper `NpCloth` (92 slots, `NpCloth+0x14` → `Cloth*`).
- Scene step `Scene_vf46` (0x1009a9f0): per scene substep, rigid bodies first, then all cloths with the
  scene's substep dt (`scene+0x74`) via the cloth step driver `re_cloth_stepAll` (0x100f9d20).

## NxClothDesc and validation (2.8.0 `NpScene::createCloth`, 0x1014ef..) [V]
- The 2.8.0 public `NxClothDesc` has the **same layout as the 2.8.1 header** (0x46 dwords): every validated field
  sits at the 2.8.1 index (thickness 0x0d, density 0x0e, bending 0x0f, stretching 0x10, damping 0x11, friction 0x12,
  pressure 0x13, tearFactor 0x14, collisionResponse 0x15, attachmentResponse 0x16, attachmentTearFactor 0x17,
  to/fromFluidResponse 0x18/0x19, minAdhereVelocity 0x1a, solverIterations 0x1b, externalAcceleration 0x1c,
  windAcceleration 0x1f, wakeUpCounter 0x22, sleepLinearVelocity 0x23, meshData 0x24 (17 dwords), collisionGroup 0x35,
  groupsMask 0x36, forceFieldMaterial 0x3a, validBounds 0x3b, relativeGridSpacing 0x41, flags 0x42, userData 0x43,
  name 0x44, compartment 0x45).
- Checks are the header's `isValid()`: finite pose, thickness ≥ 0, density > 0, bending ∈ [0,1], stretching ∈ (0,1],
  pressure ≥ 0, tearFactor > 1, attachmentTearFactor > 1, solverIterations ≥ 1, friction ∈ [0,1], damping ∈ [0,1],
  collisionResponse ≥ 0, wakeUpCounter ≥ 0, attachmentResponse ∈ [0,1], fluid coefficients ≥ 0, minAdhere ≥ 0,
  relativeGridSpacing ≥ 0.01, collisionGroup < 32, plus the meshData buffer/pointer pairing rules.
- Defaults are the header's `setToDefault()` (thickness 0.01, density 1, bending 1, stretching 1, damping 0.5,
  friction 0.5, pressure 1, tearFactor 1.5, solverIterations 5, flags = GRAVITY, sleepLinearVelocity −1 → SDK default).

## NxCloth vtable / ABI (2.8.0 vs 2.8.1) [V]
- `NpCloth` primary vtable has 92 slots in both cores, and the **42 slots that carry an `NpCloth::xxx` name string**
  (setters, attach*, tear, add*Force*, wakeUp, …) sit at exactly the index the 2.8.1 `NxCloth.h` declares
  (e.g. 3 setBendingStiffness, 22 setSolverIterations, 45/46 setPosition(s), 79 setFlags, 82 addForceAtVertex,
  91 setForceFieldMaterial). The getters in between match by position. **The 2.8.1 header's NxCloth interface is
  ABI-compatible with the game's 2.8.0 core.** (`NxScene` is not: `createCloth` is reached through the 155- vs 157-slot
  NxScene vtable, see `docs/abi-2.8.0-vs-2.8.1.md`.)
- The internal `Cloth` class grew from 122 to 124 slots in 2.8.1 (two inserted before slot ~72: `isSleeping` moves
  0x120 → 0x128). Internal only.

## Cloth object layout (2.8.0), from saveToDesc (`Cloth_vf13`) and init (`Cloth_vf114`, 0x100f12d0) [V]
| off | meaning |
|---|---|
| +0x48 | kind: 1 cloth, 2 soft body |
| +0x4c / +0x50 / +0x54 | stretchingStiffness / bendingStiffness / (internal, 0) |
| +0x58 / +0x5c / +0x60 | damping / collisionResponse / attachmentResponse |
| +0x64 / +0x68 | friction / pressure |
| +0x6c / +0x70 / +0x74 | toFluid / fromFluid / minAdhereVelocity |
| +0x78 / +0x7c / +0x80 | tearFactor / attachmentTearFactor / solverIterations |
| +0x84 / +0x90 | externalAcceleration / windAcceleration |
| +0x9c / +0xa4 | wakeUpCounter (desc) / live wake counter |
| +0xa0 | sleepLinearVelocity (desc value; if < 0, `sqrt(SDK parameter 2)` = default sleep lin vel² [A: param 2]) |
| +0xa8 | NxClothFlag |
| +0xd0 | bending model selector, **set to 1 in the constructor and never written again** (see bending) |
| +0xd4 / +0xd8 | mesh / meshData copy |
| +0x21510 | collision radius = **thickness · 0.5** (saveToDesc returns 2× this) |
| +0x21514 | density · +0x21518 global pose · +0x215e0 validBounds · +0x215f8 relativeGridSpacing |
| +0x21608 | particle array, **128 bytes per particle** |
| +0x2164c | user-vertex → internal-particle index map |
| +0x2167c | edge-constraint records (0x44 bytes in 2.8.0, 0x40 in 2.8.1) |
Object size 0x42dac bytes (allocated by `ClothManager` 0x100fab60, constructor 0x100e7e70).

### Particle record (128 bytes, 2.8.0) [V offsets; names A where marked]
+0x00 position · +0x0c inverse mass w · +0x10 predicted position p* · +0x20 velocity · +0x2c contact-plane distance ·
+0x30 attachment local position (or contact debug data) · +0x3c flags · +0x40 contact normal ·
+0x50 impulse accumulator (for two-way coupling) · +0x5c contact/attachment shape · +0x60 vertex normal.
(2.8.1 reorders: velocity +0x10, predicted +0x20, w +0x2c, shape +0x4c, normal +0x50 …)

Particle flag bits: 0x1 attached · 0x2 attachment is hard (vertex follows target exactly; w treated as 0) ·
0x4 has collision contact this step · 0x20 slower than sleep velocity · 0x80 tearable (mesh/flag) ·
0x100 attachment tearable · 0x200 [A: torn/split vertex; excluded from bending] · 0x400 removed (drained by an
`NX_SF_CLOTH_DRAIN` shape or outside validBounds; frozen) · 0x800 dominated (w treated as 0 until expiry).

### Mass [V] (`0x100efe70`)
Per triangle with edges e1, e2: m_tri = |e1 × e2| · density / 6 (= area·density/3) is added to each of its 3 vertices.
Per-vertex masses from the cooked mesh (`NxClothMeshDesc::vertexMasses`) override this when present. Mesh vertex
flags `NX_CLOTH_VERTEX_ATTACHED` (attaches to its initial global position) and `NX_CLOTH_VERTEX_TEARABLE` are
copied in. Then w = 1/m (0 stays 0).

## Per-substep pipeline: `re_cloth_stepAll` (0x100f9d20) [V]
Called once per scene substep (after the rigid-body substep) with the scene's fixed substep dt. Each stage loops
over **all cloths** before the next stage starts; cloths flagged STATIC (flag 2) are skipped.
1. **UpdateCollision** (`0x100e1e90`): broadphase query of the cloth's bounds (predicted ∪ current, expanded by
   2 × collision radius) → candidate shape list.
2. **ComputeNewState** (`0x100f0780`, skipped while sleeping):
   - self-collision prepass if `NX_CLF_SELFCOLLISION` (`0x100ef1b0`), adhere prepass if `NX_CLF_ADHERE` (`0x100dfc70`).
   - a = externalAcceleration (+ scene gravity if `NX_CLF_GRAVITY`).
   - **Wind:** if windAcceleration ≠ 0, per particle a_i = a + n_i (n_i · wind), with n_i the vertex normal
     (so wind acts only along the normal, signed; a back-facing normal pushes the opposite way).
   - **v += a · dt** for every particle (attached ones too).
   - COM damping (`0x100df6e0`) if `NX_CLF_DAMPING` and `NX_CLF_COMDAMPING`.
   - **p* = x + v · dt** (removed particles: p* = x). Recompute bounds. Particles that had flag 0x4 lose flags 0x1|0x4
     and their contact shape (contact-attachments last one step).
   - clear the impulse accumulators; pressure pass if `NX_CLF_PRESSURE` (`0x100e5b70`).
3. **GenerateContacts** (`0x100e17b0`, unless `NX_CLF_DISABLE_COLLISION`): see Collision.
4. **Solver**: for iter = 0 .. max(solverIterations over cloths)−1, every cloth with iter < its own solverIterations runs
   `0x100e2140`: edge constraints (`0x100dd9e0`, cloth) or tetra constraints (`0x100dec90`, soft body), then attachment
   projection (`0x100e0d90`), then the collision-plane projection (`0x100e1300`, unless DISABLE_COLLISION).
5. **IntegrateTimeStep** (`0x100f0b50`), not for STATIC cloth:
   - dominance timers (`0x100e5860`): dominated particles whose expiry passed lose flag 0x800.
   - **v = (p* − x)/dt; x = p*** (pure position-based velocity update). |v|² < sleepLinVel² → flag 0x20.
   - **Damping** (`NX_CLF_DAMPING` without COMDAMPING), cloth `0x100df490`: for every edge (i,j) whose particles are not
     hard-attached/removed, sequentially in edge order: Δ = (v_j − v_i) · damping · 0.5; v_i += Δ; v_j −= Δ.
     Soft bodies do the same over the 6 edges of each tetrahedron (`0x100df570`).
   - **Friction** (`0x100e5e80`, if friction ≠ 0): for particles in contact (flag 4, not attached) with surface velocity
     v_s of the contact shape's actor (zero for static): v_r = v − v_s, v_n = n·v_r, v_t = v_r − n v_n;
     **v = v_s + n · max(v_n, 0) + (1 − friction) · v_t**; the removed tangential momentum friction·v_t/w goes to the
     impulse accumulator.
   - validBounds culling and two-way impulse application (`0x100e7820`): particles outside validBounds get 0x400;
     with `NX_CLF_COLLISION_TWOWAY` (and shape flag `NX_SF_CLOTH_TWOWAY`) the accumulated impulse ×
     collisionResponseCoefficient is applied to the dynamic body.
   - tearing if mesh was cooked tearable and `NX_CLF_TEARABLE` (`0x100edb90` → `0x100ea530`); split-vertex fix-up
     (`0x100e13f0`); normals/bounds/sleep bookkeeping (`0x100e21a0`, `0x100e23b0`).

## Constraint solver `0x100dd9e0` (cloth, per iteration) [V]
Per-iteration stiffness factors (x87, verified in the disassembly):
**s_stretch = 1 − (1 − stretchingStiffness)^(1/N)**, **s_bend = 1 − (1 − bendingStiffness)^(1/N)**, N = solverIterations.
So after N iterations the compound effect equals the user stiffness (Müller et al. 2007, PBD §3.3).
Edges are processed sequentially (Gauss–Seidel) in cooked edge order. Each edge record holds the edge (i, j), the two
opposite vertices (k, l; l = −1 on a border), rest length L0, the k–l rest distance, the rest dihedral angle, and flags.
- **Stretch:** w_i, w_j (0 if the particle is hard-attached or dominated). d = p_j − p_i, L = |d|, n = d/L,
  C = L − L0 (zeroed if |C| < FLT_EPSILON), C *= s_stretch;
  p_i += n · C · w_i/(w_i+w_j); p_j −= n · C · w_j/(w_i+w_j). Edge flag bit 0 skips it (torn).
- **Attachment tearing (inside the stretch loop):** if either particle is attached with a tearable attachment and
  L > attachmentTearFactor · L0, that particle is detached (flags 0x1, 0x2, 0x100 cleared).
- **Bending** (`NX_CLF_BENDING`, interior edge, no particle flagged 0x200, edge flag bit 2 clear):
  - without `NX_CLF_BENDING_ORTHO`: distance constraint between the two opposite vertices k, l with the rest k–l
    distance and s_bend (same form as stretch);
  - with `BENDING_ORTHO`: the dihedral-angle constraint of Müller et al. 2007 (appendix A) on (i, j, k, l):
    normals n1 = (e × (p_k−p_i))/|·|, n2 = (e × (p_l−p_i))/|·|, d = clamp(n1·n2, −1, 1),
    Δp_m = −w_m · s · q_m with s = s_bend · sqrt(1−d²) · (acos(d) − φ0) / (Σ_m |q_m|²) / (Σ_m w_m).
    Two deviations from the paper: (1) the mass weighting uses w_m/Σw instead of the paper's Σ w_m|q_m|²
    denominator; (2) **acos is approximated by π/2 − asinSeries(d)** with the 5-term Taylor series
    x(1 + x²/6 + 3x⁴/40 + 5x⁶/112 + 35x⁸/1152). At d = 1 (flat) this gives 0.254 rad instead of 0, and the error
    grows steeply above |d| ≈ 0.9. Whether φ0 was computed with the same approximation (consistent rest state) is
    open [A]. **Treat the series as an artifact, use a real acos in our solver.**
  - The code has a third, "midpoint" bending variant selected by +0xd0 == 0; +0xd0 is fixed at 1, so it is dead.
- Edge records with a second vertex pair (fields 4/5 ≥ 0, produced by tearing [A]) get an extra stretch constraint
  on that pair instead of bending.

## Attachment projection `0x100e0d90` (per iteration) [V]
For attached particles (0x1, not removed): target = attachment local position transformed by the shape's current
world pose (global attachments: the stored world position).
- hard (0x2): p* = target.
- soft: with body inverse mass w_b (0 for static/kinematic), p* += (target − p*) · w/(w + w_b); the reaction
  −(target − p*)/(w + w_b)/dt is stored in the impulse accumulator (applied to the body later, two-way).

## Collision `0x100e17b0` + per-shape tests (`0x100f6e20` dispatch) [V structure, A details]
- Per particle not attached/removed: a "collision info" segment from x (old) to p* (predicted) with radius
  r = thickness/2 is tested against every candidate shape (group mask, `NX_SF_CLOTH_DISABLE_COLLISION`, triggers and
  disabled shapes filtered). Moving shapes are tested with their previous and current pose unless CCD is off
  (`NX_CLF_DISABLE_DYNAMIC_CCD` or first step).
- Shape type dispatch (NxShapeType numbering): 0 plane `0x100f2ff0`, 1 sphere `0x100f4be0`, 2 box `0x100f4530`,
  3 capsule `0x100f4eb0`, 5 convex `0x100f3200`, 6 mesh `0x100f6bb0`, 7 heightfield `0x100f6ce0`.
- Result encoding in t: −1 none; (−1, 0) proximity (end point within r + margin, closer → larger);
  [0,1) swept impact time; 1 start point already inside one shape; > 1 inside several shapes (positions/normals averaged).
  Contact points are pushed 1e-4 along the normal.
- t ≤ 1: particle gets flag 0x4, contact normal n and plane distance n·c (c = contact point incl. radius offset).
  Each solver iteration then projects **n·p* ≥ n·c** (`0x100e1300`), accumulating n·depth/(w·dt) as impulse.
- t > 1 (stuck in several shapes): the particle is temporarily attached (0x1|0x4) to the last shape at the averaged point.
- Shape flag `NX_SF_CLOTH_DRAIN` (0x8000): touching particles are removed (0x400).
- Plane test: inside if signed distance of x < r; else proximity if end distance < 2r; then swept time
  t = (r − n·x − d)/(n·(p*−x)) accepted in [0,1).

## 2.8.0 vs 2.8.1 [V]
Function-by-function match of the cloth range (`re/out`, text similarity after normalisation; scratch script) and
manual diffs of the solver, ComputeNewState, IntegrateTimeStep, damping, friction, contact generation and attachment
projection: **identical math and constants; only the particle/edge struct layouts and internal vtable offsets differ.**
Same per-iteration stiffness formula, same series-acos, same thresholds (FLT_EPSILON, 1e-6). So the 2.8.1 core is a
valid cloth oracle for the game's 2.8.0 behaviour.

## Oracle (2.8.1 core) results [V]
- `harness/src/cloth_oracle.cpp` builds and runs. Every scenario in `scenarios/cloth/` runs end to end with ok=true,
  no NaN, 0 SDK errors and 2 SDK warnings each. The warnings are hidden by `quietWarnings` and have not been
  inspected yet.
- **Deterministic:** `flag_pinned` run twice gives bit-identical vertex data on all 180 steps (`clothbin.py compare`).
- **Free fall** (`freefall_probe`): v_n = n·g·dt and y_n − y0 = g·dt²·n(n+1)/2, exact to float precision.
  That's symplectic Euler (v += a·dt, then x += v·dt), which matches ComputeNewState plus v = (p*−x)/dt.
- **Resting height on a plane** (`drop_plane`, thickness 0.04): every vertex ends at y = 0.0201 = thickness/2 + 1e-4.
  This confirms collision radius = thickness·0.5 and the 1e-4 contact push-out. That cloth also falls asleep.
- `tear_flag` tears: the vertex count grows 441 → 542 while the triangle count stays 800.
- Wall time for a 441-vertex cloth with 5 iterations is about 1 ms/step (single thread, under Wine).

### How to run
    bash harness/build.sh                    # builds hello, fluid_oracle, cloth_oracle into build/i686
    tools/run_cloth_oracle.sh scenarios/cloth/flag_pinned.txt      # RUNS=n for repeats
    python3 analysis/clothbin.py summary captures/cloth/flag_pinned/oracle-1.bin
    python3 analysis/clothbin.py compare captures/cloth/X/oracle-1.bin captures/cloth/X/oracle-2.bin
The runner uses its own prefix `.wine-harness-cloth/` (created on first use by `tools/setup_prefix.sh`; it holds a
`.gitignore` with `*` so git ignores it) and `build/run-cloth/`. It only kills its own wineserver.
PXCL format: see the header comment of `cloth_oracle.cpp` (header, rest mesh indices and positions, then per step
counts, timing, optional index buffer, and {pos, vel} per vertex in user order).

### Scenarios (`scenarios/cloth/`)
`freefall_probe`, `drop_plane`, `drop_sphere`, `drop_box`, `flag_pinned` (two top corners pinned), `flag_pinned_jitter`
(`jitter 1e-5 1`; change the seed per run to measure spread), `bend_off` / `bend_dist` / `bend_ortho` (cantilever
strip with bending off / distance bending / dihedral bending), `tear_flag`.

## Open / next steps
(Session 4 added capsule contacts, COM damping and a tearing v0; see the last section for what is done and what is left. Items 3 and 6 below are partly done.)
1. **Look at the results (user):** no PNGs have been rendered yet. Write `analysis/render_cloth.py` (like
   `render_fluid.py`: contact sheet of triangles from PXCL) and ask the user to judge `drop_sphere`, `drop_box`,
   `bend_*` and `tear_flag`.
2. **Probes still to run on the oracle:** damping formula (unpinned cloth with an initial shear velocity via
   `setVelocities`, compare against the per-edge Δ = (v_j−v_i)·damping/2), friction (sliding on a tilted plane),
   the per-iteration stiffness s = 1−(1−k)^(1/N) (single edge, 2 vertices, one step), wind along normals, and
   whether `bend_ortho` uses the series-acos rest angle (does a flat strip with ORTHO stay flat?).
3. **RE not yet read:** pressure `0x100e5b70`; self-collision `0x100ef1b0` (with the spatial hash
   `0x100fc8f0`/`0x100fe1c0`); adhere `0x100dfc70`; COM damping `0x100df6e0`; tearing `0x100edb90` → `0x100ea530`
   and split fix-up `0x100e13f0`; sleep and wake bookkeeping `0x100e21a0`/`0x100e23b0`; per-shape tests for sphere
   `0x100f4be0`, box `0x100f4530`, capsule `0x100f4eb0`, convex `0x100f3200`, and mesh/heightfield `0x100f6bb0`/`0x100f6ce0`
   with their LocalReport callbacks at 0x100f4210/0x100f53a0/0x100f5b20; mesh cooking/loading `ClothMesh_vf1` 0x100fb240
   (edge-record build: rest lengths, opposite vertices, rest dihedral angle); attachToCore `+0x42cd0` path
   `0x100e2ff0`; how mesh vertex order maps to internal particles (`+0x2164c`).
   Functions above were identified by decompile reading only. **No Ghidra renames were made** (MCP not used), so
   the `re_cloth_*` names in this file are proposals.
4. **2.8.1 address map:** a 2.8.0→2.8.1 function match of the whole cloth range came from a scratch script
   (normalised text similarity; most pairs score ≥0.94). Key pairs: solver 0x100dd9e0→0x100ddb80,
   ComputeNewState 0x100f0780→0x100ef3e0, Integrate 0x100f0b50→0x100f0db0, contacts 0x100e17b0→0x100dee10,
   attachments 0x100e0d90→0x100e1540, friction 0x100e5e80→0x100e6060, damping 0x100df490→0x100dfcc0,
   pressure 0x100e5b70→0x100e5d50, step driver 0x100f9d20→0x100fa070, init 0x100f12d0→0x100f1540.
   The plane projection 0x100e1300 had no confident match.
5. **Game coverage:** the 87 cloth meshes in `docs/phase1-assets.md` / `captures/assets/` haven't been checked for
   flags or parameter ranges yet, so the scenarios use generic values.
6. **Native solver `solver/cloth/` v0:** not started. Planned order: integrator + stretch + attachments (exact vs
   `freefall_probe` / `flag_pinned`), then plane/sphere/box collision, damping, friction, bending (use a real acos), then tearing.
7. **Pinned / friction divergence (session 5), root cause narrowed to edge order [V unless marked]:**
   - Ruled out (no change in error): pins applied before prediction or before/after edges (`cloth.cpp` pin reset position),
     pinned inverse mass != 0, uniform vs area mass, per-iteration s = 1-(1-k)^(1/N) (other s values fit worse), an
     N-pass variant, duplicated or per-triangle edge records, edge damping on (the oracle has it off), first/last
     appearance and triangle/edge-corner reordering, and sorted or reversed edge orders.
   - Step 0 of `probe_stiffness_pin_iter1` is exact. Step 1 is the first step where a stretch constraint is active. At
     step 1 the oracle's pin-adjacent vertex (1) receives exactly the full correction C*s from the pinned edge (0,1) and
     vertex 2 receives none, so the oracle processes edge (1,2) before (0,1). Ours (first appearance) does the reverse and
     pulls vertex 1 back. The 2.8.1 solver loop (`0x100ddb80`, stride 0x40 edge records) confirms the sequential order
     of the cooked array, so the answer lives in the cooker's edge emission (PhysXCooking.dll), which is not decompiled.
   - An order fit by hill-climbing reproduces steps 0-2 of `probe_stiffness_pin_iter1` but not later steps (end rms 1e-2),
     so it is not the true order. Next step: RE the edge-record emission in PhysXCooking.dll, or fit the order against all
     60 steps of `probe_stiffness_pin_iter1`/`_iter10` and `probe_damping_pinned_ctrl` with a C++ evaluator.
   - Friction: the original's friction block (`0x100e6060` in 2.8.1, `0x100e5e80` in 2.8.0) is guarded by friction != 0,
     which matches `cloth.cpp`. The normal clamp is inside that guard, so there is no separate clamp at zero friction
     [V]. `probe_friction_tilt0` and `probe_friction_tilt` have identical errors at impact (step 11) [V], so the zero-friction
     divergence starts with the same solver-ordering error as item A, not with the friction formula. Friction 0.3 damps that
     tangential error, which is why it stays close [A].


## Status, session 2 (renderer, native solver v0, comparison)
Tools: `analysis/render_cloth.py` (sheet/frames; oblique + side view, static shapes drawn in red; sheets are in
`captures/cloth/renders/`, **not yet judged by the user**), `solver/cloth/` (`cloth.h/.cpp`, `run_scenario.cpp`; build and
run all scenarios with `tools/run_cloth_ours.sh` -> `captures/cloth/<name>/ours-1.bin`, same PXCL format),
`analysis/cloth_metrics.py` (oracle-1 vs ours-1 per scenario, `--json` for more).

v0 implements the pipeline above: gravity/external/wind (area-weighted vertex normals [A]), symplectic Euler, swept
contacts vs plane/sphere/box (r = thickness/2, +1e-4 push), interleaved iterations (per edge: stretch then bending; hard pins;
contact half-spaces), s = 1-(1-k)^(1/N), per-edge damping, friction, bending by distance and by dihedral with a real acos.
Not implemented (sleeping and the shape-contact rules below were added later): tearing, capsule/convex/mesh shapes, self collision, pressure, adhere, COM damping, soft attachments,
two-way coupling.

Findings from the comparison (ours vs oracle) [V unless marked]:
- `freefall_probe` and `drop_plane` match bit-for-bit up to 1e-8 (resting height thickness/2 + 1e-4).
- **A proximity hit (end point within 2r of the plane, no penetration) must not count as a contact for friction.**
  Treating it as one zeroes the normal velocity one step early (drop_plane diverged by 1.1 cm for one step). Our proximity
  constraints are kept for sphere/box/plane but flagged `touch=false` (no friction). Whether the original keeps a proximity
  constraint at all is [A]; for a plane it has no visible effect.
- Cooked edge order is unknown [A]; we use first appearance in triangle order. Sorted/reversed orders were tried on
  `flag_pinned` and are worse (step-0 error 7e-4 vs 8e-4/1.4e-3). The step-0 error of 7e-4 m next to the pins is the
  Gauss-Seidel order effect, not a formula error.
- bend_off (no bending) swings like the oracle, but the oracle loses more energy per swing (final min y -0.678 vs -0.584); changing the damping factor 0..2 does not explain it, so it is something else in the solver
  (soft-pin handling, mass, or edge order) [open].
- bend_ortho at step 1 differs by 3e-3 (the others 3e-5): expected from the original's series acos plus its rest angle [A];
  ours uses a real acos on both.
- tear_flag: the oracle tears off completely (cloth falls away, 441 -> 542 vertices); ours has no tearing, so the table
  row is meaningless until tearing is implemented.
- Chaotic scenarios (drape over sphere/box) diverge vertex-wise by 0.06-0.28 m rms; compare statistics instead
  (centroid trajectory, bbox, strain), as with fluid.

## Cooked cloth mesh stream (`NXS` / `CLTH`) [V unless marked]
Read by `ClothMesh_vf1` (2.8.0 `0x100fb240`; the chunk tag check is `0x100a4c20`, dword/float/word readers
`0x100a49a0` / `0x100a49e0` / `0x100a4970`). Our oracle dumps the stream it cooked as `cooked.bin`
(`captures/cloth/<name>/`). Layout below was read off the reader and checked on the 5x5 probe grid
(`probe_stiffness_pin_iter1`, 2184 bytes). All integers little-endian; the file ends exactly after the last record.
- Chunk header: `"NXS"` + flag byte (bit 0 = 1 means not byte-swapped) + `"CLTH"` + u32 version (3).
- u32 mode (1 = the path used here; 2 selects a different record layout), u32 flags (0), f32 (0.0, meaning [A]).
- Vertices: u32 N (25), then N x 3 f32, **original grid order**.
- Triangles: u32 M (32), then 3M u32, **original indices**, same triangles as the scenario grid.
- Four small lists: f32 list (0 items), u32 list (0), u32 list of N entries (the permutation P, below), u32 list (1 item, 0).
- Groups: u32 G (1). Per group: u32 key (3), u32 c then c u32 (local-to-stored map; identity here, [A] meaning),
  u32 R (56), then R edge records.
- Edge record, 22 bytes, packed: u16 a, u16 b, u16 c, u16 d (d = 0xFFFF on a border edge), f32 L0, f32 kl0,
  f32 phi0, u16 e (0 in all records here; meaning unknown).

Vertex index mapping: the record indices are *stored* indices. Original index = Q[stored], where Q is the inverse of
the N-entry permutation P (P[original] = stored). [V] With this mapping all 56 records are grid edges, c and d are
apexes of the triangles on that edge, and L0 / kl0 equal the distances in the positions block to about 1e-8.
The 56 records are the unique edges of the grid (40 grid edges + 16 diagonals).

Stored rest values: L0 = edge length, kl0 = distance between the two apexes (both [V] above). phi0 is **not** the
solver's convention here: every interior edge of this flat grid stores 2.8875, while the acos of the rest geometry
(our `phi0`) is pi. So the cooked third float is a different dihedral convention or includes an offset [A, open].

Edge order: the records are in the order the solver walks them. Three simple rules do not reproduce it [V]:
first appearance over triangles in original order, first appearance over triangles in stored-index order, and
triangles sorted by their stored vertex triple. The order looks like a hash/map order inside the cooker [A].
The solver should read it from the stream instead of inferring it.
Solver status: `run_cloth_ours.sh` passes `cooked.bin` when present (`OURS_ORDER=first` forces the old first-appearance
order). With the stored order the end-of-run position rms against the oracle drops from 1e-3..1e-2 to 4e-7..2e-6 on
the five pinned/stretched probes; the three free/static ones stay as before. Using the stored L0/kl0/phi0 instead
(`PXC_COOKED_REST=1`) changes little, and the default bending path only uses kl0, so the phi0 convention does not
explain the remaining ~1e-6 [V, probes]; the residual is probably float operation order [A].

## Shape contacts and sleep (session 3) [V unless marked]
Probes: `probe_shape_sphere_centre`, `probe_shape_sphere_offset`, `probe_shape_box_edge`, `probe_shape_box_corner`
(5x5 cloth, 40 steps, `captures/cloth/`). Before this section the first divergence was at step 15-16 (error 1e-2).

**Per-shape tests** (2.8.0 addresses; the 2.8.1 math matches per the function map above [A]). Plane `0x100f2ff0`, sphere
`0x100f4be0`, box `0x100f4530`. Result setters: inside `0x100f2890`, swept `0x100f29b0`, proximity `0x100f2aa0`. Caller:
contact generation `0x100e17b0` loops the candidate shapes per particle and calls the dispatch `0x100f6e20`.
- **One contact slot per particle, shared by all shapes**: t = -1 none; (-1, 0) proximity (closer = larger t);
  [0, 1) swept impact time (earlier wins; a swept hit overrides a proximity hit); t = 1 start point inside a shape. A
  second inside result averages position and normal with the first and raises t to the count. Our solver keeps one
  contact per particle.
- Radii: the shape is expanded by r = thickness/2. **Inside** uses the start point: plane distance < r, sphere |x| < R + r,
  box inside the box expanded by r per axis. Contact point = projection of x onto the nearest expanded surface.
- **Proximity** uses the start point x, not the end point: distance to the expanded surface in [0, r) (plane: start
  distance in [r, 2r)). Contact point = point on the expanded surface, normal = outward normal there.
- **Swept**: entry time into the expanded shape; contact point x + t (p* - x) + 1e-4 n, normal at the impact point.
- **Box = sharp box expanded by r per axis, not a rounded box.** Normals are face normals, so a particle beside a box
  edge gets an exact +y normal, not a tilted one. The rounded version we used tilted by about 0.05 rad and diverged at
  the first edge contact (`probe_shape_box_edge`, step 14).
- **Proximity contacts are normal contacts** for projection and for friction: flag 0x4 is set for any t != -1, and the
  2.8.1 friction block (`0x100e6060`) tests flag 4 only. Our earlier "no friction for proximity" rule came from testing
  the end point and is superseded [V for the shape probes; drop_plane unchanged].

**Sleep** (2.8.0, `0x100f0b50` integrate, `0x100e7820` after friction, `Cloth_vf72/76` = isSleeping / putToSleep).
- Per particle, flag 0x20 when |v|^2 < sleepLinearVelocity^2, with v = (p* - x)/dt before damping and friction.
- The cloth keeps a running wake counter, reset to wakeUpCounter (+0x9c) whenever any particle is fast. If all
  particles are slow, the counter drops by dt; at counter <= 0 the cloth is put to sleep. Sleeping means the step is
  skipped (`ComputeNewState` skipped) and all velocities are zeroed (`0x100dd990`).
- Defaults [A]: sleepLinearVelocity 0.15, wakeUpCounter 0.4 s. drop_box then sleeps exactly on frame 113, the 24th
  consecutive all-slow step (oracle: all 441 velocities exactly 0 from frame 113). No other capture sleeps under this rule.
- **Open:** drop_plane and probe_stiffness_iter1/iter10 zero all velocities on the landing frame (18), while the rule
  predicts sleep at frame 42. The landing case is not explained. Positions still agree there (<= 1e-7).

**Chaos limit** [V]: the centred drop_sphere and drop_box are sensitive to rounding. Moving the oracle's initial grid by
1.2e-7 (`captures/cloth/chaos_drop_*_x0pert`) changes its own 240-step result by 3.6e-4 (sphere) and 1.4e-4 (box). Our
solver moved the same way changes by 5e-4 and 1.8e-3. So a 1e-4 match over the full drop runs needs bit-exact
arithmetic; first-divergence step and statistics are the right metrics.

**Not done:** the t > 1 case (particle inside several shapes; the original attaches it temporarily with flags 0x1|0x4);
shape order (the original uses broadphase order, we use scenario order); capsule, convex, mesh and heightfield shapes.

## Session 4: capsule contacts, COM damping, tearing (solver v0) [V unless marked]
New scenarios: `probe_shape_capsule_cap`, `probe_shape_capsule_side`. Existing: `probe_comdamping`, `tear_flag`.

### Capsule shape (`0x100f4eb0`, 2.8.0)
- Axis = shape-local y; the segment ends are at +-h with **h = height/2** [V: the cap probe with height 0.2 lands on
  the top at centre + 0.1 + radius]. Radius R + r is the expanded radius (r = thickness/2).
- Inside: distance from the start point to the segment < R + r. Contact point = nearest axis point + n (R + r).
- Swept: earliest entry in [0, 1) among the top sphere, the bottom sphere (both at the segment ends) and the infinite
  cylinder, the cylinder hit kept only if its y lies inside the segment. Normal = (point - nearest axis point)/|..|.
  The root helper `0x100f3570` returns the smaller root and falls back to the larger one when the smaller is before
  t = 0. **We take the entry root only** [deliberate: the fallback makes exit hits from inside a cylinder that is
  outside the capsule].
- Proximity: start distance to the segment minus (R + r) in [0, r), normal from the nearest axis point (same rule as
  the sphere). Degenerate start exactly on the axis: we use +y; the original gives a zero normal [deliberate].
- Code: `solver/cloth/cloth.cpp` (capsule branch in `generateContacts`), `cloth.h` (`ShapeType::Capsule`),
  `run_scenario.cpp` (`capsule r h x y z`).
- Result: `probe_shape_capsule_cap` max 8e-6. `probe_shape_capsule_side` (hanging pinned sheet through the capsule
  side and cap rim): the first contact is at step 10 at the cap rim (1e-4 proximity). Error 1.2e-3 at that step,
  then 2.9e-5 at the end. **Open:** tested and ruled out: half-height scale (1.0 is best), proximity range 0.01-0.04
  (no change), flipping the rim normal's y (7e-2), dropping the rim proximity (5e-2). Not explained.

### COM damping (`0x100df6e0`, called from ComputeNewState)
- Runs after v += a dt (gravity, external and wind), before p* = x + v dt. Gate: NX_CLF_DAMPING (0x100) and
  NX_CLF_COMDAMPING (0x4000). **With COM damping on, the edge damping of IntegrateTimeStep (`0x100df490`) is off**
  (its gate is 0x100 without 0x4000).
- Math: mass centre xc and mean velocity vc (mass-weighted); L = sum m r x v; I = sum m (|r|^2 E - r r^T);
  w = I^-1 L; per particle t = vc + w x r and v <- t + (1 - k)(v - t) with k = the full damping coefficient (no 1/2).
- Masses are the stored ones, pinned particles included (their stored inverse mass is non-zero; the solver's w is 0
  for pins, so `mass_` is kept separately). [V: `probe_comdamping`, pinned corners, so w matters]
- Deliberate: singular inertia (points on a line) gives w = 0; the original falls back to an identity-based expression [A].
- Code: `cloth.cpp` `comDamping()` and the gate in `step()`; `run_scenario` flag `comdamping`.
- Result: `probe_comdamping` max 3.7e-6 (was 0.166). `probe_damping` and `probe_damping_pinned` unchanged.

### Tearing (`0x100edb90` detection, `0x100ea530` split) [session 5 update]
- Detection [V]: edges in array order, on the positions after the solve (the call is after the sim step, before friction).
  Tear when |x_i - x_j| >= tearFactor * L0 (`0x1e`, edge `+0x1c`), neither end has 0x400, at least one end has 0x80 and
  not 0x800. Direction for the split is d = x_i - x_j (the edge's stored order), whichever end splits.
- Split end [V for the first tear; A otherwise]: the first eligible end; both eligible uses `in_ECX[0x856b] & 1` (we take
  end i). Flag 0x8 on an end makes it the preferred split end (not replicated, see below).
- Arc split (`0x100ea530`, [V] tear_5x5 step 1): the triangles around v (buffer order) get side = centroid.d > x_v.d.
  Triangles sharing a rim vertex and the same side form one arc. The first arc keeps v; each other arc gets one copy of
  v (state copied, appended in arc order: indices base + g - 1), which takes that arc's corners. Torn edges are not cut.
- Post-split [V on tear_5x5 indices]: the moved edges are rewired in place (the copy takes the edge's slot), and the
  splits are applied one at a time, so later edges see the new topology. The original also sets flag 0x8 on the
  non-pinned rim vertices and clears 0x8 on v; the copy gets v's flags with 0x800 cleared [A, from the decompile:
  not implemented].
- Oracle facts: copies start at the parent's position and velocity; pinned ends never split (tear_flag). tear_5x5
  step 1: copies 25 (of 9) and 26 (of 5), tri [5,10,6] -> [26,10,6], [8,14,9] -> [8,14,25]. Step 2 (oracle): copies
  27 (of 8), 28 (of 6), 29 (of 7); ours makes 27 and 28 but not 29 (see open).
- Code: `solver/cloth/cloth.cpp` `tearStep()` (arc split, one rebuild per split), `rebuildEdges()` (edges kept in
  stored order; moved pairs rewired to the copy with the same rest vertex; new pairs appended), `recomputeMass()`.
  `cut_` removed (no cut in the original). `run_scenario`: `cloth.tearFactor`, flag `tearable`, `mesh.tearable`.
- Results: tear_5x5 (`scenarios/cloth/tear_5x5.txt`): step 1 identical (27 vertices, same triangles); step 2 position
  error 1.5e-3 (solver order / bending of moved springs; not zero), count 29 vs 30. tear_flag: onset identical (441, 441,
  447 vs 449 at step 3); count 530 at step 10 (oracle 529), 530 at step 12 (oracle 541), 542 final: ours stops at 530
  (oracle 542). Previously stuck at 513.
- **Open:** (1) step-2 position error: the oracle's springs after a split are not in our appended order (appending
  the new pairs after their source slot gave count 30 but error 4.7e-2, so neither order matches); (2) the extra copy
  of vertex 7 in the oracle at step 2 (a split of 7 along the vertical edge 7-12 or a second tear of the same arc),
  which our step-2 positions do not reproduce; (3) 0x8 next-tear priority and the 0x800 flag not implemented.

### Crack tips (flag 0x8) and the tip re-split [V: decompile of 0x100edb90 / 0x100ea530; oracle counts] (2026-10-13)
- Split end: an end is eligible when tearable (0x80) and not dominated (0x800, set only by dominateVertex, which the
  game never calls). Both eligible: the end with 0x8 (i first), else i. The "i or j" choice reads bit 0 of a pointer
  field, so it is always i. A split of an attached vertex (flag 1 or 2) is refused and nothing tears on that edge
  (ours used to skip every edge with a pinned end, which also kept the free end of such an edge from splitting).
- After a split with a direction, the far ends of the edges of v whose two triangles went to different copies (the
  opened edges, in edge-record order) are the crack tips: each gets 0x8 unless attached, and each is split once more
  without a direction (all triangles on one side, so only a fan that no longer hangs together comes apart: a border
  vertex the cut ran into). That pass marks its own tips but does not recurse further. v loses 0x8; copies start
  without it. The detection pass walks only the edge records present when it started.
- Effect: a tear continues from the tip along a line instead of starting new cuts beside it. tear_5x5 vertex count
  now equals the original's at every step (the "extra copy of vertex 7" above was a tip re-split); tear_flag step 2 exact
  (449), final 538 vs 542 (was 530); bullet_front 366 vs 369 (was 345), graze 404 vs 403 (was 496), oblique 353 vs 360
  (was 388). Non-tearing scenarios bit-identical.
- Not replicated: the original keeps an opened edge's second spring inside the same record (fields 4/5) and never
  tear-tests it; ours appends it as an edge of its own. Making appended edges non-tearing gave no clear gain (tried).
- Step rate matters far more than any of this: the same 12-hit banner (0.27 m, game desc) in the original tears to
  375-435 vertices at 62 Hz steps, 280-328 at 144 Hz, none at 250 Hz.

### Regression (this session, max over scenarios, rms_worst / max@end)
freefall 0 / 0; drop_plane 1.5e-8 / 0; damping 3e-8 / 3e-8; wind 3e-8 / 3e-8; stiffness iter1, iter10 1.5e-8 / 0;
flag_pinned 1.0e-6 / 6.7e-7; bend_dist 4.0e-6 / 1.1e-5; bend_off 3.9e-6 / 6.5e-6 (all unchanged from before).
drop_box 8.6e-5 / 5.7e-5 and drop_sphere 1.0e-4 / 2.3e-4 are unchanged (the brief's figures 0.21 / 0.29 do not
reproduce; `HEAD` is bit-identical when built with the same flags). probe_shape_sphere_* 8e-6 / 2e-5,
probe_shape_box_* <= 3.5e-6. `bend_ortho` (0.07) and `attach_shape` (0.46) are unchanged from HEAD (truncated acos
artifact not replicated; attachments to shapes unsupported).


## Self-collision (NX_CLF_SELFCOLLISION, flag 0x8) [V unless marked]
Prepass of ComputeNewState (`0x100ef1b0`, 2.8.0), called first, before gravity/wind and before p* = x + v dt. Gate:
flag bit 0x8 in the cloth flag byte, not sleeping, dt != 0, thickness != 0. It changes velocities only (no positions).
- **Distance:** D = thickness (the original's 2 x collision radius). Predicted positions q = x + v dt with the velocities of
  the previous step (gravity not yet added).
- **Three passes** per step. Each pass bins every particle by its current q in a spatial hash, then tests pairs.
  - Cells have side L = 10 D, floored per axis: cell = floor((q - off) / L), off = 0, +D, -2D for passes 0, 1, 2.
  - Pass 1 bins only particles with some residual q_k - cell_k L >= L - 2D; pass 2 bins only particles with all residuals
    <= 4D. Bin = (cy*0x4b ^ cz*0x19 ^ cx*(-9)) & 0xff (256 bins).
  - Within a bin, pairs (i, c), i before c in particle order, in bin order; pass p+1 re-bins after pass p changed velocities.
- **Prefilter (`0x100ef1b0` at 0x100efc36):** a pair is skipped if the squared distance of the *cooked rest* positions
  (the +0x70 copy made when the particles are built) is <= D^2. Rest neighbours and duplicated vertices never interact.
  [A: setPositions does not refresh +0x70; the oracle probe with jitter gave no clean test of this.]
- **Test:** pair skipped unless |q_c - q_i|^2 < D^2 (current velocities, so earlier pairs of the same pass count).
- **Correction (impulse on velocities):** d = q_c - q_i, r = |d|, s = (r - D)/r / (w_i + w_c) / dt; v_i += s d w_i,
  v_c -= s d w_c (w = inverse masses; pinned w = 0). Then **relative-velocity damping with the cloth friction (+0x64):**
  k = friction * 0.5 / (w_i + w_c); v_i += k w_i (v_c - v_i); v_c -= k w_c (v_c - v_i), using the post-correction velocities.
  Momentum is conserved; with friction 0.5 the relative velocity drops by 25% per tested pair.
- Write-back: velocities only, then gravity, wind and p* = x + v dt follow in ComputeNewState.
- Cost: O(n) per pass plus the pairs inside each bin (three passes, 256 bins). Our code follows this exactly.

Code: `solver/cloth/cloth_self.cpp` (`selfBin`, `Cloth::selfCollision`, `Cloth::selfPair`), hook in `cloth.cpp` `step()`
(before gravity), field `ClothParams::useSelfCollision` (`cloth.h`), runner flag `selfcollision` (`run_scenario.cpp`).
Oracle flag: `flag selfcollision 1` (keyword present in `harness/src/cloth_oracle.cpp`).

Probes (oracle 2.8.1 core, our solver, `analysis` scratch checker): `self_sphere_fold` (21x21 sheet, 1 m, dropped on a sphere
r 0.2 on a plane, thickness 0.03, 150 steps). Self-collision off: 113 non-rest-neighbour pairs closer than D at the end
(min distance 4e-4). On: 58 pairs (min distance 9.5e-3). Oracle: 39 pairs (min 6.8e-3). Vertex error is zero until the
first contacts, then rms 9e-2 by mid-run. **Not yet exact:** the first contact step does not match. Candidate causes [A]:
pass/bin acceptance details, prefilter reference (rest vs initial), order of the velocity write-back.
`self_pinch` (4 pinned corners, sphere under the centre, thickness 0.12): no pair is ever tested, on and off identical.
`self_jitter*` (jittered initial positions): the oracle with self-collision off already differs from ours at step 1
(vel up to 0.3 m/s), so the jitter setup does not match (not a self-collision issue); not used for conclusions.

Regression (self-collision off, our build vs oracle captures, rms worst): freefall 0, drop_plane 1.5e-8, damping 3e-8,
wind 3e-8, stiffness iter1/iter10 1.5e-8 (all within 1.1e-5). flag_pinned 2.4e-2, bend_dist 1e-1, bend_off 1.5e-1,
probe_comdamping 3e-3 **exceed 1.1e-5**; the committed HEAD sources give the same values against the current oracle
captures, so these come from the captures or the parallel attachment/tearing edits, not from self-collision.

## Attachments (session 5: `solver/cloth/cloth_attach.cpp`) [V unless marked]
Entry points, decompiled from 2.8.0 (NpCloth vtable slots 24-29 forward to Cloth slots 86-91): attachToShape
(`0x100dca30`), attachToCollidingShapes (`0x100e8cd0`), the per-vertex projection (`0x100e0d90`), the point-in-shape
predicate used by both attach calls (`0x100f6490`, the same per-type tests as the contact code, with radius = thickness/2).
- **Flags** (NxClothAttachmentFlag): bit 0 TWOWAY, bit 1 TEARABLE. As decompiled: a **one-way attachment is hard**
  (flag 0x2 set: vertex = target, w treated as 0) and a **two-way attachment is soft** (flag 0x2 clear). Tearable sets 0x100.
- **Which vertices:** attachToShape takes every vertex that is not removed and is not already attached to a shape (and
  is inside or within r of the shape). attachToCollidingShapes takes the same vertices and attaches each to the first
  candidate shape that passes the predicate; the candidate order is not known [A: we use addShape order].
- **Local position:** stored in the shape frame, +0x30 = R^T (x - t) with the shape's cached rotation (+0x40 .. +0x60)
  and translation (+0x64 .. +0x6c). The cache is refreshed when the body's pose generation (+0x178) changes.
- **Projection (per solver iteration, after the edge constraints and before the collision projection):** target =
  t + R * local. Hard: p = target. Soft: p += (target - p) * w / (w + wb), wb = body inverse mass (0 for static or
  kinematic). The reaction -(target - p) / (w + wb) / dt is written to the vertex's impulse slot (assignment, so the last
  iteration's value stays). The response factor (attachmentResponseCoefficient) is applied when the impulse reaches the
  body [A: header says "factor for the impulse transfer to attached bodies, two-way only"].
- **Tearing** (inside the stretch loop): an attached end of an edge whose length exceeds attachmentTearFactor * rest length
  is detached (flags 0x1, 0x2, 0x100 cleared) if its attachment is tearable. Checked before that edge's constraint.
- **Collision:** attached vertices take no contacts (the contact loop skips flag 0x1). Their velocity comes from the
  position change, as for every vertex.
- **Global attachments** (attachVertexToGlobalPosition, the desc flag NX_CLOTH_VERTEX_ATTACHED) are the pin path (hard,
  w = 0). freeVertex restores the inverse mass.

### What we implemented (`cloth_attach.cpp`, hooks in `cloth.cpp`: contacts skip, `attachTearEdge` in `solveEdges`,
`attachProject` before the hard-pin loop; declarations and `AttachInfo`/`ShapePose` in `cloth.h`)
- Shape frame: without `setShapePose` a sphere, box or capsule frame has its origin at the geometric centre (so the
  arithmetic is (x - c) + c, as in the core with an identity actor pose); planes use the origin.
- `setShapePose(shape, t, R)` (world = t + R * local, row-major), `setShapeInvMass(shape, w)`, `attachToShape`,
  `attachToCollidingShapes`, `detachFromShape`, `attachVertexToShape`, `attachVertexToGlobalPosition`,
  `attachVertexToInitialPosition` (NX_CLOTH_VERTEX_ATTACHED, uses the rest position [A]), `freeVertex`,
  `setAttachmentResponse`, `setAttachmentTearFactor`, `attachReactions()` (two-way vertices: shape, world target,
  response * impulse).
- Scenario directives (native runner and oracle): `attach shape k flags`, `attach colliding flags`,
  `cloth.attachmentResponseCoefficient`, `cloth.attachmentTearFactor`. Attachments are applied before the pins, as in the
  oracle, and pins now go through `attachVertexToGlobalPosition`.
- **Not done:** collision with moving shapes (contacts still use the static geometry from `addShape`); the pose only
  drives attachments. Shape ids are `addShape` order, so the shim must map its shapes to the same order.

### Oracle results (2.8.1 core, `attach_*` scenarios; max over steps of the vertex position error)
| scenario | before | after |
|---|---|---|
| attach_shape (box two-way: no vertex; colliding one-way to sphere) | 0.483 | 0.00246 |
| attach_shape_sphere (attachToShape one-way to sphere) | 0.483 | 0.00246 |
| attach_twoway (colliding two-way, soft) | 0.483 | 0.000636 |
| attach_tear (tearable one-way, factor 1.1, stretching 0.3) | 0.446 | 0.00166 |
Tearing matches the oracle: the six attached vertices release between steps 5 and 20 in both (within 4e-4 in position at
step 20). The residual of attach_shape is float-rounding amplified by this sheet: a 1e-7 change to one attachment
target moves our own step-70 output by 2.6e-4 (checked by perturbation), so it is not an attachment-logic offset [A].
Regression (freefall, drop_plane, damping, wind, stiffness x2, flag_pinned, bend_dist, bend_off, probe_comdamping):
bit-identical positions and velocities with and without the attachment hooks, and equal to the figures above.
Moving shape (native only, no oracle): hard attachments track a moving pose with zero error; two-way attachments produce
reaction impulses.

### For the shim
Per step, before `step(dt)`: `setShapePose(id, t, R)` for every shape that carries an attachment, with the shape's world
pose (origin at the centre for sphere, box and capsule). Per body: `setShapeInvMass(id, 1/mass)` for dynamic bodies, 0
otherwise. After `step`: `attachReactions()` and apply each `impulse` at `point` (NX_IMPULSE). Shape ids are the
creation order of the `addShape` calls on the native side and must match the shim's list.

### Oracle gap
The oracle cannot move a shape: `attach` works on static actors only. A moving-shape scenario needs a keyword such as
`kinematic k` (a kinematic actor) and `move k frame x y z` (`NxActor::moveGlobalPose` before each step). Mesh vertex flags
(NX_CLOTH_VERTEX_ATTACHED in the cooked mesh) need a directive too, so the desc flag is untested against the oracle.
The Oct 9 oracle build rejected `attach shape` lines; the Oct 10 build added support. This describes the historical probe setup, not a current parser limitation.

## Moving shapes (session 6: `setShapeMotion`, cloth.cpp; probes `scenarios/cloth/dyn_*`) [V unless marked]
Decompile: `0x100e17b0` (contact loop) passes a mode to the per-shape dispatch `0x100f6e20`. Mode 0 (swept) when the
cloth has a previous state and `NX_CLF_DISABLE_DYNAMIC_CCD` (0x20000) is clear; otherwise mode 1 (current pose only).
In mode 0 the dispatch keeps the shape's previous and current pose (cache at shape +0x1d.. and +0x10..) and tests the
particle segment with its start in the previous shape frame and its end in the current frame. Hit points come back
through the previous rotation in the decompile, but the oracle needs the current pose for them [V, see below].
- **Kinematic** (oracle `kinematic k` + `move`): first contact matches a static shape at the same pose exactly
  (`dyn_capsule_sweep`, z = 0.0701 at step 44 both). The current-pose and swept variants agree there, so the kinematic
  rule is undetermined: we use current pose [A]; the swept variant gives a lower run error (capsule 0.241 vs 0.268,
  sphere 0.259 vs 0.319).
- **Dynamic** (`dynamic k mass`, initial velocity from `move`): swept hit with the segment from the previous to the
  current pose. `dyn_sweep_v20` first hit (step 2) is exact (sheet z 0.3201 both) only with hit points mapped by the
  current pose; with the previous pose the sheet stays at z = 0.0 [V]. Normals: current rotation.
- **CCD off** (`flag disable_dynamic_ccd 1`): current pose only; `dyn_sweep_v20n` max error 8e-5, `dyn_tunnel_noccd`
  2.7e-7 [V].
- **Waking**: a sleeping cloth is woken by a moving shape that touches a particle (sleep rule ignores shapes). Without
  this the swept sheet slept and stopped responding (`dyn_sweep_v20`) [A rule, V effect].
- **Not matched yet**: `dyn_tunnel_ccd` (30 m/s, dynamic): the oracle shows no response at any step, our swept test
  hits at step 1. 20 and 40 m/s match at their first hit; 25 m/s does not (oracle responds at step 3, we at 1). Open.
- Residuals (max vertex error over the run): kinematic capsule 0.27 (first diverges step 43, 1.7e-3), kinematic sphere
  0.32 (first diverges step 11 on the 4 neighbours of the centre, 7e-4, proximity band), dynamic 20 m/s 0.95 (exact at
  step 2, diverges at step 3 by 0.03, so the friction or the second contact is different). Friction relative to the shape
  velocity is untested. Two-way collision impulses (NX_CLF_COLLISION_TWOWAY) are not implemented.

### API (solver/cloth/cloth.h)
`setShapeMotion(shape, t, R, prevT*, prevR*, dynamicBody)` per step before `step()`; without prev the shape is
tested at its current pose. It also sets the attachment pose (`setShapePose`). `ClothParams::disableDynamicCcd`.
Regression: the 38 static scenarios are bit-identical to HEAD (positions and velocities; the wall-clock field of the
dump differs, as before).

## External forces: addForceAtVertex / addForceAtPos / addDirectedForceAtPos (NpCloth 82-84 → Cloth 100-102)
Read from the 2.8.0 binary (0x100dc040 per vertex; 0x100e8560 shared by both AtPos calls; per-mode jump table at
0x100e8c2c) [V structure, no oracle run]. The game uses addDirectedForceAtPos for bullet hits (helicopter in Escape_p).
- A static cloth (NX_CLF_STATIC) ignores all three. The velocity changes at the call, the next step integrates it, and
  the cloth wakes with the desc wakeUpCounter.
- Modes: force → v += f·invMass·dt, impulse → v += f·invMass, velocity change → v += f, acceleration → v += f·dt; the
  two smooth modes do nothing. dt is a scene field (+0x74) [A: the scene's step].
- AtPos: particles strictly within the radius, except attached ones (particle flag 1 [A: attached]) and flag 0x400,
  get weight 1 − d²/r². The direction is the given vector (directed) or away from the point (radial). It is normalised
  and scaled by weight·magnitude. For force and impulse the magnitude is first divided by the summed mass of the
  particles in reach, so the hit gives a velocity, not a per-particle impulse. Radius ≤ 0 or no particle in reach: no-op.
- An extra pass runs only for a cloth attached to a core (attachToCore, not supported by us); left out.
- Ours: `Cloth::addForceAtVertex/addForceAtPos` (cloth.cpp), shim `o_add*Force*` with dt = the scene's last substep.
  Tearing then follows from the stretch, as in the original. A 21×21 grid at 0.1 m spacing, pinned at the top, starts
  tearing between impulses of 5 and 20 at radius 0.25 m.
- Oracle check (`scenarios/cloth/bullet_{front,graze,oblique}.txt`, harness directive `force`): a game-like banner
  (0.27 m spacing, density 1, tearFactor 1.5, top row pinned) hit twice by the game's bullet (impulse, |f| 480,
  r 0.5). Peak speed matches the original exactly (72 / 69 m/s front / oblique) [V]. Both shred the banner; the
  original leaves 127 / 186 / 89 vertices hanging, ours 254 / 126 / 108. A grazing hit (force in the cloth's plane)
  pulls the whole banner off its pins at 100+ m/s in both.
- Ours, not in the original: the summed mass is floored at half the mass of an intact disc of the radius
  (density · πr² / 2). A hit at a hole or torn edge otherwise spreads the impulse over the few light particles left
  there (thousands of m/s). Intact cloth never reaches the floor, so the scenarios above are unchanged.

## Triangle meshes (cloth vs static meshes, convexes, heightfields)
Original (2.8.0): `0x100f6bb0` queries the mesh with the particle path's box grown by the radius; the per-triangle
report `0x100f53a0` (`combinedSphereTriangleMesh` LocalReport; the heightfield one at `0x100f5b20` has the same shape)
[V structure, A details]:
- r = thickness/2, q = closest triangle point to the start x. Inside: |x − q| < r with x on the front side → contact at
  q + r·(x − q)/|x − q|. Proximity: front side within 2r [A]. Swept: the face only when moving against its front
  (one-sided), then the three edges (swept sphere vs segment) and vertices, from either side.
- Every triangle is a separate inside result, so a particle near the shared edge of two triangles counts as "inside
  several shapes".
- Oracle `scenarios/cloth/mesh_box_drape.txt` (drop_box with the box as a cooked 4×4-per-face triangle mesh, harness
  directive `boxmesh`): the original lets up to 48 of 441 vertices into the box (39 at the end). The same box as a box
  shape: 1, briefly.

Ours (`cloth_mesh.cpp` AABB tree, `meshTests` in cloth.cpp), improvements over the original:
- One result per mesh: the closest inside triangle, otherwise the earliest swept hit.
- Two-sided: each triangle's front is the side the particle starts on (inside, proximity and the face sweep), so a
  particle crosses no triangle either way. UE3 cooks mirrored meshes with NX_MF_FLIPNORMALS, which the original
  honours and we cannot see, so a one-sided test could take the wrong face for the front: cloth then passed through the
  visible face and was held inside. A test banner in front of a flipped wall: one-sided, a light hit left 216 of 280
  vertices inside it; two-sided it behaves as with the correct winding. mesh_box_drape is unchanged. Shim switch
  `PXSHIM_CLOTH_MESH=0` drops scene meshes for cloth (bisecting).
- A post-solve sweep of the solved path x → p against meshes: the edge solve could drag a particle through a wall it
  wasn't heading for, and a closed mesh never pushes it back out. Meshes only, so mesh-free scenes keep the original's
  results (all 51 scenarios bit-identical).
- mesh_box_drape through the shim: 0 vertices inside at every step, top at box + r (0.620).
- Shim: static meshes clipped to the cloth's reach (`meshOf`), body convexes whole; both were skipped before (cloth
  fell through ledges and awnings once awake).

## Self contact, particle vs triangle (ours, not in the original)
The original's self-collision (flag 0x8, cloth_self.cpp) keeps particles a thickness apart from other particles only.
The game never sets the flag at creation (desc flags 0x1020), and its cloths are 0.01 thick with vertices far further
apart, so even with the flag on, torn pieces lying on each other and folds would pass through each other.
- `cloth_selftri.cpp`, `ClothParams::selfContactDist` (h; 0 = off, the default in the solver and the scenarios).
  Per step: a hashed grid of triangle boxes over the step; each vertex's candidates (not its own triangles) go through
  an SSE2 filter on the triangle planes at the start and at the prediction (4 triangles at a time), then the exact
  test: over the triangle at either end and closer than h or crossing; pairs within 1.5 h in the rest shape are
  skipped. The side at the start is kept (a vertex in the plane takes the side against gravity). Per solver
  iteration, after the edge solve: point-triangle distance projection to h, by inverse masses and barycentric weights.
- Shim: on by default, h = 0.2 x the mesh's median rest edge, at least the thickness (`PXSHIM_CLOTH_SELF` sets the
  factor, 0 turns it off). tools/run_cloth_shim.sh keeps it off so hosted runs compare with the oracle.
- Native checks (scratch tests, thickness 0.01, spacing 0.1, h 0.025): two sheets of one cloth stacked on the floor,
  gap 0 -> 0.018; a towel fold (edge carried over onto the other half), coplanar vertex-triangle pairs 540 -> 19 and
  folded vertices lying in the floor layer 126 -> 3.
- Cost: 1681-vertex falling flag 0.44 -> 1.46 ms per step (the grid walk: ~85 bucket items per vertex for ~6
  candidates). A cached candidate list (Verlet list) and single-cell queries were tried and were slower here: the
  vertices move about a margin per step. Sleeping cloths skip it.

## Shape tree (ours, performance only)
generateContacts builds an AABB tree over the step's shape boxes (static: culling box; posed or kinematic: the ball
around the pose) when a cloth has more than 16 shapes; each particle visits the shapes near its path plus planes and
swept bodies, in index order. Same visits as the plain loop minus shapes it would cull, so results are bit-identical
(51 scenarios; a 41x41 cloth among 500 boxes: identical output, 2.36 -> 0.57 ms per step).

### Self contact, session 2 (game feedback: respawn falls, torn pieces still z-fight)
- Game flags (trace): 0x1020 -> 0x9020 (VALIDBOUNDS) at setup, then 0x9020 <-> 0x9022: the game freezes cloths with
  NX_CLF_STATIC and unfreezes them; a few cloths get SELFCOLLISION (0x20 -> 0x28). The shim now skips the whole step
  of a STATIC cloth, as the original (oracle `shim_static_flag`: frozen for 60 steps in both).
- 2.8.0 Cloth slots 58 / 60 (setVelocity / setVelocities, 0x100dd520 / 0x100dd5d0) write velocities only, no wake-up.
  Ours woke the cloth; UE3 writes velocities every frame after unfreezing, which woke cloths the original keeps asleep.
- Self contact: mesh-neighbour exclusion instead of the rest-shape one (torn pieces adjacent at the tear collide);
  inelastic velocity pass with friction (pile max speed 1.8 -> 0.6 m/s); pair sides persist across steps; pairs left
  unresolved 30 steps are dropped; pushes capped at h.
- Open: torn pile test (25x25 sheet blasted by impulses of 40 at r 0.5, ~30 m/s) ends with ~965 intersecting triangle
  pairs (461 without self contact), formed during the violent tearing (fast pieces pass each other between steps), not
  while settling; they z-fight once the pieces lie down. Edge-edge pairs (selfEdgeFind, now off) made it worse (constant
  agitation at creases, runaway tearing). Next: continuous vertex-triangle detection for fast pairs, and measure the
  game's bullet magnitudes (logged now: "cloth ours: force call").
- Session 3: the game's bullets are NX_IMPULSE 480 at radius 0.5 (very violent: hundreds of units/s on the hit
  particles). Respawn falls: the sleep rule woke a cloth when a "moving" shape touched it, and posed statics (rotated
  level boxes, fed through setShapeMotion every step) counted as moving; now only shapes whose pose changed since the last
  step wake a cloth. Game cloths have no bending flag (0x1020), so pieces fold flat along an edge: for cloths without
  bending, self contact now excludes only the vertex's own triangles (flat folds were z-fighting with themselves).
  Slow particles of acting pairs stop (piles creep at half the speed; they still do not fully sleep).

### Untangling (2026-10-13; game run 20261013-130704, a shot non-tearable banner folded through itself and stayed)
- Cause: once part of a cloth is through the cloth (bullet hits throw vertices at 40-60 m/s, faster than the
  vertex-triangle pairs catch), the pairs held each vertex on the side it was on, and pairs left on the wrong side for
  4 x kGiveUp steps adopt that side. The original (no self contact) also stays tangled: cloth001 of that run ends with
  41 crossings in the oracle replay, 7 in ours before, 2 with self contact off.
- `untangleFind` / `untangleSolve` (cloth_selftri.cpp): per step, edges piercing a triangle of the same cloth (own grid
  of tight triangle boxes at the step start, SSE2 box filter, plane-side filter, then the exact segment test).
  Global intersection analysis (Baraff/Witkin/Kass 2003): the piercing edges cut each piece into regions; the
  non-largest regions went through; pairs across regions are dropped with their remembered sides, and a piercing edge
  between regions pulls its smaller-region vertex to the other vertex's side of the triangle. Contour shortening
  (Volino/Magnenat-Thalmann 2006): piercing points chain into curves (two per intersecting triangle pair); each slides in
  the triangle's plane toward its curve neighbours. That resolves curves that cut nothing off (two borders hooked
  through each other: cloth002 of that run, 2 piercings held for 2500 steps). Both pulls run once after the solver
  iterations, at most 2 m/s and half the distance to the curve neighbour: inside the iterations the edge solve took
  back most of each pull at the game's 300 FPS (run 20261013-135341 cloth007: two neighbouring triangles hooked
  through each other 0.1 m apart for the whole run). Search every 2 steps while tangled, every 4 while not; no
  piercing, no change.
- Replays (dual, i686), mean crossings: 130704 cloth001 7.7 -> 0.5 (end 7 -> 0), cloth002 0.7 -> 0.1; 135341 cloth007
  8.8 (pulls inside the iterations) -> 0.5. Over 57 game recordings, pulls inside vs after the iterations: summed means
  515 -> 472, summed end counts 1239 -> 1047 (21 recordings better, 9 worse; chaotic per recording). Crumpled torn
  piles of 500 crossings (013929 cloth005) stay tangled. No pieces through floors (--fall 0, as before). 80 native
  scenarios identical (they have no self contact).
- Cost: the search is ~30% of a step on a crumpled torn pile (~17 triangle boxes per edge): 1.7 -> 2.5 ms per step
  single-threaded; untorn banners +10-20%. In game (run 135341, 8 banners): fetch wait max 0.56 ms.
- User test (run 135341, the version with pulls inside the iterations): "better, not perfect".
- Back-off (same day): a tangle that does not shrink (no fewer piercings, curve no 10% shorter than its best) is
  searched every 4, then 8 steps instead of 2. Progress has to include the curve length: on piercing count alone the
  hooked banners (2 piercings while they slide apart) were throttled, and dropping the pulls between searches stalled
  them (130704 cloth001 mean 0.5 -> 4.6). Cost, single-threaded, sum over run 141629 (12 banners) + 013929 cloth005:
  HEAD 16.8, untangling 19.5 (+16%), with back-off 18.3 ms per step (+9%); the torn pile 2.4 / 3.8 / 3.3. Crossings
  over 72 recordings: summed means 507 -> 416, end counts 1099 -> 989 (chaotic per recording; small banners unchanged).
  Tried and dropped: skipping the search while the cloth barely moves (no measurable saving). Piles never sleep, with
  or without untangling (asleep 0-1 steps in every recording at HEAD too), so the pulls cost no sleep.
- Poked vertices (game run 20261013-171200 cloth002, a torn banner stuck for the rest of the run): a single vertex
  through the two triangles next to one of its neighbours. The curve ends at that shared corner (edges sharing a corner
  with a triangle are not tested), nothing is cut off, so no pull-through; the two curve slides at the poked vertex
  pointed opposite ways and cancelled. Tried: adding the normal part of the contour gradient (moving the edge across
  the plane shifts its piercing point) - no better. Kept: once a tangle stopped shrinking, a vertex with at least half
  its edges piercing while the edge's other end has under half that share is pulled through to the other end's side,
  in a region of its own (its self contact pairs dropped). Acting before the tangle is stuck, or with a stricter rule,
  was worse. 76 recordings, without the two crumpled torn piles (chaotic: they changed as much with the rule never
  acting on them): summed mean crossings 98 -> 65, end counts 373 -> 217; 171200 cloth002 mean 1.2 -> 0.1, end 3 -> 0.

## Game recordings and replays (debugging on the game's exact input)
- The shim records every solver call of every hosted cloth (`solver/cloth/cloth_rec.h`, `RecCloth`; on by default,
  `PXSHIM_CLOTH_REC=0` off) to `PhysXLoader_cloth<id>.pxcr` next to the log; `tools/run_game_logged.sh` collects them.
  Calls that change nothing are skipped; every 60th step stores a hash of the positions.
- `solver/cloth/cloth_replay.cpp` replays a recording natively (hashes matched on all 11 cloths of an Escape_p run):
  hits and what follows them, `--frames a b out.bin` (PXCL), `--trace a b` (every call), `--flicker a b` (front/back
  swaps of resting vertices), experiment switches `--self`, `--refdt`, `--no-mesh`, `--drop-shapes n`.
- `harness/src/cloth_replay_oracle.cpp` feeds the same recording to the original core: cloth cooked from the recorded
  rest mesh, same parameters and calls, moving shapes as kinematic actors on their recorded poses (no two-way feedback).
- Findings on the Escape_p run (helicopter): the game sets thickness 0.3-0.6 and stretching 0.5 on some cloths after
  creation (setThickness / setStretchingStiffness), steps cloth at ~150 Hz, and writes every particle velocity each
  frame. cloth004 (the only non-tearable banner, 23 x 7.6 m) is stretched 2.5-3x by a ragdoll landing in it; the
  original stretches it the same (2.4-3.4x) [V replay]. The bullet hit that followed: ours 1046 m/s / 18x stretch,
  original 562 / 5.6x. Cause: the step-rate rescaling (refDt 1/62 at dt 1/150) gave a soft cloth one iteration at
  stiffness 0.15. Fix (cloth.cpp step): a step with any edge predicted past 1.5x rest uses the given stiffnesses and
  all iterations. After: 554 / 5.6x on the replay; tearable banners tear on the same hits as the original (cloth000:
  first hit no tear, 1039 vs 1051 vertices after the second; cloth001 1208 vs 1237), fragments ~100 m/s as the
  original instead of 300-600.
- Floor/fold flicker (front/back swaps of resting vertices against another piece's triangle, `--flicker`): cloth001
  on the same input: original 1807 swaps over the session, ours as played 1581. Cause in ours: the game writes a small
  steady velocity into every particle each frame (~0.03 m/s, wind through setVelocities), so crumpled pieces creep;
  a self-contact pair that left the range for a step came back with the side of that moment and was pushed through.
  Sides are now remembered for 30 steps after a pair leaves (`kSideMemory`, cloth_selftri.cpp): 218 swaps (with the
  violent-step fix). Holding stuck pairs at a few mm instead of skipping them cut it about as much but agitated the
  torn-pile test, so stuck pairs stay skipped.
- Correction (second Escape_p run): the violent-step fix above made banners nearly untearable (cloth002, 82 hits, 34
  splits) and is removed. The game runs its scene with variable timing (recorded steps 0.0067 / 0.0049 s), so the
  original stepped cloth once per frame: at the 62 FPS cap in the original game. Oracle replays with the game's frame
  times on fixed 1/62 s steps (`cloth_replay_oracle --fixed 0.016129`) shred cloth002 (986 -> 1498 vertices, then the
  original diverges to 1e8 m/s at its buffer cap); at 150 Hz steps the original barely tears it either. The original's
  tearing order is confirmed in the step function (0x100f0b50): solve (0x100e5860), velocities and sleep flags, then
  tear detection (0x100edb90) after the solve.
- Fix (cloth.cpp step, refDt rescaling): iterations scale with N r per step (the original's iterations per second)
  instead of N r^2. Re-timed replays of cloth002 against the original at 62 Hz (torn vertex counts at the end: original
  1363; ours 1303 at 100 Hz, 1335 at 150, 1400 at 200, 1361 at 300, 1449 at the game's own 150-200 Hz frames; N r^2:
  1498 = buffer full at 124 and 248 Hz). Cloth keeps stepping once per frame, so physics runs at the game's frame rate.
  (A fixed 62 Hz clock with interpolated output matched as well but capped physics at 62 Hz; rejected.)
- cloth004 (non-tearable) at 62 Hz: the hit's spike is the original's too (5.24x vs 5.21x on the same edge). Ours caps a
  hit on a cloth that cannot tear at two median rest edges per step (addForceAtPos): soft test banner 45 m/s / 1.86x
  instead of 190 / 4.9x.
- The game reads velocities, adds wind and writes them back every frame (written ~ read + constant, e.g. (0, -0.10, 0)
  m/s per frame on cloth004). The shim applies a write after a cloth step since the read as a change on the current
  velocities (o_setVelocities) and logs how often that happens. Re-timed replays cannot judge hits on cloths with such
  writes: the recorded writes carry the recording solver's response.

## Wake-up from attach calls; depenetration at refDt; game velocity writes as changes (session 4 of game feedback)
- **2.8.0 wakes a cloth from the attach and free calls** [V decompile + oracle]: Cloth slot 90 (attachVertexToGlobalPosition,
  0x100e5800) refuses a removed vertex (0x400) or one attached to a shape (flag 1 without 4, shape set); otherwise it
  stores the target, sets flags 1|2 (attached, hard), clears 4 and calls slot 75 (wakeUp) with the desc wakeUpCounter
  (+0x9c). isSleeping (slot 72) is "live counter +0xa4 <= 0", putToSleep (slot 76) zeroes it and the velocities. The same
  wake-up: attachToShape (86), attachToCollidingShapes (87), attachVertexToShape (0x100dd0b0, refused for a vertex already
  attached to a shape), freeVertex (91), detachFromShape (0x100dcf00, when it frees a vertex), the force calls.
- UE3 re-pins the fixed vertices of every cloth (95 of 986 on the Escape_p banners) with attachVertexToGlobalPosition every
  frame, after putting the cloth to sleep at creation. The original therefore wakes every banner on its first frame and
  never lets it sleep; ours kept them asleep in the cooked pose for hundreds to thousands of steps (two never woke in a
  whole run), and when a hit or a body woke one, the whole sheet dropped from its rest pose at once and tore along the
  pinned row. Oracle `repin_sleep` (sleep, then re-pin every step): ours now within 3e-6 m of the original (it stayed
  frozen before). Cost: every banner is awake, as in the original.
- **Depenetration at refDt** (ours): a particle that starts a step inside a contact is projected out and leaves with
  depth / dt, the one velocity that does not keep its size at other step lengths. The game creates banners against walls
  at thickness 0.3-0.6, so all their particles start inside: the original at 62 FPS throws them off at ~19 m/s, ours at
  the game's 230 FPS frames threw them at 69. The part of the push that takes the start point out now gets depth / refDt
  (cloth.cpp step, `depen_`). Escape_p replays against the original at 62 Hz (first steps, p99 speed): 21.4 vs 21.4,
  8.6 vs 8.5, 11.1 vs 11.0; long runs within the run-to-run spread (cloth004 until the original diverges, cloth000 through
  its bullet hits: 1047 vertices original, 1055 ours).
  Same kind, not handled: a one-step edge correction of a badly violated start state (cloth006 is created intersecting
  the level; both shred it at once, ours ~3x faster pieces).
- **Replays with other rules**: the recorded velocity writes carry the recording solver's state. The shim now records them
  as `kRecGameVel` (value set and the game's change = write minus read); `cloth_replay --dual` (and the oracle replay)
  apply the change. Older recordings: `cloth_replay old.pxcr --to-delta new.pxcr` replays the recording build's rules
  next to this build's and writes the changes (`kRecAddVel`). The game's change is small (~1-2 mm/s per frame on calm
  banners, a pull toward the wind velocity).
- **Performance** (all bit-identical; scenarios, recordings' hashes, 1 vs 7 threads): contacts and self contact split over
  the shim's pool inside each cloth (`Cloth::setParallelFor`, chunks joined in order); posed level boxes in the shape tree
  by their rotated box instead of the corner ball (39 -> ~0 needless tests per particle); swept bodies rejected 4 at a time;
  tree hits merged instead of sorting every list; self-contact grid boxes tested 4 at a time, candidates in triangle order
  (the one ordering change; no scenario uses self contact); a split rewires only the split vertex's edges (`splitEdges`)
  and the schedule and masses are rebuilt once per tear pass. Escape_p cloths, 7 threads: 0.4-1.0 ms per step, 1.6 for a
  torn pile (was 0.8-4.5 single threaded); worst tear step 115 -> 21 ms (235 splits), bullet tears 17 -> 5 ms.
