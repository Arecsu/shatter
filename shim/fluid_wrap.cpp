// NxFluid stand-in (shim step 4a: pass-through wrapper).
//
// NxScene::createFluid hands the game a ShimFluid instead of the core's NxFluid. Its vtable has the
// 81 NxFluid slots; every slot forwards to the real fluid (ECX swapped to the real object, stack and
// return value untouched), counting and logging calls on the way. A few slots are typed so their
// arguments can be traced (addParticles, removeAllParticles, updateParticles) or their result hooked
// (createEmitter). NxFluidEmitter objects are not wrapped: their vtable is patched with counting thunks.
//
// Slot names follow the 2.8.1 header declaration order; 2.8.0 has the same 81 slots in the same order
// (docs/re-notes-fluid-mesh.md 3.1) [V].
//
// Trace (PXSHIM_TRACE=1: API inputs, 2: plus every fluid's write buffers after each fetchResults)
// goes to PhysXLoader_fluid.trace next to the DLL. Format ("PXFT", analysis/fluid_trace.py reads it):
//   header: "PXFT" u32 version (1)
//   records: u32 tag, u32 fluid (creation index, ~0 when not fluid-specific), u32 bytes, payload
//   tag 1 CREATE      352-byte NxFluidDesc as passed (pointers are the game's) + u32 real NxFluid*
//   tag 2 RELEASE     -
//   tag 3 ADD         u32 requested, u32 appendIds, u32 returned, u32 streams (bit0 pos, 1 vel, 2 life,
//                     3 density, 4 id, 5 flag, 6 normal), then per particle 8 floats pos vel life density
//                     (absent streams are 0); then, if appendIds and an id buffer, u32 ids per returned particle
//   tag 4 UPDATE      u32 forceMode, u32 n, u32 streams (bit0 force, 1 flag, 2 id), then per update
//                     u32 id, u32 flag, 3 floats force
//   tag 5 REMOVEALL   -
//   tag 6 SIMULATE    float elapsed
//   tag 7 FETCHED     u32 status, u32 result
//   tag 8 PARTICLES   u32 n, then n * 52-byte records (pos3 vel3 density life normal3 id flags, the
//                     same record as harness/src/fluid_oracle.cpp; streams without a buffer are 0)
//   tag 9 CREATED_IDS u32 n, ids        tag 10 DELETED_IDS u32 n, ids
//   tag 11 PACKETS    u32 n, n * 36-byte packets (aabb min3 max3, firstParticleIndex, numParticles, id)
//   tag 14 WRITE_COUNT_AFTER_ADD (level 2) *particlesWriteData.numParticlesPtr right after addParticles
//   tag 13 WORLD_BOUNDS (level 2) getWorldBounds result: 6 floats min3 max3
//   tag 12 CREATED_IDS_AFTER_ADD  (level 2) the creation id buffer right after addParticles returned: u32 n, ids

#include <windows.h>

#include <float.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <vector>

#include "fluid_internal.h"
#include "scene_read.h"
#include "shim.h"

