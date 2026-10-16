#include "cloth.h"

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <map>
#include <unordered_map>

#if defined(__SSE2__)
#include <emmintrin.h>
#endif

namespace pxc {

static const float kPush = 1e-4f;  // contact points are pushed 1e-4 along the normal [V, oracle resting height]

static float len(const V3& a) { return std::sqrt(a.len2()); }

// ---- cooked stream ----------------------------------------------------------------------------------------------

namespace {
// Little-endian reader with bounds checks; any overrun clears ok.
struct Reader {
  const std::vector<uint8_t>& b;
  size_t o = 0;
  bool ok = true;
  bool need(uint64_t n) {
    if (!ok || n > b.size() - o) ok = false;
    return ok;
  }
  uint32_t u32() {
    if (!need(4)) return 0;
    uint32_t v;
    std::memcpy(&v, &b[o], 4);
    o += 4;
    return v;
  }
  uint16_t u16() {
    if (!need(2)) return 0;
    uint16_t v;
    std::memcpy(&v, &b[o], 2);
    o += 2;
    return v;
  }
  float f32() {
    uint32_t v = u32();
    float f;
    std::memcpy(&f, &v, 4);
    return f;
  }
  // a count of n items of `size` bytes must fit in the rest of the stream
  bool count(uint32_t n, uint64_t size) { return need((uint64_t)n * size); }
};
}  // namespace

bool parseCookedCloth(const std::vector<uint8_t>& b, CookedCloth& out) {
  if (b.size() < 12 || std::memcmp(b.data(), "NXS", 3) != 0 || (b[3] & 1) != 1 ||
      std::memcmp(b.data() + 4, "CLTH", 4) != 0)
    return false;
  Reader r{b};
  r.o = 8;
  r.u32();                       // version (3 in the probe streams; not used)
  if (r.u32() != 1) return false;  // mode: only the layout used by the probe streams
  r.u32();                       // flags (0 here)
  r.f32();                       // (0.0 here; meaning unknown)

  const uint32_t nV = r.u32();
  if (!r.count(nV, 12)) return false;
  out.pos.resize(nV);
  for (V3& v : out.pos) { v.x = r.f32(); v.y = r.f32(); v.z = r.f32(); }

  const uint32_t nT = r.u32();
  if (!r.count(nT, 12)) return false;
  out.tri.resize(3 * (size_t)nT);
  for (uint32_t& t : out.tri) t = r.u32();

  for (int list = 0; list < 2; ++list) {  // a float list and a dword list (both empty in the probes)
    uint32_t n = r.u32();
    if (!r.count(n, 4)) return false;
    r.o += 4 * (size_t)n;
  }
  const uint32_t nPerm = r.u32();  // stored = perm[original]
  if (!r.count(nPerm, 4) || nPerm != nV) return false;
  std::vector<uint32_t> perm(nPerm), inv(nPerm, 0xffffffffu);
  for (uint32_t& v : perm) v = r.u32();
  for (uint32_t i = 0; i < nPerm; ++i) {
    if (perm[i] >= nPerm || inv[perm[i]] != 0xffffffffu) return false;
    inv[perm[i]] = i;
  }
  {
    uint32_t n = r.u32();  // one dword list (one item here)
    if (!r.count(n, 4)) return false;
    r.o += 4 * (size_t)n;
  }

  const uint32_t nGroups = r.u32();
  out.edges.clear();
  for (uint32_t g = 0; g < nGroups && r.ok; ++g) {
    r.u32();                   // group key (3 here)
    const uint32_t nMap = r.u32();  // local index -> stored index; identity in the probes
    if (!r.count(nMap, 4)) return false;
    std::vector<uint32_t> map(nMap);
    for (uint32_t& m : map) m = r.u32();
    const uint32_t nRec = r.u32();
    if (!r.count(nRec, 22)) return false;
    for (uint32_t k = 0; k < nRec; ++k) {
      uint16_t idx[4];
      for (uint16_t& x : idx) x = r.u16();
      Edge e;
      e.L0 = r.f32();
      e.kl0 = r.f32();
      e.phi0 = r.f32();
      r.u16();  // trailing word (0 in the probes; meaning unknown)
      // stored local index -> stored index -> original index
      auto orig = [&](uint16_t local) -> int {
        if (local == 0xffff) return -1;
        uint32_t st = map.empty() ? local : (local < nMap ? map[local] : 0xffffffffu);
        if (st >= nPerm) return -2;
        return (int)inv[st];
      };
      e.i = orig(idx[0]);
      e.j = orig(idx[1]);
      e.k = orig(idx[2]);
      e.l = orig(idx[3]);
      if (e.i < 0 || e.j < 0 || e.k < 0 || (e.l == -2)) return false;
      out.edges.push_back(e);
    }
  }
  return r.ok && r.o == b.size();
}

Cloth::Cloth(const ClothParams& p, const std::vector<V3>& rest, const std::vector<uint32_t>& tri,
             const std::vector<V3>& x0, const std::vector<Edge>* stored, bool storedRest)
    : P_(p), tri_(tri), x_(x0), v_(x0.size()), p_(x0.size()), pinPos_(x0.size()), w_(x0.size(), 0.f),
      hard_(x0.size(), 0), contacts_(x0.size()) {
  wakeCounter_ = P_.wakeUpCounter;
  removed_.assign(x0.size(), 0);
  // mass: |e1 x e2| * density / 6 added to each triangle vertex, w = 1/m [V]
  std::vector<float> m(rest.size(), 0.f);
  for (size_t t = 0; t + 2 < tri.size(); t += 3) {
    V3 e1 = rest[tri[t + 1]] - rest[tri[t]], e2 = rest[tri[t + 2]] - rest[tri[t]];
    float mt = len(e1.cross(e2)) * p.density / 6.0f;
    for (int c = 0; c < 3; ++c) m[tri[t + c]] += mt;
  }
  for (size_t i = 0; i < m.size(); ++i) w_[i] = m[i] > 0 ? 1.0f / m[i] : 0.f;
  mass_ = m;
  {
    std::vector<float> L;
    L.reserve(tri.size());
    for (size_t t = 0; t + 2 < tri.size(); t += 3)
      for (int c = 0; c < 3; ++c) L.push_back(len(rest[tri[t + c]] - rest[tri[t + (c + 1) % 3]]));
    if (!L.empty()) {
      std::nth_element(L.begin(), L.begin() + L.size() / 2, L.end());
      edgeMedian_ = L[L.size() / 2];
    }
  }
  rest_ = rest;
  orig_.resize(x0.size());
  for (size_t i = 0; i < orig_.size(); ++i) orig_[i] = (int)i;

  // edges in order of first appearance [A: the cooked order is unknown]; opposite vertices from adjacent triangles
  if (stored) {
    edges_ = *stored;  // cooked-stream order
  } else {
    std::map<std::pair<int, int>, int> id;
    for (size_t t = 0; t + 2 < tri.size(); t += 3)
      for (int c = 0; c < 3; ++c) {
        int a = tri[t + c], b = tri[t + (c + 1) % 3], o = tri[t + (c + 2) % 3];
        auto key = std::make_pair(std::min(a, b), std::max(a, b));
        auto it = id.find(key);
        if (it == id.end()) {
          Edge e;
          e.i = key.first; e.j = key.second; e.k = o; e.l = -1;
          id[key] = (int)edges_.size();
          edges_.push_back(e);
        } else if (edges_[it->second].l < 0) {
          edges_[it->second].l = o;
        }
      }
  }
  for (Edge& e : edges_) {
    if (stored && storedRest) continue;
    e.L0 = len(rest[e.j] - rest[e.i]);
    if (e.l >= 0) {
      e.kl0 = len(rest[e.l] - rest[e.k]);
      V3 p2 = rest[e.j] - rest[e.i], p3 = rest[e.k] - rest[e.i], p4 = rest[e.l] - rest[e.i];
      V3 c1 = p2.cross(p3), c2 = p2.cross(p4);
      float l1 = len(c1), l2 = len(c2);
      float d = (l1 > 0 && l2 > 0) ? c1.dot(c2) / (l1 * l2) : 1.f;
      e.phi0 = std::acos(std::max(-1.f, std::min(1.f, d)));
    }
  }
  buildSchedule();
}

// ---- collision ------------------------------------------------------------------------------------------------

static V3 clampBox(const V3& p, const V3& c, const V3& h) {
  return {std::max(c.x - h.x, std::min(c.x + h.x, p.x)), std::max(c.y - h.y, std::min(c.y + h.y, p.y)),
          std::max(c.z - h.z, std::min(c.z + h.z, p.z))};
}

// Contact slot per particle, as in the original (per-shape results merged through one shared t value):
//   t = -1 none; (-1,0) proximity, closer = larger t; [0,1) swept impact time, earlier wins; t >= 1 start point
//   inside shape(s): first inside result sets t = 1, later ones average position/normal and increase t (count).
// Swept hits override proximity; a proximity hit is only taken when no hit is stored yet. [V 2.8.0 decomp, 0x100f4530 /
// 0x100f4be0 / 0x100f2ff0 and the setters 0x100f2890 / 0x100f29b0 / 0x100f2aa0]
struct Cloth::ContactSlot {
  float t = -1.f;
  V3 n, c;
  int shape = -1;  // moving shape that last wrote the slot (-1: a static one), for the surface velocity of friction
  void inside(const V3& nn, const V3& cc) {  // cc without the 1e-4 push
    V3 p = cc + nn * kPush;
    if (t < 1.f) {
      t = 1.f; n = nn; c = p;
    } else {
      n = (n * t + nn) * (1.f / (t + 1.f));
      c = (c * t + p) * (1.f / (t + 1.f));
      t += 1.f;
    }
  }
  void swept(float th, const V3& nn, const V3& cc) {  // cc = impact point on the path
    if (!(t < 1.f) || th < 0.f || th >= 1.f) return;
    if (t > 0.f && th >= t) return;
    t = th; n = nn; c = cc + nn * kPush;
  }
  void prox(float dist, float L, const V3& nn, const V3& cc) {
    if (!(t < 0.f) || dist < 0.f || dist >= L) return;
    float v = -(dist / L);
    if (v >= 0.f) v = -0.0001f;
    if (!(v > t)) return;
    t = v; n = nn; c = cc + nn * kPush;
  }
};
namespace {
using Slot = Cloth::ContactSlot;
}  // namespace

// Shape tests in the shape frame, shared by static and moving shapes. x = start point, d = displacement (both in
// the frame the shape is tested in; the shape sits at its origin, with a = centre). Results go to a sink: WorldSink
// for static shapes, MotionSink for moving ones (maps the frame back to world).
namespace {
struct WorldSink {
  Slot& s;
  void inside(const V3& n, const V3& c) { s.inside(n, c); }
  void swept(float th, const V3& n, const V3& c) { s.swept(th, n, c); }
  void prox(float dist, float L, const V3& n, const V3& c) { s.prox(dist, L, n, c); }
};
inline V3 mRot(const float* r, const V3& v) {
  return {r[0] * v.x + r[1] * v.y + r[2] * v.z, r[3] * v.x + r[4] * v.y + r[5] * v.z, r[6] * v.x + r[7] * v.y + r[8] * v.z};
}
inline V3 mRotT(const float* r, const V3& v) {
  return {r[0] * v.x + r[3] * v.y + r[6] * v.z, r[1] * v.x + r[4] * v.y + r[7] * v.z, r[2] * v.x + r[5] * v.y + r[8] * v.z};
}
// Hit points and normals are mapped with the CURRENT pose (t, R). The segment itself is tested in the start
// frame (see generateContacts). Using the start pose for the points put the swept contact behind the sheet
// (dyn_sweep_v20: the sheet at z=0.32 in the oracle, 0.0 with start-pose points) [V].
struct MotionSink {
  Slot& s;
  const float* R; V3 t;
  void inside(const V3& n, const V3& c) { s.inside(mRot(R, n), mRot(R, c) + t); }
  void swept(float th, const V3& n, const V3& c) { s.swept(th, mRot(R, n), mRot(R, c) + t); }
  void prox(float dist, float L, const V3& n, const V3& c) { s.prox(dist, L, mRot(R, n), mRot(R, c) + t); }
};
// ---- triangle meshes ----
// Per triangle, as the original's sphere-vs-mesh report (0x100f53a0, same structure as the heightfield one at
// 0x100f5b20) [V structure, A details]: with r = thickness/2 and q = the triangle point closest to the start x,
//  - inside: |x - q| < r on the front side (x - q along the face normal) -> contact at q + r (x - q)/|x - q|;
//  - proximity: within 2r on the front side [A: 2r as for planes];
//  - swept: the face only when moving against its front (one-sided), the three edges and vertices from either side.
// Improvements over the original:
//  - all triangles of one mesh give ONE result (the closest inside triangle, else the earliest hit). The original counts
//    every triangle a particle is inside of as a separate shape, so a particle near the shared edge of two triangles
//    counts as stuck in several shapes, and our contact rule drops that case: cloth slipped through meshes at seams;
//  - two-sided: each triangle's front is the side the particle starts on, so a particle crosses no triangle either way.
//    One-sided, a mesh whose winding we read the wrong way round (mirrored UE3 meshes are cooked with flipped normals;
//    the flag is not visible to us) let cloth through its visible face and then held it inside: in game, a banner
//    creased into a wall and bullets pushed it in. A particle that starts inside a closed mesh stays inside.
// Earliest t in [0, 1) at which x + t d enters the ball |p - c| < R, from outside; -1 if none.
inline float sweepBall(const V3& x, const V3& d, const V3& c, float R) {
  const V3 m = x - c;
  const float A = d.len2(), B = m.dot(d), C = m.len2() - R * R;
  if (C <= 0.f || B >= 0.f || A <= 0.f) return -1.f;
  const float disc = B * B - A * C;
  if (disc < 0.f) return -1.f;
  const float t = (-B - std::sqrt(disc)) / A;
  return t >= 0.f && t < 1.f ? t : -1.f;
}
// Same against the cylinder of radius R around segment ab, hit within the segment; q = the axis point hit.
inline float sweepEdge(const V3& x, const V3& d, const V3& a, const V3& b, float R, V3& q) {
  const V3 e = b - a;
  const float ee = e.len2();
  if (ee < 1e-12f) return -1.f;
  const V3 m = x - a;
  const V3 dp = d - e * (d.dot(e) / ee), mp = m - e * (m.dot(e) / ee);
  const float A = dp.len2(), B = mp.dot(dp), C = mp.len2() - R * R;
  if (A < 1e-20f || C <= 0.f || B >= 0.f) return -1.f;
  const float disc = B * B - A * C;
  if (disc < 0.f) return -1.f;
  const float t = (-B - std::sqrt(disc)) / A;
  if (!(t >= 0.f && t < 1.f)) return -1.f;
  const float u = (m + d * t).dot(e) / ee;
  if (u < 0.f || u > 1.f) return -1.f;
  q = a + e * u;
  return t;
}
template <class Sink>
void meshTests(const TriMesh& M, float r, const V3& x, const V3& d, Sink& s) {
  const float g = 2.f * r + 1e-3f;
  const V3 e = x + d;
  const V3 lo(std::min(x.x, e.x) - g, std::min(x.y, e.y) - g, std::min(x.z, e.z) - g);
  const V3 hi(std::max(x.x, e.x) + g, std::max(x.y, e.y) + g, std::max(x.z, e.z) + g);
  const float r2 = r * r;
  float inD2 = r2, pxD2 = 4.f * r2, bestT = 2.f;
  bool in = false, px = false;
  V3 inN, inC, pxN, pxC, tN, tC;
  auto hit = [&](float th, const V3& n, const V3& c) {
    if (th >= 0.f && th < bestT) { bestT = th; tN = n; tC = c; }
  };
  M.query(lo, hi, [&](uint32_t t) {
    const V3 &a = M.v[3 * t], &b = M.v[3 * t + 1], &c = M.v[3 * t + 2];
    V3 n = (b - a).cross(c - a);
    const float nl = len(n);
    if (nl < 1e-12f) return;
    n = n * (1.f / nl);
    const V3 q = closestPointOnTriangle(x, a, b, c);
    const V3 dq = x - q;
    const float dist2 = dq.len2();
    const float side = (x - a).dot(n) >= 0.f ? 1.f : -1.f;  // the particle's side is the front (two-sided, see above)
    const V3 nf = n * side;
    const V3 dir = dist2 > 1e-18f ? dq * (1.f / std::sqrt(dist2)) : nf;
    if (dist2 < r2) {  // inside: no sweep needed for this triangle
      if (dist2 < inD2 || !in) { inD2 = dist2; inN = dir; inC = q + dir * r; in = true; }
      return;
    }
    if (dist2 < pxD2) { pxD2 = dist2; pxN = dir; pxC = q + dir * r; px = true; }
    const float dn = d.dot(nf);
    if (dn < 0.f) {  // face, moving against the particle's side
      const float th = ((a - x).dot(nf) + r) / dn;
      if (th >= 0.f && th < 1.f) {
        const V3 p = x + d * th - nf * r;  // touch point on the face plane
        if (n.dot((b - a).cross(p - a)) >= 0.f && n.dot((c - b).cross(p - b)) >= 0.f &&
            n.dot((a - c).cross(p - c)) >= 0.f)
          hit(th, nf, x + d * th);
      }
    }
    const V3* vs[3] = {&a, &b, &c};
    for (int k = 0; k < 3; ++k) {  // edges, then vertices, either side
      V3 qe;
      const float te = sweepEdge(x, d, *vs[k], *vs[(k + 1) % 3], r, qe);
      if (te >= 0.f && te < bestT) {
        const V3 pt = x + d * te, nn = pt - qe;
        const float l = len(nn);
        if (l > 1e-12f) hit(te, nn * (1.f / l), pt);
      }
      const float tv = sweepBall(x, d, *vs[k], r);
      if (tv >= 0.f && tv < bestT) {
        const V3 pt = x + d * tv;
        hit(tv, (pt - *vs[k]) * (1.f / r), pt);
      }
    }
  });
  if (in) {
    s.inside(inN, inC);
    return;
  }
  if (px) {
    const float dist = std::sqrt(pxD2);
    s.prox(dist - r, r, pxN, pxC);
  }
  if (bestT < 1.f) s.swept(bestT, tN, tC);
}

template <class Sink>
void shapeTests(const StaticShape& sh, float r, const V3& x, const V3& d, Sink& s) {
  if (sh.type == ShapeType::Mesh) {
    if (sh.mesh) meshTests(*sh.mesh, r, x, d, s);
    return;
  }
  if (sh.type == ShapeType::Plane) {
    const float d0 = sh.a.dot(x) - sh.d;  // signed start distance
    if (d0 < r) {
      s.inside(sh.a, x + sh.a * (r - d0));
      return;
    }
    const float dn = sh.a.dot(d);
    if (d0 < 2 * r) s.prox(d0 - r, r, sh.a, x + sh.a * (r - d0));  // start point within 2r
    if (dn < 0.f) {
      const float th = (r - d0) / dn;
      if (th >= 0.f && th < 1.f) s.swept(th, sh.a, x + d * th);
    }
  } else if (sh.type == ShapeType::Sphere) {
    const float R = sh.d + r;
    const V3 rel = x - sh.a;
    const float l0 = len(rel);
    if (l0 < R) {
      V3 n = l0 > 1e-9f ? rel * (1.f / l0) : V3(0, 1, 0);
      s.inside(n, sh.a + n * R);
      return;
    }
    const float A = d.len2(), B = 2 * rel.dot(d), C = rel.len2() - R * R;
    const float disc = B * B - 4 * A * C;
    if (A > 0 && disc >= 0) {
      const float th = (-B - std::sqrt(disc)) / (2 * A);
      if (th >= 0 && th < 1) {
        const V3 pt = x + d * th;
        const V3 n = (pt - sh.a) * (1.f / R);
        s.swept(th, n, pt);
      }
    }
    if (l0 - R < r && l0 > 1e-9f) {
      const V3 n = rel * (1.f / l0);
      s.prox(l0 - R, r, n, sh.a + n * R);
    }
  } else if (sh.type == ShapeType::Capsule) {
    // capsule = segment (axis = local y, ends at +-h) swept by a sphere of radius R = radius + r. The start point is
    // tested against the segment; the swept hit is the earliest of the two end caps and the side (cylinder), with the
    // side hit kept only if it lies within the segment. Normal = (point - nearest axis point) / |..|. [A: structure
    // read from 0x100f4eb0; probes capsule_cap / capsule_side]
    const float R = sh.d + r, h = sh.half.y;
    const V3 rel = x - sh.a;
    const float cy = std::max(-h, std::min(h, rel.y));
    const V3 q = rel - V3(0, cy, 0);  // start point minus its nearest axis point
    const float dist2 = q.len2();
    if (dist2 < R * R) {
      const float l = len(q);
      const V3 n = l > 1e-9f ? q * (1.f / l) : V3(0, 1, 0);
      s.inside(n, sh.a + V3(0, cy, 0) + n * R);
      return;
    }
    // earliest swept entry: top cap, bottom cap (spheres at the segment ends), then the side
    float best = 2.f;
    const V3 ends[2] = {V3(0, h, 0), V3(0, -h, 0)};
    for (const V3& e : ends) {
      const V3 ce = rel - e;  // start point relative to this end cap
      const float A = d.len2(), B = ce.dot(d), C = ce.len2() - R * R;
      const float disc = B * B - A * C;
      if (A > 0 && disc >= 0) {
        const float th = (-B - std::sqrt(disc)) / A;
        if (th >= 0 && th < 1 && th < best) best = th;
      }
    }
    {
      const float A = d.x * d.x + d.z * d.z, B = rel.x * d.x + rel.z * d.z, C = rel.x * rel.x + rel.z * rel.z - R * R;
      const float disc = B * B - A * C;
      if (A > 1e-12f && disc >= 0) {
        const float th = (-B - std::sqrt(disc)) / A;
        const float yt = rel.y + th * d.y;
        if (th >= 0 && th < 1 && th < best && yt > -h && yt < h) best = th;
      }
    }
    if (best < 1.f) {
      const V3 pt = x + d * best;
      const V3 rp = pt - sh.a;
      const float cp = std::max(-h, std::min(h, rp.y));
      V3 qn = rp - V3(0, cp, 0);
      const float ln = len(qn);
      if (ln > 1e-9f) qn = qn * (1.f / ln); else qn = V3(0, 1, 0);
      s.swept(best, qn, pt);
    }
    // proximity: start point within r of the expanded capsule surface (same rule as the sphere)
    const float l = len(q);
    if (l - R < r && l > 1e-9f) s.prox(l - R, r, q * (1.f / l), sh.a + V3(0, cy, 0) + q * (R / l));
  } else {
    // box: sharp box expanded by r per axis (not rounded). Normals are axis-aligned face normals.
    const V3 ex = sh.half + V3(r, r, r);
    const V3 rel = x - sh.a;
    if (std::fabs(rel.x) < ex.x && std::fabs(rel.y) < ex.y && std::fabs(rel.z) < ex.z) {
      // start inside: nearest face along the axis with the smallest depth (ties: x, then y, then z)
      float dxm = ex.x - std::fabs(rel.x), dym = ex.y - std::fabs(rel.y), dzm = ex.z - std::fabs(rel.z);
      V3 n, c = x;
      if (dxm <= dym && dxm <= dzm) {
        n = V3(rel.x < 0 ? -1.f : 1.f, 0, 0);
        c.x = sh.a.x + n.x * ex.x;
      } else if (dym <= dzm) {
        n = V3(0, rel.y < 0 ? -1.f : 1.f, 0);
        c.y = sh.a.y + n.y * ex.y;
      } else {
        n = V3(0, 0, rel.z < 0 ? -1.f : 1.f);
        c.z = sh.a.z + n.z * ex.z;
      }
      s.inside(n, c);
      return;
    }
    // proximity: start point outside the expanded box, distance to it below r
    const V3 q0 = clampBox(x, sh.a, ex);
    const V3 dq = x - q0;
    const float dist = len(dq);
    if (dist < r && dist > 1e-9f) s.prox(dist, r, dq * (1.f / dist), q0);
    // swept: slab test of the segment against the expanded box; entry axis gives the normal
    float t0 = -3.4e38f, t1 = 3.4e38f;
    int ax = -1;
    bool hit = true;
    const float xs[3] = {rel.x, rel.y, rel.z}, ds[3] = {d.x, d.y, d.z}, es[3] = {ex.x, ex.y, ex.z};
    for (int a = 0; a < 3 && hit; ++a) {
      if (std::fabs(ds[a]) < 1e-12f) {
        if (std::fabs(xs[a]) >= es[a]) hit = false;
      } else {
        float ta = (-es[a] - xs[a]) / ds[a], tb = (es[a] - xs[a]) / ds[a];
        if (ta > tb) std::swap(ta, tb);
        if (ta > t0) { t0 = ta; ax = a; }
        t1 = std::min(t1, tb);
      }
    }
    if (hit && t0 <= t1 && t1 >= 0.f && t0 < 1.f && ax >= 0) {
      V3 n;
      float dd = ds[ax];
      if (ax == 0) n.x = dd > 0 ? -1.f : 1.f;
      else if (ax == 1) n.y = dd > 0 ? -1.f : 1.f;
      else n.z = dd > 0 ? -1.f : 1.f;
      s.swept(t0, n, x + d * t0);
    }
  }
}
}  // namespace

void Cloth::setShapeMotion(int shape, const V3& t, const float R[9], const V3* prevT, const float* prevR, bool dynamicBody) {
  if (shape < 0) return;
  if ((size_t)shape >= motion_.size()) motion_.resize(shape + 1);
  Motion& m = motion_[shape];
  m.moving = true;
  m.t = t;
  std::copy(R, R + 9, m.r);
  m.hasPrev = prevT && prevR;
  m.dynamic = dynamicBody;
  if (m.hasPrev) {
    m.pt = *prevT;
    std::copy(prevR, prevR + 9, m.pr);
  }
  setShapePose(shape, t, R);  // attachments follow the same pose
}

void Cloth::setShapeVelocity(int shape, const V3& lin, const V3& ang, const V3& com) {
  if (shape < 0) return;
  if ((size_t)shape >= motion_.size()) motion_.resize(shape + 1);
  Motion& m = motion_[shape];
  m.hasVel = lin.x != 0.f || lin.y != 0.f || lin.z != 0.f || ang.x != 0.f || ang.y != 0.f || ang.z != 0.f;
  m.lin = lin, m.ang = ang, m.com = com;
}

long Cloth::edgesThroughShapes(std::map<int, long>* perShape, std::vector<std::pair<int, int>>* which) const {
  if (bounds_.size() != shapes_.size()) return 0;
  long n = 0;
  for (const Edge& e : edges_) {
    if (anyRemoved_ && (removed_[e.i] || removed_[e.j])) continue;
    ContactSlot a, b;
    collide(x_[e.i], x_[e.j], 0.f, false, a);
    if (!(a.t >= 0.f && a.t < 1.f)) continue;  // no surface crossed (or the start is inside)
    collide(x_[e.j], x_[e.i], 0.f, false, b);
    if (!(b.t >= 0.f && b.t < 1.f)) continue;
    ++n;
    if (perShape) ++(*perShape)[a.shape];
    if (which) which->emplace_back(e.i, e.j);
  }
  return n;
}

// Contact search for one particle path x -> pe against every shape (generateContacts computed bounds_ for this step).
// meshesOnly: triangle meshes only (the post-solve sweep, see step).
void Cloth::collide(const V3& x, const V3& pe, float r, bool meshesOnly, ContactSlot& s, CollideScratch* cs) const {
  const V3 d = pe - x;
  const float seg[3][2] = {{std::min(x.x, pe.x), std::max(x.x, pe.x)},
                           {std::min(x.y, pe.y), std::max(x.y, pe.y)},
                           {std::min(x.z, pe.z), std::max(x.z, pe.z)}};
  auto one = [&](size_t k) {
    const StaticShape& sh = shapes_[k];
    if (meshesOnly && sh.type != ShapeType::Mesh) return;
    if (k >= motion_.size() || !motion_[k].moving) {
      const ShapeBound& b = bounds_[k];
      if (b.cull) {
        bool miss = false;
        for (int a = 0; a < 3 && !miss; ++a) miss = seg[a][1] < b.lo[a] || seg[a][0] > b.hi[a];
        if (miss) return;
      }
      WorldSink ws{s};
      const float t0 = s.t;
      shapeTests(sh, r, x, d, ws);
      if (s.t != t0) s.shape = -1;  // every write changes t
      return;
    }
    // Moving shape (setShapeMotion). Start point in the previous pose, end point in the current pose, and the
    // shape is tested with the relative segment in its own frame. Without a previous pose, or with
    // NX_CLF_DISABLE_DYNAMIC_CCD, only the current pose is used. [A rule; see docs/re-notes-cloth.md "Moving shapes"]
    // Culling as for static shapes, with a ball of radius rho around the pose origin (the shape is centred there):
    // a segment that stays outside it gets no result. Without a sweep the local segment is the world one moved
    // rigidly, so a world box test around m.t rejects first, before any rotation (posed level boxes: the game's
    // oriented statics, dozens per cloth, were tested in full for every vertex).
    const Motion& m = motion_[k];
    const bool swept = m.hasPrev && m.dynamic && !P_.disableDynamicCcd;
    const ShapeBound& b = bounds_[k];
    if (b.cull && !swept) {
      const float c[3] = {m.t.x, m.t.y, m.t.z};
      bool miss = false;
      for (int a = 0; a < 3 && !miss; ++a) miss = seg[a][1] < c[a] - b.rho || seg[a][0] > c[a] + b.rho;
      if (miss) return;
    }
    const float* Rp = swept ? m.pr : m.r;
    const V3 tp = swept ? m.pt : m.t;
    const V3 xl = mRotT(Rp, x - tp), el = mRotT(m.r, pe - m.t);
    if (b.cull) {  // exact segment-ball distance in the shape frame
      const V3 dl = el - xl;
      const float dd = dl.dot(dl);
      const float u = dd > 0.f ? std::min(1.f, std::max(0.f, -xl.dot(dl) / dd)) : 0.f;
      const V3 q = xl + dl * u;
      if (q.dot(q) > b.rho * b.rho) return;
    }
    StaticShape loc = sh;
    if (loc.type != ShapeType::Plane) loc.a = V3();  // moving shapes are centred on their pose origin
    MotionSink ms{s, m.r, m.t};
    const float t0 = s.t;
    shapeTests(loc, r, xl, el - xl, ms);
    // Friction takes the shape's surface velocity for kinematic shapes and swept dynamic ones; a dynamic body tested at
    // its current pose only (NX_CLF_DISABLE_DYNAMIC_CCD) is treated as at rest [A: fitted, oracle dyn_sweep_v20n 8e-5
    // with v_s = 0, 0.57 with v_s; dyn_sweep_v20 6e-5 with v_s, 0.95 without; dyn_capsule_sweep (kinematic) 0.27 -> 0.05]
    if (s.t != t0) s.shape = (!m.dynamic || swept) ? (int)k : -1;
  };
  if (!shapeTree_) {
    for (size_t k = 0; k < shapes_.size(); ++k) one(k);
    return;
  }
  // Shapes near the path from the tree, plus the ones it does not hold, in index order: the same visits as the plain
  // loop minus shapes it would have culled anyway.
  // Swept bodies first rejected 4 at a time by the ball test one() would make (identical arithmetic, so identical
  // decisions); the tree's hits are sorted and merged with shapeAlways_ (sorted) instead of sorting the whole list.
  CollideScratch& scr = cs ? *cs : collideScratch_;
  std::vector<uint8_t>& skip = scr.skip;
  skip.assign(shapeAlways_.size(), 0);
  const size_t nsw = sweptAt_.size();
  for (size_t g = 0; g < nsw; g += 4) {
    const float* L = &swept4_[g * 25];  // field f of lane j at L[f * 4 + j]
    int rej = 0;
#if defined(__SSE2__)
    auto F = [&](int f) { return _mm_loadu_ps(L + f * 4); };
    const __m128 X0 = _mm_set1_ps(x.x), X1 = _mm_set1_ps(x.y), X2 = _mm_set1_ps(x.z);
    const __m128 E0 = _mm_set1_ps(pe.x), E1 = _mm_set1_ps(pe.y), E2 = _mm_set1_ps(pe.z);
    const __m128 a0 = _mm_sub_ps(X0, F(18)), a1 = _mm_sub_ps(X1, F(19)), a2 = _mm_sub_ps(X2, F(20));  // x - tp
    const __m128 b0 = _mm_sub_ps(E0, F(21)), b1 = _mm_sub_ps(E1, F(22)), b2 = _mm_sub_ps(E2, F(23));  // pe - t
    auto rotT = [&](int o, __m128 v0, __m128 v1, __m128 v2, int row) {  // (R^T v)[row] = R[row] v0 + R[3+row] v1 + R[6+row] v2
      return _mm_add_ps(_mm_add_ps(_mm_mul_ps(F(o + row), v0), _mm_mul_ps(F(o + 3 + row), v1)), _mm_mul_ps(F(o + 6 + row), v2));
    };
    const __m128 xl0 = rotT(0, a0, a1, a2, 0), xl1 = rotT(0, a0, a1, a2, 1), xl2 = rotT(0, a0, a1, a2, 2);
    const __m128 el0 = rotT(9, b0, b1, b2, 0), el1 = rotT(9, b0, b1, b2, 1), el2 = rotT(9, b0, b1, b2, 2);
    const __m128 d0 = _mm_sub_ps(el0, xl0), d1 = _mm_sub_ps(el1, xl1), d2 = _mm_sub_ps(el2, xl2);
    const __m128 dd = _mm_add_ps(_mm_add_ps(_mm_mul_ps(d0, d0), _mm_mul_ps(d1, d1)), _mm_mul_ps(d2, d2));
    const __m128 xd = _mm_add_ps(_mm_add_ps(_mm_mul_ps(xl0, d0), _mm_mul_ps(xl1, d1)), _mm_mul_ps(xl2, d2));
    const __m128 zero = _mm_setzero_ps();
    const __m128 neg = _mm_xor_ps(xd, _mm_set1_ps(-0.f));
    const __m128 uc = _mm_min_ps(_mm_max_ps(_mm_div_ps(neg, dd), zero), _mm_set1_ps(1.f));
    const __m128 u = _mm_and_ps(_mm_cmpgt_ps(dd, zero), uc);
    const __m128 q0 = _mm_add_ps(xl0, _mm_mul_ps(d0, u)), q1 = _mm_add_ps(xl1, _mm_mul_ps(d1, u)), q2 = _mm_add_ps(xl2, _mm_mul_ps(d2, u));
    const __m128 qq = _mm_add_ps(_mm_add_ps(_mm_mul_ps(q0, q0), _mm_mul_ps(q1, q1)), _mm_mul_ps(q2, q2));
    rej = _mm_movemask_ps(_mm_cmpgt_ps(qq, F(24)));
#else
    for (int j = 0; j < 4; ++j) {
      auto f = [&](int k) { return L[k * 4 + j]; };
      const float Rp[9] = {f(0), f(1), f(2), f(3), f(4), f(5), f(6), f(7), f(8)};
      const float R[9] = {f(9), f(10), f(11), f(12), f(13), f(14), f(15), f(16), f(17)};
      const V3 xl = mRotT(Rp, x - V3(f(18), f(19), f(20))), el = mRotT(R, pe - V3(f(21), f(22), f(23)));
      const V3 dl = el - xl;
      const float dd = dl.dot(dl);
      const float u = dd > 0.f ? std::min(1.f, std::max(0.f, -xl.dot(dl) / dd)) : 0.f;
      const V3 q = xl + dl * u;
      if (q.dot(q) > f(24)) rej |= 1 << j;
    }
#endif
    for (int j = 0; j < 4 && g + j < nsw; ++j)
      if (rej & (1 << j)) skip[sweptAt_[g + j]] = 1;
  }
  std::vector<uint32_t>& cand = scr.cand;
  cand.clear();
  queryBoxTree(shapeNodes_, shapeOrder_, V3(seg[0][0], seg[1][0], seg[2][0]), V3(seg[0][1], seg[1][1], seg[2][1]),
               [&](uint32_t i) { cand.push_back(shapeIdx_[i]); });
  std::sort(cand.begin(), cand.end());
  size_t a = 0, c = 0;
  const size_t na = shapeAlways_.size(), nc = cand.size();
  while (a < na || c < nc) {
    if (c == nc || (a < na && shapeAlways_[a] < cand[c])) {
      if (!skip[a]) one(shapeAlways_[a]);
      ++a;
    } else {
      one(cand[c++]);
    }
  }
}

void Cloth::generateContacts() {
  const float r = P_.thickness * 0.5f;
  // Static shapes: a box that contains every point where shapeTests can report a contact for this radius, so a
  // particle whose segment (start, predicted position) misses it gets no result from that shape. Exact rejection: the
  // margin (1e-3, far above float rounding at scene scale) keeps every test that could report a contact. Planes and
  // moving shapes are never culled. Inside: x within half+r; proximity: within half+2r; swept: segment within half+r.
  const float M = 1e-3f;
  bounds_.resize(shapes_.size());
  for (size_t k = 0; k < shapes_.size(); ++k) {
    const StaticShape& sh = shapes_[k];
    ShapeBound& b = bounds_[k];
    b.cull = sh.type != ShapeType::Plane;
    if (!b.cull) continue;
    if (sh.type == ShapeType::Mesh) {  // the tree's root box (shape frame), grown like the others; rho about the origin
      const float e = 2.f * r + M;
      if (!sh.mesh || sh.mesh->nodes.empty()) {
        for (int a = 0; a < 3; ++a) b.lo[a] = 1e30f, b.hi[a] = -1e30f;
        b.rho = -1.f;
        continue;
      }
      const TriMesh::Node& root = sh.mesh->nodes[0];
      const float lo[3] = {root.lo.x - e, root.lo.y - e, root.lo.z - e}, hi[3] = {root.hi.x + e, root.hi.y + e, root.hi.z + e};
      float m2 = 0.f;
      for (int a = 0; a < 3; ++a) {
        b.lo[a] = lo[a];
        b.hi[a] = hi[a];
        const float far = std::max(std::fabs(lo[a]), std::fabs(hi[a]));
        m2 += far * far;
      }
      b.rho = std::sqrt(m2);
      continue;
    }
    V3 h;  // half extents of the culling box, centred on sh.a
    if (sh.type == ShapeType::Sphere) h = V3(sh.d, sh.d, sh.d);
    else if (sh.type == ShapeType::Capsule) h = V3(sh.d, sh.half.y + sh.d, sh.d);
    else h = sh.half;
    const float e = 2.f * r + M;
    const float c[3] = {sh.a.x, sh.a.y, sh.a.z}, hh[3] = {h.x + e, h.y + e, h.z + e};
    b.rho = std::sqrt(hh[0] * hh[0] + hh[1] * hh[1] + hh[2] * hh[2]);  // corner of the grown box, any rotation
    for (int a = 0; a < 3; ++a) {
      b.lo[a] = c[a] - hh[a];
      b.hi[a] = c[a] + hh[a];
    }
  }
  // Shape tree for this step (poses of moving shapes change every step). Static shapes by their culling box, moving
  // ones tested at their current pose by the ball of radius rho around it; planes, swept bodies and unbounded shapes
  // are visited by every particle.
  shapeTree_ = shapes_.size() > 16;
  if (shapeTree_) {
    shapeLo_.clear();
    shapeHi_.clear();
    shapeIdx_.clear();
    shapeAlways_.clear();
    sweptAt_.clear();
    for (size_t k = 0; k < shapes_.size(); ++k) {
      const ShapeBound& b = bounds_[k];
      const bool moving = k < motion_.size() && motion_[k].moving;
      if (!b.cull) {
        shapeAlways_.push_back((uint32_t)k);
        continue;
      }
      if (!moving) {
        shapeLo_.push_back(V3(b.lo[0], b.lo[1], b.lo[2]));
        shapeHi_.push_back(V3(b.hi[0], b.hi[1], b.hi[2]));
      } else {
        const Motion& m = motion_[k];
        if (m.hasPrev && m.dynamic && !P_.disableDynamicCcd) {
          sweptAt_.push_back((uint32_t)shapeAlways_.size());
          shapeAlways_.push_back((uint32_t)k);
          continue;
        }
        // The world box of the posed culling box (rotated half extents), not the ball of radius rho: the game feeds
        // its oriented level boxes (long walls) through setShapeMotion every step, and their balls reached every
        // particle of a banner hanging next to them (39 shapes per particle, nearly all rejected by the test in
        // collide()). Exact like the static boxes: contacts only come from inside the grown box, and the pad covers
        // the rounding of the rotation at scene scale.
        V3 cl, hl;
        if (shapes_[k].type == ShapeType::Mesh) {
          cl = V3((b.lo[0] + b.hi[0]) * 0.5f, (b.lo[1] + b.hi[1]) * 0.5f, (b.lo[2] + b.hi[2]) * 0.5f);
          hl = V3((b.hi[0] - b.lo[0]) * 0.5f, (b.hi[1] - b.lo[1]) * 0.5f, (b.hi[2] - b.lo[2]) * 0.5f);
        } else {
          hl = V3((b.hi[0] - b.lo[0]) * 0.5f, (b.hi[1] - b.lo[1]) * 0.5f, (b.hi[2] - b.lo[2]) * 0.5f);
        }
        const V3 cw = m.t + mRot(m.r, cl);
        const float* R = m.r;
        const float pad = M + 1e-6f * (std::fabs(cw.x) + std::fabs(cw.y) + std::fabs(cw.z) + hl.x + hl.y + hl.z);
        const V3 hw(std::fabs(R[0]) * hl.x + std::fabs(R[1]) * hl.y + std::fabs(R[2]) * hl.z + pad,
                    std::fabs(R[3]) * hl.x + std::fabs(R[4]) * hl.y + std::fabs(R[5]) * hl.z + pad,
                    std::fabs(R[6]) * hl.x + std::fabs(R[7]) * hl.y + std::fabs(R[8]) * hl.z + pad);
        shapeLo_.push_back(cw - hw);
        shapeHi_.push_back(cw + hw);
      }
      shapeIdx_.push_back((uint32_t)k);
    }
    buildBoxTree(shapeLo_, shapeHi_, shapeNodes_, shapeOrder_);
    // lanes of the swept bodies (padding lanes: a ball no segment can reach, rho^2 = -1)
    const size_t nsw = sweptAt_.size(), ng = (nsw + 3) / 4;
    swept4_.assign(ng * 4 * 25, 0.f);
    for (size_t g = 0; g < ng; ++g) {
      float* L = &swept4_[g * 100];
      for (int j = 0; j < 4; ++j) {
        if (g * 4 + j >= nsw) {
          L[24 * 4 + j] = -1.f;
          continue;
        }
        const uint32_t k = shapeAlways_[sweptAt_[g * 4 + j]];
        const Motion& m = motion_[k];
        for (int e = 0; e < 9; ++e) L[e * 4 + j] = m.pr[e], L[(9 + e) * 4 + j] = m.r[e];
        L[18 * 4 + j] = m.pt.x, L[19 * 4 + j] = m.pt.y, L[20 * 4 + j] = m.pt.z;
        L[21 * 4 + j] = m.t.x, L[22 * 4 + j] = m.t.y, L[23 * 4 + j] = m.t.z;
        L[24 * 4 + j] = bounds_[k].rho * bounds_[k].rho;
      }
    }
  }
  // Particles are independent: chunks on the pool, each with its own scratch.
  const size_t K = 256, nch = (x_.size() + K - 1) / K;
  if (contactScratch_.size() < nch) contactScratch_.resize(nch);
  auto chunk = [&](size_t ci) {
    const size_t end = std::min(x_.size(), (ci + 1) * K);
    for (size_t i = ci * K; i < end; ++i) {
      auto& cs = contacts_[i];
      cs.clear();
      if (hard_[i] || isAttachedToShape(i)) continue;  // attached vertices take no contacts
      Slot s;
      collide(x_[i], p_[i], r, false, s, &contactScratch_[ci]);
      if (s.t > -1.f && s.t <= 1.f) cs.push_back({s.n, s.n.dot(s.c), s.shape});
    }
  };
  parallel(nch, chunk);
}

// ---- constraints ----------------------------------------------------------------------------------------------

static inline void distanceConstraint(V3& pi, V3& pj, float wi, float wj, float rest, float s) {
  float ws = wi + wj;
  if (ws <= 0) return;
  V3 d = pj - pi;
  float L = len(d);
  if (L <= 0) return;
  float C = L - rest;
  if (std::fabs(C) < FLT_EPSILON) C = 0;
  C *= s;
  V3 n = d * (1.f / L);
  pi += n * (C * wi / ws);
  pj -= n * (C * wj / ws);
}

void Cloth::bendDihedral(const Edge& e, float s) {
  // Dihedral-angle constraint, Mueller et al. 2007 appendix A, with the original's mass weighting (w_m / sum w)
  // and a REAL acos (the original uses a truncated series, which is an artifact).
  V3& p1 = p_[e.i];
  V3 p2 = p_[e.j] - p1, p3 = p_[e.k] - p1, p4 = p_[e.l] - p1;
  V3 c1 = p2.cross(p3), c2 = p2.cross(p4);
  float l1 = len(c1), l2 = len(c2);
  if (l1 < 1e-12f || l2 < 1e-12f) return;
  V3 n1 = c1 * (1.f / l1), n2 = c2 * (1.f / l2);
  float d = std::max(-1.f, std::min(1.f, n1.dot(n2)));
  float sq = std::sqrt(std::max(0.f, 1 - d * d));
  if (sq < 1e-6f) return;
  V3 q3 = (p2.cross(n2) + n1.cross(p2) * d) * (1.f / l1);
  V3 q4 = (p2.cross(n1) + n2.cross(p2) * d) * (1.f / l2);
  V3 q2 = -((p3.cross(n2) + n1.cross(p3) * d) * (1.f / l1)) - ((p4.cross(n1) + n2.cross(p4) * d) * (1.f / l2));
  V3 q1 = -(q2 + q3 + q4);
  const int idx[4] = {e.i, e.j, e.k, e.l};
  const V3 q[4] = {q1, q2, q3, q4};
  float sumQ = 0, sumW = 0;
  for (int m = 0; m < 4; ++m) {
    sumQ += q[m].len2();
    sumW += w_[idx[m]];
  }
  if (sumQ < 1e-12f || sumW <= 0) return;
  float sc = s * sq * (std::acos(d) - e.phi0) / sumQ / sumW;
  for (int m = 0; m < 4; ++m) p_[idx[m]] -= q[m] * (w_[idx[m]] * sc);
}

// Edge schedule. Two edges that share no vertex commute exactly (each reads and writes only its own vertices), so the
// sequential sweep over edges_ gives the same bits in any order that keeps every pair of conflicting edges in its
// original order. Level(e) = 1 + the largest level of an earlier edge sharing a vertex with e (vertices i, j, k, l);
// sedges_ lists the edges stably sorted by level, so neighbours in sedges_ are independent and the CPU can overlap them.
// Rebuilt whenever edges_ changes (constructor, rebuildEdges).
void Cloth::buildSchedule() {
  std::vector<uint32_t> lastLevel(x_.size(), 0), level(edges_.size());
  uint32_t maxLevel = 0;
  for (size_t e = 0; e < edges_.size(); ++e) {
    const Edge& ed = edges_[e];
    const int vs[4] = {ed.i, ed.j, ed.k, ed.l};
    uint32_t lv = 0;
    for (int v : vs)
      if (v >= 0) lv = std::max(lv, lastLevel[v]);
    lv += 1;
    for (int v : vs)
      if (v >= 0) lastLevel[v] = lv;
    level[e] = lv;
    maxLevel = std::max(maxLevel, lv);
  }
  std::vector<size_t> at(maxLevel + 2, 0);
  for (uint32_t lv : level) ++at[lv + 1];
  for (size_t k = 1; k < at.size(); ++k) at[k] += at[k - 1];
  sedges_.resize(edges_.size());
  levelStart_.assign(at.begin() + 1, at.end());  // at[lv] = start of level lv (levels start at 1)
  for (size_t e = 0; e < edges_.size(); ++e) sedges_[at[level[e]]++] = edges_[e];
}

#if defined(__SSE2__)
// distanceConstraint on 4 vertex pairs at once (all 8 vertices distinct). Same operations in the same order per lane
// (sqrtps/divps round like the scalar ops); a lane whose scalar version returns early keeps its positions, and lanes
// with on = false are left alone too.
static inline void distanceConstraint4(V3* p, const float* w, const int* I, const int* J, const float* rest, float s,
                                       const bool* on) {
  alignas(16) float a[3][4], b[3][4], wi[4], wj[4], r[4];
  for (int t = 0; t < 4; ++t) {
    const V3 &pi = p[I[t]], &pj = p[J[t]];
    a[0][t] = pi.x, a[1][t] = pi.y, a[2][t] = pi.z, b[0][t] = pj.x, b[1][t] = pj.y, b[2][t] = pj.z;
    wi[t] = w[I[t]], wj[t] = w[J[t]], r[t] = rest[t];
  }
  const __m128 zero = _mm_setzero_ps();
  const __m128 vwi = _mm_load_ps(wi), vwj = _mm_load_ps(wj), ws = _mm_add_ps(vwi, vwj);
  const __m128 ax = _mm_load_ps(a[0]), ay = _mm_load_ps(a[1]), az = _mm_load_ps(a[2]);
  const __m128 bx = _mm_load_ps(b[0]), by = _mm_load_ps(b[1]), bz = _mm_load_ps(b[2]);
  const __m128 dx = _mm_sub_ps(bx, ax), dy = _mm_sub_ps(by, ay), dz = _mm_sub_ps(bz, az);
  const __m128 L = _mm_sqrt_ps(_mm_add_ps(_mm_add_ps(_mm_mul_ps(dx, dx), _mm_mul_ps(dy, dy)), _mm_mul_ps(dz, dz)));
  int m = _mm_movemask_ps(_mm_and_ps(_mm_cmpnle_ps(ws, zero), _mm_cmpnle_ps(L, zero)));  // !(ws <= 0) && !(L <= 0)
  for (int t = 0; t < 4; ++t)
    if (!on[t]) m &= ~(1 << t);
  if (!m) return;
  __m128 C = _mm_sub_ps(L, _mm_load_ps(r));
  const __m128 absC = _mm_and_ps(C, _mm_castsi128_ps(_mm_set1_epi32(0x7fffffff)));
  C = _mm_andnot_ps(_mm_cmplt_ps(absC, _mm_set1_ps(FLT_EPSILON)), C);  // |C| < eps -> +0, as C = 0
  C = _mm_mul_ps(C, _mm_set1_ps(s));
  const __m128 inv = _mm_div_ps(_mm_set1_ps(1.f), L);
  const __m128 nx = _mm_mul_ps(dx, inv), ny = _mm_mul_ps(dy, inv), nz = _mm_mul_ps(dz, inv);
  const __m128 ki = _mm_div_ps(_mm_mul_ps(C, vwi), ws), kj = _mm_div_ps(_mm_mul_ps(C, vwj), ws);
  _mm_store_ps(a[0], _mm_add_ps(ax, _mm_mul_ps(nx, ki))), _mm_store_ps(a[1], _mm_add_ps(ay, _mm_mul_ps(ny, ki))),
      _mm_store_ps(a[2], _mm_add_ps(az, _mm_mul_ps(nz, ki)));
  _mm_store_ps(b[0], _mm_sub_ps(bx, _mm_mul_ps(nx, kj))), _mm_store_ps(b[1], _mm_sub_ps(by, _mm_mul_ps(ny, kj))),
      _mm_store_ps(b[2], _mm_sub_ps(bz, _mm_mul_ps(nz, kj)));
  for (int t = 0; t < 4; ++t)
    if (m >> t & 1) {
      p[I[t]] = V3(a[0][t], a[1][t], a[2][t]);
      p[J[t]] = V3(b[0][t], b[1][t], b[2][t]);
    }
}
#endif

void Cloth::solveEdges(float sStretch, float sBend) {
  if (anyAttachTear_) {  // tearable attachments can free vertices inside the sweep: keep the original order
    for (const Edge& e : edges_) {
      attachTearEdge(e);
      distanceConstraint(p_[e.i], p_[e.j], w_[e.i], w_[e.j], e.L0, sStretch);
      if (P_.useBending && e.l >= 0) {
        if (P_.bendingOrtho) bendDihedral(e, sBend);
        else distanceConstraint(p_[e.k], p_[e.l], w_[e.k], w_[e.l], e.kl0, sBend);
      }
    }
    return;
  }
  const bool bend = P_.useBending, ortho = P_.bendingOrtho;
#if defined(__SSE2__)
  // Edges of one level share no vertex (i, j, k, l), so 4 of them are solved at once: their stretch constraints, then
  // their bending distances, gives what the one-by-one order gives. The dihedral bend and level tails stay scalar.
  if (!ortho) {
    size_t e = 0;
    for (size_t lv = 0; lv + 1 < levelStart_.size(); ++lv) {
      const size_t end = levelStart_[lv + 1];
      for (; e + 4 <= end; e += 4) {
        int I[4], J[4], K[4], Lv[4];
        float r[4], rb[4];
        bool on[4] = {true, true, true, true}, onB[4];
        for (int t = 0; t < 4; ++t) {
          const Edge& ed = sedges_[e + t];
          I[t] = ed.i, J[t] = ed.j, r[t] = ed.L0;
          onB[t] = bend && ed.l >= 0;
          K[t] = ed.k, Lv[t] = onB[t] ? ed.l : ed.k, rb[t] = ed.kl0;
        }
        distanceConstraint4(p_.data(), w_.data(), I, J, r, sStretch, on);
        if (onB[0] | onB[1] | onB[2] | onB[3]) {
          for (int t = 0; t < 4; ++t)  // lanes without a bend need valid, distinct indices: reuse their own i, j
            if (!onB[t]) K[t] = I[t], Lv[t] = J[t];
          distanceConstraint4(p_.data(), w_.data(), K, Lv, rb, sBend, onB);
        }
      }
      for (; e < end; ++e) {
        const Edge& ed = sedges_[e];
        distanceConstraint(p_[ed.i], p_[ed.j], w_[ed.i], w_[ed.j], ed.L0, sStretch);
        if (bend && ed.l >= 0) distanceConstraint(p_[ed.k], p_[ed.l], w_[ed.k], w_[ed.l], ed.kl0, sBend);
      }
    }
    return;
  }
#endif
  for (const Edge& e : sedges_) {  // schedule order (see buildSchedule); attachTearEdge is a no-op here
    distanceConstraint(p_[e.i], p_[e.j], w_[e.i], w_[e.j], e.L0, sStretch);
    if (bend && e.l >= 0) {
      if (ortho) bendDihedral(e, sBend);
      else distanceConstraint(p_[e.k], p_[e.l], w_[e.k], w_[e.l], e.kl0, sBend);
    }
  }
}

// Rigid-motion damping (2.8.0 0x100df6e0): about the mass centre of all particles, split the velocity into a rigid part
// vc + w x r (vc = mass-weighted mean velocity, w = I^-1 L from the angular momentum L = sum m r x v and the inertia
// I = sum m (|r|^2 E - r r^T)) and the rest. The rest is scaled by (1 - damping); the rigid part is kept.
// Masses are the stored (non-zero) masses, so pinned particles still count. [V structure from the decompile; probes]
void Cloth::comDamping(float keep) {
  const size_t n = x_.size();
  const uint8_t* rm = anyRemoved_ ? removed_.data() : nullptr;  // removed particles are in none of the three loops
  float M = 0;
  V3 xc, vc;
  for (size_t i = 0; i < n; ++i) {
    if (rm && rm[i]) continue;
    const float m = mass_[i];
    M += m;
    xc += x_[i] * m;
    vc += v_[i] * m;
  }
  if (!(M > 0)) return;
  xc = xc * (1.f / M);
  vc = vc * (1.f / M);
  V3 L;
  float I[3][3] = {{0, 0, 0}, {0, 0, 0}, {0, 0, 0}};
  for (size_t i = 0; i < n; ++i) {
    if (rm && rm[i]) continue;
    const V3 r = x_[i] - xc;
    const float m = mass_[i];
    L += r.cross(v_[i]) * m;
    const float rr = r.len2();
    const float rv[3] = {r.x, r.y, r.z};
    for (int a = 0; a < 3; ++a)
      for (int b = 0; b < 3; ++b) I[a][b] += m * ((a == b ? rr : 0.f) - rv[a] * rv[b]);
  }
  // w = I^-1 L by cofactors; a singular inertia (points on a line) gives w = 0
  const float c00 = I[1][1] * I[2][2] - I[1][2] * I[2][1];
  const float c01 = I[1][2] * I[2][0] - I[1][0] * I[2][2];
  const float c02 = I[1][0] * I[2][1] - I[1][1] * I[2][0];
  const float det = I[0][0] * c00 + I[0][1] * c01 + I[0][2] * c02;
  V3 w;
  if (std::fabs(det) > 1e-30f) {
    const float inv = 1.f / det;
    const float c10 = I[0][2] * I[2][1] - I[0][1] * I[2][2], c11 = I[0][0] * I[2][2] - I[0][2] * I[2][0],
                c12 = I[0][1] * I[2][0] - I[0][0] * I[2][1];
    const float c20 = I[0][1] * I[1][2] - I[0][2] * I[1][1], c21 = I[0][2] * I[1][0] - I[0][0] * I[1][2],
                c22 = I[0][0] * I[1][1] - I[0][1] * I[1][0];
    w.x = inv * (c00 * L.x + c10 * L.y + c20 * L.z);
    w.y = inv * (c01 * L.x + c11 * L.y + c21 * L.z);
    w.z = inv * (c02 * L.x + c12 * L.y + c22 * L.z);
  }
  for (size_t i = 0; i < n; ++i) {
    if (rm && rm[i]) continue;
    const V3 r = x_[i] - xc;
    const V3 rigid = vc + w.cross(r);
    v_[i] = rigid + (v_[i] - rigid) * keep;
  }
}

// Valid bounds (NX_CLF_VALIDBOUNDS, 2.8.0 0x100e7820 / fix-up 0x100e13f0; spec in docs/audit/GAPS.md, row 4 [V]). After the
// step, before tearing, a particle strictly outside the box (x < lo or x > hi on an axis) is removed, for good. The empty
// default box removes everything. A removed particle keeps its place (its prediction is x, so v = 0 from the next step;
// this step's v and friction stay, as in the original [V: vb_fall, vb_swing]) and is left out of contacts, pins and
// attachments, COM and edge damping, friction, wind normals, self contact, the mesh sweep and tearing (the filters sit at
// those places). The original's fix-up switches constraints off; here the edges are rebuilt from the triangles whose three
// corners are alive, and a particle left in no alive triangle is removed too. Masses are unchanged. The one difference: an
// edge with both ends alive but both triangles dead still damps in the original.
void Cloth::validBoundsStep() {
  const float *lo = P_.validLo, *hi = P_.validHi;
  size_t fresh = 0;
  for (size_t i = 0; i < x_.size(); ++i) {
    if (removed_[i]) continue;
    const V3& q = x_[i];
    if (!(q.x < lo[0] || q.x > hi[0] || q.y < lo[1] || q.y > hi[1] || q.z < lo[2] || q.z > hi[2])) continue;
    removed_[i] = 1;  // v and contacts stay for this step's output and friction (the original tests after friction)
    ++fresh;
  }
  if (!fresh) return;
  removedCount_ += fresh;
  anyRemoved_ = removedDirty_ = true;
}

void Cloth::setRemoved(const std::vector<uint8_t>& m) {
  removedCount_ = 0;
  for (size_t i = 0; i < removed_.size(); ++i) {
    removed_[i] = i < m.size() && m[i];
    if (removed_[i]) ++removedCount_, contacts_[i].clear();
  }
  anyRemoved_ = removedCount_ > 0;
  removedDirty_ = anyRemoved_;
}

// force: the removed set changed, so the edges are rebuilt; without it (first step of a flagged cloth) they are rebuilt
// only if a particle in no triangle got removed.
void Cloth::removedFixup(bool force) {
  removedDirty_ = false;
  removedChecked_ = true;
  std::vector<uint8_t> used(x_.size(), 0);
  for (size_t t = 0; t + 2 < tri_.size(); t += 3)
    if (!triDead(t))
      for (int c = 0; c < 3; ++c) used[tri_[t + c]] = 1;
  for (size_t i = 0; i < x_.size(); ++i)
    if (!removed_[i] && !used[i]) removed_[i] = 1, ++removedCount_, force = true;  // dead triangles are already out of the edges
  anyRemoved_ = removedCount_ > 0;
  if (force) rebuildEdges();
}

// Tearing (NX_CLF_TEARABLE). Detection follows 2.8.0 0x100edb90: edges in array order, on the positions after the update;
// an edge tears when |x_i - x_j| >= tearFactor * L0 and neither end is hard-pinned. [V: onset in tear_flag and tear_5x5]
// Split (0x100ea530): the triangles around v are cut into arcs by the plane through x_v with normal d = x_i - x_j
// (side = centroid . d > x_v . d). Triangles that share an edge (v, w) and are on the same side form one arc. The first
// arc (in triangle order) keeps v; every other arc gets a copy of v (same state, appended) that takes its corners.
// [V: tear_5x5 step 1, copy of 5 takes [5,10,6]]. The torn edge is not cut: it moves with its triangles.
// Edges keep their array order; after each split the moved edges are rewired in place (rebuildEdges), so later edges
// in the same step see the new topology. Not replicated [A/open]: the 0x8 priority and 0x800 flags, the parity choice
// when both ends are eligible (we always take the first end), and the extra copy seen in tear_5x5 at step 2 (probably
// the original's follow-up pass: after a split it splits each rim vertex again with no plane, i.e. by connectivity only).
bool Cloth::tearStep(bool* purgedOut) {
  bool changed = false;
  const bool rmAny = anyRemoved_;
  // The tear loop reads the rewired edges after each split, but not the solve schedule or the masses: those are
  // rebuilt once after the last split (same values as after every split).
  deferEdgeUpkeep_ = true;
  // Triangles of each vertex in buffer order, built at the first candidate (a stretched edge that cannot split, one arc,
  // used to scan every triangle on every step) and kept up to date by each split (rebuilding them per split cost ~20 us).
  std::vector<std::vector<uint32_t>> fans;
  bool fanValid = false;
  std::vector<std::vector<int>> fam;  // current vertices of each rest vertex in index order (splitEdges), built at need
  PairSlots slotOf;  // pair -> edge slot (splitEdges), built at the first incremental split
  std::vector<uint64_t> pOld, pNew;
  bool purged = false;  // a full rebuild in this pass took this step's removed particles out of the edges
  std::vector<int> dead;  // edge slots splitEdges dropped (dead edges, i < 0) since the last compaction
  size_t scanE = 0, scanPos = 0, scanDead = 0;  // the scan below: its position, its slot, dead.size() at its start
  auto compact = [&] {  // the dead edges out, the others in order: the array erasing them one by one would have left
    if (dead.empty()) return;
    edges_.erase(std::remove_if(edges_.begin(), edges_.end(), [](const Edge& e) { return e.i < 0; }), edges_.end());
    dead.clear();
  };
  auto buildFans = [&] {
    fans.assign(x_.size(), {});
    for (size_t t = 0; t + 2 < tri_.size(); t += 3)
      for (int k = 0; k < 3; ++k)
        if (k == 0 || (tri_[t + k] != tri_[t] && (k == 1 || tri_[t + 2] != tri_[t + 1]))) fans[tri_[t + k]].push_back((uint32_t)t);
    fanValid = true;
  };
  if (tip_.size() != x_.size()) tip_.resize(x_.size(), 0);
  // Split v (0x100ea530): its triangles in buffer order are grouped into arcs and every arc but the first gets a copy of
  // v. With a tear direction d the triangles are sided by the plane through x_v normal to d; without one (the crack-tip
  // pass below) all are on one side, so only a fan that no longer hangs together (a vertex the cut reached from both
  // sides, or a border vertex the cut ran into) comes apart. Attached vertices never split. tips (when given) receives
  // the far ends of the edges of v that the split opened (their two triangles now sit on different copies), in edge
  // order: the crack tips.
  std::vector<uint32_t> tipOrder;
  auto splitVertex = [&](uint32_t v, const V3* dp, std::vector<uint32_t>* tips) -> bool {
    if (hard_[v] || isAttachedToShape(v)) return false;
    if (!fanValid) buildFans();
    const V3 d = dp ? *dp : V3();
    const float ref = x_[v].dot(d);
    std::vector<size_t> ts;
    std::vector<int> side;
    for (const uint32_t t : fans[v]) {
      const V3 c = (x_[tri_[t]] + x_[tri_[t + 1]] + x_[tri_[t + 2]]) * (1.f / 3.f);
      ts.push_back(t);
      side.push_back(!dp || c.dot(d) > ref ? 1 : -1);
    }
    const size_t nt = ts.size();
    // arcs: union of fan triangles that share a rim vertex w != v and the same side
    std::vector<size_t> par(nt);
    for (size_t a = 0; a < nt; ++a) par[a] = a;
    auto root = [&](size_t a) {
      while (par[a] != a) a = par[a] = par[par[a]];
      return a;
    };
    auto sharesRim = [&](size_t a, size_t b) {
      for (int k = 0; k < 3; ++k) {
        const uint32_t w = tri_[ts[a] + k];
        if (w == v) continue;
        for (int m = 0; m < 3; ++m)
          if (tri_[ts[b] + m] == w) return true;
      }
      return false;
    };
    for (size_t a = 0; a < nt; ++a)
      for (size_t b = a + 1; b < nt; ++b)
        if (side[a] == side[b] && sharesRim(a, b)) par[root(a)] = root(b);
    std::vector<int> label(nt, -1), grp(nt, 0);  // label by arc root, numbered in triangle order
    int ng = 0;
    for (size_t a = 0; a < nt; ++a) {
      const size_t r = root(a);
      if (label[r] < 0) label[r] = ng++;
      grp[a] = label[r];
    }
    if (ng < 2) return false;  // one arc: nothing to separate
    // The original skips a split whose copies would not fit the receive buffers ("Could not tear cloth; buffer full!",
    // 0x100ea530: count + copies <= cap) and goes on with the next edge. [V: decompile; oracle mesh.maxVertices]
    if (P_.maxVertices && x_.size() + (size_t)(ng - 1) > P_.maxVertices) return false;
    if (tips) {  // rim vertices whose edge with v had its two triangles put on different arcs
      tips->clear();
      std::vector<std::pair<uint32_t, int>> seen;  // rim vertex, arc of its first triangle with v
      for (size_t a = 0; a < nt; ++a)
        for (int k = 0; k < 3; ++k) {
          const uint32_t w = tri_[ts[a] + k];
          if (w == v) continue;
          bool found = false;
          for (auto& sw : seen)
            if (sw.first == w) {
              found = true;
              if (sw.second != grp[a] && std::find(tips->begin(), tips->end(), w) == tips->end()) tips->push_back(w);
            }
          if (!found) seen.push_back({w, grp[a]});
        }
      if (tips->size() > 1) {  // edge order, as the original walks its edge records
        tipOrder.clear();
        for (const Edge& e2 : edges_) {
          const int o = e2.i == (int)v ? e2.j : e2.j == (int)v ? e2.i : -1;
          if (o >= 0 && std::find(tips->begin(), tips->end(), (uint32_t)o) != tips->end() &&
              std::find(tipOrder.begin(), tipOrder.end(), (uint32_t)o) == tipOrder.end())
            tipOrder.push_back((uint32_t)o);
        }
        for (uint32_t w : *tips)
          if (std::find(tipOrder.begin(), tipOrder.end(), w) == tipOrder.end()) tipOrder.push_back(w);
        tips->swap(tipOrder);
      }
    }
    tip_[v] = 0;  // the split vertex loses its tip mark; its copies start without one
    // copies of v for arcs 1..ng-1 (indices appended in arc order), then the corners of those arcs move to them
    const uint32_t base = (uint32_t)x_.size();
    for (int g = 1; g < ng; ++g) {
      x_.push_back(x_[v]);
      v_.push_back(v_[v]);
      p_.push_back(p_[v]);
      pinPos_.push_back(pinPos_[v]);
      w_.push_back(w_[v]);
      mass_.push_back(mass_[v]);
      hard_.push_back(0);
      removed_.push_back(0);
      tip_.push_back(0);
      if (trackParents_) parent_.push_back(v);
      contacts_.emplace_back();
      orig_.push_back(orig_[v]);
    }
    auto pairsOfFan = [&](std::vector<uint64_t>& out) {
      out.clear();
      for (size_t a = 0; a < nt; ++a)
        for (int k = 0; k < 3; ++k) {
          const uint32_t p = tri_[ts[a] + k], q = tri_[ts[a] + (k + 1) % 3];
          if (p != q) out.push_back(((uint64_t)std::min(p, q) << 32) | std::max(p, q));
        }
    };
    if (edgesRebuilt_) pairsOfFan(pOld);
    for (size_t a = 0; a < nt; ++a)
      if (grp[a] > 0)
        for (int k = 0; k < 3; ++k)
          if (tri_[ts[a] + k] == v) tri_[ts[a] + k] = base + (uint32_t)grp[a] - 1;
    splits_ += ng - 1;
    changed = true;
    // fans: v keeps the first arc's triangles, each copy takes its arc's (ts is v's fan, so buffer order is kept)
    fans.resize(x_.size());
    size_t keep = 0;
    for (size_t a = 0; a < nt; ++a)
      if (grp[a] > 0) fans[base + (uint32_t)grp[a] - 1].push_back((uint32_t)ts[a]);
      else fans[v][keep++] = (uint32_t)ts[a];
    fans[v].resize(keep);
    if (!fam.empty())
      for (int g = 1; g < ng; ++g) fam[(size_t)orig_[v]].push_back((int)(base + (uint32_t)g - 1));
    // Rewire the moved edges in place, so the later edges see the new topology. The first rebuild of a cloth runs in
    // full (the constructor's edges may hold stored rest values); later ones only touch this vertex's triangles.
    // Particles removed this step (valid bounds) are still in the edges: the first split rebuilds in full, which purges
    // them; after that the edges match the alive triangles and the incremental path (which skips dead ones) applies.
    if (!edgesRebuilt_ || (removedDirty_ && !purged)) {
      compact();
      rebuildEdges();
      scanPos = scanE, scanDead = 0;  // the array is compact again: position e is slot e
      purged = true;
      slotOf.clear();
    } else {
      if (slotOf.empty())
        for (size_t k = 0; k < edges_.size(); ++k)
          slotOf.set(((uint64_t)(uint32_t)std::min(edges_[k].i, edges_[k].j) << 32) |
                         (uint32_t)std::max(edges_[k].i, edges_[k].j), (int)k);
      if (fam.empty()) {
        int maxOrig = 0;
        for (int o : orig_) maxOrig = std::max(maxOrig, o);
        fam.resize((size_t)maxOrig + 1);
        for (size_t i = 0; i < x_.size(); ++i) fam[(size_t)orig_[i]].push_back((int)i);
      }
      pairsOfFan(pNew);
      splitEdges(pOld, pNew, fans, fam, slotOf, dead);
    }
    return true;
  };
  std::vector<uint32_t> tips, tips2;
  // Detection (0x100edb90): edges in array order (only those that existed when the pass started), on the positions after
  // the solve. An edge tears at |x_i - x_j| >= tearFactor * L0 unless an end is removed. The split end is a crack tip
  // (flag 0x8) when there is one, i first, else i; an attached end refuses and nothing tears there. After a split the
  // tips of the opened edges get the mark (unless attached) and are split without a direction, so a crack runs on from
  // its tip along a line instead of starting new cuts next to it. [V: decompile of 0x100edb90 / 0x100ea530; the
  // original's "i or j by parity" reads the low bit of a pointer, always i]
  // The scan visits the array as it would be with every dropped edge erased as soon as it was dropped (the original's
  // order): position e counts alive edges only, and slot pc is the alive edge at position e. A split can drop edges
  // before pc (position e then lies further along), drop pc itself, and append at the end.
  const size_t nEdges = edges_.size();
  for (size_t e = 0, pc = 0; e < nEdges && e < edges_.size() - dead.size(); ++e) {
    // every split makes at least one copy: with the buffer full nothing more can tear (see the cap in splitVertex)
    if (P_.maxVertices && x_.size() >= P_.maxVertices) break;
    const Edge ed = edges_[pc];
    scanE = e, scanPos = pc, scanDead = dead.size();
    if (!(rmAny && (removed_[ed.i] || removed_[ed.j]))) {  // skip an edge with a removed end (edges not yet purged)
      V3 d = x_[ed.i] - x_[ed.j];
      const float L = P_.tearFactor * ed.L0;
      const uint32_t v = tip_[ed.i] ? (uint32_t)ed.i : tip_[ed.j] ? (uint32_t)ed.j : (uint32_t)ed.i;
      if (d.len2() >= L * L && splitVertex(v, &d, &tips)) {
        changed = true;
        for (uint32_t w : tips) {
          if (!hard_[w] && !isAttachedToShape(w)) tip_[w] = 1;
          splitVertex(w, nullptr, &tips2);
          for (uint32_t w2 : tips2)
            if (!hard_[w2] && !isAttachedToShape(w2)) tip_[w2] = 1;
        }
      }
    }
    // to position e + 1: alive edges before scanPos number e minus those dropped there in this pass (a full rebuild
    // compacts the array and puts scanPos at e)
    pc = scanPos;
    size_t before = e;
    for (size_t k = scanDead; k < dead.size(); ++k) before -= (size_t)dead[k] < pc;
    while (pc < edges_.size() && (before < e + 1 || edges_[pc].i < 0)) {
      if (edges_[pc].i >= 0) ++before;
      ++pc;
    }
  }
  compact();
  deferEdgeUpkeep_ = false;
  if (changed) buildSchedule(), recomputeMass();
  if (purgedOut) *purgedOut = purged;
  return changed;
}

// Edge array rebuilt from the current triangles in the stored order: an old edge whose pair still has a triangle keeps
// its slot; an old edge whose pair was moved by a split is rewired to the copy (same rest vertex) that shares a triangle
// with the other end. Pairs that are new (none so far) are appended. Rest lengths from each end's rest vertex;
// k/l (opposite vertices) from the adjacent triangles. Masses are recomputed.
void Cloth::rebuildEdges() {
  // Pairs (min, max) with the first two opposite vertices in triangle order, sorted by pair. Sorted vectors instead of
  // maps: the same key order, first occurrence and emit order as the map-based version, without node allocations.
  struct Adj { int a, b, o1, o2; };  // a < b; o1, o2 = opposite vertices (o2 = -1 on border)
  std::vector<Adj> half;
  half.reserve(tri_.size());
  for (size_t t = 0; t + 2 < tri_.size(); t += 3) {
    if (anyRemoved_ && triDead(t)) continue;  // removed particles: only the alive triangles make edges
    for (int k = 0; k < 3; ++k) {
      const int x = (int)tri_[t + k], y = (int)tri_[t + (k + 1) % 3], o = (int)tri_[t + (k + 2) % 3];
      half.push_back({std::min(x, y), std::max(x, y), o, -1});
    }
  }
  // Sorted by (a, b), stably (triangle order within a pair): bucketed by a in triangle order, then each short bucket
  // stably sorted by b. The same order as one stable sort of all half-edges, at a fraction of its cost; adjStart (the
  // first pair of each a) makes a lookup a scan of a's few pairs instead of a binary search over all of them.
  const size_t nv = x_.size();
  std::vector<uint32_t> hStart(nv + 1, 0);
  for (const Adj& h : half) ++hStart[(size_t)h.a + 1];
  for (size_t v = 0; v < nv; ++v) hStart[v + 1] += hStart[v];
  std::vector<Adj> sorted(half.size());
  {
    std::vector<uint32_t> fill(hStart.begin(), hStart.end() - 1);
    for (const Adj& h : half) sorted[fill[(size_t)h.a]++] = h;
  }
  std::vector<Adj> adj;
  adj.reserve(half.size());
  std::vector<uint32_t> adjStart(nv + 1, 0);
  for (size_t v = 0; v < nv; ++v) {
    Adj* b0 = sorted.data() + hStart[v];
    Adj* b1 = sorted.data() + hStart[v + 1];
    std::stable_sort(b0, b1, [](const Adj& p, const Adj& q) { return p.b < q.b; });
    for (const Adj* h = b0; h < b1; ++h) {
      if (adj.size() == adjStart[v] || adj.back().b != h->b) adj.push_back({h->a, h->b, h->o1, -1});
      else if (adj.back().o2 < 0) adj.back().o2 = h->o1;
    }
    adjStart[v + 1] = (uint32_t)adj.size();
  }
  half.clear();
  auto find = [&](int a, int b) -> int {  // index in adj of pair {a, b}, or -1
    const int lo = std::min(a, b), hi = std::max(a, b);
    if (lo < 0 || (size_t)hi >= nv) return -1;
    for (uint32_t k = adjStart[(size_t)lo]; k < adjStart[(size_t)lo + 1]; ++k)
      if (adj[k].b == hi) return (int)k;
    return -1;
  };
  // current vertices of each rest vertex (the original and its copies), in index order: CSR by rest vertex
  int maxOrig = 0;
  for (int o : orig_) maxOrig = std::max(maxOrig, o);
  std::vector<size_t> famStart((size_t)maxOrig + 2, 0);
  for (size_t i = 0; i < x_.size(); ++i) ++famStart[(size_t)orig_[i] + 1];
  for (size_t k = 1; k < famStart.size(); ++k) famStart[k] += famStart[k - 1];
  std::vector<int> famList(x_.size());
  {
    std::vector<size_t> fill(famStart.begin(), famStart.end() - 1);
    for (size_t i = 0; i < x_.size(); ++i) famList[fill[(size_t)orig_[i]]++] = (int)i;
  }
  std::vector<char> done(adj.size(), 0);
  std::vector<Edge> out;
  std::vector<const Edge*> from;  // the old edge each new one came from (null: a new pair)
  out.reserve(adj.size());
  from.reserve(adj.size());
  const Edge* src = nullptr;
  auto emit = [&](int a, int b) {
    const int idx = find(a, b);
    if (idx < 0 || done[idx]) return;
    done[idx] = 1;
    Edge e;
    e.i = a;
    e.j = b;
    e.k = adj[idx].o1;
    e.l = adj[idx].o2;
    out.push_back(e);
    from.push_back(src);
  };
  for (const Edge& old : edges_) {
    src = &old;
    if (find(old.i, old.j) >= 0) {
      emit(old.i, old.j);
      continue;
    }
    bool found = false;
    const size_t oi = (size_t)orig_[old.i], oj = (size_t)orig_[old.j];
    for (size_t p1 = famStart[oi]; p1 < famStart[oi + 1] && !found; ++p1) {
      const int y = famList[p1];
      for (size_t p2 = famStart[oj]; p2 < famStart[oj + 1]; ++p2) {
        const int z = famList[p2];
        if (find(y, z) >= 0) {
          emit(y, z);
          found = true;
          break;
        }
      }
    }
  }
  src = nullptr;
  for (const Adj& kv : adj) emit(kv.a, kv.b);
  for (size_t q = 0; q < out.size(); ++q) {
    Edge& e = out[q];
    // Rest values depend only on the rest vertices of i, j, k, l: an edge that kept them (nearly all, after a split)
    // takes the old edge's values instead of recomputing them (the acos made each rebuild ~0.4 ms, one per split).
    // Only edges an earlier rebuild made (the constructor's may hold stored rest values).
    if (const Edge* o = edgesRebuilt_ ? from[q] : nullptr) {
      auto ro = [&](int a) { return a >= 0 ? orig_[a] : -1; };
      if (ro(o->i) == ro(e.i) && ro(o->j) == ro(e.j) && ro(o->k) == ro(e.k) && ro(o->l) == ro(e.l)) {
        e.L0 = o->L0;
        if (e.l >= 0) e.kl0 = o->kl0, e.phi0 = o->phi0;
        continue;
      }
    }
    edgeRest(e);
  }
  edges_ = out;
  edgesRebuilt_ = true;
  if (deferEdgeUpkeep_) return;  // tearStep: once after its last split
  buildSchedule();
  recomputeMass();
}

// Rest values of an edge from the rest vertices of its ends and opposite vertices (rebuildEdges, splitEdges).
void Cloth::edgeRest(Edge& e) const {
  const V3 &ri = rest_[orig_[e.i]], &rj = rest_[orig_[e.j]];
  e.L0 = len(rj - ri);
  if (e.l >= 0) {
    const V3 &rk = rest_[orig_[e.k]], &rl = rest_[orig_[e.l]];
    e.kl0 = len(rl - rk);
    V3 p2 = rj - ri, p3 = rk - ri, p4 = rl - ri;
    V3 c1 = p2.cross(p3), c2 = p2.cross(p4);
    float l1 = len(c1), l2 = len(c2);
    float dd = (l1 > 0 && l2 > 0) ? c1.dot(c2) / (l1 * l2) : 1.f;
    e.phi0 = std::acos(std::max(-1.f, std::min(1.f, dd)));
  }
}

// rebuildEdges after one split, touching only the edges of the split vertex's triangles (the tear pass splits up to
// hundreds of vertices in one step, and a full rebuild per split cost ~0.4 ms: 115 ms steps when a hit shredded a
// banner). Same result as rebuildEdges: old edges keep their slots in order (a kept pair with its new opposite vertices,
// a vanished pair rewired to the first family pair that exists, a slot whose target an earlier slot already emitted is
// dropped), then the new pairs not yet emitted, in pair order. pOld / pNew: pairs of the split vertex's triangles before
// and after the split; fans: triangles of each vertex after it, fam: current vertices of each rest vertex (tearStep).
void Cloth::splitEdges(const std::vector<uint64_t>& pOld, const std::vector<uint64_t>& pNew,
                       const std::vector<std::vector<uint32_t>>& fans, const std::vector<std::vector<int>>& fam,
                       PairSlots& slotOf, std::vector<int>& dead) {
  auto key = [](int a, int b) { return ((uint64_t)(uint32_t)std::min(a, b) << 32) | (uint32_t)std::max(a, b); };
  // the pair's first two triangles (buffer order) and their opposite vertices, as rebuildEdges' sorted half-edges
  auto adjOf = [&](int a, int b, int& o1, int& o2) {
    o1 = o2 = -1;
    int n = 0;
    for (const uint32_t ft : fans[a]) {
      if (anyRemoved_ && triDead(ft)) continue;  // as rebuildEdges: only the alive triangles make edges
      const uint32_t* t = &tri_[ft];
      int o = -1;
      bool hasB = false;
      for (int k = 0; k < 3; ++k) {
        if ((int)t[k] == b) hasB = true;
        else if ((int)t[k] != a) o = (int)t[k];
      }
      if (!hasB) continue;
      if (n == 0) o1 = o;
      else if (n == 1) o2 = o;
      ++n;
    }
    return n > 0;
  };
  std::vector<int> aff;
  for (uint64_t k : pOld) {
    const int sl = slotOf.find(k);
    if (sl >= 0) aff.push_back(sl);
  }
  std::sort(aff.begin(), aff.end());
  aff.erase(std::unique(aff.begin(), aff.end()), aff.end());
  auto isAff = [&](int sl) { return std::binary_search(aff.begin(), aff.end(), sl); };
  std::vector<uint64_t> emitted;  // pairs emitted by affected slots
  std::vector<int> drop;
  std::vector<std::pair<int, Edge>> repl;
  auto emittedHas = [&](uint64_t q) { return std::find(emitted.begin(), emitted.end(), q) != emitted.end(); };
  for (int sl : aff) {
    const Edge old = edges_[sl];
    int a = -1, b = -1, o1, o2;
    if (adjOf(old.i, old.j, o1, o2)) {
      a = old.i, b = old.j;
    } else {
      const std::vector<int>&famI = fam[(size_t)orig_[old.i]], &famJ = fam[(size_t)orig_[old.j]];
      for (size_t p1 = 0; p1 < famI.size() && a < 0; ++p1)
        for (size_t p2 = 0; p2 < famJ.size(); ++p2)
          if (adjOf(famI[p1], famJ[p2], o1, o2)) {
            a = famI[p1], b = famJ[p2];
            break;
          }
    }
    if (a < 0) {
      drop.push_back(sl);
      continue;
    }
    const uint64_t q = key(a, b);
    const int own = slotOf.find(q);
    const bool done = emittedHas(q) || (own >= 0 && own < sl && !isAff(own));
    if (done) {
      drop.push_back(sl);
      continue;
    }
    emitted.push_back(q);
    if (own >= 0 && own > sl && !isAff(own)) drop.push_back(own);
    Edge e;
    e.i = a, e.j = b, e.k = o1, e.l = o2;
    auto ro = [&](int v) { return v >= 0 ? orig_[v] : -1; };
    if (ro(old.i) == ro(e.i) && ro(old.j) == ro(e.j) && ro(old.k) == ro(e.k) && ro(old.l) == ro(e.l)) {
      e.L0 = old.L0;
      if (e.l >= 0) e.kl0 = old.kl0, e.phi0 = old.phi0;
    } else {
      edgeRest(e);
    }
    repl.push_back({sl, e});
  }
  // new pairs not emitted, in pair order
  std::vector<uint64_t> fresh(pNew);
  std::sort(fresh.begin(), fresh.end());
  fresh.erase(std::unique(fresh.begin(), fresh.end()), fresh.end());
  std::vector<int> dropped(drop);
  std::sort(dropped.begin(), dropped.end());
  std::vector<Edge> app;
  for (uint64_t q : fresh) {
    if (emittedHas(q)) continue;
    const int own = slotOf.find(q);
    if (own >= 0 && !isAff(own) && !std::binary_search(dropped.begin(), dropped.end(), own))
      continue;  // emitted by its own unaffected slot
    Edge e;
    e.i = (int)(q >> 32), e.j = (int)(uint32_t)q;
    int o1, o2;
    if (!adjOf(e.i, e.j, o1, o2)) continue;
    e.k = o1, e.l = o2;
    edgeRest(e);
    app.push_back(e);
  }
  for (const auto& r : repl) slotOf.erase(key(edges_[r.first].i, edges_[r.first].j));
  for (int sl : dropped) slotOf.erase(key(edges_[sl].i, edges_[sl].j));
  for (const auto& r : repl) edges_[r.first] = r.second;
  // Dropped slots stay in place as dead edges (i = j = -1) until tearStep compacts the array once at its end: erasing
  // them here shifted every later slot, and refilling their slot map was most of the cost of a heavy tear step. Slots
  // keep their order, so the comparisons above (own < sl, the sorted lists) mean the same as on the compacted array.
  for (int sl : dropped) edges_[sl].i = edges_[sl].j = -1, dead.push_back(sl);
  for (const auto& r : repl) slotOf.set(key(r.second.i, r.second.j), r.first);
  for (const Edge& e : app) slotOf.set(key(e.i, e.j), (int)edges_.size()), edges_.push_back(e);
}

// Mass per corner as in the constructor (|e1 x e2| density / 6 per triangle corner), inverse mass 0 for hard pins.
void Cloth::recomputeMass() {
  std::fill(mass_.begin(), mass_.end(), 0.f);
  for (size_t t = 0; t + 2 < tri_.size(); t += 3) {
    V3 e1 = rest_[orig_[tri_[t + 1]]] - rest_[orig_[tri_[t]]], e2 = rest_[orig_[tri_[t + 2]]] - rest_[orig_[tri_[t]]];
    float mt = len(e1.cross(e2)) * P_.density / 6.0f;
    for (int c = 0; c < 3; ++c) mass_[tri_[t + c]] += mt;
  }
  for (size_t i = 0; i < mass_.size(); ++i) w_[i] = (!hard_[i] && mass_[i] > 0) ? 1.0f / mass_[i] : 0.f;
}

// ---- external forces ----------------------------------------------------------------------------------------------
// 2.8.0 Cloth slots 100-102 (0x100dc040, 0x100e8560) [V: read from the binary, no oracle run yet]. Velocities change at
// the call; the next step integrates them. Particles the original skips (attached, flag 1) are the ones with no inverse
// mass here (pins, one-way attachments) [A: flag 1 = attached]. The original's extra pass for a cloth attached to a
// core (attachToCore, unsupported) is left out.

void Cloth::addForceAtVertex(size_t i, const V3& f, uint32_t mode, float dt) {
  if (i >= v_.size() || w_[i] == 0.f) return;
  switch (mode) {
    case 0: v_[i] += f * (w_[i] * dt); break;
    case 1: v_[i] += f * w_[i]; break;
    case 2: v_[i] += f; break;
    case 5: v_[i] += f * dt; break;
    default: return;
  }
  asleep_ = false;
  wakeCounter_ = P_.wakeUpCounter;
}

void Cloth::addForceAtPos(const V3& p, const V3* dir, float mag, float radius, uint32_t mode, float dt) {
  if (!(radius > 0.f) || (mode != 0 && mode != 1 && mode != 2 && mode != 5)) return;
  const float r2 = radius * radius;
  float massSum = 0.f;
  bool any = false;
  for (size_t i = 0; i < x_.size(); ++i) {
    if (w_[i] == 0.f || (x_[i] - p).len2() >= r2) continue;
    any = true;
    massSum += 1.f / w_[i];
  }
  if (!any) return;
  // Spread over the mass in reach, as the original, but never less than half the mass an intact disc of the radius
  // holds (mass per area = density; ours, not in the original). With the original's sum alone, a hit at a hole or a
  // torn edge spreads the impulse over the few light particles left there: a game bullet (480, r 0.5) then gave them
  // thousands of m/s, and they yanked the rest of the banner in ("implosion"). Intact cloth keeps the original's value.
  massSum = std::max(massSum, 0.5f * 3.14159265f * r2 * P_.density);
  float s = mag;
  if ((mode == 0 || mode == 1) && massSum != 0.f) s = mag / massSum;
  // A cloth that cannot tear (ours, not in the original): no particle is thrown further than two rest edges per step.
  // The game's bullet gives one light vertex of such a banner 1000+ m/s; the original, at the game's 62 FPS cap too,
  // stretched it 5x into a spike that snapped back (game replay cloth004: 5.2x on the same edge in both). Tearable
  // cloths keep the full hit, which is what opens holes.
  const bool capHit = !P_.useTearing && edgeMedian_ > 0.f && dt > 0.f && (mode == 1 || mode == 2);
  const float cap = capHit ? 2.f * edgeMedian_ / std::max(dt, P_.refDt) : 0.f;
  if (mode == 0 || mode == 5) s *= dt;
  for (size_t i = 0; i < x_.size(); ++i) {
    if (w_[i] == 0.f) continue;
    const V3 d = x_[i] - p;
    const float q = d.len2();
    if (q >= r2) continue;
    const V3 u = dir ? *dir : d;
    const float l = len(u);
    if (l == 0.f) continue;
    float dv = (1.f - q / r2) * s;
    if (capHit) dv = std::min(dv, cap);
    v_[i] += u * (dv / l);
  }
  if (mode == 1 || mode == 2) hitPending_ = true;  // see the reference step in step()
  asleep_ = false;
  wakeCounter_ = P_.wakeUpCounter;
}

// ---- step -----------------------------------------------------------------------------------------------------

void Cloth::step(float dt, V3 gravity) {
  const size_t n = x_.size();
  if (asleep_) {  // a moving shape that touches a particle wakes the cloth (the sleep rule ignores shapes) [A]
    // Only shapes that actually moved since the last step: posed statics (rotated level boxes) also come through
    // setShapeMotion every step, and counting them woke every sleeping cloth hanging against a wall (game respawn:
    // the original keeps them asleep, ours fell).
    bool wake = false;
    for (size_t k = 0; k < motion_.size() && !wake; ++k) {
      const Motion& m = motion_[k];
      if (!m.moving || !m.hasPrev) continue;
      bool moved = (m.t - m.pt).len2() > 0.f;
      for (int e = 0; e < 9 && !moved; ++e) moved = m.r[e] != m.pr[e];
      if (moved)
        for (size_t i = 0; i < n && !wake; ++i) wake = touching((int)k, x_[i], P_.thickness * 0.5f);
    }
    if (wake) {
      asleep_ = false;
      wakeCounter_ = P_.wakeUpCounter;
    }
  }
  if (asleep_) {  // asleep: no state change, velocities stay zero
    for (V3& v : v_) v = V3();
    return;
  }
  // Reference step after a hit (ours, step-rate independence). With steps shorter than refDt the coefficients are
  // rescaled to keep the original's look at 62 FPS, but a hit is not: the solve turns a position change into velocity
  // over the step length, so the same pull-back on a hit particle left it about 3x faster in a 5 ms step than in the
  // original's 16 ms one. Hit fragments flew off at 2-3x the original's speed and kept it (bullet scenes at 180 Hz:
  // 167 m/s and 41 particles past 50 m/s against the original's 72 and 19 at 60 Hz; 0.1 s later 132 against 32).
  // The step after an impulse hit therefore runs as one whole refDt step (prediction, full coefficients and
  // iterations, tearing, velocities over refDt), and the particles then advance only dt / refDt of its way, so time
  // keeps its pace. Same bullet scenes: 73 m/s / 18, then 34; second hit into a hole 162 m/s against 162; torn vertex
  // counts as before (307/326/386 against the original's 369/360/403 and 319/338/375 before). Tear copies start at
  // the position of the vertex they came from.
  if (hitPending_) {
    hitPending_ = false;
    if (P_.refDt > dt) {
      const std::vector<V3> x0 = x_;
      const size_t n0 = x_.size();
      parent_.clear();
      trackParents_ = true;
      step(P_.refDt, gravity);
      trackParents_ = false;
      const float f = dt / P_.refDt;
      for (size_t i = 0; i < x_.size(); ++i) {
        size_t o = i;
        while (o >= n0) o = parent_[o - n0];
        x_[i] = x0[o] + (x_[i] - x0[o]) * f;
      }
      return;
    }
  }
  if (removedDirty_) removedFixup(true);  // a restored mask (setRemoved): edges from the alive triangles
  else if (P_.useValidBounds && !removedChecked_) removedFixup(false);  // the original's fix-up on the first step
  // This step's coefficients. The original applies them per step, so the same cloth is stiffer, more damped and
  // grippier the more steps run per second. With P_.refDt they keep the meaning they have in steps of refDt:
  //  - stretch and bend as a fixed compliance (XPBD): 1/k - 1 scales with 1/r^2, r = dt / refDt, so a static sag
  //    (per step about g dt^2 (1 - k) / k) does not depend on dt; k = 1 stays rigid;
  //  - damping and friction as a fixed decay per second: the kept fraction (1 - c) becomes (1 - c)^r;
  //  - iterations: the same number per second as the original at refDt, N r per step, spread over ceil(N r) iterations by
  //    capping the per-iteration stiffness at (N r / ceil)^0.7. Game replays (Escape_p, the banner the helicopter
  //    shoots, 82 hits) against the original at 62 Hz: torn vertex counts within the run-to-run spread (~5%) from 100
  //    to 300 Hz steps and at the game's own 150-200 Hz frames. N r^2 (fitted to keep a rigid cloth's sag) left one weak
  //    iteration per step: hits spread instead of being resolved and banners tore 30-60% more; the full N per step
  //    barely tore them. Rigid sag with N r: mean strain 0.6% at 150 Hz, 0.4% at 250 Hz, 1.2% at 62 Hz.
  float stretching = P_.stretching, bending = P_.bending, damping = P_.damping;
  friction_ = P_.friction;
  uint32_t iterations = P_.solverIterations;
  float relax = 1.f;
  if (P_.refDt > 0.f && dt != P_.refDt) {
    const float r = dt / P_.refDt;
    auto stiff = [r](float k) {
      if (!(k < 1.f)) return k;
      if (!(k > 0.f)) return 0.f;
      return k / (k + (1.f - k) / (r * r));
    };
    auto decay = [r](float c) { return c >= 1.f ? c : c <= 0.f ? 0.f : 1.f - std::pow(1.f - c, r); };
    stretching = stiff(stretching);
    bending = stiff(bending);
    damping = decay(damping);
    friction_ = decay(friction_);
    if (iterations > 0) {
      const float nf = (float)iterations * r;
      iterations = std::max<uint32_t>(1, (uint32_t)std::ceil(nf - 1e-3f));
      relax = std::pow(std::min(1.f, nf / (float)iterations), 0.7f);
    }
  }
  if (P_.useSelfCollision) selfCollision(dt);  // ComputeNewState: self-collision prepass, before gravity (cloth_self.cpp)
  V3 a = P_.externalAcceleration;
  if (P_.useGravity) a += gravity;
  const bool wind = P_.windAcceleration.len2() > 0;
  std::vector<V3>& nrm = nrm_;
  if (wind) {  // area-weighted vertex normals of the current mesh [A]
    nrm.assign(n, V3());
    for (size_t t = 0; t + 2 < tri_.size(); t += 3) {
      if (anyRemoved_ && triDead(t)) continue;
      V3 fn = (x_[tri_[t + 1]] - x_[tri_[t]]).cross(x_[tri_[t + 2]] - x_[tri_[t]]);
      for (int c = 0; c < 3; ++c) nrm[tri_[t + c]] += fn;
    }
    for (V3& v : nrm) {
      float l = len(v);
      if (l > 0) v = v * (1.f / l);
    }
  }
  const bool comDamp = P_.useDamping && P_.useComDamping;
  for (size_t i = 0; i < n; ++i) {  // velocity update, then prediction (fused unless COM damping sits between them)
    V3 ai = a;
    if (wind) ai += nrm[i] * nrm[i].dot(P_.windAcceleration);
    v_[i] += ai * dt;
    if (!comDamp) p_[i] = x_[i] + v_[i] * dt;
  }
  if (comDamp) {  // ComputeNewState: COM damping replaces the edge damping
    comDamping(1.f - damping);
    for (size_t i = 0; i < n; ++i) p_[i] = x_[i] + v_[i] * dt;
  }
  if (anyRemoved_)  // a removed particle's prediction is where it is (its velocity is 0 from the end of this step)
    for (size_t i = 0; i < n; ++i)
      if (removed_[i]) p_[i] = x_[i];
  if (P_.disableCollision) {
    for (auto& c : contacts_) c.clear();
  } else {
    generateContacts();
    if (anyRemoved_)
      for (size_t i = 0; i < n; ++i)
        if (removed_[i]) contacts_[i].clear();
  }
  if (P_.selfContactDist > 0.f) selfContactFind(gravity, dt);
  else ut_.clear();
  eh_.clear();
  if (P_.edgeShapeContact && !P_.disableCollision) edgeShapeFind();
  // Depenetration at refDt (ours): a particle that starts the step inside a contact surface (the game raised the
  // thickness of a banner hanging against a wall, a shape appeared on it) is projected out in this step and leaves with
  // depth / dt. That is the one velocity of the original that does not keep its size at other step lengths: the
  // original at 62 FPS blew such banners off the wall at depth * 62, ours at the game's 230 FPS frames at 3.7x that
  // (Escape_p replay, cloth004: 69 m/s against 19-23). The part of the push that takes the start point out gets the
  // velocity it has in a step of refDt; the part that cancels motion into the surface is unchanged.
  depen_.clear();
  if (P_.refDt > 0.f && dt != P_.refDt) {
    for (size_t i = 0; i < n; ++i) {
      if (hard_[i]) continue;
      float best = 0.f;
      V3 bn;
      for (const Contact& c : contacts_[i]) {
        const float push = std::min(c.dist - c.n.dot(x_[i]), c.dist - c.n.dot(p_[i]));
        if (push > best) best = push, bn = c.n;
      }
      if (best > 0.f) depen_.push_back({(uint32_t)i, bn * best});
    }
  }

  const float N = (float)std::max<uint32_t>(1, iterations);
  float sStretch = 1.f - std::pow(1.f - stretching, 1.f / N);
  float sBend = 1.f - std::pow(1.f - bending, 1.f / N);
  if (relax != 1.f) sStretch = std::min(sStretch, relax), sBend = std::min(sBend, relax);
  for (uint32_t it = 0; it < iterations; ++it) {
    solveEdges(sStretch, sBend);
    if (!sc_.empty()) selfContactSolve();
    if (!se_.empty()) selfEdgeSolve();
    attachProject(dt);  // cloth_attach.cpp: shape attachments (hard ones set pinPos_)
    if (!eh_.empty()) edgeShapeSolve();
    for (size_t i = 0; i < n; ++i) {  // pins, then contacts of the same particle (contacts never sit on pinned ones)
      if (hard_[i]) p_[i] = pinPos_[i];
      for (const Contact& c : contacts_[i]) {
        float pen = c.dist - c.n.dot(p_[i]);
        if (pen > 0) p_[i] += c.n * pen;
      }
    }
  }
  // Untangling (ours, cloth_selftri.cpp) once after the iterations: inside them the edge solve took back most of each
  // pull at the game's 300 FPS (a hooked pair of triangles stayed 0.1 m apart for 5000 steps); 57 game recordings:
  // crossings 515 -> 472 summed over the replays, at the end 1239 -> 1047.
  if (!ut_.empty()) untangleSolve();
  if (anyRemoved_)  // pins and attachments do not act on removed particles
    for (size_t i = 0; i < n; ++i)
      if (removed_[i]) p_[i] = x_[i];
  // Post-solve mesh sweep (not in the original): contacts come from the predicted path x -> p*, but the edge solve
  // can then drag a particle sideways through a wall it was not heading for (a sheet draped over a box mesh lost its
  // corners into the box, and a closed mesh never pushes a particle back out). The solved path x -> p is swept against
  // the triangle meshes once more and a hit is projected like any contact (and kept for friction). Meshes only, so
  // cloths without them keep the original's results.
  if (hasMesh_ && !P_.disableCollision) {
    const float r = P_.thickness * 0.5f;
    for (size_t i = 0; i < n; ++i) {
      if (hard_[i] || isAttachedToShape(i) || (anyRemoved_ && removed_[i])) continue;
      ContactSlot s;
      collide(x_[i], p_[i], r, true, s);
      if (!(s.t >= 0.f && s.t <= 1.f)) continue;
      const Contact c{s.n, s.n.dot(s.c), s.shape};
      const float pen = c.dist - c.n.dot(p_[i]);
      if (pen <= 0.f) continue;
      p_[i] += c.n * pen;
      bool known = false;
      for (const Contact& o : contacts_[i]) known = known || (o.n.dot(c.n) > 0.9999f && std::fabs(o.dist - c.dist) < 1e-6f);
      if (!known) contacts_[i].push_back(c);
    }
  }

  // Sleep test on the pre-damping, pre-friction velocity (0x20 flag in IntegrateTimeStep, 2.8.0 0x100f0b50), summed over
  // all particles (0x100e7820). Counter rule and defaults [A], fitted to drop_box (sleeps on the 24th all-slow step).
  bool allSlow = true;
  const float sleepV2 = sleepVelocity() * sleepVelocity();
  for (size_t i = 0; i < n; ++i) {
    v_[i] = (p_[i] - x_[i]) * (1.f / dt);
    x_[i] = p_[i];
    if (v_[i].len2() >= sleepV2) allSlow = false;
  }
  if (!depen_.empty()) {
    const float k = 1.f / P_.refDt - 1.f / dt;
    for (const auto& d : depen_) v_[d.first] += d.second * k;
  }
  if (!sc_.empty() || !se_.empty()) {  // self contact (ours): inelastic pairs with friction, then the sleep test
    selfContactVelocity();
    selfEdgeVelocity();
    selfContactRest();
    allSlow = true;
    for (size_t i = 0; i < n && allSlow; ++i) allSlow = v_[i].len2() < sleepV2;
  }
  if (P_.useDamping && !P_.useComDamping) {
    const float k = damping * 0.5f;
    for (const Edge& e : sedges_) {
      if (hard_[e.i] || hard_[e.j]) continue;
      V3 dv = (v_[e.j] - v_[e.i]) * k;
      v_[e.i] += dv;
      v_[e.j] -= dv;
    }
  }
  if (P_.useValidBounds) validBoundsStep();
  bool purged = false;
  if (P_.useTearing) tearStep(&purged);  // rebuilds the edges after each split
  // after a purge in the tear pass the edges already match the alive triangles (a full rebuild would give the same):
  // only look for particles left in no alive triangle
  if (removedDirty_) removedFixup(!purged);
  if (friction_ != 0) {
    for (size_t i = 0; i < n; ++i) {
      if (hard_[i]) continue;
      for (const Contact& c : contacts_[i]) {  // proximity contacts take friction too (2.8.1 friction block tests flag 4 only)
        const Motion* m = c.shape >= 0 && (size_t)c.shape < motion_.size() && motion_[c.shape].hasVel ? &motion_[c.shape] : nullptr;
        if (m) {
          // Moving shape: relative to the surface velocity of its body at the particle, v_s = lin + ang x (x - com)
          // (2.8.0 0x100e5e80 with 0x100e2d40; zero for statics, which keep the form below)
          const V3 vs = m->lin + m->ang.cross(x_[i] - m->com);
          const V3 vr = v_[i] - vs;
          const float vn = c.n.dot(vr);
          const V3 vt = vr - c.n * vn;
          v_[i] = c.n * std::max(vn, 0.f) + (vt - vt * friction_) + vs;
          continue;
        }
        float vn = c.n.dot(v_[i]);
        V3 vt = v_[i] - c.n * vn;
        v_[i] = c.n * std::max(vn, 0.f) + vt * (1.f - friction_);
      }
    }
  }
  if (allSlow) {
    wakeCounter_ -= dt;
    if (wakeCounter_ <= 0.f) {  // putToSleep: velocities zeroed, positions frozen from the next step on
      asleep_ = true;
      for (V3& v : v_) v = V3();
    }
  } else {
    wakeCounter_ = P_.wakeUpCounter;
  }
}

}  // namespace pxc
