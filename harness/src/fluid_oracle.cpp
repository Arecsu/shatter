// Fluid oracle: run a scenario through the original PhysX core and dump every particle each step.
//
//   fluid_oracle.exe <scenario.txt> <out.bin> [--loader PhysXLoader.dll]
//
// Scenario file: one directive per line, '#' comments. All units are PhysX units (metres).
//   dt 0.0166667            steps 120            gravity 0 -9.81 0
//   fluid.<field> <value>   (restParticlesPerMeter, restDensity, kernelRadiusMultiplier,
//                            motionLimitMultiplier, collisionDistanceMultiplier, packetSizeMultiplier,
//                            stiffness, viscosity, damping, restitutionForStaticShapes,
//                            dynamicFrictionForStaticShapes, restitutionForDynamicShapes,
//                            dynamicFrictionForDynamicShapes, collisionResponseCoefficient,
//                            maxParticles, simulationMethod (sph|none|mixed), disableGravity (0|1),
//                            externalAcceleration x y z)
//   fluid.flag <NAME> [0|1]             NxFluidDesc.flags bit: COLLISION_TWOWAY, DISABLE_GRAVITY (default 1)
//   fluid.collisionMethod static|dynamic|both   (default both = NX_F_STATIC|NX_F_DYNAMIC)
//   plane nx ny nz d                    static plane  n.x = d
//   box hx hy hz  px py pz              static box (half extents, position)
//   rotbox hx hy hz px py pz ax ay az deg  static box rotated by deg degrees about the axis (ax, ay, az)
//   hfield rows cols rowScale colScale heightScale px py pz  static heightfield (NxHeightFieldShape, S16_TM samples)
//                                       at shape position p. Procedural samples: h(r,c) = round(10 sin(0.5 r) + 7 cos(0.4 c)
//                                       + 2 r), tessFlag = (r+c) & 1, material 0. Shape space: row -> x, column -> z,
//                                       height -> y. Following hfhole lines cut holes into this heightfield.
//   hfhole r c                          hole in the cell with upper-left sample (r, c) (holeMaterial 127)
//   sphere r  px py pz                  static sphere
//   capsule r h  px py pz               static capsule (along y, h = height of cylinder part)
//   dynbox hx hy hz px py pz mass       dynamic actor (body) with a box shape, mass in kg, centre at p
//   dynsphere r px py pz mass           dynamic actor with a sphere shape
//   kinbox hx hy hz px py pz mass vx vy vz  kinematic box (NX_BF_KINEMATIC), moved each step by
//                                       moveGlobalPosition(p + v * (step+1) * dt)
//   shapeflag <NAME>                    adds a NxShapeDesc flag to the most recent box/sphere/capsule/plane/dyn
//                                       shape: FLUID_TWOWAY (NX_SF_FLUID_TWOWAY), FLUID_DISABLE_COLLISION, FLUID_DRAIN
//                                       (dynamic shapes have no two-way flag unless set here)
//                                       also DISABLE_COLLISION, DISABLE_RESPONSE, TRIGGER_ENABLE (shapeFlags bits)
//   actorflag <NAME>                    NxActorDesc.flags of the actor holding the most recent static shape:
//                                       DISABLE_COLLISION, DISABLE_RESPONSE, FLUID_DISABLE_COLLISION
//   rbcompartment                       the most recent static shape's actor goes in an NX_SCT_RIGIDBODY compartment
//   shapegroup G | shapemask bits0      NxShapeDesc.group / groupsMask.bits0 of the most recent shape
//   groupcollision a b 0|1              scene->setGroupCollisionFlag (before the statics are created)
//   fluid.collisionGroup G | fluid.groupsMask bits0   NxFluidDesc collision group / groups mask (bits0)
//   quad sx sz  px py pz                static triangle mesh: flat square (2 triangles) in XZ, half sizes sx,sz,
//                                       at p; face normal (b-a)x(c-a) = +Y (NX_MF_FLIPNORMALS not set)
//   tri  x0 y0 z0  x1 y1 z1  x2 y2 z2   one world-space triangle. All quad/tri lines form ONE static mesh
//                                       actor (cooked with NxCookTriangleMesh, NxTriangleMeshShapeDesc)
//   block nx ny nz spacing  ox oy oz  vx vy vz   particles on a lattice, added at step 0
//   jitter amp [seed]                   uniform ±amp on initial positions (LCG, same as solver runner)
// Game-style API use (what Mirror's Edge does, docs/shim-game-run.md):
//   fluid.numReserveParticles n         (with fluid.flag PRIORITY_MODE)
//   fluid.flag PRIORITY_MODE|PROJECT_TO_PLANE|ENABLED [0|1]
//   fluid.compartment 1                 create the fluid in an NX_SCT_FLUID compartment (CPU device)
//   fluid.copies N                      N - 1 identical extra fluids in the scene (output: the first only)
//   fluid.writeIds 1                    also pass creation/deletion id write buffers and packet data (as the game);
//                                       their contents are only visible in the shim trace (PXSHIM_TRACE=2)
//   at S add nx ny nz spacing ox oy oz vx vy vz [life]   addParticles before simulate number S (0-based);
//                                       with life, a life buffer (all particles get that lifetime)
//   at S delete id [id ...]             updateParticles before simulate S: flag NX_FP_DELETE for those ids
//                                       (interleaved id/flag buffer, stride 8, like the game)
//   at S deleteEvery k [r]              same, for every current particle whose id % k == r (default r 0)
//   at S removeAll                      removeAllParticles before simulate S
//   at S force M id fx fy fz [id fx fy fz ...]   updateParticles before simulate S with force buffer (no flags, id
//                                       based): M = NxForceMode 0..5 (0 FORCE, 1 IMPULSE, 2 VELOCITY_CHANGE,
//                                       3 SMOOTH_IMPULSE, 4 SMOOTH_VELOCITY_CHANGE, 5 ACCELERATION). Several lines per step add up.
//   at S dt X                           simulate(X) for step S instead of the frame dt (e.g. to see which dt a force uses)
//   timing maxTimestep maxIter          setTiming(maxTimestep, maxIter, FIXED); default: the frame dt, 1 substep.
//                                       With it, simulate(dt) runs dt / maxTimestep substeps (at most maxIter).
//
// Output (little endian): "PXFD" u32 version=1, u32 steps, u32 maxParticles, f32 dt, then per step:
//   u32 count, f64 stepSeconds, count × { f32 pos[3], vel[3], density, life, normal[3]; u32 id, flags }
// bodies.bin (written next to out.bin, no header): per step (0-based, same order as out.bin):
//   u32 step, u32 nbodies, nbodies × { f32 pos[3] (getGlobalPosition), vel[3] (getLinearVelocity) }
//   bodies are the dynbox/dynsphere/kinbox lines in scenario order (nbodies = 0 when there are none).
// A JSON summary line goes to stdout (includes particle_mass and collision_response_coefficient as
// reported by the SDK after createFluid).

