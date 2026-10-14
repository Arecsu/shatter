// Cloth oracle: run a scenario through the original PhysX core (software cloth) and dump every vertex each step.
//
//   cloth_oracle.exe <scenario.txt> <out.bin> [--loader PhysXLoader.dll]
//
// The cloth mesh is a regular grid, cooked at run time with the PhysXCooking.dll the loader picks
// (NxGetCookingLib, same SDK version as the core).
//
// Scenario file: one directive per line, '#' comments. Units are metres / seconds.
//   dt 0.0166667            steps 120            gravity 0 -9.81 0
//   grid nx ny spacing ox oy oz axis    nx*ny vertices, (nx-1)*(ny-1)*2 triangles, alternating diagonals.
//                                       axis xz (horizontal, +x/+z) or xy (vertical, +x/-y: row 0 is the top).
//                                       Vertex index = iy*nx + ix.
//   cloth.pose m00 .. m22 tx ty tz   creation pose (row-major 3x3, then translation); pins follow it
//   cloth.<field> <value>   thickness, density, bendingStiffness, stretchingStiffness, dampingCoefficient, friction,
//                           pressure, tearFactor, collisionResponseCoefficient, attachmentResponseCoefficient,
//                           attachmentTearFactor, solverIterations, sleepLinearVelocity, wakeUpCounter,
//                           relativeGridSpacing, minAdhereVelocity, externalAcceleration x y z, windAcceleration x y z
//   flag <name> 0|1         NxClothFlag by name: pressure static disable_collision selfcollision visualization gravity
//                           bending bending_ortho damping collision_twoway triangle_collision tearable comdamping
//                           validbounds fluid_collision disable_dynamic_ccd adhere
//   bounds x0 y0 z0 x1 y1 z1  cloth->setValidBounds (after creation); with flag validbounds 1 the particles strictly
//                           outside the box are removed (sticky, frozen at v=0)
//   mesh.tearable 0|1       cook with NX_CLOTH_MESH_TEARABLE (needed for tearing; buffers get 2x headroom)
//   mesh.maxVertices n      receive buffer size in vertices (default: 2x for a tearable mesh, else the vertex count)
//   pin ix iy               attach grid vertex (ix,iy) to its initial world position (attachVertexToGlobalPosition)
//   sleep                   putToSleep right after creation, before the first simulate (the game's respawn does this)
//   repin                   attachVertexToGlobalPosition again for every pin before each simulate (as UE3 does)
//   plane nx ny nz d | box hx hy hz px py pz | sphere r px py pz | capsule r h px py pz     static shapes
//   boxmesh hx hy hz px py pz [n]   static box as a cooked triangle mesh (outward faces, n x n quads per face)
//   attach shape k flags    cloth.attachToShape(static shape k (creation order), flags)
//   attach colliding flags  cloth.attachToCollidingShapes(flags)  (exercises the shim's attach logging)
//   kinematic k             static shape k becomes a kinematic actor (mass 1); its shape is re-centred on the actor
//   dynamic k mass          static shape k becomes a dynamic actor of this mass (falls under gravity)
//   move k vx vy vz         kinematic shape k: before step s, moveGlobalPosition(centre + v * (s+1) * dt)
//   kinvel 0|1              1 (default): a kinematic body is created with velocity v too (SDK setLinearVelocity on
//                           a kinematic actor is an error, so it is not repeated each step); dynamic: initial velocity v
//   jitter amp seed         add uniform ±amp to every initial vertex position after creation (setPositions; the cooked
//                           rest shape is unchanged). Same LCG as fluid_oracle.
//
// Output (little endian) "PXCL": u32 version=1, u32 steps, u32 maxVertices, u32 initialVertices,
//   u32 initialTriangles, f32 dt, then initialTriangles*3 u32 indices and initialVertices x f32 pos[3]
//   (the mesh as cooked = rest shape, before pins/jitter), then per step:
//   u32 numVertices, u32 numTriangles, f64 stepSeconds, u32 hasIndices,
//   [numTriangles*3 u32 indices if hasIndices (first step and whenever the index buffer changed, e.g. tearing)],
//   numVertices x { f32 pos[3], f32 vel[3] }   (user vertex order; torn-off vertices are appended)
// A JSON summary line goes to stdout.

