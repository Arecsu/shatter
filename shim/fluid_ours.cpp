// Hosted NxFluid (shim step 4b): our solver (solver/fluid) runs the fluid instead of the core.
//
// PXSHIM_FLUID selects which fluids are hosted: unset or "ours-all" (default) = every fluid, "ours" =
// NO_PARTICLE_INTERACTION fluids only (the game's collide-only debris/glass/dust), "off" (or "0") = pass-through
// wrapper only (the original core runs them, on whatever hardware it picks).
//
// The game holds a ShimFluid whose vtable is g_hostVt: all 81 NxFluid slots are implemented here with the
// semantics measured on the original (docs/re-notes-fluid-api.md). Descriptor values are kept as the raw
// 352-byte NxFluidDesc so getters, setters and saveToDesc round-trip; setters that would change the running
// solver's parameters are stored and logged but not applied yet.
//
// Per frame: NxScene::simulate(elapsed) reads the scene's geometry (scene_read.cpp: cached static shapes, dynamic
// actors read each frame) and steps every hosted fluid of that scene (scene timing mirrored: fixed substeps of
// maxTimestep, up to maxIter). By default the steps run on a background thread while the core simulates the rigid
// bodies, and fetchResults waits for them before it writes the user's particle, id and packet buffers
// (PXSHIM_FLUID_ASYNC=0: inside simulate, as before). A scene with a two-way fluid steps inside simulate, so the
// impulses reach the bodies before their own step. Collision runs on PXSHIM_THREADS threads (results do not
// depend on the count). Any call on a hosted fluid while its step runs waits for the step first.

#include <windows.h>

#include <float.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include <algorithm>
#include <condition_variable>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "../solver/fastfloor.h"
#include "../solver/fluid/fluid.h"
#include "../solver/fluid/step_dump.h"
#include "../solver/fluid/thread_pool.h"
#include "coverage.h"
#include "fluid_internal.h"
#include "scene_read.h"
#include "shim.h"

