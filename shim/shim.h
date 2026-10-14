// Shared helpers of the PhysXLoader.dll shim (PhysXLoader.cpp: exports and SDK/scene hooks;
// fluid_wrap.cpp: the NxFluid stand-in). No NVIDIA headers: layouts are written out as dword offsets.
#pragma once

#include <stddef.h>
#include <stdint.h>

namespace shim {

// Directory of the shim DLL, with a trailing backslash (set on DLL attach).
extern char g_dir[];

void logLine(const char* fmt, ...);
// Hex dump of a descriptor, 8 dwords per line. Returns false if the memory isn't readable.
bool dumpWords(const char* tag, const void* p, size_t bytes);
// Overwrite one vtable slot, returning the previous value in *orig.
bool patchSlot(uint32_t vt, uint32_t slot, uint32_t value, uint32_t* orig);

inline uint32_t ptr32(const void* p) { return static_cast<uint32_t>(reinterpret_cast<uintptr_t>(p)); }
template <class T>
inline T* fromPtr32(uint32_t p) {
  return reinterpret_cast<T*>(static_cast<uintptr_t>(p));
}

// Switch value: the environment variable, else its shatter.ini setting (next to the DLL), else "" (the default in code).
const char* env(const char* name);
// Debug output (PXSHIM_DEBUG=1 / debug_log = on): the log file, descriptor dumps, census, slow-step dumps, call counting.
bool debug();
// CPU only (default): the game is told there is no PhysX hardware, the SDK is created with NX_SDKF_NO_HARDWARE, and
// a hardware flag or compartment in a fluid/cloth desc is dropped. PXSHIM_ALLOW_GPU=1 leaves all of it to the core.
bool cpuOnly();
// Hooks into MirrorsEdge.exe's own classes (game_hooks.cpp, known exe build only): the slide steering fix (default off,
// PXSHIM_SLIDEFIX=1 / slide_steering_fix = on) and the developer movement/aim logger (PXSHIM_MOVELOG=1, movelog.bin).
void gameHooksInit();
void gameHooksShutdown();
// Step-rate independence of the hosted cloths and fluids (ClothParams/FluidParams::refDt): the step size their
// coefficients are taken at. The game steps once per frame (scene timing = the frame's time, substeps of at most
// 0.02 s), so without it cloth stiffness, damping and friction would change with the frame rate. Default 1/62 s, the
// game's default frame cap (MaxSmoothedFrameRate=62), so the default look is unchanged. PXSHIM_REF_HZ=n sets 1/n,
// PXSHIM_REF_HZ=0 turns it off (the original's per-step behaviour).
float stepRefDt();
// Cloth's own reference (ClothParams::refDt), default 1/62 s like the fluids. The original's cloth does not keep its
// look across frame rates (the same 40-hit banner: 340-424 vertices at 90 Hz steps, 280-301 at 220 Hz; uncapped on a
// fast machine banners barely tear), so a reference is a choice: 62, the game's own frame cap, is what its scripted
// bullet effects were made for (95 and 110 were tried in game and felt too stiff). PXSHIM_CLOTH_REF_HZ=n sets 1/n,
// 0 (or PXSHIM_REF_HZ=0) turns it off.
float clothRefDt();
// ---- loader_core.cpp: our own copy of the original loader (used when no PhysXLoader_orig.dll sits next to the DLL) ----
struct LoaderFns {
  uint32_t(__cdecl* createPhysicsSDK)(uint32_t, uint32_t, uint32_t, uint32_t, uint32_t);
  uint32_t(__cdecl* createPhysicsSDKWithID)(uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t,
                                            uint32_t, uint32_t);
  uint32_t(__cdecl* getCookingLib)(uint32_t);
  uint32_t(__cdecl* getCookingLibWithID)(uint32_t, uint32_t, uint32_t, uint32_t, uint32_t);
  uint32_t(__cdecl* getFoundationSDK)(void);
  uint32_t(__cdecl* getPhysicsSDK)(void);
  uint32_t(__cdecl* getPhysicsSDKAllocator)(void);
  uint32_t(__cdecl* getUtilLib)(void);
  void(__cdecl* releasePhysicsSDK)(uint32_t);
};
LoaderFns ownLoader();
// Core version of the module that owns an address, from its version resource: 280 (2.8.0.45 or 2.8.0.7, the 2.8.0
// builds checked), 281 (2.8.1.x), else 0 (the caller falls back to pass-through).
int coreVersionOf(uint32_t addr);
// True if the first `slots` entries of a vtable all point into the image of the module that owns the vtable.
bool vtableInModule(uint32_t vt, uint32_t slots);

// Runs fn(ctx, 0..n-1) on the shim's worker pool (fluid_ours.cpp; serial when PXSHIM_THREADS=1).
void shimPoolRun(size_t n, void (*fn)(void*, size_t), void* ctx);

// ---- fluid_wrap.cpp ----
// NxScene::createFluid / releaseFluid / fetchResults go through these; `orig*` are the core's functions.
typedef uint32_t(__fastcall* FnCreateFluid)(void* self, void* edx, const void* desc);
typedef void(__fastcall* FnReleaseFluid)(void* self, void* edx, void* fluid);
void fluidInit();
uint32_t fluidCreate(void* scene, const void* desc, FnCreateFluid orig);
void fluidRelease(void* scene, void* fluid, FnReleaseFluid orig);
void fluidOnSceneRelease(void* scene);
void fluidOnSimulate(void* scene, float elapsed);
void fluidOnFetched(void* scene, uint32_t status, bool ok);
void fluidLogSummary(const char* why);
void fluidShutdown();
const char* fluidStatsLine();  // live fluids, particles, shapes near them (for the frame-cost log)

// ---- cloth_wrap.cpp (logging pass-through for NxCloth; PXSHIM_CLOTH=0 disables) ----
void clothInit();                       // reads PXSHIM_CLOTH (call once on attach)
void clothInstallScene(uint32_t sceneVt);  // NxScene createCloth 131 / releaseCloth 132
void clothInstallSdk(uint32_t sdkVt);      // NxPhysicsSDK createClothMesh 21 / releaseClothMesh 22 (version-gated)
void clothLogSummary(const char* why);     // per-slot NxCloth call counts

}  // namespace shim