#include <cmath>
#include <vector>
#include <string>
#include <map>
#include <algorithm>

#include "oracle_common.h"
#include "NxStream.h"
#include "NxCooking.h"

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

struct Block {
  int n[3];
  float spacing, o[3], v[3];
};

// Dynamic or kinematic actor (dynbox / dynsphere / kinbox). Shape is owned.
struct DynBody {
  NxShapeDesc* shape;
  NxVec3 p, v;  // start position (actor global pose), start linear velocity (kinbox: move velocity)
  float mass;
  bool kinematic;
};

// A scripted API call before simulate number `step` (the `at` directive).
// hfield / hfhole lines: a heightfield made from the procedural samples in the header, with its holes.
struct HfieldSpec {
  int rows = 0, cols = 0;
  float rowScale = 1, colScale = 1, heightScale = 1, px = 0, py = 0, pz = 0;
  std::vector<std::pair<int, int>> holes;  // upper-left sample (row, col) of each hole cell
};

struct Event {
  int step;
  enum Kind { Add, Delete, DeleteEvery, RemoveAll, Force, SetDt } kind;
  float dtValue = 0;        // SetDt
  Block block;              // Add
  float life = 0;           // Add: > 0 passes a life buffer
  std::vector<NxU32> ids;   // Delete
  NxU32 k = 1, r = 0;       // DeleteEvery
  NxU32 mode = 0;           // Force: NxForceMode
  std::vector<NxU32> forceIds;  // Force: id per entry, forceVec[3 * i .. 3 * i + 2] its force
  std::vector<float> forceVec;
};

struct Scenario {
  std::vector<Event> events;
  bool compartment = false;
  bool writeIds = false;
  int copies = 1;  // fluid.copies: identical extra fluids (not written), to exercise several fluids per scene
  float dt = 1.0f / 60.0f;
  float maxTimestep = 0;  // timing directive; 0 = dt
  int maxIter = 1;
  int steps = 120;
  NxVec3 gravity{0, -9.81f, 0};
  NxFluidDesc fd;
  std::vector<NxShapeDesc*> statics;  // owned
  std::vector<DynBody> bodies;        // owned shapes
  NxShapeDesc* lastShape = nullptr;   // target of the shapeflag directive
  std::map<NxShapeDesc*, NxU32> staticActorFlags;  // actorflag directive: NxActorDesc.flags of a static actor
  struct GroupFlag { NxU32 a, b; bool on; };
  std::vector<GroupFlag> groupFlags;               // groupcollision directive: scene->setGroupCollisionFlag
  std::map<NxShapeDesc*, bool> rbCompartment;      // rbcompartment directive: that static's actor in an NX_SCT_RIGIDBODY compartment
  std::vector<float> meshTris;        // 9 floats per world-space triangle (quad/tri lines), one shared mesh
  std::vector<HfieldSpec> hfields;    // hfield lines (heightfield shapes, created after the SDK is loaded)
  std::vector<Block> blocks;
  float jitter = 0;  // uniform ±jitter added to initial positions (deterministic LCG)
  unsigned jitterSeed = 1;

  void addStatic(NxShapeDesc* sh) {
    statics.push_back(sh);
    lastShape = sh;
  }
};

