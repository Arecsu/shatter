// Our own PhysX loader: what the original Epic loader (PhysXLoader.dll 2.8.0.7, "2.8.0_FC7") does, so the shim needs no
// PhysXLoader_orig.dll next to it. Behaviour as read from the original's code [V: disassembly]:
//
// Core module (loaded once, on the first export that needs it, with the version of that call):
//   1. LoadLibraryA("PhysXCore.dll") by bare name (normal DLL search: the exe's directory first).
//   2. Otherwise HKLM\Software\Ageia Technologies, value "PhysXCore Path" (must be REG_SZ, KEY_READ, default registry
//      view), plus "\v<major>.<minor>.<bugfix>\" from the requested version (bytes 3, 2, 1 in decimal), plus
//      "PhysXCore.dll". For the game's 0x02080000 that is <path>\v2.8.0\PhysXCore.dll.
//   Ours only: if the default view has no usable value, the 64-bit view is tried too (a 64-bit installer may have
//   written it there); the original would fail at that point.
// Cooking module: the same with "NxCooking.dll" by bare name (the game ships one in Binaries/) and "PhysXCooking.dll"
// in the registry directory; cached separately, never unloaded.
//
// Exports:
//   NxCreatePhysicsSDK(version, alloc, output, desc, err): if version > 2.6.1 and err != 0, *err = 1
//     (NXCE_PHYSX_NOT_FOUND) first. Core or NpCreatePhysicsSDK missing: return 0 (err stays 1). Otherwise
//     NpCreatePhysicsSDK(version, alloc, output, desc, &e); the SDK pointer is kept and *err = e (version > 2.6.1 only).
//   NxCreatePhysicsSDKWithID: the company/app strings are dropped, then as NxCreatePhysicsSDK.
//   NxReleasePhysicsSDK(sdk): sdk 0 = nothing. With a kept SDK: NpReleasePhysicsSDK(sdk); a non-zero return (still
//     referenced) stops here. Without that export the kept SDK's vtable slot 1 (release) is called instead. Then the
//     kept SDK and util lib are forgotten and the core is unloaded.
//   NxGetPhysicsSDK / NxGetFoundationSDK / NxGetPhysicsSDKAllocator: the core's Np* function, 0 without a loaded core
//     (these never load it).
//   NxGetUtilLib: NpGetUtilLib once, cached until the SDK is released.
//   NxGetCookingLib = NxGetCookingLibWithID (one function, reads only the version): NxGetCookingInterface(version) of
//     the cooking module, 0 if it is missing.

#include <windows.h>

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "shim.h"

#pragma comment(lib, "advapi32.lib")