namespace shim {
namespace {

const uint32_t kFluidSlots = 81, kEmitterSlots = 41;
const char* const kFluidNames[kFluidSlots] = {
    "~NxFluid", "createEmitter", "releaseEmitter", "getNbEmitters", "getEmitters", "addParticles",
    "removeAllParticles", "setParticlesWriteData", "getParticlesWriteData", "setParticleDeletionIdWriteData",
    "getParticleDeletionIdWriteData", "setParticleCreationIdWriteData", "getParticleCreationIdWriteData",
    "getNumReserveParticles", "setNumReserveParticles", "getCurrentParticleLimit", "setCurrentParticleLimit",
    "updateParticles", "setFluidPacketData", "getFluidPacketData", "getSimulationMethod", "setSimulationMethod",
    "getStiffness", "setStiffness", "getViscosity", "setViscosity", "getSurfaceTension", "setSurfaceTension",
    "getDamping", "setDamping", "getFadeInTime", "setFadeInTime", "getExternalAcceleration",
    "setExternalAcceleration", "getProjectionPlane", "setProjectionPlane", "getCollisionMethod",
    "setCollisionMethod", "getRestitutionForStaticShapes", "setRestitutionForStaticShapes",
    "getDynamicFrictionForStaticShapes", "setDynamicFrictionForStaticShapes", "getStaticFrictionForStaticShapes",
    "setStaticFrictionForStaticShapes", "getAttractionForStaticShapes", "setAttractionForStaticShapes",
    "getRestitutionForDynamicShapes", "setRestitutionForDynamicShapes", "getDynamicFrictionForDynamicShapes",
    "setDynamicFrictionForDynamicShapes", "getStaticFrictionForDynamicShapes", "setStaticFrictionForDynamicShapes",
    "getAttractionForDynamicShapes", "setAttractionForDynamicShapes", "setCollisionResponseCoefficient",
    "getCollisionResponseCoefficient", "setFlag", "getFlag", "getScene", "getMaxParticles",
    "getKernelRadiusMultiplier", "getMotionLimitMultiplier", "getCollisionDistanceMultiplier",
    "getPacketSizeMultiplier", "getRestParticlesPerMeter", "getRestDensity", "getRestParticleDistance",
    "getParticleMass", "setGroup", "getGroup", "setGroupsMask", "getGroupsMask", "getWorldBounds", "loadFromDesc",
    "saveToDesc", "getPPUTime", "setName", "getName", "getCompartment", "getForceFieldMaterial",
    "setForceFieldMaterial"};
const char* const kEmitterNames[kEmitterSlots] = {
    "~NxFluidEmitter", "getFluid", "setGlobalPose", "setGlobalPosition", "setGlobalOrientation",
    "getGlobalPoseVal", "getGlobalPositionVal", "getGlobalOrientationVal", "setLocalPose", "setLocalPosition",
    "setLocalOrientation", "getLocalPoseVal", "getLocalPositionVal", "getLocalOrientationVal", "setFrameShape",
    "getFrameShape", "getDimensionX", "getDimensionY", "setRandomPos", "getRandomPos", "setRandomAngle",
    "getRandomAngle", "setFluidVelocityMagnitude", "getFluidVelocityMagnitude", "setRate", "getRate",
    "setParticleLifetime", "getParticleLifetime", "setRepulsionCoefficient", "getRepulsionCoefficient",
    "resetEmission", "getMaxParticles", "getNbParticlesEmitted", "setFlag", "getFlag", "getShape", "getType",
    "loadFromDesc", "saveToDesc", "setName", "getName"};
const uint32_t kSlotCreateEmitter = 1, kSlotAddParticles = 5, kSlotRemoveAll = 6, kSlotSetWriteData = 7,
               kSlotUpdateParticles = 17, kSlotGetWorldBounds = 72;
const uint32_t kEmitterIdBase = 0x100;  // thunk id: slot (fluid) or 0x100 + slot (emitter)

std::vector<ShimFluid*> g_fluids;  // live wrappers
uint32_t g_nextIndex = 0;
uint32_t g_wrapVt[kFluidSlots];
uint32_t g_emitterVt = 0;
uint32_t g_emitterOrig[kEmitterSlots];
volatile LONG g_fluidCount[kFluidSlots], g_emitterCount[kEmitterSlots];
const LONG kLogFirstCalls = 12;  // per slot; afterwards only at powers of two
FILE* g_trace = nullptr;
int g_traceLevel = 0;

ShimFluid* asShim(uint32_t p) {
  for (ShimFluid* f : g_fluids)
    if (ptr32(f) == p) return f;
  return nullptr;
}

// Entry `slot` of a core object's vtable.
uint32_t realSlot(uint32_t obj, uint32_t slot) { return fromPtr32<const uint32_t>(*fromPtr32<const uint32_t>(obj))[slot]; }

// Counts a call; true if this one should be logged.
bool countCall(volatile LONG* counter, LONG* n) {
  *n = InterlockedIncrement(counter);
  return *n <= kLogFirstCalls || (*n & (*n - 1)) == 0;
}

// ---- trace ------------------------------------------------------------------------------
enum Tag : uint32_t {
  kCreate = 1, kRelease, kAdd, kUpdate, kRemoveAll, kSimulate, kFetched, kParticles, kCreatedIds, kDeletedIds,
  kPackets, kCreatedIdsAfterAdd, kWorldBounds, kWriteCountAfterAdd
};
const uint32_t kNoFluid = ~0u;
const uint32_t kMaxCount = 0x10000;  // particle/id/packet counts above this are garbage (max 65535 particles)

std::vector<uint8_t> g_rec;  // payload being assembled
void recBegin() { g_rec.clear(); }
void recPut(const void* p, size_t n) {
  const uint8_t* b = static_cast<const uint8_t*>(p);
  g_rec.insert(g_rec.end(), b, b + n);
}
void recU32(uint32_t v) { recPut(&v, 4); }
void recF32(float v) { recPut(&v, 4); }
void recEnd(uint32_t tag, uint32_t fluid) {
  if (!g_trace) return;
  const uint32_t hdr[3] = {tag, fluid, static_cast<uint32_t>(g_rec.size())};
  fwrite(hdr, sizeof(hdr), 1, g_trace);
  if (!g_rec.empty()) fwrite(g_rec.data(), 1, g_rec.size(), g_trace);
}

// Element i of a strided user buffer (null buffer -> null).
template <class T>
const T* elem(uint32_t buf, uint32_t stride, uint32_t i) {
  return buf ? fromPtr32<const T>(buf + i * stride) : nullptr;
}

// After fetchResults: the particle write buffers, id buffers and packets of one fluid.
void traceOutputs(const ShimFluid* f) {
  const ParticleData& w = f->write;
  uint32_t n = w.numPtr ? *fromPtr32<const uint32_t>(w.numPtr) : 0;
  if (n > kMaxCount) {
    logLine("trace: fluid %u particle count %u out of range, not traced", f->index, n);
    return;
  }
  recBegin();
  recU32(n);
  for (uint32_t i = 0; i < n; ++i) {
    float rec[11] = {0};
    uint32_t idf[2] = {0, 0};
    if (const float* p = elem<float>(w.buf[kPos], w.stride[kPos], i)) memcpy(rec, p, 12);
    if (const float* p = elem<float>(w.buf[kVel], w.stride[kVel], i)) memcpy(rec + 3, p, 12);
    if (const float* p = elem<float>(w.buf[kDensity], w.stride[kDensity], i)) rec[6] = *p;
    if (const float* p = elem<float>(w.buf[kLife], w.stride[kLife], i)) rec[7] = *p;
    if (const float* p = elem<float>(w.buf[kNormal], w.stride[kNormal], i)) memcpy(rec + 8, p, 12);
    if (const uint32_t* p = elem<uint32_t>(w.buf[kId], w.stride[kId], i)) idf[0] = *p;
    if (const uint32_t* p = elem<uint32_t>(w.buf[kFlag], w.stride[kFlag], i)) idf[1] = *p;
    recPut(rec, sizeof(rec));
    recPut(idf, sizeof(idf));
  }
  recEnd(kParticles, f->index);
  const IdData* ids[2] = {&f->creationIds, &f->deletionIds};
  const Tag tags[2] = {kCreatedIds, kDeletedIds};
  for (int k = 0; k < 2; ++k) {
    if (!ids[k]->numPtr) continue;
    uint32_t m = *fromPtr32<const uint32_t>(ids[k]->numPtr);
    if (m > kMaxCount) {
      logLine("trace: fluid %u %s id count %u out of range", f->index, k ? "deletion" : "creation", m);
      continue;
    }
    recBegin();
    recU32(m);
    for (uint32_t i = 0; i < m; ++i) recU32(ids[k]->buf ? *elem<uint32_t>(ids[k]->buf, ids[k]->stride, i) : 0);
    recEnd(tags[k], f->index);
  }
  if (f->packets.numPtr) {
    uint32_t m = *fromPtr32<const uint32_t>(f->packets.numPtr);
    if (m > kMaxCount) {
      logLine("trace: fluid %u packet count %u out of range", f->index, m);
      return;
    }
    recBegin();
    recU32(m);
    if (f->packets.buf) recPut(fromPtr32<const void>(f->packets.buf), m * 36);
    recEnd(kPackets, f->index);
  }
}

// ---- generic slot thunks ------------------------------------------------------------------
// Each slot gets a 2-instruction thunk (push id; jmp common). The common stub saves all registers and
// calls onSlotCall(regs), which counts/logs the call and writes the target over the id on the stack;
// popad + ret then jumps there with the stack exactly as the caller left it. For a wrapped fluid ECX is
// replaced with the real NxFluid; emitters are hooked in place, so ECX stays.
// regs: pushad block (EDI ESI EBP ESP EBX EDX ECX EAX), then the thunk id, return address, args.
extern "C" void __cdecl onSlotCall(uint32_t* regs) {
  uint32_t* frame = regs + 8;
  const uint32_t id = frame[0];
  const bool emitter = id >= kEmitterIdBase;
  const uint32_t slot = emitter ? id - kEmitterIdBase : id;
  const uint32_t* a = frame + 2;
  uint32_t target = 0;
  if (emitter) {
    target = g_emitterOrig[slot];
  } else {
    ShimFluid* f = fromPtr32<ShimFluid>(regs[6]);
    if (slot == kSlotSetWriteData && a[0]) memcpy(&f->write, fromPtr32<const void>(a[0]), sizeof(ParticleData));
    regs[6] = f->real;
    target = realSlot(f->real, slot);
  }
  LONG n = 0;
  if (countCall(emitter ? &g_emitterCount[slot] : &g_fluidCount[slot], &n)) {
    float fa[4];
    memcpy(fa, a, sizeof(fa));
    logLine("%s::%s #%ld (this=%08X ret=%08X) args %08X %08X %08X %08X (as float %g %g %g %g)",
            emitter ? "NxFluidEmitter" : "NxFluid", emitter ? kEmitterNames[slot] : kFluidNames[slot], n, regs[6],
            frame[1], a[0], a[1], a[2], a[3], (double)fa[0], (double)fa[1], (double)fa[2], (double)fa[3]);
  }
  frame[0] = target;
}

__declspec(naked) void slotCommon() {
  __asm {
    pushad
    push esp
    call onSlotCall
    add esp, 4
    popad
    ret
  }
}

#define THUNK(id)                      \
  __declspec(naked) void thunk_##id() { \
    __asm { push id }                  \
    __asm { jmp slotCommon }           \
  }
THUNK(0) THUNK(1) THUNK(2) THUNK(3) THUNK(4) THUNK(5) THUNK(6) THUNK(7)
THUNK(8) THUNK(9) THUNK(10) THUNK(11) THUNK(12) THUNK(13) THUNK(14) THUNK(15)
THUNK(16) THUNK(17) THUNK(18) THUNK(19) THUNK(20) THUNK(21) THUNK(22) THUNK(23)
THUNK(24) THUNK(25) THUNK(26) THUNK(27) THUNK(28) THUNK(29) THUNK(30) THUNK(31)
THUNK(32) THUNK(33) THUNK(34) THUNK(35) THUNK(36) THUNK(37) THUNK(38) THUNK(39)
THUNK(40) THUNK(41) THUNK(42) THUNK(43) THUNK(44) THUNK(45) THUNK(46) THUNK(47)
THUNK(48) THUNK(49) THUNK(50) THUNK(51) THUNK(52) THUNK(53) THUNK(54) THUNK(55)
THUNK(56) THUNK(57) THUNK(58) THUNK(59) THUNK(60) THUNK(61) THUNK(62) THUNK(63)
THUNK(64) THUNK(65) THUNK(66) THUNK(67) THUNK(68) THUNK(69) THUNK(70) THUNK(71)
THUNK(72) THUNK(73) THUNK(74) THUNK(75) THUNK(76) THUNK(77) THUNK(78) THUNK(79)
THUNK(80) THUNK(256) THUNK(257) THUNK(258) THUNK(259) THUNK(260) THUNK(261) THUNK(262)
THUNK(263) THUNK(264) THUNK(265) THUNK(266) THUNK(267) THUNK(268) THUNK(269) THUNK(270)
THUNK(271) THUNK(272) THUNK(273) THUNK(274) THUNK(275) THUNK(276) THUNK(277) THUNK(278)
THUNK(279) THUNK(280) THUNK(281) THUNK(282) THUNK(283) THUNK(284) THUNK(285) THUNK(286)
THUNK(287) THUNK(288) THUNK(289) THUNK(290) THUNK(291) THUNK(292) THUNK(293) THUNK(294)
THUNK(295) THUNK(296)

void (*const kFluidThunks[kFluidSlots])() = {
    thunk_0, thunk_1, thunk_2, thunk_3, thunk_4, thunk_5, thunk_6, thunk_7,
    thunk_8, thunk_9, thunk_10, thunk_11, thunk_12, thunk_13, thunk_14, thunk_15,
    thunk_16, thunk_17, thunk_18, thunk_19, thunk_20, thunk_21, thunk_22, thunk_23,
    thunk_24, thunk_25, thunk_26, thunk_27, thunk_28, thunk_29, thunk_30, thunk_31,
    thunk_32, thunk_33, thunk_34, thunk_35, thunk_36, thunk_37, thunk_38, thunk_39,
    thunk_40, thunk_41, thunk_42, thunk_43, thunk_44, thunk_45, thunk_46, thunk_47,
    thunk_48, thunk_49, thunk_50, thunk_51, thunk_52, thunk_53, thunk_54, thunk_55,
    thunk_56, thunk_57, thunk_58, thunk_59, thunk_60, thunk_61, thunk_62, thunk_63,
    thunk_64, thunk_65, thunk_66, thunk_67, thunk_68, thunk_69, thunk_70, thunk_71,
    thunk_72, thunk_73, thunk_74, thunk_75, thunk_76, thunk_77, thunk_78, thunk_79,
    thunk_80};
void (*const kEmitterThunks[kEmitterSlots])() = {
    thunk_256, thunk_257, thunk_258, thunk_259, thunk_260, thunk_261, thunk_262, thunk_263,
    thunk_264, thunk_265, thunk_266, thunk_267, thunk_268, thunk_269, thunk_270, thunk_271,
    thunk_272, thunk_273, thunk_274, thunk_275, thunk_276, thunk_277, thunk_278, thunk_279,
    thunk_280, thunk_281, thunk_282, thunk_283, thunk_284, thunk_285, thunk_286, thunk_287,
    thunk_288, thunk_289, thunk_290, thunk_291, thunk_292, thunk_293, thunk_294, thunk_295,
    thunk_296};

// ---- typed slots ----------------------------------------------------------------------------
// __fastcall(this, edx_unused, args...) is the __thiscall calling convention plus an ignored EDX.
typedef uint32_t(__fastcall* FnCreateEmitter)(uint32_t self, void* edx, const void* desc);
typedef uint32_t(__fastcall* FnAddParticles)(uint32_t self, void* edx, const ParticleData* pd, uint32_t appendIds);
typedef void(__fastcall* FnRemoveAll)(uint32_t self, void* edx);
typedef void(__fastcall* FnUpdateParticles)(uint32_t self, void* edx, const UpdateData* ud);

void installEmitterHooks(uint32_t emitter) {
  uint32_t vt = *fromPtr32<const uint32_t>(emitter);
  if (!vt || g_emitterVt) {
    if (vt != g_emitterVt) logLine("warning: second NxFluidEmitter vtable %08X ignored (first %08X)", vt, g_emitterVt);
    return;
  }
  g_emitterVt = vt;
  uint32_t* p = fromPtr32<uint32_t>(vt);
  DWORD old = 0;
  if (!VirtualProtect(p, kEmitterSlots * 4, PAGE_READWRITE, &old)) return;
  for (uint32_t i = 0; i < kEmitterSlots; ++i) {
    g_emitterOrig[i] = p[i];
    p[i] = ptr32(reinterpret_cast<const void*>(kEmitterThunks[i]));
  }
  VirtualProtect(p, kEmitterSlots * 4, old, &old);
  FlushInstructionCache(GetCurrentProcess(), p, kEmitterSlots * 4);
  logLine("emitter vtable %08X hooked (%u slots)", vt, kEmitterSlots);
}

uint32_t __fastcall wCreateEmitter(ShimFluid* f, void*, const void* desc) {
  LONG n = 0;
  countCall(&g_fluidCount[kSlotCreateEmitter], &n);
  logLine("> NxFluid::createEmitter #%ld (fluid %u desc=%08X)", n, f->index, ptr32(desc));
  dumpWords("emitterDesc", desc, 0x74);
  uint32_t r = ((FnCreateEmitter)realSlot(f->real, kSlotCreateEmitter))(f->real, nullptr, desc);
  logLine("< NxFluid::createEmitter -> %08X", r);
  if (r) installEmitterHooks(r);
  return r;
}

typedef void(__fastcall* FnGetWorldBounds)(uint32_t self, void* edx, float* dest);

uint32_t __fastcall wAddParticles(ShimFluid* f, void*, const ParticleData* pd, uint32_t appendIds) {
  uint32_t r = ((FnAddParticles)realSlot(f->real, kSlotAddParticles))(f->real, nullptr, pd, appendIds);
  traceAdd(f, pd, appendIds, r);
  return r;
}

void __fastcall wRemoveAll(ShimFluid* f, void*) {
  traceRemoveAll(f);
  ((FnRemoveAll)realSlot(f->real, kSlotRemoveAll))(f->real, nullptr);
}

void __fastcall wUpdateParticles(ShimFluid* f, void*, const UpdateData* ud) {
  traceUpdate(f, ud);
  ((FnUpdateParticles)realSlot(f->real, kSlotUpdateParticles))(f->real, nullptr, ud);
}

void __fastcall wGetWorldBounds(ShimFluid* f, void*, float* dest) {
  ((FnGetWorldBounds)realSlot(f->real, kSlotGetWorldBounds))(f->real, nullptr, dest);
  traceBounds(f, dest);
}

// Scene settings and a geometry census as seen by this fluid (PXSHIM_GEOM_CENSUS=1).
void logSceneCensus(uint32_t scene, const uint32_t* desc) {
  SceneSettings ss;
  readSceneSettings(scene, ss);
  logLine("scene %08X: gravity (%g %g %g) timing maxTimestep %g maxIter %u method %u", scene, (double)ss.gravity.x,
          (double)ss.gravity.y, (double)ss.gravity.z, (double)ss.maxTimestep, ss.maxIter, ss.timestepMethod);
  FluidFilterDesc ff;
  ff.collisionGroup = desc[kDescCollisionGroup] & 0xffff;
  memcpy(ff.groupsMask, desc + kDescGroupsMask, sizeof(ff.groupsMask));
  SceneGeometry geom;
  geom.refresh(scene, {}, 0, true);
  std::vector<const pxr::StaticShape*> sel;
  const float inf = FLT_MAX;
  geom.selectStatic(ff, Box3{pxr::Vec3(-inf, -inf, -inf), pxr::Vec3(inf, inf, inf)}, sel);
  logLine("  %u static shapes pass this fluid's group/groupsMask filter", (unsigned)sel.size());
}

}  // namespace