bool parse(const char* path, Scenario& s) {
  FILE* f = fopen(path, "r");
  if (!f) return false;
  s.fd.setToDefault();
  s.fd.flags &= ~NX_FF_HARDWARE;
  char line[512];
  int lineNo = 0;
  while (fgets(line, sizeof(line), f)) {
    lineNo++;
    char* hash = strchr(line, '#');
    if (hash) *hash = 0;
    char key[128];
    float a[12] = {0};
    char sval[64] = "";
    if (sscanf(line, "%127s", key) != 1) continue;
    const char* rest = line + strspn(line, " \t") + strlen(key);
    int na = sscanf(rest, "%f %f %f %f %f %f %f %f %f %f %f %f", &a[0], &a[1], &a[2], &a[3], &a[4], &a[5],
                    &a[6], &a[7], &a[8], &a[9], &a[10], &a[11]);
    sscanf(rest, "%63s", sval);
    std::string k = key;
    NxFluidDesc& d = s.fd;
    if (k == "dt") s.dt = a[0];
    else if (k == "timing" && na >= 2) s.maxTimestep = a[0], s.maxIter = (int)a[1];
    else if (k == "steps") s.steps = (int)a[0];
    else if (k == "gravity") s.gravity = NxVec3(a[0], a[1], a[2]);
    else if (k == "fluid.restParticlesPerMeter") d.restParticlesPerMeter = a[0];
    else if (k == "fluid.restDensity") d.restDensity = a[0];
    else if (k == "fluid.kernelRadiusMultiplier") d.kernelRadiusMultiplier = a[0];
    else if (k == "fluid.motionLimitMultiplier") d.motionLimitMultiplier = a[0];
    else if (k == "fluid.collisionDistanceMultiplier") d.collisionDistanceMultiplier = a[0];
    else if (k == "fluid.packetSizeMultiplier") d.packetSizeMultiplier = (NxU32)a[0];
    else if (k == "fluid.stiffness") d.stiffness = a[0];
    else if (k == "fluid.viscosity") d.viscosity = a[0];
    else if (k == "fluid.damping") d.damping = a[0];
    else if (k == "fluid.restitutionForStaticShapes") d.restitutionForStaticShapes = a[0];
    else if (k == "fluid.dynamicFrictionForStaticShapes") d.dynamicFrictionForStaticShapes = a[0];
    else if (k == "fluid.staticFrictionForStaticShapes") d.staticFrictionForStaticShapes = a[0];
    else if (k == "fluid.staticFrictionForDynamicShapes") d.staticFrictionForDynamicShapes = a[0];
    else if (k == "fluid.attractionForStaticShapes") d.attractionForStaticShapes = a[0];
    else if (k == "fluid.restitutionForDynamicShapes") d.restitutionForDynamicShapes = a[0];
    else if (k == "fluid.dynamicFrictionForDynamicShapes") d.dynamicFrictionForDynamicShapes = a[0];
    else if (k == "fluid.collisionResponseCoefficient") d.collisionResponseCoefficient = a[0];
    else if (k == "fluid.flag") {
      // Name is the first word; optional 0|1 after it (default 1).
      float on = 1.0f;  // a[] was parsed from the name, so re-read the value after the name
      sscanf(rest + strspn(rest, " \t") + strlen(sval), "%f", &on);
      NxU32 bit = !strcmp(sval, "COLLISION_TWOWAY") ? NX_FF_COLLISION_TWOWAY
                  : !strcmp(sval, "DISABLE_GRAVITY") ? NX_FF_DISABLE_GRAVITY
                  : !strcmp(sval, "PRIORITY_MODE") ? NX_FF_PRIORITY_MODE
                  : !strcmp(sval, "PROJECT_TO_PLANE") ? NX_FF_PROJECT_TO_PLANE
                  : !strcmp(sval, "ENABLED") ? NX_FF_ENABLED : 0;
      if (!bit) {
        fprintf(stderr, "%s:%d: unknown fluid flag '%s'\n", path, lineNo, sval);
        fclose(f);
        return false;
      }
      if (on != 0) d.flags |= bit;
      else d.flags &= ~bit;
    } else if (k == "fluid.collisionMethod") {
      d.collisionMethod = !strcmp(sval, "static") ? NX_F_STATIC
                          : !strcmp(sval, "dynamic") ? NX_F_DYNAMIC : (NX_F_STATIC | NX_F_DYNAMIC);
    } else if (k == "fluid.maxParticles") d.maxParticles = (NxU32)a[0];
    else if (k == "fluid.numReserveParticles") d.numReserveParticles = (NxU32)a[0];
    else if (k == "fluid.compartment") s.compartment = a[0] != 0;
    else if (k == "fluid.writeIds") s.writeIds = a[0] != 0;
    else if (k == "fluid.copies") s.copies = (int)a[0];
    else if (k == "rbcompartment" && s.lastShape) s.rbCompartment[s.lastShape] = true;
    else if (k == "fluid.collisionGroup") s.fd.collisionGroup = (NxCollisionGroup)(NxU32)a[0];  // group probes
    else if (k == "fluid.groupsMask") s.fd.groupsMask.bits0 = (NxU32)a[0];
    else if (k == "groupcollision" && na >= 3) s.groupFlags.push_back({(NxU32)a[0], (NxU32)a[1], a[2] != 0});
    else if (k == "shapegroup" && s.lastShape) s.lastShape->group = (NxCollisionGroup)(NxU32)a[0];
    else if (k == "shapemask" && s.lastShape) s.lastShape->groupsMask.bits0 = (NxU32)a[0];
    else if (k == "at" && na >= 1) {
      // "at S verb args...": a[] holds S only (the verb stops the float scan), so re-scan after the verb.
      Event e;
      e.step = (int)a[0];
      char verb[32] = "";
      int used = 0;
      sscanf(rest, "%*s %31s %n", verb, &used);
      const char* args = rest + used;
      float b[12] = {0};
      int nb = sscanf(args, "%f %f %f %f %f %f %f %f %f %f %f", &b[0], &b[1], &b[2], &b[3], &b[4], &b[5], &b[6],
                      &b[7], &b[8], &b[9], &b[10]);
      if (!strcmp(verb, "add") && nb >= 10) {
        e.kind = Event::Add;
        e.block = {{(int)b[0], (int)b[1], (int)b[2]}, b[3], {b[4], b[5], b[6]}, {b[7], b[8], b[9]}};
        e.life = nb >= 11 ? b[10] : 0;
      } else if (!strcmp(verb, "delete") && nb >= 1) {
        e.kind = Event::Delete;
        for (int i = 0; i < nb; ++i) e.ids.push_back((NxU32)b[i]);
      } else if (!strcmp(verb, "deleteEvery") && nb >= 1) {
        e.kind = Event::DeleteEvery;
        e.k = (NxU32)b[0];
        e.r = nb >= 2 ? (NxU32)b[1] : 0;
      } else if (!strcmp(verb, "removeAll")) {
        e.kind = Event::RemoveAll;
      } else if (!strcmp(verb, "dt") && nb >= 0) {
        e.kind = Event::SetDt;
        e.dtValue = (float)strtod(args, nullptr);
      } else if (!strcmp(verb, "force")) {
        // Mode, then id fx fy fz groups (all numbers; the ids are read as floats, ids are small).
        e.kind = Event::Force;
        const char* q = args;
        char* end = nullptr;
        e.mode = (NxU32)strtod(q, &end);
        q = end;
        for (;;) {
          const double id = strtod(q, &end);
          if (end == q) break;
          q = end;
          e.forceIds.push_back((NxU32)id);
          for (int c = 0; c < 3; ++c) {
            e.forceVec.push_back((float)strtod(q, &end));
            q = end;
          }
        }
      } else {
        fprintf(stderr, "%s:%d: malformed 'at' directive\n", path, lineNo);
        fclose(f);
        return false;
      }
      s.events.push_back(e);
    }
    else if (k == "fluid.externalAcceleration") d.externalAcceleration = NxVec3(a[0], a[1], a[2]);
    else if (k == "fluid.disableGravity") {
      if (a[0] != 0) d.flags |= NX_FF_DISABLE_GRAVITY;
      else d.flags &= ~NX_FF_DISABLE_GRAVITY;
    } else if (k == "fluid.simulationMethod") {
      d.simulationMethod = !strcmp(sval, "none") ? NX_F_NO_PARTICLE_INTERACTION
                           : !strcmp(sval, "mixed") ? NX_F_MIXED_MODE : NX_F_SPH;
    } else if (k == "plane" && na >= 4) {
      auto* p = new NxPlaneShapeDesc();
      p->normal = NxVec3(a[0], a[1], a[2]);
      p->d = a[3];
      s.addStatic(p);
    } else if (k == "box" && na >= 6) {
      auto* b = new NxBoxShapeDesc();
      b->dimensions = NxVec3(a[0], a[1], a[2]);
      b->localPose.t = NxVec3(a[3], a[4], a[5]);
      s.addStatic(b);
    } else if (k == "rotbox" && na >= 10) {
      // rotbox hx hy hz px py pz ax ay az deg: static box rotated by deg degrees about axis (ax, ay, az)
      auto* b = new NxBoxShapeDesc();
      b->dimensions = NxVec3(a[0], a[1], a[2]);
      NxVec3 axis(a[6], a[7], a[8]);
      axis.normalize();
      NxQuat q;
      q.fromAngleAxis(a[9], axis);
      b->localPose.M.fromQuat(q);
      b->localPose.t = NxVec3(a[3], a[4], a[5]);
      s.addStatic(b);
    } else if (k == "sphere" && na >= 4) {
      auto* sp = new NxSphereShapeDesc();
      sp->radius = a[0];
      sp->localPose.t = NxVec3(a[1], a[2], a[3]);
      s.addStatic(sp);
    } else if (k == "capsule" && na >= 5) {
      auto* c = new NxCapsuleShapeDesc();
      c->radius = a[0];
      c->height = a[1];
      c->localPose.t = NxVec3(a[2], a[3], a[4]);
      s.addStatic(c);
    } else if (k == "dynbox" && na >= 7) {
      auto* b = new NxBoxShapeDesc();
      b->dimensions = NxVec3(a[0], a[1], a[2]);
      s.bodies.push_back({b, NxVec3(a[3], a[4], a[5]), NxVec3(0, 0, 0), a[6], false});
      s.lastShape = b;
    } else if (k == "dynsphere" && na >= 5) {
      auto* sp = new NxSphereShapeDesc();
      sp->radius = a[0];
      s.bodies.push_back({sp, NxVec3(a[1], a[2], a[3]), NxVec3(0, 0, 0), a[4], false});
      s.lastShape = sp;
    } else if (k == "kinbox" && na >= 10) {
      auto* b = new NxBoxShapeDesc();
      b->dimensions = NxVec3(a[0], a[1], a[2]);
      s.bodies.push_back({b, NxVec3(a[3], a[4], a[5]), NxVec3(a[7], a[8], a[9]), a[6], true});
      s.lastShape = b;
    } else if (k == "shapeflag") {
      NxU32 bit = !strcmp(sval, "FLUID_TWOWAY") ? NX_SF_FLUID_TWOWAY
                  : !strcmp(sval, "FLUID_DISABLE_COLLISION") ? NX_SF_FLUID_DISABLE_COLLISION
                  : !strcmp(sval, "FLUID_DRAIN") ? NX_SF_FLUID_DRAIN
                  : !strcmp(sval, "DISABLE_COLLISION") ? NX_SF_DISABLE_COLLISION
                  : !strcmp(sval, "DISABLE_RESPONSE") ? NX_SF_DISABLE_RESPONSE
                  : !strcmp(sval, "TRIGGER_ENABLE") ? NX_TRIGGER_ENABLE : 0;
      if (!bit || !s.lastShape) {
        fprintf(stderr, "%s:%d: shapeflag '%s' needs a known name and a preceding shape\n", path, lineNo, sval);
        fclose(f);
        return false;
      }
      s.lastShape->shapeFlags |= bit;
    } else if (k == "actorflag") {
      // Actor flag of the actor holding the most recent STATIC shape (NxActorDesc.flags).
      NxU32 bit = !strcmp(sval, "DISABLE_COLLISION") ? NX_AF_DISABLE_COLLISION
                  : !strcmp(sval, "DISABLE_RESPONSE") ? NX_AF_DISABLE_RESPONSE
                  : !strcmp(sval, "FLUID_DISABLE_COLLISION") ? NX_AF_FLUID_DISABLE_COLLISION : 0;
      if (!bit || !s.lastShape || std::find(s.statics.begin(), s.statics.end(), s.lastShape) == s.statics.end()) {
        fprintf(stderr, "%s:%d: actorflag '%s' needs a known name and a preceding static shape\n", path, lineNo, sval);
        fclose(f);
        return false;
      }
      s.staticActorFlags[s.lastShape] |= bit;
    } else if (k == "quad" && na >= 5) {
      // Corners ordered so that (b-a)x(c-a) = +Y for both triangles.
      const float sx = a[0], sz = a[1], px = a[2], py = a[3], pz = a[4];
      const float v[6][3] = {{-sx, 0, -sz}, {-sx, 0, sz}, {sx, 0, -sz}, {sx, 0, -sz}, {-sx, 0, sz}, {sx, 0, sz}};
      for (const auto& p : v) {
        s.meshTris.push_back(p[0] + px);
        s.meshTris.push_back(p[1] + py);
        s.meshTris.push_back(p[2] + pz);
      }
    } else if (k == "tri" && na >= 9) {
      s.meshTris.insert(s.meshTris.end(), a, a + 9);
    } else if (k == "jitter" && na >= 1) {
      s.jitter = a[0];
      s.jitterSeed = na >= 2 ? (unsigned)a[1] : 1u;
    } else if (k == "hfield" && na >= 8) {
      HfieldSpec h;
      h.rows = (int)a[0], h.cols = (int)a[1];
      h.rowScale = a[2], h.colScale = a[3], h.heightScale = a[4];
      h.px = a[5], h.py = a[6], h.pz = a[7];
      if (h.rows < 2 || h.cols < 2 || h.rows > 4000 || h.cols > 4000) {
        fprintf(stderr, "%s:%d: hfield needs 2..4000 rows and columns\n", path, lineNo);
        fclose(f);
        return false;
      }
      s.hfields.push_back(h);
    } else if (k == "hfhole" && na >= 2) {
      if (s.hfields.empty() || a[0] < 0 || a[1] < 0 || a[0] >= s.hfields.back().rows - 1 ||
          a[1] >= s.hfields.back().cols - 1) {
        fprintf(stderr, "%s:%d: hfhole needs a preceding hfield and a cell inside it\n", path, lineNo);
        fclose(f);
        return false;
      }
      s.hfields.back().holes.push_back({(int)a[0], (int)a[1]});
    } else if (k == "block" && na >= 10) {
      Block b = {{(int)a[0], (int)a[1], (int)a[2]}, a[3], {a[4], a[5], a[6]}, {a[7], a[8], a[9]}};
      s.blocks.push_back(b);
    } else {
      fprintf(stderr, "%s:%d: unknown or malformed directive '%s'\n", path, lineNo, key);
      fclose(f);
      return false;
    }
  }
  fclose(f);
  return true;
}

