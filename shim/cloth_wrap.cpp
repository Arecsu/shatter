// NxCloth pass-through layer (logging only; zero behaviour change).
//
// Nothing is wrapped: the game gets the core's own NxCloth objects. The hooks log and forward:
//   NxScene vtable:     createCloth 131, releaseCloth 132 (2.8.1 header declaration index; 2.8.0 same [V],
//                       docs/re-notes-fluid-mesh.md section 3). Slots >= 153 differ by version and are never touched.
//   NxPhysicsSDK vtable: createClothMesh 21, releaseClothMesh 22, on 2.8.0 and 2.8.1 (PXSHIM_CLOTH_MESH=0 turns the
//                       hook off). 2.8.0 [V]: vf22 has the releaseClothMesh signature; vf21 (0x1013c6a0) pops one
//                       stack argument (ret 4; the decompiler lost it) and constructs an NpClothMesh (vtable
//                       0x102ec5c0) that loads through the stream, so it is createClothMesh(NxStream&).
//   NxCloth vtable:     all 92 slots (2.8.1 NxCloth.h declaration order). Patched in place on the first createCloth
//                       with counting thunks (same mechanism as fluid_wrap.cpp's emitter thunks). The 2.8.0
//                       NpCloth vtable has 92 slots and its 42 named slots sit at the same index [V]
//                       (docs/re-notes-cloth.md "NxCloth vtable / ABI"); the unnamed getters are matched by position [A].
// PXSHIM_CLOTH=0 disables the layer completely (no patches, no thunks).
//
// Log lines (PhysXLoader_shim.log): "> / < NxScene::createCloth", the NxClothDesc as 70 dwords plus decoded fields,
// "NxCloth::<name> #n (this ret) args ...", the first 12 calls per slot and then powers of two; attach calls are
// always logged with the shape's RTTI class name; "NxCloth summary" lists the call count of every slot used.
// Mesh: "> / < NxPhysicsSDK::createClothMesh" and the NxClothMeshDesc counts from saveToDesc (2.8.1 slot 1 [V]).

#include <windows.h>

#include <stdarg.h>
#include <stdio.h>
#include <stdint.h>
#include <string.h>

#include <vector>

#include "cloth_ours.h"
#include "shim.h"

