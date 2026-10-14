// PhysXLoader.dll replacement. Hooks on NxPhysicsSDK/NxScene host fluids and cloth on the CPU solvers;
// other APIs continue to use the original runtime. Unsupported cores remain pass-through.
//
// Exports match the original Epic loader's nine names and ordinals. Runtime discovery is in loader_core.cpp;
// a local PhysXLoader_orig.dll is an optional development override. Exports are __cdecl (caller cleans the stack).
//
// Optional debug log: PhysXLoader_shim.log next to the DLL, with pid/tid tags. Disabled by default.
//
// No NVIDIA headers are included. Argument counts were taken from the original's
// disassembly (stack offsets at each export's entry), cross-checked against the public header:
//   NxCreatePhysicsSDK        5 dwords  (version, allocator, output, desc*, errorCode*)
//   NxCreatePhysicsSDKWithID  9 dwords  (original reads args 1,6,7,8,9; 2..5 are the app strings)
//   NxGetCookingLib           1 dword   (version)
//   NxGetCookingLibWithID     5 dwords  (header; the original body only reads arg 1)
//   NxGetFoundationSDK        0
//   NxGetPhysicsSDK           0
//   NxGetPhysicsSDKAllocator  0
//   NxGetUtilLib              0
//   NxReleasePhysicsSDK       1 dword   (sdk*)

#include <windows.h>

#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cloth_ours.h"
#include "coverage.h"
#include "shim.h"
#include "version.h"