struct ParticleOut {
  float pos[3], vel[3], density, life, normal[3];
  NxU32 id, flags;
};

// Cooks every quad/tri triangle into one NxTriangleMesh and appends a static triangle-mesh shape to sc.statics.
bool cookMeshShape(LoadedSdk& L, NxUserOutputStream* out, Scenario& sc) {
  auto getCooking = (PFN_NxGetCookingLib)GetProcAddress(L.loader, "NxGetCookingLib");
  NxCookingInterface* cooking = getCooking ? getCooking(NX_PHYSICS_SDK_VERSION) : nullptr;
  if (!cooking || !cooking->NxInitCooking(nullptr, out)) {
    printf("{\"ok\":false,\"error\":\"cooking unavailable\"}\n");
    return false;
  }
  const NxU32 nT = (NxU32)(sc.meshTris.size() / 9);
  std::vector<NxU32> idx(3 * nT);
  for (NxU32 i = 0; i < 3 * nT; ++i) idx[i] = i;
  NxTriangleMeshDesc md;
  md.numVertices = 3 * nT;
  md.numTriangles = nT;
  md.pointStrideBytes = 3 * sizeof(float);
  md.triangleStrideBytes = 3 * sizeof(NxU32);
  md.points = sc.meshTris.data();
  md.triangles = idx.data();
  md.flags = 0;  // no NX_MF_FLIPNORMALS
  MemStream ms;
  bool cooked = cooking->NxCookTriangleMesh(md, ms);
  cooking->NxCloseCooking();
  if (!cooked) {
    printf("{\"ok\":false,\"error\":\"NxCookTriangleMesh failed\"}\n");
    return false;
  }
  NxTriangleMesh* tm = L.sdk->createTriangleMesh(ms);
  if (!tm) {
    printf("{\"ok\":false,\"error\":\"createTriangleMesh failed\"}\n");
    return false;
  }
  auto* shape = new NxTriangleMeshShapeDesc();
  shape->meshData = tm;
  sc.statics.push_back(shape);
  return true;
}

