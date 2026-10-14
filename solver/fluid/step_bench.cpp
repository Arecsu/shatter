// Replays one dumped fluid step (step_dump.h, written by the shim for a slow hosted step) and times it.
//   step_bench <dump.pxsd> [repeats [out.bin]]   (out.bin: particles after one step, to compare two builds)
// Rebuilds the mesh midphases, then runs the step `repeats` times from the same inputs and prints the cost per phase
// as the solver sees it (whole step, and the step with no shapes for the non-collision part).
#include <chrono>
#include <cstdio>
#include <cstdlib>

#include "fluid.h"
#include "step_dump.h"

using namespace pxr;

namespace {
double timeSteps(const StepDump& D, const std::vector<const StaticShape*>& st, const std::vector<DynamicShape>& dyn,
                 int reps, size_t* left) {
  double best = 1e30;
  for (int r = 0; r < reps; ++r) {
    Fluid fl(D.params);
    fl.addParticles(D.pos.data(), D.vel.data(), D.life.data(), (uint32_t)D.pos.size(), nullptr);
    const auto t0 = std::chrono::steady_clock::now();
    fl.step(D.dt, D.gravity, st, dyn);
    const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    if (ms < best) best = ms;
    *left = fl.size();
  }
  return best;
}
}  // namespace

int main(int argc, char** argv) {
  if (argc < 2) {
    fprintf(stderr, "usage: step_bench <dump.pxsd> [repeats]\n");
    return 2;
  }
  StepDump D;
  if (!readStepDump(argv[1], D)) {
    fprintf(stderr, "cannot read %s\n", argv[1]);
    return 1;
  }
  const int reps = argc > 2 ? atoi(argv[2]) : 5;
  size_t tris = 0, types[5] = {0};
  for (StaticShape& s : D.statics) {
    if (s.type == StaticShape::Mesh) buildMeshBVH(s), tris += s.tris.size();
    if ((unsigned)s.type < 5) ++types[s.type];
  }
  for (StaticShape& s : D.dynGeoms)
    if (s.type == StaticShape::Mesh) buildMeshBVH(s);
  std::vector<const StaticShape*> st;
  for (const StaticShape& s : D.statics) st.push_back(&s);
  printf("dump: %zu particles, dt %g, %zu statics (plane %zu sphere %zu box %zu capsule %zu mesh %zu, %zu tris), "
         "%zu dynamics; game step %.2f ms\n", D.pos.size(), (double)D.dt, D.statics.size(), types[0], types[1],
         types[2], types[3], types[4], tris, D.dynamics.size(), (double)D.stepMs);
  printf("params: restPPM %g KRM %g MLM %g CDM %g method %d maxParticles %u reserve %u priority %d\n",
         (double)D.params.restParticlesPerMeter, (double)D.params.kernelRadiusMultiplier,
         (double)D.params.motionLimitMultiplier, (double)D.params.collisionDistanceMultiplier, (int)D.params.method,
         D.params.maxParticles, D.params.numReserveParticles, (int)D.params.priorityMode);
  size_t left = 0;
  const double all = timeSteps(D, st, D.dynamics, reps, &left);
  printf("step, all shapes:     %8.3f ms (%zu particles after)\n", all, left);
  const double stOnly = timeSteps(D, st, {}, reps, &left);
  printf("step, statics only:   %8.3f ms\n", stOnly);
  const double none = timeSteps(D, {}, {}, reps, &left);
  printf("step, no shapes:      %8.3f ms\n", none);
  if (argc > 3) {  // write the particles after one step with all shapes (pos, vel, flags), to compare builds
    Fluid fl(D.params);
    fl.addParticles(D.pos.data(), D.vel.data(), D.life.data(), (uint32_t)D.pos.size(), nullptr);
    fl.step(D.dt, D.gravity, st, D.dynamics);
    FILE* f = fopen(argv[3], "wb");
    for (size_t i = 0; i < fl.size(); ++i) {
      fwrite(&fl.positions()[i], 12, 1, f), fwrite(&fl.velocities()[i], 12, 1, f), fwrite(&fl.flags()[i], 4, 1, f);
    }
    fclose(f);
  }
  return 0;
}