namespace shim {
namespace {

const uint32_t kClothSlots = 92;
// 2.8.1 NxCloth.h declaration order (slot 0 = scalar deleting destructor).
const char* const kClothNames[kClothSlots] = {
    "~NxCloth", "saveToDesc", "getClothMesh", "setBendingStiffness", "getBendingStiffness",
    "setStretchingStiffness", "getStretchingStiffness", "setDampingCoefficient", "getDampingCoefficient",
    "setFriction", "getFriction", "setPressure", "getPressure", "setTearFactor", "getTearFactor",
    "setAttachmentTearFactor", "getAttachmentTearFactor", "setThickness", "getThickness", "getDensity",
    "getRelativeGridSpacing", "getSolverIterations", "setSolverIterations", "getWorldBounds", "attachToShape",
    "attachToCollidingShapes", "detachFromShape", "attachVertexToShape", "attachVertexToGlobalPosition", "freeVertex",
    "dominateVertex", "getVertexAttachmentStatus", "getVertexAttachmentShape", "getVertexAttachmentPosition",
    "attachToCore", "tearVertex", "raycast", "setGroup", "getGroup", "setGroupsMask", "getGroupsMask", "setMeshData",
    "getMeshData", "setValidBounds", "getValidBounds", "setPosition", "setPositions", "getPosition", "getPositions",
    "setVelocity", "setVelocities", "getVelocity", "getVelocities", "getNumberOfParticles", "queryShapePointers",
    "getStateByteSize", "getShapePointers", "setShapePointers", "saveStateToStream", "loadStateFromStream",
    "setCollisionResponseCoefficient", "getCollisionResponseCoefficient", "setAttachmentResponseCoefficient",
    "getAttachmentResponseCoefficient", "setFromFluidResponseCoefficient", "getFromFluidResponseCoefficient",
    "setToFluidResponseCoefficient", "getToFluidResponseCoefficient", "setExternalAcceleration",
    "getExternalAcceleration", "setMinAdhereVelocity", "getMinAdhereVelocity", "setWindAcceleration",
    "getWindAcceleration", "isSleeping", "getSleepLinearVelocity", "setSleepLinearVelocity", "wakeUp", "putToSleep",
    "setFlags", "getFlags", "setName", "addForceAtVertex", "addForceAtPos", "addDirectedForceAtPos",
    "overlapAABBTriangles", "getScene", "getName", "getCompartment", "getPPUTime", "getForceFieldMaterial",
    "setForceFieldMaterial"};

const uint32_t kSceneCreateCloth = 131, kSceneReleaseCloth = 132;
const uint32_t kSdkCreateClothMesh = 21, kSdkReleaseClothMesh = 22;
const uint32_t kMeshSaveToDesc = 1;
const uint32_t kSlotAttachToShape = 24, kSlotAttachToColliding = 25, kSlotAttachVertexToShape = 27;
const uint32_t kClfHardwareBit = 1u << 13;  // NX_CLF_HARDWARE
const uint32_t kDescBytes = 280;      // NxClothDesc, 2.8.1 layout (sizeof 280); same dword indices as 2.8.0 [V]
const uint32_t kMeshDescBytes = 48;   // NxClothMeshDesc
const LONG kLogFirstCalls = 12;

// NxClothDesc byte offsets (2.8.1 MSVC layout, probe of the headers; docs/re-notes-cloth.md for 2.8.0 [V]).
const uint32_t kOffClothMesh = 0, kOffPoseT = 40, kOffThickness = 52, kOffDensity = 56, kOffBending = 60,
               kOffStretching = 64, kOffDamping = 68, kOffFriction = 72, kOffPressure = 76, kOffTear = 80,
               kOffCollResp = 84, kOffAttachResp = 88, kOffAttachTear = 92, kOffToFluid = 96, kOffFromFluid = 100,
               kOffMinAdhere = 104, kOffIterations = 108, kOffExtAccel = 112, kOffWind = 124, kOffWakeUp = 136,
               kOffSleepVel = 140, kOffMeshData = 144, kOffCollGroup = 212, kOffGroupsMask = 216,
               kOffFfMaterial = 232, kOffValidBounds = 236, kOffGridSpacing = 260, kOffFlags = 264,
               kOffUserData = 268, kOffName = 272, kOffCompartment = 276;
// NxMeshData inside the desc (68 bytes at kOffMeshData).
const uint32_t kOffMdPos = 0, kOffMdNormal = 4, kOffMdPosStride = 8, kOffMdNormStride = 12, kOffMdMaxVerts = 16,
               kOffMdNumVertsPtr = 20, kOffMdIdx = 24, kOffMdIdxStride = 28, kOffMdMaxIdx = 32, kOffMdNumIdxPtr = 36,
               kOffMdParent = 40, kOffMdParentStride = 44, kOffMdMaxParent = 48, kOffMdNumParentPtr = 52,
               kOffMdDirtyPtr = 56, kOffMdFlags = 60, kOffMdName = 64;

volatile LONG g_clothCount[kClothSlots];
uint32_t g_clothOrig[kClothSlots];
uint32_t g_clothVt = 0;
volatile LONG g_createCount = 0, g_releaseCount = 0, g_meshCreateCount = 0, g_meshReleaseCount = 0;
bool g_enabled = true;
bool g_meshHooks = false;
bool g_meshHooksSetUp = false;

typedef uint32_t(__fastcall* FnCreateCloth)(void* self, void* edx, const void* desc);
typedef void(__fastcall* FnReleaseCloth)(void* self, void* edx, void* cloth);
typedef uint32_t(__fastcall* FnCreateClothMesh)(void* self, void* edx, void* stream);
typedef void(__fastcall* FnReleaseClothMesh)(void* self, void* edx, void* mesh);
typedef bool(__fastcall* FnSaveMeshDesc)(void* self, void* edx, void* desc);
FnCreateCloth g_origCreateCloth = nullptr;
FnReleaseCloth g_origReleaseCloth = nullptr;
FnCreateClothMesh g_origCreateClothMesh = nullptr;
FnReleaseClothMesh g_origReleaseClothMesh = nullptr;

// Created cloth meshes: pointer and the counts read back right after creation (for the desc log).
struct MeshInfo {
  uint32_t ptr;
  uint32_t numVertices, numTriangles, flags;
};
std::vector<MeshInfo> g_meshes;  // only filled when the mesh hooks are on

// Count a call; true if it should be logged (first kLogFirstCalls, then powers of two).
bool countCall(volatile LONG* counter, LONG* n) {
  *n = InterlockedIncrement(counter);
  return *n <= kLogFirstCalls || (*n & (*n - 1)) == 0;
}

// Class name from the MSVC RTTI of an object (read only, no calls into the object). x86 layout: the vtable
// is preceded by a Complete Object Locator whose pTypeDescriptor (absolute address) points at the name.
const char* classNameOf(uint32_t obj) {
  static char buf[4][96];
  static int next = 0;
  char* out = buf[next++ & 3];
  strcpy(out, "(unknown)");
  if (!obj || IsBadReadPtr(fromPtr32<const void>(obj), 4)) return out;
  uint32_t vt = *fromPtr32<const uint32_t>(obj);
  if (!vt || IsBadReadPtr(fromPtr32<const void>(vt - 4), 4)) return out;
  uint32_t col = *fromPtr32<const uint32_t>(vt - 4);
  if (!col || IsBadReadPtr(fromPtr32<const void>(col), 20)) return out;
  uint32_t td = fromPtr32<const uint32_t>(col)[3];
  if (!td || IsBadReadPtr(fromPtr32<const void>(td + 8), 4)) return out;
  const char* name = fromPtr32<const char>(td + 8);
  if (IsBadStringPtrA(name, 90)) return out;
  if (!strncmp(name, ".?AV", 4) || !strncmp(name, ".?AU", 4)) name += 4;
  snprintf(out, 96, "%s", name);
  char* at = strstr(out, "@@");
  if (at) *at = 0;
  return out;
}

// Text of a pointer's target: "null", "unreadable" or the value. Ring buffer, 4 results per log line.
const char* peekU32(uint32_t p) {
  static char buf[4][24];
  static int next = 0;
  char* out = buf[next++ & 3];
  if (!p) snprintf(out, 24, "null");
  else if (IsBadReadPtr(fromPtr32<const void>(p), 4)) snprintf(out, 24, "unreadable");
  else snprintf(out, 24, "%u", *fromPtr32<const uint32_t>(p));
  return out;
}

const char* nameAt(uint32_t p) {
  if (!p) return "null";
  if (IsBadStringPtrA(fromPtr32<const char>(p), 64)) return "(unreadable)";
  return fromPtr32<const char>(p);
}

float fAt(const uint8_t* d, uint32_t off) {
  float v;
  memcpy(&v, d + off, 4);
  return v;
}
uint32_t uAt(const uint8_t* d, uint32_t off) {
  uint32_t v;
  memcpy(&v, d + off, 4);
  return v;
}

void dumpClothDesc(const void* desc) {
  const uint8_t* d = static_cast<const uint8_t*>(desc);
  if (IsBadReadPtr(desc, kDescBytes)) {
    logLine("  clothDesc unreadable at %08X", ptr32(desc));
    return;
  }
  dumpWords("clothDesc", desc, kDescBytes);  // raw, for offline decoding
  uint32_t mesh = uAt(d, kOffClothMesh);
  logLine("  clothDesc: clothMesh=%08X pose.t=(%g %g %g) thickness=%g density=%g bending=%g stretching=%g damping=%g "
          "friction=%g pressure=%g tearFactor=%g",
          mesh, (double)fAt(d, kOffPoseT), (double)fAt(d, kOffPoseT + 4), (double)fAt(d, kOffPoseT + 8),
          (double)fAt(d, kOffThickness), (double)fAt(d, kOffDensity), (double)fAt(d, kOffBending),
          (double)fAt(d, kOffStretching), (double)fAt(d, kOffDamping), (double)fAt(d, kOffFriction),
          (double)fAt(d, kOffPressure), (double)fAt(d, kOffTear));
  logLine("  clothDesc: collisionResp=%g attachResp=%g attachTear=%g toFluid=%g fromFluid=%g minAdhere=%g "
          "solverIterations=%u externalAccel=(%g %g %g) wind=(%g %g %g)",
          (double)fAt(d, kOffCollResp), (double)fAt(d, kOffAttachResp), (double)fAt(d, kOffAttachTear),
          (double)fAt(d, kOffToFluid), (double)fAt(d, kOffFromFluid), (double)fAt(d, kOffMinAdhere),
          uAt(d, kOffIterations), (double)fAt(d, kOffExtAccel), (double)fAt(d, kOffExtAccel + 4),
          (double)fAt(d, kOffExtAccel + 8), (double)fAt(d, kOffWind), (double)fAt(d, kOffWind + 4),
          (double)fAt(d, kOffWind + 8));
  logLine("  clothDesc: wakeUpCounter=%g sleepLinearVelocity=%g collisionGroup=%u groupsMask=%08X %08X %08X %08X "
          "ffMaterial=%u validBounds=(%g %g %g)-(%g %g %g)",
          (double)fAt(d, kOffWakeUp), (double)fAt(d, kOffSleepVel), uAt(d, kOffCollGroup), uAt(d, kOffGroupsMask),
          uAt(d, kOffGroupsMask + 4), uAt(d, kOffGroupsMask + 8), uAt(d, kOffGroupsMask + 12),
          (uint32_t)(*(const uint16_t*)(d + kOffFfMaterial)), (double)fAt(d, kOffValidBounds),
          (double)fAt(d, kOffValidBounds + 4), (double)fAt(d, kOffValidBounds + 8), (double)fAt(d, kOffValidBounds + 12),
          (double)fAt(d, kOffValidBounds + 16), (double)fAt(d, kOffValidBounds + 20));
  logLine("  clothDesc: relativeGridSpacing=%g flags=%08X userData=%08X name=\"%s\" compartment=%08X",
          (double)fAt(d, kOffGridSpacing), uAt(d, kOffFlags), uAt(d, kOffUserData), nameAt(uAt(d, kOffName)),
          uAt(d, kOffCompartment));
  const uint8_t* m = d + kOffMeshData;
  logLine("  clothDesc.meshData: pos=%08X normal=%08X posStride=%d normStride=%d maxVertices=%u numVerticesPtr=%08X(%s) "
          "indices=%08X idxStride=%d maxIndices=%u numIndicesPtr=%08X(%s)",
          uAt(m, kOffMdPos), uAt(m, kOffMdNormal), (int)uAt(m, kOffMdPosStride), (int)uAt(m, kOffMdNormStride),
          uAt(m, kOffMdMaxVerts), uAt(m, kOffMdNumVertsPtr), peekU32(uAt(m, kOffMdNumVertsPtr)), uAt(m, kOffMdIdx),
          (int)uAt(m, kOffMdIdxStride), uAt(m, kOffMdMaxIdx), uAt(m, kOffMdNumIdxPtr),
          peekU32(uAt(m, kOffMdNumIdxPtr)));
  logLine("  clothDesc.meshData: parent=%08X parentStride=%d maxParent=%u numParentPtr=%08X(%s) dirtyPtr=%08X "
          "flags=%08X name=\"%s\"",
          uAt(m, kOffMdParent), (int)uAt(m, kOffMdParentStride), uAt(m, kOffMdMaxParent),
          uAt(m, kOffMdNumParentPtr), peekU32(uAt(m, kOffMdNumParentPtr)), uAt(m, kOffMdDirtyPtr),
          uAt(m, kOffMdFlags), nameAt(uAt(m, kOffMdName)));
  if (mesh) {
    for (const MeshInfo& mi : g_meshes)
      if (mi.ptr == mesh)
        logLine("  clothMesh %08X: %u vertices, %u triangles, flags %08X (from createClothMesh)", mesh,
                mi.numVertices, mi.numTriangles, mi.flags);
  }
}

// ---- counting thunks on the NxCloth vtable ----------------------------------------------------
// Same mechanism as fluid_wrap.cpp: `push id; jmp common`; the common stub saves the registers, calls
// onClothSlotCall, which counts/logs and writes the core's target over the id on the stack; popad + ret
// then jumps there with the stack exactly as the caller left it. ECX (this) is untouched.
// regs: pushad block (EDI ESI EBP ESP EBX EDX ECX EAX), then the slot id, return address, arguments.
extern "C" void __cdecl onClothSlotCall(uint32_t* regs) {
  uint32_t* frame = regs + 8;
  const uint32_t slot = frame[0];
  const uint32_t* a = frame + 2;
  frame[0] = g_clothOrig[slot];
  const bool always = slot == kSlotAttachToShape || slot == kSlotAttachToColliding || slot == kSlotAttachVertexToShape;
  LONG n = 0;
  const bool log = countCall(&g_clothCount[slot], &n);
  if (!log && !always) return;
  if (slot == kSlotAttachToShape) {
    logLine("NxCloth::attachToShape #%ld (this=%08X) shape=%08X [%s] flags=%08X", n, regs[6], a[0],
            classNameOf(a[0]), a[1]);
  } else if (slot == kSlotAttachVertexToShape) {
    logLine("NxCloth::attachVertexToShape #%ld (this=%08X) vertex=%u shape=%08X [%s] localPos=%08X flags=%08X", n,
            regs[6], a[0], a[1], classNameOf(a[1]), a[2], a[3]);
  } else if (slot == kSlotAttachToColliding) {
    logLine("NxCloth::attachToCollidingShapes #%ld (this=%08X) flags=%08X", n, regs[6], a[0]);
  } else {
    float fa[4];
    memcpy(fa, a, sizeof(fa));
    logLine("NxCloth::%s #%ld (this=%08X ret=%08X) args %08X %08X %08X %08X (as float %g %g %g %g)", kClothNames[slot],
            n, regs[6], frame[1], a[0], a[1], a[2], a[3], (double)fa[0], (double)fa[1], (double)fa[2],
            (double)fa[3]);
  }
}

__declspec(naked) void clothCommon() {
  __asm {
    pushad
    push esp
    call onClothSlotCall
    add esp, 4
    popad
    ret
  }
}

#define CLOTH_THUNK(id)                      \
  __declspec(naked) void cloth_thunk_##id() { \
    __asm { push id }                        \
    __asm { jmp clothCommon }                \
  }
CLOTH_THUNK(0) CLOTH_THUNK(1) CLOTH_THUNK(2) CLOTH_THUNK(3) CLOTH_THUNK(4) CLOTH_THUNK(5) CLOTH_THUNK(6)
CLOTH_THUNK(7) CLOTH_THUNK(8) CLOTH_THUNK(9) CLOTH_THUNK(10) CLOTH_THUNK(11) CLOTH_THUNK(12) CLOTH_THUNK(13)
CLOTH_THUNK(14) CLOTH_THUNK(15) CLOTH_THUNK(16) CLOTH_THUNK(17) CLOTH_THUNK(18) CLOTH_THUNK(19) CLOTH_THUNK(20)
CLOTH_THUNK(21) CLOTH_THUNK(22) CLOTH_THUNK(23) CLOTH_THUNK(24) CLOTH_THUNK(25) CLOTH_THUNK(26) CLOTH_THUNK(27)
CLOTH_THUNK(28) CLOTH_THUNK(29) CLOTH_THUNK(30) CLOTH_THUNK(31) CLOTH_THUNK(32) CLOTH_THUNK(33) CLOTH_THUNK(34)
CLOTH_THUNK(35) CLOTH_THUNK(36) CLOTH_THUNK(37) CLOTH_THUNK(38) CLOTH_THUNK(39) CLOTH_THUNK(40) CLOTH_THUNK(41)
CLOTH_THUNK(42) CLOTH_THUNK(43) CLOTH_THUNK(44) CLOTH_THUNK(45) CLOTH_THUNK(46) CLOTH_THUNK(47) CLOTH_THUNK(48)
CLOTH_THUNK(49) CLOTH_THUNK(50) CLOTH_THUNK(51) CLOTH_THUNK(52) CLOTH_THUNK(53) CLOTH_THUNK(54) CLOTH_THUNK(55)
CLOTH_THUNK(56) CLOTH_THUNK(57) CLOTH_THUNK(58) CLOTH_THUNK(59) CLOTH_THUNK(60) CLOTH_THUNK(61) CLOTH_THUNK(62)
CLOTH_THUNK(63) CLOTH_THUNK(64) CLOTH_THUNK(65) CLOTH_THUNK(66) CLOTH_THUNK(67) CLOTH_THUNK(68) CLOTH_THUNK(69)
CLOTH_THUNK(70) CLOTH_THUNK(71) CLOTH_THUNK(72) CLOTH_THUNK(73) CLOTH_THUNK(74) CLOTH_THUNK(75) CLOTH_THUNK(76)
CLOTH_THUNK(77) CLOTH_THUNK(78) CLOTH_THUNK(79) CLOTH_THUNK(80) CLOTH_THUNK(81) CLOTH_THUNK(82) CLOTH_THUNK(83)
CLOTH_THUNK(84) CLOTH_THUNK(85) CLOTH_THUNK(86) CLOTH_THUNK(87) CLOTH_THUNK(88) CLOTH_THUNK(89) CLOTH_THUNK(90)
CLOTH_THUNK(91)

void (*const kClothThunks[kClothSlots])() = {
    cloth_thunk_0,  cloth_thunk_1,  cloth_thunk_2,  cloth_thunk_3,  cloth_thunk_4,  cloth_thunk_5,  cloth_thunk_6,
    cloth_thunk_7,  cloth_thunk_8,  cloth_thunk_9,  cloth_thunk_10, cloth_thunk_11, cloth_thunk_12, cloth_thunk_13,
    cloth_thunk_14, cloth_thunk_15, cloth_thunk_16, cloth_thunk_17, cloth_thunk_18, cloth_thunk_19, cloth_thunk_20,
    cloth_thunk_21, cloth_thunk_22, cloth_thunk_23, cloth_thunk_24, cloth_thunk_25, cloth_thunk_26, cloth_thunk_27,
    cloth_thunk_28, cloth_thunk_29, cloth_thunk_30, cloth_thunk_31, cloth_thunk_32, cloth_thunk_33, cloth_thunk_34,
    cloth_thunk_35, cloth_thunk_36, cloth_thunk_37, cloth_thunk_38, cloth_thunk_39, cloth_thunk_40, cloth_thunk_41,
    cloth_thunk_42, cloth_thunk_43, cloth_thunk_44, cloth_thunk_45, cloth_thunk_46, cloth_thunk_47, cloth_thunk_48,
    cloth_thunk_49, cloth_thunk_50, cloth_thunk_51, cloth_thunk_52, cloth_thunk_53, cloth_thunk_54, cloth_thunk_55,
    cloth_thunk_56, cloth_thunk_57, cloth_thunk_58, cloth_thunk_59, cloth_thunk_60, cloth_thunk_61, cloth_thunk_62,
    cloth_thunk_63, cloth_thunk_64, cloth_thunk_65, cloth_thunk_66, cloth_thunk_67, cloth_thunk_68, cloth_thunk_69,
    cloth_thunk_70, cloth_thunk_71, cloth_thunk_72, cloth_thunk_73, cloth_thunk_74, cloth_thunk_75, cloth_thunk_76,
    cloth_thunk_77, cloth_thunk_78, cloth_thunk_79, cloth_thunk_80, cloth_thunk_81, cloth_thunk_82, cloth_thunk_83,
    cloth_thunk_84, cloth_thunk_85, cloth_thunk_86, cloth_thunk_87, cloth_thunk_88, cloth_thunk_89, cloth_thunk_90,
    cloth_thunk_91};

// Patches the 92 NxCloth slots of the core's vtable, once (the first cloth object seen).
void installClothVtable(uint32_t cloth) {
  uint32_t vt = *fromPtr32<const uint32_t>(cloth);
  if (!vt) return;
  if (g_clothVt) {
    if (vt != g_clothVt) logLine("warning: second NxCloth vtable %08X not hooked (first %08X)", vt, g_clothVt);
    return;
  }
  uint32_t* p = fromPtr32<uint32_t>(vt);
  DWORD old = 0;
  if (!VirtualProtect(p, kClothSlots * 4, PAGE_READWRITE, &old)) {
    logLine("NxCloth vtable %08X: VirtualProtect failed gle=%lu, not hooked", vt, (unsigned long)GetLastError());
    return;
  }
  for (uint32_t i = 0; i < kClothSlots; ++i) {
    g_clothOrig[i] = p[i];
    p[i] = ptr32(reinterpret_cast<const void*>(kClothThunks[i]));
  }
  VirtualProtect(p, kClothSlots * 4, old, &old);
  FlushInstructionCache(GetCurrentProcess(), p, kClothSlots * 4);
  g_clothVt = vt;
  logLine("NxCloth vtable %08X hooked (%u slots)", vt, kClothSlots);
}

// ---- NxScene::createCloth / releaseCloth ----------------------------------------------------

uint32_t __fastcall hkCreateCloth(void* self, void* edx, const void* desc) {
  (void)edx;
  const LONG n = InterlockedIncrement(&g_createCount);
  logLine("> NxScene::createCloth #%ld (this=%08X)", n, ptr32(self));
  uint32_t copy[kDescBytes / 4];
  const uint8_t* d8 = static_cast<const uint8_t*>(desc);
  if (d8 && cpuOnly() && ((*reinterpret_cast<const uint32_t*>(d8 + kOffFlags) & kClfHardwareBit) ||
                          *reinterpret_cast<const uint32_t*>(d8 + kOffCompartment))) {
    memcpy(copy, desc, kDescBytes);  // NX_CLF_HARDWARE and the compartment dropped (shim.h cpuOnly)
    logLine("cpu only: cloth desc flags %08X compartment %08X -> software", copy[kOffFlags / 4], copy[kOffCompartment / 4]);
    copy[kOffFlags / 4] &= ~kClfHardwareBit;
    copy[kOffCompartment / 4] = 0;
    desc = copy;
  }
  dumpClothDesc(desc);
  if (oursEnabled()) {
    const uint32_t c = oursCreate(self, desc);
    if (c) {
      logLine("< NxScene::createCloth #%ld -> %08X (hosted on solver/cloth)", n, c);
      return c;
    }
  }
  uint32_t r = g_origCreateCloth(self, nullptr, desc);
  logLine("< NxScene::createCloth #%ld -> %08X", n, r);
  if (r && debug()) installClothVtable(r);  // call counting only
  return r;
}

void __fastcall hkReleaseCloth(void* self, void* edx, void* cloth) {
  (void)edx;
  const LONG n = InterlockedIncrement(&g_releaseCount);
  logLine("> NxScene::releaseCloth #%ld (this=%08X cloth=%08X [%s])", n, ptr32(self), ptr32(cloth),
          classNameOf(ptr32(cloth)));
  if (oursOwns(ptr32(cloth))) {
    oursRelease(ptr32(cloth));
    logLine("< NxScene::releaseCloth #%ld (hosted)", n);
    return;
  }
  g_origReleaseCloth(self, nullptr, cloth);
  logLine("< NxScene::releaseCloth #%ld", n);
}

// ---- NxPhysicsSDK::createClothMesh / releaseClothMesh (gated, see the header comment) ---------

// NxStream as the 2.8.1 header declares it (virtual order: dtor, readers 1-6, writers 7-12).
class IStream {
 public:
  virtual ~IStream() {}
  virtual uint8_t readByte() const = 0;
  virtual uint16_t readWord() const = 0;
  virtual uint32_t readDword() const = 0;
  virtual float readFloat() const = 0;
  virtual double readDouble() const = 0;
  virtual void readBuffer(void* buffer, uint32_t size) const = 0;
  virtual IStream& storeByte(uint8_t b) = 0;
  virtual IStream& storeWord(uint16_t w) = 0;
  virtual IStream& storeDword(uint32_t d) = 0;
  virtual IStream& storeFloat(float f) = 0;
  virtual IStream& storeDouble(double f) = 0;
  virtual IStream& storeBuffer(const void* buffer, uint32_t size) = 0;
};

// Forwards to the game's stream and records what the core reads from it (the cooked mesh bytes, for cloth_ours).
class StreamTap : public IStream {
 public:
  StreamTap(IStream* in, std::vector<uint8_t>* rec) : in_(in), rec_(rec) {}
  uint8_t readByte() const override { uint8_t v = in_->readByte(); add(&v, 1); return v; }
  uint16_t readWord() const override { uint16_t v = in_->readWord(); add(&v, 2); return v; }
  uint32_t readDword() const override { uint32_t v = in_->readDword(); add(&v, 4); return v; }
  float readFloat() const override { float v = in_->readFloat(); add(&v, 4); return v; }
  double readDouble() const override { double v = in_->readDouble(); add(&v, 8); return v; }
  void readBuffer(void* buffer, uint32_t size) const override { in_->readBuffer(buffer, size); add(buffer, size); }
  IStream& storeByte(uint8_t b) override { in_->storeByte(b); return *this; }
  IStream& storeWord(uint16_t w) override { in_->storeWord(w); return *this; }
  IStream& storeDword(uint32_t d) override { in_->storeDword(d); return *this; }
  IStream& storeFloat(float f) override { in_->storeFloat(f); return *this; }
  IStream& storeDouble(double f) override { in_->storeDouble(f); return *this; }
  IStream& storeBuffer(const void* buffer, uint32_t size) override { in_->storeBuffer(buffer, size); return *this; }

