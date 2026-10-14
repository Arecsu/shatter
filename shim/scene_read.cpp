// Reads collision geometry back from a live NxScene for the hosted fluid solver.
//
// Everything goes through the core's vtables (slots from the 2.8.1 headers' declaration order; the 2.8.0
// core has the same slot counts and order for NxScene 0..152, NxActor, NxShape and its subclasses and
// NxTriangleMesh/NxConvexMesh: tools/rtti_vtables.py plus assert strings [V for the
// string-anchored slots, A for the rest, checked end to end with the oracle through the shim]).
// Methods returning a class (NxMat34, NxPlane, NxVec3, NxGroupsMask) use MSVC's hidden return pointer:
// it is the first stack argument and comes back in EAX.

#include <windows.h>

#include <float.h>
#include <math.h>
#include <string.h>

#include <iterator>
#include <map>
#include <memory>
#include <unordered_map>
#include <vector>

#include "../solver/fluid/fluid.h"
#include "coverage.h"
#include "scene_read.h"
#include "shim.h"

namespace shim {
namespace {

// NxScene
const uint32_t kSceneGetGravity = 6, kSceneGetGroupCollisionFlag = 48, kSceneGetFilterOps = 59,
               kSceneGetFilterBool = 60, kSceneGetFilterConstant0 = 61, kSceneGetFilterConstant1 = 62,
               kSceneGetNbActors = 63, kSceneGetActors = 64, kSceneGetTiming = 83;
// NxActor
const uint32_t kActorGetNbShapes = 19, kActorGetShapes = 20, kActorIsDynamic = 29;
// NxShape (39 slots) and subclasses (their own methods follow at 39+)
const uint32_t kShapeGetGroup = 3, kShapeGetFlag = 6, kShapeGetGlobalPose = 16, kShapeGetType = 23,
               kShapeGetGroupsMask = 36;
const uint32_t kPlaneGetPlane = 41, kSphereGetRadius = 40, kBoxGetDimensions = 40, kCapsuleGetRadius = 41,
               kCapsuleGetHeight = 43, kMeshShapeGetMesh = 40, kConvexShapeGetMesh = 40;
// NxTriangleMesh / NxConvexMesh
const uint32_t kMeshGetCount = 2, kMeshGetFormat = 3, kMeshGetBase = 4, kMeshGetStride = 5;
// NxHeightField (15 slots in 2.8.0 and 2.8.1; order from the 2.8.1 header) and NxHeightFieldShape (55 slots in both;
// its own methods follow NxShape's 39, getTriangle is the 9th of them). The 2.8.0 order is [A].
const uint32_t kHfGetNbRows = 3, kHfGetNbColumns = 4, kHfGetFormat = 5;
const uint32_t kHfShapeGetField = 40, kHfShapeGetHeightScale = 41, kHfShapeGetRowScale = 42,
               kHfShapeGetColumnScale = 43, kHfShapeGetTriangle = 47;
const uint32_t kHfFormatS16Tm = 1;  // NX_HF_S16_TM, the only format the 2.8 SDK creates

enum ShapeType { kPlane = 0, kSphere, kBox, kCapsule, kWheel, kConvex, kMesh, kHeightField };
// NxShapeFlag bits (2.8.1 Nxp.h) that fluid collision reads; the values are the solver's kShape* constants.
const uint32_t kSfDisableCollision = pxr::kShapeDisableCollision, kSfFluidDisableCollision = pxr::kShapeFluidDisableCollision,
               kSfFluidDrain = pxr::kShapeFluidDrain, kSfDisableResponse = pxr::kShapeDisableResponse;
const uint32_t kSfTriggerBits[3] = {1u << 0, 1u << 1, 1u << 2};  // NX_TRIGGER_ON_ENTER, ON_LEAVE, ON_STAY
// NxActorFlag bits and NxCompartment (slots [A]: NxActor::getCompartment 100, NxCompartment::getType 0, from the 2.8.1
// header order minus the 2-slot offset that the verified NxActor slots show)
const uint32_t kActorGetCompartment = 100, kCompartmentGetType = 0, kCompartmentRigidBody = 0;  // NX_SCT_RIGIDBODY
// Periodic re-read of the static shapes' flags and groups (every this many refreshes; a changed one takes effect then).
const uint32_t kStaticFlagRecheck = 30;
enum { kArrayTriangles = 0, kArrayVertices = 1 };
enum { kFormatNoData = 0, kFormatFloat, kFormatByte, kFormatShort, kFormatInt };

uint32_t slotOf(uint32_t obj, uint32_t slot) { return fromPtr32<const uint32_t>(*fromPtr32<const uint32_t>(obj))[slot]; }

uint32_t callU32(uint32_t obj, uint32_t slot) {
  return ((uint32_t(__fastcall*)(uint32_t, void*))slotOf(obj, slot))(obj, nullptr);
}
uint32_t callU32(uint32_t obj, uint32_t slot, uint32_t a) {
  return ((uint32_t(__fastcall*)(uint32_t, void*, uint32_t))slotOf(obj, slot))(obj, nullptr, a);
}
uint32_t callU32(uint32_t obj, uint32_t slot, uint32_t a, uint32_t b) {
  return ((uint32_t(__fastcall*)(uint32_t, void*, uint32_t, uint32_t))slotOf(obj, slot))(obj, nullptr, a, b);
}
bool callBool(uint32_t obj, uint32_t slot) { return ((bool(__fastcall*)(uint32_t, void*))slotOf(obj, slot))(obj, nullptr); }
bool callBool(uint32_t obj, uint32_t slot, uint32_t a) {
  return ((bool(__fastcall*)(uint32_t, void*, uint32_t))slotOf(obj, slot))(obj, nullptr, a);
}
bool callBool(uint32_t obj, uint32_t slot, uint32_t a, uint32_t b) {
  return ((bool(__fastcall*)(uint32_t, void*, uint32_t, uint32_t))slotOf(obj, slot))(obj, nullptr, a, b);
}
float callF32(uint32_t obj, uint32_t slot) { return ((float(__fastcall*)(uint32_t, void*))slotOf(obj, slot))(obj, nullptr); }
// Class return through the hidden pointer.
void callRet(uint32_t obj, uint32_t slot, void* ret) {
  ((void*(__fastcall*)(uint32_t, void*, void*))slotOf(obj, slot))(obj, nullptr, ret);
}
// Out-parameter by reference (pointer on the stack).
void callOut(uint32_t obj, uint32_t slot, void* out) {
  ((void(__fastcall*)(uint32_t, void*, void*))slotOf(obj, slot))(obj, nullptr, out);
}

using pxr::Vec3;
using pxr::Pose;  // NxMat34 layout: row-major rotation, then the translation

Vec3 poseCol(const Pose& p, int c) { return Vec3(p.m[0][c], p.m[1][c], p.m[2][c]); }
bool axisAligned(const Pose& p) {
  for (int r = 0; r < 3; ++r)
    for (int c = 0; c < 3; ++c)
      if (fabsf(p.m[r][c] - (r == c ? 1.0f : 0.0f)) > 1e-6f) return false;
  return true;
}
Pose compose(const Pose& a, const Pose& b) {  // a * b
  Pose r;
  for (int i = 0; i < 3; ++i)
    for (int j = 0; j < 3; ++j) r.m[i][j] = a.m[i][0] * b.m[0][j] + a.m[i][1] * b.m[1][j] + a.m[i][2] * b.m[2][j];
  r.t = a.apply(b.t);
  return r;
}
// Rotation by the vector w (axis * angle), Rodrigues.
Pose rotationOf(const Vec3& w) {
  Pose r;
  const float a = sqrtf(w.len2());
  if (a < 1e-12f) return r;
  const Vec3 k = w * (1.0f / a);
  const float c = cosf(a), s = sinf(a), v = 1.0f - c;
  r.m[0][0] = c + k.x * k.x * v, r.m[0][1] = k.x * k.y * v - k.z * s, r.m[0][2] = k.x * k.z * v + k.y * s;
  r.m[1][0] = k.y * k.x * v + k.z * s, r.m[1][1] = c + k.y * k.y * v, r.m[1][2] = k.y * k.z * v - k.x * s;
  r.m[2][0] = k.z * k.x * v - k.y * s, r.m[2][1] = k.z * k.y * v + k.x * s, r.m[2][2] = c + k.z * k.z * v;
  return r;
}

struct GroupsMask {
  uint32_t bits[4];
};

// The scene's group-mask filter (NxScene::setFilterOps etc.): (G0 op0 K0) op2 (G1 op1 K1) == bool.
struct Filter {
  uint32_t op[3];
  bool value;
  GroupsMask k0, k1;
};
enum { kOpAnd, kOpOr, kOpXor, kOpNand, kOpNor, kOpNxor, kOpSwapAnd };

GroupsMask applyOp(uint32_t op, const GroupsMask& a, const GroupsMask& b) {
  GroupsMask r;
  for (int i = 0; i < 4; ++i) {
    const uint32_t x = a.bits[i], y = b.bits[i];
    switch (op) {
      case kOpAnd: r.bits[i] = x & y; break;
      case kOpOr: r.bits[i] = x | y; break;
      case kOpXor: r.bits[i] = x ^ y; break;
      case kOpNand: r.bits[i] = ~(x & y); break;
      case kOpNor: r.bits[i] = ~(x | y); break;
      case kOpNxor: r.bits[i] = ~(x ^ y); break;
      default: r.bits[i] = x & b.bits[(i + 2) & 3]; break;  // SWAP_AND: 64-bit halves swapped [A]
    }
  }
  return r;
}

bool filterPasses(const Filter& f, const GroupsMask& g0, const GroupsMask& g1) {
  const GroupsMask a = applyOp(f.op[0], g0, f.k0), b = applyOp(f.op[1], g1, f.k1), r = applyOp(f.op[2], a, b);
  const bool nonZero = (r.bits[0] | r.bits[1] | r.bits[2] | r.bits[3]) != 0;
  return nonZero == f.value;
}

void addTri(pxr::StaticShape& mesh, const Vec3& a, const Vec3& b, const Vec3& c) { mesh.tris.push_back({a, b, c}); }

// Triangles of an NxTriangleMesh / NxConvexMesh (submesh 0), transformed to world space.
bool appendMeshTris(uint32_t mesh, const Pose& pose, pxr::StaticShape& out) {
  const uint32_t nTri = callU32(mesh, kMeshGetCount, 0, kArrayTriangles);
  const uint32_t nVert = callU32(mesh, kMeshGetCount, 0, kArrayVertices);
  const uint32_t triFmt = callU32(mesh, kMeshGetFormat, 0, kArrayTriangles);
  const uint32_t vertFmt = callU32(mesh, kMeshGetFormat, 0, kArrayVertices);
  const uint32_t triBase = callU32(mesh, kMeshGetBase, 0, kArrayTriangles);
  const uint32_t vertBase = callU32(mesh, kMeshGetBase, 0, kArrayVertices);
  const uint32_t triStride = callU32(mesh, kMeshGetStride, 0, kArrayTriangles);
  const uint32_t vertStride = callU32(mesh, kMeshGetStride, 0, kArrayVertices);
  if (vertFmt != kFormatFloat || !triBase || !vertBase || (triFmt != kFormatInt && triFmt != kFormatShort)) {
    logLine("geometry: mesh %08X unsupported (tris %u fmt %u, verts %u fmt %u)", mesh, nTri, triFmt, nVert, vertFmt);
    return false;
  }
  std::vector<Vec3> v(nVert);
  for (uint32_t i = 0; i < nVert; ++i) {
    const float* p = fromPtr32<const float>(vertBase + i * vertStride);
    v[i] = pose.apply(Vec3(p[0], p[1], p[2]));
  }
  for (uint32_t i = 0; i < nTri; ++i) {
    uint32_t idx[3];
    for (int k = 0; k < 3; ++k)
      idx[k] = triFmt == kFormatInt ? fromPtr32<const uint32_t>(triBase + i * triStride)[k]
                                    : fromPtr32<const uint16_t>(triBase + i * triStride)[k];
    if (idx[0] >= nVert || idx[1] >= nVert || idx[2] >= nVert) continue;
    addTri(out, v[idx[0]], v[idx[1]], v[idx[2]]);
  }
  return true;
}

// Shape-frame triangles of a heightfield shape, cached per NxShape (the cache is valid while the field and the three
// scales are unchanged; holes are fixed per shape). The core supplies the triangles: getTriangle with translation and
// rotation off, so the row/column/height scales, the tessellation diagonals and the holes (ids the core answers 0
// for: no triangle or the hole material) are already applied. Ids run over 2 * (rows - 1) * (cols - 1): the core
// reports an SDK error for an id past that.
struct HfEntry {
  bool valid = false;
  uint32_t field = 0;
  float scale[3] = {0, 0, 0};  // heightScale, rowScale, columnScale
  std::vector<pxr::MeshTri> tris;
};
using HfCache = std::map<uint32_t, HfEntry>;

// NxHeightFieldShape::getTriangle(NxTriangle& world, NxTriangle* edge, NxU32* flags, NxTriangleID id, bool, bool):
// thiscall with six stack arguments, passed through the ecx/edx pair like the other calls here.
uint32_t callTriangle(uint32_t sh, uint32_t id, float* tri, uint32_t* flags) {
  return ((uint32_t(__fastcall*)(uint32_t, void*, float*, void*, uint32_t*, uint32_t, uint32_t, uint32_t))slotOf(
      sh, kHfShapeGetTriangle))(sh, nullptr, tri, nullptr, flags, id, 0, 0);
}

bool readHeightField(uint32_t sh, HfEntry& e) {
  const uint32_t hf = callU32(sh, kHfShapeGetField);
  const uint32_t rows = callU32(hf, kHfGetNbRows), cols = callU32(hf, kHfGetNbColumns);
  if (callU32(hf, kHfGetFormat) != kHfFormatS16Tm || rows < 2 || cols < 2) {
    logLine("geometry: heightfield %08X shape %08X unsupported (rows %u cols %u format %u)", hf, sh, rows, cols,
            callU32(hf, kHfGetFormat));
    return false;
  }
  e.field = hf;
  e.scale[0] = callF32(sh, kHfShapeGetHeightScale);
  e.scale[1] = callF32(sh, kHfShapeGetRowScale);
  e.scale[2] = callF32(sh, kHfShapeGetColumnScale);
  e.tris.clear();
  uint32_t up = 0, down = 0;
  // The core numbers the two triangles of the cell with upper-left sample (r, c) as 2 * (r * cols + c) + k; cells on
  // the last row or column have none (asking for them makes the core report an SDK error). Each triangle is checked
  // against the cell its id names (rows along x, columns along z).
  const uint32_t nIds = 2 * (rows - 1) * (cols - 1), nAll = 2 * rows * cols;
  uint32_t misplaced = 0;
  for (uint32_t id = 0; id < nAll; ++id) {
    const uint32_t cell = id >> 1, r = cell / cols, c = cell % cols;
    if (r >= rows - 1 || c >= cols - 1) continue;
    float t[9];
    uint32_t flags = 0;
    if (!callTriangle(sh, id, t, &flags)) continue;
    const pxr::MeshTri m{Vec3(t[0], t[1], t[2]), Vec3(t[3], t[4], t[5]), Vec3(t[6], t[7], t[8])};
    float x0 = FLT_MAX, x1 = -FLT_MAX, z0 = FLT_MAX, z1 = -FLT_MAX;
    for (const Vec3* v : {&m.v0, &m.v1, &m.v2}) {
      x0 = fminf(x0, v->x), x1 = fmaxf(x1, v->x);
      z0 = fminf(z0, v->z), z1 = fmaxf(z1, v->z);
    }
    const float eps = 1e-3f * (fabsf(e.scale[1]) + fabsf(e.scale[2]));  // scales may be negative (mirrored)
    const float xa = r * e.scale[1], xb = (r + 1) * e.scale[1], za = c * e.scale[2], zb = (c + 1) * e.scale[2];
    if (x0 < fminf(xa, xb) - eps || x1 > fmaxf(xa, xb) + eps || z0 < fminf(za, zb) - eps || z1 > fmaxf(za, zb) + eps)
      ++misplaced;
    // front = (v1 - v0) x (v2 - v0) in our solver; the top of a heightfield is +y in the shape frame
    const Vec3 a = m.v1 - m.v0, b = m.v2 - m.v0;
    const float ny = a.z * b.x - a.x * b.z;  // y component of a x b
    (ny >= 0 ? up : down)++;
    e.tris.push_back(m);
  }
  e.valid = true;
  logLine("geometry: heightfield %08X shape %08X %ux%u samples, %zu triangles (%u facing +y, %u facing -y, %u outside "
          "their cell; %u holes of %u cells), scales %g %g %g",
          hf, sh, rows, cols, e.tris.size(), up, down, misplaced, nIds - (uint32_t)e.tris.size(), nIds,
          e.scale[0], e.scale[1], e.scale[2]);
  return true;
}

// Appends the heightfield's world triangles (through the cache when one is given).
bool appendHeightFieldTris(uint32_t sh, const Pose& pose, pxr::StaticShape& out, HfCache* cache) {
  HfEntry local;
  HfEntry& e = cache ? (*cache)[sh] : local;
  const uint32_t field = callU32(sh, kHfShapeGetField);
  const float scale[3] = {callF32(sh, kHfShapeGetHeightScale), callF32(sh, kHfShapeGetRowScale),
                          callF32(sh, kHfShapeGetColumnScale)};
  if (!e.valid || e.field != field || memcmp(e.scale, scale, sizeof scale) != 0) {
    e = HfEntry{};
    if (!readHeightField(sh, e)) return false;
  }
  out.tris.reserve(out.tris.size() + e.tris.size());
  for (const pxr::MeshTri& m : e.tris) out.tris.push_back({pose.apply(m.v0), pose.apply(m.v1), pose.apply(m.v2)});
  return true;
}

// One NxShape as one of our static shapes (world space). Triangle/convex meshes and heightfields become a Mesh shape
// with its own midphase; rotated boxes and tilted capsules keep their type, oriented by the shape pose. Returns false
// for unsupported shapes (wheel). hf caches heightfield triangles per shape (null: read every time).
bool shapeToStatic(uint32_t sh, uint32_t type, const Pose& pose, pxr::StaticShape& s, HfCache* hf = nullptr) {
  s = pxr::StaticShape{pxr::StaticShape::Plane};
  switch (type) {
    case kPlane: {
      float plane[4];  // NxPlane: normal, d (n.x = d)
      callRet(sh, kPlaneGetPlane, plane);
      s.n = Vec3(plane[0], plane[1], plane[2]);
      s.d = plane[3];
      return true;
    }
    case kSphere:
      s.type = pxr::StaticShape::Sphere;
      s.r = callF32(sh, kSphereGetRadius);
      s.c = pose.t;
      return true;
    case kBox: {
      float h[3];
      callRet(sh, kBoxGetDimensions, h);
      s.type = pxr::StaticShape::Box;
      s.half = Vec3(h[0], h[1], h[2]);
      if (axisAligned(pose)) {
        s.c = pose.t;
      } else {  // rotated: box rules in the box's frame
        s.oriented = true;
        s.frame = pose;
      }
      return true;
    }
    case kCapsule: {
      const Vec3 axis = poseCol(pose, 1);  // our capsule runs along world y, NxCapsuleShape along its local y
      s.type = pxr::StaticShape::Capsule;
      s.r = callF32(sh, kCapsuleGetRadius);
      s.h = callF32(sh, kCapsuleGetHeight);
      if (fabsf(fabsf(axis.y) - 1.0f) < 1e-5f) {
        s.c = pose.t;
      } else {  // tilted: capsule rules in the capsule's frame
        s.oriented = true;
        s.frame = pose;
      }
      return true;
    }
    case kMesh:
    case kConvex:
      s.type = pxr::StaticShape::Mesh;
      s.convex = type == kConvex;
      if (!appendMeshTris(callU32(sh, type == kMesh ? kMeshShapeGetMesh : kConvexShapeGetMesh), pose, s)) return false;
      pxr::buildMeshBVH(s);
      return true;
    case kHeightField:
      s.type = pxr::StaticShape::Mesh;
      if (!appendHeightFieldTris(sh, pose, s, hf)) return false;
      pxr::buildMeshBVH(s);
      return true;
    default:
      return false;
  }
}

// World bounds of one of our static shapes (planes: everything).
void staticBounds(const pxr::StaticShape& s, Vec3& lo, Vec3& hi) {
  if (!pxr::staticShapeBounds(s, lo, hi)) lo = Vec3(-FLT_MAX, -FLT_MAX, -FLT_MAX), hi = Vec3(FLT_MAX, FLT_MAX, FLT_MAX);
}

void readFilter(uint32_t scene, Filter& filt) {
  ((void(__fastcall*)(uint32_t, void*, uint32_t*, uint32_t*, uint32_t*))slotOf(scene, kSceneGetFilterOps))(
      scene, nullptr, &filt.op[0], &filt.op[1], &filt.op[2]);
  filt.value = callBool(scene, kSceneGetFilterBool);
  callRet(scene, kSceneGetFilterConstant0, &filt.k0);
  callRet(scene, kSceneGetFilterConstant1, &filt.k1);
}

// One cached static NxShape.
struct StaticEntry {
  pxr::StaticShape geom;
  Vec3 lo, hi;
  uint32_t actor = 0, type = 0, mesh = 0;
  uint32_t group = 0;
  GroupsMask mask;
  Pose pose;
  bool usable = false;   // converted (supported type)
  bool collides = true;  // shapeCollidesWithFluid() (shape flags, actor flags, rigid-body compartment)
  uint32_t shapeBits = 0, actorBits = 0;  // the fluid-relevant flag bits last read (kShape*, kActor* in fluid.h)
  bool rbComp = false;
  uint32_t seen = 0;
};

// One shape of a dynamic actor, in the actor's frame.
struct DynShapeEntry {
  uint32_t shape = 0, type = 0;
  Pose local;                                  // shape pose in the actor frame
  std::shared_ptr<const pxr::StaticShape> geom;  // shape-frame geometry (null: unsupported)
  float radius = 0;                            // bounding sphere about the shape origin
  uint32_t group = 0;
  GroupsMask mask;
  uint32_t bits = 0;  // readShapeBits()
  bool collides = true, drain = false, twoWay = false;  // collides: shapeCollidesWithFluid() without the actor's flags
};

struct DynActorEntry {
  std::vector<DynShapeEntry> shapes;
  uint32_t nShapes = 0, shapesPtr = 0, builtAt = 0;
  float radius = 0;  // bounding sphere of all shapes about the actor origin
  Vec3 lastPos;      // actor origin at the previous refresh (the culling sweeps from it)
  bool hasLast = false;
};

// A dynamic shape read in the last refresh: d.end is the pose at refresh time, d.begin unused (selectDynamic sets
// both for the substep), box the world bounds swept over the whole frame.
struct DynNow {
  pxr::DynamicShape d;
  float radius;
  bool fresh;  // actor first seen in this refresh: no earlier pose, so it does not sweep (it appears at d.end)
  Box3 box;
  uint32_t group;
  GroupsMask mask;
};

enum ActorKind : uint8_t { kStaticActor = 1, kDynamicActor = 2 };

}  // namespace

struct SceneGeometry::State {
  std::map<uint32_t, StaticEntry> statics;  // key: NxShape*
  struct Flat {
    Vec3 lo, hi;
    const pxr::StaticShape* geom;
    uint32_t group;
    GroupsMask mask;
  };
  std::vector<Flat> flat;  // usable, colliding statics in map order (rebuilt with every rescan)
  std::unordered_map<uint32_t, uint32_t> kind;  // NxActor* -> ActorKind | (generation << 8)
  std::unordered_map<uint32_t, DynActorEntry> dynActors;
  std::map<uint32_t, std::shared_ptr<const pxr::StaticShape>> convexes;  // NxConvexMesh* -> shape-frame triangles
  HfCache hfields;  // NxShape* of static heightfields -> shape-frame triangles (pruned with the statics)
  std::vector<uint32_t> dynList;  // dynamic actors of the last refresh
  std::vector<DynNow> dynNow;
  Filter filt;
  uint8_t groupPair[32][32];  // NxScene::getGroupCollisionFlag
  uint32_t staticSig[2] = {~0u, ~0u}, refreshes = 0, generation = 0, staticGeneration = 0;
};

namespace {

const uint32_t kActorGetGlobalPose = 9, kActorReadActorFlag = 27, kActorGetCMassGlobalPosition = 43,
               kActorGetLinearVelocity = 58, kActorGetAngularVelocity = 59, kActorAddForce = 72, kActorAddTorque = 74,
               kActorReadBodyFlag = 91;
const uint32_t kShapeGetLocalPose = 10;
const uint32_t kBfKinematic = 1u << 7;
const uint32_t kSfFluidTwoWay = 1u << 11;
const uint32_t kForceImpulse = 1;  // NX_IMPULSE

double nowMs() {
  static LARGE_INTEGER freq = [] { LARGE_INTEGER f; QueryPerformanceFrequency(&f); return f; }();
  LARGE_INTEGER t;
  QueryPerformanceCounter(&t);
  return 1000.0 * (double)t.QuadPart / (double)freq.QuadPart;
}

// A dynamic shape's pose s seconds before `end`, integrated back with its velocities (semi-implicit Euler, what
// the rigid step did): rotated about the centre of mass by -w s, the centre of mass moved by -v s.
Pose poseBack(const pxr::DynamicShape& d, const Pose& end, float s) {
  if (s == 0) return end;
  const Pose back = rotationOf(d.angVel * -s);
  Pose r = compose(back, end);
  r.t = d.com - d.linVel * s + back.rotate(end.t - d.com);
  return r;
}

bool boxesOverlap(const Box3& a, const Box3& b) {
  return a.lo.x <= b.hi.x && b.lo.x <= a.hi.x && a.lo.y <= b.hi.y && b.lo.y <= a.hi.y && a.lo.z <= b.hi.z && b.lo.z <= a.hi.z;
}

// Bounding sphere radius of a shape-frame geometry about its origin.
float localRadius(const pxr::StaticShape& g) {
  switch (g.type) {
    case pxr::StaticShape::Sphere: return g.r;
    case pxr::StaticShape::Box: return sqrtf(g.half.len2());
    case pxr::StaticShape::Capsule: return g.r + 0.5f * g.h;
    case pxr::StaticShape::Mesh: {
      float r2 = 0;
      for (const pxr::MeshTri& t : g.tris)
        for (const Vec3* v : {&t.v0, &t.v1, &t.v2}) r2 = fmaxf(r2, v->len2());
      return sqrtf(r2);
    }
    default: return 0;
  }
}

// Shape-frame geometry of a dynamic actor's shape; convexes are shared per mesh.
std::shared_ptr<const pxr::StaticShape> localGeometry(SceneGeometry::State& S, uint32_t sh, uint32_t type) {
  const Pose identity;
  if (type == kConvex || type == kMesh) {
    const uint32_t mesh = callU32(sh, type == kMesh ? kMeshShapeGetMesh : kConvexShapeGetMesh);
    auto it = S.convexes.find(mesh);
    if (it != S.convexes.end()) return it->second;
    auto g = std::make_shared<pxr::StaticShape>();
    if (!shapeToStatic(sh, type, identity, *g)) g.reset();
    S.convexes[mesh] = g;
    return g;
  }
  auto g = std::make_shared<pxr::StaticShape>();
  if (!shapeToStatic(sh, type, identity, *g) || g->type == pxr::StaticShape::Plane) return nullptr;
  return g;
}

// The NxShape flags fluid collision reads, as one word of the solver's kShape* bits (trigger bits one by one).
uint32_t readShapeBits(uint32_t sh) {
  uint32_t b = 0;
  // NxShape::getFlag returns NX_BOOL (an int): the whole word, not just al, so read it as u32
  for (uint32_t t : kSfTriggerBits)
    if (callU32(sh, kShapeGetFlag, t)) b |= t;
  const uint32_t others[] = {kSfDisableCollision, kSfFluidDrain, kSfFluidDisableCollision, kSfDisableResponse};
  for (uint32_t f : others)
    if (callU32(sh, kShapeGetFlag, f)) b |= f;
  return b;
}

// Coverage (docs/audit/GAPS.md rows 10, 11): cloth-only shape flags the hosted cloth does not read, and the fluid-only
// gates it applies to cloth as well. Called when a shape's flags are first read or change.
void coverShapeFlags(uint32_t sh, uint32_t shapeBits, uint32_t actorBits, bool rbComp) {
  const uint32_t kSfClothDrain = 1u << 15, kSfClothDisableCollision = 1u << 16;  // Nxp.h
  if (callU32(sh, kShapeGetFlag, kSfClothDrain) || callU32(sh, kShapeGetFlag, kSfClothDisableCollision))
    cov(kCovShapeClothFlags);
  if ((shapeBits & (kSfFluidDisableCollision | kSfDisableResponse)) || (actorBits & pxr::kActorFluidDisableCollision) || rbComp)
    cov(kCovShapeFluidGates);
}

// The NxActor flags fluid collision reads (kActor* bits of the solver).
uint32_t readActorBits(uint32_t actor) {
  uint32_t b = 0;
  const uint32_t flags[] = {pxr::kActorDisableCollision, pxr::kActorDisableResponse, pxr::kActorFluidDisableCollision};
  for (uint32_t f : flags)
    if (callBool(actor, kActorReadActorFlag, f)) b |= f;
  return b;
}

bool inRigidBodyCompartment(uint32_t actor) {
  const uint32_t comp = callU32(actor, kActorGetCompartment);
  return comp != 0 && callU32(comp, kCompartmentGetType) == kCompartmentRigidBody;
}

// Reads a static shape's fluid flags, group and drain into its entry (call after shapeToStatic: it may reset geom).
// Returns true if any of them changed.
bool readStaticFlags(uint32_t sh, uint32_t actor, StaticEntry& e) {
  const uint32_t sb = readShapeBits(sh), ab = readActorBits(actor);
  const bool rb = inRigidBodyCompartment(actor);
  const uint32_t group = callU32(sh, kShapeGetGroup) & 31;
  const bool changed = sb != e.shapeBits || ab != e.actorBits || rb != e.rbComp || group != e.group;
  if (changed) coverShapeFlags(sh, sb, ab, rb);
  e.shapeBits = sb, e.actorBits = ab, e.rbComp = rb, e.group = group;
  e.collides = pxr::shapeCollidesWithFluid(sb, ab, rb);
  e.geom.group = (uint16_t)group;
  e.geom.drain = (sb & kSfFluidDrain) != 0;
  return changed;
}

// The selection list: usable, colliding statics in map order (the solver's results depend on the order).
void rebuildFlat(SceneGeometry::State& S) {
  S.flat.clear();
  for (const auto& kv : S.statics) {
    const StaticEntry& e = kv.second;
    if (e.usable && e.collides) S.flat.push_back({e.lo, e.hi, &e.geom, e.group, e.mask});
  }
}

void buildDynActor(SceneGeometry::State& S, uint32_t actor, DynActorEntry& e) {
  e.shapes.clear();
  e.radius = 0;
  e.nShapes = callU32(actor, kActorGetNbShapes);
  e.shapesPtr = callU32(actor, kActorGetShapes);
  e.builtAt = S.refreshes;
  const uint32_t* shapes = fromPtr32<const uint32_t>(e.shapesPtr);
  for (uint32_t si = 0; si < e.nShapes; ++si) {
    DynShapeEntry d;
    d.shape = shapes[si];
    d.type = callU32(d.shape, kShapeGetType);
    callRet(d.shape, kShapeGetLocalPose, &d.local);
    d.group = callU32(d.shape, kShapeGetGroup) & 31;
    callRet(d.shape, kShapeGetGroupsMask, &d.mask);
    d.bits = readShapeBits(d.shape);
    coverShapeFlags(d.shape, d.bits, 0, false);
    d.drain = (d.bits & kSfFluidDrain) != 0;
    d.collides = pxr::shapeCollidesWithFluid(d.bits, 0, false);  // dynamic actors: the compartment rule does not apply
    d.twoWay = callU32(d.shape, kShapeGetFlag, kSfFluidTwoWay) != 0;
    d.geom = localGeometry(S, d.shape, d.type);
    if (d.geom) {
      d.radius = localRadius(*d.geom);
      e.radius = fmaxf(e.radius, sqrtf(d.local.t.len2()) + d.radius);
    }
    e.shapes.push_back(d);
  }
}

}  // namespace

SceneGeometry::SceneGeometry() : st_(new State()) {}
SceneGeometry::~SceneGeometry() { delete st_; }

uint32_t SceneGeometry::staticCount() const { return (uint32_t)st_->statics.size(); }
uint32_t SceneGeometry::dynamicNear() const { return (uint32_t)st_->dynNow.size(); }

void SceneGeometry::refresh(uint32_t scene, const std::vector<Box3>& nearBoxes, float dt, bool census) {
  State& S = *st_;
  const double t0 = nowMs();
  ++S.refreshes;
  ++S.generation;
  // 1) Classify the actors (isDynamic only for actors not seen before) and fingerprint the static set.
  const uint32_t nActors = callU32(scene, kSceneGetNbActors);
  const uint32_t* actors = fromPtr32<const uint32_t>(callU32(scene, kSceneGetActors));
  uint32_t sig[2] = {0, 0};
  S.dynList.clear();
  for (uint32_t ai = 0; ai < nActors; ++ai) {
    const uint32_t actor = actors[ai];
    auto it = S.kind.find(actor);
    uint8_t k;
    if (it == S.kind.end()) {
      k = callBool(actor, kActorIsDynamic) ? kDynamicActor : kStaticActor;
      S.kind.emplace(actor, k | (S.generation << 8));
    } else {
      k = (uint8_t)(it->second & 0xff);
      it->second = k | (S.generation << 8);
    }
    if (k == kStaticActor) {
      sig[0] += 1;
      sig[1] ^= actor * 2654435761u + (actor >> 7);
    } else {
      S.dynList.push_back(actor);
    }
  }
  if (S.kind.size() > nActors) {  // forget released actors
    for (auto it = S.kind.begin(); it != S.kind.end();) {
      if ((it->second >> 8) != (S.generation & 0xffffff)) {
        S.dynActors.erase(it->first);
        it = S.kind.erase(it);
      } else {
        ++it;
      }
    }
  }
  const double t1 = nowMs();

  // 2) Static shapes: rescan when the static actor set changed, and check the poses every 600 refreshes.
  const bool staticChanged = sig[0] != S.staticSig[0] || sig[1] != S.staticSig[1];
  const bool rescan = staticChanged || S.refreshes % 600 == 1 || census;
  if (rescan) {
    S.staticSig[0] = sig[0], S.staticSig[1] = sig[1];
    ++S.staticGeneration;
    readFilter(scene, S.filt);
    uint32_t added = 0, rebuilt = 0, unsupported = 0, typeCount[8] = {0}, nStatic = 0;
    for (uint32_t ai = 0; ai < nActors; ++ai) {
      const uint32_t actor = actors[ai];
      if ((S.kind[actor] & 0xff) != kStaticActor) continue;
      ++nStatic;
      const uint32_t nShapes = callU32(actor, kActorGetNbShapes);
      const uint32_t* shapes = fromPtr32<const uint32_t>(callU32(actor, kActorGetShapes));
      for (uint32_t si = 0; si < nShapes; ++si) {
        const uint32_t sh = shapes[si];
        const uint32_t type = callU32(sh, kShapeGetType);
        if (type < 8) ++typeCount[type];
        Pose pose;
        callRet(sh, kShapeGetGlobalPose, &pose);
        const uint32_t mesh = type == kMesh ? callU32(sh, kMeshShapeGetMesh) : type == kConvex ? callU32(sh, kConvexShapeGetMesh)
                              : type == kHeightField ? callU32(sh, kHfShapeGetField) : 0;
        auto it = S.statics.find(sh);
        if (it != S.statics.end() && it->second.actor == actor && it->second.type == type && it->second.mesh == mesh &&
            !memcmp(&it->second.pose, &pose, sizeof(pose))) {
          it->second.seen = S.staticGeneration;
          continue;
        }
        (it == S.statics.end() ? added : rebuilt)++;
        StaticEntry& e = S.statics[sh];
        e.actor = actor, e.type = type, e.mesh = mesh, e.pose = pose, e.seen = S.staticGeneration;
        callRet(sh, kShapeGetGroupsMask, &e.mask);
        e.usable = shapeToStatic(sh, type, pose, e.geom, &S.hfields);
        readStaticFlags(sh, actor, e);
        if (e.usable) staticBounds(e.geom, e.lo, e.hi);
        else ++unsupported;
      }
    }
    uint32_t removed = 0;
    for (auto it = S.statics.begin(); it != S.statics.end();)
      if (it->second.seen != S.staticGeneration) {
        it = S.statics.erase(it);
        ++removed;
      } else {
        ++it;
      }
    for (auto it = S.hfields.begin(); it != S.hfields.end();)  // heightfield caches of released shapes
      it = S.statics.count(it->first) ? std::next(it) : S.hfields.erase(it);
    rebuildFlat(S);
    if (census || added || removed || rebuilt)
      logLine("static geometry scene %08X: %u actors (%u static, %u dynamic), %u static shapes (+%u, %u rebuilt, -%u, "
              "%u unsupported) in %.2f ms; types plane %u sphere %u box %u capsule %u wheel %u convex %u mesh %u hf %u",
              scene, nActors, nStatic, (unsigned)S.dynList.size(), (unsigned)S.statics.size(), added, rebuilt, removed,
              unsupported, nowMs() - t1, typeCount[0], typeCount[1], typeCount[2], typeCount[3], typeCount[4],
              typeCount[5], typeCount[6], typeCount[7]);
  }
  // The scene's group table (NxScene::getGroupCollisionFlag, 1024 reads): every refresh, as the game may change it.
  for (uint32_t a = 0; a < 32; ++a)
    for (uint32_t b = 0; b < 32; ++b) S.groupPair[a][b] = callBool(scene, kSceneGetGroupCollisionFlag, a, b) ? 1 : 0;
  // Static flags and groups can change at runtime: a rescan reads them, otherwise they are re-read every
  // kStaticFlagRecheck refreshes and the selection list is rebuilt when one changed.
  if (!rescan && S.refreshes % kStaticFlagRecheck == 0) {
    uint32_t changed = 0;
    for (auto& kv : S.statics) changed += readStaticFlags(kv.first, kv.second.actor, kv.second) ? 1 : 0;
    if (changed) {
      rebuildFlat(S);
      logLine("static fluid flags: %u shapes changed on scene %08X (%u in the selection list)", changed, scene,
              (unsigned)S.flat.size());
    }
  }
  const double t2 = nowMs();

  // 3) Dynamic actors near any fluid, at their current pose.
  S.dynNow.clear();
  uint32_t censusShapes = 0, censusUnsupported = 0, censusTypes[8] = {0};
  for (const uint32_t actor : S.dynList) {
    DynActorEntry& e = S.dynActors[actor];
    const bool fresh = e.builtAt == 0;
    if (fresh) buildDynActor(S, actor, e);
    if (census)
      for (const DynShapeEntry& d : e.shapes) {
        ++censusShapes;
        if (d.type < 8) ++censusTypes[d.type];
        if (!d.geom) ++censusUnsupported;
      }
    Pose ap;
    callRet(actor, kActorGetGlobalPose, &ap);
    const Vec3 from = e.hasLast ? e.lastPos : ap.t;
    e.lastPos = ap.t;
    e.hasLast = true;
    if (e.radius <= 0) continue;
    // swept bounds of the actor's sphere since the last refresh
    const Box3 sweep{Vec3(fminf(from.x, ap.t.x), fminf(from.y, ap.t.y), fminf(from.z, ap.t.z)) - Vec3(e.radius, e.radius, e.radius),
                     Vec3(fmaxf(from.x, ap.t.x), fmaxf(from.y, ap.t.y), fmaxf(from.z, ap.t.z)) + Vec3(e.radius, e.radius, e.radius)};
    bool isNear = false;
    for (const Box3& b : nearBoxes) isNear = isNear || boxesOverlap(sweep, b);
    if (!isNear) continue;
    // Near: make sure the shape list is current (shapes added/released, or the address reused by a new actor).
    if (!fresh && (callU32(actor, kActorGetNbShapes) != e.nShapes || callU32(actor, kActorGetShapes) != e.shapesPtr ||
                   S.refreshes - e.builtAt >= 120))
      buildDynActor(S, actor, e);
    if (!pxr::shapeCollidesWithFluid(0, readActorBits(actor), false)) continue;
    Vec3 v, w, com;
    callRet(actor, kActorGetLinearVelocity, &v);
    callRet(actor, kActorGetAngularVelocity, &w);
    callRet(actor, kActorGetCMassGlobalPosition, &com);
    const bool kinematic = callBool(actor, kActorReadBodyFlag, kBfKinematic);
    for (const DynShapeEntry& d : e.shapes) {
      if (!d.geom || !d.collides) continue;
      DynNow n;
      n.d.geom = d.geom.get();
      n.d.shape = d.shape;
      n.d.end = compose(ap, d.local);
      n.d.linVel = v, n.d.angVel = w, n.d.com = com;
      n.d.twoWay = d.twoWay && !kinematic;
      n.d.body = actor;
      n.d.drain = d.drain;
      n.radius = d.radius;
      n.fresh = fresh;
      const Vec3 a = fresh ? n.d.end.t : poseBack(n.d, n.d.end, dt).t, b = n.d.end.t, r(d.radius, d.radius, d.radius);
      n.box = {Vec3(fminf(a.x, b.x), fminf(a.y, b.y), fminf(a.z, b.z)) - r, Vec3(fmaxf(a.x, b.x), fmaxf(a.y, b.y), fmaxf(a.z, b.z)) + r};
      n.group = d.group;
      n.mask = d.mask;
      S.dynNow.push_back(n);
    }
  }
  if (census)
    logLine("dynamic geometry scene %08X: %u dynamic actors, %u shapes (%u unsupported); types plane %u sphere %u box %u "
            "capsule %u wheel %u convex %u mesh %u hf %u; %u near the fluids", scene, (unsigned)S.dynList.size(),
            censusShapes, censusUnsupported, censusTypes[0], censusTypes[1], censusTypes[2], censusTypes[3],
            censusTypes[4], censusTypes[5], censusTypes[6], censusTypes[7], (unsigned)S.dynNow.size());
  const double t3 = nowMs();
  timing_.actors = t1 - t0, timing_.statics = t2 - t1, timing_.dynamics = t3 - t2;
}

void SceneGeometry::collisionGroups(pxr::CollisionGroups& out) const {
  const State& S = *st_;
  for (uint32_t a = 0; a < 32; ++a) {
    out.allow[a] = 0;
    for (uint32_t b = 0; b < 32; ++b)
      if (S.groupPair[a][b]) out.allow[a] |= 1u << b;
  }
}

void SceneGeometry::selectStatic(const FluidFilterDesc& fluid, const Box3& box, std::vector<const pxr::StaticShape*>& out) const {
  const State& S = *st_;
  out.clear();
  if (!fluid.includeStatic) return;
  GroupsMask fm;
  memcpy(fm.bits, fluid.groupsMask, sizeof(fm.bits));
  const uint8_t* pair = S.groupPair[fluid.collisionGroup & 31];
  for (const State::Flat& e : S.flat) {
    if (e.lo.x > box.hi.x || e.lo.y > box.hi.y || e.lo.z > box.hi.z || e.hi.x < box.lo.x || e.hi.y < box.lo.y || e.hi.z < box.lo.z)
      continue;
    if (!pair[e.group]) continue;
    if (fluid.useGroupsMask && !filterPasses(S.filt, fm, e.mask)) continue;
    out.push_back(e.geom);
  }
}

void SceneGeometry::selectDynamic(const FluidFilterDesc& fluid, const Box3& box, float backBegin, float backEnd,
                                  std::vector<pxr::DynamicShape>& out) const {
  const State& S = *st_;
  out.clear();
  if (!fluid.includeDynamic) return;
  GroupsMask fm;
  memcpy(fm.bits, fluid.groupsMask, sizeof(fm.bits));
  for (const DynNow& n : S.dynNow) {
    if (!S.groupPair[fluid.collisionGroup & 31][n.group] || !boxesOverlap(n.box, box)) continue;
    if (fluid.useGroupsMask && !filterPasses(S.filt, fm, n.mask)) continue;
    pxr::DynamicShape d = n.d;
    d.group = (uint16_t)n.group;
    if (n.fresh) {
      d.begin = d.end;  // [A] the original has no earlier pose for an actor created this frame either
    } else {
      d.begin = poseBack(n.d, n.d.end, backBegin);
      d.end = poseBack(n.d, n.d.end, backEnd);
      if (backEnd != 0) d.com = n.d.com - n.d.linVel * backEnd;
    }
    out.push_back(d);
  }
}

void SceneGeometry::applyImpulses(const std::vector<pxr::BodyImpulse>& impulses) const {
  const State& S = *st_;
  struct Sum {
    Vec3 lin, ang;
  };
  std::map<uint32_t, Sum> sums;
  for (const pxr::BodyImpulse& bi : impulses) {
    auto it = S.kind.find(bi.body);
    if (it == S.kind.end() || (it->second & 0xff) != kDynamicActor) continue;  // released since
    Vec3 com;
    callRet(bi.body, kActorGetCMassGlobalPosition, &com);
    Sum& s = sums[bi.body];
    s.lin += bi.impulse;
    const Vec3 r = bi.pos - com;
    s.ang += Vec3(r.y * bi.impulse.z - r.z * bi.impulse.y, r.z * bi.impulse.x - r.x * bi.impulse.z, r.x * bi.impulse.y - r.y * bi.impulse.x);
  }
  for (const auto& kv : sums) {
    // addForce / addTorque(const NxVec3&, NxForceMode, bool wakeup)
    ((void(__fastcall*)(uint32_t, void*, const Vec3*, uint32_t, uint32_t))slotOf(kv.first, kActorAddForce))(
        kv.first, nullptr, &kv.second.lin, kForceImpulse, 1);
    ((void(__fastcall*)(uint32_t, void*, const Vec3*, uint32_t, uint32_t))slotOf(kv.first, kActorAddTorque))(
        kv.first, nullptr, &kv.second.ang, kForceImpulse, 1);
  }
}

bool SceneGeometry::findShapeGeometry(uint32_t nxShape, const pxr::StaticShape*& geom) const {
  const State& S = *st_;
  auto it = S.statics.find(nxShape);
  if (it != S.statics.end() && it->second.usable) {
    geom = &it->second.geom;
    return true;
  }
  for (const auto& kv : S.dynActors)
    for (const DynShapeEntry& d : kv.second.shapes)
      if (d.shape == nxShape && d.geom) {
        geom = d.geom.get();
        return true;
      }
  return false;
}

uint32_t sceneActorCount(uint32_t scene) { return callU32(scene, kSceneGetNbActors); }

void readSceneSettings(uint32_t scene, SceneSettings& s) {
  callOut(scene, kSceneGetGravity, &s.gravity);
  float maxTimestep = 0;
  uint32_t maxIter = 0, method = 0;
  ((void(__fastcall*)(uint32_t, void*, float*, uint32_t*, uint32_t*, uint32_t*))slotOf(scene, kSceneGetTiming))(
      scene, nullptr, &maxTimestep, &maxIter, &method, nullptr);
  s.maxTimestep = maxTimestep;
  s.maxIter = maxIter;
  s.timestepMethod = method;
}

}  // namespace shim