void countFluidCall(uint32_t slot, const ShimFluid* f, const char* fmt, ...) {
  LONG n = 0;
  if (!countCall(&g_fluidCount[slot], &n)) return;
  char msg[256];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(msg, sizeof(msg), fmt, ap);
  va_end(ap);
  logLine("NxFluid::%s #%ld (fluid %u%s) %s", kFluidNames[slot], n, f->index, f->hosted ? ", ours" : "", msg);
}

void traceAdd(const ShimFluid* f, const ParticleData* pd, uint32_t appendIds, uint32_t r) {
  const uint32_t req = pd && pd->numPtr ? *fromPtr32<const uint32_t>(pd->numPtr) : 0;
  countFluidCall(kSlotAddParticles, f, "n=%u appendIds=%u -> %u", req, appendIds & 0xff, r);
  if (g_traceLevel >= 1 && pd) {
    uint32_t streams = 0;
    for (int k = 0; k < 7; ++k)
      if (pd->buf[k]) streams |= 1u << k;
    recBegin();
    recU32(req);
    recU32(appendIds & 0xff);
    recU32(r);
    recU32(streams);
    for (uint32_t i = 0; i < req; ++i) {
      float v[8] = {0};
      if (const float* p = elem<float>(pd->buf[kPos], pd->stride[kPos], i)) memcpy(v, p, 12);
      if (const float* p = elem<float>(pd->buf[kVel], pd->stride[kVel], i)) memcpy(v + 3, p, 12);
      if (const float* p = elem<float>(pd->buf[kLife], pd->stride[kLife], i)) v[6] = *p;
      if (const float* p = elem<float>(pd->buf[kDensity], pd->stride[kDensity], i)) v[7] = *p;
      recPut(v, sizeof(v));
    }
    if ((appendIds & 0xff) && pd->buf[kId])
      for (uint32_t i = 0; i < r; ++i) recU32(*elem<uint32_t>(pd->buf[kId], pd->stride[kId], i));
    recEnd(kAdd, f->index);
  }
  if (g_traceLevel >= 2 && f->creationIds.numPtr) {  // the creation id buffer right after addParticles
    uint32_t m = *fromPtr32<const uint32_t>(f->creationIds.numPtr);
    if (m <= kMaxCount) {
      recBegin();
      recU32(m);
      for (uint32_t i = 0; i < m; ++i)
        recU32(f->creationIds.buf ? *elem<uint32_t>(f->creationIds.buf, f->creationIds.stride, i) : 0);
      recEnd(kCreatedIdsAfterAdd, f->index);
    }
  }
  if (g_traceLevel >= 2 && f->write.numPtr) {  // does addParticles update the particle write buffer itself?
    recBegin();
    recU32(*fromPtr32<const uint32_t>(f->write.numPtr));
    recEnd(kWriteCountAfterAdd, f->index);
  }
}