 private:
  void add(const void* p, size_t n) const {
    const uint8_t* b = static_cast<const uint8_t*>(p);
    rec_->insert(rec_->end(), b, b + n);
  }
  IStream* in_;
  std::vector<uint8_t>* rec_;
};

uint32_t __fastcall hkCreateClothMesh(void* self, void* edx, void* stream) {
  (void)edx;
  const LONG n = InterlockedIncrement(&g_meshCreateCount);
  const uint32_t streamVt = stream && !IsBadReadPtr(stream, 4) ? *static_cast<const uint32_t*>(stream) : 0;
  logLine("> NxPhysicsSDK::createClothMesh #%ld (this=%08X stream=%08X streamVt=%08X)", n, ptr32(self), ptr32(stream),
          streamVt);
  std::vector<uint8_t> cooked;
  StreamTap tap(static_cast<IStream*>(stream), &cooked);
  const bool tapped = oursEnabled() && stream;
  uint32_t r = g_origCreateClothMesh(self, nullptr, tapped ? static_cast<void*>(&tap) : stream);
  logLine("< NxPhysicsSDK::createClothMesh #%ld -> %08X", n, r);
  if (tapped && r) oursRegisterCookedMesh(r, cooked.data(), cooked.size());
  if (r && !IsBadReadPtr(fromPtr32<const void>(r), 4)) {
    // NxClothMesh::saveToDesc (vtable slot 1 in 2.8.1, declared const) into a zeroed NxClothMeshDesc.
    uint32_t vt = *fromPtr32<const uint32_t>(r);
    uint32_t desc[kMeshDescBytes / 4] = {0};
    if (vt && !IsBadReadPtr(fromPtr32<const void>(vt), 4 * (kMeshSaveToDesc + 1))) {
      auto save = reinterpret_cast<FnSaveMeshDesc>(fromPtr32<const uint32_t>(vt)[kMeshSaveToDesc]);
      save(fromPtr32<void>(r), nullptr, desc);
      MeshInfo mi = {r, desc[0], desc[1], desc[6]};
      g_meshes.push_back(mi);
      logLine("  clothMesh %08X desc: vertices=%u triangles=%u pointStride=%u triStride=%u points=%08X triangles=%08X "
              "flags=%08X vertexMassStride=%u vertexFlagStride=%u vertexMasses=%08X vertexFlags=%08X welding=%g",
              r, desc[0], desc[1], desc[2], desc[3], desc[4], desc[5], desc[6], desc[7], desc[8], desc[9], desc[10],
              (double)*reinterpret_cast<const float*>(&desc[11]));
    }
  }
  return r;
}

void __fastcall hkReleaseClothMesh(void* self, void* edx, void* mesh) {
  (void)edx;
  const LONG n = InterlockedIncrement(&g_meshReleaseCount);
  logLine("> NxPhysicsSDK::releaseClothMesh #%ld (this=%08X mesh=%08X)", n, ptr32(self), ptr32(mesh));
  g_origReleaseClothMesh(self, nullptr, mesh);
  logLine("< NxPhysicsSDK::releaseClothMesh #%ld", n);
}

}  // namespace

