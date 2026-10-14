// Scenario file parser shared by run_scenario (solver run) and replay_trace (API replay).
// Format: harness/src/fluid_oracle.cpp, one directive per line, '#' starts a comment.
#pragma once

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <utility>
#include <vector>

#include "fluid.h"

namespace pxr {

struct ScenarioBlock {
  int n[3];
  float spacing, o[3], v[3];
};

// dynbox / dynsphere / kinbox: geometry in the body frame (the shape sits at the body origin).
struct ScenarioBody {
  StaticShape geom{StaticShape::Box};
  Vec3 p, v;  // start position; kinbox: move velocity
  float mass = 1;
  bool kinematic = false;
  bool twoWayShape = false;  // shapeflag FLUID_TWOWAY
  uint32_t shapeFlags = 0;   // NxShapeDesc.shapeFlags (shapeflag directives)
  uint16_t group = 0;        // shapegroup
};

struct Scenario {
  FluidParams P;
  float dt = 1.0f / 60.0f;
  int steps = 120;
  Vec3 gravity{0, -9.81f, 0};
  std::vector<StaticShape> statics;  // all statics, the quad/tri mesh (with its BVH) last
  std::vector<ScenarioBlock> blocks;
  std::vector<ScenarioBody> bodies;
  std::vector<int> removeAllAt;  // "at S removeAll" (run_scenario; other "at" verbs are replay-only)
  // "at S force M id fx fy fz [id fx fy fz ...]": updateParticles with a force buffer before simulate S (run_scenario)
  struct Force {
    int step;
    ForceMode mode;
    std::vector<uint32_t> ids;
    std::vector<Vec3> f;
  };
  std::vector<Force> forces;
  std::vector<std::pair<int, float>> dtAt;  // "at S dt X": simulate(X) for step S
  float maxTimestep = 0;                    // "timing maxTimestep maxIter": fixed substeps (0 = one step of the frame dt)
  int maxIter = 1;
  uint16_t fluidGroup = 0;       // fluid.collisionGroup
  CollisionGroups groups;        // groupcollision directives
  float jitter = 0;
  unsigned jitterSeed = 1;
  std::string unknown;  // first directive the parser did not accept (parsing stops there); empty if none
};

// Returns false if the file cannot be opened. replayDirectives: also accept the directives that only the oracle
// harness uses (at, fluid.writeIds, fluid.compartment, other fluid.flag names, fluid.numReserveParticles); they are ignored,
// since a trace replay takes its API calls and FluidParams from the trace itself.
inline bool loadScenario(const char* path, Scenario& sc, bool replayDirectives) {
  FILE* f = fopen(path, "r");
  if (!f) return false;
  FluidParams& P = sc.P;
  StaticShape mesh{StaticShape::Mesh};  // all quad/tri lines form one mesh, appended after the other statics (as in the harness)
  bool mlmSet = false, cdmSet = false, lastBody = false;
  // Per static shape (parallel to statics): the flags that decide whether it collides with fluid at all. The
  // shapes that do not are dropped after the file is read (shapeCollidesWithFluid), as the shim does not pass them.
  struct StaticFlags { uint32_t shapeFlags = 0, actorFlags = 0; bool rbCompartment = false; };
  std::vector<StaticFlags> sflags;
  int lastStatic = -1;  // index of the most recent static shape (-1: none)
  auto addStatic = [&](const StaticShape& sh) {
    sc.statics.push_back(sh);
    sflags.emplace_back();
    lastStatic = (int)sc.statics.size() - 1;
    lastBody = false;
  };
  char line[512];
  while (fgets(line, sizeof line, f)) {
    if (char* h = strchr(line, '#')) *h = 0;
    char key[128], sval[64] = "";
    float a[12] = {0};
    if (sscanf(line, "%127s", key) != 1) continue;
    const char* rest = line + strspn(line, " \t") + strlen(key);
    int na = sscanf(rest, "%f %f %f %f %f %f %f %f %f %f %f %f", &a[0], &a[1], &a[2], &a[3], &a[4], &a[5], &a[6],
                    &a[7], &a[8], &a[9], &a[10], &a[11]);
    sscanf(rest, "%63s", sval);
    std::string k = key;
    auto& statics = sc.statics;
    if (k == "dt") sc.dt = a[0];
    else if (k == "steps") sc.steps = (int)a[0];
    else if (k == "gravity") sc.gravity = Vec3(a[0], a[1], a[2]);
    else if (k == "fluid.restParticlesPerMeter") P.restParticlesPerMeter = a[0];
    else if (k == "fluid.restDensity") P.restDensity = a[0];
    else if (k == "fluid.kernelRadiusMultiplier") P.kernelRadiusMultiplier = a[0];
    else if (k == "fluid.motionLimitMultiplier") { P.motionLimitMultiplier = a[0]; mlmSet = true; }
    else if (k == "fluid.collisionDistanceMultiplier") { P.collisionDistanceMultiplier = a[0]; cdmSet = true; }
    else if (k == "fluid.packetSizeMultiplier") P.packetSizeMultiplier = (uint32_t)a[0];
    else if (k == "fluid.stiffness") P.stiffness = a[0];
    else if (k == "fluid.viscosity") P.viscosity = a[0];
    else if (k == "fluid.damping") P.damping = a[0];
    else if (k == "fluid.refDt") P.refDt = a[0];  // step-rate independence (FluidParams::refDt; not an SDK field)
    else if (k == "fluid.restitutionForStaticShapes") P.restitutionStatic = a[0];
    else if (k == "fluid.dynamicFrictionForStaticShapes") P.frictionStatic = a[0];
    else if (k == "fluid.restitutionForDynamicShapes") P.restitutionDynamic = a[0];
    else if (k == "fluid.dynamicFrictionForDynamicShapes") P.frictionDynamic = a[0];
    else if (k == "fluid.collisionResponseCoefficient") P.collisionResponseCoefficient = a[0];
    else if (k == "fluid.flag" && !strcmp(sval, "COLLISION_TWOWAY")) {
      int on = 1;
      sscanf(rest, "%*s %d", &on);
      P.twoWay = on != 0;
    }
    else if (k == "fluid.staticFrictionForStaticShapes" || k == "fluid.staticFrictionForDynamicShapes" ||
             k == "fluid.attractionForStaticShapes") {}  // not in 2.8.0 (game): ignored
    else if (k == "fluid.maxParticles") P.maxParticles = (uint32_t)a[0];
    else if (k == "fluid.externalAcceleration") P.externalAcceleration = Vec3(a[0], a[1], a[2]);
    else if (k == "fluid.disableGravity") P.disableGravity = a[0] != 0;
    else if (k == "fluid.simulationMethod")
      P.method = !strcmp(sval, "none") ? SimMethod::NoInteraction : !strcmp(sval, "mixed") ? SimMethod::Mixed : SimMethod::SPH;
    else if (k == "timing" && na >= 2) {
      sc.maxTimestep = a[0];
      sc.maxIter = (int)a[1];
    } else if (!replayDirectives && k == "at" && na >= 1) {
      const int step = (int)a[0];
      char verb[32] = "";
      int used = 0;
      sscanf(rest, "%*s %31s %n", verb, &used);
      const char* args = rest + used;
      if (!strcmp(verb, "removeAll")) sc.removeAllAt.push_back(step);
      else if (!strcmp(verb, "dt")) sc.dtAt.push_back({step, (float)strtod(args, nullptr)});
      else if (!strcmp(verb, "force")) {
        Scenario::Force fo{step, ForceMode::Force, {}, {}};
        char* end = nullptr;
        fo.mode = (ForceMode)(uint32_t)strtod(args, &end);
        const char* q = end;
        for (;;) {
          const double id = strtod(q, &end);
          if (end == q) break;
          q = end;
          Vec3 v;
          v.x = (float)strtod(q, &end), q = end;
          v.y = (float)strtod(q, &end), q = end;
          v.z = (float)strtod(q, &end), q = end;
          fo.ids.push_back((uint32_t)id);
          fo.f.push_back(v);
        }
        sc.forces.push_back(fo);
      }
    }
    else if (replayDirectives && (k == "at" || k == "fluid.writeIds" || k == "fluid.compartment" ||
                                  k == "fluid.flag" || k == "fluid.numReserveParticles")) {}
    else if (k == "dynbox" && na >= 7) {
      ScenarioBody b; b.geom.half = Vec3(a[0], a[1], a[2]); b.p = Vec3(a[3], a[4], a[5]); b.mass = a[6];
      sc.bodies.push_back(b); lastBody = true;
    }
    else if (k == "dynsphere" && na >= 5) {
      ScenarioBody b; b.geom.type = StaticShape::Sphere; b.geom.r = a[0]; b.p = Vec3(a[1], a[2], a[3]); b.mass = a[4];
      sc.bodies.push_back(b); lastBody = true;
    }
    else if (k == "kinbox" && na >= 10) {
      ScenarioBody b; b.geom.half = Vec3(a[0], a[1], a[2]); b.p = Vec3(a[3], a[4], a[5]); b.mass = a[6];
      b.v = Vec3(a[7], a[8], a[9]); b.kinematic = true;
      sc.bodies.push_back(b); lastBody = true;
    }
    else if (k == "shapeflag") {  // NxShapeDesc.shapeFlags of the most recent body or static shape
      const uint32_t bit = !strcmp(sval, "FLUID_TWOWAY") ? (1u << 11)  // NX_SF_FLUID_TWOWAY
                           : !strcmp(sval, "FLUID_DRAIN") ? kShapeFluidDrain
                           : !strcmp(sval, "FLUID_DISABLE_COLLISION") ? kShapeFluidDisableCollision
                           : !strcmp(sval, "DISABLE_COLLISION") ? kShapeDisableCollision
                           : !strcmp(sval, "DISABLE_RESPONSE") ? kShapeDisableResponse
                           : !strcmp(sval, "TRIGGER_ENABLE") ? kShapeTriggerEnable : 0;
      if (!bit || (!lastBody && lastStatic < 0)) { sc.unknown = std::string("shapeflag ") + sval; break; }
      if (lastBody) {
        sc.bodies.back().shapeFlags |= bit;
        if (bit == (1u << 11)) sc.bodies.back().twoWayShape = true;
      } else {
        sflags[lastStatic].shapeFlags |= bit;  // FLUID_TWOWAY on a static is accepted and has no effect
        if (bit == kShapeFluidDrain) statics[lastStatic].drain = true;
      }
    }
    else if (k == "actorflag") {  // NxActorDesc.flags of the actor holding the most recent STATIC shape
      const uint32_t bit = !strcmp(sval, "DISABLE_COLLISION") ? kActorDisableCollision
                           : !strcmp(sval, "DISABLE_RESPONSE") ? kActorDisableResponse
                           : !strcmp(sval, "FLUID_DISABLE_COLLISION") ? kActorFluidDisableCollision : 0;
      if (!bit || lastBody || lastStatic < 0) { sc.unknown = std::string("actorflag ") + sval; break; }
      sflags[lastStatic].actorFlags |= bit;
    }
    else if (k == "rbcompartment") {  // the most recent static's actor goes in a rigid-body compartment
      if (!lastBody && lastStatic >= 0) sflags[lastStatic].rbCompartment = true;
    }
    else if (k == "shapegroup" && (lastBody || lastStatic >= 0)) {
      if (lastBody) sc.bodies.back().group = (uint16_t)a[0];
      else statics[lastStatic].group = (uint16_t)a[0];
    }
    else if (k == "shapemask" && (lastBody || lastStatic >= 0)) {}  // groupsMask bits0: no effect on fluid pairs [V]
    else if (k == "fluid.collisionGroup") sc.fluidGroup = (uint16_t)a[0];
    else if (k == "fluid.groupsMask") {}  // no effect on fluid pairs [V] (grp_mask_*)
    else if (k == "groupcollision" && na >= 3) sc.groups.set((uint32_t)a[0], (uint32_t)a[1], a[2] != 0);
    else if (k == "plane" && na >= 4) { lastBody = false; StaticShape s{StaticShape::Plane}; s.n = Vec3(a[0], a[1], a[2]); s.d = a[3]; addStatic(s); }
    else if (k == "box" && na >= 6) { lastBody = false; StaticShape s{StaticShape::Box}; s.half = Vec3(a[0], a[1], a[2]); s.c = Vec3(a[3], a[4], a[5]); addStatic(s); }
    else if (k == "rotbox" && na >= 10) {  // static box rotated by deg degrees about an axis (NxQuat::fromAngleAxis)
      lastBody = false;
      StaticShape s{StaticShape::Box};
      s.half = Vec3(a[0], a[1], a[2]);
      s.oriented = true;
      const float l = std::sqrt(a[6] * a[6] + a[7] * a[7] + a[8] * a[8]), h = a[9] * 0.5f * 3.14159265358979f / 180.0f;
      const float x = a[6] / l * std::sin(h), y = a[7] / l * std::sin(h), z = a[8] / l * std::sin(h), w = std::cos(h);
      float (*m)[3] = s.frame.m;
      m[0][0] = 1 - 2 * (y * y + z * z), m[0][1] = 2 * (x * y - w * z), m[0][2] = 2 * (x * z + w * y);
      m[1][0] = 2 * (x * y + w * z), m[1][1] = 1 - 2 * (x * x + z * z), m[1][2] = 2 * (y * z - w * x);
      m[2][0] = 2 * (x * z - w * y), m[2][1] = 2 * (y * z + w * x), m[2][2] = 1 - 2 * (x * x + y * y);
      s.frame.t = Vec3(a[3], a[4], a[5]);
      addStatic(s);
    }
    else if (k == "sphere" && na >= 4) { lastBody = false; StaticShape s{StaticShape::Sphere}; s.r = a[0]; s.c = Vec3(a[1], a[2], a[3]); addStatic(s); }
    else if (k == "capsule" && na >= 5) { lastBody = false; StaticShape s{StaticShape::Capsule}; s.r = a[0]; s.h = a[1]; s.c = Vec3(a[2], a[3], a[4]); addStatic(s); }
    else if (k == "quad" && na >= 5) {  // same corners and order as harness/src/fluid_oracle.cpp: (b-a)x(c-a) = +Y
      const float sx = a[0], sz = a[1], px = a[2], py = a[3], pz = a[4];
      const Vec3 v[6] = {{-sx, 0, -sz}, {-sx, 0, sz}, {sx, 0, -sz}, {sx, 0, -sz}, {-sx, 0, sz}, {sx, 0, sz}};
      for (int i = 0; i < 6; i += 3) {
        Vec3 o(px, py, pz);
        mesh.tris.push_back({v[i] + o, v[i + 1] + o, v[i + 2] + o});
      }
    }
    else if (k == "quadgrid" && na >= 7) {  // nx nz sx sz px py pz: nx*nz quads (2 tris each, +Y) over [-sx,sx]x[-sz,sz]
      const int nx = (int)a[0], nz = (int)a[1];
      const float sx = a[2], sz = a[3];
      const Vec3 o(a[4], a[5], a[6]);
      for (int i = 0; i < nx; ++i)
        for (int j = 0; j < nz; ++j) {
          const float x0 = -sx + 2 * sx * i / nx, x1 = -sx + 2 * sx * (i + 1) / nx;
          const float z0 = -sz + 2 * sz * j / nz, z1 = -sz + 2 * sz * (j + 1) / nz;
          // same winding as quad: (b-a)x(c-a) = +Y
          mesh.tris.push_back({Vec3(x0, 0, z0) + o, Vec3(x0, 0, z1) + o, Vec3(x1, 0, z0) + o});
          mesh.tris.push_back({Vec3(x1, 0, z0) + o, Vec3(x0, 0, z1) + o, Vec3(x1, 0, z1) + o});
        }
    }
    else if (k == "tri" && na >= 9) mesh.tris.push_back({Vec3(a[0], a[1], a[2]), Vec3(a[3], a[4], a[5]), Vec3(a[6], a[7], a[8])});
    else if (k == "jitter" && na >= 1) { sc.jitter = a[0]; sc.jitterSeed = na >= 2 ? (unsigned)a[1] : 1u; }
    else if (k == "block" && na >= 10) sc.blocks.push_back({{(int)a[0], (int)a[1], (int)a[2]}, a[3], {a[4], a[5], a[6]}, {a[7], a[8], a[9]}});
    else {
      sc.unknown = key;
      break;
    }
  }
  fclose(f);
  if (!sc.unknown.empty()) return true;
  {  // drop the static shapes that never collide with fluid (the shim does not pass them either)
    size_t w = 0;
    for (size_t i = 0; i < sc.statics.size(); ++i) {
      const StaticFlags& fl = sflags[i];
      if (!shapeCollidesWithFluid(fl.shapeFlags, fl.actorFlags, fl.rbCompartment)) continue;
      if (w != i) sc.statics[w] = std::move(sc.statics[i]);
      ++w;
    }
    sc.statics.resize(w);
  }
  if (!mesh.tris.empty()) {
    buildMeshBVH(mesh);
    sc.statics.push_back(mesh);
  }
  // NxFluidDesc defaults derive these from kernelRadiusMultiplier when not given explicitly.
  if (!mlmSet) P.motionLimitMultiplier = 3.0f * P.kernelRadiusMultiplier;
  if (!cdmSet) P.collisionDistanceMultiplier = 0.1f * P.kernelRadiusMultiplier;
  return true;
}

}  // namespace pxr
