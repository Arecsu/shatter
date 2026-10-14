// Replays a cloth recording from the game (PhysXLoader_cloth<id>.pxcr, solver/cloth/cloth_rec.h) through the original
// PhysX core, to compare it with ours (solver/cloth/cloth_replay.cpp) on exactly the game's input.
//
//   cloth_replay_oracle.exe <file.pxcr> <out.bin> [first last] [--loader PhysXLoader.dll]
//
// The cloth is cooked from the recorded rest mesh and created with the recorded parameters, then every call is
// repeated: pins, velocity writes, forces, wake/sleep, parameter changes. Shapes: a slot whose pose never changes is a
// static actor; one that moves is a kinematic actor driven to the recorded pose before each step (meshes on moving
// slots are cooked as convexes, static ones as triangle meshes). Two-way reactions are not fed back (the recorded
// bodies move as they did in the game). Only the first Create is replayed.
// Output: PXCL as cloth_oracle, steps first..last (default all).
#include <algorithm>
#include <map>
#include <string>
#include <vector>

#include "oracle_common.h"
#include "NxStream.h"
#include "NxCooking.h"
#include "../../solver/cloth/cloth_rec.h"

namespace {

typedef NxCookingInterface*(NX_CALL_CONV* PFN_NxGetCookingLib)(NxU32);

class MemStream : public NxStream {
 public:
  std::vector<NxU8> data;
  mutable size_t rd = 0;
  NxU8 readByte() const override { NxU8 v; readBuffer(&v, 1); return v; }
  NxU16 readWord() const override { NxU16 v; readBuffer(&v, 2); return v; }
  NxU32 readDword() const override { NxU32 v; readBuffer(&v, 4); return v; }
  NxF32 readFloat() const override { NxF32 v; readBuffer(&v, 4); return v; }
  NxF64 readDouble() const override { NxF64 v; readBuffer(&v, 8); return v; }
  void readBuffer(void* b, NxU32 n) const override {
    if (rd + n > data.size()) { memset(b, 0, n); rd = data.size(); return; }
    memcpy(b, data.data() + rd, n);
    rd += n;
  }
  NxStream& storeByte(NxU8 v) override { return storeBuffer(&v, 1); }
  NxStream& storeWord(NxU16 v) override { return storeBuffer(&v, 2); }
  NxStream& storeDword(NxU32 v) override { return storeBuffer(&v, 4); }
  NxStream& storeFloat(NxF32 v) override { return storeBuffer(&v, 4); }
  NxStream& storeDouble(NxF64 v) override { return storeBuffer(&v, 8); }
  NxStream& storeBuffer(const void* b, NxU32 n) override {
    data.insert(data.end(), (const NxU8*)b, (const NxU8*)b + n);
    return *this;
  }
};

using pxc::V3;

struct Reader {
  std::vector<uint8_t> b;
  size_t o = 0;
  bool ok = true;
  void raw(void* p, size_t n) {
    if (o + n > b.size()) { ok = false; memset(p, 0, n); return; }
    memcpy(p, b.data() + o, n);
    o += n;
  }
  uint8_t u8() { uint8_t v; raw(&v, 1); return v; }
  uint32_t u32() { uint32_t v; raw(&v, 4); return v; }
  float f() { float v; raw(&v, 4); return v; }
  V3 v3() { float a = f(), c = f(), d = f(); return V3(a, c, d); }
  template <class T> std::vector<T> vec() {
    std::vector<T> v(u32());
    if (!v.empty()) raw(v.data(), v.size() * sizeof(T));
    return v;
  }
};

struct Params {  // the recorded ClothParams blob
  float thickness, density, bending, stretching, damping, friction;
  uint32_t iters;
  float sleepV, wakeC;
  V3 ext, wind;
  bool gravity, bendingOn, ortho, dampingOn, comDamping, tearing, selfColl;
  float tearFactor;
  uint32_t maxV;
  float attResp, attTear;
  bool noDynCcd;
};
Params paramsFrom(const std::vector<uint8_t>& blob) {
  Reader r;
  r.b = blob;
  Params p;
  p.thickness = r.f(), p.density = r.f(), p.bending = r.f(), p.stretching = r.f(), p.damping = r.f(), p.friction = r.f();
  p.iters = r.u32(), p.sleepV = r.f(), p.wakeC = r.f();
  p.ext = r.v3(), p.wind = r.v3();
  p.gravity = r.u8(), p.bendingOn = r.u8(), p.ortho = r.u8(), p.dampingOn = r.u8(), p.comDamping = r.u8();
  p.tearing = r.u8(), p.selfColl = r.u8();
  p.tearFactor = r.f(), p.maxV = r.u32(), p.attResp = r.f(), p.attTear = r.f();
  p.noDynCcd = r.u8();
  return p;
}
NxU32 flagsOf(const Params& p) {
  NxU32 f = 0;
  if (p.gravity) f |= NX_CLF_GRAVITY;
  if (p.bendingOn) f |= NX_CLF_BENDING;
  if (p.ortho) f |= NX_CLF_BENDING_ORTHO;
  if (p.dampingOn) f |= NX_CLF_DAMPING;
  if (p.comDamping) f |= NX_CLF_COMDAMPING;
  if (p.tearing) f |= NX_CLF_TEARABLE;
  if (p.selfColl) f |= NX_CLF_SELFCOLLISION;
  if (p.noDynCcd) f |= NX_CLF_DISABLE_DYNAMIC_CCD;
  return f;
}
NxVec3 nx(const V3& v) { return NxVec3(v.x, v.y, v.z); }

struct Slot {
  uint32_t type = 0;
  V3 a, half;
  float d = 0;
  int32_t mesh = -1;
  bool moves = false;
  bool havePose = false;
  V3 t0;
  float R0[9] = {1, 0, 0, 0, 1, 0, 0, 0, 1};
  NxActor* act = nullptr;
};
NxMat34 poseOf(const V3& t, const float* R) {  // R row-major
  NxMat34 m;
  m.M.setRowMajor(R);
  m.t = nx(t);
  return m;
}

}  // namespace