#include <algorithm>
#include <vector>
#include <string>

#include "oracle_common.h"
#include "NxStream.h"
#include "NxCooking.h"
#include "NxRay.h"

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

struct Pin { int ix, iy; };
struct Attach { bool colliding; int shape; NxU32 flags; };
struct BodyMove { int shape; NxVec3 v; };  // kinematic static shape k: centre += v * (step+1) * dt

struct RayProbe { int step; float o[3], d[3]; };  // raycast <step> ox oy oz dx dy dz
struct ForceCmd { int step; float p[3], f[3], r; int mode; };  // force <step> px py pz fx fy fz radius mode (before that step)

struct Scenario {
  float dt = 1.0f / 60.0f;
  int steps = 120;
  NxVec3 gravity{0, -9.81f, 0};
  NxClothDesc cd;
  bool haveGrid = false;
  int nx = 0, ny = 0;
  float spacing = 0.1f, origin[3] = {0, 0, 0};
  bool vertical = false;
  bool meshTearable = false;
  unsigned maxVertices = 0;  // mesh.maxVertices (0: default)
  bool haveBounds = false;   // bounds directive: cloth->setValidBounds (NX_CLF_VALIDBOUNDS)
  float boundsMin[3] = {0, 0, 0}, boundsMax[3] = {0, 0, 0};
  std::vector<Pin> pins;
  struct BoxMesh { size_t slot; float h[3], p[3]; int n; };
  std::vector<BoxMesh> boxMeshes;  // statics[slot] is filled once cooking is up
  bool sleepAtStart = false;
  bool repin = false;  // re-attach the pins before every simulate (UE3 does this every frame for its fixed vertices)
  std::vector<Attach> attaches;
  std::vector<NxShapeDesc*> statics;  // owned
  std::vector<int> kinematic;                 // shape indices made kinematic actors (NX_BF_KINEMATIC)
  std::vector<std::pair<int, float>> dynamic; // shape index, mass: dynamic actors (fall under gravity)
  std::vector<BodyMove> moves;                // per-step linear velocity of a kinematic shape
  std::vector<RayProbe> rays;                 // cloth->raycast calls, made after that step's fetchResults
  std::vector<ForceCmd> forces;               // cloth->addDirectedForceAtPos calls, made before that step's simulate
  bool kinVel = true;                         // kinvel 0|1: also give the kinematic body its velocity (setLinearVelocity)
  float jitter = 0;
  unsigned jitterSeed = 1;
};

struct FlagName { const char* name; NxU32 bit; };
const FlagName kFlags[] = {
    {"pressure", NX_CLF_PRESSURE}, {"static", NX_CLF_STATIC}, {"disable_collision", NX_CLF_DISABLE_COLLISION},
    {"selfcollision", NX_CLF_SELFCOLLISION}, {"visualization", NX_CLF_VISUALIZATION}, {"gravity", NX_CLF_GRAVITY},
    {"bending", NX_CLF_BENDING}, {"bending_ortho", NX_CLF_BENDING_ORTHO}, {"damping", NX_CLF_DAMPING},
    {"collision_twoway", NX_CLF_COLLISION_TWOWAY}, {"triangle_collision", NX_CLF_TRIANGLE_COLLISION},
    {"tearable", NX_CLF_TEARABLE}, {"comdamping", NX_CLF_COMDAMPING}, {"validbounds", NX_CLF_VALIDBOUNDS},
    {"fluid_collision", NX_CLF_FLUID_COLLISION}, {"disable_dynamic_ccd", NX_CLF_DISABLE_DYNAMIC_CCD},
    {"adhere", NX_CLF_ADHERE}, {"hardware", NX_CLF_HARDWARE},
};

