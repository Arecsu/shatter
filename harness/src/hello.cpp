// Phase 0 hello-world harness.
//
// Loads a PhysXLoader.dll at runtime (no import lib), creates the SDK, reports
// version / hardware info, builds a tiny software scene (ground plane + sphere),
// steps it once and shuts down. Emits one JSON object on stdout; human-readable
// progress goes to stderr.
//
// Modes:
//   --abi 2.8.1 (default)  Full test. Requests NX_PHYSICS_SDK_VERSION from the
//                          2.8.1 headers, so the loader picks Engine\v2.8.1 whose
//                          ABI matches the headers we compile against.
//   --abi 2.8.0-probe      Load-only probe of the core the game actually uses.
//                          Requests 0x02080000 with the game's 7-dword SDK
//                          descriptor (observed in MirrorsEdge.exe), then releases.
//                          No virtual calls: the 2.8.1 vtables are known NOT to
//                          match 2.8.0 for NxScene (see docs/abi-2.8.0-vs-2.8.1.md).

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <stdio.h>
#include <string.h>

#include "NxPhysics.h"

typedef NxPhysicsSDK*(NX_CALL_CONV* PFN_NxCreatePhysicsSDK)(NxU32, NxUserAllocator*, NxUserOutputStream*,
                                                           const NxPhysicsSDKDesc&, NxSDKCreateError*);
typedef void(NX_CALL_CONV* PFN_NxReleasePhysicsSDK)(NxPhysicsSDK*);

namespace {

class StderrOutput : public NxUserOutputStream {
 public:
  void reportError(NxErrorCode code, const char* message, const char* file, int line) override {
    bool warn = code == NXE_DB_INFO || code == NXE_DB_WARNING || code == NXE_DB_PRINT;
    fprintf(stderr, "[physx] %s %d: %s (%s:%d)\n", warn ? "warning" : "error", (int)code, message,
            file ? file : "?", line);
    if (warn) warnings++;
    else errors++;
    if (message && strstr(message, "CUDA not available")) cudaUnavailable = true;
  }
  NxAssertResponse reportAssertViolation(const char* message, const char* file, int line) override {
    fprintf(stderr, "[physx] assert: %s (%s:%d)\n", message, file ? file : "?", line);
    errors++;
    return NX_AR_CONTINUE;
  }
  void print(const char* message) override { fprintf(stderr, "[physx] %s\n", message); }
  int errors = 0;
  int warnings = 0;
  bool cudaUnavailable = false;
};

// Game's descriptor as written by MirrorsEdge.exe before NxCreatePhysicsSDK
// (Epic 2.8.0_FC7 layout, 7 dwords; values with ini PhysXGpuHeapSize=32,
// PhysXMeshCacheSize=8). Field names after the 5th are inferred from the ini keys
// that feed them, not from a header.
struct GameSdkDesc280 {
  NxU32 w[7];
};

const char* createErrorName(NxSDKCreateError e) {
  switch (e) {
    case NXCE_NO_ERROR: return "NXCE_NO_ERROR";
    case NXCE_PHYSX_NOT_FOUND: return "NXCE_PHYSX_NOT_FOUND";
    case NXCE_WRONG_VERSION: return "NXCE_WRONG_VERSION";
    case NXCE_DESCRIPTOR_INVALID: return "NXCE_DESCRIPTOR_INVALID";
    case NXCE_CONNECTION_ERROR: return "NXCE_CONNECTION_ERROR";
    case NXCE_RESET_ERROR: return "NXCE_RESET_ERROR";
    case NXCE_IN_USE_ERROR: return "NXCE_IN_USE_ERROR";
    case NXCE_BUNDLE_ERROR: return "NXCE_BUNDLE_ERROR";
    default: return "unknown";
  }
}

void moduleVersion(HMODULE m, char* out, size_t n) {
  char path[MAX_PATH];
  out[0] = 0;
  if (!m || !GetModuleFileNameA(m, path, sizeof(path))) return;
  DWORD h = 0, sz = GetFileVersionInfoSizeA(path, &h);
  if (!sz) { snprintf(out, n, "%s", path); return; }
  static char buf[16384];
  if (sz > sizeof(buf) || !GetFileVersionInfoA(path, 0, sz, buf)) return;
  VS_FIXEDFILEINFO* fi = nullptr;
  UINT len = 0;
  if (VerQueryValueA(buf, "\\", (void**)&fi, &len) && fi)
    snprintf(out, n, "%u.%u.%u.%u", HIWORD(fi->dwFileVersionMS), LOWORD(fi->dwFileVersionMS),
             HIWORD(fi->dwFileVersionLS), LOWORD(fi->dwFileVersionLS));
}

// Escape-free JSON string (paths only contain printable ASCII; backslashes doubled).
void jsonStr(const char* s) {
  putchar('"');
  for (; *s; ++s) {
    if (*s == '\\' || *s == '"') putchar('\\');
    putchar(*s);
  }
  putchar('"');
}

void printCoreModule() {
  HMODULE core = GetModuleHandleA("PhysXCore.dll");
  char path[MAX_PATH] = "", ver[64] = "";
  if (core) GetModuleFileNameA(core, path, sizeof(path));
  moduleVersion(core, ver, sizeof(ver));
  printf("\"core_path\":");
  jsonStr(path);
  printf(",\"core_version\":");
  jsonStr(ver);
}

int run281(PFN_NxCreatePhysicsSDK create, PFN_NxReleasePhysicsSDK release) {
  StderrOutput out;
  NxSDKCreateError err = NXCE_NO_ERROR;
  NxPhysicsSDKDesc desc;  // defaults; hardware allowed so getHWVersion is meaningful
  NxPhysicsSDK* sdk = create(NX_PHYSICS_SDK_VERSION, nullptr, &out, desc, &err);
  printf("{\"abi\":\"2.8.1\",\"requested_version\":\"0x%08x\",\"create_error\":\"%s\",", NX_PHYSICS_SDK_VERSION,
         createErrorName(err));
  printCoreModule();
  if (!sdk) {
    printf(",\"ok\":false}\n");
    return 2;
  }
  NxU32 apiRev = 0, descRev = 0, branch = 0;
  NxU32 internal = sdk->getInternalVersion(apiRev, descRev, branch);
  NxHWVersion hw = sdk->getHWVersion();
  NxU32 ppus = sdk->getNbPPUs();
  printf(",\"internal_version\":\"0x%08x\",\"api_rev\":%u,\"desc_rev\":%u,\"branch\":%u", internal, apiRev, descRev,
         branch);
  printf(",\"hw_version\":%d,\"nb_ppus\":%u", (int)hw, ppus);
  fprintf(stderr, "SDK created: internal=0x%08x hw=%d ppus=%u\n", internal, (int)hw, ppus);

  NxSceneDesc sd;
  sd.gravity = NxVec3(0, -9.81f, 0);
  sd.simType = NX_SIMULATION_SW;
  NxScene* scene = sdk->createScene(sd);
  bool stepped = false;
  NxVec3 p0(0, 0, 0), p1(0, 0, 0);
  if (scene) {
    // Static ground plane
    NxPlaneShapeDesc plane;
    NxActorDesc ground;
    ground.shapes.pushBack(&plane);
    scene->createActor(ground);
    // Dynamic sphere 10 m up
    NxSphereShapeDesc sphere;
    sphere.radius = 0.5f;
    NxBodyDesc body;
    NxActorDesc ad;
    ad.shapes.pushBack(&sphere);
    ad.body = &body;
    ad.density = 1.0f;
    ad.globalPose.t = NxVec3(0, 10, 0);
    NxActor* a = scene->createActor(ad);
    if (a) p0 = a->getGlobalPosition();
    scene->simulate(1.0f / 60.0f);
    scene->flushStream();
    stepped = scene->fetchResults(NX_RIGID_BODY_FINISHED, true);
    if (a) p1 = a->getGlobalPosition();
    printf(",\"scene_sim_type\":%d", (int)scene->getSimType());
    sdk->releaseScene(*scene);
  }
  printf(",\"scene_created\":%s,\"stepped\":%s", scene ? "true" : "false", stepped ? "true" : "false");
  printf(",\"sphere_y_before\":%.6f,\"sphere_y_after\":%.6f", p0.y, p1.y);
  release(sdk);
  bool ok = scene && stepped && p1.y < p0.y && out.errors == 0;
  printf(",\"sdk_errors\":%d,\"sdk_warnings\":%d,\"cuda_unavailable_reported\":%s,\"ok\":%s}\n", out.errors,
         out.warnings, out.cudaUnavailable ? "true" : "false", ok ? "true" : "false");
  return ok ? 0 : 3;
}

int probe280(PFN_NxCreatePhysicsSDK create, PFN_NxReleasePhysicsSDK release) {
  // hwPageSize, hwPageMax, hwConvexMax, ?, ?, (PhysXGpuHeapSize), (PhysXMeshCacheSize)
  GameSdkDesc280 d = {{0x10000, 0x100, 0x800, 0x80000000u, 0, 32, 8}};
  NxSDKCreateError err = NXCE_NO_ERROR;
  NxPhysicsSDK* sdk =
      create(0x02080000, nullptr, nullptr, *reinterpret_cast<const NxPhysicsSDKDesc*>(&d), &err);
  printf("{\"abi\":\"2.8.0-probe\",\"requested_version\":\"0x02080000\",\"create_error\":\"%s\",",
         createErrorName(err));
  printCoreModule();
  printf(",\"sdk_created\":%s", sdk ? "true" : "false");
  if (sdk) release(sdk);
  printf(",\"ok\":%s}\n", sdk ? "true" : "false");
  return sdk ? 0 : 2;
}

}  // namespace

