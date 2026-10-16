# Shatter: PhysX 2.8 compatibility layer for Mirror's Edge

Shatter replaces fluid and cloth effects with a multithreaded SIMD CPU implementation behind `PhysXLoader.dll`. Rigid bodies and cooking stay on the original runtime.

## Project rules

- The game install and its original Wine prefix are read-only. Experiments belong in repository-local throwaway prefixes. Ask before launching the game.
- If present, read `PERSONAL.md` and `docs/HANDOFF.md` for local setup and session state. Both are gitignored; never copy machine-specific paths or personal data into tracked files.
- Never commit or redistribute vendor binaries, SDK headers, extracted assets, recordings or decompiler output. Keep them in the ignored locations listed in `.gitignore`. Research notes describe behavior in original words; do not paste decompiled code.
- One exception: shrunk cloth test cases in `tests/cloth_cases/` (`.pxcr.zst` + `.json`, written by `tools/cloth_case.py`). They hold the cloth's rest mesh, the primitive shapes it touches and the calls of a short window, never a level triangle mesh or a machine path; the tool and `tools/release_audit.py` enforce that. A case that needs a level mesh stays in the ignored `captures/cases/`. Full recordings stay private as the cases' source. The `.pxcr.zst` files are in Git LFS (`.gitattributes`); `git lfs pull` fetches them. A rebase changes only a case's `.json`; recut (new binaries in LFS storage) only when a state layout change or a solver change requires it, and in batches.
- The DLL, installer and oracle harnesses are 32-bit i686 with the MSVC ABI: clang-cl + lld-link + xwin. Do not use MinGW GCC for the C++ interface boundary. Native solver-only tools use clang++.
- For visual checks, render a PNG and ask the user to assess it. Do not claim visual correctness from your own image reading.
- Mark research claims [V] verified or [A] assumed. Ask before installing large dependencies.

## ABI constraints

The game requests SDK `0x02080000`. The examined runtime is PhysXCore 2.8.0.45; supported runtime checks live in `shim/loader_core.cpp`. Public 2.8.1 headers are not a drop-in ABI for 2.8.0 (`NxScene`: 155 vs 157 slots; SDK descriptor: seven vs five dwords). Oracle runs use 2.8.1 with matched headers.

## References

- [README.md](README.md): player-facing behavior and compatibility.
- [CONTRIBUTING.md](CONTRIBUTING.md): builds, tests and contribution rules.
- [docs/README.md](docs/README.md): research index and historical-document boundaries.
- [docs/release.md](docs/release.md): publication checks.
- `shim/`: loader, ABI hooks and game integration; `solver/`: CPU implementations; `harness/`: oracle drivers; `analysis/`: comparison tools.