namespace shim {

char g_dir[MAX_PATH] = "";
namespace {
FILE* g_log = nullptr;
}

void logLine(const char* fmt, ...) {
  if (!g_log) return;
  va_list ap;
  va_start(ap, fmt);
  _lock_file(g_log);  // whole lines (the hosted fluids also log from their stepping thread)
  fprintf(g_log, "[pid %lu tid %lu] ", (unsigned long)GetCurrentProcessId(), (unsigned long)GetCurrentThreadId());
  vfprintf(g_log, fmt, ap);
  fputc('\n', g_log);
  fflush(g_log);
  _unlock_file(g_log);
  va_end(ap);
}

// ---- configuration: environment variable, else shatter.ini next to the DLL, else the default in code ----------------
namespace {
struct IniEntry {
  char key[48];
  char value[208];
};
IniEntry g_ini[64];
int g_iniCount = 0;

bool iniBool(const char* v, bool* out) {
  if (!_stricmp(v, "1") || !_stricmp(v, "on") || !_stricmp(v, "true") || !_stricmp(v, "yes")) return *out = true, true;
  if (!_stricmp(v, "0") || !_stricmp(v, "off") || !_stricmp(v, "false") || !_stricmp(v, "no")) return *out = false, true;
  return false;
}

void iniSet(const char* key, const char* value) {
  for (int i = 0; i < g_iniCount; ++i)
    if (!strcmp(g_ini[i].key, key)) return (void)snprintf(g_ini[i].value, sizeof g_ini[i].value, "%s", value);
  if (g_iniCount == 64) return;
  snprintf(g_ini[g_iniCount].key, sizeof g_ini[0].key, "%s", key);
  snprintf(g_ini[g_iniCount].value, sizeof g_ini[0].value, "%s", value);
  ++g_iniCount;
}

// One "key = value" line of shatter.ini as the switch it stands for. Returns false for an unknown key or value.
bool iniApply(const char* k, const char* v) {
  bool b = false;
  if (!_strnicmp(k, "PXSHIM_", 7)) {  // developer switches, verbatim
    char up[48];
    snprintf(up, sizeof up, "%s", k);
    _strupr(up);
    iniSet(up, v);
    return true;
  }
  if (!_stricmp(k, "fluid")) {
    const char* m = !_stricmp(v, "collide-only") ? "ours" : iniBool(v, &b) ? (b ? "ours-all" : "off") : nullptr;
    return m && (iniSet("PXSHIM_FLUID", m), true);
  }
  if (!_stricmp(k, "cloth")) return iniBool(v, &b) && (iniSet("PXSHIM_CLOTH", b ? "ours" : "off"), true);
  if (!_stricmp(k, "cloth_edge_contact"))
    return iniBool(v, &b) && (iniSet("PXSHIM_CLOTH_EDGESHAPE", b ? "1" : "0"), true);
  if (!_stricmp(k, "cloth_self_contact")) {
    if (iniBool(v, &b)) return b || (iniSet("PXSHIM_CLOTH_SELF", "0"), true);  // on = the default distance
    char* end = nullptr;
    const double d = strtod(v, &end);
    return end != v && !*end && d >= 0 && (iniSet("PXSHIM_CLOTH_SELF", v), true);  // a distance fraction
  }
  if (!_stricmp(k, "threads")) {
    if (!_stricmp(v, "auto")) return true;
    return atoi(v) >= 1 && (iniSet("PXSHIM_THREADS", v), true);
  }
  if (!_stricmp(k, "slide_steering_fix"))
    return iniBool(v, &b) && (iniSet("PXSHIM_SLIDEFIX", b ? "1" : "0"), true);
  if (!_stricmp(k, "debug_log")) return iniBool(v, &b) && (iniSet("PXSHIM_DEBUG", b ? "1" : "0"), true);
  if (!_stricmp(k, "cloth_record")) return iniBool(v, &b) && (iniSet("PXSHIM_CLOTH_REC", b ? "1" : "0"), true);
  if (!_stricmp(k, "fluid_trace")) return atoi(v) >= 0 && (iniSet("PXSHIM_TRACE", v), true);
  return false;
}

char g_iniErrors[512] = "";  // logged once the log is open

void iniLoad() {
  char path[MAX_PATH];
  snprintf(path, sizeof path, "%sshatter.ini", g_dir);
  FILE* f = fopen(path, "r");
  if (!f) return;
  char line[320];
  int n = 0;
  while (fgets(line, sizeof line, f)) {
    ++n;
    char* p = line;
    while (*p == ' ' || *p == '\t') ++p;
    if (*p == '#' || *p == ';' || *p == '[' || *p == '\r' || *p == '\n' || !*p) continue;
    char* eq = strchr(p, '=');
    char* c = strpbrk(eq ? eq : p, "#;");
    if (c) *c = 0;  // trailing comment
    bool ok = false;
    if (eq) {
      *eq = 0;
      char* v = eq + 1;
      for (char* e = eq - 1; e >= p && (*e == ' ' || *e == '\t'); --e) *e = 0;
      while (*v == ' ' || *v == '\t') ++v;
      for (char* e = v + strlen(v) - 1; e >= v && strchr(" \t\r\n", *e); --e) *e = 0;
      ok = *p && *v && iniApply(p, v);
    }
    if (!ok) {
      const size_t len = strlen(g_iniErrors);
      snprintf(g_iniErrors + len, sizeof g_iniErrors - len, "%s%d", len ? ", " : "", n);
    }
  }
  fclose(f);
}
}  // namespace

const char* env(const char* name) {
  static char buf[8][256];
  static int next = 0;
  char* b = buf[next++ & 7];
  DWORD n = GetEnvironmentVariableA(name, b, sizeof(buf[0]));
  if (n == 0 || n >= sizeof(buf[0])) b[0] = 0;
  if (n == 0)
    for (int i = 0; i < g_iniCount; ++i)
      if (!strcmp(g_ini[i].key, name)) return g_ini[i].value;
  return b;
}

bool debug() {
  static const bool on = env("PXSHIM_DEBUG")[0] == '1';
  return on;
}

bool cpuOnly() {
  static const bool on = env("PXSHIM_ALLOW_GPU")[0] != '1';
  return on;
}

float stepRefDt() {
  static const float refDt = [] {
    const char* s = env("PXSHIM_REF_HZ");
    const float hz = s[0] ? (float)atof(s) : 62.0f;
    const float dt = hz > 0 ? 1.0f / hz : 0.0f;
    logLine("step-rate independence: %s", dt > 0 ? "on" : "off (PXSHIM_REF_HZ=0)");
    if (dt > 0) logLine("  fluid coefficients taken at %g Hz steps (PXSHIM_REF_HZ)", (double)hz);
    return dt;
  }();
  return refDt;
}

float clothRefDt() {
  static const float refDt = [] {
    const char* s = env("PXSHIM_CLOTH_REF_HZ");
    const float hz = s[0] ? (float)atof(s) : 62.0f;
    const float dt = hz > 0 && stepRefDt() > 0 ? 1.0f / hz : 0.0f;
    if (dt > 0) logLine("  cloth coefficients taken at %g Hz steps (PXSHIM_CLOTH_REF_HZ)", (double)hz);
    else logLine("  cloth step-rate independence off");
    return dt;
  }();
  return refDt;
}

bool dumpWords(const char* tag, const void* p, size_t bytes) {
  if (!g_log || !p || IsBadReadPtr(p, bytes)) return false;
  const uint32_t* d = reinterpret_cast<const uint32_t*>(p);
  for (size_t i = 0; i < bytes / 4; i += 8) {
    char line[160];
    int n = snprintf(line, sizeof(line), "  %s+%03X:", tag, (unsigned)(i * 4));
    for (size_t j = i; j < i + 8 && j < bytes / 4; ++j) n += snprintf(line + n, sizeof(line) - n, " %08X", d[j]);
    logLine("%s", line);
  }
  return true;
}

bool patchSlot(uint32_t vt, uint32_t slot, uint32_t value, uint32_t* orig) {
  uint32_t* p = reinterpret_cast<uint32_t*>(static_cast<uintptr_t>(vt)) + slot;
  DWORD old = 0;
  if (!VirtualProtect(p, sizeof(uint32_t), PAGE_READWRITE, &old)) {
    logLine("patch vt=%08X slot %u: VirtualProtect failed gle=%lu", vt, slot, (unsigned long)GetLastError());
    return false;
  }
  *orig = *p;
  *p = value;
  VirtualProtect(p, sizeof(uint32_t), old, &old);
  FlushInstructionCache(GetCurrentProcess(), p, sizeof(uint32_t));
  logLine("patch vt=%08X slot %u: %08X -> %08X", vt, slot, *orig, value);
  return true;
}

}  // namespace shim

