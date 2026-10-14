# Valid-bounds regression comparison (historical)

The valid-bounds change was checked against the preceding shim build using the same oracle executables. Expected result: no changes in scenarios that do not enable bounds. This report records the original experiment, not the current test-suite size.

## Method

- Run BASE and NEW sequentially through `tools/run_cloth_shim.sh` and `tools/run_fluid_shim.sh`, with separate build/run destinations and output tags. Do not run concurrently in a shared Wine prefix; `wineserver -k` acts on the whole prefix.
- Cloth: 59 scenarios, excluding the new `vb_fall` and `vb_swing` probes, with `PXSHIM_CLOTH=ours`.
- Fluid: 42 scenarios, with `PXSHIM_FLUID=ours-all`: 11 top-level, 14 game-SPH, four API, one rotated-box shim scene and 12 dynamic-body probes. The geometry-only census was excluded.
- Compare frame dumps byte-for-byte except wall-clock fields: PXCL frame time at offset +8; PXFD step time at +4. Compare headers, index buffers and trailing bytes exactly. Compare fluid `bodies.bin` without masking; ignore only `sim_seconds_*` in JSON summaries.
- BASE repeated on `attach_shape_sphere` differed only in the frame-clock doubles (391 bytes), validating the mask. Both builds used the default `PXSHIM_REF_HZ`.

Raw captures and one-off comparison scripts are local-only and are not distributed.

## Results [V]

| Path | Attempted | Compared | Identical | Differences |
|---|---:|---:|---:|---:|
| Cloth | 59 | 59 | 59 | 0 |
| Fluid | 42 | 39 | 39 | 0 |

The three noncomparable fluid scenes were `bench_game_glass`, `bench_mesh_glass` and `bench_mesh_sph`: they used `quadgrid`, which the oracle parser did not support at the time. Both builds failed before any fluid reached the shim, with the same error. They were excluded, not counted as passes.

BASE and NEW DLLs were different, and each run's log confirmed its intended DLL and hosted solver path. None of the compared cloth scenes used `bounds` or `validbounds`; the enabled-feature check is documented separately in [row4-probe.md](row4-probe.md). No original game files were modified.
