# Documentation index

## Current guidance

- [Project README](../README.md): installation, behavior and compatibility.
- [Development](../CONTRIBUTING.md): prerequisites, builds, oracle tests and data boundaries.
- [Release checklist](release.md): privacy, source and binary publication checks.
- [Settings template](../package/shatter.ini): player-facing configuration keys and defaults.
- [In-game test spots](test-spots.md): development runner checkpoint selection.

Current behavior is defined by the source and verified tests. The settings template and `shim/version.h` define package defaults and versioning; research reports are not substitutes for them.

## Reverse-engineering references

These notes retain measurements, hypotheses and rejected approaches. They developed over several iterations: old status sections, line numbers, scenario counts and follow-up lists may be historical. [V] means verified by the stated source or experiment, not revalidated on every later release; [A] means assumed or inferred. Referenced captures, cooked streams, extracted properties and decompiler outputs are intentionally not distributed.

- [Runtime and toolchain findings](../NOTES.md).
- [2.8.0 vs 2.8.1 ABI](abi-2.8.0-vs-2.8.1.md).
- [Fluid algorithm](re-notes-fluid.md), [API semantics](re-notes-fluid-api.md), [mesh collision](re-notes-fluid-mesh.md), [drains and filtering](re-notes-fluid-drain.md).
- [Cloth algorithm and validation](re-notes-cloth.md).
- [SPH game-configuration measurements](sph-game-validation.md).
- [Early game traces](shim-game-run.md): recorded API usage and initial timings, not current installation instructions.
- [Content inventory summary](phase1-assets.md) and [hardware-feature survey](hw-features.md): observations with unresolved count/layout questions; see the detailed audits below.
- [Initial tracing design](phase1-plan.md): historical design rationale, not an implementation roadmap.

## Detailed audit snapshots

The reports in `audit/` record a compatibility investigation, not a live issue tracker. Their W/M row labels refer to [the gap table](audit/GAPS.md). Later fixes may supersede a report's description of the shim. Static content scans do not prove runtime feature use or absence.

- Binary paths and initialization: [setup](audit/exe-setup.md), [scenes](audit/exe-scene.md), [fluid calls](audit/exe-fluid.md), [cloth creation](audit/exe-cloth-create.md), [cloth runtime](audit/exe-cloth-runtime.md), [remaining static questions](audit/exe-open.md).
- Implementation coverage at audit time: [scenes](audit/shim-scene.md), [fluids](audit/shim-fluid.md), [cloth](audit/shim-cloth.md).
- Content scans: [non-map packages](audit/content-pkgs.md), maps [part 0](audit/content-maps0.md), [part 1](audit/content-maps1.md), [part 2](audit/content-maps2.md), and [UE3 property mapping](audit/ue3-prop-map.md).
- Valid-bounds measurements: [probe](audit/row4-probe.md), [regression check](audit/row4-regress.md).

`inventory.json` and `abi/` contain binary identity metadata and virtual-slot counts, not executable code or SDK headers. `tools/pe_inventory.py` and `tools/rtti_vtables.py` read locally supplied binaries to generate such reports; sanitize paths before publishing generated output.