using namespace shim;

namespace {

HMODULE g_orig = nullptr;

// Format a char* argument: hex pointer plus (up to 64 chars of) its text.
const char* sx(uint32_t p) { return p ? reinterpret_cast<const char*>(p) : "(null)"; }

typedef uint32_t(__cdecl* Fn0)(void);
typedef uint32_t(__cdecl* Fn1)(uint32_t);
typedef uint32_t(__cdecl* Fn5)(uint32_t, uint32_t, uint32_t, uint32_t, uint32_t);
typedef uint32_t(__cdecl* Fn9)(uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t);
typedef void(__cdecl* FnV1)(uint32_t);

Fn5 g_CreatePhysicsSDK = nullptr;
Fn9 g_CreatePhysicsSDKWithID = nullptr;
Fn1 g_GetCookingLib = nullptr;
Fn5 g_GetCookingLibWithID = nullptr;
Fn0 g_GetFoundationSDK = nullptr;
Fn0 g_GetPhysicsSDK = nullptr;
Fn0 g_GetPhysicsSDKAllocator = nullptr;
Fn0 g_GetUtilLib = nullptr;
FnV1 g_ReleasePhysicsSDK = nullptr;
bool g_passThrough = false;  // unknown core or unexpected vtables: no vtable hooks at all

FARPROC resolve(const char* name) {
  FARPROC p = g_orig ? GetProcAddress(g_orig, name) : nullptr;
  logLine("resolve %s -> %p", name, (void*)p);
  return p;
}

void shimInit(HINSTANCE self) {
  char path[MAX_PATH] = "";
  GetModuleFileNameA(self, path, MAX_PATH);
  snprintf(g_dir, sizeof(g_dir), "%s", path);
  char* slash = strrchr(g_dir, '\\');
  if (slash) slash[1] = 0;

  iniLoad();
  if (shim::debug()) {  // PXSHIM_DEBUG=1 or debug_log = on
    char logPath[MAX_PATH];
    snprintf(logPath, sizeof(logPath), "%sPhysXLoader_shim.log", g_dir);
    g_log = fopen(logPath, "a");
  }
  logLine("Shatter %s, shatter.ini: %d setting(s)%s%s", SHATTER_VERSION_STR, g_iniCount,
          g_iniErrors[0] ? ", ignored lines " : "", g_iniErrors);

  // A PhysXLoader_orig.dll next to us (dev setups) is used as the loader; otherwise our own copy of it (loader_core.cpp).
  char origPath[MAX_PATH];
  snprintf(origPath, sizeof(origPath), "%sPhysXLoader_orig.dll", g_dir);
  g_orig = GetFileAttributesA(origPath) != INVALID_FILE_ATTRIBUTES ? LoadLibraryA(origPath) : nullptr;
  logLine("attach: shim=%s, loader: %s", path, g_orig ? origPath : "own (no PhysXLoader_orig.dll)");
  if (g_orig) {
    g_CreatePhysicsSDK = (Fn5)resolve("NxCreatePhysicsSDK");
    g_CreatePhysicsSDKWithID = (Fn9)resolve("NxCreatePhysicsSDKWithID");
    g_GetCookingLib = (Fn1)resolve("NxGetCookingLib");
    g_GetCookingLibWithID = (Fn5)resolve("NxGetCookingLibWithID");
    g_GetFoundationSDK = (Fn0)resolve("NxGetFoundationSDK");
    g_GetPhysicsSDK = (Fn0)resolve("NxGetPhysicsSDK");
    g_GetPhysicsSDKAllocator = (Fn0)resolve("NxGetPhysicsSDKAllocator");
    g_GetUtilLib = (Fn0)resolve("NxGetUtilLib");
    g_ReleasePhysicsSDK = (FnV1)resolve("NxReleasePhysicsSDK");
  } else {
    const LoaderFns f = shim::ownLoader();
    g_CreatePhysicsSDK = f.createPhysicsSDK;
    g_CreatePhysicsSDKWithID = f.createPhysicsSDKWithID;
    g_GetCookingLib = f.getCookingLib;
    g_GetCookingLibWithID = f.getCookingLibWithID;
    g_GetFoundationSDK = f.getFoundationSDK;
    g_GetPhysicsSDK = f.getPhysicsSDK;
    g_GetPhysicsSDKAllocator = f.getPhysicsSDKAllocator;
    g_GetUtilLib = f.getUtilLib;
    g_ReleasePhysicsSDK = f.releasePhysicsSDK;
  }
  shim::fluidInit();
  shim::clothInit();
  shim::gameHooksInit();
}

// ---- vtable hooks on the core's NxPhysicsSDK / NxScene ----------------------------------
// Slots (2.8.1 headers; 2.8.0 agrees for all of these, docs/abi-2.8.0-vs-2.8.1.md):
//   NxPhysicsSDK: createScene 4, releaseScene 5 (both patched)
//   NxScene:      createActor 7 (located only, not hooked), createFluid 126, releaseFluid 127,
//                 simulate 140, fetchResults 142 (patched; the fluid ones route to fluid_wrap.cpp).
//                 Slots >= 153 differ by version: never hooked.
//                 Cloth (cloth_wrap.cpp, logging only): NxScene createCloth 131 / releaseCloth 132,
//                 NxPhysicsSDK createClothMesh 21 / releaseClothMesh 22 (2.8.0 and 2.8.1; PXSHIM_CLOTH_MESH=0 = off).
// Hooks are __fastcall(this, edx_unused, args...): the game calls __thiscall, so ECX = this and the
// stack args are identical; the callee pops its stack args exactly as the original would.
// Each vtable is patched once (first object seen), the saved original is called through the same
// __fastcall type with EDX ignored.

const uint32_t kSdkCreateScene = 4, kSdkReleaseScene = 5;
// getHWVersion 31 / getNbPPUs 32 (2.8.0 [V]: both forward to the hardware object and return 0 without one). The game
// asks getHWVersion only in its hardware gate (MirrorsEdge.exe 0x00f8c2b0), which decides NX_FF_HARDWARE,
// NX_CLF_HARDWARE and the PPU compartment; PhysXEnhanced (the extra effects) does not depend on it [V: call graph].
const uint32_t kSdkGetHWVersion = 31, kSdkGetNbPPUs = 32;
const uint32_t kSceneCreateFluid = 126, kSceneReleaseFluid = 127, kSceneSimulate = 140, kSceneFetchResults = 142;
// Coverage only (docs/audit/GAPS.md row 31: hosted objects are missing from these lists): counted pass-throughs.
// Checked before hooking: every slot up to the last one shared by 2.8.0 and 2.8.1 points into the core's image.
const uint32_t kSceneSlotsSame = 153, kSdkSlotsChecked = 33;
const uint32_t kSceneGetNbFluids = 128, kSceneGetFluids = 129, kSceneGetNbCloths = 133, kSceneGetCloths = 134;

typedef uint32_t(__fastcall* FnCreateScene)(void* self, void* edx, const void* desc);
typedef void(__fastcall* FnReleaseScene)(void* self, void* edx, void* scene);
typedef void(__fastcall* FnSimulate)(void* self, void* edx, float elapsed);
typedef bool(__fastcall* FnFetchResults)(void* self, void* edx, uint32_t status, uint32_t block, uint32_t errorState);

uint32_t g_sdkVt = 0;
uint32_t g_sceneVt = 0;
FnCreateScene g_origCreateScene = nullptr;
FnReleaseScene g_origReleaseScene = nullptr;
FnCreateFluid g_origCreateFluid = nullptr;
FnReleaseFluid g_origReleaseFluid = nullptr;
FnSimulate g_origSimulate = nullptr;
FnFetchResults g_origFetchResults = nullptr;
typedef uint32_t(__fastcall* FnSceneU32)(void* self, void* edx);
FnSceneU32 g_origSceneList[4] = {nullptr, nullptr, nullptr, nullptr};  // getNbFluids, getFluids, getNbCloths, getCloths
template <int I>
uint32_t __fastcall hkSceneList(void* self, void* edx) {
  (void)edx;
  shim::cov(I < 2 ? shim::kCovSceneGetFluids : shim::kCovSceneGetCloths);
  return g_origSceneList[I](self, nullptr);
}
uint32_t g_simCalls = 0;
uint32_t g_fetchCalls = 0;

// Frame cost: the core's simulate + fetchResults vs our hosted fluids (inside the same calls), in ms.
struct FrameStats {
  double core = 0, ours = 0, coreMax = 0, oursMax = 0, frameCore = 0, frameOurs = 0;
  double coreSim = 0, coreFetch = 0, coreFetchMax = 0, frameFetch = 0;  // the core's part split by call
  double wall = 0, wallMax = 0;  // the game's whole frame: time between successful fetchResults
  uint32_t frames = 0;
} g_stats;
double g_lastFetchMs = 0;
double nowMs() {
  static LARGE_INTEGER freq = [] { LARGE_INTEGER f; QueryPerformanceFrequency(&f); return f; }();
  LARGE_INTEGER t;
  QueryPerformanceCounter(&t);
  return 1000.0 * (double)t.QuadPart / (double)freq.QuadPart;
}


uint32_t __fastcall hkCreateFluid(void* self, void* edx, const void* desc);
void __fastcall hkReleaseFluid(void* self, void* edx, void* fluid);
void __fastcall hkSimulate(void* self, void* edx, float elapsed);
bool __fastcall hkFetchResults(void* self, void* edx, uint32_t status, uint32_t block, uint32_t errorState);

// NxScene vtable (the object returned by createScene): patch the four fluid-path slots once.
void installSceneHooks(uint32_t scene) {
  uint32_t vt = *reinterpret_cast<const uint32_t*>(static_cast<uintptr_t>(scene));
  if (!vt || g_passThrough) return;
  if (g_sceneVt) {
    if (vt != g_sceneVt) logLine("warning: second NxScene vtable %08X ignored (first %08X)", vt, g_sceneVt);
    return;
  }
  g_sceneVt = vt;
  if (!vtableInModule(vt, kSceneSlotsSame)) {
    g_passThrough = true;
    logLine("NxScene vtable %08X does not look like the 2.8.0/2.8.1 one: full pass-through (no fluid/cloth hosting)", vt);
    return;
  }
  uint32_t o = 0;
  if (patchSlot(vt, kSceneCreateFluid, ptr32(&hkCreateFluid), &o)) g_origCreateFluid = (FnCreateFluid)(uintptr_t)o;
  if (patchSlot(vt, kSceneReleaseFluid, ptr32(&hkReleaseFluid), &o)) g_origReleaseFluid = (FnReleaseFluid)(uintptr_t)o;
  if (patchSlot(vt, kSceneSimulate, ptr32(&hkSimulate), &o)) g_origSimulate = (FnSimulate)(uintptr_t)o;
  if (patchSlot(vt, kSceneFetchResults, ptr32(&hkFetchResults), &o)) g_origFetchResults = (FnFetchResults)(uintptr_t)o;
  const uint32_t listSlots[4] = {kSceneGetNbFluids, kSceneGetFluids, kSceneGetNbCloths, kSceneGetCloths};
  const uint32_t listHooks[4] = {ptr32((void*)&hkSceneList<0>), ptr32((void*)&hkSceneList<1>), ptr32((void*)&hkSceneList<2>),
                                 ptr32((void*)&hkSceneList<3>)};
  for (int i = 0; i < 4; ++i)
    if (patchSlot(vt, listSlots[i], listHooks[i], &o)) g_origSceneList[i] = (FnSceneU32)(uintptr_t)o;
  shim::clothInstallScene(vt);  // createCloth 131 / releaseCloth 132
}

// Logs the frame-cost line for the frames since the last one and starts a new window.
void logFrameCost() {
  const uint32_t n = g_stats.frames;
  if (!n) return;
  logLine("frame cost over %u frames: game frame avg %.2f ms max %.2f | core avg %.2f ms max %.2f (simulate %.2f, "
          "fetchResults %.2f max %.2f) | hosted fluids on the game thread avg %.2f ms max %.2f | %s", n,
          g_stats.wall / n, g_stats.wallMax, g_stats.core / n, g_stats.coreMax, g_stats.coreSim / n,
          g_stats.coreFetch / n, g_stats.coreFetchMax, g_stats.ours / n, g_stats.oursMax, shim::fluidStatsLine());
  char cloth[512];
  shim::oursStatsLine(cloth, sizeof cloth);  // hosted cloths (PXSHIM_CLOTH=ours); "" otherwise
  if (cloth[0]) logLine("frame cost, cloths: %s", cloth);
  g_stats = FrameStats();
}

void logSummary(const char* why) {
  logLine("summary (%s): simulate=%u fetchResults=%u", why, g_simCalls, g_fetchCalls);
  shim::fluidLogSummary(why);
  shim::clothLogSummary(why);
  shim::covLog(why, true);
}

// NxFluidDesc (352 bytes): flags dword 0x4f (NX_FF_HARDWARE 0x10), compartment dword 0x52 (as fluid_ours.cpp).
uint32_t __fastcall hkCreateFluid(void* self, void* edx, const void* desc) {
  (void)edx;
  const uint32_t* d = static_cast<const uint32_t*>(desc);
  if (d && shim::cpuOnly() && ((d[0x4f] & 0x10) || d[0x52])) {
    uint32_t copy[88];
    memcpy(copy, d, sizeof copy);
    logLine("cpu only: fluid desc flags %08X compartment %08X -> software", copy[0x4f], copy[0x52]);
    copy[0x4f] &= ~0x10u;
    copy[0x52] = 0;
    return shim::fluidCreate(self, copy, g_origCreateFluid);
  }
  return shim::fluidCreate(self, desc, g_origCreateFluid);
}

void __fastcall hkReleaseFluid(void* self, void* edx, void* fluid) {
  (void)edx;
  shim::fluidRelease(self, fluid, g_origReleaseFluid);
}

void __fastcall hkSimulate(void* self, void* edx, float elapsed) {
  (void)edx;
  uint32_t n = ++g_simCalls;
  bool log = n <= 3 || n % 100 == 0;
  if (log) logLine("> NxScene::simulate #%u (this=%08X elapsed=%g)", n, ptr32(self), (double)elapsed);
  double t0 = nowMs();
  shim::fluidOnSimulate(self, elapsed);
  shim::oursOnSimulate(self, elapsed);  // hosted cloths (PXSHIM_CLOTH=ours)
  double t1 = nowMs();
  g_origSimulate(self, nullptr, elapsed);
  g_stats.frameOurs += t1 - t0;
  const double coreSim = nowMs() - t1;
  g_stats.frameCore += coreSim;
  g_stats.coreSim += coreSim;
  if (log) logLine("< NxScene::simulate #%u", n);
  if (n % 2000 == 0) logSummary("periodic");
}

bool __fastcall hkFetchResults(void* self, void* edx, uint32_t status, uint32_t block, uint32_t errorState) {
  (void)edx;
  uint32_t n = ++g_fetchCalls;
  bool log = n <= 3 || n % 100 == 0;
  if (log) logLine("> NxScene::fetchResults #%u (this=%08X status=%u block=%08X errorState=%08X)", n, ptr32(self),
                   status, block, errorState);
  double t0 = nowMs();
  bool r = g_origFetchResults(self, nullptr, status, block, errorState);
  double t1 = nowMs();
  if (log) logLine("< NxScene::fetchResults #%u -> %d", n, r ? 1 : 0);
  shim::fluidOnFetched(self, status, r);
  shim::oursOnFetched(self, r);
  g_stats.frameCore += t1 - t0;
  g_stats.frameFetch += t1 - t0;
  g_stats.frameOurs += nowMs() - t1;
  if (r) {  // one frame done
    g_stats.coreFetch += g_stats.frameFetch;
    if (g_stats.frameFetch > g_stats.coreFetchMax) g_stats.coreFetchMax = g_stats.frameFetch;
    g_stats.frameFetch = 0;
    g_stats.core += g_stats.frameCore, g_stats.ours += g_stats.frameOurs;
    if (g_stats.frameCore > g_stats.coreMax) g_stats.coreMax = g_stats.frameCore;
    if (g_stats.frameOurs > g_stats.oursMax) g_stats.oursMax = g_stats.frameOurs;
    g_stats.frameCore = g_stats.frameOurs = 0;
    const double now = nowMs();
    if (g_lastFetchMs > 0) {
      g_stats.wall += now - g_lastFetchMs;
      if (now - g_lastFetchMs > g_stats.wallMax) g_stats.wallMax = now - g_lastFetchMs;
    }
    g_lastFetchMs = now;
    if (++g_stats.frames == 300) logFrameCost();
  }
  return r;
}

uint32_t __fastcall hkCreateScene(void* self, void* edx, const void* desc);
void __fastcall hkReleaseScene(void* self, void* edx, void* scene);

// NxPhysicsSDK vtable: patch createScene/releaseScene once.
uint32_t __fastcall hkNoHardware(void* self, void* edx) {
  (void)self, (void)edx;
  return 0;  // NX_HW_VERSION_NONE / no PPUs
}

void installSdkHooks(uint32_t sdk) {
  if (!sdk) return;
  uint32_t vt = *reinterpret_cast<const uint32_t*>(static_cast<uintptr_t>(sdk));
  if (!vt) return;
  if (g_sdkVt) {
    if (vt != g_sdkVt) logLine("warning: second NxPhysicsSDK vtable %08X ignored (first %08X)", vt, g_sdkVt);
    return;
  }
  g_sdkVt = vt;
  const int ver = coreVersionOf(vt);
  if (!ver || !vtableInModule(vt, kSdkSlotsChecked)) {
    g_passThrough = true;
    logLine("core %s (NxPhysicsSDK vtable %08X): full pass-through (no fluid/cloth hosting)",
            ver ? "vtable unexpected" : "version unknown", vt);
    return;
  }
  logLine("core 2.8.%d", ver % 10);
  uint32_t o = 0;
  if (patchSlot(vt, kSdkCreateScene, ptr32(&hkCreateScene), &o)) g_origCreateScene = (FnCreateScene)(uintptr_t)o;
  if (patchSlot(vt, kSdkReleaseScene, ptr32(&hkReleaseScene), &o)) g_origReleaseScene = (FnReleaseScene)(uintptr_t)o;
  shim::clothInstallSdk(vt);  // createClothMesh 21 / releaseClothMesh 22 (PXSHIM_CLOTH_MESH=0 = off)
  if (shim::cpuOnly()) {
    patchSlot(vt, kSdkGetHWVersion, ptr32((void*)&hkNoHardware), &o);
    patchSlot(vt, kSdkGetNbPPUs, ptr32((void*)&hkNoHardware), &o);
    logLine("cpu only: getHWVersion/getNbPPUs report no PhysX hardware (PXSHIM_ALLOW_GPU=1 to leave them)");
  }
}

uint32_t __fastcall hkCreateScene(void* self, void* edx, const void* desc) {
  (void)edx;
  logLine("> NxPhysicsSDK::createScene(this=%08X desc=%08X)", ptr32(self), ptr32(desc));
  dumpWords("sceneDesc", desc, 0xa0);  // flags, simType, threads, timing: read offline (2.8.0 layout [A])
  uint32_t r = g_origCreateScene(self, nullptr, desc);
  logLine("< NxPhysicsSDK::createScene -> %08X", r);
  if (r) installSceneHooks(r);
  return r;
}

void __fastcall hkReleaseScene(void* self, void* edx, void* scene) {
  (void)edx;
  logLine("> NxPhysicsSDK::releaseScene(this=%08X scene=%08X)", ptr32(self), ptr32(scene));
  logFrameCost();
  logSummary("releaseScene");
  shim::fluidOnSceneRelease(scene);
  shim::oursOnSceneRelease(scene);
  g_origReleaseScene(self, nullptr, scene);
  logLine("< NxPhysicsSDK::releaseScene");
}

}  // namespace