bool parse(const char* path, Scenario& s) {
  FILE* f = fopen(path, "r");
  if (!f) return false;
  s.cd.setToDefault();
  s.cd.flags &= ~NX_CLF_HARDWARE;
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
    NxClothDesc& d = s.cd;
    bool ok = true;
    if (k == "dt") s.dt = a[0];
    else if (k == "steps") s.steps = (int)a[0];
    else if (k == "gravity") s.gravity = NxVec3(a[0], a[1], a[2]);
    else if (k == "cloth.thickness") d.thickness = a[0];
    else if (k == "cloth.pose" && na == 12) {  // m00 m01 m02 m10 .. m22 tx ty tz (row-major, NxMat33::setRowMajor)
      d.globalPose.M.setRowMajor(a);
      d.globalPose.t = NxVec3(a[9], a[10], a[11]);
    }
    else if (k == "cloth.density") d.density = a[0];
    else if (k == "cloth.bendingStiffness") d.bendingStiffness = a[0];
    else if (k == "cloth.stretchingStiffness") d.stretchingStiffness = a[0];
    else if (k == "cloth.dampingCoefficient") d.dampingCoefficient = a[0];
    else if (k == "cloth.friction") d.friction = a[0];
    else if (k == "cloth.pressure") d.pressure = a[0];
    else if (k == "cloth.tearFactor") d.tearFactor = a[0];
    else if (k == "cloth.collisionResponseCoefficient") d.collisionResponseCoefficient = a[0];
    else if (k == "cloth.attachmentResponseCoefficient") d.attachmentResponseCoefficient = a[0];
    else if (k == "cloth.attachmentTearFactor") d.attachmentTearFactor = a[0];
    else if (k == "cloth.solverIterations") d.solverIterations = (NxU32)a[0];
    else if (k == "cloth.sleepLinearVelocity") d.sleepLinearVelocity = a[0];
    else if (k == "cloth.wakeUpCounter") d.wakeUpCounter = a[0];
    else if (k == "cloth.relativeGridSpacing") d.relativeGridSpacing = a[0];
    else if (k == "cloth.minAdhereVelocity") d.minAdhereVelocity = a[0];
    else if (k == "cloth.externalAcceleration") d.externalAcceleration = NxVec3(a[0], a[1], a[2]);
    else if (k == "cloth.windAcceleration") d.windAcceleration = NxVec3(a[0], a[1], a[2]);
    else if (k == "flag") {
      char name[64] = "";
      int v = 1;
      ok = sscanf(rest, "%63s %d", name, &v) >= 1;
      bool found = false;
      for (const FlagName& fl : kFlags)
        if (!strcmp(fl.name, name)) {
          found = true;
          if (v) d.flags |= fl.bit;
          else d.flags &= ~fl.bit;
        }
      ok = ok && found;
    } else if (k == "bounds" && na >= 6) {
      s.haveBounds = true;
      for (int c = 0; c < 3; ++c) {
        s.boundsMin[c] = a[c];
        s.boundsMax[c] = a[3 + c];
      }
    } else if (k == "mesh.tearable") s.meshTearable = a[0] != 0;
    else if (k == "mesh.maxVertices") s.maxVertices = (unsigned)a[0];
    else if (k == "grid" && na >= 6) {
      char axis[16] = "xz";
      sscanf(rest, "%*f %*f %*f %*f %*f %*f %15s", axis);
      s.haveGrid = true;
      s.nx = (int)a[0];
      s.ny = (int)a[1];
      s.spacing = a[2];
      s.origin[0] = a[3];
      s.origin[1] = a[4];
      s.origin[2] = a[5];
      s.vertical = !strcmp(axis, "xy");
    } else if (k == "pin" && na >= 2) s.pins.push_back({(int)a[0], (int)a[1]});
    else if (k == "sleep") s.sleepAtStart = true;
    else if (k == "repin") s.repin = true;
    else if (k == "attach" && (!strcmp(sval, "shape") || !strcmp(sval, "colliding"))) {
      const bool coll = !strcmp(sval, "colliding");
      const char* r2 = rest + strspn(rest, " \t") + strlen(sval);
      float b[2] = {0, 0};
      int nb = sscanf(r2, "%f %f", &b[0], &b[1]);
      if (coll ? nb < 1 : nb < 2) ok = false;
      else s.attaches.push_back({coll, coll ? 0 : (int)b[0], (NxU32)(coll ? b[0] : b[1])});
    }
    else if (k == "jitter" && na >= 2) {
      s.jitter = a[0];
      s.jitterSeed = (unsigned)a[1];
    } else if (k == "plane" && na >= 4) {
      auto* p = new NxPlaneShapeDesc();
      p->normal = NxVec3(a[0], a[1], a[2]);
      p->d = a[3];
      s.statics.push_back(p);
    } else if (k == "box" && na >= 6) {
      auto* b = new NxBoxShapeDesc();
      b->dimensions = NxVec3(a[0], a[1], a[2]);
      b->localPose.t = NxVec3(a[3], a[4], a[5]);
      s.statics.push_back(b);
    } else if (k == "boxmesh" && na >= 6) {
      s.boxMeshes.push_back({s.statics.size(), {a[0], a[1], a[2]}, {a[3], a[4], a[5]}, na >= 7 ? std::max(1, (int)a[6]) : 1});
      s.statics.push_back(nullptr);
    } else if (k == "sphere" && na >= 4) {
      auto* sp = new NxSphereShapeDesc();
      sp->radius = a[0];
      sp->localPose.t = NxVec3(a[1], a[2], a[3]);
      s.statics.push_back(sp);
    } else if (k == "capsule" && na >= 5) {
      auto* c = new NxCapsuleShapeDesc();
      c->radius = a[0];
      c->height = a[1];
      c->localPose.t = NxVec3(a[2], a[3], a[4]);
      s.statics.push_back(c);
    } else if (k == "kinematic" && na >= 1) s.kinematic.push_back((int)a[0]);
    else if (k == "dynamic" && na >= 2) s.dynamic.push_back({(int)a[0], a[1]});
    else if (k == "kinvel" && na >= 1) s.kinVel = a[0] != 0;
    else if (k == "move" && na >= 4) s.moves.push_back({(int)a[0], NxVec3(a[1], a[2], a[3])});
    else if (k == "force" && na >= 9)
      s.forces.push_back({(int)a[0], {a[1], a[2], a[3]}, {a[4], a[5], a[6]}, a[7], (int)a[8]});
    else if (k == "raycast" && na >= 7) s.rays.push_back({(int)a[0], {a[1], a[2], a[3]}, {a[4], a[5], a[6]}});
    else ok = false;
    if (!ok) {
      fprintf(stderr, "%s:%d: unknown or malformed directive '%s'\n", path, lineNo, key);
      fclose(f);
      return false;
    }
  }
  fclose(f);
  if (!s.haveGrid || s.nx < 2 || s.ny < 2) {
    fprintf(stderr, "%s: missing or degenerate 'grid' directive\n", path);
    return false;
  }
  return true;
}

