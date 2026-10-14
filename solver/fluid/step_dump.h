// One fluid step's inputs (parameters, particles, static and dynamic shapes) as a flat binary file, written by the
// shim when a hosted step is slow and replayed offline by step_bench. Every field is written as 4 bytes, so the
// 32-bit shim and the 64-bit tools agree. Ids, contact caches and two-way debts are not kept (profiling only).
#pragma once

#include <cstdio>
#include <cstring>
#include <vector>

#include "fluid.h"

namespace pxr {

struct StepDump {
  FluidParams params;
  float dt = 0;
  Vec3 gravity;
  std::vector<Vec3> pos, vel;
  std::vector<float> life;
  std::vector<StaticShape> statics;
  std::vector<DynamicShape> dynamics;
  std::vector<StaticShape> dynGeoms;  // dynamics[i].geom points here after reading
  float stepMs = 0;                   // the step's cost in the game, for reference
};

namespace dumpio {
inline void w32(FILE* f, const void* p) { fwrite(p, 4, 1, f); }
inline void wu(FILE* f, uint32_t v) { w32(f, &v); }
inline void wf(FILE* f, float v) { w32(f, &v); }
inline void wv(FILE* f, const Vec3& v) { wf(f, v.x), wf(f, v.y), wf(f, v.z); }
inline void wpose(FILE* f, const Pose& p) {
  for (int i = 0; i < 3; ++i)
    for (int j = 0; j < 3; ++j) wf(f, p.m[i][j]);
  wv(f, p.t);
}
inline void wshape(FILE* f, const StaticShape& s) {
  wu(f, (uint32_t)s.type), wv(f, s.n), wf(f, s.d), wv(f, s.c), wv(f, s.half), wf(f, s.r), wf(f, s.h);
  wu(f, s.oriented), wpose(f, s.frame);  // version 2
  wu(f, (uint32_t)s.tris.size());
  for (const MeshTri& t : s.tris) wv(f, t.v0), wv(f, t.v1), wv(f, t.v2);
}
struct R {
  FILE* f;
  bool ok = true;
  uint32_t version = 0;
  uint32_t u() { uint32_t v = 0; ok = ok && fread(&v, 4, 1, f) == 1; return v; }
  float fl() { float v = 0; ok = ok && fread(&v, 4, 1, f) == 1; return v; }
  Vec3 v() { Vec3 r; r.x = fl(), r.y = fl(), r.z = fl(); return r; }
  Pose pose() {
    Pose p;
    for (int i = 0; i < 3; ++i)
      for (int j = 0; j < 3; ++j) p.m[i][j] = fl();
    p.t = v();
    return p;
  }
  StaticShape shape() {
    StaticShape s{(StaticShape::Type)u()};
    s.n = v(), s.d = fl(), s.c = v(), s.half = v(), s.r = fl(), s.h = fl();
    if (version >= 2) s.oriented = u() != 0, s.frame = pose();
    const uint32_t n = u();
    if (!ok || n > (1u << 24)) { ok = false; return s; }
    s.tris.resize(n);
    for (MeshTri& t : s.tris) t.v0 = v(), t.v1 = v(), t.v2 = v();
    return s;
  }
};
}  // namespace dumpio

// Writes the inputs of the step about to run: the particles as they are now, the shapes it will get.
inline bool writeStepDump(const char* path, const Fluid& fl, const FluidParams& P, float dt, const Vec3& g,
                          const std::vector<const StaticShape*>& statics, const std::vector<DynamicShape>& dynamics,
                          float stepMs) {
  using namespace dumpio;
  FILE* f = fopen(path, "wb");
  if (!f) return false;
  wu(f, 0x44535850), wu(f, 2);  // "PXSD", version (2: shapes carry oriented + frame)
  wf(f, stepMs), wf(f, dt), wv(f, g);
  wu(f, P.maxParticles), wf(f, P.restParticlesPerMeter), wf(f, P.restDensity), wf(f, P.kernelRadiusMultiplier);
  wf(f, P.motionLimitMultiplier), wf(f, P.collisionDistanceMultiplier), wu(f, P.packetSizeMultiplier);
  wf(f, P.stiffness), wf(f, P.viscosity), wf(f, P.damping), wv(f, P.externalAcceleration);
  wf(f, P.restitutionStatic), wf(f, P.frictionStatic), wf(f, P.restitutionDynamic), wf(f, P.frictionDynamic);
  wu(f, (uint32_t)P.method), wu(f, P.disableGravity), wu(f, P.numReserveParticles), wu(f, P.priorityMode);
  wu(f, P.twoWay), wf(f, P.collisionResponseCoefficient);
  const uint32_t n = (uint32_t)fl.size();
  wu(f, n);
  for (uint32_t i = 0; i < n; ++i) wv(f, fl.positions()[i]), wv(f, fl.velocities()[i]), wf(f, fl.lifetimes()[i]);
  wu(f, (uint32_t)statics.size());
  for (const StaticShape* s : statics) wshape(f, *s);
  wu(f, (uint32_t)dynamics.size());
  for (const DynamicShape& d : dynamics) {
    wshape(f, *d.geom), wpose(f, d.begin), wpose(f, d.end), wv(f, d.linVel), wv(f, d.angVel), wv(f, d.com);
    wu(f, d.twoWay), wu(f, d.body);
  }
  fclose(f);
  return true;
}

inline bool readStepDump(const char* path, StepDump& D) {
  using namespace dumpio;
  FILE* f = fopen(path, "rb");
  if (!f) return false;
  R r{f};
  if (r.u() != 0x44535850) { fclose(f); return false; }
  r.version = r.u();
  if (r.version < 1 || r.version > 2) { fclose(f); return false; }
  D.stepMs = r.fl(), D.dt = r.fl(), D.gravity = r.v();
  FluidParams& P = D.params;
  P.maxParticles = r.u(), P.restParticlesPerMeter = r.fl(), P.restDensity = r.fl(), P.kernelRadiusMultiplier = r.fl();
  P.motionLimitMultiplier = r.fl(), P.collisionDistanceMultiplier = r.fl(), P.packetSizeMultiplier = r.u();
  P.stiffness = r.fl(), P.viscosity = r.fl(), P.damping = r.fl(), P.externalAcceleration = r.v();
  P.restitutionStatic = r.fl(), P.frictionStatic = r.fl(), P.restitutionDynamic = r.fl(), P.frictionDynamic = r.fl();
  P.method = (SimMethod)r.u(), P.disableGravity = r.u() != 0, P.numReserveParticles = r.u(), P.priorityMode = r.u() != 0;
  P.twoWay = r.u() != 0, P.collisionResponseCoefficient = r.fl();
  const uint32_t n = r.u();
  for (uint32_t i = 0; i < n && r.ok; ++i) D.pos.push_back(r.v()), D.vel.push_back(r.v()), D.life.push_back(r.fl());
  const uint32_t ns = r.u();
  for (uint32_t i = 0; i < ns && r.ok; ++i) D.statics.push_back(r.shape());
  const uint32_t nd = r.u();
  D.dynGeoms.reserve(nd);
  for (uint32_t i = 0; i < nd && r.ok; ++i) {
    D.dynGeoms.push_back(r.shape());
    DynamicShape d;
    d.begin = r.pose(), d.end = r.pose(), d.linVel = r.v(), d.angVel = r.v(), d.com = r.v();
    d.twoWay = r.u() != 0, d.body = r.u();
    D.dynamics.push_back(d);
  }
  for (size_t i = 0; i < D.dynamics.size(); ++i) D.dynamics[i].geom = &D.dynGeoms[i];
  fclose(f);
  return r.ok;
}

}  // namespace pxr