BOOL WINAPI DllMain(HINSTANCE self, DWORD reason, LPVOID) {
  if (reason == DLL_PROCESS_ATTACH) {
    DisableThreadLibraryCalls(self);
    shimInit(self);
  } else if (reason == DLL_PROCESS_DETACH) {
    logSummary("detach");
    shim::covLog("detach", false);
    logLine("detach");
    shim::fluidShutdown();
    shim::gameHooksShutdown();
  }
  return TRUE;
}

// ---- exports (names/ordinals in PhysXLoader.def) --------------------------------------

// NxPhysicsSDKDesc.flags (+0x10 in the game's 7-dword desc and in the 2.8.1 header) |= NX_SDKF_NO_HARDWARE (1): the core
// does not open the PhysX hardware at all [A: public 2.8 SDK semantics, not traced in the 2.8.0 core].
void forceSoftwareSdk(uint32_t desc) {
  if (!desc || !shim::cpuOnly()) return;
  uint32_t* flags = reinterpret_cast<uint32_t*>(static_cast<uintptr_t>(desc) + 0x10);
  if (!(*flags & 1)) logLine("cpu only: NxPhysicsSDKDesc.flags %08X -> %08X (NX_SDKF_NO_HARDWARE)", *flags, *flags | 1);
  *flags |= 1;
}