// ---- entry points used by PhysXLoader.cpp ----------------------------------------------------

void clothInstallScene(uint32_t sceneVt) {
  if (!g_enabled) return;
  uint32_t o = 0;
  if (patchSlot(sceneVt, kSceneCreateCloth, ptr32(&hkCreateCloth), &o)) g_origCreateCloth = (FnCreateCloth)(uintptr_t)o;
  if (patchSlot(sceneVt, kSceneReleaseCloth, ptr32(&hkReleaseCloth), &o))
    g_origReleaseCloth = (FnReleaseCloth)(uintptr_t)o;
}

void clothInstallSdk(uint32_t sdkVt) {
  if (!g_enabled || g_meshHooksSetUp) return;
  g_meshHooksSetUp = true;
  const int ver = coreVersionOf(sdkVt);
  g_meshHooks = (ver == 280 || ver == 281) && strcmp(env("PXSHIM_CLOTH_MESH"), "0") != 0;
  logLine("cloth layer: core 2.8.%s, createClothMesh/releaseClothMesh %s", ver == 281 ? "1" : ver == 280 ? "0" : "?",
          g_meshHooks ? "hooked" : "not hooked (unknown core or PXSHIM_CLOTH_MESH=0)");
  if (!g_meshHooks) return;
  uint32_t o = 0;
  if (patchSlot(sdkVt, kSdkCreateClothMesh, ptr32(&hkCreateClothMesh), &o))
    g_origCreateClothMesh = (FnCreateClothMesh)(uintptr_t)o;
  if (patchSlot(sdkVt, kSdkReleaseClothMesh, ptr32(&hkReleaseClothMesh), &o))
    g_origReleaseClothMesh = (FnReleaseClothMesh)(uintptr_t)o;
}

