# Logged game run 1 (2026-10-09): what Mirror's Edge calls on NxFluid

Historical setup: an isolated `.wine-game/` copy of the game's Wine prefix, including the game. `tools/run_game_logged.sh` used umu-run with a locally configured Proton runner (GAMEID umu-0 [A]).
Shim in `Binaries/` (and `Binaries/PhysXLocal/`, which was never loaded). Raw log + decode:
`captures/game-run/20261009-004614/{Binaries_shim.log,decoded.txt}`; decoder `analysis/shim_log.py`.

The run was short (2461 simulate steps in total, ~40 s in the level after the menu scene), so this is the API surface
for **one level's worth of collide-only effects**. It doesn't cover SPH smoke or the Chapter 1 glass yet.

## Load path [V]
- `MirrorsEdge.exe` imports `PhysXLoader.dll` statically from `Binaries/`. `PhysXLocal/PhysXLoader.dll` (same file) is not
  loaded. The core is `Engine\v2.8.0\PhysXCore.dll` from the registry path; `cudart32_60.dll` and `PhysXDevice.dll` load too.
- Calls: `NxGetCookingLib(0x02080000)`, then `NxCreatePhysicsSDK(0x02080000, ...)`. One scene for the menu (released), one per
  level. All on one thread (the main thread), simulate + fetchResults(status 1 = RIGID_BODY_FINISHED, block = true) each frame.
  The first step is 0.333 s, the rest ~1/62 s.

## NxFluid slots the game uses [V]
Totals for the whole run (calls from the game exe, plus a few the core makes internally through the vtable):

| slot | method | calls | caller |
|---|---|---|---|
| 72 | getWorldBounds | 25066 | game, many per frame (render bounds) |
| 58 | getScene | 4482 | game |
| 5 | addParticles | 520 | game |
| 17 | updateParticles | 79 | game |
| 6 | removeAllParticles | 6 | game, right before releaseFluid |
| 76 | setName | 4 | core, inside createFluid (name = null) |
| 0, 3, 4 | ~NxFluid, getNbEmitters, getEmitters | 5 each | core, inside releaseFluid |

The game never calls any other slot: **no emitters** (createEmitter 0, no NxFluidEmitter vtable ever seen), no parameter
setters/getters, no getParticlesWriteData, no flags, no packet queries. UE3 owns spawning and lifetime itself.

- `addParticles(const NxParticleData&, appendIds=false)`: the game passes numParticlesPtr, pos, vel and **life** buffers
  (pos/vel stride 0xC0 = the UE3 particle struct, life stride 4); density/id/flag/normal are null.
- `updateParticles(const NxParticleUpdateData&)`: forceMode 0, **no force buffer**, flag + id buffers (stride 8, interleaved
  id/flag pairs), 1..n updates. So it is used only to set particle flags by id, most likely NX_FP_DELETE when UE3 kills a
  particle [A: flag values not dumped yet].
- `removeAllParticles()` then `NxScene::releaseFluid` on teardown.

## createFluid descriptors (5 fluids in the level) [V]
Common to all five: simulationMethod **NO_PARTICLE_INTERACTION**, collisionMethod STATIC|DYNAMIC, flags 0x29 =
VISUALIZATION | ENABLED | PRIORITY_MODE (**no HARDWARE**, so the SW path), restParticlesPerMeter **0.78125**, restDensity 1000,
KRM 2, collisionDistanceMultiplier 0.03125, packetSizeMultiplier 16, stiffness 20, viscosity 6, collisionResponseCoefficient 0.2,
projectionPlane (0,0,1,0), groupsMask {0x40, 0, 0x1025, 0}, collisionGroup 0, **compartment set** (non-null, the same for all
five), no emitters, null name/userData. Write buffers: particlesWriteData (pos, vel, id, collisionNormal; stride 32/32/32/12),
deletion and creation id buffers, fluidPacketData: all set.

| fluid | maxParticles | reserve | motionLimitM | damping | static rest/dynFr | dynamic rest/dynFr |
|---|---|---|---|---|---|---|
| 0 | 1200 | 200 | 0.25 | 0.05 | 0.25 / 0.05 | 0.05 / 0.02 |
| 1 | 1300 | 500 | 0.25 | 0.05 | 0.25 / 0.05 | 0.05 / 0.02 |
| 2 | 2300 | 300 | 0.25 | 0.1 | 0.2 / 0.05 | 0.5 / 0.5 |
| 3, 4 | 1600 | 200 | 0.3125 | 0.1 | 0.75 / 0.05 | 0.5 / 0.5 |