extern "C" uint32_t __cdecl NxCreatePhysicsSDK(uint32_t version, uint32_t allocator, uint32_t output,
                                               uint32_t desc, uint32_t errorCode) {
  logLine("> NxCreatePhysicsSDK(version=%08X allocator=%08X output=%08X desc=%08X errorCode=%08X)", version,
          allocator, output, desc, errorCode);
  if (!g_CreatePhysicsSDK) return 0;
  forceSoftwareSdk(desc);
  uint32_t r = g_CreatePhysicsSDK(version, allocator, output, desc, errorCode);
  logLine("< NxCreatePhysicsSDK -> %08X", r);
  installSdkHooks(r);
  return r;
}

extern "C" uint32_t __cdecl NxCreatePhysicsSDKWithID(uint32_t version, uint32_t company, uint32_t app,
                                                     uint32_t appVersion, uint32_t appUser, uint32_t allocator,
                                                     uint32_t output, uint32_t desc, uint32_t errorCode) {
  logLine("> NxCreatePhysicsSDKWithID(version=%08X company=%08X \"%.64s\" app=%08X \"%.64s\" appVer=%08X "
          "\"%.64s\" appUser=%08X \"%.64s\" allocator=%08X output=%08X desc=%08X errorCode=%08X)",
          version, company, sx(company), app, sx(app), appVersion, sx(appVersion), appUser, sx(appUser), allocator,
          output, desc, errorCode);
  if (!g_CreatePhysicsSDKWithID) return 0;
  forceSoftwareSdk(desc);
  uint32_t r = g_CreatePhysicsSDKWithID(version, company, app, appVersion, appUser, allocator, output, desc,
                                        errorCode);
  logLine("< NxCreatePhysicsSDKWithID -> %08X", r);
  installSdkHooks(r);
  return r;
}

