// Run a fluid scenario (same format as harness/src/fluid_oracle.cpp) through our solver and write
// the same PXFD dump, so analysis/ tools compare both directly.
//   run_scenario <scenario.txt> <out.bin> [bodies.bin [impulses.txt]]
// We have no rigid-body solver, so dynbox/dynsphere/kinbox bodies follow the trajectory in an oracle bodies.bin
// (pose after each step; begin pose = previous entry; velocity = displacement / dt). Without it a body stays at its start position, as a
// kinematic shape with zero velocity. impulses.txt gets the two-way impulses: "step body jx jy jz px py pz".
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "fluid.h"
#include "scenario.h"
#include "thread_pool.h"

using namespace pxr;

int main(int argc, char** argv) {
  if (argc < 3) {
    fprintf(stderr, "usage: run_scenario <scenario.txt> <out.bin> [bodies.bin [impulses.txt]]\n");
    return 1;
  }
  Scenario sc;
  if (!loadScenario(argv[1], sc, false)) return 1;
  if (!sc.unknown.empty()) {
    fprintf(stderr, "unknown directive %s\n", sc.unknown.c_str());
    return 1;
  }
  // PXF_AGE_LATENCY=n: analytic shapes skip a new particle's first n steps (FluidParams::newParticleShapeLatency)
  if (const char* e = getenv("PXF_AGE_LATENCY")) sc.P.newParticleShapeLatency = (uint32_t)atoi(e);
  const FluidParams& P = sc.P;
  const float dt = sc.dt;
  const int steps = sc.steps;
  const Vec3 gravity = sc.gravity;
  const std::vector<StaticShape>& statics = sc.statics;
  std::vector<const StaticShape*> ptrs;
  for (const StaticShape& sh : statics) ptrs.push_back(&sh);
  const float jitter = sc.jitter;
  const unsigned jitterSeed = sc.jitterSeed;

  Fluid fluid(P);
  fluid.setCollisionGroups(sc.fluidGroup, sc.groups);
  // PXF_THREADS=n: collide on n threads (results must not change)
  static ThreadPool* pool = getenv("PXF_THREADS") ? new ThreadPool((unsigned)atoi(getenv("PXF_THREADS"))) : nullptr;
  if (pool) fluid.setParallelFor([](size_t n, void (*fn)(void*, size_t), void* ctx) { pool->run(n, fn, ctx); });
  std::vector<Vec3> ip, iv;
  for (auto& b : sc.blocks)
    for (int z = 0; z < b.n[2]; ++z)
      for (int y = 0; y < b.n[1]; ++y)
        for (int x = 0; x < b.n[0]; ++x) {
          ip.emplace_back(b.o[0] + x * b.spacing, b.o[1] + y * b.spacing, b.o[2] + z * b.spacing);
          iv.emplace_back(b.v[0], b.v[1], b.v[2]);
        }
  if (jitter > 0) {  // same LCG and order (x,y,z per particle) as harness/src/fluid_oracle.cpp
    unsigned st = jitterSeed * 2654435761u + 12345u;
    for (auto& p : ip)
      for (float* x : {&p.x, &p.y, &p.z}) {
        st = st * 1664525u + 1013904223u;
        *x += jitter * ((float)(st >> 8) * (2.0f / 16777216.0f) - 1.0f);
      }
  }
  fluid.addParticles(ip, iv);

  // Body trajectories: per step u32 step, u32 n, n x {pos[3], vel[3]} (oracle bodies.bin).
  const size_t nb = sc.bodies.size();
  std::vector<std::vector<float>> traj;  // per step, 6 floats per body
  if (argc >= 4 && nb) {
    FILE* fb = fopen(argv[3], "rb");
    if (!fb) { fprintf(stderr, "cannot open %s\n", argv[3]); return 1; }
    uint32_t h[2];
    while (fread(h, 4, 2, fb) == 2) {
      std::vector<float> rec(h[1] * 6);
      if (fread(rec.data(), 4, rec.size(), fb) != rec.size() || h[1] != nb) break;
      traj.push_back(rec);
    }
    fclose(fb);
  }
  FILE* fimp = argc >= 5 ? fopen(argv[4], "w") : nullptr;
  std::vector<DynamicShape> dyn(nb), live;  // live: the bodies that collide with fluid (the shim passes only those)
  std::vector<Vec3> bodyPos(nb);
  for (size_t b = 0; b < nb; ++b) bodyPos[b] = sc.bodies[b].p;

  FILE* fo = fopen(argv[2], "wb");
  uint32_t hdr[3] = {1, (uint32_t)steps, P.maxParticles};
  fwrite("PXFD", 1, 4, fo);
  fwrite(hdr, sizeof hdr, 1, fo);
  fwrite(&dt, 4, 1, fo);
  double total = 0;
  // Fixed substeps as the shim runs them (hosted timing): the accumulator carries the leftover time; without a timing
  // directive a frame is one step. simTime = the simulated time of the last simulate (force scale, ForceMode).
  const float mass = particleMass(P);
  float accum = 0, simTime = 0;
  for (int s = 0; s < steps; ++s) {
    auto t0 = std::chrono::steady_clock::now();
    if (std::find(sc.removeAllAt.begin(), sc.removeAllAt.end(), s) != sc.removeAllAt.end()) fluid.removeAllParticles();
    float frameDt = dt;
    for (const auto& d : sc.dtAt)
      if (d.first == s) frameDt = d.second;
    const float sub = sc.maxTimestep > 0 ? sc.maxTimestep : frameDt;
    for (const auto& fo : sc.forces)
      if (fo.step == s)
        for (size_t i = 0; i < fo.ids.size(); ++i)
          fluid.addVelocityChange(fo.ids[i], forceVelocityChange(fo.mode, fo.f[i], simTime, mass));
    for (size_t b = 0; b < nb; ++b) {
      DynamicShape& d = dyn[b];
      d.geom = &sc.bodies[b].geom;
      d.begin.t = bodyPos[b];
      if ((size_t)s < traj.size()) {
        const float* r = &traj[s][b * 6];
        // The fluid sees the rigid step's velocity, before its own two-way impulse (which the recorded velocity
        // already contains): take it from the displacement (semi-implicit Euler).
        bodyPos[b] = Vec3(r[0], r[1], r[2]);
        d.linVel = (bodyPos[b] - d.begin.t) * (1.0f / dt);
      }
      d.end.t = d.com = bodyPos[b];
      d.twoWay = sc.bodies[b].twoWayShape && !sc.bodies[b].kinematic;
      d.body = (uint32_t)b;
      d.drain = (sc.bodies[b].shapeFlags & kShapeFluidDrain) != 0;
      d.group = sc.bodies[b].group;
    }
    live.clear();
    for (size_t b = 0; b < nb; ++b)
      if (shapeCollidesWithFluid(sc.bodies[b].shapeFlags, 0, false)) live.push_back(dyn[b]);
    int nSub = 1;
    if (sc.maxTimestep > 0) {
      accum += frameDt;
      nSub = (int)std::floor(accum / sub);  // same carry rule as the shim (the original rounds differently at whole multiples)
      if (nSub > sc.maxIter) nSub = sc.maxIter;
      accum -= nSub * sub;
      if (accum > sub) accum = 0;
    }
    simTime = (nSub > 1 ? nSub : 1) * sub;
    for (int k = 0; k < nSub; ++k) fluid.step(sub, gravity, ptrs, live);
    if (fimp)
      for (const BodyImpulse& bi : fluid.bodyImpulses())
        fprintf(fimp, "%d %u %.9g %.9g %.9g %.9g %.9g %.9g\n", s, bi.body, bi.impulse.x, bi.impulse.y, bi.impulse.z,
                bi.pos.x, bi.pos.y, bi.pos.z);
    double t = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    total += t;
    uint32_t n = (uint32_t)fluid.size();
    fwrite(&n, 4, 1, fo);
    fwrite(&t, 8, 1, fo);
    for (uint32_t i = 0; i < n; ++i) {
      const Vec3 &p = fluid.positions()[i], &v = fluid.velocities()[i], &nn = fluid.normals()[i];
      float rec[11] = {p.x, p.y, p.z, v.x, v.y, v.z, fluid.densities()[i], 0.0f, nn.x, nn.y, nn.z};
      uint32_t idf[2] = {fluid.ids()[i], fluid.flags()[i]};
      fwrite(rec, sizeof rec, 1, fo);
      fwrite(idf, sizeof idf, 1, fo);
    }
  }
  fclose(fo);
  if (fimp) fclose(fimp);
  printf("{\"ok\":true,\"steps\":%d,\"particles\":%zu,\"sim_seconds_total\":%.6f}\n", steps, fluid.size(), total);
  return 0;
}