void clothInit() {
  g_enabled = strcmp(env("PXSHIM_CLOTH"), "0") != 0;
  logLine("cloth layer %s (cloths are hosted on solver/cloth by default; PXSHIM_CLOTH=off leaves them to the core "
          "with logging, PXSHIM_CLOTH=0 turns the layer off)",
          g_enabled ? "on" : "off");
  oursInit();
}

void clothLogSummary(const char* why) {
  if (!g_enabled) return;
  oursLogSummary(why);
  logLine("NxCloth summary (%s): createCloth=%ld releaseCloth=%ld createClothMesh=%ld releaseClothMesh=%ld", why,
          (long)g_createCount, (long)g_releaseCount, (long)g_meshCreateCount, (long)g_meshReleaseCount);
  char line[512] = "";
  size_t len = 0;
  for (uint32_t i = 0; i < kClothSlots; ++i) {
    const LONG c = g_clothCount[i];
    if (!c) continue;
    char item[96];
    const int k = snprintf(item, sizeof(item), "%s=%ld ", kClothNames[i], (long)c);
    if (len + k >= sizeof(line) - 1) {  // flush a full line
      logLine("  calls: %s", line);
      len = 0;
      line[0] = 0;
    }
    memcpy(line + len, item, k + 1);
    len += k;
  }
  if (len) logLine("  calls: %s", line);
}

}  // namespace shim
