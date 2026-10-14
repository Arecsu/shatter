// Hosted cloth (default; PXSHIM_CLOTH=off leaves cloths to the core; see cloth_ours.h): NxCloth objects backed by solver/cloth.
//
// The object handed to the game is {vtable, Hosted*}. Its 92 slots are the functions below, in the 2.8.1 NxCloth.h
// declaration order (the same order as cloth_wrap.cpp's kClothNames). Calling convention: MSVC thiscall (this in ECX,
// arguments on the stack, callee pops), written as __fastcall(self, edx, args...) so the stack cleanup matches. Struct
// returns take the hidden pointer as the first stack argument, and the function returns it in EAX. NxVec3 by value is
// three float dwords. A slot whose behaviour is not implemented logs once (unsupported()) and returns a neutral value.
//
// Solver mapping (solver/cloth public API; scene geometry, poses, filters and impulses from scene_read.h):
//  - The pxc::Cloth is built lazily at the first NxScene::simulate. Simple parameter setters (stiffnesses, damping,
//    friction, thickness, iterations, tear factor, wind/external acceleration, sleep velocity) update the running solver
//    in place (setLive); setFlags snapshots the state and rebuilds at the next step (velocities kept); a torn
//    cloth is never rebuilt. wakeUp, putToSleep and the velocity setters act on the running solver.
//  - Shapes: the scene statics the cloth's group table and mask let through (planes, spheres, boxes, capsules, and
//    oriented boxes/capsules as posed shapes) at the first build; dynamic and kinematic actors join when a step first
//    sees them (selectDynamic at the cloth's box) and park far away with their attachments released when they leave.
//    Solver shape ids are these slots in order. Triangle meshes, convexes and heightfields go in as solver meshes (the
//    statics clipped to the region the cloth can reach; see meshOf).
//  - Every substep, body shapes get their pose: the refresh pose is where the core starts the simulate, the substep
//    runs ahead of it (kinematic moves land there, as moveGlobalPosition does), with the previous pose for the sweep.
//    Inverse mass 1/m for non-kinematic bodies (NxActor::getMass), 0 otherwise.
//  - Attachments: attachToShape, attachToCollidingShapes, attachVertexToShape, detachFromShape and freeVertex are queued
//    and run at the next step, with that step's poses (the solver evaluates them then). Records per vertex keep them
//    across rebuilds; tearable attachments are dropped when the solver releases them. attachVertexToGlobalPosition pins
//    in place. NX_CLOTH_VERTEX_ATTACHED mesh vertices are pinned where they start [A: flags from saveToDesc].
//  - Two-way attachments produce reactions (solver attachReactions) applied to dynamic bodies with NX_IMPULSE.
//  - Collision filter: group, groups mask and the scene filter ops, as the fluid path (scene_read.h FluidFilterDesc).
//  - Stepping mirrors the fluid hosting: fixed substeps of the scene's maxTimestep (at most maxIter per simulate),
//    or one step of the elapsed time for variable timing.
//  - Asynchronous (PXSHIM_CLOTH_ASYNC, default on): at simulate the game thread reads the core (scene geometry, the
//    build, the body flags and masses of the shapes near each cloth) and starts one job on the cloth thread that runs
//    every substep of every cloth (the shim pool runs the cloths in parallel). Two-way reactions are applied to the core
//    at fetchResults, after the core's step (PXSHIM_CLOTH_ASYNC=0: at simulate, before the core's step, as before).
//    Calls that touch a cloth's solver wait for the job (joinFor); pure reads of the bounds use the state at simulate.
//  - Output: positions, normals (area-weighted, [A]), indices, and parent indices (the rest vertex each particle came
//    from, tear copies included) are written to the desc's NxMeshData buffers on fetchResults.
#include <windows.h>

#include <math.h>
#include <stdio.h>
#include <string.h>

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include "../solver/cloth/cloth.h"
#include "../solver/cloth/cloth_rec.h"
#include "cloth_ours.h"
#include "coverage.h"
#include "scene_read.h"
#include "shim.h"

