// Shared helpers for oracle harness binaries: load PhysXLoader at runtime, SDK output stream,
// timing. Header-only; included by each harness .cpp.
#pragma once

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <stdio.h>
#include <string.h>

#include "NxPhysics.h"

typedef NxPhysicsSDK*(NX_CALL_CONV* PFN_NxCreatePhysicsSDK)(NxU32, NxUserAllocator*, NxUserOutputStream*,
                                                           const NxPhysicsSDKDesc&, NxSDKCreateError*);
typedef void(NX_CALL_CONV* PFN_NxReleasePhysicsSDK)(NxPhysicsSDK*);

class StderrOutput : public NxUserOutputStream {
 public:
  void reportError(NxErrorCode code, const char* message, const char* file, int line) override {
    bool warn = code == NXE_DB_INFO || code == NXE_DB_WARNING || code == NXE_DB_PRINT;
    if (!quietWarnings || !warn)
      fprintf(stderr, "[physx] %s %d: %s (%s:%d)\n", warn ? "warning" : "error", (int)code, message,
              file ? file : "?", line);
    if (warn) warnings++;
    else errors++;
  }
  NxAssertResponse reportAssertViolation(const char* message, const char* file, int line) override {
    fprintf(stderr, "[physx] assert: %s (%s:%d)\n", message, file ? file : "?", line);
    errors++;
    return NX_AR_CONTINUE;
  }
  void print(const char* message) override { fprintf(stderr, "[physx] %s\n", message); }
  int errors = 0;
  int warnings = 0;
  bool quietWarnings = true;
};

struct LoadedSdk {
  HMODULE loader = nullptr;
  PFN_NxCreatePhysicsSDK create = nullptr;
  PFN_NxReleasePhysicsSDK release = nullptr;
  NxPhysicsSDK* sdk = nullptr;
  NxSDKCreateError err = NXCE_NO_ERROR;

  bool open(const char* loaderPath, NxUserOutputStream* out) {
    loader = LoadLibraryA(loaderPath);
    if (!loader) return false;
    create = (PFN_NxCreatePhysicsSDK)GetProcAddress(loader, "NxCreatePhysicsSDK");
    release = (PFN_NxReleasePhysicsSDK)GetProcAddress(loader, "NxReleasePhysicsSDK");
    if (!create || !release) return false;
    NxPhysicsSDKDesc desc;
    desc.flags |= NX_SDKF_NO_HARDWARE;
    sdk = create(NX_PHYSICS_SDK_VERSION, nullptr, out, desc, &err);
    return sdk != nullptr;
  }
  void close() {
    if (sdk) release(sdk);
    sdk = nullptr;
    if (loader) FreeLibrary(loader);
    loader = nullptr;
  }
};

inline double nowSeconds() {
  static LARGE_INTEGER freq = [] {
    LARGE_INTEGER f;
    QueryPerformanceFrequency(&f);
    return f;
  }();
  LARGE_INTEGER t;
  QueryPerformanceCounter(&t);
  return (double)t.QuadPart / (double)freq.QuadPart;
}
