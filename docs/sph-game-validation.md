# SPH and mixed-mode fluids: game parameters, oracle vs hosted ours-all

Historical comparison from 2026-10-10, before all-fluid hosting became the release default. These measurements describe the tested variants, not current release readiness. Age latency (`PXF_AGE_LATENCY=2`, FluidParams::newParticleShapeLatency)
removes the step-0 position mismatch in all 14 configs and most of the failing metrics; 3 configs still differ and 8 differ
only in `contact_frac`. Solver change in `solver/fluid/fluid.cpp` (2026-10-10). Evidence below.

## Inputs

- Game descriptors: `captures/game-run/20261009-010153/Binaries_shim.log` and `.../20261009-233837/Binaries_shim.log`
  (`analysis/shim_log.py`). They contain **only the Heli pair** (Heli_Smoke, Heli_Smoke_Thin, 6 createFluid
  each, 12 SPH descs). No MIXED desc appears in either log.
- All 14 SPH/mixed configs come from `captures/assets/physx_inventory.json` (UE3 props).
  The Heli descriptors match the inventory conversion exactly [V]:
  `restPPM = 1/(RPD*0.02)`, `motionLimitMultiplier = MaxMotion/RPD`, `collisionDistanceMultiplier = CollisionDistance/RPD`
  (engine default 10 uu), `extAccel = uu*0.02`, unset props take engine defaults (restitution static 0.5,
  dynamic friction static 0.05, dynamic friction dynamic 0.9 when set; restitution dynamic 0.5 default).
- Scenarios: `scenarios/fluid/game_sph/*.txt` (14 files, generated from the inventory; the generator is not committed).

## Known gaps in the test, not in the solver

- **No emission over time.** The scenario format has no emitter or `addParticles` schedule. Each scenario is a
  single initial block (the steady-state cloud). The game's continuous emission and per-particle lifetime are untested.
- **Gravity** of the game scene is not in the logs [A: -9.81 down in our frame].
- **Static friction** (`statFr`): the game descriptor has 4 floats per shape class (rest, dynFr, statFr, attr) with
  statFr = 0.05 (static shapes) and 0.5 (dynamic shapes). `scenario.h` says 2.8.0 has no static friction and ignores it.
  Both sides are set to 0 here, so the comparison does not cover it. The static-friction interpretation remains unverified by this comparison.
- **WaterFall** has bStatic/bDynamicCollision = False. The scenario has no shapes, so it is a free fall.
- **Mixed Dustmotes** and GroundFog run with disableGravity and ~zero external acceleration, so their particles
  barely move. Their passes are weak evidence.

## Method

- Oracle: `tools/run_fluid_oracle.sh`, hosted: `PXSHIM_FLUID=ours-all SHIM_TAG=ours tools/run_fluid_shim.sh`.
- Ensemble: 6 seeds per config, `jitter 1e-5 <seed>`, variants in `captures/fluid/_variants/<name>.j<s>.txt`.
  The oracle is bit-identical across repeats (checked: `pos_err_max 0`), so jitter is needed for a spread.
- `analysis/fluid_metrics.py` with K=3, tol 5%. Reports in `captures/fluid/reports-sph/`.
- Cost = harness `sim_seconds_total` per 240-step run, mean of 6 seeds.

## Results

