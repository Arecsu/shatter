// Internals shared by fluid_wrap.cpp (wrapper, trace, dispatch) and fluid_ours.cpp (hosted fluid).
#pragma once

#include <stdint.h>

#include <vector>

namespace shim {

struct HostedFluid;

// NxFluidDesc (352 bytes) dword indices, docs/re-notes-fluid-mesh.md 3.1.
const uint32_t kDescDwords = 0x58;
const uint32_t kDescWriteData = 0x35, kDescDeletionIds = 0x45, kDescCreationIds = 0x49, kDescPackets = 0x4d,
               kDescUserData = 0x50, kDescCollisionGroup = 0x2f, kDescGroupsMask = 0x30, kDescEmittersBegin = 0x54, kDescEmittersEnd = 0x55;

// NxParticleData (16 dwords): numParticlesPtr, 7 buffers (pos vel life density id flag normal), 7 strides, name.
struct ParticleData {
  uint32_t numPtr;
  uint32_t buf[7];
  uint32_t stride[7];
  uint32_t name;
};
enum { kPos, kVel, kLife, kDensity, kId, kFlag, kNormal };
// NxParticleIdData (4 dwords) and NxFluidPacketData (2 dwords: the buffer comes first, then the count pointer).
struct IdData {
  uint32_t numPtr, buf, stride, name;
};
struct PacketData {
  uint32_t buf, numPtr;
};
// NxParticleUpdateData (8 dwords).
struct UpdateData {
  uint32_t forceMode, num, bufForce, bufFlag, bufId, strideForce, strideFlag, strideId;
};

const uint32_t kMagic = 0x46535850;  // "PXSF"

// What the game holds instead of an NxFluid. NxFluid's only data member is `void* userData` right after
// the vtable pointer, so it stays at +4.
struct ShimFluid {
  const uint32_t* vtbl;
  uint32_t userData;
  uint32_t magic;
  uint32_t real;  // the core's NxFluid
  uint32_t scene;
  uint32_t index;  // creation order, used as the trace's fluid id
  ParticleData write;
  IdData deletionIds, creationIds;
  PacketData packets;
  HostedFluid* hosted;  // non-null: our solver runs this fluid (real == 0), fluid_ours.cpp
};


// Trace hooks (fluid_wrap.cpp); every one is a no-op below its trace level.
void traceAdd(const ShimFluid* f, const ParticleData* pd, uint32_t appendIds, uint32_t returned);
void traceUpdate(const ShimFluid* f, const UpdateData* ud);
void traceRemoveAll(const ShimFluid* f);
void traceBounds(const ShimFluid* f, const float* bounds6);
// Logs a call of a fluid slot (first calls, then powers of two).
void countFluidCall(uint32_t slot, const ShimFluid* f, const char* fmt, ...);

// Hosted fluids (fluid_ours.cpp).
bool hostWants(const uint32_t* desc);  // PXSHIM_FLUID decides which descriptors we run ourselves
HostedFluid* hostCreate(ShimFluid* f, const uint32_t* desc);
void hostDestroy(ShimFluid* f);
void hostSimulateScene(uint32_t scene, const std::vector<ShimFluid*>& fluids, float elapsed);  // the scene's hosted fluids; its simulate(elapsed) is about to run
void hostFetched(ShimFluid* f);
// Waits until f's step (if running on the fluid thread) is done before a call that changes the solver or the filter.
// entry names the call for the frame-cost log (forced waits per entry point).
void hostSync(ShimFluid* f, const char* entry);
void hostSceneReleased(uint32_t scene);
void hostCounts(const ShimFluid* f, uint32_t* particles, uint32_t* shapes);  // adds this fluid's numbers
void hostGeometryStats(char* buf, size_t size);  // geometry refresh costs since the last call, then resets them
extern const uint32_t g_hostVt[];

}  // namespace shim