namespace shim {
namespace {

using pxc::Cloth;
using pxc::ClothParams;
using pxc::CookedCloth;
using pxc::Edge;
using pxc::ShapeType;
using pxc::StaticShape;
using pxc::V3;

const uint32_t kSlots = 92;
// NxClothDesc byte offsets (2.8.1 layout, sizeof 280; same as cloth_wrap.cpp).
const uint32_t kDescBytes = 280;
const uint32_t kOffClothMesh = 0, kOffPose = 4, kOffPoseT = 40, kOffThickness = 52, kOffDensity = 56,
               kOffBending = 60, kOffStretching = 64, kOffDamping = 68, kOffFriction = 72, kOffPressure = 76,
               kOffTear = 80, kOffCollResp = 84, kOffAttachResp = 88, kOffAttachTear = 92, kOffToFluid = 96,
               kOffFromFluid = 100, kOffMinAdhere = 104, kOffIterations = 108, kOffExtAccel = 112, kOffWind = 124,
               kOffWakeUp = 136, kOffSleepVel = 140, kOffMeshData = 144, kOffCollGroup = 212, kOffGroupsMask = 216,
               kOffValidBounds = 236, kOffGridSpacing = 260, kOffFlags = 264, kOffUserData = 268, kOffName = 272;
// NxMeshData (68 bytes, inside the desc and returned by getMeshData)
const uint32_t kMdBytes = 68;
const uint32_t kMdPos = 0, kMdNormal = 4, kMdPosStride = 8, kMdNormStride = 12, kMdMaxVerts = 16, kMdNumVertsPtr = 20,
               kMdIdx = 24, kMdIdxStride = 28, kMdMaxIdx = 32, kMdNumIdxPtr = 36, kMdParent = 40, kMdParentStride = 44,
               kMdMaxParent = 48, kMdNumParentPtr = 52, kMdDirtyPtr = 56;
const uint32_t kNxDirtyPos = 1, kNxDirtyNormal = 2, kNxDirtyIndices = 4, kNxDirtyParent = 8;
// NxClothFlag (NxClothDesc.h)
const uint32_t kClfPressure = 1u << 0, kClfStatic = 1u << 1, kClfDisableCollision = 1u << 2,
               kClfSelfCollision = 1u << 3, kClfGravity = 1u << 5, kClfBending = 1u << 6, kClfBendingOrtho = 1u << 7,
               kClfDamping = 1u << 8, kClfCollisionTwoway = 1u << 9, kClfTriangleCollision = 1u << 11,
               kClfTearable = 1u << 12, kClfComDamping = 1u << 14, kClfValidBounds = 1u << 15,
               kClfFluidCollision = 1u << 16, kClfAdhere = 1u << 18, kClfHardware = 1u << 13;
const uint32_t kClfDisableDynamicCcd = 1u << 17;  // NX_CLF_DISABLE_DYNAMIC_CCD (NxClothDesc.h)
const uint32_t kClothMeshTearable = 1u << 8;  // NxClothMeshDesc flags (saveToDesc)
// NxClothMeshDesc (48 bytes, NxSimpleTriangleMesh 28 bytes + vertexMassStride, vertexFlagStride, vertexMasses,
// vertexFlags, weldingDistance): the per-vertex flags, in bytes from the start.
const uint32_t kCmdVertexFlagStride = 32, kCmdVertexFlags = 40;
const uint32_t kVertexAttachedGlobalFlag = 1, kVertexTearableFlag = 1u << 7;  // NxClothVertexFlags
const uint32_t kVertexAttachmentNone = 0, kVertexAttachmentGlobal = 1, kVertexAttachmentShape = 2;  // NxClothVertexAttachmentStatus
const uint32_t kAttTearable = 2;  // NxClothAttachmentFlag NX_CLOTH_ATTACHMENT_TEARABLE (1<<1)
const uint32_t kBfKinematic = 1u << 7;  // NX_BF_KINEMATIC (NxActor::readBodyFlag)
const uint32_t kActorGetMass = 46, kActorReadBodyFlag = 91;  // NxActor vtable slots (2.8.1 header; see scene_read.cpp)
const uint32_t kMaxRebuildLogs = 20;

// Values the solver does not use: stored so the getters return what was set.
struct Extra {
  float pressure = 1.0f, attachTearFactor = 1.5f, attachResp = 1.0f, collResp = 1.0f, fromFluid = 0.0f,
        toFluid = 0.0f, minAdhere = 0.0f, gridSpacing = 0.1f;
};

struct Hosted;
struct Obj {  // what the game holds: vtable pointer (slot 0 of the table is ours, index 1 onwards are the slots)
  const uint32_t* vt;
  Hosted* h;
};

// One solver shape, fixed index = its id in the solver (addShape order). Statics are added at the first build;
// dynamic and kinematic shapes join when a step first sees them and park (far away, no attachments) when they leave.
struct SolverShape {
  enum Kind : uint8_t { Static, Posed, Body } kind = Static;  // Posed: oriented static (fixed pose); Body: actor
  StaticShape geom;               // solver geometry; for posed shapes centred on the pose origin (a = 0)
  const void* key = nullptr;      // scene_read geometry pointer (findShapeGeometry / selectDynamic)
  bool posed = false;             // pose (t, R) set in the solver for this step
  V3 t;
  float R[9] = {1, 0, 0, 0, 1, 0, 0, 0, 1};
  uint32_t actor = 0;             // NxActor* (Body)
  uint32_t nx = 0;                // NxShape* (Body): the slot's identity; key follows scene_read's geometry rebuilds
  bool dynamic = false;           // non-kinematic body: two-way reactions apply to it
  bool parked = false;
  uint32_t seen = 0;              // step serial of the last selectDynamic that returned it
  V3 lin, acc;                    // body velocity at the last frame seen, and its acceleration over that frame
  uint32_t frame = 0;             // frame of the last sighting (Hosted::frame)
};

// Two-way / one-way attachment to a shape, kept per vertex so a rebuild of the solver can restore it.
struct AttRec {
  int slot = -1;
  V3 local;
  uint32_t flags = 0, nx = 0;  // nx: NxShape* as the game gave it
};

// An attach call waits for the next step: the solver needs the shapes' poses to evaluate the attachment.
struct PendingOp {
  enum Kind { ToShape, ToColliding, VertexToShape, DetachShape, FreeVertex } kind;
  uint32_t nx = 0, vertex = 0, flags = 0;
  V3 local;
};

struct Hosted {
  Obj* obj = nullptr;
  uint32_t scene = 0, mesh = 0;  // NxScene*, NxClothMesh*
  ClothParams P;
  Extra X;
  uint32_t flags = 0, group = 0, groupsMask[4] = {0, 0, 0, 0}, userData = 0;
  std::string name;
  bool meshTearable = false;
  float validBounds[6] = {0, 0, 0, 0, 0, 0};
  uint8_t md[kMdBytes] = {0};
  bool hasMd = false;
  std::vector<V3> rest;      // mesh frame (cooked positions, or saveToDesc points)
  std::vector<uint32_t> tri;  // original triangles
  std::vector<Edge> edges;    // cooked stream order (when cookedEdges)
  bool cookedEdges = false;
  std::vector<V3> x, v;       // state while the solver is absent, or snapshot before a rebuild
  // Sleep state with no solver (before the first build, or a snapshot before a rebuild). The game creates its cloths
  // and puts them to sleep in the same frame (Escape_p respawn); dropping that left them awake, and cloths resting on
  // geometry the solver does not have (triangle meshes) fell to the ground.
  bool asleep = false;
  float wakeCounter = -1.f;  // wakeUp before the build (< 0: none)
  std::map<uint32_t, V3> pins;  // attachVertexToGlobalPosition / attachToCollidingShapes (world positions)
  std::vector<SolverShape> slots;     // solver shape i = slots[i]
  std::map<const void*, int> slotOfKey;
  std::map<uint32_t, int> slotOfBody;  // NxShape* -> body slot
  // Triangle meshes handed to the solver (static: clipped to staticBox; body convexes: whole). The solver keeps raw
  // pointers: freed only after a rebuild, when no slot uses them (pruneMeshes); meshOf maps a scene shape to its copy.
  std::vector<std::unique_ptr<pxc::TriMesh>> meshStore;
  std::map<const void*, const pxc::TriMesh*> meshOfKey;
  bool staticsDone = false;
  uint32_t shapeSerial = 0;          // step counter for SolverShape::seen
  uint32_t frame = 0;                // simulate calls seen by the scene (SolverShape::frame)
  std::map<uint32_t, AttRec> att;    // vertex -> shape attachment (what the solver holds, restored on rebuild)
  std::vector<PendingOp> pending;
  std::vector<uint32_t> meshAttached;  // NX_CLOTH_VERTEX_ATTACHED vertices of the mesh (pinned at creation)
  uint32_t meshAttachedTearable = 0;
  std::unique_ptr<pxc::RecCloth> sim;
  std::shared_ptr<pxc::Recording> rec;  // PXSHIM_CLOTH_REC=1 (default off): every solver call, for cloth_replay
  uint32_t recId = 0;
  bool needRebuild = false;
  std::vector<uint8_t> removed;  // valid-bounds removals carried over a rebuild (markRebuild -> build), empty: none
  size_t removedCounted = 0;     // removals already counted for coverage
  std::vector<uint32_t> drawAt;  // output position of each particle (removed ones: a live mesh neighbour), see writeResults
  size_t drawAtRemoved = 0, drawAtSize = 0;  // removed count and particle count drawAt was built for
  bool paramsDeferred = false;  // simple parameters set during a job (setLive), applied at the next simulate
  double lastStepMs = 0;       // solver time of the last substep (stepSolve, any thread)
  Box3 staticBox{};            // region the static shapes were selected for (collectStatics)
  uint32_t requeries = 0;      // statics re-collected (requeryStatics)
  std::vector<Box3> coveredBoxes;  // every region statics were taken from since the last collection (extendStatics)
  uint32_t extensions = 0, lastExtendFrame = 0;  // extendStatics: count, and the frame of the last one
  std::map<uint32_t, const void*> pendingStaticAtt;  // vertex -> static shape key, resolved at the next collection
  uint32_t steps = 0, rebuilds = 0, skippedRebuilds = 0;
  std::vector<V3> velRead;    // what getVelocities last returned, and the step count then (stale writes, setVelocities)
  uint32_t velReadStep = 0;
  uint32_t staleWrites = 0, plainWrites = 0;
  const char* rebuildWhy = nullptr;  // the call that last asked for a rebuild (log)
  double simMs = 0;
  bool inJob = false;               // the cloth thread holds this cloth (set and cleared on the game thread)
  float boundsAtStep[6] = {0, 0, 0, 0, 0, 0};  // particle bounds when the job was started (= at the last fetch)
  // Raycasts during a job answer from the state at its start (what the game last fetched; the original's buffered API
  // also shows the pre-step state while simulating) instead of waiting for it. Taken only for cloths the game raycasts.
  bool raycasts = false;
  std::vector<V3> rayX;
  std::vector<uint32_t> rayTri;
};

// A dynamic or kinematic shape near a cloth, as the substeps read it: the scene snapshot's entry, and the body's flags
// and mass read from the core on the game thread at simulate.
struct DynRec {
  pxr::DynamicShape d;
  bool kinematic = false;
  float mass = 0;
};

struct ClothRun {  // one hosted cloth in the job
  Hosted* h = nullptr;
  std::vector<DynRec> dyn;
  std::vector<pxr::BodyImpulse> impulses;  // two-way reactions of its substeps, in order
  double ms = 0, solverMs = 0;
  uint32_t reactions = 0;
};

// One scene simulate: the job runs on the cloth thread, the game thread takes it back at the next join.
struct ClothJob {
  uint32_t scene = 0;
  const SceneGeometry* geom = nullptr;
  std::vector<ClothRun> runs;
  int steps = 0;
  float dt = 0, frameDt = 0;
  V3 g;
  double ms = 0;                           // wall time of the stepping
  std::vector<pxr::BodyImpulse> impulses;  // reactions of the finished job, applied on the game thread
};

struct AsyncStats {  // since the last oursStatsLine
  uint32_t simulates = 0, asyncJobs = 0, fetchWaits = 0, forcedWaits = 0, reactions = 0;
  double jobMs = 0, solverMs = 0, fetchWaitMs = 0, fetchWaitMax = 0, forcedWaitMs = 0;
};

struct SceneState {
  SceneGeometry* geom = nullptr;
  double accum = 0;
  float stepDt = 1.f / 60.f;  // the scene's substep (last simulate), for NxCloth force calls; before any: 1/60 [A]
};

uint32_t g_vt[1 + kSlots];  // [0] stays 0: no RTTI record for classNameOf (vt - 4 reads 0)
bool g_vtReady = false;
bool g_on = false;
std::vector<Hosted*> g_cloths;
std::map<uint32_t, CookedCloth> g_cooked;  // NxClothMesh -> cooked stream (createClothMesh tap)
std::map<uint32_t, SceneState> g_scenes;
std::set<std::string> g_unsupportedSeen;
uint32_t g_created = 0, g_released = 0, g_refused = 0, g_rebuildsTotal = 0;
double g_simMsTotal = 0;
uint32_t g_impulsesTotal = 0;  // two-way reactions handed to dynamic bodies
bool g_asyncOn = true;         // PXSHIM_CLOTH_ASYNC
bool g_edgeShape = true;      // PXSHIM_CLOTH_EDGESHAPE=0: particle-only shape contact, as the original
bool g_meshOn = true;          // PXSHIM_CLOTH_MESH=0: no scene meshes (triangle meshes, convexes, heightfields) for cloth
// The job. g_job is written by the game thread only while no job is in flight (g_jobBusy false), and read by the
// cloth thread only between the start (g_jobQueued set under g_jobM) and the end of the job.
std::mutex g_jobM;
std::condition_variable g_jobCv;
bool g_jobQueued = false;  // guarded by g_jobM: the cloth thread has the job (queued or running)
bool g_jobBusy = false;    // game thread: a job was started and not joined yet
ClothJob g_job;
AsyncStats g_as;
std::map<std::string, uint32_t> g_forcedBy;  // joins forced by a cloth call, by the call's name

Hosted* H(void* self) { return static_cast<Obj*>(self)->h; }

// PXSHIM_CLOTH_TRACE=1: the NxCloth calls the game made on hosted cloths between two simulates (name x count), logged
// at the simulate when the set of names differs from the previous frame's, so steady per-frame calls show once and
// events (respawn, script resets) stand out. Game thread only.
bool g_trace = false;
uint32_t g_traceFrame = 0;
std::map<std::string, uint32_t> g_traceCalls;
std::string g_traceLastNames;
void traceCall(const char* fn) {
  if (g_trace) ++g_traceCalls[fn];
}
void traceFlush() {
  if (!g_trace) return;
  ++g_traceFrame;
  std::string names, line;
  for (const auto& e : g_traceCalls) {
    names += e.first + " ";
    line += (line.empty() ? "" : ", ") + e.first + " x" + std::to_string(e.second);
  }
  if (names != g_traceLastNames)
    logLine("cloth trace: simulate %u, calls since the last one: %s", g_traceFrame, line.empty() ? "none" : line.c_str());
  g_traceLastNames = names;
  g_traceCalls.clear();
}

double nowMs() {
  return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now().time_since_epoch()).count();
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
V3 v3At(const uint8_t* d, uint32_t off) { return V3(fAt(d, off), fAt(d, off + 4), fAt(d, off + 8)); }
V3 v3Ptr(const void* p) {
  V3 v;
  memcpy(&v, p, 12);
  return v;
}

void unsupported(const char* what) {
  cov(kCovClothUnsupported);
  static std::mutex m;
  std::lock_guard<std::mutex> l(m);
  if (g_unsupportedSeen.insert(what).second) logLine("cloth ours: %s is not supported (logged once)", what);
}

// Coverage (docs/audit/GAPS.md rows 4, 7, 8, 22, 32): flags the solver does not implement, counted when they turn on.
void coverFlags(uint32_t before, uint32_t after) {
  const uint32_t on = after & ~before;
  if (on & kClfValidBounds) cov(kCovClothValidBoundsFlag);
  if (on & kClfPressure) cov(kCovClothPressureFlag);
  if (on & kClfCollisionTwoway) cov(kCovClothTwoWayFlag);
  if (on & kClfHardware) cov(kCovClothHardware);
  if (on & kClfAdhere) cov(kCovClothAdhere);
}

void applyFlags(Hosted* h) {
  h->P.useGravity = h->flags & kClfGravity;
  h->P.useBending = h->flags & kClfBending;
  h->P.bendingOrtho = h->flags & kClfBendingOrtho;
  h->P.useDamping = h->flags & kClfDamping;
  h->P.useComDamping = h->flags & kClfComDamping;
  h->P.useTearing = (h->flags & kClfTearable) && h->meshTearable;
  h->P.useSelfCollision = h->flags & kClfSelfCollision;
  h->P.edgeShapeContact = g_edgeShape;  // ours (cloth_edgeshape.cpp): edges kept out of thin shapes
  h->P.disableDynamicCcd = h->flags & kClfDisableDynamicCcd;
  h->P.disableCollision = h->flags & kClfDisableCollision;
  h->P.useValidBounds = h->flags & kClfValidBounds;
}

// The solver's state is the source of truth while it exists and nothing is pending.
const std::vector<V3>& posOf(const Hosted* h) { return (h->sim && !h->needRebuild) ? h->sim->pos() : h->x; }
const std::vector<V3>& velOf(const Hosted* h) { return (h->sim && !h->needRebuild) ? h->sim->vel() : h->v; }
const std::vector<uint32_t>& triOf(const Hosted* h) { return (h->sim && !h->needRebuild) ? h->sim->tri() : h->tri; }
// Valid-bounds removals (null: none).
const std::vector<uint8_t>* removedOf(const Hosted* h) {
  if (h->sim && !h->needRebuild) return h->sim->removedCount() ? &h->sim->removed() : nullptr;
  return h->removed.empty() ? nullptr : &h->removed;
}

// Before anything that rebuilds the solver: take its state (build() restores positions and velocities).
void markRebuild(Hosted* h, const char* why) {
  h->rebuildWhy = why;
  if (h->sim && !h->needRebuild) {
    h->x = h->sim->pos();
    h->v = h->sim->vel();
    h->asleep = h->sim->isSleeping();
    h->wakeCounter = -1.f;
    h->removed = h->sim->removedCount() ? h->sim->removed() : std::vector<uint8_t>();
  }
  if (h->sim) h->needRebuild = true;
}

// Tearing stops at the receive buffer's size, as in the original (a split that would not fit is skipped). Without the
// cap a torn cloth outgrew the game's buffer (212 for an 84-vertex cloth in Escape_p): the vertex count was clipped
// while the indices still pointed past it, and the game drew triangles to stale buffer entries.
uint32_t tearCapOf(const uint8_t* md) {
  const uint32_t maxV = uAt(md, kMdMaxVerts);
  return maxV ? maxV : 1;  // no buffer: no room for a copy
}

// NxActor calls (vtable slots as scene_read.cpp: the 2.8.1 header order; 2.8.0 slot order [A], see abi doc).
uint32_t actorSlot(uint32_t obj, uint32_t slot) { return fromPtr32<const uint32_t>(*fromPtr32<const uint32_t>(obj))[slot]; }
bool callBodyBool(uint32_t obj, uint32_t slot, uint32_t a) {
  return ((bool(__fastcall*)(uint32_t, void*, uint32_t))actorSlot(obj, slot))(obj, nullptr, a);
}
float callBodyF32(uint32_t obj, uint32_t slot) {
  return ((float(__fastcall*)(uint32_t, void*))actorSlot(obj, slot))(obj, nullptr);
}

// ---- scene shapes -> solver shapes (scene_read supplies geometry, poses, filters and impulses) ----------------------

// Solver geometry of a scene shape. A static keeps its centre (a). A posed shape (dynamic or kinematic body, oriented
// static) is centred on its pose origin: a = 0, and the centre goes into the pose.
// Solver copy of a scene mesh (triangle mesh, convex or heightfield; scene_read gives them as triangles). Posed shapes
// are centred on their pose origin like the others (triangles minus c). clip: keep only triangles overlapping it.
const pxc::TriMesh* meshOf(Hosted* h, const pxr::StaticShape& s, bool posed, const Box3* clip) {
  auto it = h->meshOfKey.find(&s);
  if (it != h->meshOfKey.end()) return it->second;
  auto m = std::unique_ptr<pxc::TriMesh>(new pxc::TriMesh());
  const pxr::Vec3 c = posed ? s.c : pxr::Vec3();
  for (const pxr::MeshTri& t : s.tris) {
    if (clip) {
      const float lx = fminf(t.v0.x, fminf(t.v1.x, t.v2.x)), hx = fmaxf(t.v0.x, fmaxf(t.v1.x, t.v2.x));
      const float ly = fminf(t.v0.y, fminf(t.v1.y, t.v2.y)), hy = fmaxf(t.v0.y, fmaxf(t.v1.y, t.v2.y));
      const float lz = fminf(t.v0.z, fminf(t.v1.z, t.v2.z)), hz = fmaxf(t.v0.z, fmaxf(t.v1.z, t.v2.z));
      if (hx < clip->lo.x || lx > clip->hi.x || hy < clip->lo.y || ly > clip->hi.y || hz < clip->lo.z || lz > clip->hi.z)
        continue;
    }
    for (const pxr::Vec3* v : {&t.v0, &t.v1, &t.v2}) m->v.push_back(V3(v->x - c.x, v->y - c.y, v->z - c.z));
  }
  if (m->v.empty()) {
    h->meshOfKey[&s] = nullptr;
    return nullptr;
  }
  m->build();
  const pxc::TriMesh* r = m.get();
  h->meshStore.push_back(std::move(m));
  h->meshOfKey[&s] = r;
  return r;
}

bool convertGeom(Hosted* h, const pxr::StaticShape& s, bool posed, StaticShape& out, const Box3* clip) {
  const V3 a = posed ? V3() : V3(s.c.x, s.c.y, s.c.z);
  switch (s.type) {
    case pxr::StaticShape::Plane:
      if (posed) return false;
      out = StaticShape{ShapeType::Plane, V3(s.n.x, s.n.y, s.n.z), s.d, V3()};
      return true;
    case pxr::StaticShape::Sphere:
      out = StaticShape{ShapeType::Sphere, a, s.r, V3()};
      return true;
    case pxr::StaticShape::Box:
      out = StaticShape{ShapeType::Box, a, 0, V3(s.half.x, s.half.y, s.half.z)};
      return true;
    case pxr::StaticShape::Capsule:
      out = StaticShape{ShapeType::Capsule, a, s.r, V3(0, s.h * 0.5f, 0)};
      return true;
    case pxr::StaticShape::Mesh: {  // triangle mesh, convex, heightfield
      if (!g_meshOn) return false;
      const pxc::TriMesh* m = meshOf(h, s, posed, clip);
      if (!m) return false;
      out = StaticShape{ShapeType::Mesh, V3(), 0, V3(), m};
      return true;
    }
    default:
      return false;
  }
}

// World pose of a shape whose geometry is centred at c in its own frame p (row-major R, as Pose::m).
void poseOf(const pxr::Pose& p, const pxr::Vec3& c, V3& t, float* R) {
  const pxr::Vec3 w = p.apply(c);
  t = V3(w.x, w.y, w.z);
  for (int r = 0; r < 3; ++r)
    for (int k = 0; k < 3; ++k) R[3 * r + k] = p.m[r][k];
}

// Shape frame, as cloth_attach.cpp: the pose origin when posed, else the geometric centre (planes: the origin).
V3 localOf(const SolverShape& s, const V3& x) {
  if (s.posed) {
    const V3 d = x - s.t;
    const float* r = s.R;
    return V3(r[0] * d.x + r[3] * d.y + r[6] * d.z, r[1] * d.x + r[4] * d.y + r[7] * d.z, r[2] * d.x + r[5] * d.y + r[8] * d.z);
  }
  return s.geom.type == ShapeType::Plane ? x : x - s.geom.a;
}
V3 worldOf(const SolverShape& s, const V3& l) {
  if (s.posed) {
    const float* r = s.R;
    return V3(s.t.x + r[0] * l.x + r[1] * l.y + r[2] * l.z, s.t.y + r[3] * l.x + r[4] * l.y + r[5] * l.z,
              s.t.z + r[6] * l.x + r[7] * l.y + r[8] * l.z);
  }
  return (s.geom.type == ShapeType::Plane ? V3() : s.geom.a) + l;
}

// Inside-or-touching test with radius r, the solver's touching() rule (cloth_attach.cpp) for attachToCollidingShapes.
bool touchesSlot(const SolverShape& s, const V3& x, float r) {
  const StaticShape& g = s.geom;
  const V3 q = localOf(s, x);
  switch (g.type) {
    case ShapeType::Plane:
      return g.a.dot(q) - g.d <= r;
    case ShapeType::Sphere:
      return sqrtf(q.len2()) <= g.d + r;
    case ShapeType::Box: {
      auto out = [](float v, float h) { return fmaxf(fabsf(v) - h, 0.f); };
      const V3 o(out(q.x, g.half.x), out(q.y, g.half.y), out(q.z, g.half.z));
      return sqrtf(o.len2()) <= r;
    }
    case ShapeType::Capsule: {
      V3 p = q;
      p.y = fmaxf(-g.half.y, fminf(g.half.y, p.y));
      return sqrtf((q - p).len2()) <= g.d + r;
    }
    case ShapeType::Mesh: {  // as the solver's touching(): any triangle within r
      if (!g.mesh) return false;
      bool hit = false;
      const V3 e(r, r, r);
      g.mesh->query(q - e, q + e, [&](uint32_t t) {
        const V3* v = &g.mesh->v[3 * t];
        hit = hit || (pxc::closestPointOnTriangle(q, v[0], v[1], v[2]) - q).len2() <= r * r;
      });
      return hit;
    }
  }
  return false;
}

SceneState& sceneState(uint32_t scene) {
  SceneState& st = g_scenes[scene];
  if (!st.geom) st.geom = new SceneGeometry();
  return st;
}

// The collision filter of the cloth (as a fluid's, scene_read.h): its group and groups mask, with the scene's group
// table and the filter ops applied to the mask.
FluidFilterDesc filterOf(const Hosted* h) {
  FluidFilterDesc f;
  f.collisionGroup = h->group;
  memcpy(f.groupsMask, h->groupsMask, sizeof f.groupsMask);
  f.useGroupsMask = true;  // the scene's filter ops decide (as the fluid path, scene_read.h)
  return f;
}

int addSlot(Hosted* h, SolverShape&& s) {
  const int k = (int)h->slots.size();
  if (s.key) h->slotOfKey[s.key] = k;
  h->slots.push_back(std::move(s));
  if (h->sim && !h->needRebuild) h->sim->addShape(h->slots.back().geom);
  return k;
}

// Scene statics near the cloth (once per cloth, at its first build; oursOnSimulate refreshed the geometry before).
constexpr float kStaticMargin = 2.0f;  // metres beyond the cloth's bounds + its extent
void collectStatics(Hosted* h, const SceneGeometry& geom) {
  if (h->flags & kClfDisableCollision) return;  // collected when collision is turned on (setFlags)
  h->staticsDone = true;
  FluidFilterDesc f = filterOf(h);
  f.includeStatic = true;
  f.includeDynamic = false;
  // Only the statics near the cloth: its bounds now, grown by kStaticMargin plus its own size (a hanging cloth can
  // swing by about its extent). Taking the whole level (game run 11: 1191 statics for one cloth, every vertex tested
  // against all of them each iteration) cost ~300 ms per frame.
  const std::vector<V3>& x = posOf(h);
  V3 lo(1e30f, 1e30f, 1e30f), hi(-1e30f, -1e30f, -1e30f);
  for (const V3& p : x) {
    lo = V3(fminf(lo.x, p.x), fminf(lo.y, p.y), fminf(lo.z, p.z));
    hi = V3(fmaxf(hi.x, p.x), fmaxf(hi.y, p.y), fmaxf(hi.z, p.z));
  }
  if (x.empty()) lo = hi = V3();
  const float grow = kStaticMargin + sqrtf((hi - lo).len2());
  const Box3 around{pxr::Vec3(lo.x - grow, lo.y - grow, lo.z - grow), pxr::Vec3(hi.x + grow, hi.y + grow, hi.z + grow)};
  h->staticBox = around;
  h->coveredBoxes.assign(1, around);
  h->extensions = 0;
  std::vector<const pxr::StaticShape*> sel;
  geom.selectStatic(f, around, sel);
  uint32_t skipped = 0, posed = 0;
  for (const pxr::StaticShape* s : sel) {
    SolverShape ss;
    ss.key = s;
    if (s->convex) cov(kCovClothConvexNear);
    if (!convertGeom(h, *s, s->oriented, ss.geom, &around)) {
      ++skipped;
      continue;
    }
    if (s->oriented) {
      ss.kind = SolverShape::Posed;
      poseOf(s->frame, s->c, ss.t, ss.R);
      ss.posed = true;
      ++posed;
    }
    addSlot(h, std::move(ss));
  }
  size_t meshes = 0;
  uint32_t tris = 0;
  for (const SolverShape& s : h->slots)
    if (s.geom.mesh) ++meshes, tris += (uint32_t)s.geom.mesh->triCount();
  logLine("cloth ours: %08X collision shapes: %zu static (%u oriented, posed; %zu meshes, %u triangles near the cloth), "
          "%u skipped, %zu in scene", h->obj ? ptr32(h->obj) : 0u, h->slots.size(), posed, meshes, tris, skipped,
          sel.size());
}

// The original re-queries the broadphase every step; ours collects the statics once, in a box around the cloth. When
// the cloth comes within kRequeryMargin of that box's edge (teleport by setPositions, ResetClothVertsToRefPose, a cloth
// that falls or travels with its owner) the statics are collected again around its current bounds: the static slots
// are dropped (body slots keep their state, renumbered), static meshes are clipped again, attachments to statics follow
// their shape, and the solver is rebuilt. A torn solver cannot be rebuilt: it keeps its statics and gets the ones around
// its pieces added (extendStatics).
constexpr float kRequeryMargin = 1.0f;  // metres; the box leaves at least kStaticMargin (2 m) around the cloth
bool needsStaticRequery(const Hosted* h) {
  if (!h->staticsDone || (h->flags & kClfDisableCollision)) return false;
  const std::vector<V3>& x = posOf(h);
  if (x.empty()) return false;
  const Box3& b = h->staticBox;
  const std::vector<uint8_t>* rm = removedOf(h);  // removed particles stay where they left the valid bounds
  for (size_t i = 0; i < x.size(); ++i) {
    if (rm && i < rm->size() && (*rm)[i]) continue;
    const V3& p = x[i];
    if (p.x < b.lo.x + kRequeryMargin || p.y < b.lo.y + kRequeryMargin || p.z < b.lo.z + kRequeryMargin ||
        p.x > b.hi.x - kRequeryMargin || p.y > b.hi.y - kRequeryMargin || p.z > b.hi.z - kRequeryMargin)
      return true;
  }
  return false;
}
// Torn cloths: the statics around the pieces' current bounds are added to the running solver, keeping the ones it has
// (pieces of a shredded banner flew 10-15 m and fell through floors the first region did not hold). A shape already
// held is skipped; a static mesh gets another copy with only the triangles no earlier region took (meshes are clipped
// to their region), so no surface is there twice. The region is grown by what the fastest piece travels until the next
// extension may run.
constexpr uint32_t kExtendEvery = 8;     // frames between two extensions of one cloth
constexpr uint32_t kMaxExtensions = 64;  // per collection (coveredBoxes grows by one each)
void extendStatics(Hosted* h, const SceneGeometry& geom) {
  if (h->extensions && h->frame - h->lastExtendFrame < kExtendEvery) return;
  if (h->extensions >= kMaxExtensions) return;
  const std::vector<V3>& x = posOf(h);
  const std::vector<V3>& v = velOf(h);
  const std::vector<uint8_t>* rm = removedOf(h);
  V3 lo(1e30f, 1e30f, 1e30f), hi(-1e30f, -1e30f, -1e30f);
  float vmax2 = 0;
  for (size_t i = 0; i < x.size(); ++i) {
    if (rm && i < rm->size() && (*rm)[i]) continue;
    lo = V3(fminf(lo.x, x[i].x), fminf(lo.y, x[i].y), fminf(lo.z, x[i].z));
    hi = V3(fmaxf(hi.x, x[i].x), fmaxf(hi.y, x[i].y), fmaxf(hi.z, x[i].z));
    if (i < v.size()) vmax2 = fmaxf(vmax2, v[i].len2());
  }
  if (lo.x > hi.x) return;  // every particle removed
  h->lastExtendFrame = h->frame;
  ++h->extensions;
  const float ahead = fminf(20.f, sqrtf(vmax2) * sceneState(h->scene).stepDt * (float)kExtendEvery);
  const float grow = kStaticMargin + kRequeryMargin + ahead;
  const Box3 around{pxr::Vec3(lo.x - grow, lo.y - grow, lo.z - grow), pxr::Vec3(hi.x + grow, hi.y + grow, hi.z + grow)};
  FluidFilterDesc f = filterOf(h);
  f.includeStatic = true;
  f.includeDynamic = false;
  std::vector<const pxr::StaticShape*> sel;
  geom.selectStatic(f, around, sel);
  auto overlaps = [](const Box3& b, float lx, float hx, float ly, float hy, float lz, float hz) {
    return !(hx < b.lo.x || lx > b.hi.x || hy < b.lo.y || ly > b.hi.y || hz < b.lo.z || lz > b.hi.z);
  };
  uint32_t added = 0, tris = 0;
  for (const pxr::StaticShape* s : sel) {
    SolverShape ss;
    if (s->type == pxr::StaticShape::Mesh) {
      if (!g_meshOn) continue;
      // the triangles in this region that no earlier region took (meshOf's test, on the same coordinates)
      auto m = std::unique_ptr<pxc::TriMesh>(new pxc::TriMesh());
      const pxr::Vec3 c = s->oriented ? s->c : pxr::Vec3();
      for (const pxr::MeshTri& t : s->tris) {
        const float lx = fminf(t.v0.x, fminf(t.v1.x, t.v2.x)), hx = fmaxf(t.v0.x, fmaxf(t.v1.x, t.v2.x));
        const float ly = fminf(t.v0.y, fminf(t.v1.y, t.v2.y)), hy = fmaxf(t.v0.y, fmaxf(t.v1.y, t.v2.y));
        const float lz = fminf(t.v0.z, fminf(t.v1.z, t.v2.z)), hz = fmaxf(t.v0.z, fmaxf(t.v1.z, t.v2.z));
        if (!overlaps(around, lx, hx, ly, hy, lz, hz)) continue;
        bool taken = false;
        for (const Box3& b : h->coveredBoxes) taken = taken || overlaps(b, lx, hx, ly, hy, lz, hz);
        if (taken) continue;
        for (const pxr::Vec3* q : {&t.v0, &t.v1, &t.v2}) m->v.push_back(V3(q->x - c.x, q->y - c.y, q->z - c.z));
      }
      if (m->v.empty()) continue;
      m->build();
      tris += (uint32_t)m->triCount();
      ss.geom = StaticShape{ShapeType::Mesh, V3(), 0, V3(), m.get()};
      h->meshStore.push_back(std::move(m));
      ss.key = h->slotOfKey.count(s) ? nullptr : s;  // the first slot of a shape keeps the key (attachments)
    } else {
      if (h->slotOfKey.count(s) || !convertGeom(h, *s, s->oriented, ss.geom, nullptr)) continue;
      ss.key = s;
    }
    if (s->convex) cov(kCovClothConvexNear);
    if (s->oriented) {
      ss.kind = SolverShape::Posed;
      poseOf(s->frame, s->c, ss.t, ss.R);
      ss.posed = true;
    }
    addSlot(h, std::move(ss));
    ++added;
  }
  h->staticBox = around;
  h->coveredBoxes.push_back(around);
  if (h->extensions <= kMaxRebuildLogs)
    logLine("cloth ours: %08X torn, statics extended #%u: +%u shapes (%u new mesh triangles), pieces (%.2f %.2f %.2f)-(%.2f "
            "%.2f %.2f), %zu shapes in total", ptr32(h->obj), h->extensions, added, tris, lo.x, lo.y, lo.z, hi.x, hi.y,
            hi.z, h->slots.size());
}

void requeryStatics(Hosted* h, const SceneGeometry& geom) {
  if (h->sim && h->sim->splits() > 0) {
    extendStatics(h, geom);
    return;
  }
  cov(kCovClothStaticsRequery);
  markRebuild(h, "statics re-query");  // takes the solver state before the slots change
  std::vector<int> newIndex(h->slots.size(), -1);
  std::vector<SolverShape> kept;
  for (size_t k = 0; k < h->slots.size(); ++k) {
    if (h->slots[k].kind == SolverShape::Body) {
      newIndex[k] = (int)kept.size();
      kept.push_back(std::move(h->slots[k]));
    }
  }
  // Static meshes were clipped to the old box, and shapes clipped to nothing were remembered as "no triangles": only the
  // body slots' meshes stay (pruneMeshes frees the rest once the new solver no longer points at them).
  {
    std::map<const void*, const pxc::TriMesh*> keepMesh;
    for (const SolverShape& s : kept) {
      auto it = s.key ? h->meshOfKey.find(s.key) : h->meshOfKey.end();
      if (it != h->meshOfKey.end()) keepMesh.insert(*it);
    }
    h->meshOfKey = std::move(keepMesh);
  }
  // Attachments: body slots renumbered; static ones remembered by shape and resolved after the new collection.
  std::map<uint32_t, const void*> staticAtt;
  for (auto it = h->att.begin(); it != h->att.end(); ++it) {
    AttRec& a = it->second;
    if (a.slot < 0 || (size_t)a.slot >= newIndex.size()) continue;
    if (newIndex[a.slot] >= 0) {
      a.slot = newIndex[a.slot];
    } else {
      staticAtt[it->first] = h->slots[a.slot].key;
      a.slot = -1;
    }
  }
  h->slots = std::move(kept);
  h->slotOfKey.clear();
  h->slotOfBody.clear();
  for (size_t k = 0; k < h->slots.size(); ++k) {
    if (h->slots[k].key) h->slotOfKey[h->slots[k].key] = (int)k;
    if (h->slots[k].nx) h->slotOfBody[h->slots[k].nx] = (int)k;
  }
  h->pendingStaticAtt = std::move(staticAtt);
  h->staticsDone = false;  // build() collects them around the current bounds
  ++h->requeries;
  if (h->requeries <= kMaxRebuildLogs) {
    const std::vector<V3>& x = posOf(h);
    V3 lo(1e30f, 1e30f, 1e30f), hi(-1e30f, -1e30f, -1e30f);
    for (const V3& p : x) {
      lo = V3(fminf(lo.x, p.x), fminf(lo.y, p.y), fminf(lo.z, p.z));
      hi = V3(fmaxf(hi.x, p.x), fmaxf(hi.y, p.y), fmaxf(hi.z, p.z));
    }
    const Box3& b = h->staticBox;
    logLine("cloth ours: %08X statics re-query #%u, cloth (%.2f %.2f %.2f)-(%.2f %.2f %.2f), old box (%.2f %.2f %.2f)-(%.2f "
            "%.2f %.2f)", ptr32(h->obj), h->requeries, lo.x, lo.y, lo.z, hi.x, hi.y, hi.z, b.lo.x, b.lo.y, b.lo.z, b.hi.x,
            b.hi.y, b.hi.z);
  }
}

// Solver shape of a scene NxShape (attachToShape and friends), -1 when the cloth does not collide with it.
int slotOfNx(Hosted* h, const SceneGeometry& geom, uint32_t nx) {
  const pxr::StaticShape* g = nullptr;
  if (!geom.findShapeGeometry(nx, g)) return -1;
  auto it = h->slotOfKey.find(g);
  return it == h->slotOfKey.end() ? -1 : it->second;
}

// Frees the meshes no slot uses any more (statics re-query: the copies clipped to the old box). Called once the solver
// has been rebuilt from the slots, so nothing points at them; the recording forgets their ids (an address can be reused).
void pruneMeshes(Hosted* h) {
  std::set<const pxc::TriMesh*> used;
  for (const SolverShape& s : h->slots)
    if (s.geom.mesh) used.insert(s.geom.mesh);
  std::vector<std::unique_ptr<pxc::TriMesh>> keep;
  for (auto& m : h->meshStore) {
    if (used.count(m.get())) {
      keep.push_back(std::move(m));
    } else if (h->rec) {
      h->rec->meshIds.erase(m.get());
    }
  }
  h->meshStore = std::move(keep);
}

// Solver (re)build: shapes in slot order, the attachment settings, pins and the attachments the cloth keeps.
void build(Hosted* h, const SceneGeometry& geom) {
  if (!h->staticsDone) {
    collectStatics(h, geom);
    for (const auto& pa : h->pendingStaticAtt) {  // statics re-query: attachments to statics follow their shape
      auto a = h->att.find(pa.first);
      if (a == h->att.end()) continue;
      auto k = h->slotOfKey.find(pa.second);
      if (k != h->slotOfKey.end()) a->second.slot = k->second;
      else h->att.erase(a);  // the shape is no longer near the cloth
    }
    h->pendingStaticAtt.clear();
  }
  if (h->sim && h->needRebuild && h->sim->splits() > 0) {
    // A torn solver is kept as it is: its copies have no rest data in the public API.
    if (h->skippedRebuilds++ == 0)
      logLine("cloth ours: %08X torn, parameter/position changes not applied (first: %s)", ptr32(h->obj),
              h->rebuildWhy ? h->rebuildWhy : "?");
    h->needRebuild = false;
    return;
  }
  if (h->sim) {
    ++h->rebuilds;
    ++g_rebuildsTotal;
    bool moving = false;
    for (const V3& v : h->v) moving = moving || v.len2() > 0;
    if (h->rebuilds <= kMaxRebuildLogs)
      logLine("cloth ours: %08X rebuild #%u after %s, flags %08X (velocities %s)", ptr32(h->obj), h->rebuilds,
              h->rebuildWhy ? h->rebuildWhy : "?", h->flags, moving ? "kept" : "zero");
  }
  if (!h->rec) {
    h->rec = std::make_shared<pxc::Recording>();
    h->rec->full = true;  // recording off: nothing is kept
  }
  h->sim.reset(new pxc::RecCloth(h->rec, h->P, h->rest, h->tri, h->x, h->cookedEdges ? &h->edges : nullptr, false));
  h->sim->setParallelFor(shimPoolRun);  // contacts and self contact of one cloth split over the pool (nested runs)
  // Velocities carry over (the original keeps them through setFlags and setPosition(s)); zeroing them on every rebuild
  // made cloths the game touches often stall in mid-air.
  if (h->v.size() == h->x.size())
    for (size_t i = 0; i < h->v.size(); ++i)
      if (h->v[i].len2() > 0) h->sim->setVelocity(i, h->v[i]);
  h->sim->setAttachmentResponse(h->X.attachResp);
  h->sim->setAttachmentTearFactor(h->X.attachTearFactor);
  for (const SolverShape& s : h->slots) h->sim->addShape(s.geom);
  pruneMeshes(h);
  const uint32_t n = (uint32_t)h->x.size();
  for (const auto& p : h->pins)
    if (p.first < n) h->sim->pinHard(p.first, p.second);
  for (const auto& a : h->att)
    if (a.first < n && a.second.slot >= 0) h->sim->attachVertexToShape(a.first, a.second.slot, a.second.local, a.second.flags);
  if (!h->removed.empty()) h->sim->setRemoved(h->removed);
  if (h->asleep) h->sim->putToSleep();
  else if (h->wakeCounter >= 0.f) h->sim->wakeUp(h->wakeCounter);
  h->wakeCounter = -1.f;
  h->needRebuild = false;
}

// Coverage (docs/audit/GAPS.md row 3): the cloth left the box its statics were collected for.
void coverBounds(const Hosted* h, const std::vector<V3>& x) {
  if (h->staticsDone && !(h->flags & kClfDisableCollision)) {
    const Box3& b = h->staticBox;
    for (const V3& p : x)
      if (p.x < b.lo.x || p.y < b.lo.y || p.z < b.lo.z || p.x > b.hi.x || p.y > b.hi.y || p.z > b.hi.z) {
        cov(kCovClothOutsideStaticBox);
        break;
      }
  }
}

void writeResults(Hosted* h) {
  if (!h->hasMd) return;
  const uint8_t* m = h->md;
  const std::vector<V3>& x = posOf(h);
  coverBounds(h, x);
  const std::vector<uint8_t>* rm = removedOf(h);
  if (h->sim && !h->needRebuild && h->sim->removedCount() > h->removedCounted) {  // row 4: particles removed so far
    cov(kCovClothOutsideValidBounds, (uint32_t)(h->sim->removedCount() - h->removedCounted));
    h->removedCounted = h->sim->removedCount();
  }
  // Triangles with a removed corner are collapsed to (a, a, a) in the output indices (the original does the same), and
  // the normals come from the collapsed list.
  std::vector<uint32_t> collapsed;
  const std::vector<uint32_t>* triP = &triOf(h);
  if (rm) {
    collapsed = *triP;
    for (size_t t = 0; t + 2 < collapsed.size(); t += 3) {
      const uint32_t a = collapsed[t], b = collapsed[t + 1], c = collapsed[t + 2];
      const auto dead = [&](uint32_t k) { return k < rm->size() && (*rm)[k]; };
      if (dead(a) || dead(b) || dead(c)) collapsed[t + 1] = collapsed[t + 2] = a;
    }
    triP = &collapsed;
  }
  const std::vector<uint32_t>& tri = *triP;
  // Removed particles are drawn at the nearest live particle of the mesh (ours, not in the original). They stay frozen
  // where they left the valid bounds, and the collapsed indices above hide their triangles only for a renderer that
  // reads the indices: UE3 does for tearable cloths, and for the others draws its own fixed index list [A: UE3 source,
  // not traced in the exe]. A pinned non-tearable banner whose edge swung out of its box (slow motion, game run
  // 20261012-183254 cloth015, 21 particles) then showed triangles stretched 8-21x between the frozen points and the
  // banner. Folded onto the live edge they disappear the same way with either renderer.
  const std::vector<uint32_t>* drawAt = nullptr;
  if (rm) {
    size_t nRemoved = 0;
    for (uint8_t b : *rm) nRemoved += b;
    if (h->drawAtRemoved != nRemoved || h->drawAtSize != x.size()) {
      const std::vector<uint32_t>& whole = triOf(h);
      std::vector<std::vector<uint32_t>> adj(x.size());
      for (size_t t = 0; t + 2 < whole.size(); t += 3)
        for (int k = 0; k < 3; ++k) {
          const uint32_t a = whole[t + k], b = whole[t + (k + 1) % 3];
          if (a < x.size() && b < x.size() && a != b) adj[a].push_back(b), adj[b].push_back(a);
        }
      std::vector<uint32_t>& to = h->drawAt;
      to.assign(x.size(), UINT32_MAX);
      std::vector<uint32_t> queue;
      for (uint32_t i = 0; i < x.size(); ++i)
        if (!(i < rm->size() && (*rm)[i])) to[i] = i, queue.push_back(i);
      for (size_t q = 0; q < queue.size(); ++q)  // breadth first from every live particle at once
        for (uint32_t j : adj[queue[q]])
          if (to[j] == UINT32_MAX) to[j] = to[queue[q]], queue.push_back(j);
      for (uint32_t i = 0; i < x.size(); ++i)
        if (to[i] == UINT32_MAX) to[i] = i;  // a piece removed whole: drawn where it froze
      h->drawAtRemoved = nRemoved, h->drawAtSize = x.size();
    }
    drawAt = &h->drawAt;
  }
  uint32_t n = (uint32_t)x.size();
  const uint32_t maxV = uAt(m, kMdMaxVerts);
  if (n > maxV) n = maxV;
  uint32_t bits = 0;
  uint8_t* pos = reinterpret_cast<uint8_t*>(static_cast<uintptr_t>(uAt(m, kMdPos)));
  const uint32_t pStride = uAt(m, kMdPosStride) ? uAt(m, kMdPosStride) : 12;
  if (pos) {
    for (uint32_t i = 0; i < n; ++i) memcpy(pos + i * pStride, &x[drawAt ? (*drawAt)[i] : i], 12);
    bits |= kNxDirtyPos;
  }
  uint8_t* nrm = reinterpret_cast<uint8_t*>(static_cast<uintptr_t>(uAt(m, kMdNormal)));
  if (nrm) {
    std::vector<V3> acc(n);
    for (size_t t = 0; t + 2 < tri.size(); t += 3) {
      if (tri[t] >= n || tri[t + 1] >= n || tri[t + 2] >= n) continue;
      const V3 e1 = x[tri[t + 1]] - x[tri[t]], e2 = x[tri[t + 2]] - x[tri[t]];
      const V3 c = e1.cross(e2);  // area weighted
      acc[tri[t]] += c;
      acc[tri[t + 1]] += c;
      acc[tri[t + 2]] += c;
    }
    const uint32_t nStride = uAt(m, kMdNormStride) ? uAt(m, kMdNormStride) : 12;
    for (uint32_t i = 0; i < n; ++i) {
      const V3& a = acc[drawAt && (*drawAt)[i] < n ? (*drawAt)[i] : i];
      const float l2 = a.len2();
      const V3 nv = l2 > 0 ? a * (1.0f / sqrtf(l2)) : V3(0, 0, 0);
      memcpy(nrm + i * nStride, &nv, 12);
    }
    bits |= kNxDirtyNormal;
  }
  if (uint32_t* numV = fromPtr32<uint32_t>(uAt(m, kMdNumVertsPtr))) *numV = n;
  uint32_t nIdx = (uint32_t)tri.size();
  const uint32_t maxI = uAt(m, kMdMaxIdx);
  if (nIdx > maxI) nIdx = maxI;
  uint8_t* idx = reinterpret_cast<uint8_t*>(static_cast<uintptr_t>(uAt(m, kMdIdx)));
  if (idx) {
    const uint32_t iStride = uAt(m, kMdIdxStride) ? uAt(m, kMdIdxStride) : 4;
    for (uint32_t k = 0; k < nIdx; ++k) memcpy(idx + k * iStride, &tri[k], 4);
    bits |= kNxDirtyIndices;
  }
  if (uint32_t* numI = fromPtr32<uint32_t>(uAt(m, kMdNumIdxPtr))) *numI = nIdx;
  uint8_t* par = reinterpret_cast<uint8_t*>(static_cast<uintptr_t>(uAt(m, kMdParent)));
  if (par) {  // parent = the rest vertex a particle came from (tear copies: the vertex they were split from)
    const uint32_t pStr = uAt(m, kMdParentStride) ? uAt(m, kMdParentStride) : 4;
    const uint32_t maxP = uAt(m, kMdMaxParent);
    const uint32_t np = n < maxP ? n : maxP;
    const bool live = h->sim && !h->needRebuild;
    for (uint32_t i = 0; i < np; ++i) {
      const uint32_t o = live ? (uint32_t)h->sim->origin(i) : i;
      memcpy(par + i * pStr, &o, 4);
    }
    if (uint32_t* numP = fromPtr32<uint32_t>(uAt(m, kMdNumParentPtr))) *numP = np;
    bits |= kNxDirtyParent;
  }
  if (uint32_t* dirty = fromPtr32<uint32_t>(uAt(m, kMdDirtyPtr))) *dirty |= bits;
}

// Parks a body shape that left the cloth's neighbourhood: far away, and its attachments are released (they would
// otherwise pull the vertices towards the far pose).
void park(Hosted* h, size_t k) {
  cov(kCovClothBodySlotPark);
  SolverShape& s = h->slots[k];
  const V3 parkAt(1e6f, 1e6f, 1e6f);
  const float I[9] = {1, 0, 0, 0, 1, 0, 0, 0, 1};
  h->sim->setShapeMotion((int)k, parkAt, I, nullptr, nullptr, false);
  h->sim->setShapeInvMass((int)k, 0.f);
  h->sim->setShapeVelocity((int)k, V3(), V3(), V3());
  h->sim->detachFromShape((int)k);
  s.t = parkAt;
  memcpy(s.R, I, sizeof I);
  s.posed = true;
  s.parked = true;
  s.dynamic = false;
  for (auto it = h->att.begin(); it != h->att.end();) {
    if (it->second.slot == (int)k) it = h->att.erase(it);
    else ++it;
  }
}

// Dynamic and kinematic shapes near a cloth's box, read on the game thread: the scene snapshot's shapes (selectDynamic
// with no back sweep: the same list for every substep) with each body's flags and mass from the core now.
std::vector<DynRec> nearDynamic(const Hosted* h, const SceneGeometry& geom, const Box3& box) {
  std::vector<DynRec> out;  // with NX_CLF_DISABLE_COLLISION too: bodies keep their poses for attachments
  FluidFilterDesc f = filterOf(h);
  f.includeStatic = false;
  f.includeDynamic = true;
  std::vector<pxr::DynamicShape> dyn;
  geom.selectDynamic(f, box, 0.f, 0.f, dyn);  // refresh poses (scene_read extrapolates with the velocity only)
  for (const pxr::DynamicShape& d : dyn) {
    if (!d.geom) continue;
    DynRec r;
    r.d = d;
    r.kinematic = callBodyBool(d.body, kActorReadBodyFlag, kBfKinematic);
    r.mass = callBodyF32(d.body, kActorGetMass);
    out.push_back(r);
  }
  return out;
}

// Body shapes (dynamic and kinematic actors) near the cloth, at the pose of substep `sub` (dt each). The refresh pose
// is where the core's bodies start this simulate; the substep ends `sub + 1` steps after it. A body moves by its
// velocity and, when dynamic, by its acceleration over the last frame (the core's semi-implicit steps:
// x_n = x_0 + v n dt + a dt^2 n(n+1)/2 [A, the model of run_scenario.cpp]). Oriented statics keep their pose.
void syncShapes(Hosted* h, const std::vector<DynRec>& dyn, int sub, float dt, float frameDt, V3 g, bool atBegin) {
  for (size_t k = 0; k < h->slots.size(); ++k)
    if (h->slots[k].kind == SolverShape::Posed) h->sim->setShapeMotion((int)k, h->slots[k].t, h->slots[k].R, nullptr, nullptr, false);
  const uint32_t serial = ++h->shapeSerial;
  for (const DynRec& rec : dyn) {
    const pxr::DynamicShape& d = rec.d;
    int k = -1;
    // Body slots are keyed on the NxShape: scene_read rebuilds a near actor's shape geometry now and then (new pointer,
    // same shape), which used to add a new slot each time and park the old one with its attachments.
    auto it = d.shape ? h->slotOfBody.find(d.shape) : h->slotOfBody.end();
    if (it == h->slotOfBody.end()) {
      auto ik = h->slotOfKey.find(d.geom);
      if (ik != h->slotOfKey.end()) it = h->slotOfBody.insert({d.shape, ik->second}).first;
    }
    if (it != h->slotOfBody.end() && h->slots[it->second].kind == SolverShape::Body) {
      k = it->second;
      SolverShape& old = h->slots[k];
      if (old.key != d.geom) {  // same shape, rebuilt geometry object: the lookups by geometry follow it
        auto ok = h->slotOfKey.find(old.key);
        if (ok != h->slotOfKey.end() && ok->second == k) h->slotOfKey.erase(ok);
        old.key = d.geom;
        h->slotOfKey[d.geom] = k;
      }
      if (old.parked) cov(kCovClothBodySlotReuse);
    } else {
      if (h->sim->splits() > 0) continue;  // a torn solver takes no new shapes
      SolverShape ss;
      ss.kind = SolverShape::Body;
      ss.key = d.geom;
      ss.nx = d.shape;
      if (d.geom->convex) cov(kCovClothConvexNear);
      if (!convertGeom(h, *d.geom, true, ss.geom, nullptr)) {
        continue;
      }
      k = addSlot(h, std::move(ss));
      if (d.shape) h->slotOfBody[d.shape] = k;
      cov(kCovClothBodySlotNew);
    }
    SolverShape& s = h->slots[k];
    if (s.kind != SolverShape::Body) continue;
    s.actor = d.body;
    s.seen = serial;
    const float mass = rec.mass;
    s.dynamic = !rec.kinematic;
    const float invMass = (s.dynamic && mass > 0.f) ? 1.f / mass : 0.f;
    const bool first = s.frame == 0;  // first sighting: no earlier pose, so no sweep (run_scenario: step 0)
    const V3 lv(d.linVel.x, d.linVel.y, d.linVel.z);
    if (lv.len2() > 1e-6f || d.angVel.len2() > 1e-6f) cov(kCovClothMovingBodySubsteps);
    if (s.frame != h->frame) {
      // a dynamic body seen for the first time is taken as in free fall (the native model's start); later frames use the
      // velocity change, so a body at rest has no acceleration
      const bool consecutive = s.frame + 1 == h->frame;
      s.acc = !s.dynamic ? V3() : (s.frame == 0 ? g : (consecutive ? (lv - s.lin) * (1.f / frameDt) : V3()));
      s.lin = lv;
      s.frame = h->frame;
    }
    V3 c0;
    poseOf(d.end, d.geom->c, c0, s.R);
    const float n0 = (float)sub, n1 = (float)(sub + 1);
    auto disp = [&](float n) { return s.lin * (n * dt) + s.acc * (dt * dt * n * (n + 1.f) * 0.5f); };
    const V3 tEnd = c0 + disp(n1), tBegin = c0 + disp(n0);
    float Rp[9];
    memcpy(Rp, s.R, sizeof Rp);
    s.posed = true;
    s.parked = false;
    const V3 tSet = atBegin ? tBegin : tEnd;  // attach calls see the pose at the call (the substep's start)
    s.t = tSet;
    h->sim->setShapeMotion(k, tSet, s.R, first ? nullptr : &tBegin, first ? nullptr : Rp, s.dynamic);
    h->sim->setShapeInvMass(k, invMass);
    // Surface velocity for contact friction (the original's v - v_s): the body's velocity at the end of the substep and
    // its centre of mass moved like the shape.
    const V3 com0(d.com.x, d.com.y, d.com.z);
    h->sim->setShapeVelocity(k, s.lin + s.acc * (n1 * dt), V3(d.angVel.x, d.angVel.y, d.angVel.z), com0 + disp(n1));
  }
  for (size_t k = 0; k < h->slots.size(); ++k) {
    const SolverShape& s = h->slots[k];
    if (s.kind == SolverShape::Body && !s.parked && s.seen != serial) park(h, k);
  }
}

void recordAttach(Hosted* h, uint32_t v, int slot, const V3& local, uint32_t flags, uint32_t nx) {
  AttRec& r = h->att[v];
  r.slot = slot;
  r.local = local;
  r.flags = flags;
  r.nx = nx;
}

// Attach calls of this step, in call order, against the solver with this step's shape poses.
void processPending(Hosted* h, const SceneGeometry& geom) {
  if (h->pending.empty()) return;
  pxc::RecCloth& c = *h->sim;
  const std::vector<V3>& x = c.pos();
  const float r = h->P.thickness * 0.5f;
  for (const PendingOp& op : h->pending) {
    switch (op.kind) {
      case PendingOp::ToShape: {
        const int k = slotOfNx(h, geom, op.nx);
        if (k < 0) {
          unsupported("NxCloth::attachToShape (shape is not a collision shape of this cloth)");
          break;
        }
        std::vector<bool> before(x.size());
        for (size_t i = 0; i < x.size(); ++i) before[i] = c.isAttachedToShape(i);
        c.attachToShape(k, op.flags);
        for (size_t i = 0; i < x.size(); ++i)
          if (!before[i] && c.isAttachedToShape(i)) recordAttach(h, (uint32_t)i, k, localOf(h->slots[k], x[i]), op.flags, op.nx);
        break;
      }
      case PendingOp::ToColliding: {
        // The solver's rule: each vertex that is free (no pin, no attachment) takes the first shape it touches.
        for (size_t i = 0; i < x.size(); ++i) {
          if (h->pins.count((uint32_t)i) || h->att.count((uint32_t)i)) continue;
          for (size_t k = 0; k < h->slots.size(); ++k) {
            if (!touchesSlot(h->slots[k], x[i], r)) continue;
            c.attachVertexToShape((int)i, (int)k, localOf(h->slots[k], x[i]), op.flags);
            recordAttach(h, (uint32_t)i, (int)k, localOf(h->slots[k], x[i]), op.flags, 0);
            break;
          }
        }
        c.wakeFromCall();  // as the core's attachToCollidingShapes (0x100e8cd0)
        break;
      }
      case PendingOp::VertexToShape: {
        const int k = slotOfNx(h, geom, op.nx);
        if (k < 0 || op.vertex >= x.size()) {
          unsupported("NxCloth::attachVertexToShape (shape is not a collision shape of this cloth)");
          break;
        }
        if (h->att.count(op.vertex)) break;  // the core keeps an existing shape attachment (0x100dd0b0)
        h->pins.erase(op.vertex);
        c.attachVertexToShape((int)op.vertex, k, op.local, op.flags);
        recordAttach(h, op.vertex, k, op.local, op.flags, op.nx);
        break;
      }
      case PendingOp::DetachShape: {
        const int k = slotOfNx(h, geom, op.nx);
        if (k < 0) break;
        c.detachFromShape(k);
        for (auto it = h->att.begin(); it != h->att.end();) {
          if (it->second.slot == k) it = h->att.erase(it);
          else ++it;
        }
        break;
      }
      case PendingOp::FreeVertex:
        c.freeVertex((int)op.vertex);
        h->att.erase(op.vertex);
        break;
    }
  }
  h->pending.clear();
}

void stepSolve(Hosted* h, float dt, V3 g) {
  const double t0 = nowMs();
  h->sim->step(dt, g);
  h->lastStepMs = nowMs() - t0;
}
// After a solver step (cloth thread): the reactions of the step on the dynamic bodies go to the cloth's list, and the
// torn attachments are dropped from the records.
void harvest(Hosted* h, ClothRun& r) {
  h->simMs += h->lastStepMs;
  r.solverMs += h->lastStepMs;
  ++h->steps;
  for (const pxc::AttachReaction& a : h->sim->attachReactions()) {
    if (a.shape < 0 || (size_t)a.shape >= h->slots.size()) continue;
    const SolverShape& s = h->slots[a.shape];
    if (s.kind != SolverShape::Body || !s.dynamic) continue;
    r.impulses.push_back(pxr::BodyImpulse{s.actor, pxr::Vec3(a.impulse.x, a.impulse.y, a.impulse.z),
                                          pxr::Vec3(a.point.x, a.point.y, a.point.z)});
    ++r.reactions;
  }
  for (auto it = h->att.begin(); it != h->att.end();) {
    if ((it->second.flags & kAttTearable) && !h->sim->isAttachedToShape(it->first)) it = h->att.erase(it);
    else ++it;
  }
}

// All substeps of one cloth (cloth thread, or inline with PXSHIM_CLOTH_ASYNC=0). Only the solver, the cloth's records
// and the job's own data are touched: the core was read on the game thread when the job was made.
void stepCloth(ClothRun& r, const SceneGeometry& geom, int steps, float dt, float frameDt, V3 g) {
  Hosted* h = r.h;
  const double t0 = nowMs();
  // NX_CLF_STATIC: the original skips the whole step for such a cloth (re_cloth_stepAll, docs/re-notes-cloth.md):
  // frozen where it is, velocities kept for when the flag goes. The game freezes cloths this way.
  if (h->flags & kClfStatic) {
    r.ms = nowMs() - t0;
    return;
  }
  for (int s = 0; s < steps; ++s) {
    if (s == 0 && !h->pending.empty()) {  // attach calls since the last simulate, with this step's poses
      syncShapes(h, r.dyn, s, dt, frameDt, g, true);
      processPending(h, geom);
    }
    syncShapes(h, r.dyn, s, dt, frameDt, g, false);
    stepSolve(h, dt, g);
    harvest(h, r);
  }
  r.ms = nowMs() - t0;
}

// The cloths of a job are stepped in parallel on the shim pool (the cloth thread helps, as the pool's callers do).
void runJob(ClothJob& j) {
  const double t0 = nowMs();
  shimPoolRun(j.runs.size(), [](void* c, size_t i) {
    ClothJob& job = *static_cast<ClothJob*>(c);
    stepCloth(job.runs[i], *job.geom, job.steps, job.dt, job.frameDt, job.g);
  }, &j);
  j.ms = nowMs() - t0;
}

// The cloth thread: runs one job at a time. At most one job is outstanding (joinJob waits for it).
void clothThread() {
  for (;;) {
    std::unique_lock<std::mutex> l(g_jobM);
    g_jobCv.wait(l, [] { return g_jobQueued; });
    l.unlock();
    runJob(g_job);
    l.lock();
    g_jobQueued = false;
    g_jobCv.notify_all();
  }
}

// Game thread, after the job has finished: the per-cloth stats, the cloths are free again, and the reactions are
// gathered (applied by flushReactions).
void finishJob() {
  g_as.jobMs += g_job.ms;
  for (ClothRun& r : g_job.runs) {
    r.h->inJob = false;
    g_as.solverMs += r.solverMs;
    g_as.reactions += r.reactions;
    g_simMsTotal += r.solverMs;
    g_impulsesTotal += r.reactions;
    g_job.impulses.insert(g_job.impulses.end(), r.impulses.begin(), r.impulses.end());
  }
  g_job.runs.clear();
}

// Waits for the job in flight (if any), then finishJob. byCall: the cloth call that forced the wait (nullptr: the
// expected join at fetchResults or the next simulate). Returns the time waited in ms.
double joinJob(const char* byCall) {
  if (!g_jobBusy) return 0;
  const double t0 = nowMs();
  {
    std::unique_lock<std::mutex> l(g_jobM);
    g_jobCv.wait(l, [] { return !g_jobQueued; });
  }
  const double waited = nowMs() - t0;
  g_jobBusy = false;
  finishJob();
  if (byCall) {
    ++g_forcedBy[byCall];
    ++g_as.forcedWaits;
    g_as.forcedWaitMs += waited;
  }
  return waited;
}

// A cloth call that reads or writes the solver of `h` (or its records) joins the job first when it holds the cloth.
void joinFor(const Hosted* h, const char* byCall) {
  if (h->inJob) joinJob(byCall);
}

// The reactions of the finished job go to the core now (game thread, after the core's step: at fetchResults).
void flushReactions() {
  if (g_job.impulses.empty()) return;
  if (g_job.geom) g_job.geom->applyImpulses(g_job.impulses);
  g_job.impulses.clear();
}

// Particle bounds (no thickness); an empty cloth gives zeros.
// Removed particles (valid bounds) are left out.
void pointBounds(const std::vector<V3>& x, float* out, const std::vector<uint8_t>* rm = nullptr) {
  V3 lo(1e30f, 1e30f, 1e30f), hi(-1e30f, -1e30f, -1e30f);
  bool any = false;
  for (size_t i = 0; i < x.size(); ++i) {
    if (rm && i < rm->size() && (*rm)[i]) continue;
    const V3& p = x[i];
    any = true;
    lo = V3(fminf(lo.x, p.x), fminf(lo.y, p.y), fminf(lo.z, p.z));
    hi = V3(fmaxf(hi.x, p.x), fmaxf(hi.y, p.y), fmaxf(hi.z, p.z));
  }
  if (!any) lo = hi = V3();
  out[0] = lo.x, out[1] = lo.y, out[2] = lo.z;
  out[3] = hi.x, out[4] = hi.y, out[5] = hi.z;
}

// Simple parameters go straight into the running solver (it reads them every step): no rebuild, velocities kept.
// UE3 sets wind and similar values every frame.
template <class T>
void setLive(Hosted* h, T pxc::ClothParams::*field) {
  if (h->inJob) {  // mid-step: no wait; applied at the next simulate (applyDeferredParams), as PhysX buffers API changes
    h->paramsDeferred = true;
    return;
  }
  if (h->sim && !h->needRebuild) h->sim->params().*field = h->P.*field;
}
void setLiveBounds(Hosted* h) {  // valid-bounds box, as setLive
  if (h->inJob) {
    h->paramsDeferred = true;
    return;
  }
  if (h->sim && !h->needRebuild) {
    memcpy(h->sim->params().validLo, h->P.validLo, sizeof h->P.validLo);
    memcpy(h->sim->params().validHi, h->P.validHi, sizeof h->P.validHi);
  }
}
// The simple parameters set while the cloth was in a job, copied into the solver before the next job starts.
void applyDeferredParams(Hosted* h) {
  if (!h->paramsDeferred) return;
  h->paramsDeferred = false;
  if (!h->sim || h->needRebuild) return;
  pxc::ClothParams& q = h->sim->params();
  q.bending = h->P.bending, q.stretching = h->P.stretching, q.damping = h->P.damping, q.friction = h->P.friction;
  q.tearFactor = h->P.tearFactor, q.thickness = h->P.thickness, q.solverIterations = h->P.solverIterations;
  q.externalAcceleration = h->P.externalAcceleration, q.windAcceleration = h->P.windAcceleration;
  q.sleepLinearVelocity = h->P.sleepLinearVelocity, q.maxVertices = h->P.maxVertices;
  q.useGravity = h->P.useGravity, q.useBending = h->P.useBending, q.bendingOrtho = h->P.bendingOrtho;
  q.useDamping = h->P.useDamping, q.useComDamping = h->P.useComDamping, q.useTearing = h->P.useTearing;
  q.useSelfCollision = h->P.useSelfCollision, q.disableDynamicCcd = h->P.disableDynamicCcd;
  q.disableCollision = h->P.disableCollision, q.useValidBounds = h->P.useValidBounds;
  memcpy(q.validLo, h->P.validLo, sizeof q.validLo), memcpy(q.validHi, h->P.validHi, sizeof q.validHi);
}

// ---- NxCloth slots (2.8.1 NxCloth.h order) -----------------------------------------------------

// SELF: the entry point may touch the solver or the cloth's records, so it waits for a job that holds the cloth.
// SELF_NJ: it only reads fields the job never writes (parameters, flags, names, the mesh and the desc's buffers).
#define SELF Hosted* h = H(self); (void)edx; traceCall(__func__); joinFor(h, __func__)
#define SELF_NJ Hosted* h = H(self); (void)edx; traceCall(__func__)
#define GETTER(name, expr)                                     \
  float __fastcall name(void* self, void* edx) {               \
    SELF_NJ;                                                   \
    return (expr);                                             \
  }

void __fastcall o_dtor(void* self, void* edx, uint32_t flags) {
  (void)self; (void)edx; (void)flags;  // the game releases through the scene
}
bool __fastcall o_saveToDesc(void* self, void* edx, void* desc) {
  (void)self; (void)edx; (void)desc;
  unsupported("NxCloth::saveToDesc");
  return false;
}
uint32_t __fastcall o_getClothMesh(void* self, void* edx) {
  SELF_NJ;
  return h->mesh;
}
void __fastcall o_setBendingStiffness(void* self, void* edx, float v) {
  SELF_NJ;
  h->P.bending = v;
  setLive(h, &pxc::ClothParams::bending);
}
GETTER(o_getBendingStiffness, h->P.bending)
void __fastcall o_setStretchingStiffness(void* self, void* edx, float v) {
  SELF_NJ;
  h->P.stretching = v;
  setLive(h, &pxc::ClothParams::stretching);
}
GETTER(o_getStretchingStiffness, h->P.stretching)
void __fastcall o_setDampingCoefficient(void* self, void* edx, float v) {
  SELF_NJ;
  h->P.damping = v;
  setLive(h, &pxc::ClothParams::damping);
}
GETTER(o_getDampingCoefficient, h->P.damping)
void __fastcall o_setFriction(void* self, void* edx, float v) {
  SELF_NJ;
  h->P.friction = v;
  setLive(h, &pxc::ClothParams::friction);
}
GETTER(o_getFriction, h->P.friction)
void __fastcall o_setPressure(void* self, void* edx, float v) {
  SELF;
  h->X.pressure = v;
  if (h->flags & kClfPressure) cov(kCovClothSetPressure);
  unsupported("NxCloth::setPressure (stored only)");
}
GETTER(o_getPressure, h->X.pressure)
void __fastcall o_setTearFactor(void* self, void* edx, float v) {
  SELF_NJ;
  h->P.tearFactor = v;
  setLive(h, &pxc::ClothParams::tearFactor);
}
GETTER(o_getTearFactor, h->P.tearFactor)
void __fastcall o_setAttachmentTearFactor(void* self, void* edx, float v) {
  SELF;
  h->X.attachTearFactor = v;
  if (h->sim) h->sim->setAttachmentTearFactor(v);
}
GETTER(o_getAttachmentTearFactor, h->X.attachTearFactor)
void __fastcall o_setThickness(void* self, void* edx, float v) {
  SELF_NJ;
  h->P.thickness = v;
  setLive(h, &pxc::ClothParams::thickness);
}
GETTER(o_getThickness, h->P.thickness)
GETTER(o_getDensity, h->P.density)
GETTER(o_getRelativeGridSpacing, h->X.gridSpacing)
uint32_t __fastcall o_getSolverIterations(void* self, void* edx) {
  SELF_NJ;
  return h->P.solverIterations;
}
void __fastcall o_setSolverIterations(void* self, void* edx, uint32_t n) {
  SELF_NJ;
  h->P.solverIterations = n ? n : 1;
  setLive(h, &pxc::ClothParams::solverIterations);
}
// NxCloth::getWorldBounds is a pure read: while a job holds the cloth it reports the state of the last fetch (what the
// SDK reports between simulate and fetchResults), so it does not wait for the job.
void __fastcall o_getWorldBounds(void* self, void* edx, float* bounds) {  // NxBounds3: min, max
  SELF_NJ;
  float b[6];
  if (h->inJob) memcpy(b, h->boundsAtStep, sizeof b);
  else pointBounds(posOf(h), b, removedOf(h));
  const float t = h->P.thickness;
  bounds[0] = b[0] - t, bounds[1] = b[1] - t, bounds[2] = b[2] - t;
  bounds[3] = b[3] + t, bounds[4] = b[4] + t, bounds[5] = b[5] + t;
}
void __fastcall o_attachToShape(void* self, void* edx, const void* shape, uint32_t flags) {
  SELF;
  h->pending.push_back(PendingOp{PendingOp::ToShape, ptr32(shape), 0, flags, V3()});
}
void __fastcall o_attachToCollidingShapes(void* self, void* edx, uint32_t flags) {
  SELF;
  cov(kCovClothAttachColliding);
  h->pending.push_back(PendingOp{PendingOp::ToColliding, 0, 0, flags, V3()});
}
void __fastcall o_detachFromShape(void* self, void* edx, const void* shape) {
  SELF;
  h->pending.push_back(PendingOp{PendingOp::DetachShape, ptr32(shape), 0, 0, V3()});
}
void __fastcall o_attachVertexToShape(void* self, void* edx, uint32_t vid, const void* shape, const void* localPos,
                                      uint32_t flags) {
  SELF;
  h->pending.push_back(PendingOp{PendingOp::VertexToShape, ptr32(shape), vid, flags, v3Ptr(localPos)});
}
void __fastcall o_attachVertexToGlobalPosition(void* self, void* edx, uint32_t vid, const void* pos) {
  SELF;
  if (vid >= posOf(h).size()) return;
  // The core refuses a vertex attached to a shape, and wakes the cloth otherwise (Cloth slot 90, 0x100e5800). UE3 calls
  // this every frame for its fixed vertices, so the game's cloths never sleep in the original.
  if (h->att.count(vid)) return;
  const V3 p = v3Ptr(pos);
  h->pins[vid] = p;
  if (h->sim && !h->needRebuild) {
    h->sim->attachVertexToGlobalPosition((int)vid, p);
  } else {  // applied at the build
    h->asleep = false;
    h->wakeCounter = h->P.wakeUpCounter;
  }
}
void __fastcall o_freeVertex(void* self, void* edx, uint32_t vid) {
  SELF;
  h->pins.erase(vid);
  h->pending.push_back(PendingOp{PendingOp::FreeVertex, 0, vid, 0, V3()});
}
void __fastcall o_dominateVertex(void* self, void* edx, uint32_t vid, float expiration, float weight) {
  (void)self; (void)edx; (void)vid; (void)expiration; (void)weight;
  unsupported("NxCloth::dominateVertex");
}
uint32_t __fastcall o_getVertexAttachmentStatus(void* self, void* edx, uint32_t vid) {
  SELF;
  if (h->pins.count(vid)) return kVertexAttachmentGlobal;
  if (h->att.count(vid)) return kVertexAttachmentShape;
  return kVertexAttachmentNone;
}
void* __fastcall o_getVertexAttachmentShape(void* self, void* edx, uint32_t vid) {
  SELF;
  auto it = h->att.find(vid);
  if (it != h->att.end() && !it->second.nx) cov(kCovClothAttachShapeNull);
  return it != h->att.end() ? fromPtr32<void>(it->second.nx) : nullptr;
}
void* __fastcall o_getVertexAttachmentPosition(void* self, void* edx, V3* ret, uint32_t vid) {
  SELF;
  *ret = V3();
  auto p = h->pins.find(vid);
  auto it = h->att.find(vid);
  if (p != h->pins.end()) *ret = p->second;
  else if (it != h->att.end() && it->second.slot >= 0 && (size_t)it->second.slot < h->slots.size())
    *ret = worldOf(h->slots[it->second.slot], it->second.local);
  return ret;
}
void __fastcall o_attachToCore(void* self, void* edx, void* actor, float threshold, float depth, float maxDef) {
  (void)self; (void)edx; (void)actor; (void)threshold; (void)depth; (void)maxDef;
  cov(kCovClothAttachToCore);
  unsupported("NxCloth::attachToCore");
}
bool __fastcall o_tearVertex(void* self, void* edx, uint32_t vid, const void* normal) {
  (void)self; (void)edx; (void)vid; (void)normal;
  unsupported("NxCloth::tearVertex");
  return false;
}
// NxCloth::raycast (2.8.1 NxCloth.h). Rule as the original's kind-1 cloth path (re-notes: NpCloth slot 36 -> Cloth
// slot 99 -> triangle loop): (1) the ray must pass a slab test against the box of the current particle positions
// (1e-5 slack; an origin inside the box passes); (2) every triangle of the current list is tested with an unbounded,
// two-sided ray (the direction need not be unit; edges inclusive, t >= 0); (3) the hit nearest the ray origin wins,
// the first triangle on ties; (4) the id is the corner of that triangle nearest the hit point, ties going to the
// later corner (third over second over first). Sleep and tearing do not matter.
namespace {
bool rayBoxPass(const V3& o, const V3& d, const V3& lo, const V3& hi) {
  const float oo[3] = {o.x, o.y, o.z}, dd[3] = {d.x, d.y, d.z};
  const float l[3] = {lo.x, lo.y, lo.z}, u[3] = {hi.x, hi.y, hi.z};
  if (oo[0] >= l[0] && oo[0] <= u[0] && oo[1] >= l[1] && oo[1] <= u[1] && oo[2] >= l[2] && oo[2] <= u[2]) return true;
  float te[3];
  int k = 0;
  for (int i = 0; i < 3; ++i) {
    te[i] = dd[i] == 0.f ? -1.f : ((dd[i] > 0.f ? l[i] : u[i]) - oo[i]) / dd[i];
    if (te[i] > te[k]) k = i;
  }
  if (te[k] < 0.f) return false;
  for (int i = 0; i < 3; ++i) {  // the entry point on the other two axes must lie inside the box
    if (i == k) continue;
    const float p = oo[i] + dd[i] * te[k];
    if (p < l[i] - 1e-5f || p > u[i] + 1e-5f) return false;
  }
  return true;
}
bool rayTriHit(const V3& o, const V3& d, const V3& a, const V3& b, const V3& c, V3& hit) {
  const V3 n = (b - a).cross(c - a);
  const float denom = d.dot(n);
  if (denom == 0.f) return false;
  const float t = (a - o).dot(n) / denom;
  if (t < 0.f) return false;
  const V3 p = o + d * t;
  if (n.dot((b - p).cross(c - p)) < 0.f) return false;
  if (n.dot((c - p).cross(a - p)) < 0.f) return false;
  if (n.dot((a - p).cross(b - p)) < 0.f) return false;
  hit = p;
  return true;
}
bool clothRaycast(const std::vector<V3>& x, const std::vector<uint32_t>& tri, const V3& o, const V3& d, V3& hit,
                  uint32_t& vid) {
  if (x.empty()) return false;
  V3 lo = x[0], hi = x[0];
  for (const V3& p : x) {
    lo = V3(fminf(lo.x, p.x), fminf(lo.y, p.y), fminf(lo.z, p.z));
    hi = V3(fmaxf(hi.x, p.x), fmaxf(hi.y, p.y), fmaxf(hi.z, p.z));
  }
  if (!rayBoxPass(o, d, lo, hi)) return false;
  bool found = false;
  float best = 3.4028235e38f;
  for (size_t t = 0; t + 2 < tri.size(); t += 3) {
    const uint32_t i0 = tri[t], i1 = tri[t + 1], i2 = tri[t + 2];
    if (i0 >= x.size() || i1 >= x.size() || i2 >= x.size()) continue;
    V3 p;
    if (!rayTriHit(o, d, x[i0], x[i1], x[i2], p)) continue;
    const float dO = (p - o).len2();
    if (!(dO < best)) continue;
    const float d0 = (p - x[i0]).len2(), d1 = (p - x[i1]).len2(), d2 = (p - x[i2]).len2();
    vid = (d1 <= d0 || d2 <= d0) ? ((d2 <= d1) ? i2 : i1) : i0;
    best = dO;
    hit = p;
    found = true;
  }
  return found;
}
}  // namespace
bool __fastcall o_raycast(void* self, void* edx, const void* ray, void* hit, uint32_t* vid) {  // const NxRay&, NxVec3&, NxU32&
  SELF_NJ;
  const bool snap = h->inJob && h->raycasts;  // the snapshot taken at the job's start (see Hosted::raycasts)
  if (!snap) joinFor(h, __func__);  // first raycast on this cloth: wait once, snapshots from the next job on
  h->raycasts = true;
  const float* r = static_cast<const float*>(ray);  // orig[3], dir[3]
  V3 p;
  uint32_t v = 0;
  if (!clothRaycast(snap ? h->rayX : posOf(h), snap ? h->rayTri : triOf(h), V3(r[0], r[1], r[2]),
                    V3(r[3], r[4], r[5]), p, v))
    return false;
  float* o = static_cast<float*>(hit);
  o[0] = p.x, o[1] = p.y, o[2] = p.z;
  *vid = v;
  return true;
}
void __fastcall o_setGroup(void* self, void* edx, uint32_t g) {
  SELF;
  h->group = g;
}
uint32_t __fastcall o_getGroup(void* self, void* edx) {
  SELF_NJ;
  return h->group;
}
void __fastcall o_setGroupsMask(void* self, void* edx, const void* mask) {
  SELF;
  memcpy(h->groupsMask, mask, 16);
}
void* __fastcall o_getGroupsMask(void* self, void* edx, void* ret) {
  SELF_NJ;
  memcpy(ret, h->groupsMask, 16);
  return ret;
}
void __fastcall o_setMeshData(void* self, void* edx, const void* md) {
  SELF;
  memcpy(h->md, md, kMdBytes);
  h->hasMd = true;
  h->P.maxVertices = tearCapOf(h->md);
  setLive(h, &pxc::ClothParams::maxVertices);
}
void* __fastcall o_getMeshData(void* self, void* edx, void* ret) {
  SELF_NJ;
  if (h->hasMd) memcpy(ret, h->md, kMdBytes);
  else memset(ret, 0, kMdBytes);
  return ret;
}
void __fastcall o_setValidBounds(void* self, void* edx, const float* b) {
  SELF;
  memcpy(h->validBounds, b, 24);
  memcpy(h->P.validLo, b, 12), memcpy(h->P.validHi, b + 3, 12);
  setLiveBounds(h);
  cov(kCovClothSetValidBounds);
}
void __fastcall o_getValidBounds(void* self, void* edx, float* b) {
  SELF_NJ;
  memcpy(b, h->validBounds, 24);
}
void __fastcall o_setPosition(void* self, void* edx, const void* p, uint32_t vid) {
  SELF;
  if (vid >= posOf(h).size()) return;
  cov(kCovClothSetPositions);
  markRebuild(h, "setPosition");
  h->x[vid] = v3Ptr(p);
}
void __fastcall o_setPositions(void* self, void* edx, const void* buf, uint32_t stride) {
  SELF;
  const uint8_t* b = static_cast<const uint8_t*>(buf);
  cov(kCovClothSetPositions);
  markRebuild(h, "setPositions");
  if (h->x.empty()) return;
  for (uint32_t i = 0; i < h->x.size(); ++i) h->x[i] = v3Ptr(b + i * (stride ? stride : 12));
}
V3* __fastcall o_getPosition(void* self, void* edx, V3* ret, uint32_t vid) {
  SELF;
  *ret = vid < posOf(h).size() ? posOf(h)[vid] : V3();
  return ret;
}
void __fastcall o_getPositions(void* self, void* edx, void* buf, uint32_t stride) {
  SELF;
  uint8_t* b = static_cast<uint8_t*>(buf);
  const std::vector<V3>& x = posOf(h);
  for (uint32_t i = 0; i < x.size(); ++i) memcpy(b + i * (stride ? stride : 12), &x[i], 12);
}
void __fastcall o_setVelocity(void* self, void* edx, const void* v, uint32_t vid) {  // const NxVec3&, vertex id
  SELF;
  const float* f = static_cast<const float*>(v);
  if (h->sim && !h->needRebuild) h->sim->setVelocity(vid, V3(f[0], f[1], f[2]));
  else if (vid < h->v.size()) h->v[vid] = V3(f[0], f[1], f[2]);
}
// The game reads the velocities, adds its wind and writes them back every frame (game replay: written = read + a small
// constant). In the original nothing steps between the two calls. Here a cloth step can (the asynchronous cloth
// thread, the fixed cloth clock): a plain write then put back the velocities from before that step and erased what it
// did (a hit, a fall). So when a step ran since the last read, the game's change (written - read) is added to the
// current velocities instead; otherwise it is the plain write the original does.
void __fastcall o_setVelocities(void* self, void* edx, const void* buf, uint32_t stride) {  // NxVec3 per vertex
  SELF;
  const bool live = h->sim && !h->needRebuild;
  const size_t n = live ? h->sim->vel().size() : h->v.size();
  if (!stride) stride = 12;
  const bool stale = live && h->steps != h->velReadStep && h->velRead.size() == n;
  ++(stale ? h->staleWrites : h->plainWrites);
  if (stale && (h->staleWrites == 1 || h->staleWrites % 5000 == 0))
    logLine("cloth ours: %08X setVelocities after a step since getVelocities: applied as a change (%u such writes, %u "
            "plain)", ptr32(h->obj), h->staleWrites, h->plainWrites);
  const bool known = h->velRead.size() == n;  // the game's change is the write minus what it read
  for (size_t i = 0; i < n; ++i) {
    const float* f = reinterpret_cast<const float*>(static_cast<const char*>(buf) + i * stride);
    V3 w(f[0], f[1], f[2]);
    const V3 change = w - (known ? h->velRead[i] : velOf(h)[i]);
    if (stale) w = h->sim->vel()[i] + change;
    if (live) h->sim->setGameVelocity(i, w, change);
    else h->v[i] = w;
  }
}
V3* __fastcall o_getVelocity(void* self, void* edx, V3* ret, uint32_t vid) {
  SELF;
  *ret = vid < velOf(h).size() ? velOf(h)[vid] : V3();
  return ret;
}
void __fastcall o_getVelocities(void* self, void* edx, void* buf, uint32_t stride) {
  SELF;
  uint8_t* b = static_cast<uint8_t*>(buf);
  const std::vector<V3>& v = velOf(h);
  h->velRead = v;
  h->velReadStep = h->steps;
  for (uint32_t i = 0; i < v.size(); ++i) memcpy(b + i * (stride ? stride : 12), &v[i], 12);
}
uint32_t __fastcall o_getNumberOfParticles(void* self, void* edx) {
  SELF;
  return (uint32_t)posOf(h).size();
}
uint32_t __fastcall o_queryShapePointers(void* self, void* edx) {
  (void)self; (void)edx;
  return 0;
}
uint32_t __fastcall o_getStateByteSize(void* self, void* edx) {
  (void)self; (void)edx;
  return 0;
}
void __fastcall o_getShapePointers(void* self, void* edx, void** shapes, uint32_t* flags) {
  (void)self; (void)edx; (void)shapes; (void)flags;
}
void __fastcall o_setShapePointers(void* self, void* edx, void** shapes, uint32_t n) {
  (void)self; (void)edx; (void)shapes; (void)n;
  unsupported("NxCloth::setShapePointers");
}
void __fastcall o_saveStateToStream(void* self, void* edx, void* stream, bool permute) {
  (void)self; (void)edx; (void)stream; (void)permute;
  unsupported("NxCloth::saveStateToStream");
}
void __fastcall o_loadStateFromStream(void* self, void* edx, void* stream) {
  (void)self; (void)edx; (void)stream;
  unsupported("NxCloth::loadStateFromStream");
}
// Coefficients the solver does not use: stored for the getters.
void __fastcall o_setCollisionResponseCoefficient(void* self, void* edx, float v) {
  SELF;
  h->X.collResp = v;
  if (h->flags & kClfCollisionTwoway) cov(kCovClothSetCollResponse);
}
GETTER(o_getCollisionResponseCoefficient, h->X.collResp)
void __fastcall o_setAttachmentResponseCoefficient(void* self, void* edx, float v) {
  SELF;
  h->X.attachResp = v;
  if (h->sim) h->sim->setAttachmentResponse(v);
}
GETTER(o_getAttachmentResponseCoefficient, h->X.attachResp)
void __fastcall o_setFromFluidResponseCoefficient(void* self, void* edx, float v) {
  SELF;
  h->X.fromFluid = v;
}
GETTER(o_getFromFluidResponseCoefficient, h->X.fromFluid)
void __fastcall o_setToFluidResponseCoefficient(void* self, void* edx, float v) {
  SELF;
  h->X.toFluid = v;
}
GETTER(o_getToFluidResponseCoefficient, h->X.toFluid)
void __fastcall o_setExternalAcceleration(void* self, void* edx, float x, float y, float z) {
  SELF_NJ;
  h->P.externalAcceleration = V3(x, y, z);
  setLive(h, &pxc::ClothParams::externalAcceleration);
}
V3* __fastcall o_getExternalAcceleration(void* self, void* edx, V3* ret) {
  SELF_NJ;
  *ret = h->P.externalAcceleration;
  return ret;
}
void __fastcall o_setMinAdhereVelocity(void* self, void* edx, float v) {
  SELF;
  h->X.minAdhere = v;
  cov(kCovClothAdhere);
  unsupported("NxCloth::setMinAdhereVelocity (stored only)");
}
GETTER(o_getMinAdhereVelocity, h->X.minAdhere)
void __fastcall o_setWindAcceleration(void* self, void* edx, float x, float y, float z) {
  SELF_NJ;
  h->P.windAcceleration = V3(x, y, z);
  setLive(h, &pxc::ClothParams::windAcceleration);
}
V3* __fastcall o_getWindAcceleration(void* self, void* edx, V3* ret) {
  SELF_NJ;
  *ret = h->P.windAcceleration;
  return ret;
}
bool __fastcall o_isSleeping(void* self, void* edx) {
  SELF;
  return (h->sim && !h->needRebuild) ? h->sim->isSleeping() : h->asleep;
}
GETTER(o_getSleepLinearVelocity, h->P.sleepLinearVelocity)
void __fastcall o_setSleepLinearVelocity(void* self, void* edx, float v) {
  SELF_NJ;
  h->P.sleepLinearVelocity = v;
  setLive(h, &pxc::ClothParams::sleepLinearVelocity);
}
void __fastcall o_wakeUp(void* self, void* edx, float counter) {
  SELF;
  if (h->sim && !h->needRebuild) {
    h->sim->wakeUp(counter);
  } else {  // applied at the build
    h->asleep = false;
    h->wakeCounter = counter;
  }
}
void __fastcall o_putToSleep(void* self, void* edx) {
  SELF;
  if (h->sim && !h->needRebuild) {
    h->sim->putToSleep();
  } else {  // applied at the build (sleeping clears the velocities, as the solver does)
    h->asleep = true;
    h->wakeCounter = -1.f;
    for (V3& v : h->v) v = V3();
  }
}
void __fastcall o_setFlags(void* self, void* edx, uint32_t flags) {
  SELF;
  const uint32_t changed = h->flags ^ flags;
  coverFlags(h->flags, flags);
  if ((changed & kClfDisableCollision) && h->sim) cov(kCovClothDisableCollToggle);
  if (g_trace && changed) logLine("cloth trace: %08X setFlags %08X -> %08X", ptr32(h->obj), h->flags, flags);
  h->flags = flags;
  applyFlags(h);
  // The solver reads every flag setting per step, so they go into the running cloth (torn ones included, which cannot
  // be rebuilt). Only a collision on/off switch rebuilds: the static shapes are collected at the first build.
  // NX_CLF_DISABLE_COLLISION goes into the running solver like the others (contacts off, shapes kept for attachments);
  // turning collision on for a cloth whose statics were never collected (created without collision) rebuilds it.
  if ((changed & kClfDisableCollision) && !(flags & kClfDisableCollision) && !h->staticsDone) {
    markRebuild(h, "setFlags");
    return;
  }
  setLive(h, &pxc::ClothParams::disableCollision);
  setLive(h, &pxc::ClothParams::useGravity);
  setLive(h, &pxc::ClothParams::useBending);
  setLive(h, &pxc::ClothParams::bendingOrtho);
  setLive(h, &pxc::ClothParams::useDamping);
  setLive(h, &pxc::ClothParams::useComDamping);
  setLive(h, &pxc::ClothParams::useTearing);
  setLive(h, &pxc::ClothParams::useSelfCollision);
  setLive(h, &pxc::ClothParams::disableDynamicCcd);
  setLive(h, &pxc::ClothParams::useValidBounds);
}
uint32_t __fastcall o_getFlags(void* self, void* edx) {
  SELF_NJ;
  return h->flags;
}
void __fastcall o_setName(void* self, void* edx, const char* name) {
  SELF;
  h->name = name && !IsBadStringPtrA(name, 256) ? name : "";
}
// Forces act on the running solver's velocities at the call (the original changes its particles' velocities there
// too). The original's dt for force and acceleration modes is the scene's step [A: scene field +0x74 = step dt].
bool forceTarget(Hosted* h, const char* call, uint32_t mode, float radius) {
  if (h->flags & kClfStatic) return false;
  if (!h->sim || h->needRebuild) {  // pending rebuild: no solver state to push yet (not seen in the game)
    unsupported("NxCloth force call on a cloth waiting for a rebuild (dropped)");
    return false;
  }
  return true;
}
// First few force calls with their magnitude (how hard the game's bullets hit, for tuning the tests).
void logForce(const char* call, uint32_t mode, float mag, float radius) {
  static uint32_t logged = 0;
  if (logged < 8) {
    ++logged;
    logLine("cloth ours: force call: %s mode=%u magnitude=%g radius=%g", call, mode, mag, radius);
  }
}
void __fastcall o_addForceAtVertex(void* self, void* edx, const void* f, uint32_t vid, uint32_t mode) {
  SELF;
  if (!forceTarget(h, "addForceAtVertex", mode, 0.f)) return;
  logForce("addForceAtVertex", mode, sqrtf(v3Ptr(f).len2()), 0.f);
  h->sim->addForceAtVertex(vid, v3Ptr(f), mode, sceneState(h->scene).stepDt);
}
void __fastcall o_addForceAtPos(void* self, void* edx, const void* p, float mag, float radius, uint32_t mode) {
  SELF;
  if (!forceTarget(h, "addForceAtPos", mode, radius)) return;
  logForce("addForceAtPos", mode, mag, radius);
  h->sim->addForceAtPos(v3Ptr(p), nullptr, mag, radius, mode, sceneState(h->scene).stepDt);
}
void __fastcall o_addDirectedForceAtPos(void* self, void* edx, const void* p, const void* f, float radius,
                                        uint32_t mode) {
  SELF;
  if (!forceTarget(h, "addDirectedForceAtPos", mode, radius)) return;
  const V3 d = v3Ptr(f);
  logForce("addDirectedForceAtPos", mode, sqrtf(d.len2()), radius);
  h->sim->addForceAtPos(v3Ptr(p), &d, sqrtf(d.len2()), radius, mode, sceneState(h->scene).stepDt);
}
bool __fastcall o_overlapAABBTriangles(void* self, void* edx, const void* bounds, uint32_t* nb,
                                       const uint32_t** indices) {
  (void)self; (void)edx; (void)bounds; (void)indices;
  if (nb) *nb = 0;
  unsupported("NxCloth::overlapAABBTriangles");
  return false;
}
uint32_t __fastcall o_getScene(void* self, void* edx) {
  SELF_NJ;
  return h->scene;
}
const char* __fastcall o_getName(void* self, void* edx) {
  SELF_NJ;
  return h->name.c_str();
}
uint32_t __fastcall o_getCompartment(void* self, void* edx) {
  (void)self; (void)edx;
  return 0;
}
uint32_t __fastcall o_getPPUTime(void* self, void* edx) {
  (void)self; (void)edx;
  return 0;
}
uint32_t __fastcall o_getForceFieldMaterial(void* self, void* edx) {
  (void)self; (void)edx;
  return 0;
}
void __fastcall o_setForceFieldMaterial(void* self, void* edx, uint32_t m) {
  (void)self; (void)edx; (void)m;
  unsupported("NxCloth::setForceFieldMaterial");
}

void ensureVtable() {
  if (g_vtReady) return;
  g_vtReady = true;
  g_vt[0] = 0;
  const void* const fns[kSlots] = {
      reinterpret_cast<const void*>(&o_dtor), reinterpret_cast<const void*>(&o_saveToDesc),
      reinterpret_cast<const void*>(&o_getClothMesh), reinterpret_cast<const void*>(&o_setBendingStiffness),
      reinterpret_cast<const void*>(&o_getBendingStiffness), reinterpret_cast<const void*>(&o_setStretchingStiffness),
      reinterpret_cast<const void*>(&o_getStretchingStiffness), reinterpret_cast<const void*>(&o_setDampingCoefficient),
      reinterpret_cast<const void*>(&o_getDampingCoefficient), reinterpret_cast<const void*>(&o_setFriction),
      reinterpret_cast<const void*>(&o_getFriction), reinterpret_cast<const void*>(&o_setPressure),
      reinterpret_cast<const void*>(&o_getPressure), reinterpret_cast<const void*>(&o_setTearFactor),
      reinterpret_cast<const void*>(&o_getTearFactor), reinterpret_cast<const void*>(&o_setAttachmentTearFactor),
      reinterpret_cast<const void*>(&o_getAttachmentTearFactor), reinterpret_cast<const void*>(&o_setThickness),
      reinterpret_cast<const void*>(&o_getThickness), reinterpret_cast<const void*>(&o_getDensity),
      reinterpret_cast<const void*>(&o_getRelativeGridSpacing), reinterpret_cast<const void*>(&o_getSolverIterations),
      reinterpret_cast<const void*>(&o_setSolverIterations), reinterpret_cast<const void*>(&o_getWorldBounds),
      reinterpret_cast<const void*>(&o_attachToShape), reinterpret_cast<const void*>(&o_attachToCollidingShapes),
      reinterpret_cast<const void*>(&o_detachFromShape), reinterpret_cast<const void*>(&o_attachVertexToShape),
      reinterpret_cast<const void*>(&o_attachVertexToGlobalPosition), reinterpret_cast<const void*>(&o_freeVertex),
      reinterpret_cast<const void*>(&o_dominateVertex), reinterpret_cast<const void*>(&o_getVertexAttachmentStatus),
      reinterpret_cast<const void*>(&o_getVertexAttachmentShape), reinterpret_cast<const void*>(&o_getVertexAttachmentPosition),
      reinterpret_cast<const void*>(&o_attachToCore), reinterpret_cast<const void*>(&o_tearVertex),
      reinterpret_cast<const void*>(&o_raycast), reinterpret_cast<const void*>(&o_setGroup),
      reinterpret_cast<const void*>(&o_getGroup), reinterpret_cast<const void*>(&o_setGroupsMask),
      reinterpret_cast<const void*>(&o_getGroupsMask), reinterpret_cast<const void*>(&o_setMeshData),
      reinterpret_cast<const void*>(&o_getMeshData), reinterpret_cast<const void*>(&o_setValidBounds),
      reinterpret_cast<const void*>(&o_getValidBounds), reinterpret_cast<const void*>(&o_setPosition),
      reinterpret_cast<const void*>(&o_setPositions), reinterpret_cast<const void*>(&o_getPosition),
      reinterpret_cast<const void*>(&o_getPositions), reinterpret_cast<const void*>(&o_setVelocity),
      reinterpret_cast<const void*>(&o_setVelocities), reinterpret_cast<const void*>(&o_getVelocity),
      reinterpret_cast<const void*>(&o_getVelocities), reinterpret_cast<const void*>(&o_getNumberOfParticles),
      reinterpret_cast<const void*>(&o_queryShapePointers), reinterpret_cast<const void*>(&o_getStateByteSize),
      reinterpret_cast<const void*>(&o_getShapePointers), reinterpret_cast<const void*>(&o_setShapePointers),
      reinterpret_cast<const void*>(&o_saveStateToStream), reinterpret_cast<const void*>(&o_loadStateFromStream),
      reinterpret_cast<const void*>(&o_setCollisionResponseCoefficient),
      reinterpret_cast<const void*>(&o_getCollisionResponseCoefficient),
      reinterpret_cast<const void*>(&o_setAttachmentResponseCoefficient),
      reinterpret_cast<const void*>(&o_getAttachmentResponseCoefficient),
      reinterpret_cast<const void*>(&o_setFromFluidResponseCoefficient),
      reinterpret_cast<const void*>(&o_getFromFluidResponseCoefficient),
      reinterpret_cast<const void*>(&o_setToFluidResponseCoefficient),
      reinterpret_cast<const void*>(&o_getToFluidResponseCoefficient), reinterpret_cast<const void*>(&o_setExternalAcceleration),
      reinterpret_cast<const void*>(&o_getExternalAcceleration), reinterpret_cast<const void*>(&o_setMinAdhereVelocity),
      reinterpret_cast<const void*>(&o_getMinAdhereVelocity), reinterpret_cast<const void*>(&o_setWindAcceleration),
      reinterpret_cast<const void*>(&o_getWindAcceleration), reinterpret_cast<const void*>(&o_isSleeping),
      reinterpret_cast<const void*>(&o_getSleepLinearVelocity), reinterpret_cast<const void*>(&o_setSleepLinearVelocity),
      reinterpret_cast<const void*>(&o_wakeUp), reinterpret_cast<const void*>(&o_putToSleep),
      reinterpret_cast<const void*>(&o_setFlags), reinterpret_cast<const void*>(&o_getFlags),
      reinterpret_cast<const void*>(&o_setName), reinterpret_cast<const void*>(&o_addForceAtVertex),
      reinterpret_cast<const void*>(&o_addForceAtPos), reinterpret_cast<const void*>(&o_addDirectedForceAtPos),
      reinterpret_cast<const void*>(&o_overlapAABBTriangles), reinterpret_cast<const void*>(&o_getScene),
      reinterpret_cast<const void*>(&o_getName), reinterpret_cast<const void*>(&o_getCompartment),
      reinterpret_cast<const void*>(&o_getPPUTime), reinterpret_cast<const void*>(&o_getForceFieldMaterial),
      reinterpret_cast<const void*>(&o_setForceFieldMaterial)};
  for (uint32_t i = 0; i < kSlots; ++i) g_vt[1 + i] = ptr32(fns[i]);
}

// ---- creation -----------------------------------------------------------------------------------

// NxClothMesh -> points, triangles: saveToDesc (vtable slot 1, NxClothMesh.h) into an NxClothMeshDesc, then the
// cooked stream if the createClothMesh tap recorded one for this mesh.
bool readMesh(Hosted* h) {
  if (IsBadReadPtr(fromPtr32<const void>(h->mesh), 4)) return false;
  const uint32_t vt = *fromPtr32<const uint32_t>(h->mesh);
  if (!vt || IsBadReadPtr(fromPtr32<const void>(vt), 8)) return false;
  uint32_t md[12] = {0};  // NxClothMeshDesc (48 bytes): numVertices, numTriangles, pointStride, triStride, points,
                          // triangles, flags, ...
  auto save = reinterpret_cast<bool(__fastcall*)(void*, void*, void*)>(fromPtr32<const uint32_t>(vt)[1]);
  if (!save(fromPtr32<void>(h->mesh), nullptr, md)) return false;
  const uint32_t nV = md[0], nT = md[1], pStride = md[2] ? md[2] : 12, tStride = md[3] ? md[3] : 12;
  const uint8_t* pts = fromPtr32<const uint8_t>(md[4]);
  const uint8_t* tris = fromPtr32<const uint8_t>(md[5]);
  if (!nV || !nT || nV > (1u << 20) || nT > (1u << 20) || !pts || !tris) return false;
  if (IsBadReadPtr(pts, (nV - 1) * pStride + 12) || IsBadReadPtr(tris, (nT - 1) * tStride + 12)) return false;
  h->meshTearable = md[6] & kClothMeshTearable;
  h->meshAttached.clear();
  h->meshAttachedTearable = 0;
  const uint8_t* vf = fromPtr32<const uint8_t>(md[kCmdVertexFlags / 4]);
  const uint32_t vfStride = md[kCmdVertexFlagStride / 4] ? md[kCmdVertexFlagStride / 4] : 4;
  if (vf && !IsBadReadPtr(vf, (nV - 1) * vfStride + 4)) {
    for (uint32_t i = 0; i < nV; ++i) {
      uint32_t fl;
      memcpy(&fl, vf + i * vfStride, 4);
      if (fl & kVertexAttachedGlobalFlag) h->meshAttached.push_back(i);
      if (fl & kVertexTearableFlag) ++h->meshAttachedTearable;
    }
  }
  std::vector<V3> dp(nV);
  for (uint32_t i = 0; i < nV; ++i) dp[i] = v3Ptr(pts + i * pStride);
  std::vector<uint32_t> dt(3 * (size_t)nT);
  for (uint32_t t = 0; t < nT; ++t)
    for (int c = 0; c < 3; ++c) memcpy(&dt[3 * t + c], tris + t * tStride + 4 * c, 4);
  for (uint32_t i : dt)
    if (i >= nV) return false;
  auto it = g_cooked.find(h->mesh);
  if (it != g_cooked.end() && it->second.pos.size() == nV && it->second.tri == dt) {
    bool same = true;
    for (uint32_t i = 0; same && i < nV; ++i)
      same = fabsf(it->second.pos[i].x - dp[i].x) < 1e-6f && fabsf(it->second.pos[i].y - dp[i].y) < 1e-6f &&
             fabsf(it->second.pos[i].z - dp[i].z) < 1e-6f;
    if (same) {
      h->rest = it->second.pos;
      h->tri = it->second.tri;
      h->edges = it->second.edges;
      h->cookedEdges = true;
      return true;
    }
    logLine("cloth ours: cooked stream of %08X does not match saveToDesc, using saveToDesc (first-appearance edges)",
            h->mesh);
  } else {
    logLine("cloth ours: no cooked stream for mesh %08X (createClothMesh tap off?), first-appearance edges [A]", h->mesh);
  }
  h->rest = dp;
  h->tri = dt;
  return true;
}

// NxClothDesc -> Hosted (parameters, pose, receive buffers, flags). Logs the non-default fields the solver ignores.
void readDesc(Hosted* h, const uint8_t* d) {
  ClothParams& P = h->P;
  P.thickness = fAt(d, kOffThickness);
  P.density = fAt(d, kOffDensity);
  P.bending = fAt(d, kOffBending);
  P.stretching = fAt(d, kOffStretching);
  P.damping = fAt(d, kOffDamping);
  P.friction = fAt(d, kOffFriction);
  P.tearFactor = fAt(d, kOffTear);
  P.solverIterations = uAt(d, kOffIterations) ? uAt(d, kOffIterations) : 1;
  P.refDt = clothRefDt();
  const float sleepV = fAt(d, kOffSleepVel), wake = fAt(d, kOffWakeUp);
  P.sleepLinearVelocity = sleepV >= 0 ? sleepV : pxc::Cloth::kDefaultSleepVelocity;  // -1: SDK default
  P.wakeUpCounter = wake >= 0 ? wake : 0.4f;
  P.externalAcceleration = v3At(d, kOffExtAccel);
  P.windAcceleration = v3At(d, kOffWind);
  h->X.pressure = fAt(d, kOffPressure);
  h->X.attachTearFactor = fAt(d, kOffAttachTear);
  h->X.attachResp = fAt(d, kOffAttachResp);
  h->X.collResp = fAt(d, kOffCollResp);
  h->X.fromFluid = fAt(d, kOffFromFluid);
  h->X.toFluid = fAt(d, kOffToFluid);
  h->X.minAdhere = fAt(d, kOffMinAdhere);
  h->X.gridSpacing = fAt(d, kOffGridSpacing);
  h->flags = uAt(d, kOffFlags);
  coverFlags(0, h->flags);
  applyFlags(h);
  h->group = uAt(d, kOffCollGroup) & 0xffff;  // NxCollisionGroup is 16 bits (the next two bytes are padding)
  for (int k = 0; k < 4; ++k) h->groupsMask[k] = uAt(d, kOffGroupsMask + 4 * k);
  h->userData = uAt(d, kOffUserData);
  const char* nm = fromPtr32<const char>(uAt(d, kOffName));
  h->name = nm && !IsBadStringPtrA(nm, 256) ? nm : "";
  memcpy(h->md, d + kOffMeshData, kMdBytes);
  h->hasMd = true;
  P.maxVertices = tearCapOf(h->md);
  for (int k = 0; k < 6; ++k) h->validBounds[k] = fAt(d, kOffValidBounds + 4 * k);
  for (int k = 0; k < 3; ++k) P.validLo[k] = h->validBounds[k], P.validHi[k] = h->validBounds[3 + k];

  // pose (NxMat34: row-major 3x3 at +4 (Nx9F32.h, TRANSPOSED_MAT33 unset), translation at +40); applied to the rest
  // positions [V: read column-major, the three banners with a non-symmetric pose were created mirrored and shredded on
  // the game's first re-pin]
  float M[9];
  memcpy(M, d + kOffPose, 36);
  const V3 t = v3At(d, kOffPoseT);
  h->x.resize(h->rest.size());
  for (size_t i = 0; i < h->rest.size(); ++i) {
    const V3& r = h->rest[i];
    h->x[i] = V3(M[0] * r.x + M[1] * r.y + M[2] * r.z + t.x, M[3] * r.x + M[4] * r.y + M[5] * r.z + t.y,
                 M[6] * r.x + M[7] * r.y + M[8] * r.z + t.z);
  }
  h->v.assign(h->rest.size(), V3());

  // what the solver ignores, logged per cloth
  std::string ign;
  auto note = [&ign](const char* s) { ign += ign.empty() ? s : std::string(", ") + s; };
  if (h->flags & kClfPressure) note("pressure");
  if (h->flags & kClfSelfCollision) note("selfCollision");
  if (h->flags & kClfTriangleCollision) note("triangleCollision");
  if (h->flags & kClfCollisionTwoway) note("collisionTwoWay");
  if (h->flags & kClfValidBounds) note("validBounds");
  if (h->flags & kClfFluidCollision) note("fluidCollision");
  if (h->flags & kClfAdhere) note("adhere");
  if (h->flags & kClfDisableCollision) note("(collision disabled)");
  if (h->X.minAdhere > 0) note("minAdhereVelocity");
  if (h->X.toFluid > 0 || h->X.fromFluid > 0) note("fluid responses");
  if (!ign.empty()) logLine("cloth ours: desc fields not supported: %s", ign.c_str());
}

// Recording of a cloth's solver calls -> PhysXLoader_cloth<id>.pxcr next to the log (tools/run_game_logged.sh collects
// them; solver/cloth/cloth_replay.cpp replays them). Appended at release, every 600 simulates, and at detach.
bool g_recOn = false;  // PXSHIM_CLOTH_REC=1: on
uint32_t g_recNext = 0, g_simCount = 0;
void writeRec(Hosted* h) {
  if (!g_recOn || !h->rec) return;
  char path[64];
  snprintf(path, sizeof path, "PhysXLoader_cloth%03u.pxcr", h->recId);
  if (!h->rec->write(path)) logLine("cloth ours: could not write %s", path);
}

void unregister(Hosted* h) {
  writeRec(h);
  for (size_t i = 0; i < g_cloths.size(); ++i)
    if (g_cloths[i] == h) {
      g_cloths.erase(g_cloths.begin() + i);
      break;
    }
  delete h->obj;
  delete h;
}

}  // namespace

// ---- entry points --------------------------------------------------------------------------------

bool oursEnabled() { return g_on; }

void oursInit() {
  const char* mode = env("PXSHIM_CLOTH");  // default on: unset or "ours"; "off" or "0" leaves cloths to the core
  g_on = strcmp(mode, "off") != 0 && strcmp(mode, "0") != 0;
  g_asyncOn = env("PXSHIM_CLOTH_ASYNC")[0] != '0';
  g_trace = debug() && env("PXSHIM_CLOTH_TRACE")[0] == '1';
  g_edgeShape = env("PXSHIM_CLOTH_EDGESHAPE")[0] != '0';
  g_meshOn = env("PXSHIM_CLOTH_MESH")[0] != '0';
  g_recOn = env("PXSHIM_CLOTH_REC")[0] == '1';  // off unless asked for
  if (g_on && !g_meshOn) logLine("cloth ours: scene meshes off for cloth (PXSHIM_CLOTH_MESH=0)");
  if (g_on) logLine("cloth ours: NxScene::createCloth is hosted on solver/cloth (%s)",
                    g_asyncOn ? "steps on the cloth thread (PXSHIM_CLOTH_ASYNC)" : "synchronous (PXSHIM_CLOTH_ASYNC=0)");
}

void oursRegisterCookedMesh(uint32_t mesh, const uint8_t* bytes, size_t n) {
  CookedCloth cc;
  std::vector<uint8_t> v(bytes, bytes + n);
  if (!pxc::parseCookedCloth(v, cc)) {
    logLine("cloth ours: cooked stream for mesh %08X not understood (%zu bytes)", mesh, n);
    return;
  }
  logLine("cloth ours: cooked stream for mesh %08X: %zu vertices, %zu triangles, %zu edges", mesh, cc.pos.size(),
          cc.tri.size() / 3, cc.edges.size());
  g_cooked[mesh] = cc;
}

// Particle-triangle self contact (not in the original; solver/cloth/cloth_selftri.cpp), on by default: torn pieces
// lying on each other and wind folds no longer pass through each other. Distance = PXSHIM_CLOTH_SELF (default 0.2) times
// the mesh's median rest edge, at least the thickness (the game's 0.01 is far below its vertex spacing).
// PXSHIM_CLOTH_SELF=0 turns it off.
void setSelfContact(Hosted* h) {
  const char* e = env("PXSHIM_CLOTH_SELF");
  const float frac = e[0] ? (float)atof(e) : 0.2f;
  h->P.selfContactDist = 0.f;
  if (!(frac > 0.f) || h->tri.size() < 3) return;
  std::vector<float> L;
  L.reserve(h->tri.size());
  for (size_t t = 0; t + 2 < h->tri.size(); t += 3)
    for (int k = 0; k < 3; ++k) {
      const uint32_t a = h->tri[t + k], b = h->tri[t + (k + 1) % 3];
      if (a < h->rest.size() && b < h->rest.size()) L.push_back(sqrtf((h->rest[a] - h->rest[b]).len2()));
    }
  if (L.empty()) return;
  std::nth_element(L.begin(), L.begin() + L.size() / 2, L.end());
  h->P.selfContactDist = fmaxf(frac * L[L.size() / 2], h->P.thickness);
  static bool logged = false;
  if (!logged) {
    logged = true;
    logLine("cloth ours: self contact on (distance %.3g x median edge, first cloth: edge %.4g -> %.4g; "
            "PXSHIM_CLOTH_SELF=0 turns it off)", frac, L[L.size() / 2], h->P.selfContactDist);
  }
}

uint32_t oursCreate(void* scene, const void* desc) {
  if (!g_on) return 0;
  ensureVtable();
  if (IsBadReadPtr(desc, kDescBytes)) {
    ++g_refused;
    return 0;
  }
  const uint8_t* d = static_cast<const uint8_t*>(desc);
  Hosted* h = new Hosted();
  h->scene = ptr32(scene);
  h->mesh = uAt(d, kOffClothMesh);
  if (!h->mesh || !readMesh(h)) {
    logLine("cloth ours: createCloth refused (mesh %08X unreadable), the core creates it", h->mesh);
    delete h;
    ++g_refused;
    return 0;
  }
  readDesc(h, d);
  setSelfContact(h);
  for (uint32_t v : h->meshAttached)
    if (v < h->x.size()) h->pins[v] = h->x[v];
  if (!h->meshAttached.empty() || h->meshAttachedTearable)
    logLine("cloth ours: mesh vertex flags: %zu attached (pinned at their start), %u tearable [A: flags come from saveToDesc]",
            h->meshAttached.size(), h->meshAttachedTearable);
  h->obj = new Obj{g_vt + 1, h};
  h->recId = g_recNext++;
  if (g_recOn) {
    h->rec = std::make_shared<pxc::Recording>();
    char note[160];
    snprintf(note, sizeof note, "cloth %08X scene %08X created at simulate %u", ptr32(h->obj), h->scene, g_simCount);
    h->rec->note(note);
  }
  g_cloths.push_back(h);
  ++g_created;
  logLine("cloth ours: created %08X in scene %08X: %zu vertices, %zu triangles, edges %s, flags %08X, group %u",
          ptr32(h->obj), h->scene, h->rest.size(), h->tri.size() / 3, h->cookedEdges ? "cooked" : "first-appearance",
          h->flags, h->group);
  return ptr32(h->obj);
}

bool oursOwns(uint32_t cloth) {
  for (Hosted* h : g_cloths)
    if (ptr32(h->obj) == cloth) return true;
  return false;
}

void oursRelease(uint32_t cloth) {
  for (Hosted* h : g_cloths)
    if (ptr32(h->obj) == cloth) {
      joinFor(h, "releaseCloth");
      ++g_released;
      unregister(h);
      return;
    }
}

void oursOnSimulate(void* scene, float elapsed) {
  traceFlush();
  ++g_simCount;
  if (!g_on || g_cloths.empty()) return;
  joinJob(nullptr);  // a job whose fetch did not come (or another scene's): done before this simulate reads anything
  if (g_simCount % 600 == 0)  // recordings appended to their files every 600 simulates (the cloth thread is idle here)
    for (Hosted* h : g_cloths) writeRec(h);
  flushReactions();
  const uint32_t sc = ptr32(scene);
  std::vector<Hosted*> hs;
  for (Hosted* h : g_cloths)
    if (h->scene == sc && !posOf(h).empty()) hs.push_back(h);
  for (Hosted* h : hs) applyDeferredParams(h);
  if (hs.empty()) return;
  SceneSettings ss;
  readSceneSettings(sc, ss);
  SceneState& st = sceneState(sc);
  // Scene timing, as the fluid hosting: fixed = whole maxTimestep substeps (at most maxIter), variable = one step (the
  // game's case: one cloth step per frame, at whatever rate it runs; ClothParams::refDt keeps the look of the original at 62 FPS).
  int steps = 1;
  float dt = elapsed;
  if (ss.timestepMethod == 0 && ss.maxTimestep > 0) {
    st.accum += elapsed;
    steps = (int)(st.accum / ss.maxTimestep);
    if (steps > (int)ss.maxIter) steps = (int)ss.maxIter;
    st.accum -= steps * ss.maxTimestep;
    if (st.accum > ss.maxTimestep) st.accum = 0;  // dropped time beyond maxIter substeps [A]
    dt = ss.maxTimestep;
  }
  st.stepDt = dt;
  if (steps <= 0) return;
  // The scene geometry, refreshed once per frame at the poses the core is about to simulate from. Each cloth's box
  // is its particles grown by its thickness and by what they can travel in the frame (dynamic shapes are culled with it).
  const float frameDt = steps * dt;
  std::vector<Box3> boxes;
  boxes.reserve(hs.size());
  for (const Hosted* h : hs) {
    const std::vector<V3>& x = posOf(h);
    const std::vector<V3>& v = velOf(h);
    V3 lo(1e30f, 1e30f, 1e30f), hi(-1e30f, -1e30f, -1e30f);
    float vmax2 = 0;
    for (size_t i = 0; i < x.size(); ++i) {
      lo = V3(fminf(lo.x, x[i].x), fminf(lo.y, x[i].y), fminf(lo.z, x[i].z));
      hi = V3(fmaxf(hi.x, x[i].x), fmaxf(hi.y, x[i].y), fmaxf(hi.z, x[i].z));
      if (i < v.size()) vmax2 = fmaxf(vmax2, v[i].len2());
    }
    const float grow = h->P.thickness + 1.0f + sqrtf(vmax2) * frameDt;
    boxes.push_back(Box3{pxr::Vec3(lo.x - grow, lo.y - grow, lo.z - grow), pxr::Vec3(hi.x + grow, hi.y + grow, hi.z + grow)});
  }
  SceneGeometry& geom = *st.geom;
  geom.refresh(sc, boxes, frameDt, false);
  for (Hosted* h : hs) ++h->frame;
  const V3 g(ss.gravity.x, ss.gravity.y, ss.gravity.z);
  // The core is read here, on the game thread: each cloth's build (once) and the shapes near it with their body data.
  g_job.runs.clear();
  for (size_t i = 0; i < hs.size(); ++i) {
    Hosted* h = hs[i];
    if (needsStaticRequery(h)) requeryStatics(h, geom);
    if (!h->sim || h->needRebuild) build(h, geom);
    if (!h->sim) continue;
    ClothRun r;
    r.h = h;
    r.dyn = nearDynamic(h, geom, boxes[i]);
    pointBounds(posOf(h), h->boundsAtStep, removedOf(h));
    g_job.runs.push_back(std::move(r));
  }
  g_job.scene = sc;
  g_job.geom = &geom;
  g_job.steps = steps;
  g_job.dt = dt;
  g_job.frameDt = frameDt;
  g_job.g = g;
  ++g_as.simulates;
  if (!g_asyncOn) {  // synchronous: the reactions are applied before the core's step, as before the job existed
    runJob(g_job);
    finishJob();
    flushReactions();
    return;
  }
  static std::thread* thread = new std::thread(clothThread);  // never joined (the fluid thread's note applies)
  (void)thread;
  shimPoolRun(0, nullptr, nullptr);  // creates the pool here, on the game thread, before the cloth thread uses it
  {
    std::lock_guard<std::mutex> l(g_jobM);
    g_jobQueued = true;
  }
  g_jobBusy = true;
  for (ClothRun& r : g_job.runs) {
    if (r.h->raycasts) r.h->rayX = posOf(r.h), r.h->rayTri = triOf(r.h);
    r.h->inJob = true;
  }
  ++g_as.asyncJobs;
  g_jobCv.notify_all();
}

void oursOnFetched(void* scene, bool ok) {
  if (!g_on || !ok) return;  // not finished (non-blocking fetch): the job stays in flight for the next fetch
  const uint32_t sc = ptr32(scene);
  if (g_jobBusy && g_job.scene == sc) {  // the expected join: the cloths' steps are done before the outputs are written
    const double waited = joinJob(nullptr);
    ++g_as.fetchWaits;
    g_as.fetchWaitMs += waited;
    g_as.fetchWaitMax = fmaxf(g_as.fetchWaitMax, (float)waited);
  }
  if (g_job.scene == sc) flushReactions();  // after the core's step (the reactions land there)
  for (Hosted* h : g_cloths)
    if (h->scene == sc) writeResults(h);
}

void oursOnSceneRelease(void* scene) {
  if (!g_on) return;
  const uint32_t sc = ptr32(scene);
  if (g_jobBusy && g_job.scene == sc) joinJob("releaseScene");
  if (g_job.scene == sc) {  // the scene's geometry goes below: its pending reactions go with it
    g_job.impulses.clear();
    g_job.geom = nullptr;
  }
  for (size_t i = 0; i < g_cloths.size();) {
    if (g_cloths[i]->scene == sc) {
      ++g_released;
      unregister(g_cloths[i]);
    } else {
      ++i;
    }
  }
  auto it = g_scenes.find(sc);
  if (it != g_scenes.end()) {
    delete it->second.geom;
    g_scenes.erase(it);
  }
}

// The cloth part of the frame-cost log: what the cloths cost on the game thread and the cloth thread since the last call.
void oursStatsLine(char* buf, size_t size) {
  buf[0] = 0;
  if (!g_on) return;
  const AsyncStats s = g_as;
  const double n = s.simulates ? s.simulates : 1;
  std::string by;
  for (const auto& e : g_forcedBy) by += (by.empty() ? "" : ", ") + e.first + " " + std::to_string(e.second);
  snprintf(buf, size, "cloth stepping %.3f ms per simulate (%u of %u simulates on the cloth thread; solver %.3f ms "
           "summed over cloths), %u reactions; game thread waited %.3f ms per simulate at fetch (max %.3f), "
           "%.3f ms in %u waits forced by cloth calls (%s)",
           s.jobMs / n, s.asyncJobs, s.simulates, s.solverMs / n, s.reactions, s.fetchWaitMs / n, s.fetchWaitMax,
           s.forcedWaitMs, s.forcedWaits, by.empty() ? "none" : by.c_str());
  g_as = AsyncStats();
  g_forcedBy.clear();
}

void oursLogSummary(const char* why) {
  if (!g_on) return;
  joinJob(nullptr);
  uint32_t steps = 0, verts = 0;
  for (const Hosted* h : g_cloths) {
    steps += h->steps;
    verts += (uint32_t)posOf(h).size();
  }
  logLine("cloth ours summary (%s): created=%u released=%u refused=%u live=%zu particles=%u steps=%u rebuilds=%u "
          "solverMs=%.3f cookedMeshes=%zu reactions=%u",
          why, g_created, g_released, g_refused, g_cloths.size(), verts, steps, g_rebuildsTotal, g_simMsTotal,
          g_cooked.size(), g_impulsesTotal);
  if (!strcmp(why, "detach") || !strcmp(why, "releaseScene"))
    for (Hosted* h : g_cloths) writeRec(h);
  if (!g_unsupportedSeen.empty()) {
    std::string s;
    for (const std::string& u : g_unsupportedSeen) s += (s.empty() ? "" : "; ") + u;
    logLine("  unsupported seen: %s", s.c_str());
  }
}

}  // namespace shim