void traceRemoveAll(const ShimFluid* f) {
  countFluidCall(kSlotRemoveAll, f, "");
  if (g_traceLevel >= 1) {
    recBegin();
    recEnd(kRemoveAll, f->index);
  }
}

void traceUpdate(const ShimFluid* f, const UpdateData* ud) {
  if (!ud) return;
  countFluidCall(kSlotUpdateParticles, f, "mode=%u n=%u force=%08X flag=%08X id=%08X strides %u %u %u first id=%u flag=%08X",
                 ud->forceMode, ud->num, ud->bufForce, ud->bufFlag, ud->bufId, ud->strideForce, ud->strideFlag,
                 ud->strideId, ud->num && ud->bufId ? *elem<uint32_t>(ud->bufId, ud->strideId, 0) : 0,
                 ud->num && ud->bufFlag ? *elem<uint32_t>(ud->bufFlag, ud->strideFlag, 0) : 0);
  if (g_traceLevel >= 1) {
    recBegin();
    recU32(ud->forceMode);
    recU32(ud->num);
    recU32((ud->bufForce ? 1u : 0u) | (ud->bufFlag ? 2u : 0u) | (ud->bufId ? 4u : 0u));
    for (uint32_t i = 0; i < ud->num; ++i) {
      recU32(ud->bufId ? *elem<uint32_t>(ud->bufId, ud->strideId, i) : 0);
      recU32(ud->bufFlag ? *elem<uint32_t>(ud->bufFlag, ud->strideFlag, i) : 0);
      float force[3] = {0, 0, 0};
      if (ud->bufForce) memcpy(force, elem<float>(ud->bufForce, ud->strideForce, i), 12);
      recPut(force, 12);
    }
    recEnd(kUpdate, f->index);
  }
}

