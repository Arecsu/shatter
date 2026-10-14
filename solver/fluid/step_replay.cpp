// Replays a dumped fluid step (step_dump.h) for several steps against the same static shapes and counts particles
// that pass through static geometry: a step whose motion crosses a mesh triangle from its front side to its back
// side, or ends inside a box/sphere it started outside of.
//   step_replay <dump.pxsd> [steps [--fix-boxes]]
// --fix-boxes reverses closed 12-triangle meshes whose faces all point inward (rotated boxes from shims older than
// the appendBoxTris winding fix), so old dumps can be replayed as the fixed shim would see them.
// Dynamic shapes are left out (their dumped poses are valid for one step only).
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "fluid.h"
#include "step_dump.h"

using namespace pxr;

namespace {
Vec3 cross(const Vec3& a, const Vec3& b) { return Vec3(a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x); }

bool allInward(const StaticShape& s) {
  Vec3 c;
  for (const MeshTri& t : s.tris) c += (t.v0 + t.v1 + t.v2) * (1.0f / 3);
  c = c * (1.0f / s.tris.size());
  for (const MeshTri& t : s.tris)
    if (cross(t.v1 - t.v0, t.v2 - t.v0).dot((t.v0 + t.v1 + t.v2) * (1.0f / 3) - c) > 0) return false;
  return true;
}

// Motion a -> b goes from the front side of the triangle to its back side through its interior.
bool crossesFront(const MeshTri& t, const Vec3& a, const Vec3& b) {
  const Vec3 n = cross(t.v1 - t.v0, t.v2 - t.v0);
  const float da = n.dot(a - t.v0), db = n.dot(b - t.v0);
  if (!(da > 0 && db < 0)) return false;
  const Vec3 h = a + (b - a) * (da / (da - db));
  const Vec3 c0 = cross(t.v1 - t.v0, h - t.v0), c1 = cross(t.v2 - t.v1, h - t.v1), c2 = cross(t.v0 - t.v2, h - t.v2);
  return c0.dot(n) >= 0 && c1.dot(n) >= 0 && c2.dot(n) >= 0;
}

bool insideSolid(const StaticShape& s, const Vec3& pw) {
  const Vec3 p = s.oriented ? s.frame.applyInv(pw) : pw;
  const Vec3 d = p - s.c;
  switch (s.type) {
    case StaticShape::Box: return std::fabs(d.x) < s.half.x && std::fabs(d.y) < s.half.y && std::fabs(d.z) < s.half.z;
    case StaticShape::Sphere: return d.len2() < s.r * s.r;
    case StaticShape::Capsule: {
      const float y = std::fmax(-0.5f * s.h, std::fmin(0.5f * s.h, d.y));
      return (d - Vec3(0, y, 0)).len2() < s.r * s.r;
    }
    case StaticShape::Plane: return s.n.dot(pw) < s.d;
    default: return false;
  }
}
}  // namespace

int main(int argc, char** argv) {
  if (argc < 2) {
    fprintf(stderr, "usage: step_replay <dump.pxsd> [steps [--fix-boxes]]\n");
    return 2;
  }
  StepDump D;
  if (!readStepDump(argv[1], D)) {
    fprintf(stderr, "cannot read %s\n", argv[1]);
    return 1;
  }
  const int steps = argc > 2 ? atoi(argv[2]) : 60;
  const bool fixBoxes = argc > 3 && !strcmp(argv[3], "--fix-boxes");
  int fixed = 0;
  for (StaticShape& s : D.statics) {
    if (s.type != StaticShape::Mesh) continue;
    if (fixBoxes && s.tris.size() == 12 && allInward(s)) {
      for (MeshTri& t : s.tris) std::swap(t.v1, t.v2);
      ++fixed;
    }
    buildMeshBVH(s);
  }
  std::vector<const StaticShape*> st;
  for (const StaticShape& s : D.statics) st.push_back(&s);
  Fluid fl(D.params);
  fl.addParticles(D.pos.data(), D.vel.data(), D.life.data(), (uint32_t)D.pos.size(), nullptr);
  printf("%zu particles, %zu statics (%d boxes re-wound), dt %g, %d steps\n", D.pos.size(), D.statics.size(), fixed,
         (double)D.dt, steps);
  size_t total = 0;
  for (int k = 0; k < steps; ++k) {
    const std::vector<Vec3> before = fl.positions();
    const std::vector<uint32_t> ids = fl.ids();
    fl.step(D.dt, D.gravity, st);
    // match particles by id (removal is stable, so the survivors keep their order)
    size_t through = 0, j = 0;
    for (size_t i = 0; i < fl.size(); ++i) {
      while (j < ids.size() && ids[j] != fl.ids()[i]) ++j;
      if (j == ids.size()) break;
      const Vec3 a = before[j], b = fl.positions()[i];
      bool bad = false;
      for (const StaticShape* s : st) {
        if (s->type == StaticShape::Mesh) {
          for (const MeshTri& t : s->tris)
            if (crossesFront(t, a, b)) { bad = true; break; }
        } else if (!insideSolid(*s, a) && insideSolid(*s, b)) {
          bad = true;
        }
        if (bad) break;
      }
      through += bad;
    }
    total += through;
    if (through) printf("step %d: %zu of %zu particles passed through static geometry\n", k, through, fl.size());
  }
  // height along -gravity
  const float gl = std::sqrt(D.gravity.len2());
  const Vec3 up = gl > 0 ? D.gravity * (-1.0f / gl) : Vec3(0, 0, 1);
  float lo = 1e30f;
  for (const Vec3& p : fl.positions()) lo = std::fmin(lo, up.dot(p));
  if (const char* out = getenv("REPLAY_OUT")) {  // id, end position, start position
    if (FILE* o = fopen(out, "w")) {
      for (size_t i = 0; i < fl.size(); ++i) {
        const Vec3 &p = fl.positions()[i], &q = D.pos[fl.ids()[i]];  // dump particles get ids 0..n-1
        fprintf(o, "%u %g %g %g %g %g %g\n", fl.ids()[i], p.x, p.y, p.z, q.x, q.y, q.z);
      }
      fclose(o);
    }
  }
  printf("total pass-throughs %zu; %zu particles left, lowest height %g\n", total, fl.size(), (double)lo);
  return total ? 3 : 0;
}
