# Shatter

A modern GPU-agnostic, CPU optimized replacement for the PhysX effects in **Mirror's Edge** (2009): glass shards, debris, smoke, steam and
cloth.

Mirror's Edge was built for GPU PhysX, which only ran on NVIDIA cards. Everyone else got the CPU fallback, a single-threaded path using x87 math ([RealWorldTech analysis via HotHardware][kanter]), so the enhanced effects stutter badly, especially when glass breaks. NVIDIA dropped 32-bit GPU PhysX support on RTX 50-series cards, but driver 591.44 restored it through per-game profiles for nine titles, including Mirror's Edge ([PCWorld][restore]). It remains NVIDIA-only, and future driver support is not guaranteed.

Shatter replaces those effects with an optimized, multithreaded CPU implementation that works with GPUs from any vendor. Its optimizations target x86 CPUs; performance on other architectures, such as ARM64, is unverified.

<p align="center">
  <img src="assets/shatter-demo.avif" alt="Shatter gameplay demonstration" width="100%">
</p>

## Install

### The easy way (recommended)

1. Download [`ShatterSetup-1.0.6.exe`](https://github.com/Arecsu/shatter/releases/download/v1.0.6/ShatterSetup-1.0.6.exe) from the [latest release](https://github.com/Arecsu/shatter/releases/latest).
2. Run it and follow the instructions.
3. Optionally, after installation, use **Game settings** to raise the 62 fps cap, skip the intro movie or enable the slide-steering fix.
4. Make sure PhysX is turned on in the game's graphics options.

That's it. There is no special switch for AMD, Intel, Nvidia, etc. They all will use this new improved and optimized CPU paths automatically.

On Linux with Wine/Proton, run the installer in the same prefix as the game so it finds the correct `Documents` directory when changing game settings.

---

### The hard way

1. Download [`Shatter-1.0.6.zip`](https://github.com/Arecsu/shatter/releases/download/v1.0.6/Shatter-1.0.6.zip) from the [latest release](https://github.com/Arecsu/shatter/releases/latest) and extract it.
2. Open the game's `Binaries/` folder (where `MirrorsEdge.exe` is).
3. Rename the original `PhysXLoader.dll` to `PhysXLoader.dll.original`. EA app version: there is none here, skip this step.
4. Copy Shatter's `PhysXLoader.dll` into `Binaries/`.
5. Turn PhysX on in the game's graphics options.

That's all. Shatter finds the PhysX runtime the same way the original loader does, keeps rigid-body physics on it,
and runs the fluids and cloth on the CPU, with or without an NVIDIA card and PhysX driver. The installer
(`ShatterSetup.exe`) does the same steps for you, with a backup.

Settings are optional: a `shatter.ini` next to the DLL can disable Shatter's fluids or cloth (to compare with the original PhysX), set the number of threads or enable a debug log. See `package/shatter.ini` for the keys. Environment variables
(`PXSHIM_*`) override the ini, for developers.

To uninstall, rename `PhysXLoader.dll.original` back to `PhysXLoader.dll`, or repair the game in Steam, GOG or the EA
app. EA app version: just delete Shatter's `PhysXLoader.dll`.

### Features

- Works with GPUs from any vendor, without GPU PhysX support.
- Faster than the original CPU fallback, using SIMD and multithreaded simulation.
- Cloth updates every rendered frame, instead of the stock GPU path's [50 Hz rate][cloth-rate].
- Per-frame fluid updates, including during reaction-time slow motion.
- Consistent cloth stiffness, damping and surface friction across frame rates.
- Improved cloth self-contact, untangling and mesh collisions; debris no longer hovers above meshes.
- Optional fix for slide steering at high frame rates.

### High frame rates

Mirror's Edge caps itself at 62 fps by default.

`ShatterSetup.exe` can change the cap through its **Game settings** button, available after installation and when you run it again. It backs up the settings file first and offers to restore the old values when you uninstall.

For a manual installation, edit `Documents/EA Games/Mirror's Edge/TdGame/Config/TdEngine.ini`: under `[Engine.GameEngine]`, set `MaxSmoothedFrameRate` to your target (for example, 144 or 240). Leave `bSmoothFrameRate=TRUE`, since disabling smoothing is reported to cause bugs. After raising the cap, you can enable VSync in the game's graphics options to limit the frame rate to your display's refresh rate; restart the game to apply the setting. If the game resets the file, make it read-only.

At high frame rates, the game also loses steering on steep slides, such as the building slide in chapter 1. It steers only on frames when the player touches the slope; at higher frame rates, the player briefly hops above it between contacts. Shatter can fix this, but the change may affect subtle movement behavior or speedrun tricks. For that reason, it is **off by default**: tick "Fix slide steering above 62 fps" in ShatterSetup's **Game settings**, or add `slide_steering_fix = on` to `shatter.ini`.

The fix has been checked against the GOG 1.0.1.0 executable. Other builds, including Steam, are unverified; the fix is skipped if the executable does not match the expected binary checks.

You don't need a separate "PhysX FPS" setting. Shatter steps cloth and fluids once per rendered frame, keeping effects smooth at high frame rates and during reaction-time slow motion without an additional cloth-rate mod.

In the original CPU path, cloth becomes stiffer and more heavily damped as the frame rate rises because stiffness, damping and friction are applied once per step. Debris sliding on a surface also slows down faster. Shatter rescales these coefficients to the step size, keeping effects consistent at 240 fps and 62 fps:

| Measured from 50 to 300 Hz | Original PhysX | Shatter |
|---|---|---|
| Hanging cloth stretch (rigid) | 11% → 0.3% | 7.0–7.7% |
| Soft cloth stretch (stiffness 0.2) | 328% → 12% | 280–298% |
| Debris slide distance on a floor | 0.29 → 0.05 m | 0.23–0.25 m |

Cost scales with frame rate (one step per frame), but cloth uses fewer solver passes per step at high rates, so its
cost per second stays about flat.

<p align="center">
  <img src="assets/the-way.webp" alt="The Way It's Meant To Be Played" width="50%">
</p>

## How it works

Shatter replaces `PhysXLoader.dll` and discovers the game's PhysX runtime itself. It patches the PhysX scene's vtables so that fluid and cloth objects are created by Shatter, and passes everything else to the original PhysX core. Unsupported core versions or unexpected vtables keep the original simulation path.

- **Fluids** (`NxFluid`): collide-only particles (glass, debris) and SPH fluids (smoke, steam, dust). They collide
  with planes, boxes, spheres, capsules, convex and triangle meshes, heightfields and moving rigid bodies,
  including two-way pushes. Drains, triggers, collision groups, and the game's particle updates and forces are
  supported.
- **Cloth** (`NxCloth`): Gauss-Seidel solver using the edge order from the game's pre-cooked meshes, with bending,
  damping, wind, tearing (bullets tear it, as in the original), attachments to shapes, moving colliders and raycasts.
  Cloth collides with the level's planes, boxes, spheres, capsules, convex and triangle meshes and heightfields.
- **Threading:**
  - Fluids and cloth step on background threads between the game's `simulate` and `fetchResults`, while the game
    builds its frame.
  - Work inside a step is split across a worker pool: about half the hardware threads minus one, at most 8.
    `PXSHIM_THREADS=n` overrides it.
  - The game thread only does the reads it must do from the original core.
- **SIMD:** the SPH neighbour search, the cloth constraint solve and the cloth self-contact search use SSE2, 4
  particles, edges or triangles at a time, without FMA. Any x86 CPU from the last 20 years runs it, and results are
  the same as the scalar code. Cloth collision looks up only the level shapes near each particle (a box tree), so a
  cloth among hundreds of level shapes costs about a quarter of what testing them all did.
- **Accuracy:**
  - Behavior was initially reverse-engineered from the original binaries and checked against the real PhysX core with deterministic probes. Chaotic fluids are compared statistically. Shatter deliberately differs where it fixes bugs, adds features or rescales solver coefficients for the frame rate.
  - Where the original has obvious artifacts, Shatter fixes them instead of copying them. For example, particles
    landing on a mesh no longer hover about 4 cm above it.
  - Cloth self-contact (not in the original): torn pieces try to stack instead of flickering in the same plane, and cloth folding in the wind is less likely to pass through itself. Difficult cases can still intersect. `PXSHIM_CLOTH_SELF=0` turns it off.
  - Cloth that ends up through itself anyway (a bullet throws a flap through the banner) untangles: the parts that
    went through are pulled back and hooked borders slide apart, where the original stays tangled for good.
  - Cloth no longer slips through triangle meshes at the seams between triangles or around corners, which the
    original does (up to 48 of 441 vertices inside a box made of triangles, in a test against the real core).
  - Frame-rate independence: cloth stiffness, damping and friction, and fluid surface friction, keep a consistent feel across frame rates rather than changing with each step's duration. See [High frame rates](#high-frame-rates).

### What it does not replace

**Rigid bodies** (props, ragdolls, glass fragments, joints and scene queries) stay on the original PhysX core shipped with the game. Shatter calls that core rather than reimplementing it:

- **Already fast:** rigid bodies always ran on the CPU. In measured game runs, the original core handles them in under 1 ms per frame, leaving little reason to replace it.
- **Huge surface:** most of the PhysX API is rigid-body work: shapes, joints, materials, contact reports, triggers,
  queries, continuous collision and cooked meshes.
- **Affects gameplay:** small differences would change how ragdolls fall, how props block you, or movement details, for no benefit.

Also left alone: PhysX cooking (the game ships pre-cooked data), and soft bodies, force fields and fluid emitters (the game doesn't use them).

## Performance

Time per physics step: the original PhysX 2.8 CPU path (the game as shipped) vs Shatter. Same scenes, same machine
(Ryzen 7 3700X), 32-bit builds under Wine. Each step also includes about 0.4 ms of fixed engine cost, so the
effect-only gains are larger than shown.

| Effect | Scene | Original PhysX | Shatter, 1 thread | Shatter, 7 threads | Faster |
|---|---|---|---|---|---|
| Glass shards | 1300 particles | 4.97 ms | 0.45 ms | 0.50 ms | **~11x** |
| Debris on level geometry | 2000 particles on a heightfield | 3.37 ms | 0.50 ms | 0.58 ms | **~6.7x** |
| Debris bounce | 576 particles | 1.07 ms | 0.45 ms | 0.43 ms | **~2.5x** |
| Smoke (SPH) | 2744 particles | 4.47 ms | 1.81 ms | 0.73 ms | **~6.1x** |
| Steam (SPH) | 1536 particles | 2.70 ms | 1.00 ms | 0.61 ms | **~4.4x** |
| Heli smoke (game config) | 384 particles | 1.06 ms | 0.44 ms | 0.45 ms | **~2.4x** |
| Cloth | 441 vertices | 0.91 ms | 0.43 ms | 0.44 ms | **~2.1x** |
| Cloth, large | 1681 vertices, 12 colliders | 2.58 ms | 0.48 ms | 0.48 ms | **~5.4x** |
| Cloth, self-collision | 441 vertices folding | 0.93 ms | 0.44 ms | 0.45 ms | **~2.1x** |
| Cloth, tearing | 441 vertices | 0.76 ms | 0.59 ms | 0.58 ms | **~1.3x** |

In game (Escape, the glass-break scene): the original PhysX averaged **19.5 ms per frame, with spikes of 95 ms**. Shatter
started at 1.8 ms on its first version, and now runs on background threads, so the game thread spends about 0.1 ms on
it. The game holds a steady 60 fps through the break.

Small scenes don't gain from more threads; they run inline. Results are bit-identical for any thread count.

## Development

The DLL and installer are 32-bit (i686), MSVC ABI, built with clang-cl, lld-link and xwin. Building Shatter itself does not require NVIDIA SDK headers or vendor DLLs; oracle testing does.

Cloth regression cases in `tests/cloth_cases/` are stored with Git LFS (`git lfs install` before cloning, or `git lfs pull`). See [CONTRIBUTING.md](CONTRIBUTING.md) for prerequisites, builds and tests, [docs/README.md](docs/README.md) for research notes, and [docs/release.md](docs/release.md) for publication checks.

## License

MIT (see [LICENSE](LICENSE)). Release packages contain Shatter's implementation, not NVIDIA/EA binaries, SDK headers or game assets. Shatter loads the PhysX runtime already installed with the game. Shatter is not affiliated with NVIDIA or EA; PhysX and Mirror's Edge are trademarks of their owners.

[kanter]: https://hothardware.com/articles/NVIDIA-Sheds-Light-On-Lack-Of-PhysX-CPU-Optimizations/
[restore]: https://www.pcworld.com/article/3002325/surprise-nvidia-brings-physx-back-to-geforce-rtx-50-gpus-kind-of.html
[cloth-rate]: https://www.youtube.com/watch?v=aTLTju_t_sM