void traceBounds(const ShimFluid* f, const float* b) {
  countFluidCall(kSlotGetWorldBounds, f, "-> (%g %g %g)-(%g %g %g)", (double)b[0], (double)b[1], (double)b[2],
                 (double)b[3], (double)b[4], (double)b[5]);
  if (g_traceLevel >= 2) {
    recBegin();
    recPut(b, 24);
    recEnd(kWorldBounds, f->index);
  }
}

void fluidInit() {
  for (uint32_t i = 0; i < kFluidSlots; ++i) g_wrapVt[i] = ptr32(reinterpret_cast<const void*>(kFluidThunks[i]));
  g_wrapVt[kSlotCreateEmitter] = ptr32(reinterpret_cast<const void*>(&wCreateEmitter));
  g_wrapVt[kSlotAddParticles] = ptr32(reinterpret_cast<const void*>(&wAddParticles));
  g_wrapVt[kSlotRemoveAll] = ptr32(reinterpret_cast<const void*>(&wRemoveAll));
  g_wrapVt[kSlotUpdateParticles] = ptr32(reinterpret_cast<const void*>(&wUpdateParticles));
  g_wrapVt[kSlotGetWorldBounds] = ptr32(reinterpret_cast<const void*>(&wGetWorldBounds));
  g_traceLevel = atoi(env("PXSHIM_TRACE"));
  if (g_traceLevel > 0) {
    char path[MAX_PATH];
    snprintf(path, sizeof(path), "%sPhysXLoader_fluid.trace", g_dir);
    g_trace = fopen(path, "wb");
    if (g_trace) {
      setvbuf(g_trace, nullptr, _IOFBF, 1 << 20);
      const uint32_t version = 1;
      fwrite("PXFT", 1, 4, g_trace);
      fwrite(&version, 4, 1, g_trace);
    }
    logLine("fluid trace level %d -> %s (%s)", g_traceLevel, path, g_trace ? "open" : "FAILED");
  }
}

