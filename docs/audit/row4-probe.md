# Valid-bounds probe: NX_CLF_VALIDBOUNDS (flag 0x8000)

Historical oracle measurements used to implement cloth valid bounds. Sources: [GAPS.md](GAPS.md), the locally supplied NxClothDesc header, and deterministic probes in `scenarios/cloth/`. The native and shim paths now apply bounds; the initial parser-only stage is not a current limitation.

## Reproduction

After preparing the private oracle dependencies described in [CONTRIBUTING.md](../../CONTRIBUTING.md):

```sh
bash harness/build.sh
tools/run_cloth_oracle.sh scenarios/cloth/vb_fall.txt scenarios/cloth/vb_swing.txt
tools/run_cloth_ours.sh scenarios/cloth/vb_fall.txt scenarios/cloth/vb_swing.txt
```

Output goes to the ignored `captures/cloth/<name>/` directories. The native runner uses the oracle's `cooked.bin` when present. One-off analysis helpers and control captures from the original investigation are not distributed.

## Scenarios

- `vb_fall`: vertical 11x11 sheet, 0.1 m spacing, top-left at (-0.5, 2.0, 0), xy plane, free fall, no shapes, 180 steps. `flag validbounds 1`, `bounds -10 -0.2 -10 10 10 10`. The bottom row starts at y=1; analytic floor crossing occurs at t=0.494 s, around step 29.6.
- `vb_swing`: vertical 11x11 sheet, top-left at (-0.5, 1.5, 0), top row pinned, 180 steps, `cloth.externalAcceleration 12 0 0`, `flag validbounds 1`, `bounds -10 -10 -10 0.6 10 10`. Only the x maximum acts.

## Oracle findings

[V] means measured in the reference output; [A] means inferred.

1. [V] The flag gates removal. The no-flag control keeps all 121 particles moving to the end with the same bounds.
2. [V] A particle detected outside after step s is frozen from step s+1: zero velocity and unchanged position. The index buffer changes on the detection step, not the freeze step.
3. [V] In `vb_fall`, the bottom row is detected first at step 29 (y=-0.27). Exactly 11 particles per step are detected through step 38, then frozen at steps 30–39. The top row never leaves the box.
4. [V] The top row nevertheless freezes at step 39, after every triangle has a removed corner. At step 38 its y=-0.126 and vy=-6.4 m/s; at step 39 all 121 velocities are zero and the cloth sleeps.
5. [V] In `vb_swing`, six right-edge particles are detected at steps 7 (two), 8, 9, 11 and 14, then frozen one step later. The pinned top row stays inside; the rest keeps swinging and reaches a normal sleeping state at step 147.
6. [V] A triangle with a removed corner becomes `(a,a,a)` in the output buffer, with `a` a nonremoved vertex in every observed case. The triangle count remains 200. Whether the original chooses the first index or first surviving index is unresolved [A]. `vb_fall` collapses 20 triangles per row; `vb_swing` collapses 3, 3, 1, 3 and 1 triangles at its detection steps.
7. [V] Vertices left in no live triangle freeze even inside the bounds. A control retaining one column freezes those 11 vertices after the first detection; retaining two columns leaves 20 live triangles and the remaining 22 particles keep falling. The native implementation models the orphan case as removal; output alone cannot distinguish removal from sleep [A].
8. [V] Pinned particles remain unaffected while inside the box.
9. [V] `cloth.windAcceleration 12 0 0` produces no sideways motion for this xy sheet: wind acts normal to its surface. `vb_swing` therefore uses external acceleration instead.
10. [A] Enabling valid bounds without specifying a box should remove everything: the descriptor default is an empty box. This comparison always specified bounds and did not test that case.
11. [V] All reference runs, including controls, reported two SDK warnings. The harness counted them silently, so this investigation did not identify their source.

## Implemented comparison

The completed native and shim paths were compared with the oracle using `REF_HZ=0`: all 121 particles freeze at the same step; index buffers are identical at every step; maximum position difference is 5.7e-7 m and velocity difference 2.1e-5 m/s [V]. The implementation carries a sticky removed mask across shim rebuilds, disables constraints involving removed particles, collapses their output triangles and excludes them from world bounds.

Unrelated-scenario regression results are recorded in [row4-regress.md](row4-regress.md). These are historical measurements, not an automatically rerun release test.