restParticlesPerMeter 0.78125 (rest spacing 1.28 m) and collision distance 0.03125 * 1.28 = 4 cm: the fluid parameters are
scaled by the game before they reach the SDK. How this relates to the 1 uu = 0.02 m assumption is open [A].

## What this means for the stand-in NxFluid
The minimum surface for this kind of content: createFluid / releaseFluid with the full NxFluidDesc (write buffers, id
buffers, packet data, compartment), addParticles with lifetime, updateParticles (flags by id), removeAllParticles,
getWorldBounds (hot: ~10 per frame, must be cheap), getScene, plus the internal teardown slots. Every other slot can forward
or stub until a later run shows otherwise. The compartment API (NxScene::createCompartment, slot >= 153?) is not hooked yet.

## Still to log (a second, longer run)
- Chapter 1 offices (glass, `Glass_big`), the helicopter (`HeliDust`, SPH), firefight debris: SPH/mixed descriptors and
  whether any of them use emitters or other slots.
- Dump the updateParticles flag values and the compartment desc (hook NxScene::createCompartment).

# Logged game run 2 (2026-10-09, Escape_p)

The initial conclusion that command-line map arguments worked was incorrect. The development runner now selects checkpoints through `TdEngine.ini`; see [test-spots.md](test-spots.md).
Log `captures/game-run/20261009-010153/` (24103 steps, ~6.5 min, clean exit; final totals: 146 fluids created and released,
addParticles 9901, updateParticles 15563, removeAllParticles 797, getScene 100822, getWorldBounds 1129396, 12 SPH / 134 collide-only).
The numbers below are from a mid-run snapshot.
- **Same slot set as run 1 and still no emitters**: addParticles 9222, updateParticles 15351, removeAllParticles 764,
  getScene 94526, getWorldBounds 1094945 (~50 per step), plus the core-internal create/release slots. 140 fluids created,
  136 released (UE3 creates and releases a fluid per effect instance).
- **12 SPH fluids** (flags 0x9 = VISUALIZATION | ENABLED: no PRIORITY_MODE, no HARDWARE): maxParticles 500, restPPM 10,
  collDistM 12, KRM 2, damping 0, motionLimitM 0.8 or 5.2, extAccel (0,0,16.2): rising smoke, UE3 is Z-up.
- 128 NO_INTERACTION fluids in 9 distinct configs (`decoded.txt`); besides the run-1 ones, flags 0x9 variants with extAccel
  z = 16, 6, -8 and restPPM 3.125.
- packetSizeMultiplier 4 or 8 on some configs (run 1 only saw 16).

# Hosted game run 1 (2026-10-09, `PXSHIM_FLUID=ours`, Escape_p, ~3800 steps)
Log `captures/game-run/20261009-015117/`. The windows break with our solver running the collide-only fluids (user
report: it stutters when the windows break, as with the original). 5 fluids, all hosted; no unsupported calls.
- Scene [V]: gravity (0, 0, -16) (UE3 Z-up, 800 uu/s^2 * 0.02), timing FIXED, maxTimestep 0.0161298, **maxIter 1**:
  one substep per simulate, so the fixed-substep mirroring is moot for this game.
- Static geometry [V]: 1153 actors at load (213 dynamic, 277 after the glass broke: the shards are rigid bodies too),
  2076 static shapes: 1403 boxes, 671 convexes, 2 triangle meshes; no planes, spheres, capsules, heightfields.
  Every shape passes the fluids' group/groupsMask filter. A full rescan of the static cache takes 0-8 ms.
- updateParticles flag = 1 (NX_FP_DELETE) by id [V].
- New fluid configs: flags 0x9, max 4000, reserve 0, KRM 2, CDM 0.09375, MLM 0.078125 (an ambient fluid, created at load).
- No frame timing yet: the shim now logs core vs hosted cost every 300 frames ("frame cost").

# Hosted vs original, same spot (2026-10-09, Escape_p, logs 20261009-015529 ours / 20261009-015649 original)
User: debris and glass look the same in both. "frame cost" lines (300-frame windows):
- Window where the glass breaks: **original core avg 19.5 ms, max 94.8 ms**; with ours the core is 0.68 ms avg
  (max 2.5) and the hosted fluids 1.14 ms avg (max 10.0 ms) for 5 fluids, 1119 particles, 165 nearby shapes.
- Quiet windows: core ~0.7 ms in both; our one ambient fluid (40 particles, 138 nearby shapes) 0.5 ms.
- So the original's stutter is the core's SW fluid. Our 10 ms spike is unexplained [A: fluid creation or the static
  cache rescan, which runs whenever the actor count changes, i.e. every time shards spawn].