Native runs: 14 configs x 6 jitter seeds, `analysis/fluid_metrics.py`, K=3, tolerance 5%. One-off ensemble helpers are not distributed.
- **HEAD** = solver before the band fix (`d0 <= 0` branch present, no age latency).
- **lat0** = band fix, default `newParticleShapeLatency = 0` (collide from the first step).
- **lat2** = band fix with `PXF_AGE_LATENCY=2` (analytic shapes skip a particle's first 2 steps).
- Verdict uses lat2: "matches" = no failing metric; "minor" = only `contact_frac` fails; "differs" = any other failure.

| config (scenario) | particles | cdm | step-0 max pos err (m) HEAD / lat0 / lat2 | failing metrics (of 20) HEAD / lat0 / lat2 | verdict (lat2) |
|---|---|---|---|---|---|
| smoke_grenade (SmokeGrenade) | 800 | 2.0 | 1.2e-7 / 1.2e-7 / 1.2e-7 | 1 / 1 / 1 | matches (minor: contact_frac) |
| sp02_waterslide_smoke | 800 | 2.0 | 6.8e-8 / 6.8e-8 / 6.8e-8 | 1 / 1 / 1 | matches (minor: contact_frac) |
| sp09shaft_steamsmokeground | 1960 | 2.0 | 1.7e-4 / 1.7e-4 / 1.3e-7 | 0 / 0 / 0 | matches |
| steamsmokeground | 1536 | 2.0 | 0.080 / 0.080 / 1.0e-7 | 3 / 3 / 1 | matches (minor: contact_frac) |
| waterfall (free fall) | 512 | 3.0 | 1.2e-7 / 1.2e-7 / 1.2e-7 | 0 / 0 / 0 | matches (trivial: no shapes) |
| dustmotes (mixed) | 1536 | 0.67 | 3.6e-7 / 3.6e-7 / 3.6e-7 | 0 / 0 / 0 | matches (trivial: ~0 motion) |
| sp09_coorexplo_smoke | 800 | 3.33 | 0.098 / 0.098 / 1.3e-7 | 11 / 11 / 1 | matches (minor: contact_frac) |
| groundfog_high_movement (mixed) | 1536 | 1.2 | 0.041 / 0.041 / 4.8e-7 | 4 / 2 / 1 | matches (minor: contact_frac) |
| flying_trash_faithpush | 1152 | 10 | 0.040 / 0.040 / 1.2e-7 | 6 / 6 / 1 | matches (minor: contact_frac) |
| celeste_smoke | 384 | 12 | 0.039 / 0.039 / 6.7e-8 | 8 / 8 / 1 | matches (minor: contact_frac) |
| ventsmoke | 384 | 12 | 0.019 / 0.019 / 6.8e-8 | 6 / 6 / 1 | matches (minor: contact_frac) |
| ventsmokerb | 384 | 12 | 0.099 / 0.099 / 6.8e-8 | 14 / 14 / 9 | differs (centroid_x, p01_x/z, speed, density, dist_pos_x, contact_frac) |
| heli_smoke (game desc) | 384 | 12 | 0.52 / 0.52 / 6.8e-8 | 12 / 12 / 10 | differs (centroid_x, p01_x/z, p99_z, speed, dist_pos_x, dist_speed, contact_frac) |
| heli_smoke_thin (game desc) | 384 | 12 | 0.079 / 0.079 / 6.8e-8 | 15 / 15 / 10 | differs (centroid_x/y, p01, p99_z, speed, dist_pos_x/z, contact_frac) |

Summary (strict count, any failing metric): HEAD 11 of 14 (9 with a failure other than contact_frac); lat0 11 of 14
(groundfog 4 -> 2, otherwise identical to HEAD); lat2 3 of 14 differ, 8 fail only `contact_frac`, 3 pass with no failure. The earlier hosted table in this file counted 12 fails; the native HEAD
run here gives 9 non-minor differs, so the hosted and native numbers are not identical (not reconciled).

Cost: not re-measured in this pass.

## Suspected bugs (status)

1. **Step-0 mismatch with large collision distance: resolved by age latency [V].** The step-0 error of every config
   with cdm >= 2 drops to ~1e-7 with lat2 (heli 0.52 -> 6.8e-8, celeste 0.039 -> 6.7e-8). The removed `d0 <= 0` branch
   does not change step 0 (lat0 = HEAD at step 0): the step-0 pushes come from particles within cd of a shape, which the
   original skips for its first 2 steps. Probe basis: `probe_band` and `probe_late` (docs/re-notes-fluid.md, band section).
2. **Remaining failures in heli_smoke, heli_smoke_thin, ventsmokerb [open].** After lat2 the late-run centroid_x and p01_x/z
   (step 239) still differ by up to 5.5x and the speed quantiles by up to 7.5x. Not isolated. Candidate: the band
   residuals below (particles whose start is on or inside a shape are pushed along a different path than the oracle).
3. **contact_frac [open, minor].** Fails in 11 configs at lat2 with max ratio 6-13 at mid-run (step 60-90). At step 0 the
   fraction now matches. Not isolated.
4. **Static friction** (`statFr`): not tested (both sides 0). Unchanged from the previous pass.

## Band residuals still open (from probe_band, lat2) [V]

Native lat2 matches the oracle exactly on 297 of 320 band probes (lat0: 118). The 23 failures are:
- 9 triangle-mesh probes (mesh path not changed here; no age latency in the original for mesh, so these are a different artifact).
- 7 started-on/inside-with-tangential-velocity probes (`par`, box A/B and plane A/B at h00/hm05; vn = 0, vt = 1 m/s). The
  oracle does not advance the tangential position in the push step: x stays at the start value for one step. Our
  end-point push advances it. A start-based push (old + n(cd - d0)) for plane/sphere/capsule with d0 <= 0 and s1 <= cd
  fixes plane A par at h00 and hm05 and plane A h00 grav, but flips plane A h05 and hm05 grav (boundary cases). Net 298 of 320
  against 297 for lat2. Experiment only, not adopted in the solver. Box is not covered by it.
- 6 plane B hm05 probes (s0 = -0.5 cd, cd 1.2 m): the push is motion-limited (0.25 cd per step) and the oracle has one
  step without a push (step 5) that we do not reproduce. Same mechanism as the documented pause.
- 1 plane A h00 grav: 6.8e-4 error from step 5, a boundary case (end point exactly at cd after a bounce, rounding decides
  whether the response fires). The start-based experiment fixes it, which shows the same sensitivity.

## Renders (oracle vs ours, side and top views at steps 0, 60, 120, 239)

- Worst: `captures/fluid/heli_smoke_thin/compare.png`
- Best: `captures/fluid/sp09shaft_steamsmokeground/compare.png`

## Limits of this comparison

This ensemble did not validate continuous emission, lifetime, or an in-game all-fluid run. Those are separate checks; see the current [development instructions](../CONTRIBUTING.md). Late-run spread and speed differences in heli/ventsmokerb and the contact-fraction metric were not isolated here.