namespace shim {

using pxr::Vec3;

// Packet cell of a position: cubes of packetSizeMultiplier * kernel radius.
struct CellKey {
  int32_t x, y, z;
  bool operator<(const CellKey& o) const { return x != o.x ? x < o.x : y != o.y ? y < o.y : z < o.z; }
  bool operator==(const CellKey& o) const { return x == o.x && y == o.y && z == o.z; }
};

struct HostedFluid {
  uint32_t desc[kDescDwords];  // the descriptor as created, updated by setters
  pxr::FluidParams params;
  pxr::Fluid* sim = nullptr;
  float accum = 0;                 // scene-timing accumulator (fixed stepping)
  float simTime = 0;               // simulated time of the last simulate (force scale); 0 before the first one
  std::vector<std::pair<uint32_t, Vec3>> pendingVel;  // force entries since the last simulate (velocity changes)
  std::vector<uint32_t> deleted;   // ids deleted since the last fetch
  uint32_t createdThisFrame = 0;   // creation id buffer fill (appendIds)
  FluidFilterDesc filter;
  std::vector<const pxr::StaticShape*> nearShapes;  // shapes selected for the current step
  std::vector<pxr::DynamicShape> nearDynamic;
  bool dumpNext = false;  // write the next step's inputs (slow-step dump)
  bool inJob = false;     // part of the scene job that may be running on the fluid thread
  float boundsAtStep[6] = {0, 0, 0, 0, 0, 0};  // world bounds when the job started (= at the last fetch); read while inJob
  std::vector<std::pair<CellKey, uint32_t>> order;  // packet order after the last step (computeOrder)
  bool orderValid = false;
};

namespace {

// Descriptor dword indices (docs/re-notes-fluid-mesh.md 3.1).
enum : uint32_t {
  dMaxParticles = 0x10, dReserve, dRestPPM, dRestDensity, dKRM, dMLM, dCDM, dPacketSize, dStiffness, dViscosity,
  dSurfaceTension, dDamping, dFadeIn, dExtAccel, dProjPlane = 0x20, dRestS = 0x24, dDynFrS, dStatFrS, dAttrS, dRestD,
  dDynFrD, dStatFrD, dAttrD, dResponse, dSimMethod, dCollMethod, dGroup, dGroupsMask, dForceFieldMaterial = 0x34,
  dFlags = 0x4f, dName = 0x51, dCompartment = 0x52
};
const uint32_t kFfDisableGravity = 0x2, kFfCollisionTwoWay = 0x4, kFfHardware = 0x10, kFfPriorityMode = 0x20;
const uint32_t kCollisionStatic = 1, kCollisionDynamic = 2;  // NxFluidCollisionMethod
const uint32_t kMethodNoInteraction = 2, kMethodMixed = 4;
const uint32_t kFpDelete = 1;
const uint32_t kMaxAddPerStep = 4096;  // header: "The limit for adding particles is 4096 new particles per timestep" [A for SW]

float descF(const HostedFluid* h, uint32_t i) {
  float v;
  memcpy(&v, &h->desc[i], 4);
  return v;
}
void setDescF(HostedFluid* h, uint32_t i, float v) { memcpy(&h->desc[i], &v, 4); }

pxr::FluidParams paramsFromDesc(const uint32_t* d) {
  pxr::FluidParams p;
  auto f = [d](uint32_t i) {
    float v;
    memcpy(&v, &d[i], 4);
    return v;
  };
  p.maxParticles = d[dMaxParticles];
  p.numReserveParticles = d[dReserve];
  p.restParticlesPerMeter = f(dRestPPM);
  p.restDensity = f(dRestDensity);
  p.kernelRadiusMultiplier = f(dKRM);
  p.motionLimitMultiplier = f(dMLM);
  p.collisionDistanceMultiplier = f(dCDM);
  p.packetSizeMultiplier = d[dPacketSize];
  p.stiffness = f(dStiffness);
  p.viscosity = f(dViscosity);
  p.damping = f(dDamping);
  p.externalAcceleration = Vec3(f(dExtAccel), f(dExtAccel + 1), f(dExtAccel + 2));
  p.restitutionStatic = f(dRestS);
  p.frictionStatic = f(dDynFrS);
  p.restitutionDynamic = f(dRestD);
  p.frictionDynamic = f(dDynFrD);
  p.collisionResponseCoefficient = f(dResponse);  // two-way impulses only (the asset's CollisionResponseCoefficient)
  p.method = d[dSimMethod] == kMethodNoInteraction ? pxr::SimMethod::NoInteraction
             : d[dSimMethod] == kMethodMixed       ? pxr::SimMethod::Mixed
                                                   : pxr::SimMethod::SPH;
  p.disableGravity = (d[dFlags] & kFfDisableGravity) != 0;
  p.priorityMode = (d[dFlags] & kFfPriorityMode) != 0;
  p.twoWay = (d[dFlags] & kFfCollisionTwoWay) != 0;
  // The original's 2-step shape latency for new particles: kept for SPH/mixed (the game's smoke was tuned with it,
  // docs/sph-game-validation.md: 3 of 14 configs differ with it, 11 without), dropped for collide-only debris (it lets
  // fresh fast particles pass through walls). PXSHIM_FLUID_LATENCY=n forces n for every fluid.
  const char* lat = env("PXSHIM_FLUID_LATENCY");
  p.newParticleShapeLatency = lat[0] ? (uint32_t)atoi(lat) : (p.method == pxr::SimMethod::NoInteraction ? 0u : 2u);
  p.refDt = stepRefDt();
  return p;
}

// Coverage counters of the create-time fields (docs/audit/GAPS.md rows 1, 15, 22).
void coverDesc(const uint32_t* d) {
  float resp, sfs, sfd;
  memcpy(&resp, &d[dResponse], 4);
  memcpy(&sfs, &d[dStatFrS], 4);
  memcpy(&sfd, &d[dStatFrD], 4);
  const bool twoWay = (d[dFlags] & kFfCollisionTwoWay) != 0;
  if (twoWay) cov(kCovFluidTwoWayCreated);
  if (resp != 0.2f) cov(twoWay ? kCovFluidResponseTwoWay : kCovFluidResponseNonDefault);
  if (sfs != 0.f || sfd != 0.f) cov(kCovFluidStaticFriction);
  if (d[dFlags] & kFfHardware) cov(kCovFluidHardware);
}

HostedFluid* H(ShimFluid* f) { return f->hosted; }

template <class T>
T* at(uint32_t buf, uint32_t stride, uint32_t i) {
  return fromPtr32<T>(buf + i * stride);
}

// Creation id buffer: ids of this addParticles call (appended with appendIds), reset at fetch.
void writeCreationIds(ShimFluid* f, const uint32_t* ids, uint32_t n, bool append) {
  const IdData& c = f->creationIds;
  if (!c.numPtr) return;
  HostedFluid* h = H(f);
  uint32_t base = append ? h->createdThisFrame : 0;
  if (c.buf)
    for (uint32_t i = 0; i < n; ++i) *at<uint32_t>(c.buf, c.stride, base + i) = ids[i];
  h->createdThisFrame = base + n;
  *fromPtr32<uint32_t>(c.numPtr) = h->createdThisFrame;
}

// addParticles also appends the new particles to the user's write buffer right away (the original's count
// grows by the number added before any simulate [V, api_life trace]); the next fetch rewrites the whole buffer.
void appendToWriteBuffer(ShimFluid* f, const Vec3* pos, const Vec3* vel, const float* life, const uint32_t* ids,
                         uint32_t n) {
  const ParticleData& w = f->write;
  if (!w.numPtr) return;
  uint32_t& count = *fromPtr32<uint32_t>(w.numPtr);
  const float zero3[3] = {0, 0, 0};
  for (uint32_t i = 0; i < n; ++i) {
    const uint32_t k = count + i;
    if (w.buf[kPos]) memcpy(at<float>(w.buf[kPos], w.stride[kPos], k), &pos[i], 12);
    if (w.buf[kVel]) memcpy(at<float>(w.buf[kVel], w.stride[kVel], k), &vel[i], 12);
    if (w.buf[kLife]) *at<float>(w.buf[kLife], w.stride[kLife], k) = life[i];
    if (w.buf[kDensity]) *at<float>(w.buf[kDensity], w.stride[kDensity], k) = 0;
    if (w.buf[kId]) *at<uint32_t>(w.buf[kId], w.stride[kId], k) = ids[i];
    if (w.buf[kFlag]) *at<uint32_t>(w.buf[kFlag], w.stride[kFlag], k) = 0;
    if (w.buf[kNormal]) memcpy(at<float>(w.buf[kNormal], w.stride[kNormal], k), zero3, 12);
  }
  count += n;
}


// Packet order of the particles (by packet cell, stable): computed after the step (on the fluid thread when the step
// runs there) and reused by the fetch unless particles were added since.
void computeOrder(HostedFluid* h) {
  const pxr::Fluid& s = *h->sim;
  const uint32_t n = (uint32_t)s.size();
  const float packetSize = h->params.packetSizeMultiplier * h->params.kernelRadiusMultiplier /
                           h->params.restParticlesPerMeter;
  const float inv = packetSize > 0 ? 1.0f / packetSize : 0.0f;
  std::vector<std::pair<CellKey, uint32_t>>& order = h->order;
  order.resize(n);
  for (uint32_t i = 0; i < n; ++i) {
    const Vec3& p = s.positions()[i];
    order[i] = {{(int32_t)pxm::floorExact(p.x * inv), (int32_t)pxm::floorExact(p.y * inv), (int32_t)pxm::floorExact(p.z * inv)}, i};
  }
  std::stable_sort(order.begin(), order.end(),
                   [](const std::pair<CellKey, uint32_t>& a, const std::pair<CellKey, uint32_t>& b) { return a.first < b.first; });
  h->orderValid = true;
}

// Writes particles in packet order plus the packet table, the deletion ids and resets the creation ids.
void writeOutputs(ShimFluid* f) {
  HostedFluid* h = H(f);
  const pxr::Fluid& s = *h->sim;
  const uint32_t n = (uint32_t)s.size();
  if (!h->orderValid || h->order.size() != n) computeOrder(h);
  const std::vector<std::pair<CellKey, uint32_t>>& order = h->order;

  const ParticleData& w = f->write;
  for (uint32_t k = 0; k < n; ++k) {
    const uint32_t i = order[k].second;
    if (w.buf[kPos]) memcpy(at<float>(w.buf[kPos], w.stride[kPos], k), &s.positions()[i], 12);
    if (w.buf[kVel]) memcpy(at<float>(w.buf[kVel], w.stride[kVel], k), &s.velocities()[i], 12);
    if (w.buf[kLife]) *at<float>(w.buf[kLife], w.stride[kLife], k) = s.lifetimes()[i];
    if (w.buf[kDensity]) *at<float>(w.buf[kDensity], w.stride[kDensity], k) = s.densities()[i];
    if (w.buf[kId]) *at<uint32_t>(w.buf[kId], w.stride[kId], k) = s.ids()[i];
    if (w.buf[kFlag]) *at<uint32_t>(w.buf[kFlag], w.stride[kFlag], k) = s.flags()[i];
    if (w.buf[kNormal]) memcpy(at<float>(w.buf[kNormal], w.stride[kNormal], k), &s.normals()[i], 12);
  }
  if (w.numPtr) *fromPtr32<uint32_t>(w.numPtr) = n;

  if (f->packets.numPtr) {
    uint32_t np = 0;
    for (uint32_t k = 0; k < n;) {
      uint32_t e = k;
      float b[6] = {FLT_MAX, FLT_MAX, FLT_MAX, -FLT_MAX, -FLT_MAX, -FLT_MAX};
      for (; e < n && order[e].first == order[k].first; ++e) {
        const Vec3& p = s.positions()[order[e].second];
        b[0] = std::min(b[0], p.x), b[1] = std::min(b[1], p.y), b[2] = std::min(b[2], p.z);
        b[3] = std::max(b[3], p.x), b[4] = std::max(b[4], p.y), b[5] = std::max(b[5], p.z);
      }
      if (f->packets.buf) {
        uint32_t* rec = at<uint32_t>(f->packets.buf, 36, np);
        memcpy(rec, b, 24);
        const CellKey& c = order[k].first;
        rec[6] = k;
        rec[7] = e - k;
        rec[8] = ((uint32_t)c.x * 73856093u ^ (uint32_t)c.y * 19349663u ^ (uint32_t)c.z * 83492791u) & 0x3ff;  // [A]
      }
      ++np;
      k = e;
    }
    *fromPtr32<uint32_t>(f->packets.numPtr) = np;
  }

  const IdData& del = f->deletionIds;
  if (del.numPtr) {
    if (del.buf)
      for (size_t i = 0; i < h->deleted.size(); ++i) *at<uint32_t>(del.buf, del.stride, (uint32_t)i) = h->deleted[i];
    *fromPtr32<uint32_t>(del.numPtr) = (uint32_t)h->deleted.size();
  }
  h->deleted.clear();
  if (f->creationIds.numPtr) *fromPtr32<uint32_t>(f->creationIds.numPtr) = 0;
  h->createdThisFrame = 0;
}

// ---- the 81 slots ---------------------------------------------------------------------------
// __fastcall(this, edx_unused, args...) == __thiscall. Class return values: hidden pointer first, returned in EAX.
typedef ShimFluid S;

void* __fastcall hDestructor(S* f, void*, uint32_t) {
  logLine("hosted fluid %u: destructor called through the vtable (ignored)", f->index);
  return f;
}
uint32_t __fastcall hCreateEmitter(S* f, void*, const void*) {
  cov(kCovFluidEmitterCreate);
  logLine("hosted fluid %u: createEmitter not supported, returning null", f->index);
  return 0;
}
void __fastcall hReleaseEmitter(S*, void*, void*) {}
uint32_t __fastcall hGetNbEmitters(S*, void*) { return 0; }
uint32_t __fastcall hGetEmitters(S*, void*) { return 0; }

uint32_t __fastcall hAddParticles(S* f, void*, const ParticleData* pd, uint32_t appendIds) {
  hostSync(f, "addParticles");
  HostedFluid* h = H(f);
  uint32_t n = pd && pd->numPtr ? *fromPtr32<const uint32_t>(pd->numPtr) : 0;
  if (n > kMaxAddPerStep) cov(kCovFluidAddOver4096);
  n = std::min(n, kMaxAddPerStep);
  std::vector<Vec3> pos(n), vel(n);
  std::vector<float> life(n, 0.0f);
  for (uint32_t i = 0; i < n; ++i) {
    if (pd->buf[kPos]) memcpy(&pos[i], at<float>(pd->buf[kPos], pd->stride[kPos], i), 12);
    if (pd->buf[kVel]) memcpy(&vel[i], at<float>(pd->buf[kVel], pd->stride[kVel], i), 12);
    if (pd->buf[kLife]) life[i] = *at<float>(pd->buf[kLife], pd->stride[kLife], i);
  }
  std::vector<uint32_t> ids(n);
  const uint32_t r = n ? h->sim->addParticles(pos.data(), vel.data(), life.data(), n, ids.data()) : 0;
  if (r) h->orderValid = false;
  writeCreationIds(f, ids.data(), r, (appendIds & 0xff) != 0);
  appendToWriteBuffer(f, pos.data(), vel.data(), life.data(), ids.data(), r);
  traceAdd(f, pd, appendIds, r);
  return r;
}

void __fastcall hRemoveAllParticles(S* f, void*) {
  hostSync(f, "removeAllParticles");
  traceRemoveAll(f);
  H(f)->sim->removeAllParticles();
}

void __fastcall hSetParticlesWriteData(S* f, void*, const ParticleData* pd) { memcpy(&f->write, pd, sizeof(f->write)); }
ParticleData* __fastcall hGetParticlesWriteData(S* f, void*, ParticleData* ret) {
  *ret = f->write;
  return ret;
}
void __fastcall hSetDeletionIds(S* f, void*, const IdData* d) { f->deletionIds = *d; }
IdData* __fastcall hGetDeletionIds(S* f, void*, IdData* ret) {
  *ret = f->deletionIds;
  return ret;
}
void __fastcall hSetCreationIds(S* f, void*, const IdData* d) { f->creationIds = *d; }
IdData* __fastcall hGetCreationIds(S* f, void*, IdData* ret) {
  *ret = f->creationIds;
  return ret;
}

void stored(S* f, const char* what) {
  cov(kCovFluidSetterAfterCreate);
  logLine("hosted fluid %u: %s stored, not applied to the running solver", f->index, what);
}

uint32_t __fastcall hGetNumReserve(S* f, void*) { return H(f)->desc[dReserve]; }
void __fastcall hSetNumReserve(S* f, void*, uint32_t v) {
  H(f)->desc[dReserve] = v;
  stored(f, "numReserveParticles");
}
uint32_t __fastcall hGetCurrentLimit(S* f, void*) { return H(f)->desc[dMaxParticles]; }
void __fastcall hSetCurrentLimit(S* f, void*, uint32_t v) {
  logLine("hosted fluid %u: setCurrentParticleLimit(%u) ignored", f->index, v);
}

// NX force buffer (NX_FORCE, IMPULSE, VELOCITY_CHANGE, ACCELERATION): each entry becomes a velocity change added at the
// start of the next simulate that steps (the core's rule, docs/re-notes-fluid-api.md updateParticles). The change is
// computed now with the time of the last simulate. Index based updates (no ids) touch every particle in order.
void queueForces(ShimFluid* f, const UpdateData* ud) {
  HostedFluid* h = H(f);
  const float mass = pxr::particleMass(h->params);
  const pxr::ForceMode mode = (pxr::ForceMode)ud->forceMode;
  if (ud->bufId) {
    for (uint32_t i = 0; i < ud->num; ++i) {
      const float* F = at<float>(ud->bufForce, ud->strideForce, i);
      const uint32_t id = *at<uint32_t>(ud->bufId, ud->strideId, i);
      h->pendingVel.push_back({id, pxr::forceVelocityChange(mode, Vec3(F[0], F[1], F[2]), h->simTime, mass)});
    }
  } else {
    hostSync(f, "updateParticles(forces)");  // reads the solver's particles, which a running job steps
    const size_t n = h->sim->size();
    const uint32_t* ids = h->sim->ids().data();
    for (size_t i = 0; i < n; ++i) {
      const float* F = at<float>(ud->bufForce, ud->strideForce, (uint32_t)i);
      h->pendingVel.push_back({ids[i], pxr::forceVelocityChange(mode, Vec3(F[0], F[1], F[2]), h->simTime, mass)});
    }
  }
  countFluidCall(17, f, "forces queued as velocity changes");
}

void __fastcall hUpdateParticles(S* f, void*, const UpdateData* ud) {
  traceUpdate(f, ud);
  if (!ud) return;
  if (ud->bufForce) queueForces(f, ud);
  if (!ud->bufId || !ud->bufFlag) return;
  // Only deletions change the solver: an update without any leaves a step in flight alone.
  bool deletes = false;
  for (uint32_t i = 0; i < ud->num && !deletes; ++i) deletes = (*at<uint32_t>(ud->bufFlag, ud->strideFlag, i) & kFpDelete) != 0;
  if (!deletes) return;
  hostSync(f, "updateParticles");
  for (uint32_t i = 0; i < ud->num; ++i)
    if (*at<uint32_t>(ud->bufFlag, ud->strideFlag, i) & kFpDelete) H(f)->sim->deleteParticle(*at<uint32_t>(ud->bufId, ud->strideId, i));
}

void __fastcall hSetPacketData(S* f, void*, const PacketData* d) { f->packets = *d; }
PacketData* __fastcall hGetPacketData(S* f, void*, PacketData* ret) {
  *ret = f->packets;
  return ret;
}

// Plain descriptor fields.
#define GET_U(name, idx) \
  uint32_t __fastcall name(S* f, void*) { return H(f)->desc[idx]; }
#define SET_U(name, idx, what)                      \
  void __fastcall name(S* f, void*, uint32_t v) { \
    H(f)->desc[idx] = v;                            \
    stored(f, what);                                \
  }
#define GET_F(name, idx) \
  float __fastcall name(S* f, void*) { return descF(H(f), idx); }
#define SET_F(name, idx, what)                   \
  void __fastcall name(S* f, void*, float v) { \
    setDescF(H(f), idx, v);                      \
    stored(f, what);                             \
  }
GET_U(hGetSimMethod, dSimMethod) SET_U(hSetSimMethod, dSimMethod, "simulationMethod")
GET_F(hGetStiffness, dStiffness) SET_F(hSetStiffness, dStiffness, "stiffness")
GET_F(hGetViscosity, dViscosity) SET_F(hSetViscosity, dViscosity, "viscosity")
GET_F(hGetSurfaceTension, dSurfaceTension) SET_F(hSetSurfaceTension, dSurfaceTension, "surfaceTension")
GET_F(hGetDamping, dDamping) SET_F(hSetDamping, dDamping, "damping")
GET_F(hGetFadeIn, dFadeIn) SET_F(hSetFadeIn, dFadeIn, "fadeInTime")
GET_U(hGetCollMethod, dCollMethod)
void __fastcall hSetCollMethod(S* f, void*, uint32_t v) {  // read by the shape selection, so it applies at once
  hostSync(f, "setCollisionMethod");
  H(f)->desc[dCollMethod] = v;
  H(f)->filter.includeStatic = (v & kCollisionStatic) != 0;
  H(f)->filter.includeDynamic = (v & kCollisionDynamic) != 0;
}
GET_F(hGetRestS, dRestS) SET_F(hSetRestS, dRestS, "restitutionForStaticShapes")
GET_F(hGetDynFrS, dDynFrS) SET_F(hSetDynFrS, dDynFrS, "dynamicFrictionForStaticShapes")
GET_F(hGetStatFrS, dStatFrS) SET_F(hSetStatFrS, dStatFrS, "staticFrictionForStaticShapes")
GET_F(hGetAttrS, dAttrS) SET_F(hSetAttrS, dAttrS, "attractionForStaticShapes")
GET_F(hGetRestD, dRestD) SET_F(hSetRestD, dRestD, "restitutionForDynamicShapes")
GET_F(hGetDynFrD, dDynFrD) SET_F(hSetDynFrD, dDynFrD, "dynamicFrictionForDynamicShapes")
GET_F(hGetStatFrD, dStatFrD) SET_F(hSetStatFrD, dStatFrD, "staticFrictionForDynamicShapes")
GET_F(hGetAttrD, dAttrD) SET_F(hSetAttrD, dAttrD, "attractionForDynamicShapes")
GET_F(hGetResponse, dResponse) SET_F(hSetResponse, dResponse, "collisionResponseCoefficient")
GET_U(hGetMaxParticles, dMaxParticles)
GET_F(hGetKRM, dKRM) GET_F(hGetMLM, dMLM) GET_F(hGetCDM, dCDM) GET_U(hGetPacketSize, dPacketSize)
GET_F(hGetRestPPM, dRestPPM) GET_F(hGetRestDensity, dRestDensity)
#undef GET_U
#undef SET_U
#undef GET_F
#undef SET_F

float* __fastcall hGetExtAccel(S* f, void*, float* ret) {
  memcpy(ret, &H(f)->desc[dExtAccel], 12);
  return ret;
}
void __fastcall hSetExtAccel(S* f, void*, const float* v) {
  memcpy(&H(f)->desc[dExtAccel], v, 12);
  stored(f, "externalAcceleration");
}
float* __fastcall hGetProjPlane(S* f, void*, float* ret) {
  memcpy(ret, &H(f)->desc[dProjPlane], 16);
  return ret;
}
void __fastcall hSetProjPlane(S* f, void*, const float* v) {
  memcpy(&H(f)->desc[dProjPlane], v, 16);
  stored(f, "projectionPlane");
}
void __fastcall hSetFlag(S* f, void*, uint32_t flag, uint32_t on) {
  uint32_t& fl = H(f)->desc[dFlags];
  fl = (on & 0xff) ? (fl | flag) : (fl & ~flag);
  stored(f, "flag");
}
uint32_t __fastcall hGetFlag(S* f, void*, uint32_t flag) { return (H(f)->desc[dFlags] & flag) ? 1 : 0; }
uint32_t __fastcall hGetScene(S* f, void*) { return f->scene; }
float __fastcall hGetRestDistance(S* f, void*) { return 1.0f / descF(H(f), dRestPPM); }
float __fastcall hGetParticleMass(S* f, void*) {
  const float ppm = descF(H(f), dRestPPM);
  return descF(H(f), dRestDensity) / (ppm * ppm * ppm);
}
void __fastcall hSetGroup(S* f, void*, uint32_t g) {
  hostSync(f, "setGroup");
  H(f)->desc[dGroup] = g & 0xffff;
  H(f)->filter.collisionGroup = g & 0xffff;
}
uint32_t __fastcall hGetGroup(S* f, void*) { return H(f)->desc[dGroup] & 0xffff; }
void __fastcall hSetGroupsMask(S* f, void*, const uint32_t* m) {
  hostSync(f, "setGroupsMask");
  memcpy(&H(f)->desc[dGroupsMask], m, 16);
  memcpy(H(f)->filter.groupsMask, m, 16);
}
uint32_t* __fastcall hGetGroupsMask(S* f, void*, uint32_t* ret) {
  memcpy(ret, &H(f)->desc[dGroupsMask], 16);
  return ret;
}
// Read-only, so it never waits for a step: while the step runs, the answer is the bounds the game last saw (the
// state at the last fetch, saved when the step started); otherwise the solver's bounds, which no step is changing.
void __fastcall hGetWorldBounds(S* f, void*, float* dest) {
  if (H(f)->inJob) {
    memcpy(dest, H(f)->boundsAtStep, 24);
  } else {
    Vec3 lo, hi;
    H(f)->sim->worldBounds(lo, hi);
    memcpy(dest, &lo, 12);
    memcpy(dest + 3, &hi, 12);
  }
  traceBounds(f, dest);
}
uint32_t __fastcall hLoadFromDesc(S* f, void*, const void*) {
  logLine("hosted fluid %u: loadFromDesc not supported", f->index);
  return 0;
}
uint32_t __fastcall hSaveToDesc(S* f, void*, uint32_t* desc) {
  memcpy(desc, H(f)->desc, 0x54 * 4);  // NxFluidDescBase part only
  memcpy(desc + kDescWriteData, &f->write, sizeof(f->write));
  memcpy(desc + kDescDeletionIds, &f->deletionIds, sizeof(f->deletionIds));
  memcpy(desc + kDescCreationIds, &f->creationIds, sizeof(f->creationIds));
  memcpy(desc + kDescPackets, &f->packets, sizeof(f->packets));
  desc[kDescUserData] = f->userData;
  return 1;
}
uint32_t __fastcall hGetPPUTime(S*, void*) { return 0; }
void __fastcall hSetName(S* f, void*, uint32_t name) { H(f)->desc[dName] = name; }
uint32_t __fastcall hGetName(S* f, void*) { return H(f)->desc[dName]; }
uint32_t __fastcall hGetCompartment(S* f, void*) { return H(f)->desc[dCompartment]; }
uint32_t __fastcall hGetFFMaterial(S* f, void*) { return H(f)->desc[dForceFieldMaterial] & 0xffff; }
void __fastcall hSetFFMaterial(S* f, void*, uint32_t m) { H(f)->desc[dForceFieldMaterial] = m & 0xffff; }

#define P(fn) ptr32(reinterpret_cast<const void*>(&fn))

}  // namespace

const uint32_t g_hostVt[] = {
    P(hDestructor), P(hCreateEmitter), P(hReleaseEmitter), P(hGetNbEmitters), P(hGetEmitters),
    P(hAddParticles), P(hRemoveAllParticles), P(hSetParticlesWriteData), P(hGetParticlesWriteData),
    P(hSetDeletionIds), P(hGetDeletionIds), P(hSetCreationIds), P(hGetCreationIds),
    P(hGetNumReserve), P(hSetNumReserve), P(hGetCurrentLimit), P(hSetCurrentLimit), P(hUpdateParticles),
    P(hSetPacketData), P(hGetPacketData), P(hGetSimMethod), P(hSetSimMethod),
    P(hGetStiffness), P(hSetStiffness), P(hGetViscosity), P(hSetViscosity), P(hGetSurfaceTension),
    P(hSetSurfaceTension), P(hGetDamping), P(hSetDamping), P(hGetFadeIn), P(hSetFadeIn),
    P(hGetExtAccel), P(hSetExtAccel), P(hGetProjPlane), P(hSetProjPlane), P(hGetCollMethod), P(hSetCollMethod),
    P(hGetRestS), P(hSetRestS), P(hGetDynFrS), P(hSetDynFrS), P(hGetStatFrS), P(hSetStatFrS), P(hGetAttrS),
    P(hSetAttrS), P(hGetRestD), P(hSetRestD), P(hGetDynFrD), P(hSetDynFrD), P(hGetStatFrD), P(hSetStatFrD),
    P(hGetAttrD), P(hSetAttrD), P(hSetResponse), P(hGetResponse), P(hSetFlag), P(hGetFlag), P(hGetScene),
    P(hGetMaxParticles), P(hGetKRM), P(hGetMLM), P(hGetCDM), P(hGetPacketSize), P(hGetRestPPM),
    P(hGetRestDensity), P(hGetRestDistance), P(hGetParticleMass), P(hSetGroup), P(hGetGroup),
    P(hSetGroupsMask), P(hGetGroupsMask), P(hGetWorldBounds), P(hLoadFromDesc), P(hSaveToDesc),
    P(hGetPPUTime), P(hSetName), P(hGetName), P(hGetCompartment), P(hGetFFMaterial), P(hSetFFMaterial)};
static_assert(sizeof(g_hostVt) / sizeof(g_hostVt[0]) == 81, "NxFluid has 81 slots");

namespace {
// Collision threads: one pool shared by all hosted fluids (used by one step at a time). Never destroyed: joining
// threads while the DLL unloads would deadlock on the loader lock.
pxr::ThreadPool* g_pool = nullptr;
// Jobs of a few chunks (~256 particles each) run inline: waking the workers costs more than it saves (measured under
// Wine: 576 particles were ~0.1 ms slower per step on 7 threads than on 1). Results are the same either way.
constexpr size_t kMinParallelChunks = 5;
void poolRun(size_t n, void (*fn)(void*, size_t), void* ctx) {
  if (n < kMinParallelChunks)
    for (size_t i = 0; i < n; ++i) fn(ctx, i);
  else
    g_pool->run(n, fn, ctx);
}
unsigned poolThreads() {
  static const unsigned n = [] {
    if (env("PXSHIM_THREADS")[0]) return (unsigned)std::max(1, atoi(env("PXSHIM_THREADS")));
    const unsigned hw = std::thread::hardware_concurrency();
    // About half the hardware threads (one SMT sibling per physical core) minus one for the game thread, at most 8:
    // 16 threads (8 cores) -> 7, 8 -> 3, 4 -> 1. The game itself runs ~3 busy threads (game, render, audio) [A].
    return std::max(1u, std::min(8u, hw / 2 > 1 ? hw / 2 - 1 : 1u));
  }();
  return n;
}

void joinJob(int why);  // below
}  // namespace

// The same pool for other hosted objects (cloth_ours.cpp steps its cloths on it); created on first use.
void shimPoolRun(size_t n, void (*fn)(void*, size_t), void* ctx) {
  if (poolThreads() > 1 && !g_pool) {
    static std::mutex m;
    std::lock_guard<std::mutex> l(m);
    if (!g_pool) g_pool = new pxr::ThreadPool(poolThreads());
  }
  if (g_pool) g_pool->run(n, fn, ctx);
  else
    for (size_t i = 0; i < n; ++i) fn(ctx, i);
}

bool hostWants(const uint32_t* desc) {
  const char* mode = env("PXSHIM_FLUID");
  if (!strcmp(mode, "off") || !strcmp(mode, "0")) return false;
  if (!strcmp(mode, "ours")) return desc[dSimMethod] == kMethodNoInteraction;
  return true;  // unset, "ours-all" or anything else: every fluid on our solver
}

HostedFluid* hostCreate(ShimFluid* f, const uint32_t* desc) {
  HostedFluid* h = new HostedFluid();
  memcpy(h->desc, desc, sizeof(h->desc));
  h->params = paramsFromDesc(desc);
  coverDesc(desc);
  h->sim = new pxr::Fluid(h->params);
  if (poolThreads() > 1) {
    if (!g_pool) {
      g_pool = new pxr::ThreadPool(poolThreads());
      logLine("hosted fluids: collision on %u threads", poolThreads());
    }
    h->sim->setParallelFor(poolRun);
  }
  h->filter.collisionGroup = desc[dGroup] & 0xffff;
  memcpy(h->filter.groupsMask, desc + dGroupsMask, 16);
  // The scene filter (filter ops/bool/constants read from the scene) decides; probe_drain showed no effect only under the
  // default filter state, and UE3 sets its own filter ops, so keep the mask test (PXSHIM_FLUID_NOMASK=1 turns it off).
  h->filter.useGroupsMask = env("PXSHIM_FLUID_NOMASK")[0] != '1';
  h->filter.includeStatic = (desc[dCollMethod] & kCollisionStatic) != 0;
  h->filter.includeDynamic = (desc[dCollMethod] & kCollisionDynamic) != 0 && env("PXSHIM_FLUID_NODYN")[0] != '1';
  // Initial particles (NxFluidDesc.initialParticleData, dwords 0..0x0f) go in like addParticles.
  const ParticleData* init = reinterpret_cast<const ParticleData*>(desc);
  if (init->numPtr && init->buf[kPos] && *fromPtr32<const uint32_t>(init->numPtr)) {
    f->hosted = h;
    hAddParticles(f, nullptr, init, 0);
  }
  logLine("hosted fluid %u: method %u max %u reserve %u restPPM %g KRM %g CDM %g MLM %g flags %#x", f->index,
          desc[dSimMethod], desc[dMaxParticles], desc[dReserve], (double)h->params.restParticlesPerMeter,
          (double)h->params.kernelRadiusMultiplier, (double)h->params.collisionDistanceMultiplier,
          (double)h->params.motionLimitMultiplier, desc[dFlags]);
  return h;
}

void hostDestroy(ShimFluid* f) {
  hostSync(f, "release");
  delete f->hosted->sim;
  delete f->hosted;
  f->hosted = nullptr;
}

double nowMs() {
  static LARGE_INTEGER freq = [] { LARGE_INTEGER f; QueryPerformanceFrequency(&f); return f; }();
  LARGE_INTEGER t;
  QueryPerformanceCounter(&t);
  return 1000.0 * (double)t.QuadPart / (double)freq.QuadPart;
}

// Coverage (docs/audit/GAPS.md rows 12-14): a step with a moving body shape near, a step with a convex near.
void coverNear(const HostedFluid* h) {
  bool moving = false, convex = false;
  for (const pxr::DynamicShape& d : h->nearDynamic) {
    moving = moving || d.linVel.len2() > 1e-6f || d.angVel.len2() > 1e-6f;
    convex = convex || (d.geom && d.geom->convex);
  }
  for (const pxr::StaticShape* st : h->nearShapes) convex = convex || st->convex;
  if (moving) cov(kCovFluidMovingBodySteps);
  if (convex) cov(kCovFluidConvexNear);
}

// Collision geometry per scene, shared by its hosted fluids.
std::map<uint32_t, SceneGeometry*> g_sceneGeometry;
struct {
  double actors, statics, dynamics;  // ms summed over the refreshes
  uint32_t refreshes, dynShapes, steps, stepsMax;  // fluid steps per simulate (catch-up substeps)
  double select, solve, solveMax;                   // ms: shape selection and solver steps, summed; slowest step
  uint32_t maxParticles, maxStatics, maxDynamics;   // inputs of the slowest step
  uint32_t asyncJobs, callWaits;                    // steps run on the fluid thread; joins forced by a fluid call
  double jobMs, waitMs, waitMax;                    // ms: stepping (either thread); the game thread waiting for it
  double writeMs;                                   // ms: writing the user's buffers at fetch
} g_geomStats;
std::map<std::string, uint32_t> g_forcedBy;  // forced waits per entry point since the last stats line (game thread)

// Step dumps to PhysXLoader_step<k>.pxsd (solver/fluid/step_bench times them, step_replay looks for particles
// passing through static geometry), at most PXSHIM_DUMP_MAX (default 4) per run:
//  - slow steps (PXSHIM_DUMP_MS, default 4; 0 = off): after a step slower than that, the next step of the same fluid;
//  - sampled steps (PXSHIM_DUMP_EVERY=n): every n-th hosted step that has particles and static shapes near.
uint32_t g_dumps = 0, g_dumpSample = 0;
uint32_t dumpMax() {
  static const uint32_t m = !debug() ? 0 : env("PXSHIM_DUMP_MAX")[0] ? (uint32_t)atoi(env("PXSHIM_DUMP_MAX")) : 4;  // debug only
  return m;
}

SceneGeometry& sceneGeometry(uint32_t scene) {
  SceneGeometry*& g = g_sceneGeometry[scene];
  if (!g) g = new SceneGeometry();
  return *g;
}

void hostSceneReleased(uint32_t scene) {
  joinJob(0);
  auto it = g_sceneGeometry.find(scene);
  if (it == g_sceneGeometry.end()) return;
  delete it->second;
  g_sceneGeometry.erase(it);
}

void particleBounds(const std::vector<Vec3>& pos, Box3& b) {
  b.lo = b.hi = pos[0];
  for (const Vec3& p : pos) {
    b.lo = Vec3(fminf(b.lo.x, p.x), fminf(b.lo.y, p.y), fminf(b.lo.z, p.z));
    b.hi = Vec3(fmaxf(b.hi.x, p.x), fmaxf(b.hi.y, p.y), fmaxf(b.hi.z, p.z));
  }
}

namespace {

// One scene's fluid steps for this frame: planned and geometry refreshed in simulate (the core is read there), stepped
// inline or on the fluid thread.
struct Plan {
  uint32_t steps;
  float dt, margin;
};
struct SceneJob {
  uint32_t scene = 0;
  SceneGeometry* geom = nullptr;
  std::vector<ShimFluid*> fluids;
  std::vector<Plan> plans;
  SceneSettings ss;
  std::vector<pxr::BodyImpulse> impulses;
  double ms = 0;  // time in runSteps
};

// One fluid's steps of a job (a pool task when the job has several fluids: the fluids are independent, their
// collision splits again inside). Per-fluid stats and impulses are merged in fluid order afterwards.
struct FluidRun {
  double select = 0, solve = 0, solveMax = 0;
  uint32_t maxParticles = 0, maxStatics = 0, maxDynamics = 0;
  std::vector<pxr::BodyImpulse> impulses;
};
std::mutex g_dumpM;

void stepFluid(SceneJob& job, size_t fi, FluidRun& out) {
  SceneGeometry& geom = *job.geom;
  const SceneSettings& ss = job.ss;
  HostedFluid* h = H(job.fluids[fi]);
  const Plan& pl = job.plans[fi];
  for (uint32_t i = 0; i < pl.steps; ++i) {
    const double s0 = nowMs();
    const auto& pos = h->sim->positions();
    if (pos.empty()) {
      h->nearShapes.clear();
      h->nearDynamic.clear();
    } else {
      Box3 b;
      particleBounds(pos, b);
      b.lo = b.lo - Vec3(pl.margin, pl.margin, pl.margin);
      b.hi = b.hi + Vec3(pl.margin, pl.margin, pl.margin);
      geom.selectStatic(h->filter, b, h->nearShapes);
      // substep i sweeps the bodies over the matching slice of their last frame
      geom.selectDynamic(h->filter, b, (pl.steps - i) * pl.dt, (pl.steps - i - 1) * pl.dt, h->nearDynamic);
      coverNear(h);
    }
    const double s1 = nowMs();
    {
      static const uint32_t dumpEvery = (uint32_t)atoi(env("PXSHIM_DUMP_EVERY"));
      std::lock_guard<std::mutex> l(g_dumpM);
      if (dumpEvery && !pos.empty() && !h->nearShapes.empty() && ++g_dumpSample % dumpEvery == 0 && g_dumps < dumpMax())
        h->dumpNext = true;
      if (h->dumpNext) {
        h->dumpNext = false;
        char path[64];
        snprintf(path, sizeof(path), "PhysXLoader_step%u.pxsd", g_dumps);
        if (pxr::writeStepDump(path, *h->sim, h->params, pl.dt, ss.gravity, h->nearShapes, h->nearDynamic, 0))
          logLine("hosted fluid %u: step inputs dumped to %s (%u particles, %u statics, %u dynamics)",
                  job.fluids[fi]->index, path, (unsigned)h->sim->size(), (unsigned)h->nearShapes.size(),
                  (unsigned)h->nearDynamic.size());
        ++g_dumps;
      }
    }
    const size_t nPart = h->sim->size();
    {
      pxr::CollisionGroups groups;  // the scene's group table as of this refresh; the solver skips shapes it disables
      geom.collisionGroups(groups);
      h->sim->setCollisionGroups(h->filter.collisionGroup, groups);
    }
    h->sim->step(pl.dt, ss.gravity, h->nearShapes, h->nearDynamic);
    const double s2 = nowMs();
    out.select += s1 - s0, out.solve += s2 - s1;
    if (s2 - s1 > out.solveMax) {
      out.solveMax = s2 - s1;
      out.maxParticles = (uint32_t)nPart;
      out.maxStatics = (uint32_t)h->nearShapes.size();
      out.maxDynamics = (uint32_t)h->nearDynamic.size();
    }
    static const double dumpMs = env("PXSHIM_DUMP_MS")[0] ? atof(env("PXSHIM_DUMP_MS")) : 4.0;
    if (dumpMs > 0 && s2 - s1 > dumpMs) {
      std::lock_guard<std::mutex> l(g_dumpM);
      if (g_dumps < dumpMax()) h->dumpNext = true;
    }
    h->deleted.insert(h->deleted.end(), h->sim->deletedIds().begin(), h->sim->deletedIds().end());
    out.impulses.insert(out.impulses.end(), h->sim->bodyImpulses().begin(), h->sim->bodyImpulses().end());
  }
  if (pl.steps) computeOrder(h);
}

void runSteps(SceneJob& job) {
  const double t0 = nowMs();
  std::vector<FluidRun> runs(job.fluids.size());
  struct Ctx {
    SceneJob* job;
    std::vector<FluidRun>* runs;
  } ctx{&job, &runs};
  auto task = [](void* p, size_t fi) {
    Ctx& c = *static_cast<Ctx*>(p);
    stepFluid(*c.job, fi, (*c.runs)[fi]);
  };
  if (g_pool && job.fluids.size() > 1) g_pool->run(job.fluids.size(), task, &ctx);
  else for (size_t fi = 0; fi < job.fluids.size(); ++fi) task(&ctx, fi);
  for (const FluidRun& r : runs) {
    g_geomStats.select += r.select, g_geomStats.solve += r.solve;
    if (r.solveMax > g_geomStats.solveMax) {
      g_geomStats.solveMax = r.solveMax;
      g_geomStats.maxParticles = r.maxParticles, g_geomStats.maxStatics = r.maxStatics;
      g_geomStats.maxDynamics = r.maxDynamics;
    }
    job.impulses.insert(job.impulses.end(), r.impulses.begin(), r.impulses.end());
  }
  job.ms = nowMs() - t0;
}

// The fluid thread: runs one queued scene job at a time. At most one job is outstanding; hostSimulateScene and
// every call that touches a fluid of the job wait for it first (joinJob).
std::mutex g_jobM;
std::condition_variable g_jobCv;
SceneJob g_job;
bool g_jobQueued = false, g_jobBusy = false;  // busy: queued or running (cleared by joinJob)

void fluidThread() {
  for (;;) {
    std::unique_lock<std::mutex> l(g_jobM);
    g_jobCv.wait(l, [] { return g_jobQueued; });
    l.unlock();
    runSteps(g_job);
    l.lock();
    g_jobQueued = false;
    g_jobCv.notify_all();
  }
}

// Waits for the outstanding job (if any) and releases its fluids. why: 0 = fetch/next simulate, 1 = a fluid call.
void joinJob(int why) {
  if (!g_jobBusy) return;
  const double t0 = nowMs();
  {
    std::unique_lock<std::mutex> l(g_jobM);
    g_jobCv.wait(l, [] { return !g_jobQueued; });
  }
  const double waited = nowMs() - t0;
  g_jobBusy = false;
  for (ShimFluid* f : g_job.fluids) H(f)->inJob = false;
  g_geomStats.waitMs += waited, g_geomStats.waitMax = std::max(g_geomStats.waitMax, waited);
  g_geomStats.jobMs += g_job.ms;
  if (why) ++g_geomStats.callWaits;
  if (!g_job.impulses.empty()) logLine("warning: two-way impulses from an async job dropped");  // not expected
  g_job.fluids.clear();
}

}  // namespace

// Every hosted-fluid entry point that reads or writes the solver, the filter or the user's buffers calls this.
void hostSync(ShimFluid* f, const char* entry) {
  if (f->hosted && f->hosted->inJob) {
    ++g_forcedBy[entry];
    joinJob(1);
  }
}

namespace {

void hostSimulateSceneImpl(uint32_t scene, const std::vector<ShimFluid*>& fluids, float elapsed) {
  joinJob(0);
  if (fluids.empty()) return;
  SceneSettings ss;
  readSceneSettings(scene, ss);
  // Scene timing [A: the fluid assumed to substep like the rigid bodies]: fixed = whole maxTimestep substeps,
  // at most maxIter per simulate; variable = one step of the elapsed time.
  std::vector<Plan> plans(fluids.size());
  std::vector<Box3> nearBoxes;
  float frameDt = 0;
  bool twoWay = false;
  for (size_t fi = 0; fi < fluids.size(); ++fi) {
    HostedFluid* h = H(fluids[fi]);
    Plan& pl = plans[fi];
    pl.steps = 1;
    pl.dt = elapsed;
    if (ss.timestepMethod == 0 && ss.maxTimestep > 0) {
      h->accum += elapsed;
      pl.steps = (uint32_t)(h->accum / ss.maxTimestep);
      if (pl.steps > ss.maxIter) pl.steps = ss.maxIter;
      h->accum -= pl.steps * ss.maxTimestep;
      if (h->accum > ss.maxTimestep) h->accum = 0;  // dropped time beyond maxIter substeps [A]
      pl.dt = ss.maxTimestep;
    }
    // Shapes near the particles: their bounds grown by how far a particle can get in one step (motion limit) plus
    // the proximity distance (2 * collision distance) [margin A, generous].
    const float spacing = 1.0f / h->params.restParticlesPerMeter;
    pl.margin = spacing * (h->params.motionLimitMultiplier + 2.0f * h->params.collisionDistanceMultiplier) + 0.01f;
    frameDt = fmaxf(frameDt, pl.steps * pl.dt);
    if (pl.steps && !h->sim->positions().empty()) {
      Box3 b;
      particleBounds(h->sim->positions(), b);
      const float m = pl.margin * pl.steps;
      nearBoxes.push_back({b.lo - Vec3(m, m, m), b.hi + Vec3(m, m, m)});
    }
    twoWay = twoWay || (h->params.twoWay && h->filter.includeDynamic);
    // Force scale of the next updateParticles: the substep time of this simulate, at least one substep [V, docs].
    h->simTime = (pl.steps > 1 ? pl.steps : 1) * pl.dt;
    for (const auto& pv : h->pendingVel) h->sim->addVelocityChange(pv.first, pv.second);
    h->pendingVel.clear();
  }
  SceneGeometry& geom = sceneGeometry(scene);
  // census: once per scene (it forces a full static rescan and logs)
  static std::vector<uint32_t> censused;
  bool census = false;
  if (debug() && env("PXSHIM_GEOM_CENSUS")[0] == '1' && std::find(censused.begin(), censused.end(), scene) == censused.end()) {
    censused.push_back(scene);
    census = true;
  }
  geom.refresh(scene, nearBoxes, frameDt, census);
  g_geomStats.actors += geom.timing().actors, g_geomStats.statics += geom.timing().statics;
  g_geomStats.dynamics += geom.timing().dynamics, g_geomStats.dynShapes += geom.dynamicNear();
  ++g_geomStats.refreshes;
  uint32_t maxSteps = 0;
  for (const Plan& pl : plans) maxSteps = std::max(maxSteps, pl.steps);
  g_geomStats.steps += maxSteps;
  g_geomStats.stepsMax = std::max(g_geomStats.stepsMax, maxSteps);
  if (!maxSteps) return;

  static const bool asyncOn = env("PXSHIM_FLUID_ASYNC")[0] != '0';
  if (asyncOn && !twoWay) {
    static std::thread* thread = new std::thread(fluidThread);  // never joined (see g_pool)
    (void)thread;
    std::lock_guard<std::mutex> l(g_jobM);
    g_job.scene = scene, g_job.geom = &geom, g_job.fluids = fluids, g_job.plans = plans, g_job.ss = ss;
    g_job.impulses.clear();
    for (ShimFluid* f : fluids) {
      HostedFluid* h = H(f);
      Vec3 lo, hi;  // the state the game last fetched: what getWorldBounds reports until the step is joined
      h->sim->worldBounds(lo, hi);
      memcpy(h->boundsAtStep, &lo, 12);
      memcpy(h->boundsAtStep + 3, &hi, 12);
      h->inJob = true;
    }
    g_jobQueued = g_jobBusy = true;
    ++g_geomStats.asyncJobs;
    g_jobCv.notify_all();
    return;
  }
  SceneJob job;
  job.scene = scene, job.geom = &geom, job.fluids = fluids, job.plans = plans, job.ss = ss;
  runSteps(job);
  g_geomStats.jobMs += job.ms;
  if (!job.impulses.empty()) geom.applyImpulses(job.impulses);
}

}  // namespace

void hostSimulateScene(uint32_t scene, const std::vector<ShimFluid*>& fluids, float elapsed) {
  hostSimulateSceneImpl(scene, fluids, elapsed);
}

// fetchResults succeeded: the scene's steps must be done before the outputs are written.
void hostFetched(ShimFluid* f) {
  if (f->hosted->inJob) joinJob(0);  // the expected join (not counted as forced by a fluid call)
  const double t0 = nowMs();
  writeOutputs(f);
  g_geomStats.writeMs += nowMs() - t0;
}



void hostGeometryStats(char* buf, size_t size) {
  joinJob(0);
  const double n = g_geomStats.refreshes ? g_geomStats.refreshes : 1;
  const int k = snprintf(buf, size, "geometry per refresh: actors %.3f statics %.3f dynamics %.3f ms, %.1f dynamic shapes near; "
           "fluid steps per simulate avg %.2f max %u; per simulate (CPU time summed over fluids): shape selection %.3f ms, solver %.3f ms; slowest "
           "solver step %.2f ms (%u particles, %u statics, %u dynamics)", g_geomStats.actors / n,
           g_geomStats.statics / n, g_geomStats.dynamics / n, g_geomStats.dynShapes / n, g_geomStats.steps / n,
           g_geomStats.stepsMax, g_geomStats.select / n, g_geomStats.solve / n, g_geomStats.solveMax,
           g_geomStats.maxParticles, g_geomStats.maxStatics, g_geomStats.maxDynamics);
  std::string by;
  for (const auto& e : g_forcedBy) by += (by.empty() ? "" : ", ") + e.first + " " + std::to_string(e.second);
  g_forcedBy.clear();
  if (k > 0 && (size_t)k < size)
    snprintf(buf + k, size - k, "; stepping %.3f ms per simulate (%u of %u simulates on the fluid thread, %u "
             "threads), game thread waited %.3f ms per simulate (max %.2f), %u waits forced by fluid calls (%s); output "
             "writing %.3f ms per simulate",
             g_geomStats.jobMs / n, g_geomStats.asyncJobs, g_geomStats.refreshes, poolThreads(), g_geomStats.waitMs / n,
             g_geomStats.waitMax, g_geomStats.callWaits, by.empty() ? "none" : by.c_str(), g_geomStats.writeMs / n);
  memset(&g_geomStats, 0, sizeof(g_geomStats));
}

void hostCounts(const ShimFluid* f, uint32_t* particles, uint32_t* shapes) {
  joinJob(0);
  *particles += (uint32_t)f->hosted->sim->size();
  *shapes += (uint32_t)(f->hosted->nearShapes.size() + f->hosted->nearDynamic.size());
}

}  // namespace shim
