# Development

## Boundaries

Keep the original game install and its Wine prefix read-only. Run game experiments only from an isolated copy, and ask before launching the game in an assisted session. Local paths belong in the ignored `.local.env` or `PERSONAL.md`, not in scripts or documentation.

Do not contribute NVIDIA/EA binaries, SDK headers, extracted game assets, cooked streams, recordings, or decompiled source. `vendor-dlls/`, `third_party/`, `.toolchain/`, `captures/` and reverse-engineering outputs are ignored. Describe observed behavior in original words and distinguish verified observations [V] from assumptions [A]. Ignore rules are not a substitute for checking what Git actually tracks.

## Build the DLL and installer

The build scripts run on Linux and produce 32-bit Windows binaries with the MSVC ABI. Required tools: Bash, clang-cl/clang++, lld-link, LLVM utilities (including llvm-rc and llvm-readobj), Python 3, curl, tar, and sha256sum. Packaging also needs zip/unzip. Wine and Python `pefile` are required by the current bootstrap helper and the oracle tools. Analysis uses NumPy; PNG rendering also uses Matplotlib.

```sh
tools/bootstrap_toolchain.sh
bash shim/build.sh           # build/shim/PhysXLoader.dll, with development symbols
bash shim/build.sh release   # build/shim-release/PhysXLoader.dll, without a PDB
bash installer/build.sh      # build/installer/ShatterSetup.exe, embeds a fresh release DLL
```

The bootstrap helper downloads a pinned xwin binary and the Microsoft CRT/Windows SDK into `.toolchain/`. Running it accepts Microsoft's license for those files. Review the helper before running it; these dependencies are not redistributed with the source.

Shatter's DLL and installer do **not** require PhysX SDK headers or vendor DLLs. If an original loader is locally available, `shim/build.sh` additionally compares its export names and ordinals. `XWIN` can point to an existing xwin-splatted CRT/SDK tree.

## Native solver smoke test

This builds and runs an authored cloth scenario without the original PhysX runtime:

```sh
tools/run_cloth_ours.sh scenarios/cloth/drop_plane.txt
```

It writes `captures/cloth/drop_plane/ours-1.bin`. This is a smoke test, not an oracle comparison. Native tools use clang++; the shipped DLL and oracle interfaces must use clang-cl, not MinGW GCC.

## Oracle comparisons

These require your own local PhysX DLLs and **2.8.1** SDK headers. The oracle runs the 2.8.1 core with matching headers, not the game's 2.8.0 ABI. Set `PHYSX_SDK` to the SDK's `SDKs` directory, or place it at the default ignored location `third_party/physx-sdk-2.8.1/SDKs`. The historical header source is recorded in [NOTES.md](NOTES.md); the project does not supply or automatically fetch it.

`tools/sync_vendor_dlls.sh` reads a user-supplied `GAME_PREFIX` and copies the expected game loader, runtime cores and helper DLLs into `vendor-dlls/`. It assumes the directory layout shown in the script and needs both 2.8.0 and 2.8.1 runtimes. Supply those files locally if your install differs; do not add them to Git.

```sh
GAME_PREFIX="<your-game-prefix>" tools/sync_vendor_dlls.sh
tools/setup_prefix.sh
bash harness/build.sh
tools/run_cloth_oracle.sh scenarios/cloth/drop_plane.txt
tools/run_cloth_ours.sh scenarios/cloth/drop_plane.txt
ORACLE=1 tools/fluid_probe_check.sh
```

The prefix setup writes only to a throwaway prefix. Leave `WINEPREFIX` unset for its default `.wine-harness`, or point it to another disposable prefix; never point it at the original game prefix. Oracle and shim runners expose `PX_PREFIX` and `PX_RUN` for isolated runs. `HARNESS_OUT`, `SHIM_OUT` and `OURS_BIN` override build destinations.

Cloth comparisons use the oracle's local `cooked.bin` when present to preserve stored edge order. The fluid checker needs local oracle captures; a clean checkout alone cannot run the complete comparison suite. Some original artifacts intentionally differ and have accepted Shatter outputs in `scenarios/fluid/probe_golden.json`. Other known differences can still fail the checker; investigate failures rather than updating golden data blindly. See [docs/README.md](docs/README.md).

## Game diagnostics

`tools/run_game_logged.sh` is a development runner, not an installer. It expects a separately prepared copy at `.wine-game/` and a `PROTONPATH` set in `.local.env`. It changes config files and installs the shim **in that copy**. Do not symlink the copy's writable files back to the original install. The script does not create the copy or launch safely against an arbitrary install layout.

[Test spots](docs/test-spots.md) describes checkpoint selection. Debug logs can include machine paths; recordings can contain game geometry. Review and minimize diagnostic attachments before sharing them publicly.

## Checks before contributing

```sh
python3 -m unittest discover -s tests
python3 tools/release_audit.py
git diff --check
```

Keep changes focused. Preserve comments that explain ABI offsets, numerical ordering, ownership, threading or deliberate differences from the original. Remove obsolete workflow notes rather than replacing technical explanations with restatements of the code. For publishing history and binary packages, follow [docs/release.md](docs/release.md).
