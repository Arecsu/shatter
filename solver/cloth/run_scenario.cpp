// Run a cloth scenario (same format as harness/src/cloth_oracle.cpp) through our solver and write the same PXCL dump,
// so analysis/clothbin.py and analysis/cloth_metrics.py compare both directly.
//   run_scenario <scenario.txt> <out.bin> [cooked.bin]
// With cooked.bin (the oracle's cooked stream for the same grid) the edges are taken in the stream's order; the
// positions and triangles must match the scenario grid. PXC_COOKED_REST=1 also takes L0/kl0/phi0 from the stream.
// Supported: sleeping, shapes (plane, sphere, box, capsule), COM damping, tearing (v0, see cloth.cpp).
// attach shape k flags | attach colliding flags: cloth_attach.cpp (applied before the pins, as in the oracle).
// self collision: flag selfcollision 1 (cloth_self.cpp).
// Moving shapes: kinematic / dynamic / move directives (setShapeMotion per step, see cloth.h). Unsupported: pressure, adhere.
// flag validbounds + bounds x0 y0 z0 x1 y1 z1: NX_CLF_VALIDBOUNDS / setValidBounds (ClothParams::validLo/validHi).
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#include "cloth.h"
#include <algorithm>

using namespace pxc;

int main(int argc, char** argv) {
  if (argc < 3) { fprintf(stderr, "usage: run_scenario <scenario.txt> <out.bin> [cooked.bin]\n"); return 1; }
  FILE* f = fopen(argv[1], "r");
  if (!f) return 1;
  ClothParams P;
  float dt = 1.0f / 60.0f;
  int steps = 120, nx = 0, ny = 0;
  float spacing = 0.1f, o[3] = {0, 0, 0};
  bool vertical = false, tearable = false;
  uint32_t maxVertices = 0;  // mesh.maxVertices (0: the oracle's default, 2x the vertex count for a tearable mesh)
  V3 gravity(0, -9.81f, 0);
  std::vector<StaticShape> shapes;
  // moving shapes (oracle keywords kinematic / dynamic / move; dynamic bodies integrate gravity like the SDK's
  // semi-implicit Euler, pose after step n = c + v0 n dt + g dt^2 n(n+1)/2 [A]; kinvel has no native effect)
  struct MoveCmd { int shape; V3 v; };
  std::vector<MoveCmd> moves;
  std::vector<int> kinematicShapes, dynamicShapes;
  std::vector<std::pair<int, int>> pins;
  bool sleepAtStart = false, repin = false;  // putToSleep after creation; re-pin before every step (as UE3)
  struct AttachCmd { bool colliding; int shape; uint32_t flags; };
  std::vector<AttachCmd> attaches;
  struct ForceCmd { int step; V3 p, f; float r; int mode; };  // force <step> px py pz fx fy fz radius mode
  std::vector<ForceCmd> forces;
  float jitter = 0;
  unsigned jitterSeed = 1;
  char line[512];
  while (fgets(line, sizeof line, f)) {
    if (char* h = strchr(line, '#')) *h = 0;
    char key[128];
    float a[12] = {0};
    if (sscanf(line, "%127s", key) != 1) continue;
    const char* rest = line + strspn(line, " \t") + strlen(key);
    int na = sscanf(rest, "%f %f %f %f %f %f %f %f %f %f %f %f", &a[0], &a[1], &a[2], &a[3], &a[4], &a[5], &a[6],
                    &a[7], &a[8], &a[9], &a[10], &a[11]);
    std::string k = key;
    if (k == "dt") dt = a[0];
    else if (k == "steps") steps = (int)a[0];
    else if (k == "force" && na >= 9) forces.push_back({(int)a[0], V3(a[1], a[2], a[3]), V3(a[4], a[5], a[6]), a[7], (int)a[8]});
    else if (k == "gravity") gravity = V3(a[0], a[1], a[2]);
    else if (k == "cloth.thickness") P.thickness = a[0];
    else if (k == "cloth.density") P.density = a[0];
    else if (k == "cloth.bendingStiffness") P.bending = a[0];
    else if (k == "cloth.stretchingStiffness") P.stretching = a[0];
    else if (k == "cloth.dampingCoefficient") P.damping = a[0];
    else if (k == "cloth.friction") P.friction = a[0];
    else if (k == "cloth.tearFactor") P.tearFactor = a[0];
    else if (k == "cloth.attachmentResponseCoefficient") P.attachmentResponse = a[0];
    else if (k == "cloth.attachmentTearFactor") P.attachmentTearFactor = a[0];
    else if (k == "cloth.solverIterations") P.solverIterations = (uint32_t)a[0];
    else if (k == "cloth.refDt") P.refDt = a[0];  // step-rate independence (ClothParams::refDt; not an SDK field)
    else if (k == "cloth.selfContactDist") P.selfContactDist = a[0];  // particle-triangle self contact (not an SDK field)
    else if (k == "cloth.externalAcceleration") P.externalAcceleration = V3(a[0], a[1], a[2]);
    else if (k == "cloth.windAcceleration") P.windAcceleration = V3(a[0], a[1], a[2]);
    else if (k == "flag") {
      char name[64] = "";
      int v = 1;
      sscanf(rest, "%63s %d", name, &v);
      std::string n = name;
      if (n == "gravity") P.useGravity = v;
      else if (n == "disable_dynamic_ccd") P.disableDynamicCcd = v;
      else if (n == "bending") P.useBending = v;
      else if (n == "bending_ortho") P.bendingOrtho = v;
      else if (n == "damping") P.useDamping = v;
      else if (n == "comdamping") P.useComDamping = v;
      else if (n == "tearable") P.useTearing = v;
      else if (n == "selfcollision") P.useSelfCollision = v;
      else if (n == "validbounds") P.useValidBounds = v;
      else if (v) fprintf(stderr, "note: flag %s not supported (ignored)\n", name);
    } else if (k == "mesh.tearable") tearable = a[0] != 0;
    else if (k == "mesh.maxVertices") maxVertices = (uint32_t)a[0];
    else if (k == "grid" && na >= 6) {
      char axis[16] = "xz";
      sscanf(rest, "%*f %*f %*f %*f %*f %*f %15s", axis);
      nx = (int)a[0]; ny = (int)a[1]; spacing = a[2];
      o[0] = a[3]; o[1] = a[4]; o[2] = a[5];
      vertical = !strcmp(axis, "xy");
    } else if (k == "pin" && na >= 2) pins.push_back({(int)a[0], (int)a[1]});
    else if (k == "sleep") sleepAtStart = true;
    else if (k == "repin") repin = true;
    else if (k == "attach") {
      char what[16] = "";
      int sh = -1;
      unsigned fl = 0;
      if (sscanf(rest, "%15s %d %u", what, &sh, &fl) == 3 && !strcmp(what, "shape")) attaches.push_back({false, sh, fl});
      else if (sscanf(rest, "%15s %u", what, &fl) == 2 && !strcmp(what, "colliding")) attaches.push_back({true, 0, fl});
      else fprintf(stderr, "note: attach directive not understood (ignored)\n");
    }
    else if (k == "jitter" && na >= 2) { jitter = a[0]; jitterSeed = (unsigned)a[1]; }
    else if (k == "bounds" && na >= 6)
      for (int c = 0; c < 3; ++c) { P.validLo[c] = a[c]; P.validHi[c] = a[3 + c]; }
    else if (k == "kinematic" && na >= 1) kinematicShapes.push_back((int)a[0]);
    else if (k == "dynamic" && na >= 2) dynamicShapes.push_back((int)a[0]);
    else if (k == "move" && na >= 4) moves.push_back({(int)a[0], V3(a[1], a[2], a[3])});
    else if (k == "kinvel" && na >= 1) {}
    else if (k == "plane" && na >= 4) { StaticShape s{ShapeType::Plane, V3(a[0], a[1], a[2]), a[3], V3()}; shapes.push_back(s); }
    else if (k == "box" && na >= 6) { StaticShape s{ShapeType::Box, V3(a[3], a[4], a[5]), 0, V3(a[0], a[1], a[2])}; shapes.push_back(s); }
    else if (k == "capsule" && na >= 5) { StaticShape s{ShapeType::Capsule, V3(a[2], a[3], a[4]), a[0], V3(0, a[1] * 0.5f, 0)}; shapes.push_back(s); }
    else if (k == "sphere" && na >= 4) { StaticShape s{ShapeType::Sphere, V3(a[1], a[2], a[3]), a[0], V3()}; shapes.push_back(s); }
    else fprintf(stderr, "note: directive '%s' not supported (ignored)\n", key);
  }
  fclose(f);
  if (nx < 2 || ny < 2) { fprintf(stderr, "no grid\n"); return 1; }

  // grid, identical to cloth_oracle.cpp buildGrid
  std::vector<V3> pos;
  std::vector<uint32_t> tri;
  for (int iy = 0; iy < ny; ++iy)
    for (int ix = 0; ix < nx; ++ix) {
      float u = ix * spacing, v = iy * spacing;
      pos.push_back(vertical ? V3(o[0] + u, o[1] - v, o[2]) : V3(o[0] + u, o[1], o[2] + v));
    }
  for (int iy = 0; iy + 1 < ny; ++iy)
    for (int ix = 0; ix + 1 < nx; ++ix) {
      uint32_t a = iy * nx + ix, b = a + 1, c = a + nx, d = c + 1;
      if ((ix + iy) & 1) { uint32_t t[6] = {a, c, b, b, c, d}; tri.insert(tri.end(), t, t + 6); }
      else { uint32_t t[6] = {a, c, d, a, d, b}; tri.insert(tri.end(), t, t + 6); }
    }
  std::vector<V3> x0 = pos;
  if (jitter > 0) {  // same LCG as the oracle
    unsigned st = jitterSeed * 2654435761u + 12345u;
    for (V3& p : x0)
      for (float* c : {&p.x, &p.y, &p.z}) {
        st = st * 1664525u + 1013904223u;
        *c += jitter * ((float)(st >> 8) * (2.0f / 16777216.0f) - 1.0f);
      }
  }

  // optional cooked stream: edges in stored order
  CookedCloth cooked;
  const std::vector<Edge>* stored = nullptr;
  if (argc >= 4) {
    FILE* fc = fopen(argv[3], "rb");
    if (!fc) { fprintf(stderr, "cannot open %s\n", argv[3]); return 1; }
    std::vector<uint8_t> bytes;
    uint8_t buf[4096];
    for (size_t n; (n = fread(buf, 1, sizeof buf, fc)) > 0;) bytes.insert(bytes.end(), buf, buf + n);
    fclose(fc);
    if (!parseCookedCloth(bytes, cooked)) { fprintf(stderr, "cooked stream not understood: %s\n", argv[3]); return 1; }
    bool same = cooked.pos.size() == pos.size() && cooked.tri == tri;
    for (size_t i = 0; same && i < pos.size(); ++i)
      same = std::fabs(cooked.pos[i].x - pos[i].x) < 1e-6f && std::fabs(cooked.pos[i].y - pos[i].y) < 1e-6f &&
             std::fabs(cooked.pos[i].z - pos[i].z) < 1e-6f;
    if (!same) { fprintf(stderr, "cooked stream does not match the scenario grid: %s\n", argv[3]); return 1; }
    stored = &cooked.edges;
    fprintf(stderr, "note: %zu edges in cooked order\n", cooked.edges.size());
  }
  const bool storedRest = stored && getenv("PXC_COOKED_REST") && getenv("PXC_COOKED_REST")[0] == '1';
  const uint32_t maxV = maxVertices ? maxVertices : tearable ? 2 * (uint32_t)pos.size() : (uint32_t)pos.size();
  P.maxVertices = maxV;  // the oracle's receive buffer size caps tearing
  Cloth cloth(P, pos, tri, x0, stored, storedRest);  // (tearing: flag tearable + mesh.tearable)
  for (const StaticShape& s : shapes) cloth.addShape(s);
  for (const AttachCmd& c : attaches) {
    if (c.colliding) cloth.attachToCollidingShapes(c.flags);
    else cloth.attachToShape(c.shape, c.flags);
  }
  for (auto& p : pins) cloth.attachVertexToGlobalPosition(p.second * nx + p.first, pos[p.second * nx + p.first]);
  if (sleepAtStart) cloth.putToSleep();

  FILE* fo = fopen(argv[2], "wb");
  if (!fo) return 4;
  const uint32_t nV = (uint32_t)pos.size(), nT = (uint32_t)(tri.size() / 3);
  const uint32_t hdr[5] = {1, (uint32_t)steps, maxV, nV, nT};
  std::vector<uint32_t> prevTri = tri;
  fwrite("PXCL", 1, 4, fo);
  fwrite(hdr, sizeof hdr, 1, fo);
  fwrite(&dt, sizeof(float), 1, fo);
  fwrite(tri.data(), 4, tri.size(), fo);
  fwrite(pos.data(), sizeof(V3), pos.size(), fo);
  double total = 0;
  for (int s = 0; s < steps; ++s) {
    auto t0 = std::chrono::steady_clock::now();
    for (const MoveCmd& mv : moves) {
      if (mv.shape < 0 || (size_t)mv.shape >= shapes.size() || shapes[mv.shape].type == ShapeType::Plane) continue;
      const bool dyn = std::find(dynamicShapes.begin(), dynamicShapes.end(), mv.shape) != dynamicShapes.end();
      const float I[9] = {1, 0, 0, 0, 1, 0, 0, 0, 1};
      auto poseAfter = [&](int n) {  // after n steps
        V3 p = shapes[mv.shape].a + mv.v * (n * dt);
        if (dyn) p += gravity * (dt * dt * 0.5f * (float)n * (float)(n + 1));
        return p;
      };
      const V3 cur = poseAfter(s + 1), prev = poseAfter(s);
      cloth.setShapeMotion(mv.shape, cur, I, s >= 1 ? &prev : nullptr, I, dyn);
      cloth.setShapeVelocity(mv.shape, dyn ? mv.v + gravity * (dt * (float)(s + 1)) : mv.v, V3(), cur);  // friction: v - v_s
    }
    if (repin)
      for (auto& p : pins) cloth.attachVertexToGlobalPosition(p.second * nx + p.first, pos[p.second * nx + p.first]);
    for (const ForceCmd& fc : forces)
      if (fc.step == s) {  // addDirectedForceAtPos: direction and magnitude from the force vector
        const float m = std::sqrt(fc.f.len2());
        if (m > 0.f) {
          const V3 dir = fc.f * (1.f / m);
          cloth.addForceAtPos(fc.p, &dir, m, fc.r, (uint32_t)fc.mode, dt);
        }
      }
    cloth.step(dt, gravity);
    double t = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    total += t;
    const uint32_t nowV = (uint32_t)cloth.pos().size();
    uint32_t ntri = (uint32_t)(cloth.tri().size() / 3);
    uint32_t hasIdx = s == 0 || cloth.tri() != prevTri;
    if (hasIdx) prevTri = cloth.tri();
    fwrite(&nowV, 4, 1, fo);
    fwrite(&ntri, 4, 1, fo);
    fwrite(&t, 8, 1, fo);
    fwrite(&hasIdx, 4, 1, fo);
    if (hasIdx) fwrite(cloth.tri().data(), 4, cloth.tri().size(), fo);
    for (uint32_t i = 0; i < nowV; ++i) {
      fwrite(&cloth.pos()[i], 12, 1, fo);
      fwrite(&cloth.vel()[i], 12, 1, fo);
    }
  }
  fclose(fo);
  printf("{\"ok\":true,\"scenario\":\"%s\",\"steps\":%d,\"vertices\":%u,\"sim_seconds_total\":%.6f}\n", argv[1], steps, nV, total);
  return 0;
}