extern "C" uint32_t __cdecl NxGetCookingLib(uint32_t version) {
  logLine("> NxGetCookingLib(version=%08X)", version);
  if (!g_GetCookingLib) return 0;
  uint32_t r = g_GetCookingLib(version);
  logLine("< NxGetCookingLib -> %08X", r);
  return r;
}

extern "C" uint32_t __cdecl NxGetCookingLibWithID(uint32_t version, uint32_t company, uint32_t app,
                                                  uint32_t appVersion, uint32_t appUser) {
  logLine("> NxGetCookingLibWithID(version=%08X company=%08X \"%.64s\" app=%08X \"%.64s\" appVer=%08X \"%.64s\" "
          "appUser=%08X \"%.64s\")",
          version, company, sx(company), app, sx(app), appVersion, sx(appVersion), appUser, sx(appUser));
  if (!g_GetCookingLibWithID) return 0;
  uint32_t r = g_GetCookingLibWithID(version, company, app, appVersion, appUser);
  logLine("< NxGetCookingLibWithID -> %08X", r);
  return r;
}

extern "C" uint32_t __cdecl NxGetFoundationSDK(void) {
  logLine("> NxGetFoundationSDK()");
  if (!g_GetFoundationSDK) return 0;
  uint32_t r = g_GetFoundationSDK();
  logLine("< NxGetFoundationSDK -> %08X", r);
  return r;
}

