// Edge-shape contact (ours, not in the original; ClothParams::edgeShapeContact). The original, like the rest of this
// solver, collides particles only: a shape thinner than the particle spacing can end up between the two particles of
// an edge, and then the particle contacts hold the cloth on it (game run 20261012-185748: a 0.21 m metal bar through
// a banner with 0.51 m spacing after a slow-motion swing; it hung there until the player punched it free).
// Per step: every edge whose two particles both have a shape contact (neither pinned) is tested against the spheres,
// capsules and boxes near it that are narrower than the edge; the side it is on at the step start is kept. An edge already through the shape takes the
// shortest way out, a little per step (pulled out at once, a banner created through a wall corner stretched 7x).
// Per solver iteration: when the edge's closest point (inside the edge) enters the shape, it is pushed back out along the kept side, split over the two particles by their inverse masses and their
// weights on the edge.
#include "cloth.h"

#include <algorithm>
#include <cmath>

namespace pxc {

namespace {

constexpr float kEdgeMargin = 1e-4f;  // m: the edge only has to stay out of the shape (see edgeShapeSolve)

V3 rotT(const float* r, const V3& v) {  // R^T v (world -> shape frame), R row-major
  return V3(r[0] * v.x + r[3] * v.y + r[6] * v.z, r[1] * v.x + r[4] * v.y + r[7] * v.z, r[2] * v.x + r[5] * v.y + r[8] * v.z);
}
V3 rot(const float* r, const V3& v) {
  return V3(r[0] * v.x + r[1] * v.y + r[2] * v.z, r[3] * v.x + r[4] * v.y + r[5] * v.z, r[6] * v.x + r[7] * v.y + r[8] * v.z);
}
float clampf(float x, float a, float b) { return std::min(b, std::max(a, x)); }

// Closest points of segment a-b and the shape's core in its frame (sphere: the centre; capsule: its axis segment; box:
// the solid box). u = parameter on a-b; qe / qs = the two points. Returns false for shape types not handled.
bool closest(const StaticShape& sh, const V3& a, const V3& b, float& u, V3& qe, V3& qs) {
  const V3 d = b - a;
  const float dd = d.dot(d);
  auto project = [&](const V3& c) { return dd > 0.f ? clampf((c - a).dot(d) / dd, 0.f, 1.f) : 0.f; };
  switch (sh.type) {
    case ShapeType::Sphere:
      qs = V3(), u = project(qs), qe = a + d * u;
      return true;
    case ShapeType::Capsule: {  // segment-segment (Ericson 5.1.9), axis = local y
      const float h = sh.half.y;
      const V3 p0(0.f, -h, 0.f), d2(0.f, 2.f * h, 0.f), r = a - p0;
      const float e = d2.dot(d2), f = d2.dot(r);
      float t;
      if (dd <= 1e-20f) u = 0.f, t = e > 0.f ? clampf(f / e, 0.f, 1.f) : 0.f;
      else {
        const float c = d.dot(r), bb = d.dot(d2), den = dd * e - bb * bb;
        u = den > 0.f ? clampf((bb * f - c * e) / den, 0.f, 1.f) : 0.f;
        t = e > 0.f ? (bb * u + f) / e : 0.f;
        if (t < 0.f) t = 0.f, u = clampf(-c / dd, 0.f, 1.f);
        else if (t > 1.f) t = 1.f, u = clampf((bb - c) / dd, 0.f, 1.f);
      }
      qe = a + d * u, qs = p0 + d2 * t;
      return true;
    }
    case ShapeType::Box: {  // alternating projection between the segment and the box (both convex)
      u = 0.5f;
      for (int it = 0; it < 8; ++it) {
        const V3 q = a + d * u;
        const V3 c(clampf(q.x, -sh.half.x, sh.half.x), clampf(q.y, -sh.half.y, sh.half.y), clampf(q.z, -sh.half.z, sh.half.z));
        const float nu = project(c);
        if (std::fabs(nu - u) < 1e-6f) { u = nu; break; }
        u = nu;
      }
      qe = a + d * u;
      qs = V3(clampf(qe.x, -sh.half.x, sh.half.x), clampf(qe.y, -sh.half.y, sh.half.y), clampf(qe.z, -sh.half.z, sh.half.z));
      return true;
    }
    default:
      return false;
  }
}

// Unit normal from the shape to the edge at the closest points; for an edge through a box, the face of the shortest
// way out of the point qe; for an edge through a capsule axis or a sphere centre, away from the core through the
// edge's middle (perpendicular to the capsule axis).
bool sideNormal(const StaticShape& sh, const V3& a, const V3& b, const V3& qe, const V3& qs, V3& n) {
  V3 g = qe - qs;
  float l = std::sqrt(g.dot(g));
  if (l > 1e-6f) { n = g * (1.f / l); return true; }
  if (sh.type == ShapeType::Box) {
    const float o[3] = {sh.half.x - std::fabs(qe.x), sh.half.y - std::fabs(qe.y), sh.half.z - std::fabs(qe.z)};
    const int k = o[0] <= o[1] && o[0] <= o[2] ? 0 : (o[1] <= o[2] ? 1 : 2);
    const float s = (k == 0 ? qe.x : k == 1 ? qe.y : qe.z) >= 0.f ? 1.f : -1.f;
    n = V3(k == 0 ? s : 0.f, k == 1 ? s : 0.f, k == 2 ? s : 0.f);
    return true;
  }
  g = (a + b) * 0.5f - qs;
  if (sh.type == ShapeType::Capsule) g.y = 0.f;
  l = std::sqrt(g.dot(g));
  if (!(l > 1e-6f)) return false;
  n = g * (1.f / l);
  return true;
}

}  // namespace

void Cloth::edgeShapeFind() {
  const float r = P_.thickness;
  if (!(r > 0.f) || bounds_.size() != shapes_.size()) return;
  // The shapes narrower than the longest edge, once per step (a banner has hundreds of shape slots, a bar or two of them
  // narrow; scanning all of them for every edge doubled the cloth's step time).
  float maxL0 = 0.f;
  for (const Edge& e : edges_) maxL0 = std::max(maxL0, e.L0);
  std::vector<uint32_t>& narrow = ehNarrow_;
  narrow.clear();
  ehBox_.clear();
  for (size_t k = 0; k < shapes_.size(); ++k) {
    const StaticShape& sh = shapes_[k];
    if (sh.type != ShapeType::Sphere && sh.type != ShapeType::Capsule && sh.type != ShapeType::Box) continue;
    const float width = sh.type == ShapeType::Box ? 2.f * std::min(sh.half.x, std::min(sh.half.y, sh.half.z)) : 2.f * sh.d;
    if (!(width < maxL0)) continue;
    narrow.push_back((uint32_t)k);
    // world box of the shape this step (posed shapes: the rotated local box; capsule: its axis box plus the radius)
    const bool moving = k < motion_.size() && motion_[k].moving;
    static const float I[9] = {1, 0, 0, 0, 1, 0, 0, 0, 1};
    const float* R = moving ? motion_[k].r : I;
    const V3 t = moving ? motion_[k].t : sh.a;
    const V3 h = sh.type == ShapeType::Box ? sh.half : sh.type == ShapeType::Capsule ? V3(sh.d, sh.half.y + sh.d, sh.d) : V3(sh.d, sh.d, sh.d);
    float lo[3], hi[3];
    const float c[3] = {t.x, t.y, t.z};
    for (int a = 0; a < 3; ++a) {  // |R| h along each world axis
      const float ext = std::fabs(R[3 * a]) * h.x + std::fabs(R[3 * a + 1]) * h.y + std::fabs(R[3 * a + 2]) * h.z;
      lo[a] = c[a] - ext, hi[a] = c[a] + ext;
    }
    for (int a = 0; a < 3; ++a) ehBox_.push_back(lo[a]);
    for (int a = 0; a < 3; ++a) ehBox_.push_back(hi[a]);
  }
  if (narrow.empty()) return;
  for (const Edge& e : edges_) {
    const uint32_t i = (uint32_t)e.i, j = (uint32_t)e.j;
    if (contacts_[i].empty() || contacts_[j].empty()) continue;  // both particles touch something (the cheap filter)
    if (hard_[i] || hard_[j]) continue;  // pins sit inside the thickness at will (particle contacts skip them too)
    if (anyRemoved_ && (removed_[i] || removed_[j])) continue;
    const V3 mi = p_[i] - x_[i], mj = p_[j] - x_[j];
    const float move = std::sqrt(std::max(mi.dot(mi), mj.dot(mj))) + kEdgeMargin;
    float lo[3], hi[3];  // the edge's box over the step (start and prediction), grown by its movement
    const V3 pts[4] = {x_[i], x_[j], p_[i], p_[j]};
    for (int a = 0; a < 3; ++a) {
      lo[a] = 1e30f, hi[a] = -1e30f;
      for (const V3& q : pts) {
        const float c = a == 0 ? q.x : a == 1 ? q.y : q.z;
        lo[a] = std::min(lo[a], c - move), hi[a] = std::max(hi[a], c + move);
      }
    }
    for (size_t nk = 0; nk < narrow.size(); ++nk) {
      const float* B = &ehBox_[6 * nk];
      if (hi[0] < B[0] || lo[0] > B[3] || hi[1] < B[1] || lo[1] > B[4] || hi[2] < B[2] || lo[2] > B[5]) continue;
      const uint32_t k = narrow[nk];
      const StaticShape& sh = shapes_[k];
      // Only shapes an edge can straddle: narrower than its rest length. Particle contacts already keep a cloth over
      // anything wider (the drape tests against the original stay bit-identical); over spheres and capsules wider than
      // the spacing this pass only pushed chords out during impacts, where the original lets them dip in (drop_sphere
      // 0.001 -> 0.21 m off the original).
      const float width = sh.type == ShapeType::Box ? 2.f * std::min(sh.half.x, std::min(sh.half.y, sh.half.z)) : 2.f * sh.d;
      if (!(width < e.L0)) continue;
      const bool moving = k < motion_.size() && motion_[k].moving;
      static const float I[9] = {1, 0, 0, 0, 1, 0, 0, 0, 1};
      const float* R = moving ? motion_[k].r : I;
      const V3 t = moving ? motion_[k].t : sh.a;
      const V3 a0 = rotT(R, x_[i] - t), b0 = rotT(R, x_[j] - t);  // the side at the step start
      float u;
      V3 qe, qs, n;
      if (!closest(sh, a0, b0, u, qe, qs)) continue;
      const V3 g = qe - qs;
      const float core = sh.type == ShapeType::Box ? 0.f : sh.d;
      // Kept only if the edge can enter the shape this step: within its particles' movement over the step (plus the
      // margin) of it, at the start or at the prediction. A banner resting a thickness off a thin wall has hundreds of
      // edges near narrow level boxes; with a fixed reach they doubled its step time and none of them ever acted.
      const float lim = core + move;
      if (g.dot(g) > lim * lim) {
        const V3 a1 = rotT(R, p_[i] - t), b1 = rotT(R, p_[j] - t);
        V3 qe1, qs1;
        float u1;
        closest(sh, a1, b1, u1, qe1, qs1);
        const V3 g1 = qe1 - qs1;
        if (g1.dot(g1) > lim * lim) continue;
      }
      if (!sideNormal(sh, a0, b0, qe, qs, n)) continue;
      eh_.push_back({i, j, (int)k, n, g.dot(g) <= core * core});
    }
  }
}

void Cloth::edgeShapeSolve() {
  const float r = P_.thickness;
  static const float I[9] = {1, 0, 0, 0, 1, 0, 0, 0, 1};
  for (const EdgeHook& h : eh_) {
    const StaticShape& sh = shapes_[h.k];
    const bool moving = (size_t)h.k < motion_.size() && motion_[h.k].moving;
    const float* R = moving ? motion_[h.k].r : I;
    const V3 t = moving ? motion_[h.k].t : sh.a;
    const V3 a = rotT(R, p_[h.i] - t), b = rotT(R, p_[h.j] - t);
    float u;
    V3 qe, qs;
    closest(sh, a, b, u, qe, qs);
    const float core = sh.type == ShapeType::Box ? 0.f : sh.d;
    const V3 g = qe - qs;
    // The edge only has to stay out of the shape itself, not a thickness away: the particles already keep that, and the
    // chord between two of them passes closer over a curve or a corner. Requiring a thickness (or a 5 cm margin) there
    // stretched a banner created against a ledge 6x and moved the cloth-over-sphere/box drapes off the original by up
    // to 0.56 m; an ordinary drape never puts an edge into the shape, so with this margin it is left as it was.
    const float want = core + kEdgeMargin;
    if (g.dot(g) >= want * want) continue;  // not touching now
    if (u <= 0.02f || u >= 0.98f) continue;  // closest at a particle: its own contact covers it
    const float s = g.dot(h.n);             // separation along the kept side (negative: through to the other side)
    // An edge already through at the step start comes out by at most a tenth of the thickness per step.
    const float cap = h.through ? 0.1f * r / (float)std::max<uint32_t>(1, P_.solverIterations) : 2.f * want;
    const float push = std::min(want - s, cap);
    if (!(push > 0.f)) continue;
    const float wi = w_[h.i] * (1.f - u), wj = w_[h.j] * u;
    const float W = wi * (1.f - u) + wj * u;
    if (!(W > 0.f)) continue;
    const V3 nw = rot(R, h.n) * (push / W);
    p_[h.i] += nw * wi;
    p_[h.j] += nw * wj;
  }
}

}  // namespace pxc
