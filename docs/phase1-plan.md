# Initial tracing design (historical)

This plan preceded the working fluid and cloth solvers. It is retained to explain the tracing approach, not as a task list. Current build and test instructions are in [CONTRIBUTING.md](../CONTRIBUTING.md).

## ABI strategy

Use the 2.8.1 runtime with matched headers for deterministic oracle probes. Check the game's 2.8.0 descriptor layouts and virtual slots independently in the shim. The compatibility limits are documented in [abi-2.8.0-vs-2.8.1.md](abi-2.8.0-vs-2.8.1.md).

## Loader and tracing

Export the original loader's nine entry points and intercept scene, fluid and cloth APIs through shadow vtables. Unchanged slots continue to call the original runtime. Trace creation descriptors, simulation timing, particle updates and cloth API writes when diagnostics are enabled.

The original design forwarded to a renamed loader. The shipped implementation now discovers the runtime itself; a renamed loader is only a development override. Fluid traces, slow-step dumps and cloth recordings are implemented in `shim/` and read by tools in `analysis/` and `solver/`.

## Reference scenarios

Use authored, deterministic scenarios in `scenarios/fluid/` and `scenarios/cloth/` to compare Shatter with the oracle. Repeat reference runs when establishing numerical tolerances, and compare chaotic SPH effects statistically. Keep cooked mesh streams and game recordings local; they are not release assets.

## Game validation

Run only from an isolated copy of the game and its Wine prefix, with explicit permission to launch. Check glass breaks, smoke and hanging or tearable cloth. [test-spots.md](test-spots.md) documents checkpoint selection for the development runner. GPU-reference runs remain a separate, unverified experiment.
