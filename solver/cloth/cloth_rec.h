// Cloth call recording (debugging): RecCloth is a Cloth that also appends every call that changes it to a Recording,
// so a cloth from the game can be replayed natively, step for step (solver/cloth/cloth_replay.cpp). The shim keeps one
// Recording per hosted cloth across rebuilds (each rebuild records a new Create) and writes it to a .pxcr file.
// Calls that change nothing are not recorded (a motion equal to the shape's last one, a velocity equal to the current
// one), so a whole session stays small. Every 60th step records a hash of the positions, to check the replay.
//
// File: "PXCR", u32 version, then ops: u8 op, payload (little endian, floats as stored).
#pragma once
#include <cstdio>
#include <cstring>
#include <map>
#include <memory>
#include <vector>

#include "cloth.h"

namespace pxc {

enum RecOp : uint8_t {
  kRecCreate = 1, kRecMesh, kRecAddShape, kRecPin, kRecInvMass, kRecMotion, kRecDetach, kRecAttachShape,
  kRecAttachGlobal, kRecWake, kRecSleep, kRecSetVel, kRecForceV, kRecForcePos, kRecAttResp, kRecAttTear, kRecParams,
  kRecStep, kRecCheck, kRecNote,
  kRecAddVel,  // u32 vertex, V3 change: a game velocity write as the change it made (v += change); cloth_replay --to-delta
  kRecGameVel  // u32 vertex, V3 value, V3 change: a game velocity write, the value set and the game's own change (the
               // write minus what it read), so a replay with other rules can apply the change instead of the value
  ,kRecShapeVel  // u32 shape, V3 lin, V3 ang, V3 com: setShapeVelocity (recorded when it changes)
  ,kRecRemoved   // u8 vector: the removed-particle mask (NX_CLF_VALIDBOUNDS) restored into a rebuilt cloth, see setRemoved
};
constexpr uint32_t kRecVersion = 1;

struct Recording {
  std::vector<uint8_t> b;
  std::map<const TriMesh*, uint32_t> meshIds;
  std::vector<uint8_t> lastParams;
  std::vector<std::vector<uint8_t>> lastMotion;  // per shape slot of the current Cloth
  std::vector<std::vector<uint8_t>> lastVel;     // per shape slot: last setShapeVelocity
  uint64_t steps = 0;
  bool full = false;
  size_t cap = 256u << 20;  // bytes per recording, written and buffered together
  size_t flushed = 0;       // bytes already appended to the file by write()
  Recording() { put32(0x52435850u), put32(kRecVersion); }
  void raw(const void* p, size_t n) {
    if (full) return;
    if (flushed + b.size() + n > cap) { full = true; return; }
    const uint8_t* q = static_cast<const uint8_t*>(p);
    b.insert(b.end(), q, q + n);
  }
  void put8(uint8_t v) { raw(&v, 1); }
  void put32(uint32_t v) { raw(&v, 4); }
  void putf(float v) { raw(&v, 4); }
  void putv(const V3& v) { putf(v.x), putf(v.y), putf(v.z); }
  template <class T> void putVec(const std::vector<T>& v) { put32((uint32_t)v.size()); if (!v.empty()) raw(v.data(), v.size() * sizeof(T)); }
  static std::vector<uint8_t> params(const ClothParams& p) {
    Recording r;
    r.b.clear();
    r.putf(p.thickness), r.putf(p.density), r.putf(p.bending), r.putf(p.stretching), r.putf(p.damping), r.putf(p.friction);
    r.put32(p.solverIterations), r.putf(p.sleepLinearVelocity), r.putf(p.wakeUpCounter);
    r.putv(p.externalAcceleration), r.putv(p.windAcceleration);
    const bool fl[7] = {p.useGravity, p.useBending, p.bendingOrtho, p.useDamping, p.useComDamping, p.useTearing,
                        p.useSelfCollision};
    for (bool f : fl) r.put8(f);
    r.putf(p.tearFactor), r.put32(p.maxVertices), r.putf(p.attachmentResponse), r.putf(p.attachmentTearFactor);
    r.put8(p.disableDynamicCcd), r.putf(p.refDt), r.putf(p.selfContactDist);
    r.put8(p.disableCollision);  // appended later: older recordings end before it
    r.put8(p.useValidBounds);    // appended after disableCollision (same rule)
    for (int k = 0; k < 3; ++k) r.putf(p.validLo[k]);
    for (int k = 0; k < 3; ++k) r.putf(p.validHi[k]);
    r.put8(p.edgeShapeContact);  // appended after the valid bounds (same rule)
    return r.b;
  }
  void note(const char* s) { put8(kRecNote); put32((uint32_t)strlen(s)); raw(s, strlen(s)); }
  // Appends the buffered bytes to path (the first call creates it) and frees the buffer: a 32-bit game holding every
  // live cloth's whole recording in memory ran out of address space (run 20261012-164610, ~1 GB in 11 recordings).
  bool write(const char* path) {
    if (b.empty() && flushed) return true;
    FILE* f = fopen(path, flushed ? "ab" : "wb");
    if (!f) return false;
    const bool ok = fwrite(b.data(), 1, b.size(), f) == b.size();
    fclose(f);
    if (ok) flushed += b.size(), std::vector<uint8_t>().swap(b);
    return ok;
  }
};

inline uint32_t recHash(const std::vector<V3>& x) {
  uint32_t h = 2166136261u;
  for (const V3& v : x) {
    uint32_t w[3];
    memcpy(w, &v, 12);
    for (uint32_t k : w) h = (h ^ k) * 16777619u;
  }
  return h ^ (uint32_t)x.size();
}

class RecCloth : public Cloth {
 public:
  RecCloth(std::shared_ptr<Recording> r, const ClothParams& p, const std::vector<V3>& rest,
           const std::vector<uint32_t>& tri, const std::vector<V3>& x0, const std::vector<Edge>* stored = nullptr,
           bool storedRest = false)
      : Cloth(p, rest, tri, x0, stored, storedRest), r_(std::move(r)) {
    Recording& R = *r_;
    R.put8(kRecCreate);
    R.lastParams = Recording::params(p);
    R.putVec(R.lastParams);
    R.putVec(rest), R.putVec(tri), R.putVec(x0);
    R.put8(stored != nullptr);
    if (stored) R.putVec(*stored);
    R.put8(storedRest);
    R.lastMotion.clear();
    R.lastVel.clear();
  }
  void addShape(const StaticShape& s) {
    Recording& R = *r_;
    uint32_t id = 0xffffffffu;
    if (s.mesh) {
      auto it = R.meshIds.find(s.mesh);
      if (it == R.meshIds.end()) {
        id = (uint32_t)R.meshIds.size();
        R.meshIds[s.mesh] = id;
        R.put8(kRecMesh), R.put32(id), R.putVec(s.mesh->v);
      } else {
        id = it->second;
      }
    }
    R.put8(kRecAddShape), R.put32((uint32_t)s.type), R.putv(s.a), R.putf(s.d), R.putv(s.half), R.put32(id);
    Cloth::addShape(s);
  }
  void pinHard(int v, const V3& p) { r_->put8(kRecPin), r_->put32(v), r_->putv(p); Cloth::pinHard(v, p); }
  void setShapeInvMass(int s, float w) { r_->put8(kRecInvMass), r_->put32(s), r_->putf(w); Cloth::setShapeInvMass(s, w); }
  void setShapeMotion(int s, const V3& t, const float R9[9], const V3* pt = nullptr, const float* pR = nullptr,
                      bool dyn = false) {
    Recording r;
    r.b.clear();
    r.put32(s), r.putv(t), r.raw(R9, 36), r.put8(pt != nullptr);
    if (pt) r.putv(*pt);
    r.put8(pR != nullptr);
    if (pR) r.raw(pR, 36);
    r.put8(dyn);
    Recording& R = *r_;
    if ((size_t)s >= R.lastMotion.size()) R.lastMotion.resize(s + 1);
    if (R.lastMotion[s] != r.b) {
      R.lastMotion[s] = r.b;
      R.put8(kRecMotion), R.raw(r.b.data(), r.b.size());
    }
    Cloth::setShapeMotion(s, t, R9, pt, pR, dyn);
  }
  void setShapeVelocity(int s, const V3& lin, const V3& ang, const V3& com) {
    Recording r;
    r.b.clear();
    r.put32(s), r.putv(lin), r.putv(ang), r.putv(com);
    Recording& R = *r_;
    if ((size_t)s >= R.lastVel.size()) R.lastVel.resize(s + 1);
    if (R.lastVel[s] != r.b) {
      R.lastVel[s] = r.b;
      R.put8(kRecShapeVel), R.raw(r.b.data(), r.b.size());
    }
    Cloth::setShapeVelocity(s, lin, ang, com);
  }
  void detachFromShape(int s) { r_->put8(kRecDetach), r_->put32(s); Cloth::detachFromShape(s); }
  void attachVertexToShape(int v, int s, const V3& l, uint32_t f) {
    r_->put8(kRecAttachShape), r_->put32(v), r_->put32(s), r_->putv(l), r_->put32(f);
    Cloth::attachVertexToShape(v, s, l, f);
  }
  void attachVertexToGlobalPosition(int v, const V3& p) {
    r_->put8(kRecAttachGlobal), r_->put32(v), r_->putv(p);
    Cloth::attachVertexToGlobalPosition(v, p);
  }
  void setRemoved(const std::vector<uint8_t>& m) { r_->put8(kRecRemoved), r_->putVec(m); Cloth::setRemoved(m); }
  void wakeUp(float c) { r_->put8(kRecWake), r_->putf(c); Cloth::wakeUp(c); }
  void putToSleep() { r_->put8(kRecSleep); Cloth::putToSleep(); }
  void setVelocity(size_t i, const V3& v) {
    if (i < vel().size() && memcmp(&vel()[i], &v, 12) == 0) return;
    r_->put8(kRecSetVel), r_->put32((uint32_t)i), r_->putv(v);
    Cloth::setVelocity(i, v);
  }
  void setGameVelocity(size_t i, const V3& v, const V3& change) {
    if (i < vel().size() && memcmp(&vel()[i], &v, 12) == 0) return;
    r_->put8(kRecGameVel), r_->put32((uint32_t)i), r_->putv(v), r_->putv(change);
    Cloth::setVelocity(i, v);
  }
  void addForceAtVertex(size_t i, const V3& f, uint32_t mode, float dt) {
    r_->put8(kRecForceV), r_->put32((uint32_t)i), r_->putv(f), r_->put32(mode), r_->putf(dt);
    Cloth::addForceAtVertex(i, f, mode, dt);
  }
  void addForceAtPos(const V3& p, const V3* dir, float mag, float radius, uint32_t mode, float dt) {
    r_->put8(kRecForcePos), r_->putv(p), r_->put8(dir != nullptr), r_->putv(dir ? *dir : V3()), r_->putf(mag);
    r_->putf(radius), r_->put32(mode), r_->putf(dt);
    Cloth::addForceAtPos(p, dir, mag, radius, mode, dt);
  }
  void setAttachmentResponse(float c) { r_->put8(kRecAttResp), r_->putf(c); Cloth::setAttachmentResponse(c); }
  void setAttachmentTearFactor(float f) { r_->put8(kRecAttTear), r_->putf(f); Cloth::setAttachmentTearFactor(f); }
  void step(float dt, V3 g) {
    Recording& R = *r_;
    std::vector<uint8_t> p = Recording::params(params());
    if (p != R.lastParams) {
      R.lastParams = p;
      R.put8(kRecParams), R.putVec(p);
    }
    R.put8(kRecStep), R.putf(dt), R.putv(g);
    Cloth::step(dt, g);
    if (++R.steps % 60 == 0) R.put8(kRecCheck), R.put32(recHash(pos()));
  }
  Recording& recording() { return *r_; }

 private:
  std::shared_ptr<Recording> r_;
};

}  // namespace pxc