extern "C" uint32_t __cdecl NxGetPhysicsSDK(void) {
  logLine("> NxGetPhysicsSDK()");
  if (!g_GetPhysicsSDK) return 0;
  uint32_t r = g_GetPhysicsSDK();
  logLine("< NxGetPhysicsSDK -> %08X", r);
  return r;
}

extern "C" uint32_t __cdecl NxGetPhysicsSDKAllocator(void) {
  logLine("> NxGetPhysicsSDKAllocator()");
  if (!g_GetPhysicsSDKAllocator) return 0;
  uint32_t r = g_GetPhysicsSDKAllocator();
  logLine("< NxGetPhysicsSDKAllocator -> %08X", r);
  return r;
}

extern "C" uint32_t __cdecl NxGetUtilLib(void) {
  logLine("> NxGetUtilLib()");
  if (!g_GetUtilLib) return 0;
  uint32_t r = g_GetUtilLib();
  logLine("< NxGetUtilLib -> %08X", r);
  return r;
}

extern "C" void __cdecl NxReleasePhysicsSDK(uint32_t sdk) {
  logLine("> NxReleasePhysicsSDK(sdk=%08X)", sdk);
  if (!g_ReleasePhysicsSDK) return;
  g_ReleasePhysicsSDK(sdk);
  logLine("< NxReleasePhysicsSDK");
}