uint32_t fluidCreate(void* scene, const void* desc, FnCreateFluid orig) {
  logLine("> NxScene::createFluid(this=%08X desc=%08X)", ptr32(scene), ptr32(desc));
  const uint32_t* d = static_cast<const uint32_t*>(desc);
  if (dumpWords("desc", desc, kDescDwords * 4)) {
    uint32_t b = d[kDescEmittersBegin], e = d[kDescEmittersEnd];
    if (b && e > b && (e - b) % 0x74 == 0 && (e - b) / 0x74 < 64) {
      for (uint32_t k = 0; k < (e - b) / 0x74; ++k) {
        char tag[32];
        snprintf(tag, sizeof(tag), "emitterDesc[%u]", k);
        dumpWords(tag, fromPtr32<const void>(b + k * 0x74), 0x74);
      }
    }
  }
  const bool host = hostWants(d);
  uint32_t real = host ? 0 : orig(scene, nullptr, desc);
  if (!host && !real) {
    logLine("< NxScene::createFluid -> 0 (core refused)");
    return 0;
  }
  if (!host && !debug() && g_traceLevel == 0) return real;  // the core's own fluid, no counting wrapper
  ShimFluid* f = new ShimFluid();
  f->vtbl = host ? g_hostVt : g_wrapVt;
  f->userData = d[kDescUserData];
  f->magic = kMagic;
  f->real = real;
  f->scene = ptr32(scene);
  f->index = g_nextIndex++;
  memcpy(&f->write, d + kDescWriteData, sizeof(f->write));
  memcpy(&f->deletionIds, d + kDescDeletionIds, sizeof(f->deletionIds));
  memcpy(&f->creationIds, d + kDescCreationIds, sizeof(f->creationIds));
  memcpy(&f->packets, d + kDescPackets, sizeof(f->packets));
  if (host) {
    f->hosted = hostCreate(f, d);
    if (!f->hosted) {
      logLine("< NxScene::createFluid -> 0 (our fluid refused the descriptor)");
      delete f;
      return 0;
    }
  }
  g_fluids.push_back(f);
  static uint32_t s_censusScene = 0;  // once per scene: a full scan of a level is slow
  if (debug() && env("PXSHIM_GEOM_CENSUS")[0] == '1' && s_censusScene != ptr32(scene)) {
    s_censusScene = ptr32(scene);
    logSceneCensus(ptr32(scene), d);
  }
  if (g_traceLevel >= 1) {
    recBegin();
    recPut(desc, kDescDwords * 4);
    recU32(real);
    recEnd(kCreate, f->index);
  }
  logLine("< NxScene::createFluid -> %08X (fluid %u, %s %08X)", ptr32(f), f->index, host ? "ours" : "real", real);
  return ptr32(f);
}