// Creates each hfield line's NxHeightField (procedural S16_TM samples, holes as material 127) and appends a static
// NxHeightFieldShape to sc.statics.
bool createHeightFieldShapes(LoadedSdk& L, Scenario& sc) {
  const NxMaterialIndex kHoleMaterial = 127;
  for (const HfieldSpec& h : sc.hfields) {
    std::vector<NxHeightFieldSample> samples((size_t)h.rows * h.cols);
    for (int r = 0; r < h.rows; ++r)
      for (int c = 0; c < h.cols; ++c) {
        NxHeightFieldSample& s = samples[(size_t)r * h.cols + c];
        const double height = 10.0 * sin(0.5 * r) + 7.0 * cos(0.4 * c) + 2.0 * r;
        s.height = (NxI16)lround(height);
        s.materialIndex0 = 0;
        s.materialIndex1 = 0;
        s.tessFlag = (r + c) & 1;
        s.unused = 0;
      }
    for (const auto& hole : h.holes) {  // both triangles of the cell whose upper-left corner is this sample
      NxHeightFieldSample& s = samples[(size_t)hole.first * h.cols + hole.second];
      s.materialIndex0 = kHoleMaterial;
      s.materialIndex1 = kHoleMaterial;
    }
    NxHeightFieldDesc hd;
    hd.nbRows = h.rows;
    hd.nbColumns = h.cols;
    hd.format = NX_HF_S16_TM;
    hd.sampleStride = sizeof(NxHeightFieldSample);
    hd.samples = samples.data();  // copied by the SDK
    NxHeightField* field = L.sdk->createHeightField(hd);
    if (!field) {
      printf("{\"ok\":false,\"error\":\"createHeightField failed\"}\n");
      return false;
    }
    auto* shape = new NxHeightFieldShapeDesc();
    shape->heightField = field;
    shape->heightScale = h.heightScale;
    shape->rowScale = h.rowScale;
    shape->columnScale = h.colScale;
    shape->holeMaterial = kHoleMaterial;
    shape->materialIndexHighBits = 0;
    shape->localPose.t = NxVec3(h.px, h.py, h.pz);
    sc.statics.push_back(shape);
  }
  return true;
}

}  // namespace