int main(int argc, char** argv) {
  const char* loaderPath = "PhysXLoader.dll";
  const char* abi = "2.8.1";
  for (int i = 1; i < argc; ++i) {
    if (!strcmp(argv[i], "--loader") && i + 1 < argc) loaderPath = argv[++i];
    else if (!strcmp(argv[i], "--abi") && i + 1 < argc) abi = argv[++i];
  }
  setvbuf(stdout, nullptr, _IONBF, 0);

  HMODULE loader = LoadLibraryA(loaderPath);
  if (!loader) {
    printf("{\"ok\":false,\"error\":\"LoadLibrary failed\",\"gle\":%lu}\n", GetLastError());
    return 1;
  }
  char lv[64];
  moduleVersion(loader, lv, sizeof(lv));
  fprintf(stderr, "loader %s version %s\n", loaderPath, lv);
  auto create = (PFN_NxCreatePhysicsSDK)GetProcAddress(loader, "NxCreatePhysicsSDK");
  auto release = (PFN_NxReleasePhysicsSDK)GetProcAddress(loader, "NxReleasePhysicsSDK");
  if (!create || !release) {
    printf("{\"ok\":false,\"error\":\"missing loader exports\"}\n");
    return 1;
  }
  int rc = !strcmp(abi, "2.8.0-probe") ? probe280(create, release) : run281(create, release);
  FreeLibrary(loader);
  fprintf(stderr, "loader version %s, exit %d\n", lv, rc);
  return rc;
}