void fluidRelease(void* scene, void* fluid, FnReleaseFluid orig) {
  ShimFluid* f = asShim(ptr32(fluid));
  logLine("> NxScene::releaseFluid(this=%08X fluid=%08X%s)", ptr32(scene), ptr32(fluid), f ? "" : " NOT OURS");
  if (!f) {
    orig(scene, nullptr, fluid);
    return;
  }
  if (f->hosted)
    hostDestroy(f);
  else
    orig(scene, nullptr, fromPtr32<void>(f->real));
  if (g_traceLevel >= 1) {
    recBegin();
    recEnd(kRelease, f->index);
  }
  for (size_t i = 0; i < g_fluids.size(); ++i)
    if (g_fluids[i] == f) g_fluids.erase(g_fluids.begin() + i);
  f->magic = 0;
  delete f;
  logLine("< NxScene::releaseFluid");
}

void fluidOnSceneRelease(void* scene) {
  hostSceneReleased(ptr32(scene));
  for (size_t i = 0; i < g_fluids.size();) {
    if (g_fluids[i]->scene == ptr32(scene)) {
      logLine("scene %08X released with fluid %u still alive", ptr32(scene), g_fluids[i]->index);
      if (g_fluids[i]->hosted) hostDestroy(g_fluids[i]);
      delete g_fluids[i];
      g_fluids.erase(g_fluids.begin() + i);
    } else {
      ++i;
    }
  }
}