namespace shim {
namespace {

HMODULE g_core = nullptr;
HMODULE g_cooking = nullptr;
uint32_t g_sdk = 0;
uint32_t g_util = 0;

bool readCorePath(REGSAM view, char* out, DWORD size) {
  HKEY key = nullptr;
  if (RegOpenKeyExA(HKEY_LOCAL_MACHINE, "Software\\Ageia Technologies", 0, KEY_READ | view, &key) != ERROR_SUCCESS)
    return false;
  DWORD type = 0, n = size - 1;
  const LONG r = RegQueryValueExA(key, "PhysXCore Path", nullptr, &type, reinterpret_cast<BYTE*>(out), &n);
  RegCloseKey(key);
  if (r != ERROR_SUCCESS || type != REG_SZ) return false;
  out[n] = 0;
  return true;
}

HMODULE loadPhysX(const char* bareName, const char* dirName, uint32_t version) {
  HMODULE m = LoadLibraryA(bareName);
  if (m) {
    logLine("loader: %s by name -> %p", bareName, (void*)m);
    return m;
  }
  char path[0x800];
  const REGSAM views[2] = {0, KEY_WOW64_64KEY};
  for (REGSAM view : views) {
    if (!readCorePath(view, path, sizeof path)) continue;
    const size_t len = strlen(path);
    snprintf(path + len, sizeof path - len, "\\v%u.%u.%u\\%s", version >> 24, (version >> 16) & 0xff,
             (version >> 8) & 0xff, dirName);
    m = LoadLibraryA(path);
    logLine("loader: %s (registry%s) -> %p gle=%lu", path, view ? ", 64-bit view" : "", (void*)m,
            (unsigned long)(m ? 0 : GetLastError()));
    if (m) return m;
  }
  logLine("loader: %s not found (by name, nor through HKLM\\Software\\Ageia Technologies\\PhysXCore Path)", bareName);
  return nullptr;
}

HMODULE core(uint32_t version) {
  if (!g_core) g_core = loadPhysX("PhysXCore.dll", "PhysXCore.dll", version);
  return g_core;
}

FARPROC coreProc(const char* name) { return g_core ? GetProcAddress(g_core, name) : nullptr; }

typedef uint32_t(__cdecl* FnNpCreate)(uint32_t, uint32_t, uint32_t, uint32_t, uint32_t*);
typedef uint32_t(__cdecl* FnNpRelease)(uint32_t);
typedef uint32_t(__cdecl* FnNp0)(void);
typedef uint32_t(__cdecl* FnCooking)(uint32_t);
typedef void(__fastcall* FnRelease)(void* self, void* edx);

uint32_t __cdecl ownCreate(uint32_t version, uint32_t alloc, uint32_t output, uint32_t desc, uint32_t errorCode) {
  uint32_t* err = version > 0x02060100 ? reinterpret_cast<uint32_t*>(static_cast<uintptr_t>(errorCode)) : nullptr;
  if (err) *err = 1;  // NXCE_PHYSX_NOT_FOUND until the core says otherwise
  if (!core(version)) return 0;
  FnNpCreate np = reinterpret_cast<FnNpCreate>(coreProc("NpCreatePhysicsSDK"));
  if (!np) return 0;
  uint32_t e = 0;
  g_sdk = np(version, alloc, output, desc, &e);
  if (err) *err = e;
  return g_sdk;
}

uint32_t __cdecl ownCreateWithID(uint32_t version, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t alloc,
                                 uint32_t output, uint32_t desc, uint32_t errorCode) {
  return ownCreate(version, alloc, output, desc, errorCode);
}

void __cdecl ownRelease(uint32_t sdk) {
  if (!sdk) return;
  if (g_sdk) {
    FnNpRelease np = reinterpret_cast<FnNpRelease>(coreProc("NpReleasePhysicsSDK"));
    if (np) {
      if (np(sdk)) return;  // still referenced
    } else {
      void* s = fromPtr32<void>(g_sdk);
      FnRelease rel = reinterpret_cast<FnRelease>(static_cast<uintptr_t>((*fromPtr32<uint32_t*>(g_sdk))[1]));
      rel(s, nullptr);
    }
  }
  g_sdk = 0;
  g_util = 0;
  if (g_core) {
    FreeLibrary(g_core);
    g_core = nullptr;
  }
}

template <const char* const* Name>
uint32_t __cdecl ownGet() {
  FnNp0 np = reinterpret_cast<FnNp0>(coreProc(*Name));
  return np ? np() : 0;
}
const char* const kAllocator = "NpGetPhysicsSDKAllocator";
const char* const kFoundation = "NpGetFoundationSDK";
const char* const kPhysics = "NpGetPhysicsSDK";

uint32_t __cdecl ownGetUtilLib() {
  if (!g_util) {
    FnNp0 np = reinterpret_cast<FnNp0>(coreProc("NpGetUtilLib"));
    if (np) g_util = np();
  }
  return g_util;
}

uint32_t __cdecl ownGetCookingLib(uint32_t version) {
  if (!g_cooking) g_cooking = loadPhysX("NxCooking.dll", "PhysXCooking.dll", version);
  FnCooking f = g_cooking ? reinterpret_cast<FnCooking>(GetProcAddress(g_cooking, "NxGetCookingInterface")) : nullptr;
  return f ? f(version) : 0;
}

uint32_t __cdecl ownGetCookingLibWithID(uint32_t version, uint32_t, uint32_t, uint32_t, uint32_t) {
  return ownGetCookingLib(version);
}

}  // namespace

LoaderFns ownLoader() {
  LoaderFns f;
  f.createPhysicsSDK = &ownCreate;
  f.createPhysicsSDKWithID = &ownCreateWithID;
  f.getCookingLib = &ownGetCookingLib;
  f.getCookingLibWithID = &ownGetCookingLibWithID;
  f.getFoundationSDK = &ownGet<&kFoundation>;
  f.getPhysicsSDK = &ownGet<&kPhysics>;
  f.getPhysicsSDKAllocator = &ownGet<&kAllocator>;
  f.getUtilLib = &ownGetUtilLib;
  f.releasePhysicsSDK = &ownRelease;
  return f;
}

// ---- core version and vtable checks ---------------------------------------------------------

namespace {
// VS_FIXEDFILEINFO of a loaded module, read from its RT_VERSION resource (no version.dll needed).
bool moduleFileVersion(HMODULE m, uint32_t* ms, uint32_t* ls) {
  HRSRC r = FindResourceA(m, MAKEINTRESOURCEA(1), MAKEINTRESOURCEA(16));  // RT_VERSION
  HGLOBAL g = r ? LoadResource(m, r) : nullptr;
  const uint8_t* p = g ? static_cast<const uint8_t*>(LockResource(g)) : nullptr;
  const DWORD n = r ? SizeofResource(m, r) : 0;
  for (DWORD i = 0; p && i + 52 <= n; i += 4) {
    uint32_t sig;
    memcpy(&sig, p + i, 4);
    if (sig != 0xFEEF04BDu) continue;
    memcpy(ms, p + i + 8, 4);
    memcpy(ls, p + i + 12, 4);
    return true;
  }
  return false;
}
}  // namespace

int coreVersionOf(uint32_t addr) {
  HMODULE m = nullptr;
  if (!GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                          reinterpret_cast<LPCSTR>(static_cast<uintptr_t>(addr)), &m))
    return 0;
  uint32_t ms = 0, ls = 0;
  if (!moduleFileVersion(m, &ms, &ls)) return 0;
  // 2.8.0: only builds checked against what the shim calls, reads and replaces. 2.8.0.45 is the one installed by the
  // PhysX system software (development); 2.8.0.7 is shipped next to MirrorsEdge.exe by some copies of the game and
  // is the one the game then loads: same vtable sizes for every interface, and every slot the shim calls is the same
  // or nearly the same code (vendor-dlls/engine/v2.8.0.7, compared 2026-10-13). Other 2.8.0 builds: pass-through.
  if (ms == 0x00020008 && (ls == 0x0000002D || ls == 0x00000007)) return 280;
  if (ms == 0x00020008 && (ls >> 16) == 1) return 281;  // the oracle harness core (2.8.1 headers)
  if (ms == 0x00020008 && (ls >> 16) == 0)
    logLine("core 2.8.0.%u: not a build Shatter was checked against", (unsigned)(ls & 0xFFFF));
  return 0;
}

bool vtableInModule(uint32_t vt, uint32_t slots) {
  HMODULE m = nullptr;
  if (!GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                          reinterpret_cast<LPCSTR>(static_cast<uintptr_t>(vt)), &m))
    return false;
  const uint8_t* base = reinterpret_cast<const uint8_t*>(m);
  const IMAGE_NT_HEADERS* nt =
      reinterpret_cast<const IMAGE_NT_HEADERS*>(base + reinterpret_cast<const IMAGE_DOS_HEADER*>(base)->e_lfanew);
  const uint32_t lo = ptr32(base), hi = lo + nt->OptionalHeader.SizeOfImage;
  const uint32_t* p = fromPtr32<const uint32_t>(vt);
  if (IsBadReadPtr(p, slots * 4)) return false;
  for (uint32_t i = 0; i < slots; ++i)
    if (p[i] < lo || p[i] >= hi) return false;
  return true;
}

}  // namespace shim