int main(int argc, char** argv) {
  setvbuf(stdout, nullptr, _IONBF, 0);
  if (argc < 3) {
    fprintf(stderr, "usage: fluid_oracle <scenario.txt> <out.bin> [--loader path]\n");
    return 1;
  }
  const char* loaderPath = "PhysXLoader.dll";
  for (int i = 3; i < argc; ++i)
    if (!strcmp(argv[i], "--loader") && i + 1 < argc) loaderPath = argv[++i];

  Scenario sc;
  if (!parse(argv[1], sc)) return 1;

  StderrOutput out;
  LoadedSdk L;
  if (!L.open(loaderPath, &out)) {
    printf("{\"ok\":false,\"error\":\"sdk create failed\",\"create_error\":%d}\n", (int)L.err);
    return 2;
  }
  if (!sc.meshTris.empty() && !cookMeshShape(L, &out, sc)) {
    L.close();
    return 2;
  }
  if (!sc.hfields.empty() && !createHeightFieldShapes(L, sc)) {
    L.close();
    return 2;
  }

  NxSceneDesc sd;
  sd.gravity = sc.gravity;
  sd.simType = NX_SIMULATION_SW;
  NxScene* scene = L.sdk->createScene(sd);
  if (!scene) {
    printf("{\"ok\":false,\"error\":\"createScene failed\"}\n");
    L.close();
    return 2;
  }
  // Fixed stepping: exactly one substep of dt per simulate(dt), unless a timing directive sets the substep.
  scene->setTiming(sc.maxTimestep > 0 ? sc.maxTimestep : sc.dt, sc.maxTimestep > 0 ? sc.maxIter : 1, NX_TIMESTEP_FIXED);
  for (const auto& g : sc.groupFlags) scene->setGroupCollisionFlag((NxU16)g.a, (NxU16)g.b, g.on);
  NxCompartment* rbComp = nullptr;  // one rigid-body compartment for the rbcompartment statics
  if (!sc.rbCompartment.empty()) {
    NxCompartmentDesc cd;
    cd.type = NX_SCT_RIGIDBODY;
    cd.deviceCode = NX_DC_CPU;
    rbComp = scene->createCompartment(cd);
    if (!rbComp) fprintf(stderr, "rigid-body compartment creation failed\n");
  }

  for (NxShapeDesc* sh : sc.statics) {
    NxActorDesc ad;
    ad.shapes.pushBack(sh);
    ad.compartment = sc.rbCompartment.count(sh) ? rbComp : nullptr;
    if (sc.staticActorFlags.count(sh)) ad.flags |= sc.staticActorFlags[sh];
    if (!scene->createActor(ad)) fprintf(stderr, "static actor creation failed\n");
  }

  // Dynamic and kinematic actors (bodies.bin lists them in this order). Mass is given with zero density and
  // zero inertia, so the SDK derives the inertia tensor from the shape at that mass.
  std::vector<NxActor*> actors;
  for (const DynBody& db : sc.bodies) {
    NxActorDesc ad;
    ad.shapes.pushBack(db.shape);
    ad.globalPose.id();
    ad.globalPose.t = db.p;
    NxBodyDesc bd;
    bd.mass = db.mass;
    bd.linearVelocity = db.kinematic ? NxVec3(0, 0, 0) : db.v;
    if (db.kinematic) bd.flags |= NX_BF_KINEMATIC;
    ad.body = &bd;
    NxActor* a = scene->createActor(ad);
    if (!a) fprintf(stderr, "body actor creation failed\n");
    actors.push_back(a);
  }

  // Particle write buffers
  const NxU32 maxP = sc.fd.maxParticles;
  std::vector<ParticleOut> buf(maxP);
  NxU32 numParticles = 0;
  NxParticleData& w = sc.fd.particlesWriteData;
  w.numParticlesPtr = &numParticles;
  w.bufferPos = &buf[0].pos[0];
  w.bufferPosByteStride = sizeof(ParticleOut);
  w.bufferVel = &buf[0].vel[0];
  w.bufferVelByteStride = sizeof(ParticleOut);
  w.bufferDensity = &buf[0].density;
  w.bufferDensityByteStride = sizeof(ParticleOut);
  w.bufferLife = &buf[0].life;
  w.bufferLifeByteStride = sizeof(ParticleOut);
  w.bufferCollisionNormal = &buf[0].normal[0];
  w.bufferCollisionNormalByteStride = sizeof(ParticleOut);
  w.bufferId = &buf[0].id;
  w.bufferIdByteStride = sizeof(ParticleOut);
  w.bufferFlag = &buf[0].flags;
  w.bufferFlagByteStride = sizeof(ParticleOut);

  // Id write buffers and packet data, as the game passes them (contents only seen in the shim trace).
  NxU32 numCreated = 0, numDeleted = 0, numPackets = 0;
  std::vector<NxU32> createdIds(maxP), deletedIds(maxP);
  std::vector<NxFluidPacket> packets(maxP);
  if (sc.writeIds) {
    sc.fd.particleCreationIdWriteData.numIdsPtr = &numCreated;
    sc.fd.particleCreationIdWriteData.bufferId = createdIds.data();
    sc.fd.particleCreationIdWriteData.bufferIdByteStride = sizeof(NxU32);
    sc.fd.particleDeletionIdWriteData.numIdsPtr = &numDeleted;
    sc.fd.particleDeletionIdWriteData.bufferId = deletedIds.data();
    sc.fd.particleDeletionIdWriteData.bufferIdByteStride = sizeof(NxU32);
    sc.fd.fluidPacketData.numFluidPacketsPtr = &numPackets;
    sc.fd.fluidPacketData.bufferFluidPackets = packets.data();
  }
  if (sc.compartment) {
    NxCompartmentDesc cd;
    cd.type = NX_SCT_FLUID;
    cd.deviceCode = NX_DC_CPU;
    sc.fd.compartment = scene->createCompartment(cd);
    if (!sc.fd.compartment) fprintf(stderr, "createCompartment failed\n");
  }

  NxFluid* fluid = scene->createFluid(sc.fd);
  if (!fluid) {
    printf("{\"ok\":false,\"error\":\"createFluid failed (descriptor valid=%d)\"}\n", (int)sc.fd.isValid());
    L.close();
    return 3;
  }
  const NxReal particleMass = fluid->getParticleMass();
  const NxReal responseCoef = fluid->getCollisionResponseCoefficient();

  // Initial particles (deterministic lattice)
  std::vector<float> ipos, ivel;
  for (const Block& b : sc.blocks)
    for (int z = 0; z < b.n[2]; ++z)
      for (int y = 0; y < b.n[1]; ++y)
        for (int x = 0; x < b.n[0]; ++x) {
          ipos.insert(ipos.end(), {b.o[0] + x * b.spacing, b.o[1] + y * b.spacing, b.o[2] + z * b.spacing});
          ivel.insert(ivel.end(), {b.v[0], b.v[1], b.v[2]});
        }
  if (sc.jitter > 0) {
    unsigned st = sc.jitterSeed * 2654435761u + 12345u;
    for (float& x : ipos) {
      st = st * 1664525u + 1013904223u;
      x += sc.jitter * ((float)(st >> 8) * (2.0f / 16777216.0f) - 1.0f);
    }
  }
  NxU32 nInit = (NxU32)(ipos.size() / 3);
  NxU32 added = 0;
  if (nInit) {
    NxParticleData pd;
    pd.numParticlesPtr = &nInit;
    pd.bufferPos = ipos.data();
    pd.bufferPosByteStride = 3 * sizeof(float);
    pd.bufferVel = ivel.data();
    pd.bufferVelByteStride = 3 * sizeof(float);
    added = fluid->addParticles(pd);
  }
  // fluid.copies N: N - 1 more fluids with the same descriptor and particles, each with its own write buffer
  // (their output is not written; fluids do not interact, so out.bin must not change).
  std::vector<std::vector<ParticleOut>> copyBufs(sc.copies > 1 ? sc.copies - 1 : 0, std::vector<ParticleOut>(maxP));
  std::vector<NxU32> copyCounts(copyBufs.size(), 0);
  for (size_t ci = 0; ci < copyBufs.size(); ++ci) {
    NxFluidDesc d2 = sc.fd;
    d2.particleCreationIdWriteData = NxParticleIdData();
    d2.particleDeletionIdWriteData = NxParticleIdData();
    d2.fluidPacketData = NxFluidPacketData();
    NxParticleData& w2 = d2.particlesWriteData;
    w2 = NxParticleData();
    w2.numParticlesPtr = &copyCounts[ci];
    w2.bufferPos = &copyBufs[ci][0].pos[0];
    w2.bufferPosByteStride = sizeof(ParticleOut);
    NxFluid* f2 = scene->createFluid(d2);
    if (!f2) { fprintf(stderr, "fluid copy %zu: createFluid failed\n", ci); continue; }
    if (nInit) {
      NxParticleData pd;
      pd.numParticlesPtr = &nInit;
      pd.bufferPos = ipos.data();
      pd.bufferPosByteStride = 3 * sizeof(float);
      pd.bufferVel = ivel.data();
      pd.bufferVelByteStride = 3 * sizeof(float);
      f2->addParticles(pd);
    }
  }

  // SDK-reported mass of each body (after createActor), for checking the mass the impulses are divided by.
  std::string bodyMasses;
  for (NxActor* a : actors) {
    char buf32[32];
    snprintf(buf32, sizeof(buf32), "%s%.9g", bodyMasses.empty() ? "" : ",", a ? a->getMass() : 0.0f);
    bodyMasses += buf32;
  }

  FILE* fo = fopen(argv[2], "wb");
  if (!fo) {
    printf("{\"ok\":false,\"error\":\"cannot open output\"}\n");
    return 4;
  }
  // bodies.bin goes next to out.bin (same directory, fixed name).
  std::string bodiesPath = argv[2];
  const size_t slash = bodiesPath.find_last_of("/\\");
  bodiesPath = (slash == std::string::npos ? std::string() : bodiesPath.substr(0, slash + 1)) + "bodies.bin";
  FILE* fbod = fopen(bodiesPath.c_str(), "wb");
  if (!fbod) {
    printf("{\"ok\":false,\"error\":\"cannot open bodies output\"}\n");
    fclose(fo);
    return 4;
  }
  const NxU32 hdr[3] = {1, (NxU32)sc.steps, maxP};
  fwrite("PXFD", 1, 4, fo);
  fwrite(hdr, sizeof(hdr), 1, fo);
  fwrite(&sc.dt, sizeof(float), 1, fo);

  double total = 0, worst = 0;
  bool nan = false;
  for (int s = 0; s < sc.steps; ++s) {
    // Kinematic bodies: target = start + v * (s+1) * dt, i.e. where they should be after this step.
    for (size_t i = 0; i < actors.size(); ++i)
      if (sc.bodies[i].kinematic && actors[i])
        actors[i]->moveGlobalPosition(sc.bodies[i].p + sc.bodies[i].v * ((s + 1) * sc.dt));
    for (const Event& e : sc.events) {
      if (e.step != s) continue;
      if (e.kind == Event::Add) {
        std::vector<float> p, v, life;
        const Block& b = e.block;
        for (int z = 0; z < b.n[2]; ++z)
          for (int y = 0; y < b.n[1]; ++y)
            for (int x = 0; x < b.n[0]; ++x) {
              p.insert(p.end(), {b.o[0] + x * b.spacing, b.o[1] + y * b.spacing, b.o[2] + z * b.spacing});
              v.insert(v.end(), {b.v[0], b.v[1], b.v[2]});
              life.push_back(e.life);
            }
        NxU32 n = (NxU32)life.size();
        NxParticleData pd;
        pd.numParticlesPtr = &n;
        pd.bufferPos = p.data();
        pd.bufferPosByteStride = 3 * sizeof(float);
        pd.bufferVel = v.data();
        pd.bufferVelByteStride = 3 * sizeof(float);
        if (e.life > 0) {
          pd.bufferLife = life.data();
          pd.bufferLifeByteStride = sizeof(float);
        }
        fluid->addParticles(pd);
      } else if (e.kind == Event::RemoveAll) {
        fluid->removeAllParticles();
      } else if (e.kind == Event::SetDt) {
        // not an update: applied to the simulate call below
      } else if (e.kind == Event::Force) {
        const NxU32 n = (NxU32)e.forceIds.size();
        if (n == 0) continue;
        NxParticleUpdateData ud;
        ud.forceMode = (NxForceMode)e.mode;
        ud.numUpdates = n;
        ud.bufferId = const_cast<NxU32*>(e.forceIds.data());
        ud.bufferIdByteStride = sizeof(NxU32);
        ud.bufferForce = const_cast<float*>(e.forceVec.data());
        ud.bufferForceByteStride = 3 * sizeof(float);
        ud.bufferFlag = nullptr;
        fluid->updateParticles(ud);
      } else {
        std::vector<NxU32> idFlag;  // interleaved id, flag (stride 8), like the game
        if (e.kind == Event::Delete) {
          for (NxU32 id : e.ids) idFlag.insert(idFlag.end(), {id, (NxU32)NX_FP_DELETE});
        } else {
          for (NxU32 i = 0; i < numParticles; ++i)
            if (buf[i].id % e.k == e.r) idFlag.insert(idFlag.end(), {buf[i].id, (NxU32)NX_FP_DELETE});
        }
        if (idFlag.empty()) continue;
        NxParticleUpdateData ud;
        ud.numUpdates = (NxU32)(idFlag.size() / 2);
        ud.bufferId = &idFlag[0];
        ud.bufferIdByteStride = 2 * sizeof(NxU32);
        ud.bufferFlag = &idFlag[1];
        ud.bufferFlagByteStride = 2 * sizeof(NxU32);
        fluid->updateParticles(ud);
      }
    }
    float stepDt = sc.dt;
    for (const Event& e : sc.events)
      if (e.step == s && e.kind == Event::SetDt) stepDt = e.dtValue;
    double t0 = nowSeconds();
    scene->simulate(stepDt);
    scene->flushStream();
    scene->fetchResults(NX_RIGID_BODY_FINISHED, true);
    double t = nowSeconds() - t0;
    total += t;
    NxBounds3 wb;
    fluid->getWorldBounds(wb);  // value only visible in the shim trace (PXSHIM_TRACE=2)
    if (t > worst) worst = t;
    fwrite(&numParticles, sizeof(NxU32), 1, fo);
    fwrite(&t, sizeof(double), 1, fo);
    fwrite(buf.data(), sizeof(ParticleOut), numParticles, fo);
    for (NxU32 i = 0; i < numParticles && !nan; ++i)
      for (int c = 0; c < 3; ++c)
        if (buf[i].pos[c] != buf[i].pos[c]) nan = true;

    const NxU32 step = (NxU32)s, nb = (NxU32)actors.size();
    fwrite(&step, sizeof(NxU32), 1, fbod);
    fwrite(&nb, sizeof(NxU32), 1, fbod);
    for (NxActor* a : actors) {
      const NxVec3 p = a ? a->getGlobalPosition() : NxVec3(0, 0, 0);
      const NxVec3 v = a ? a->getLinearVelocity() : NxVec3(0, 0, 0);
      const float rec[6] = {p.x, p.y, p.z, v.x, v.y, v.z};
      fwrite(rec, sizeof(rec), 1, fbod);
    }
  }
  fclose(fo);
  fclose(fbod);

  printf("{\"ok\":%s,\"scenario\":\"%s\",\"steps\":%d,\"dt\":%g,\"initial\":%u,\"added\":%u,\"final_count\":%u,"
         "\"nan\":%s,\"sim_seconds_total\":%.6f,\"sim_seconds_worst\":%.6f,\"sdk_errors\":%d,\"sdk_warnings\":%d,"
         "\"particle_mass\":%.9g,\"collision_response_coefficient\":%.9g,\"bodies\":%u,\"body_masses\":[%s]}\n",
         (!nan && out.errors == 0) ? "true" : "false", argv[1], sc.steps, sc.dt, nInit, added, numParticles,
         nan ? "true" : "false", total, worst, out.errors, out.warnings, particleMass, responseCoef,
         (unsigned)actors.size(), bodyMasses.c_str());

  scene->releaseFluid(*fluid);
  L.sdk->releaseScene(*scene);
  L.close();
  for (NxShapeDesc* sh : sc.statics) delete sh;
  for (const DynBody& db : sc.bodies) delete db.shape;
  return (!nan && out.errors == 0) ? 0 : 5;
}
