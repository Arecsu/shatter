# PhysX 2.8 compatibility research

These are historical setup findings, not release instructions. See [README.md](README.md) for player-facing behavior, [CONTRIBUTING.md](CONTRIBUTING.md) for builds and tests, and [docs/README.md](docs/README.md) for the research index. [V] marks a verified observation; [A] marks an assumption or inference.

## Runtime and loader [V]

Mirror's Edge requests SDK `0x02080000` through Epic's `2.8.0_FC7` loader. The examined game executable is PE32 i386, version 1.0.1.0. Its PhysX loader is version 2.8.0.7; the examined system runtime is PhysXCore 2.8.0.45 from PhysX System Software 9.13.1220. Binary identities, hashes, imports and exports are recorded in `docs/inventory.json`; no binaries are included.

The original loader first tries `PhysXCore.dll` by bare name, then reads `HKLM\Software\AGEIA Technologies\PhysXCore Path` and appends `\v<major>.<minor>.<bugfix>\PhysXCore.dll`. Cooking similarly tries `NxCooking.dll`, then the versioned `PhysXCooking.dll`. The registry value must be `REG_SZ` in the default view.

Its exports call the core's `Np*` entry points and cooking's `NxGetCookingInterface`. A missing core produces `NXCE_PHYSX_NOT_FOUND`. Shatter implements this discovery in `shim/loader_core.cpp`, additionally trying the 64-bit registry view. Players do not need a renamed original loader; `PhysXLoader_orig.dll` is an optional development override.

Rigid bodies and cooking remain on the original runtime. Shatter does not redistribute it.

## ABI and reference backend

The game's `NxPhysicsSDKDesc` is seven dwords; the public 2.8.1 header describes five. `NxScene` has 155 virtual slots in 2.8.0 versus 157 in 2.8.1, and the joint interfaces differ. Equal slot counts on other interfaces are not proof of compatibility. See [docs/abi-2.8.0-vs-2.8.1.md](docs/abi-2.8.0-vs-2.8.1.md).

The oracle harnesses use the 2.8.1 core with matching 2.8.1 headers. The shim uses independently checked layouts and slot mappings for the game's runtime. Measurements against 2.8.1 are evidence about that reference backend, not proof of identical behavior in every 2.8.0 path.

The locally examined headers came from an unofficial mirror of PhysX 2.8.1, changelist 30725: <https://github.com/sigmaco/physx-v2.8.1>, commit `687e1e9c`. Headers and vendor libraries remain outside the repository. No NVIDIA import libraries are linked.

## Toolchain rationale

The shipped DLL and installer target `i686-pc-windows-msvc` with clang-cl, lld-link and an xwin-provided CRT/Windows SDK. `/MT` links the CRT statically.

The DLL crosses MSVC C++ interface boundaries. MinGW GCC's C++ ABI differs in virtual destructor slots, overloaded virtual ordering, name mangling, RTTI and exception handling; it is not suitable for those interfaces. Native solver-only tools do not cross that boundary and can use clang++.

## Isolated reference runs [V]

`tools/setup_prefix.sh` copies locally supplied runtimes into a throwaway Wine prefix under `C:\PhysX\Engine\v2.8.{0,1}` and sets `PhysXCore Path` and `HwSelection=CPU` in both registry views. Harness executables load the DLLs with `LoadLibrary` and `GetProcAddress`.

The initial hello-world check established:

- `--abi 2.8.1`: SDK creation succeeds with core 2.8.1.57, internal version `0x119`, no hardware or PPUs. One 1/60-second step moves a sphere from y=10 to y=9.997275, consistent with semi-implicit Euler under gravity. The reference run reported two CUDA-unavailable warnings and no errors.
- `--abi 2.8.0-probe`: the original loader accepts the game's seven-dword descriptor and creates/releases SDK 2.8.0.45. This mode makes no virtual API calls and does not validate the full ABI.

The game install and its original prefix must remain untouched during development. Logs, extracted game data, cooked streams and decompiler output are local-only; see [CONTRIBUTING.md](CONTRIBUTING.md).