void buildGrid(const Scenario& s, std::vector<float>& pos, std::vector<NxU32>& tri) {
  for (int iy = 0; iy < s.ny; ++iy)
    for (int ix = 0; ix < s.nx; ++ix) {
      float u = ix * s.spacing, v = iy * s.spacing;
      if (s.vertical) pos.insert(pos.end(), {s.origin[0] + u, s.origin[1] - v, s.origin[2]});
      else pos.insert(pos.end(), {s.origin[0] + u, s.origin[1], s.origin[2] + v});
    }
  for (int iy = 0; iy + 1 < s.ny; ++iy)
    for (int ix = 0; ix + 1 < s.nx; ++ix) {
      NxU32 a = iy * s.nx + ix, b = a + 1, c = a + s.nx, d = c + 1;
      if ((ix + iy) & 1) tri.insert(tri.end(), {a, c, b, b, c, d});
      else tri.insert(tri.end(), {a, c, d, a, d, b});
    }
}

struct VertexOut { float pos[3], vel[3]; };

}  // namespace

int main(int argc, char** argv) {
  setvbuf(stdout, nullptr, _IONBF, 0);
  if (argc < 3) {
    fprintf(stderr, "usage: cloth_oracle <scenario.txt> <out.bin> [--loader path]\n");
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
  auto getCooking = (PFN_NxGetCookingLib)GetProcAddress(L.loader, "NxGetCookingLib");
  NxCookingInterface* cooking = getCooking ? getCooking(NX_PHYSICS_SDK_VERSION) : nullptr;
  if (!cooking || !cooking->NxInitCooking(nullptr, &out)) {
    printf("{\"ok\":false,\"error\":\"cooking unavailable\"}\n");
    L.close();
    return 2;
  }

  // Mesh
  std::vector<float> gpos;
  std::vector<NxU32> gtri;
  buildGrid(sc, gpos, gtri);
  const NxU32 nV = (NxU32)(gpos.size() / 3), nT = (NxU32)(gtri.size() / 3);
  NxClothMeshDesc md;
  md.numVertices = nV;
  md.numTriangles = nT;
  md.pointStrideBytes = 3 * sizeof(float);
  md.triangleStrideBytes = 3 * sizeof(NxU32);
  md.points = gpos.data();
  md.triangles = gtri.data();
  md.flags = sc.meshTearable ? NX_CLOTH_MESH_TEARABLE : 0;
  MemStream ms;
  if (!cooking->NxCookClothMesh(md, ms)) {
    printf("{\"ok\":false,\"error\":\"NxCookClothMesh failed\"}\n");
    L.close();
    return 3;
  }
  {  // dump the cooked stream next to out.bin (cooked.bin), for the solver's stored-order edge loader
    std::string cp(argv[2]);
    size_t sl = cp.find_last_of("/\\");
    cp = (sl == std::string::npos ? std::string() : cp.substr(0, sl + 1)) + "cooked.bin";
    if (FILE* fc = fopen(cp.c_str(), "wb")) {
      fwrite(ms.data.data(), 1, ms.data.size(), fc);
      fclose(fc);
    }
  }
  for (const auto& bm : sc.boxMeshes) {  // cooked triangle-mesh boxes (boxmesh)
    std::vector<float> bp;
    std::vector<NxU32> bt;
    const int n = bm.n;
    for (int f = 0; f < 6; ++f) {  // face f: axis f/2, side +-1; (u, v) span the other two axes
      const int ax = f / 2, u = (ax + 1) % 3, v = (ax + 2) % 3;
      const float side = (f & 1) ? 1.f : -1.f;
      const NxU32 base = (NxU32)(bp.size() / 3);
      for (int j = 0; j <= n; ++j)
        for (int i = 0; i <= n; ++i) {
          float q[3];
          q[ax] = side * bm.h[ax];
          q[u] = bm.h[u] * (2.f * i / n - 1.f);
          q[v] = bm.h[v] * (2.f * j / n - 1.f);
          bp.insert(bp.end(), {q[0], q[1], q[2]});
        }
      for (int j = 0; j < n; ++j)
        for (int i = 0; i < n; ++i) {
          NxU32 a0 = base + j * (n + 1) + i, a1 = a0 + 1, a2 = a0 + (n + 1), a3 = a2 + 1;
          // u x v = +ax: (a1 - a0) x (a2 - a0) points along +ax, so the +side face keeps this winding
          if (side > 0) bt.insert(bt.end(), {a0, a1, a2, a1, a3, a2});
          else bt.insert(bt.end(), {a0, a2, a1, a1, a2, a3});
        }
    }
    NxTriangleMeshDesc td;
    td.numVertices = (NxU32)(bp.size() / 3);
    td.numTriangles = (NxU32)(bt.size() / 3);
    td.pointStrideBytes = 3 * sizeof(float);
    td.triangleStrideBytes = 3 * sizeof(NxU32);
    td.points = bp.data();
    td.triangles = bt.data();
    MemStream tms;
    NxTriangleMesh* tm = cooking->NxCookTriangleMesh(td, tms) ? L.sdk->createTriangleMesh(tms) : nullptr;
    if (!tm) {
      printf("{\"ok\":false,\"error\":\"triangle mesh cooking failed\"}\n");
      L.close();
      return 3;
    }
    auto* d = new NxTriangleMeshShapeDesc();
    d->meshData = tm;
    d->localPose.t = NxVec3(bm.p[0], bm.p[1], bm.p[2]);
    sc.statics[bm.slot] = d;
  }
  NxClothMesh* mesh = L.sdk->createClothMesh(ms);
  if (!mesh) {
    printf("{\"ok\":false,\"error\":\"createClothMesh failed\"}\n");
    L.close();
    return 3;
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
  scene->setTiming(sc.dt, 1, NX_TIMESTEP_FIXED);
  std::vector<NxActor*> staticActors;
  struct Mover { NxActor* act; NxVec3 centre, v; };
  std::vector<Mover> movers;
  for (size_t si = 0; si < sc.statics.size(); ++si) {
    NxShapeDesc* sh = sc.statics[si];
    NxActorDesc ad;
    ad.shapes.pushBack(sh);
    NxBodyDesc bd;
    bool kin = std::find(sc.kinematic.begin(), sc.kinematic.end(), (int)si) != sc.kinematic.end();
    float dmass = -1;
    for (const auto& dy : sc.dynamic) if (dy.first == (int)si) dmass = dy.second;
    NxVec3 centre = sh->localPose.t;
    if (kin || dmass > 0) {
      // the body's pose carries the centre; the shape sits at the actor origin
      sh->localPose.t = NxVec3(0, 0, 0);
      ad.globalPose.id();
      ad.globalPose.t = centre;
      bd.mass = kin ? 1.0f : dmass;
      if (kin) bd.flags |= NX_BF_KINEMATIC;
      for (const BodyMove& m : sc.moves)
        if (m.shape == (int)si && (dmass > 0 || (kin && sc.kinVel))) bd.linearVelocity = m.v;  // dynamic: initial velocity
      ad.body = &bd;
    }
    NxActor* act = scene->createActor(ad);
    if (!act) fprintf(stderr, "static actor creation failed\n");
    staticActors.push_back(act);
    for (const BodyMove& m : sc.moves)
      if (m.shape == (int)si && act && kin) movers.push_back({act, centre, m.v});
  }

  // Receive buffers (2x headroom for tearing)
  const NxU32 maxV = sc.maxVertices ? sc.maxVertices : sc.meshTearable ? 2 * nV : nV;
  const NxU32 maxI = sc.meshTearable ? 2 * 3 * nT : 3 * nT;
  std::vector<float> rpos(3 * maxV), rnrm(3 * maxV);
  std::vector<NxU32> ridx(maxI), rparent(maxV);
  NxU32 numV = 0, numI = 0, numParent = 0, dirty = 0;
  NxMeshData& w = sc.cd.meshData;
  w.verticesPosBegin = rpos.data();
  w.verticesNormalBegin = rnrm.data();
  w.verticesPosByteStride = 3 * sizeof(float);
  w.verticesNormalByteStride = 3 * sizeof(float);
  w.maxVertices = maxV;
  w.numVerticesPtr = &numV;
  w.indicesBegin = ridx.data();
  w.indicesByteStride = sizeof(NxU32);
  w.maxIndices = maxI;
  w.numIndicesPtr = &numI;
  w.parentIndicesBegin = rparent.data();
  w.parentIndicesByteStride = sizeof(NxU32);
  w.maxParentIndices = maxV;
  w.numParentIndicesPtr = &numParent;
  w.dirtyBufferFlagsPtr = &dirty;
  sc.cd.clothMesh = mesh;

  NxCloth* cloth = scene->createCloth(sc.cd);
  if (!cloth) {
    printf("{\"ok\":false,\"error\":\"createCloth failed (descriptor valid=%d)\"}\n", (int)sc.cd.isValid());
    L.close();
    return 3;
  }
  if (sc.haveBounds) {
    NxBounds3 vb;
    vb.set(sc.boundsMin[0], sc.boundsMin[1], sc.boundsMin[2], sc.boundsMax[0], sc.boundsMax[1], sc.boundsMax[2]);
    cloth->setValidBounds(vb);
  }
  for (const Attach& t : sc.attaches) {
    if (t.colliding) {
      cloth->attachToCollidingShapes(t.flags);
      continue;
    }
    if (t.shape < 0 || (size_t)t.shape >= staticActors.size() || !staticActors[t.shape]) {
      fprintf(stderr, "attach shape %d: no such static shape\n", t.shape);
      continue;
    }
    cloth->attachToShape(staticActors[t.shape]->getShapes()[0], t.flags);
  }
  auto pinTarget = [&](NxU32 id) { return sc.cd.globalPose * NxVec3(&gpos[3 * id]); };  // cloth.pose moves the pins too
  for (const Pin& p : sc.pins) {
    NxU32 id = (NxU32)(p.iy * sc.nx + p.ix);
    if (id < nV) cloth->attachVertexToGlobalPosition(id, pinTarget(id));
  }
  if (sc.sleepAtStart) cloth->putToSleep();
  if (sc.jitter > 0) {
    std::vector<float> jp(gpos);
    unsigned st = sc.jitterSeed * 2654435761u + 12345u;
    for (float& x : jp) {
      st = st * 1664525u + 1013904223u;
      x += sc.jitter * ((float)(st >> 8) * (2.0f / 16777216.0f) - 1.0f);
    }
    cloth->setPositions(jp.data(), 3 * sizeof(float));
  }

  FILE* fo = fopen(argv[2], "wb");
  if (!fo) {
    printf("{\"ok\":false,\"error\":\"cannot open output\"}\n");
    return 4;
  }
  const NxU32 hdr[5] = {1, (NxU32)sc.steps, maxV, nV, nT};
  fwrite("PXCL", 1, 4, fo);
  fwrite(hdr, sizeof(hdr), 1, fo);
  fwrite(&sc.dt, sizeof(float), 1, fo);
  fwrite(gtri.data(), sizeof(NxU32), gtri.size(), fo);
  fwrite(gpos.data(), sizeof(float), gpos.size(), fo);

  std::vector<VertexOut> vbuf(maxV);
  std::vector<float> vel(3 * maxV);
  std::vector<NxU32> lastIdx;
  double total = 0, worst = 0;
  std::string rayJson = "[";
  bool nan = false;
  NxU32 particles = 0;
  for (int s = 0; s < sc.steps; ++s) {
    double t0 = nowSeconds();
    // kinematic targets: where each mover should be after this step (same convention as fluid_oracle)
    for (const Mover& mv : movers) {
      const float T = (s + 1) * sc.dt;
      mv.act->moveGlobalPosition(NxVec3(mv.centre.x + mv.v.x * T, mv.centre.y + mv.v.y * T, mv.centre.z + mv.v.z * T));
    }
    if (sc.repin)
      for (const Pin& p : sc.pins) {
        NxU32 id = (NxU32)(p.iy * sc.nx + p.ix);
        if (id < nV) cloth->attachVertexToGlobalPosition(id, pinTarget(id));
      }
    for (const ForceCmd& fc : sc.forces)
      if (fc.step == s)
        cloth->addDirectedForceAtPos(NxVec3(fc.p[0], fc.p[1], fc.p[2]), NxVec3(fc.f[0], fc.f[1], fc.f[2]), fc.r,
                                     (NxForceMode)fc.mode);
    scene->simulate(sc.dt);
    scene->flushStream();
    scene->fetchResults(NX_RIGID_BODY_FINISHED, true);
    double t = nowSeconds() - t0;
    total += t;
    if (t > worst) worst = t;
    particles = cloth->getNumberOfParticles();
    NxU32 nv = numV < maxV ? numV : maxV;
    cloth->getVelocities(vel.data(), 3 * sizeof(float));
    for (NxU32 i = 0; i < nv; ++i) {
      memcpy(vbuf[i].pos, &rpos[3 * i], 3 * sizeof(float));
      memcpy(vbuf[i].vel, i < particles ? &vel[3 * i] : &rpos[0], 3 * sizeof(float));
      if (i >= particles) vbuf[i].vel[0] = vbuf[i].vel[1] = vbuf[i].vel[2] = 0;
      for (int c = 0; c < 3; ++c)
        if (vbuf[i].pos[c] != vbuf[i].pos[c]) nan = true;
    }
    for (const RayProbe& rp : sc.rays) {
      if (rp.step != s) continue;
      NxRay ray;
      ray.orig = NxVec3(rp.o[0], rp.o[1], rp.o[2]);
      ray.dir = NxVec3(rp.d[0], rp.d[1], rp.d[2]);
      NxVec3 hit(0, 0, 0);
      NxU32 vid = 0xffffffffu;
      const bool hitOk = cloth->raycast(ray, hit, vid);
      char buf[256];
      snprintf(buf, sizeof(buf), "%s{\"step\":%d,\"hit\":%s,\"point\":[%.9g,%.9g,%.9g],\"vid\":%u}",
               rayJson.size() > 1 ? "," : "", s, hitOk ? "true" : "false", hit.x, hit.y, hit.z, (unsigned)vid);
      rayJson += buf;
    }
    NxU32 ntri = numI / 3;
    NxU32 hasIdx = (s == 0 || lastIdx.size() != numI || memcmp(lastIdx.data(), ridx.data(), numI * 4)) ? 1 : 0;
    if (hasIdx) lastIdx.assign(ridx.begin(), ridx.begin() + numI);
    fwrite(&nv, sizeof(NxU32), 1, fo);
    fwrite(&ntri, sizeof(NxU32), 1, fo);
    fwrite(&t, sizeof(double), 1, fo);
    fwrite(&hasIdx, sizeof(NxU32), 1, fo);
    if (hasIdx) fwrite(ridx.data(), sizeof(NxU32), ntri * 3, fo);
    fwrite(vbuf.data(), sizeof(VertexOut), nv, fo);
  }
  fclose(fo);
  rayJson += "]";

  printf("{\"ok\":%s,\"scenario\":\"%s\",\"steps\":%d,\"dt\":%g,\"vertices\":%u,\"triangles\":%u,"
         "\"final_vertices\":%u,\"final_triangles\":%u,\"particles\":%u,\"sleeping\":%s,\"nan\":%s,"
         "\"sim_seconds_total\":%.6f,\"sim_seconds_worst\":%.6f,\"sdk_errors\":%d,\"sdk_warnings\":%d,"
         "\"raycasts\":%s}\n",
         (!nan && out.errors == 0) ? "true" : "false", argv[1], sc.steps, sc.dt, nV, nT, numV, numI / 3, particles,
         cloth->isSleeping() ? "true" : "false", nan ? "true" : "false", total, worst, out.errors, out.warnings,
         rayJson.c_str());

  scene->releaseCloth(*cloth);
  L.sdk->releaseScene(*scene);
  L.sdk->releaseClothMesh(*mesh);
  cooking->NxCloseCooking();
  L.close();
  for (NxShapeDesc* sh : sc.statics) delete sh;
  return (!nan && out.errors == 0) ? 0 : 5;
}