void fluidOnSimulate(void* scene, float elapsed) {
  if (g_traceLevel >= 1) {
    recBegin();
    recF32(elapsed);
    recEnd(kSimulate, kNoFluid);
  }
  std::vector<ShimFluid*> hosted;
  for (ShimFluid* f : g_fluids)
    if (f->hosted && f->scene == ptr32(scene)) hosted.push_back(f);
  hostSimulateScene(ptr32(scene), hosted, elapsed);
}

void fluidOnFetched(void* scene, uint32_t status, bool ok) {
  if (ok)
    for (ShimFluid* f : g_fluids)
      if (f->hosted && f->scene == ptr32(scene)) hostFetched(f);
  if (g_traceLevel < 1) return;
  recBegin();
  recU32(status);
  recU32(ok ? 1 : 0);
  recEnd(kFetched, kNoFluid);
  if (g_traceLevel >= 2 && ok)
    for (const ShimFluid* f : g_fluids)
      if (f->scene == ptr32(scene)) traceOutputs(f);
  if (g_trace) fflush(g_trace);
}

void fluidLogSummary(const char* why) {
  logLine("fluid slot summary (%s): %u live wrappers", why, (unsigned)g_fluids.size());
  for (uint32_t i = 0; i < kFluidSlots; ++i)
    if (g_fluidCount[i]) logLine("  NxFluid[%2u] %-34s %ld", i, kFluidNames[i], g_fluidCount[i]);
  for (uint32_t i = 0; i < kEmitterSlots; ++i)
    if (g_emitterCount[i]) logLine("  NxFluidEmitter[%2u] %-27s %ld", i, kEmitterNames[i], g_emitterCount[i]);
}

const char* fluidStatsLine() {
  static char buf[1024];
  uint32_t hosted = 0, particles = 0, shapes = 0;
  for (const ShimFluid* f : g_fluids)
    if (f->hosted) {
      ++hosted;
      hostCounts(f, &particles, &shapes);
    }
  int n = snprintf(buf, sizeof(buf), "%u fluids (%u hosted), %u hosted particles, %u shapes near them; ",
                   (unsigned)g_fluids.size(), hosted, particles, shapes);
  hostGeometryStats(buf + n, sizeof(buf) - n);
  return buf;
}

void fluidShutdown() {
  if (g_trace) {
    fclose(g_trace);
    g_trace = nullptr;
  }
}

}  // namespace shim