int main(int argc, char** argv) {
  setvbuf(stdout, nullptr, _IONBF, 0);
  if (argc < 3) {
    fprintf(stderr, "usage: cloth_replay_oracle <file.pxcr> <out.bin> [first last] [--loader path]\n");
    return 1;
  }
  const char* loaderPath = "PhysXLoader.dll";
  float fixedDt = 0;  // --fixed t: the game's scene timing (fixed t-second steps, up to 5 per simulate; Escape_p: 0.02)
  long first = 1, last = 0x7fffffff;
  int pos = 0;
  for (int i = 3; i < argc; ++i) {
    if (!strcmp(argv[i], "--loader") && i + 1 < argc) loaderPath = argv[++i];
    else if (!strcmp(argv[i], "--fixed") && i + 1 < argc) fixedDt = (float)atof(argv[++i]);
    else if (pos == 0) first = atol(argv[i]), ++pos;
    else if (pos == 1) last = atol(argv[i]), ++pos;
  }
  Reader r;
  {
    FILE* f = fopen(argv[1], "rb");
    if (!f) { printf("{\"ok\":false,\"error\":\"cannot open input\"}\n"); return 1; }
    fseek(f, 0, SEEK_END);
    r.b.resize(ftell(f));
    fseek(f, 0, SEEK_SET);
    fread(r.b.data(), 1, r.b.size(), f);
    fclose(f);
  }
  if (r.u32() != 0x52435850u || r.u32() != pxc::kRecVersion) { printf("{\"ok\":false,\"error\":\"not PXCR v1\"}\n"); return 1; }

  // Pre-scan: which slots of the first Create ever change pose (those become kinematic actors).
  std::vector<Slot> slots;
  {
    Reader q = r;
    int creates = 0;
    std::vector<std::vector<uint8_t>> firstMotion;
    while (q.ok && q.o < q.b.size()) {
      const uint8_t op = q.u8();
      if (op == pxc::kRecCreate) {
        if (++creates > 1) break;
        q.vec<uint8_t>(), q.vec<V3>(), q.vec<uint32_t>(), q.vec<V3>();
        if (q.u8()) q.vec<pxc::Edge>();
        q.u8();
      } else if (op == pxc::kRecMesh) { q.u32(); q.vec<V3>(); }
      else if (op == pxc::kRecAddShape) {
        Slot s;
        s.type = q.u32(), s.a = q.v3(), s.d = q.f(), s.half = q.v3(), s.mesh = (int32_t)q.u32();
        slots.push_back(s);
      } else if (op == pxc::kRecPin || op == pxc::kRecAttachGlobal) { q.u32(); q.v3(); }
      else if (op == pxc::kRecInvMass) { q.u32(); q.f(); }
      else if (op == pxc::kRecMotion) {
        const size_t at = q.o;
        const uint32_t s = q.u32();
        V3 t = q.v3();
        float R[9];
        q.raw(R, 36);
        if (q.u8()) q.v3();
        float pR[9];
        if (q.u8()) q.raw(pR, 36);
        q.u8();
        if (s < slots.size()) {
          Slot& sl = slots[s];
          if (!sl.havePose) {
            sl.havePose = true, sl.t0 = t;
            memcpy(sl.R0, R, 36);
          } else if (memcmp(&sl.t0, &t, 12) || memcmp(sl.R0, R, 36)) {
            sl.moves = true;
          }
        }
        (void)at;
      } else if (op == pxc::kRecDetach) q.u32();
      else if (op == pxc::kRecAttachShape) { q.u32(); q.u32(); q.v3(); q.u32(); }
      else if (op == pxc::kRecWake || op == pxc::kRecAttResp || op == pxc::kRecAttTear) q.f();
      else if (op == pxc::kRecSleep) {}
      else if (op == pxc::kRecSetVel || op == pxc::kRecAddVel) { q.u32(); q.v3(); }
      else if (op == pxc::kRecGameVel) { q.u32(); q.v3(); q.v3(); }
      else if (op == pxc::kRecForceV) { q.u32(); q.v3(); q.u32(); q.f(); }
      else if (op == pxc::kRecForcePos) { q.v3(); q.u8(); q.v3(); q.f(); q.f(); q.u32(); q.f(); }
      else if (op == pxc::kRecParams) q.vec<uint8_t>();
      else if (op == pxc::kRecStep) { q.f(); q.v3(); }
      else if (op == pxc::kRecCheck) q.u32();
      else if (op == pxc::kRecNote) { std::vector<char> s(q.u32()); q.raw(s.data(), s.size()); }
      else if (op == pxc::kRecShapeVel) { q.u32(); q.v3(); q.v3(); q.v3(); }
      else if (op == pxc::kRecRemoved) q.vec<uint8_t>();
      else { printf("{\"ok\":false,\"error\":\"bad op %u\"}\n", op); return 1; }
    }
  }

  StderrOutput out;
  LoadedSdk L;
  if (!L.open(loaderPath, &out)) { printf("{\"ok\":false,\"error\":\"sdk create failed\"}\n"); return 2; }
  auto getCooking = (PFN_NxGetCookingLib)GetProcAddress(L.loader, "NxGetCookingLib");
  NxCookingInterface* cooking = getCooking ? getCooking(NX_PHYSICS_SDK_VERSION) : nullptr;
  if (!cooking || !cooking->NxInitCooking(nullptr, &out)) { printf("{\"ok\":false,\"error\":\"cooking\"}\n"); return 2; }

  NxSceneDesc sd;
  sd.gravity = NxVec3(0, 0, 0);
  sd.simType = NX_SIMULATION_SW;
  NxScene* scene = L.sdk->createScene(sd);
  if (!scene) { printf("{\"ok\":false,\"error\":\"createScene failed\"}\n"); return 2; }
  float curDt = -1;
  NxVec3 curG(0, 0, 0);

  std::vector<std::vector<V3>> meshes;
  std::vector<V3> rest;
  std::vector<uint32_t> tri;
  NxCloth* cloth = nullptr;
  Params P{};
  NxU32 maxV = 0, maxI = 0;
  std::vector<float> rpos, rnrm, vel;
  std::vector<NxU32> ridx, rparent;
  NxU32 numV = 0, numI = 0, numParent = 0, dirty = 0;
  size_t nextSlot = 0;
  FILE* fo = nullptr;
  long step = 0;
  NxU32 written = 0;
  std::vector<NxU32> lastIdx;
  int creates = 0;
  bool nan = false;

  auto makeActor = [&](Slot& s) {
    NxActorDesc ad;
    NxBodyDesc bd;
    NxShapeDesc* sh = nullptr;
    NxBoxShapeDesc box;
    NxSphereShapeDesc sph;
    NxCapsuleShapeDesc cap;
    NxPlaneShapeDesc pl;
    NxTriangleMeshShapeDesc tm;
    NxConvexShapeDesc cv;
    const bool posed = s.havePose;
    switch (s.type) {
      case 0: pl.normal = nx(s.a), pl.d = s.d, sh = &pl; break;
      case 1: sph.radius = s.d, sh = &sph; break;
      case 2: box.dimensions = nx(s.half), sh = &box; break;
      case 3: cap.radius = s.d, cap.height = 2.f * s.half.y, sh = &cap; break;
      case 4: {
        if (s.mesh < 0 || (size_t)s.mesh >= meshes.size()) return;
        const std::vector<V3>& v = meshes[s.mesh];
        std::vector<NxU32> idx(v.size());
        for (size_t i = 0; i < idx.size(); ++i) idx[i] = (NxU32)i;
        MemStream ms;
        if (s.moves) {
          NxConvexMeshDesc d;
          d.numVertices = (NxU32)v.size();
          d.pointStrideBytes = 12;
          d.points = v.data();
          d.flags = NX_CF_COMPUTE_CONVEX;
          if (!cooking->NxCookConvexMesh(d, ms)) return;
          cv.meshData = L.sdk->createConvexMesh(ms);
          if (!cv.meshData) return;
          sh = &cv;
        } else {
          NxTriangleMeshDesc d;
          d.numVertices = (NxU32)v.size();
          d.numTriangles = (NxU32)(v.size() / 3);
          d.pointStrideBytes = 12;
          d.triangleStrideBytes = 12;
          d.points = v.data();
          d.triangles = idx.data();
          if (!cooking->NxCookTriangleMesh(d, ms)) return;
          tm.meshData = L.sdk->createTriangleMesh(ms);
          if (!tm.meshData) return;
          sh = &tm;
        }
        break;
      }
      default: return;
    }
    if (s.type != 0 && s.type != 4) sh->localPose.t = nx(s.a);  // unposed: a = world centre; posed: a = 0
    ad.shapes.pushBack(sh);
    if (posed) ad.globalPose = poseOf(s.t0, s.R0);
    if (s.moves) {
      bd.mass = 1.f;
      bd.flags |= NX_BF_KINEMATIC;
      ad.body = &bd;
    }
    s.act = scene->createActor(ad);
    if (!s.act) fprintf(stderr, "actor for slot type %u failed\n", s.type);
  };

  while (r.ok && r.o < r.b.size()) {
    const uint8_t op = r.u8();
    switch (op) {
      case pxc::kRecNote: { std::vector<char> s(r.u32()); r.raw(s.data(), s.size()); break; }
      case pxc::kRecCreate: {
        if (++creates > 1) { r.o = r.b.size(); break; }
        P = paramsFrom(r.vec<uint8_t>());
        rest = r.vec<V3>();
        tri = r.vec<uint32_t>();
        const std::vector<V3> x0 = r.vec<V3>();
        if (r.u8()) r.vec<pxc::Edge>();
        r.u8();
        NxClothMeshDesc md;
        md.numVertices = (NxU32)rest.size();
        md.numTriangles = (NxU32)(tri.size() / 3);
        md.pointStrideBytes = 12;
        md.triangleStrideBytes = 12;
        md.points = rest.data();
        md.triangles = tri.data();
        md.flags = P.tearing ? NX_CLOTH_MESH_TEARABLE : 0;
        MemStream ms;
        if (!cooking->NxCookClothMesh(md, ms)) { printf("{\"ok\":false,\"error\":\"cook cloth\"}\n"); return 3; }
        NxClothMesh* mesh = L.sdk->createClothMesh(ms);
        NxClothDesc cd;
        cd.clothMesh = mesh;
        cd.thickness = P.thickness, cd.density = P.density, cd.bendingStiffness = P.bending;
        cd.stretchingStiffness = P.stretching, cd.dampingCoefficient = P.damping, cd.friction = P.friction;
        cd.solverIterations = P.iters, cd.sleepLinearVelocity = P.sleepV, cd.wakeUpCounter = P.wakeC;
        cd.externalAcceleration = nx(P.ext), cd.windAcceleration = nx(P.wind);
        cd.tearFactor = P.tearFactor, cd.attachmentResponseCoefficient = P.attResp;
        cd.attachmentTearFactor = P.attTear, cd.flags = flagsOf(P);
        maxV = P.maxV ? std::max<NxU32>(P.maxV, (NxU32)rest.size()) : (NxU32)rest.size() * (P.tearing ? 2 : 1);
        maxI = (NxU32)tri.size() * (P.tearing ? 2 : 1);
        rpos.assign(3 * maxV, 0), rnrm.assign(3 * maxV, 0), vel.assign(3 * maxV, 0);
        ridx.assign(maxI, 0), rparent.assign(maxV, 0);
        NxMeshData& w = cd.meshData;
        w.verticesPosBegin = rpos.data(), w.verticesNormalBegin = rnrm.data();
        w.verticesPosByteStride = 12, w.verticesNormalByteStride = 12;
        w.maxVertices = maxV, w.numVerticesPtr = &numV;
        w.indicesBegin = ridx.data(), w.indicesByteStride = 4, w.maxIndices = maxI, w.numIndicesPtr = &numI;
        w.parentIndicesBegin = rparent.data(), w.parentIndicesByteStride = 4, w.maxParentIndices = maxV;
        w.numParentIndicesPtr = &numParent, w.dirtyBufferFlagsPtr = &dirty;
        cloth = scene->createCloth(cd);
        if (!cloth) { printf("{\"ok\":false,\"error\":\"createCloth (valid=%d)\"}\n", (int)cd.isValid()); return 3; }
        cloth->setPositions((void*)x0.data(), 12);
        fo = fopen(argv[2], "wb");
        const NxU32 hdr[5] = {1, 0, maxV, (NxU32)rest.size(), (NxU32)(tri.size() / 3)};
        fwrite("PXCL", 1, 4, fo);
        fwrite(hdr, sizeof hdr, 1, fo);
        const float dt0 = 1.f / 60.f;
        fwrite(&dt0, 4, 1, fo);
        fwrite(tri.data(), 4, tri.size(), fo);
        fwrite(rest.data(), 12, rest.size(), fo);
        break;
      }
      case pxc::kRecMesh: {
        const uint32_t id = r.u32();
        if (meshes.size() <= id) meshes.resize(id + 1);
        meshes[id] = r.vec<V3>();
        break;
      }
      case pxc::kRecAddShape: {
        r.u32(), r.v3(), r.f(), r.v3(), r.u32();
        if (nextSlot < slots.size()) makeActor(slots[nextSlot]);
        ++nextSlot;
        break;
      }
      case pxc::kRecPin:
      case pxc::kRecAttachGlobal: {
        const uint32_t v = r.u32();
        const V3 p = r.v3();
        if (cloth) cloth->attachVertexToGlobalPosition(v, nx(p));
        break;
      }
      case pxc::kRecInvMass: r.u32(), r.f(); break;
      case pxc::kRecMotion: {
        const uint32_t s = r.u32();
        const V3 t = r.v3();
        float R[9], pR[9];
        r.raw(R, 36);
        if (r.u8()) r.v3();
        if (r.u8()) r.raw(pR, 36);
        r.u8();
        if (s < slots.size() && slots[s].moves && slots[s].act) slots[s].act->moveGlobalPose(poseOf(t, R));
        break;
      }
      case pxc::kRecDetach: {
        const uint32_t s = r.u32();
        if (cloth && s < slots.size() && slots[s].act) cloth->detachFromShape(slots[s].act->getShapes()[0]);
        break;
      }
      case pxc::kRecAttachShape: {
        const uint32_t v = r.u32(), s = r.u32();
        const V3 l = r.v3();
        const uint32_t f = r.u32();
        if (cloth && s < slots.size() && slots[s].act) cloth->attachVertexToShape(v, slots[s].act->getShapes()[0], nx(l), f);
        break;
      }
      case pxc::kRecWake: { const float c = r.f(); if (cloth) cloth->wakeUp(c); break; }
      case pxc::kRecSleep: if (cloth) cloth->putToSleep(); break;
      case pxc::kRecSetVel: {
        const uint32_t i = r.u32();
        const V3 v = r.v3();
        if (cloth && i < cloth->getNumberOfParticles()) cloth->setVelocity(nx(v), i);
        break;
      }
      case pxc::kRecGameVel: {  // the game's change, not the value (the value carries the recording solver's state)
        const uint32_t i = r.u32();
        r.v3();
        const V3 d = r.v3();
        if (cloth && i < cloth->getNumberOfParticles()) cloth->setVelocity(cloth->getVelocity(i) + nx(d), i);
        break;
      }
      case pxc::kRecAddVel: {  // the game's velocity write as a change (cloth_replay --to-delta)
        const uint32_t i = r.u32();
        const V3 d = r.v3();
        if (cloth && i < cloth->getNumberOfParticles()) cloth->setVelocity(cloth->getVelocity(i) + nx(d), i);
        break;
      }
      case pxc::kRecForceV: {
        const uint32_t i = r.u32();
        const V3 f = r.v3();
        const uint32_t mode = r.u32();
        r.f();
        if (cloth && i < cloth->getNumberOfParticles()) cloth->addForceAtVertex(nx(f), i, (NxForceMode)mode);
        break;
      }
      case pxc::kRecForcePos: {
        const V3 p = r.v3();
        const bool hasDir = r.u8();
        const V3 d = r.v3();
        const float mag = r.f(), rad = r.f();
        const uint32_t mode = r.u32();
        r.f();
        if (!cloth) break;
        if (hasDir) cloth->addDirectedForceAtPos(nx(p), nx(d), rad, (NxForceMode)mode);
        else cloth->addForceAtPos(nx(p), mag, rad, (NxForceMode)mode);
        fprintf(stderr, "hit at step %ld\n", step);
        break;
      }
      case pxc::kRecAttResp: { const float c = r.f(); if (cloth) cloth->setAttachmentResponseCoefficient(c); break; }
      case pxc::kRecAttTear: { const float c = r.f(); if (cloth) cloth->setAttachmentTearFactor(c); break; }
      case pxc::kRecParams: {
        const Params n = paramsFrom(r.vec<uint8_t>());
        if (cloth) {
          cloth->setThickness(n.thickness), cloth->setBendingStiffness(n.bending);
          cloth->setStretchingStiffness(n.stretching), cloth->setDampingCoefficient(n.damping);
          cloth->setFriction(n.friction), cloth->setSolverIterations(n.iters);
          cloth->setSleepLinearVelocity(n.sleepV), cloth->setExternalAcceleration(nx(n.ext));
          cloth->setWindAcceleration(nx(n.wind)), cloth->setTearFactor(n.tearFactor);
          cloth->setAttachmentResponseCoefficient(n.attResp), cloth->setAttachmentTearFactor(n.attTear);
          if (flagsOf(n) != flagsOf(P)) cloth->setFlags(flagsOf(n));
        }
        P = n;
        break;
      }
      case pxc::kRecStep: {
        const float dt = r.f();
        const V3 g = r.v3();
        if (fixedDt > 0) {
          if (curDt != fixedDt) { scene->setTiming(fixedDt, 5, NX_TIMESTEP_FIXED); curDt = fixedDt; }
        } else if (dt != curDt) {
          scene->setTiming(dt, 1, NX_TIMESTEP_FIXED);
          curDt = dt;
        }
        if (nx(g) != curG) { scene->setGravity(nx(g)); curG = nx(g); }
        scene->simulate(dt);
        scene->flushStream();
        scene->fetchResults(NX_RIGID_BODY_FINISHED, true);
        ++step;
        if (fo && step >= first && step <= last) {
          const NxU32 particles = cloth->getNumberOfParticles();
          const NxU32 nv = std::min(numV, maxV);
          cloth->getVelocities(vel.data(), 12);
          const NxU32 ntri = numI / 3;
          const NxU32 has = (lastIdx.size() != numI || memcmp(lastIdx.data(), ridx.data(), numI * 4)) ? 1 : 0;
          if (has) lastIdx.assign(ridx.begin(), ridx.begin() + numI);
          const double t = 0;
          fwrite(&nv, 4, 1, fo), fwrite(&ntri, 4, 1, fo), fwrite(&t, 8, 1, fo), fwrite(&has, 4, 1, fo);
          if (has) fwrite(ridx.data(), 4, numI, fo);
          for (NxU32 i = 0; i < nv; ++i) {
            float rec[6] = {rpos[3 * i], rpos[3 * i + 1], rpos[3 * i + 2], 0, 0, 0};
            if (i < particles) memcpy(rec + 3, &vel[3 * i], 12);
            if (rec[0] != rec[0]) nan = true;
            fwrite(rec, 4, 6, fo);
          }
          ++written;
        }
        break;
      }
      case pxc::kRecCheck: r.u32(); break;
      // the kinematic actors carry their own velocity; a removed mask comes only with a rebuild (not replayed)
      case pxc::kRecShapeVel: { float b[10]; r.raw(b, sizeof b); break; }
      case pxc::kRecRemoved: { std::vector<uint8_t> m(r.u32()); if (!m.empty()) r.raw(m.data(), m.size()); break; }
      default: printf("{\"ok\":false,\"error\":\"bad op %u\"}\n", op); return 1;
    }
  }
  if (fo) {
    fseek(fo, 8, SEEK_SET);
    fwrite(&written, 4, 1, fo);
    fclose(fo);
  }
  printf("{\"ok\":%s,\"steps\":%ld,\"written\":%u,\"slots\":%zu,\"moving\":%d,\"particles\":%u,\"nan\":%s,\"sdk_errors\":%d}\n",
         nan ? "false" : "true", step, written, slots.size(),
         (int)std::count_if(slots.begin(), slots.end(), [](const Slot& s) { return s.moves; }),
         cloth ? cloth->getNumberOfParticles() : 0, nan ? "true" : "false", out.errors);
  cooking->NxCloseCooking();
  L.close();
  return 0;
}
