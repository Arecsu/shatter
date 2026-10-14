// v0: straightforward single-threaded implementation of the behaviour described in
// docs/re-notes-fluid.md. Correctness first; SIMD/threads come after it matches the oracle.
#include "../fastfloor.h"
#include "fluid.h"

#include <cfloat>
#include <cstdlib>
#include <cstring>

#include <algorithm>
#include <iterator>
#include <cmath>
#include <limits>
#include <unordered_map>

#if defined(__SSE2__)
#include <emmintrin.h>
#endif

namespace pxr {

namespace {
constexpr float kPi = 3.14159265358979f;

inline Vec3 normalize(const Vec3& v) {
  float l2 = v.len2();
  return l2 > 0 ? v * (1.0f / std::sqrt(l2)) : Vec3();
}

struct Contact {
  bool impact = false;  // swept crossing (earliest kept)
  float t = 2.0f;
  Vec3 n, p;            // normal, pushed-out surface point (world)
  int count = 0;        // averaged "collision" contacts
  Vec3 nSum, pSum;
  int nPlanes = 0;      // new cache candidates (impact or proximity)
  Vec3 cn[8];
  float cd[8];
  int ck[8];            // shape type that produced the plane
  const DynamicShape* cdyn[8];  // dynamic shape that produced the plane (null: static)
  Vec3 cpt[8];                  // the plane's contact point
  Vec3 cvs[8];                  // dynamic: surface velocity there
  int curType = 0;
  // dynamic pass only: surface velocity of the impact / summed over the collisions, and the body that two-way
  // coupling charges (the impact's, else the last colliding shape's)
  Vec3 vs, vsSum;
  const DynamicShape* shape = nullptr;
  void addPlane(const Vec3& n, const Vec3& p) {
    if (nPlanes < 8) { cn[nPlanes] = n; cd[nPlanes] = n.dot(p); ck[nPlanes] = curType; cdyn[nPlanes] = nullptr; cpt[nPlanes] = p; nPlanes++; }
  }
};

inline Vec3 cross(const Vec3& a, const Vec3& b) {
  return Vec3(a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x);
}

// Exact closest point on triangle (a,b,c) to p (7 regions, Ericson's formulation).
Vec3 closestOnTriangle(const Vec3& p, const Vec3& a, const Vec3& b, const Vec3& c) {
  const Vec3 ab = b - a, ac = c - a, ap = p - a;
  float d1 = ab.dot(ap), d2 = ac.dot(ap);
  if (d1 <= 0 && d2 <= 0) return a;
  Vec3 bp = p - b;
  float d3 = ab.dot(bp), d4 = ac.dot(bp);
  if (d3 >= 0 && d4 <= d3) return b;
  float vc = d1 * d4 - d3 * d2;
  if (vc <= 0 && d1 >= 0 && d3 <= 0) return a + ab * (d1 / (d1 - d3));
  Vec3 cp = p - c;
  float d5 = ab.dot(cp), d6 = ac.dot(cp);
  if (d6 >= 0 && d5 <= d6) return c;
  float vb = d5 * d2 - d1 * d6;
  if (vb <= 0 && d2 >= 0 && d6 <= 0) return a + ac * (d2 / (d2 - d6));
  float va = d3 * d6 - d5 * d4;
  if (va <= 0 && (d4 - d3) >= 0 && (d5 - d6) >= 0) {
    float w = (d4 - d3) / ((d4 - d3) + (d5 - d6));
    return b + (c - b) * w;
  }
  float denom = 1.0f / (va + vb + vc);
  return a + ab * (vb * denom) + ac * (vc * denom);
}

// Mesh impact contact point: true = the particle is put back at its start position (original behaviour, spec 1.4,
// disassembly 0x1019aaec); false = hit point + n * cd (what plane/box/sphere/capsule do). The velocity reflection
// uses the triangle normal either way.
constexpr bool kMeshImpactRestoresStart = false;  // user decision 2026-10-08: the restore is an artifact

// Mesh midphase. Candidate rule: every triangle that testMesh could change must be a candidate, and candidates are
// visited in ascending triangle index, so testMesh sees the same sequence of state changes as brute force (its merge
// is order-dependent: the hit flag gates coll/prx on earlier impacts, t ties keep the first triangle, and the plane
// slots break ties by order). The superset argument:
//  - collision/proximity: target within max(cd, prox) of the triangle, so the triangle box meets the query box;
//  - swept impact: H lies on old->target and inside the triangle up to the barycentric slop 1e-4, so the same holds
//    once the triangle box is inflated by the slop;
//  - float rounding: boxes are inflated by 5% of the longest edge; triangles with sin^2(angle) < 1e-3 make the
//    barycentric solve inaccurate, so they are always visited.
// PXF_MESH_BRUTE=1 disables the midphase (A/B checks).
constexpr float kTriSlopRel = 0.05f;
constexpr float kSkinnySin2 = 1e-3f;
constexpr uint32_t kBvhLeaf = 4;

bool meshBruteForce() {
  static const bool brute = [] {
    const char* e = getenv("PXF_MESH_BRUTE");
    return e && atoi(e) != 0;
  }();
  return brute;
}

inline Vec3 vmin(const Vec3& a, const Vec3& b) { return Vec3(std::min(a.x, b.x), std::min(a.y, b.y), std::min(a.z, b.z)); }
inline Vec3 vmax(const Vec3& a, const Vec3& b) { return Vec3(std::max(a.x, b.x), std::max(a.y, b.y), std::max(a.z, b.z)); }
inline bool overlaps(const Vec3& alo, const Vec3& ahi, const Vec3& blo, const Vec3& bhi) {
  return alo.x <= bhi.x && blo.x <= ahi.x && alo.y <= bhi.y && blo.y <= ahi.y && alo.z <= bhi.z && blo.z <= ahi.z;
}

// Median split on the longest centroid axis (ties by index, so the build is deterministic).
void buildBVHNode(StaticShape& s, std::vector<uint32_t>& items, const std::vector<Vec3>& cen, uint32_t begin,
                  uint32_t end) {
  const uint32_t self = (uint32_t)s.bvh.size();
  s.bvh.emplace_back();
  const float inf = std::numeric_limits<float>::infinity();
  Vec3 lo(inf, inf, inf), hi(-inf, -inf, -inf), clo(inf, inf, inf), chi(-inf, -inf, -inf);
  for (uint32_t k = begin; k < end; ++k) {
    const uint32_t ti = items[k];
    lo = vmin(lo, s.triBox[ti].lo);
    hi = vmax(hi, s.triBox[ti].hi);
    clo = vmin(clo, cen[ti]);
    chi = vmax(chi, cen[ti]);
  }
  s.bvh[self].lo = lo;
  s.bvh[self].hi = hi;
  if (end - begin <= kBvhLeaf) {
    s.bvh[self].first = (uint32_t)s.bvhTris.size();
    s.bvh[self].count = end - begin;
    s.bvhTris.insert(s.bvhTris.end(), items.begin() + begin, items.begin() + end);
    return;
  }
  const Vec3 ext = chi - clo;
  const int axis = (ext.x >= ext.y && ext.x >= ext.z) ? 0 : (ext.y >= ext.z ? 1 : 2);
  auto key = [&](uint32_t ti) { return axis == 0 ? cen[ti].x : axis == 1 ? cen[ti].y : cen[ti].z; };
  const uint32_t mid = begin + (end - begin) / 2;
  std::nth_element(items.begin() + begin, items.begin() + mid, items.begin() + end, [&](uint32_t p, uint32_t q) {
    const float kp = key(p), kq = key(q);
    return kp < kq || (kp == kq && p < q);
  });
  buildBVHNode(s, items, cen, begin, mid);
  s.bvh[self].first = (uint32_t)s.bvh.size();  // right child follows the whole left subtree
  buildBVHNode(s, items, cen, mid, end);
}

inline uint64_t gridKey(uint32_t x, uint32_t y, uint32_t z) { return ((uint64_t)x << 42) | ((uint64_t)y << 21) | z; }
inline uint32_t gridHash(uint64_t k) { return (uint32_t)((k * 0x9E3779B97F4A7C15ull) >> 32); }

// Mesh grid (StaticShape::Grid): cell edge = twice the median triangle box extent, so a triangle covers a few cells;
// one that would cover more than 64 goes on the `big` list.
void buildMeshGrid(StaticShape& s, const std::vector<uint32_t>& items) {
  StaticShape::Grid& g = s.grid;
  g = StaticShape::Grid();
  if (items.size() < 16) return;  // tiny meshes: the tree is as fast
  Vec3 lo = s.triBox[items[0]].lo;
  std::vector<float> ext;
  ext.reserve(items.size());
  for (uint32_t ti : items) {
    const TriBox& b = s.triBox[ti];
    lo = vmin(lo, b.lo);
    ext.push_back(std::max(std::max(b.hi.x - b.lo.x, b.hi.y - b.lo.y), b.hi.z - b.lo.z));
  }
  std::nth_element(ext.begin(), ext.begin() + ext.size() / 2, ext.end());
  Vec3 hi = s.bvh[0].hi;
  const Vec3 span = hi - lo;
  float cell = std::max(2.0f * ext[ext.size() / 2], 1e-3f);
  cell = std::max(cell, std::max(std::max(span.x, span.y), span.z) / 1.0e6f);  // 21-bit cell coordinates
  g.org = lo, g.inv = 1.0f / cell;
  std::vector<std::pair<uint64_t, uint32_t>> pairs;
  for (uint32_t ti : items) {
    const TriBox& b = s.triBox[ti];
    const Vec3 a = (b.lo - g.org) * g.inv, c = (b.hi - g.org) * g.inv;
    const uint32_t x0 = (uint32_t)std::max(0.0f, a.x), y0 = (uint32_t)std::max(0.0f, a.y), z0 = (uint32_t)std::max(0.0f, a.z);
    const uint32_t x1 = (uint32_t)std::max(0.0f, c.x), y1 = (uint32_t)std::max(0.0f, c.y), z1 = (uint32_t)std::max(0.0f, c.z);
    if ((uint64_t)(x1 - x0 + 1) * (y1 - y0 + 1) * (z1 - z0 + 1) > 64) {
      g.big.push_back(ti);
      continue;
    }
    for (uint32_t z = z0; z <= z1; ++z)
      for (uint32_t y = y0; y <= y1; ++y)
        for (uint32_t x = x0; x <= x1; ++x) pairs.push_back({gridKey(x, y, z), ti});
  }
  std::sort(pairs.begin(), pairs.end());
  for (size_t i = 0; i < pairs.size(); ++i) {
    if (i == 0 || pairs[i].first != pairs[i - 1].first) {
      g.keys.push_back(pairs[i].first);
      g.start.push_back((uint32_t)i);
    }
    g.items.push_back(pairs[i].second);
  }
  g.start.push_back((uint32_t)pairs.size());
  uint32_t size = 16;
  while (size < 2 * g.keys.size()) size <<= 1;
  g.mask = size - 1;
  g.table.assign(size, 0);
  for (uint32_t c = 0; c < g.keys.size(); ++c) {
    uint32_t h = gridHash(g.keys[c]) & g.mask;
    while (g.table[h]) h = (h + 1) & g.mask;
    g.table[h] = c + 1;
  }
}

// Candidates for a particle moving old -> target, tested against the mesh with reach maxDist (= max(cd, prox)).
// Output is sorted ascending by triangle index: every triangle whose box overlaps the query box (the grid and the
// tree give the same set).
void meshCandidates(const StaticShape& s, const Vec3& old, const Vec3& target, float maxDist,
                    std::vector<uint32_t>& out) {
  out.clear();
  const Vec3 mn = vmin(old, target), mx = vmax(old, target);
  const float mag = std::max(std::max(std::max(std::fabs(old.x), std::fabs(old.y)), std::fabs(old.z)),
                             std::max(std::max(std::fabs(target.x), std::fabs(target.y)), std::fabs(target.z)));
  const float pad = maxDist * 1.01f + 1e-5f * (1.0f + mag);
  const Vec3 qlo = mn - Vec3(pad, pad, pad), qhi = mx + Vec3(pad, pad, pad);
  const StaticShape::Grid& g = s.grid;
  static const bool treeOnly = getenv("PXF_MESH_TREE") != nullptr;  // A/B: tree only
  if (g.mask && !treeOnly) {
    const Vec3 a = (qlo - g.org) * g.inv, c = (qhi - g.org) * g.inv;
    // cells beyond the mesh's corner hold nothing: clamp at 0 (the far side needs no clamp, lookups just miss)
    const float x0f = std::max(0.0f, a.x), y0f = std::max(0.0f, a.y), z0f = std::max(0.0f, a.z);
    if (c.x >= 0 && c.y >= 0 && c.z >= 0 && c.x < 2.0e6f && c.y < 2.0e6f && c.z < 2.0e6f) {
      const uint32_t x0 = (uint32_t)x0f, y0 = (uint32_t)y0f, z0 = (uint32_t)z0f;
      const uint32_t x1 = (uint32_t)c.x, y1 = (uint32_t)c.y, z1 = (uint32_t)c.z;
      if ((uint64_t)(x1 - x0 + 1) * (y1 - y0 + 1) * (z1 - z0 + 1) <= 27) {
        for (uint32_t z = z0; z <= z1; ++z)
          for (uint32_t y = y0; y <= y1; ++y)
            for (uint32_t x = x0; x <= x1; ++x) {
              const uint64_t k = gridKey(x, y, z);
              for (uint32_t h = gridHash(k) & g.mask; g.table[h]; h = (h + 1) & g.mask) {
                const uint32_t ci = g.table[h] - 1;
                if (g.keys[ci] != k) continue;
                for (uint32_t i = g.start[ci]; i < g.start[ci + 1]; ++i) {
                  const uint32_t ti = g.items[i];
                  if (overlaps(s.triBox[ti].lo, s.triBox[ti].hi, qlo, qhi)) out.push_back(ti);
                }
                break;
              }
            }
        for (uint32_t ti : g.big)
          if (overlaps(s.triBox[ti].lo, s.triBox[ti].hi, qlo, qhi)) out.push_back(ti);
        std::sort(out.begin(), out.end());
        out.erase(std::unique(out.begin(), out.end()), out.end());
        return;
      }
    } else if (c.x < 0 || c.y < 0 || c.z < 0) {
      return;  // entirely below the mesh's lower corner: no triangle box can overlap
    }
  }
  uint32_t stack[64];
  int sp = 0;
  if (!s.bvh.empty()) stack[sp++] = 0;
  while (sp) {
    const uint32_t ni = stack[--sp];
    const MeshBVHNode& nd = s.bvh[ni];
    if (!overlaps(nd.lo, nd.hi, qlo, qhi)) continue;
    if (nd.count) {
      for (uint32_t k = 0; k < nd.count; ++k) {
        const uint32_t ti = s.bvhTris[nd.first + k];
        if (overlaps(s.triBox[ti].lo, s.triBox[ti].hi, qlo, qhi)) out.push_back(ti);
      }
    } else {
      stack[sp++] = nd.first;  // right
      stack[sp++] = ni + 1;    // left (popped first)
    }
  }
  std::sort(out.begin(), out.end());
}

// Static triangle mesh, one-sided (spec docs/re-notes-fluid-mesh.md 1.3 and 1.4). Per triangle:
//  - collision (front and dist2 < cd^2) or proximity (face region, front, dist2 < pd^2), both suppressed once an
//    impact exists; a fired collision/proximity skips the swept test for that triangle;
//  - otherwise the swept test of old -> target against the triangle (earliest t in [0,1) wins).
// Merge across triangles (1.4): an impact anywhere in the mesh drops the mesh's collision contacts; the earliest
// impact wins; collisions are summed; the two cache planes with the smallest plane distance are kept, impacts and
// proximities alike (plane distance: (S - v0).n/nl for proximity, -num/nl for impact).
// Midphase slop of a triangle: its box is grown by this much, and a sliver's impact point must lie within it.
inline float triSlop(const MeshTri& tr) {
  const float maxEdge2 = std::max(std::max((tr.v1 - tr.v0).len2(), (tr.v2 - tr.v0).len2()), (tr.v2 - tr.v1).len2());
  const float mag = std::max(std::max(std::fabs(tr.v0.x), std::fabs(tr.v0.y)), std::fabs(tr.v0.z));
  const float mag2 = std::max(std::max(std::fabs(tr.v1.x), std::fabs(tr.v1.y)), std::fabs(tr.v1.z));
  const float mag3 = std::max(std::max(std::fabs(tr.v2.x), std::fabs(tr.v2.y)), std::fabs(tr.v2.z));
  return kTriSlopRel * std::sqrt(maxEdge2) + 1e-5f * (1.0f + std::max(mag, std::max(mag2, mag3)));
}

void testMesh(const StaticShape& s, const Vec3& old, const Vec3& target, float cd, float prox, Contact& c,
              bool impactRestoresStart) {
  const float cd2 = cd * cd, pd2 = prox * prox, reach = std::max(cd, prox);
  static const bool meshNoSkip = getenv("PXF_MESH_NOSKIP") != nullptr;
  bool impact = false;       // any impact in this mesh so far (sets the record's "hit")
  float bestT = 2.0f;
  Vec3 bestN, bestP;
  int count = 0;
  Vec3 nSum, pSum;
  struct Slot { Vec3 n, p; float dist = 0; bool valid = false; } slot[2];
  auto keepPlane = [&](const Vec3& n, const Vec3& p, float dist) {
    if (!slot[0].valid) slot[0] = {n, p, dist, true};
    else if (!slot[1].valid) slot[1] = {n, p, dist, true};
    else {
      int far = slot[0].dist > slot[1].dist ? 0 : 1;
      if (dist < slot[far].dist) slot[far] = {n, p, dist, true};
    }
  };
  const Vec3 motion = target - old;
  auto visit = [&](uint32_t ti) {
    const MeshTri& tr = s.tris[ti];
    const Vec3& v0 = tr.v0;
    const Vec3 e1 = tr.v1 - v0, e2 = tr.v2 - v0;
    const Vec3 nrm = cross(e1, e2);
    const float nl = std::sqrt(nrm.len2());
    if (nl == 0) return;  // zero-area triangle: no contact (not specified by the original notes)
    const Vec3 nU = nrm * (1.0f / nl);
    const bool hit = c.impact || impact;

    // Face flag (spec 1.3 step 1): the orthogonal projection of P lies in the triangle, with the original's boundary
    // convention s > 0, t > 0, s + t <= 1 (s, t from the unclamped normal equations). A probe exactly on the shared
    // diagonal of a quad counts as face in the original, so the face test is not the closest-point region test.
    const float a = e1.dot(e1), b = e1.dot(e2), cc = e2.dot(e2);
    const float dd = e1.dot(v0 - target), ee = e2.dot(v0 - target);
    const float det2 = a * cc - b * b;
    const float snum = b * ee - cc * dd, tnum = b * dd - a * ee;
    const bool face = det2 > 0 && snum > 0 && tnum > 0 && snum + tnum <= det2;
    // The closest point is needed only for a collision or proximity: neither can fire once there is an impact, nor
    // when the end point is farther from the triangle's plane than the reach (the distance to the triangle is at
    // least that; the margin covers rounding), so those skip it. PXF_MESH_NOSKIP=1 computes it always (A/B).
    bool coll = false, prx = false;
    Vec3 C, D;
    float dist2 = 0;
    if (!hit) {
      const Vec3 w = target - v0;
      const float plane = nU.dot(w);
      const float mag = std::max(std::max(std::max(std::fabs(target.x), std::fabs(target.y)), std::fabs(target.z)),
                                 std::max(std::max(std::fabs(v0.x), std::fabs(v0.y)), std::fabs(v0.z)));
      if (meshNoSkip || !(std::fabs(plane) > reach * 1.001f + 1e-5f * (1.0f + mag))) {
        C = closestOnTriangle(target, v0, tr.v1, tr.v2);
        D = target - C;
        dist2 = D.len2();
        const bool front = nrm.dot(D) > 0;
        coll = front && dist2 < cd2;
        prx = face && front && dist2 < pd2;
      }
    }
    if (coll || prx) {
      if (coll) {
        // Edges and vertices push out radially; faces (and any point within 1e-5 m^2) along the face normal.
        Vec3 nc = dist2 <= 1e-5f ? nU : D * (1.0f / std::sqrt(dist2));
        count++;
        nSum += nc;
        pSum += C + nc * cd;
      }
      if (prx) keepPlane(nU, C + nU * cd, nU.dot(old - v0));
      return;  // collision or proximity fired: no swept test for this triangle
    }

    // Swept impact: S must be on the front side and P strictly behind the plane.
    const float num = nrm.dot(v0 - old), den = nrm.dot(target - old);
    if (!(num <= 0 && den < num)) return;
    const float t = num / den;  // in [0, 1)
    const Vec3 H = old + motion * t;
    const Vec3 w = H - v0;
    const float p1 = e1.dot(w), p2 = e2.dot(w);
    const float det = det2;
    const float u = (cc * p1 - b * p2) / det, vv = (a * p2 - b * p1) / det;
    const float eps = 1e-4f;  // relative barycentric slop
    if (!(u > -eps && vv >= -eps && u + vv <= 1.0f + eps)) return;
    // Sliver (near-zero det): u, v are not trustworthy and can accept a point far from the triangle. Keep the
    // impact only if H really lies on the triangle, within the midphase slop (a numerical-artifact fix, not a
    // behaviour of the original; well-shaped triangles never reach this check).
    if (!(det2 > kSkinnySin2 * a * cc)) {
      const float sl = triSlop(tr);
      if ((H - closestOnTriangle(H, v0, tr.v1, tr.v2)).len2() > sl * sl) return;
    }

    // Any impact clears the collisions gathered so far (record flag is assigned 4); cache planes are kept.
    impact = true;
    count = 0;
    nSum = pSum = Vec3();
    const Vec3 Hcd = H + nU * cd;
    if (t < bestT) {
      bestT = t;
      bestN = nU;
      bestP = impactRestoresStart ? old : Hcd;
    }
    keepPlane(nU, Hcd, -num / nl);
  };

  if (meshBruteForce() || s.triBox.size() != s.tris.size()) {  // no midphase built: brute force
    for (uint32_t ti = 0; ti < s.tris.size(); ++ti) visit(ti);
  } else {
    static thread_local std::vector<uint32_t> cand;
    meshCandidates(s, old, target, std::max(cd, prox), cand);
    for (uint32_t ti : cand) visit(ti);
  }

  if (impact) {
    if (!c.impact || bestT < c.t) { c.impact = true; c.t = bestT; c.n = bestN; c.p = bestP; }
  } else if (count) {
    c.count += count;
    c.nSum += nSum;
    c.pSum += pSum;
  }
  for (const Slot& sl : slot)
    if (sl.valid) c.addPlane(sl.n, sl.p);
}

// old/target: start and end of the motion; cd: collision distance; prox: proximity distance.
void testShapeLocal(const StaticShape& s, const Vec3& old, const Vec3& target, float cd, float prox, Contact& c);

// Classify one particle against one static shape, in world space. An oriented shape runs the same test in its own
// frame (the frame is rigid, so distances, cd and prox are unchanged) and its results come back to the world.
void testShape(const StaticShape& s, const Vec3& old, const Vec3& target, float cd, float prox, Contact& c) {
  if (!s.oriented) {
    testShapeLocal(s, old, target, cd, prox, c);
    return;
  }
  const Pose& f = s.frame;
  Contact l;
  l.impact = c.impact;
  l.t = c.t;
  l.curType = c.curType;
  testShapeLocal(s, f.applyInv(old), f.applyInv(target), cd, prox, l);
  for (int k = 0; k < l.nPlanes; ++k) {
    const Vec3 n = f.rotate(l.cn[k]);
    c.addPlane(n, f.apply(l.cpt[k]));
  }
  if (l.impact && (!c.impact || l.t < c.t)) {
    c.impact = true;
    c.t = l.t;
    c.n = f.rotate(l.n);
    c.p = f.apply(l.p);
  } else if (l.count) {
    c.count += l.count;
    c.nSum += f.rotate(l.nSum);
    c.pSum += f.rotate(l.pSum) + f.t * (float)l.count;
  }
}

void testShapeLocal(const StaticShape& s, const Vec3& old, const Vec3& target, float cd, float prox, Contact& c) {
  if (s.type == StaticShape::Mesh) {  // no started-inside test: a particle behind a face is never stopped by it
    testMesh(s, old, target, cd, prox, c, kMeshImpactRestoresStart);
    return;
  }
  // Signed distance + outward normal at a point, per shape.
  auto sd = [&](const Vec3& x, Vec3& n) -> float {
    switch (s.type) {
      case StaticShape::Plane:
        n = s.n;
        return s.n.dot(x) - s.d;
      case StaticShape::Sphere: {
        Vec3 r = x - s.c;
        float l = std::sqrt(r.len2());
        n = l > 0 ? r * (1.0f / l) : Vec3(0, 1, 0);
        return l - s.r;
      }
      case StaticShape::Capsule: {
        Vec3 r = x - s.c;
        float hy = 0.5f * s.h;
        float cy = std::max(-hy, std::min(hy, r.y));
        Vec3 q = r - Vec3(0, cy, 0);
        float l = std::sqrt(q.len2());
        n = l > 0 ? q * (1.0f / l) : Vec3(1, 0, 0);
        return l - s.r;
      }
      case StaticShape::Box: {
        Vec3 r = x - s.c;
        Vec3 q(std::fabs(r.x) - s.half.x, std::fabs(r.y) - s.half.y, std::fabs(r.z) - s.half.z);
        Vec3 qo(std::max(q.x, 0.f), std::max(q.y, 0.f), std::max(q.z, 0.f));
        float outside = std::sqrt(qo.len2());
        if (outside > 0) {
          n = normalize(Vec3(r.x < 0 ? -qo.x : qo.x, r.y < 0 ? -qo.y : qo.y, r.z < 0 ? -qo.z : qo.z));
          return outside;
        }
        // inside: nearest face
        if (q.x >= q.y && q.x >= q.z) { n = Vec3(r.x < 0 ? -1.f : 1.f, 0, 0); return q.x; }
        if (q.y >= q.z) { n = Vec3(0, r.y < 0 ? -1.f : 1.f, 0); return q.y; }
        n = Vec3(0, 0, r.z < 0 ? -1.f : 1.f);
        return q.z;
      }
      case StaticShape::Mesh:
        break;  // handled in testMesh
    }
    return 1e30f;
  };

  // A particle that starts inside (d0 <= 0) gets no swept test: its end point is classified below like any other, so it
  // is pushed out only if the ballistic end is within cd. Matches the oracle for normal motion; a tangential start-on-
  // surface residual remains (docs/sph-game-validation.md, band residuals).
  Vec3 nStart;
  const float d0 = sd(old, nStart);
  // Exact swept test of the segment old->target against the shape surface.
  Vec3 dseg = target - old;
  float t = 2.0f;
  Vec3 nh;
  switch (s.type) {
    case StaticShape::Plane: {
      float d1 = s.n.dot(target) - s.d;
      if (d1 < 0 && d0 > 0) { t = d0 / (d0 - d1); nh = s.n; }
      break;
    }
    case StaticShape::Sphere: {
      Vec3 r0 = old - s.c;
      float a = dseg.len2(), b = 2 * r0.dot(dseg), cc = r0.len2() - s.r * s.r;
      float disc = b * b - 4 * a * cc;
      if (disc > 0 && a > 0) {
        float tt = -(std::sqrt(disc) + b) / (2 * a);
        if (tt >= 0 && tt <= 1) { t = tt; nh = normalize(r0 + dseg * tt); }
      }
      break;
    }
    case StaticShape::Box: {
      Vec3 r0 = old - s.c;
      float o[3] = {r0.x, r0.y, r0.z}, dd[3] = {dseg.x, dseg.y, dseg.z}, hh[3] = {s.half.x, s.half.y, s.half.z};
      float tin = -1e30f, tout = 1e30f;
      int axis = -1;
      float sign = 0;
      bool miss = false;
      for (int k = 0; k < 3 && !miss; ++k) {
        if (std::fabs(dd[k]) < 1e-20f) {
          if (o[k] < -hh[k] || o[k] > hh[k]) miss = true;
          continue;
        }
        float inv = 1.0f / dd[k];
        float ta = (-hh[k] - o[k]) * inv, tb = (hh[k] - o[k]) * inv;
        float sg = -1.0f;
        if (ta > tb) { std::swap(ta, tb); sg = 1.0f; }
        if (ta > tin) { tin = ta; axis = k; sign = sg; }
        tout = std::min(tout, tb);
      }
      if (!miss && axis >= 0 && tin <= tout && tin >= 0 && tin <= 1) {
        t = tin;
        nh = Vec3(axis == 0 ? sign : 0, axis == 1 ? sign : 0, axis == 2 ? sign : 0);
      }
      break;
    }
    case StaticShape::Capsule: {
      // earliest hit among the cylinder side and the two cap spheres
      Vec3 r0 = old - s.c;
      float hy = 0.5f * s.h;
      auto sphereHit = [&](const Vec3& centre) {
        Vec3 q = old - centre;
        float a = dseg.len2(), b = 2 * q.dot(dseg), cc = q.len2() - s.r * s.r;
        float disc = b * b - 4 * a * cc;
        if (disc > 0 && a > 0) {
          float tt = -(std::sqrt(disc) + b) / (2 * a);
          if (tt >= 0 && tt <= 1 && tt < t) { t = tt; nh = normalize(q + dseg * tt); }
        }
      };
      sphereHit(s.c + Vec3(0, hy, 0));
      sphereHit(s.c + Vec3(0, -hy, 0));
      float a = dseg.x * dseg.x + dseg.z * dseg.z, b = 2 * (r0.x * dseg.x + r0.z * dseg.z);
      float cc = r0.x * r0.x + r0.z * r0.z - s.r * s.r;
      float disc = b * b - 4 * a * cc;
      if (disc > 0 && a > 0) {
        float tt = -(std::sqrt(disc) + b) / (2 * a);
        float y = r0.y + dseg.y * tt;
        if (tt >= 0 && tt <= 1 && tt < t && y >= -hy && y <= hy) {
          t = tt;
          nh = normalize(Vec3(r0.x + dseg.x * tt, 0, r0.z + dseg.z * tt));
        }
      }
      break;
    }
    case StaticShape::Mesh:
      break;
  }
  if (t >= 0 && t <= 1) {
    Vec3 hit = old + dseg * t;
    Vec3 p = hit + nh * cd;
    if (!c.impact || t < c.t) { c.impact = true; c.t = t; c.n = nh; c.p = p; }
    c.addPlane(nh, p);
    return;
  }
  // No crossing: the end point is classified against the surface (re_fluidVsPlane & co). Skipped once an
  // earlier shape already produced an impact for this particle.
  //   0 < d1 <= prox : proximity -> contact plane cached for the next step
  //   d1 <= cd       : collision -> averaged push-out to the collision distance
  if (c.impact) return;
  if (s.type == StaticShape::Box) {
    // Box: the end point is tested against the six face planes separately (the generic convex-plane test
    // 0x101ab240), not against the Euclidean distance to the box [V: first-divergence trace of steam_ceiling].
    //  - any face with a positive distance > prox: neither proximity nor collision
    //  - proximity (0x10): exactly one face with a positive distance, all positive ones <= prox
    //  - collision (8): every positive face distance <= cd (square edges/corners, not rounded); the face with the
    //    largest signed distance supplies normal and push-out point
    Vec3 r = target - s.c;
    const float rr[3] = {r.x, r.y, r.z}, hh[3] = {s.half.x, s.half.y, s.half.z};
    bool collOK = true, proxOK = true;
    int npos = 0, posK = -1, bestK = -1;
    float posD = 0, bestD = -3.4e38f;
    for (int k = 0; k < 6; ++k) {
      int ax = k >> 1;
      float sg = (k & 1) ? 1.0f : -1.0f;  // face normal sign along the axis
      float d = sg * rr[ax] - hh[ax];
      if (d > prox) { collOK = proxOK = false; continue; }
      if (d > 0) { if (npos++ == 0) { posK = k; posD = d; } else proxOK = false; }
      if (d > cd) collOK = false;
      else if (d > bestD) { bestD = d; bestK = k; }
    }
    auto faceN = [&](int k) {
      float sg = (k & 1) ? 1.0f : -1.0f;
      int ax = k >> 1;
      return Vec3(ax == 0 ? sg : 0, ax == 1 ? sg : 0, ax == 2 ? sg : 0);
    };
    if (proxOK && npos == 1) c.addPlane(faceN(posK), target + faceN(posK) * (cd - posD));
    if (collOK && bestK >= 0) {
      Vec3 nb = faceN(bestK);
      c.count++; c.nSum += nb; c.pSum += target + nb * (cd - bestD);
    }
    return;
  }
  Vec3 n1;
  float d1 = sd(target, n1);
  if (d1 > prox) return;
  Vec3 p = target + n1 * (cd - d1);
  if (d1 <= cd) { c.count++; c.nSum += n1; c.pSum += p; }
  if (d1 > 0) c.addPlane(n1, p);
}

// One dynamic shape: the particle's start goes into the shape frame with the begin pose, its target with the end
// pose (relative motion), the shape test runs there, and the results come back with the end pose. An impact found
// on an earlier shape of the pass suppresses collisions and later impacts, as within the static pass. `near` is
// set when the shape produced any contact or proximity plane (the dynamic-contact flag, dyn_kin steps 54 and 58).
void testDynamic(const DynamicShape& d, const Vec3& old, const Vec3& target, float cd, float prox, Contact& c,
                 bool& near) {
  const Vec3 o = d.begin.applyInv(old), tg = d.end.applyInv(target);
  Contact l;
  l.impact = c.impact;
  l.t = c.t;
  l.curType = (int)d.geom->type;
  testShape(*d.geom, o, tg, cd, prox, l);
  if (l.nPlanes || l.count) near = true;
  for (int k = 0; k < l.nPlanes && c.nPlanes < 8; ++k) {  // cache candidates, in world space at the end pose
    const Vec3 n = d.end.rotate(l.cn[k]);
    c.cn[c.nPlanes] = n;
    c.cd[c.nPlanes] = l.cd[k] + n.dot(d.end.t);
    c.ck[c.nPlanes] = l.ck[k];
    c.cdyn[c.nPlanes] = &d;
    c.cpt[c.nPlanes] = d.end.apply(l.cpt[k]);
    c.cvs[c.nPlanes] = d.pointVelocity(c.cpt[c.nPlanes]);
    c.nPlanes++;
  }
  if (l.impact && (!c.impact || l.t < c.t)) {
    c.impact = true;
    c.t = l.t;
    c.n = d.end.rotate(l.n);
    c.p = d.end.apply(l.p);
    c.vs = d.pointVelocity(c.p);
    c.shape = &d;
  } else if (l.count) {
    const Vec3 pSum = d.end.rotate(l.pSum) + d.end.t * (float)l.count;
    c.count += l.count;
    c.nSum += d.end.rotate(l.nSum);
    c.pSum += pSum;
    c.vsSum += d.linVel * (float)l.count + cross(d.angVel, pSum - d.com * (float)l.count);
    if (!c.impact) c.shape = &d;
  }
}

// Velocity response against a surface (static: surface velocity 0).
// vIn: velocity to respond; vOld: velocity used for the normal (restitution) test.
inline Vec3 respond(const Vec3& vIn, const Vec3& vOld, const Vec3& n, float friction, float restitution) {
  float vn = n.dot(vIn);
  if (vn >= 0) return vIn;
  Vec3 out = (vIn - n * vn) * (1.0f - friction);
  float vno = n.dot(vOld);
  if (vno < 0) out -= n * (vno * restitution);
  return out;
}
}  // namespace

// Per-step data for rejecting a (particle, dynamic shape) pair before testDynamic. The test runs on the segment
// o -> tg in the shape frame (o = begin^-1 old, tg = end^-1 target), and every result it can produce lies within
// `reach` of the geometry, which lies within `radius` of the shape origin. With a = old - begin.t, |o| = |a|, and
// |tg - o| <= |target - old| + |end.t - begin.t| + theta |a| (theta: rotation angle between the two poses). The
// segment can only come within radius + reach of the origin if |o| - |tg - o| <= radius + reach, so
// |a| (1 - theta) > radius + reach + |target - old| + |end.t - begin.t| rejects the pair exactly (no false rejects).
struct DynCull {
  Vec3 tb;
  float base;  // radius + reach + |end.t - begin.t|
  float keep;  // 1 - theta (<= 0: never reject)
};

float geomRadius(const StaticShape& g) {
  switch (g.type) {
    case StaticShape::Sphere: return g.r;
    case StaticShape::Box: return std::sqrt(g.half.len2());
    case StaticShape::Capsule: return g.r + 0.5f * g.h;
    case StaticShape::Mesh: {
      float r2 = 0;
      for (const MeshTri& t : g.tris)
        for (const Vec3* v : {&t.v0, &t.v1, &t.v2}) r2 = std::max(r2, v->len2());
      return std::sqrt(r2);
    }
    default: return FLT_MAX;
  }
}

DynCull makeCull(const DynamicShape& d, float reach) {
  DynCull c;
  c.tb = d.begin.t;
  const float r = geomRadius(*d.geom);
  const Vec3 dt = d.end.t - d.begin.t;
  c.base = r == FLT_MAX ? FLT_MAX : r + reach + std::sqrt(dt.len2());
  // rotation angle of end * begin^-1: trace = sum_ij end.m[i][j] * begin.m[i][j]
  float tr = 0;
  for (int i = 0; i < 3; ++i)
    for (int j = 0; j < 3; ++j) tr += d.end.m[i][j] * d.begin.m[i][j];
  const float theta = std::acos(std::min(1.0f, std::max(-1.0f, 0.5f * (tr - 1.0f))));
  c.keep = 1.0f - theta - 1e-4f;  // margin for the rounding of the trace
  return c;
}

inline bool culled(const DynCull& c, const Vec3& old, float particleMove) {
  if (c.keep <= 0 || c.base == FLT_MAX) return false;
  const Vec3 a = old - c.tb;
  const float lim = (c.base + particleMove) * 1.001f;
  return a.len2() * c.keep * c.keep > lim * lim;
}

// World bounds of a static shape (planes: none, never rejected). A (particle, shape) pair whose segment box grown by
// `reach` misses them cannot produce a contact or a proximity plane, so skipping it leaves the result unchanged.
struct StaticCull {
  Vec3 lo, hi;
  bool always;
};

StaticCull staticBounds(const StaticShape& s) {
  StaticCull c{Vec3(), Vec3(), false};
  switch (s.type) {
    case StaticShape::Plane: c.always = true; break;
    case StaticShape::Sphere: c.lo = s.c - Vec3(s.r, s.r, s.r), c.hi = s.c + Vec3(s.r, s.r, s.r); break;
    case StaticShape::Box:
    case StaticShape::Capsule: {
      const Vec3 e = s.type == StaticShape::Box ? s.half : Vec3(s.r, s.r + 0.5f * s.h, s.r);
      if (s.oriented) {  // local box e about c, rotated: world half extents |R| e
        const Pose& f = s.frame;
        const Vec3 w = f.apply(s.c);
        Vec3 ew;
        float* o[3] = {&ew.x, &ew.y, &ew.z};
        for (int i = 0; i < 3; ++i)
          *o[i] = std::fabs(f.m[i][0]) * e.x + std::fabs(f.m[i][1]) * e.y + std::fabs(f.m[i][2]) * e.z;
        if (s.type == StaticShape::Capsule) {  // tighter: segment along the local y axis plus the radius
          const float hy = 0.5f * s.h;
          ew = Vec3(std::fabs(f.m[0][1]) * hy + s.r, std::fabs(f.m[1][1]) * hy + s.r, std::fabs(f.m[2][1]) * hy + s.r);
        }
        c.lo = w - ew, c.hi = w + ew;
      } else {
        c.lo = s.c - e, c.hi = s.c + e;
      }
      break;
    }
    case StaticShape::Mesh:
      if (!s.bvh.empty()) {
        c.lo = s.bvh[0].lo, c.hi = s.bvh[0].hi;
      } else {
        c.lo = Vec3(FLT_MAX, FLT_MAX, FLT_MAX), c.hi = Vec3(-FLT_MAX, -FLT_MAX, -FLT_MAX);
        for (const MeshTri& t : s.tris)
          for (const Vec3* v : {&t.v0, &t.v1, &t.v2}) {
            c.lo = Vec3(std::min(c.lo.x, v->x), std::min(c.lo.y, v->y), std::min(c.lo.z, v->z));
            c.hi = Vec3(std::max(c.hi.x, v->x), std::max(c.hi.y, v->y), std::max(c.hi.z, v->z));
          }
      }
      break;
  }
  return c;
}

bool staticShapeBounds(const StaticShape& s, Vec3& lo, Vec3& hi) {
  const StaticCull c = staticBounds(s);
  if (c.always) return false;
  lo = c.lo, hi = c.hi;
  return true;
}

namespace {
// Uniform grid of shape bounds over the particles of one step (the broadphase for the per-pair rejection in
// collide). Shape k is listed in every cell its inflated box touches, in ascending k; a shape covering too many cells
// goes on the `always` list. A point query returns the candidates of the point's cell merged with `always`, in
// ascending k, so the exact per-pair tests run on a superset of the passing pairs in the same order: results are
// unchanged (bit-identical to the full scan, PXF_NOCULL / PXF_NOGRID).
struct ShapeGrid {
  Vec3 org;
  float inv = 0;
  int nx = 0, ny = 0, nz = 0;
  std::vector<uint32_t> start, items, always;
  bool active = false;

  // lo/hi: query region (the particles' start positions); cell: edge length; boxes: inflated shape bounds, `all`
  // marks shapes that must always be tested.
  void build(const Vec3& lo, const Vec3& hi, float cell, const std::vector<Vec3>& blo, const std::vector<Vec3>& bhi,
             const std::vector<uint8_t>& all) {
    active = false;
    always.clear();
    const Vec3 ext = hi - lo;
    // at most ~32k cells
    const float vol = std::max(ext.x, cell) * std::max(ext.y, cell) * std::max(ext.z, cell);
    cell = std::max(cell, std::cbrt(vol / 32768.0f));
    inv = 1.0f / cell;
    org = lo;
    nx = std::min(256, (int)(ext.x * inv) + 1), ny = std::min(256, (int)(ext.y * inv) + 1),
    nz = std::min(256, (int)(ext.z * inv) + 1);
    const size_t ncell = (size_t)nx * ny * nz;
    start.assign(ncell + 1, 0);
    const size_t n = blo.size();
    std::vector<int> range(n * 6);
    auto cellRange = [&](size_t k, int* r) {
      const Vec3 a = (blo[k] - org) * inv, b = (bhi[k] - org) * inv;
      r[0] = std::max(0, (int)pxm::floorExact(a.x)), r[1] = std::min(nx - 1, (int)pxm::floorExact(b.x));
      r[2] = std::max(0, (int)pxm::floorExact(a.y)), r[3] = std::min(ny - 1, (int)pxm::floorExact(b.y));
      r[4] = std::max(0, (int)pxm::floorExact(a.z)), r[5] = std::min(nz - 1, (int)pxm::floorExact(b.z));
    };
    for (size_t k = 0; k < n; ++k) {
      int* r = &range[k * 6];
      if (all[k]) { always.push_back((uint32_t)k); r[0] = 1, r[1] = 0; continue; }
      cellRange(k, r);
      if (r[0] > r[1] || r[2] > r[3] || r[4] > r[5]) { r[0] = 1, r[1] = 0; continue; }  // outside the region
      if ((size_t)(r[1] - r[0] + 1) * (r[3] - r[2] + 1) * (r[5] - r[4] + 1) > 512) {
        always.push_back((uint32_t)k);
        r[0] = 1, r[1] = 0;
        continue;
      }
      for (int z = r[4]; z <= r[5]; ++z)
        for (int y = r[2]; y <= r[3]; ++y)
          for (int x = r[0]; x <= r[1]; ++x) ++start[((size_t)z * ny + y) * nx + x + 1];
    }
    for (size_t c = 0; c < ncell; ++c) start[c + 1] += start[c];
    items.resize(start[ncell]);
    std::vector<uint32_t> fill(start.begin(), start.end() - 1);
    for (size_t k = 0; k < n; ++k) {
      const int* r = &range[k * 6];
      if (r[0] > r[1]) continue;
      for (int z = r[4]; z <= r[5]; ++z)
        for (int y = r[2]; y <= r[3]; ++y)
          for (int x = r[0]; x <= r[1]; ++x) items[fill[((size_t)z * ny + y) * nx + x]++] = (uint32_t)k;
    }
    active = true;
  }
  // Candidates for a point inside the region (false: outside, the caller scans everything).
  bool query(const Vec3& p, std::vector<uint32_t>& out) const {
    const Vec3 a = (p - org) * inv;
    if (!(a.x >= 0 && a.y >= 0 && a.z >= 0)) return false;
    const int x = (int)a.x, y = (int)a.y, z = (int)a.z;
    if (x >= nx || y >= ny || z >= nz) return false;
    const size_t c = ((size_t)z * ny + y) * nx + x;
    out.clear();
    const uint32_t *i0 = items.data() + start[c], *i1 = items.data() + start[c + 1];
    std::merge(i0, i1, always.begin(), always.end(), std::back_inserter(out));
    return true;
  }
};
}  // namespace

Vec3 DynamicShape::pointVelocity(const Vec3& x) const { return linVel + cross(angVel, x - com); }

void buildMeshBVH(StaticShape& s) {
  s.bvh.clear();
  s.bvhTris.clear();
  const uint32_t N = (uint32_t)s.tris.size();
  s.triBox.assign(N, TriBox{});
  std::vector<uint32_t> items;
  std::vector<Vec3> cen(N);
  for (uint32_t i = 0; i < N; ++i) {
    const MeshTri& tr = s.tris[i];
    const Vec3 e1 = tr.v1 - tr.v0, e2 = tr.v2 - tr.v0;
    if (std::sqrt(cross(e1, e2).len2()) == 0) continue;  // testMesh returns early for these: never a candidate
    // Slivers go in the tree like the others: testMesh only keeps their impacts within the slop (see there).
    const float slop = triSlop(tr);
    const Vec3 lo = vmin(vmin(tr.v0, tr.v1), tr.v2), hi = vmax(vmax(tr.v0, tr.v1), tr.v2);
    s.triBox[i] = {lo - Vec3(slop, slop, slop), hi + Vec3(slop, slop, slop)};
    cen[i] = (tr.v0 + tr.v1 + tr.v2) * (1.0f / 3.0f);
    items.push_back(i);
  }
  s.bvhTris.reserve(items.size());
  if (!items.empty()) buildBVHNode(s, items, cen, 0, (uint32_t)items.size());
  buildMeshGrid(s, items);
}

// Cache retention applies to planes produced by the plane shape only [A, fitted]: steam_ceiling's box edges show the
// original does not keep un-hit box planes alive, while probe_rebound (plane) needs it.
static bool retainKind(int kind) { return kind == (int)StaticShape::Plane; }

Fluid::Fluid(const FluidParams& p) : P_(p) {
  // [A, fitted] zero dynamic friction behaves as 0.001 in the original (probe/slide_sf0.0_df0.0_*: tangential
  // speed x0.999 per contact frame at 1, 10 and 30 m/s).
  P_.frictionStatic = std::max(P_.frictionStatic, kMinFriction);
  P_.frictionDynamic = std::max(P_.frictionDynamic, kMinFriction);
  if (const char* e = getenv("PXR_FLUID_KEEPCACHE")) keepCache_ = atoi(e) != 0;
  spacing_ = 1.0f / P_.restParticlesPerMeter;
  sigma_ = 0.1f / spacing_;
  invSigma_ = 1.0f / sigma_;
  hw_ = P_.kernelRadiusMultiplier * spacing_;
  hw2_ = hw_ * hw_;
  h_ = 0.1f * P_.kernelRadiusMultiplier;
  h2_ = h_ * h_;
  float m = P_.restDensity * 0.001f;
  float h6 = h2_ * h2_ * h2_;
  poly6m_ = 315.0f / (h6 * h2_ * h_ * 64.0f * kPi) * m;
  pressC_ = 22.5f / (kPi * h6) * m;
  viscC_ = 45.0f / (kPi * h6) * m * P_.viscosity;
  selfRho_ = poly6m_ * h6;
  motionLimit_ = P_.motionLimitMultiplier * spacing_;
  collDist_ = P_.collisionDistanceMultiplier * spacing_;
}

uint32_t Fluid::addParticles(const std::vector<Vec3>& pos, const std::vector<Vec3>& vel) {
  std::vector<Vec3> v(pos.size());  // missing velocities are zero
  for (size_t i = 0; i < pos.size() && i < vel.size(); ++i) v[i] = vel[i];
  return addParticles(pos.data(), v.data(), nullptr, (uint32_t)pos.size(), nullptr);
}

uint32_t Fluid::addParticles(const Vec3* pos, const Vec3* vel, const float* life, uint32_t n, uint32_t* newIds) {
  const size_t room = P_.maxParticles > pos_.size() ? P_.maxParticles - pos_.size() : 0;
  const uint32_t m = (uint32_t)std::min<size_t>(n, room);
  for (uint32_t i = 0; i < m; ++i) {
    const Vec3 v = vel ? vel[i] : Vec3();
    const float L = life ? life[i] : 0.0f;
    pos_.push_back(pos[i]);
    vel_.push_back(v);
    force_.emplace_back();
    nrm_.emplace_back();
    rho_.push_back(0);
    press_.push_back(0);
    const uint32_t id = allocId();
    if (newIds) newIds[i] = id;
    id_.push_back(id);
    flags_.push_back(0);
    prevVel_.push_back(v);
    cache_.emplace_back();
    expiry_.push_back(L != 0 ? time_ + L : 0.0f);
    life_.push_back(L != 0 ? L : 0.0f);
    seq_.push_back(nextSeq_++);
    born_.push_back(steps_);
    owed_.emplace_back();
    owedBody_.push_back(0);
  }
  return m;
}

uint32_t Fluid::allocId() {
  if (!freeIds_.empty()) {
    const uint32_t id = *freeIds_.begin();
    freeIds_.erase(freeIds_.begin());
    return id;
  }
  return nextId_++;
}

void Fluid::deleteParticle(uint32_t id) { pendingDel_.push_back(id); }

void Fluid::addVelocityChange(uint32_t id, const Vec3& dv) { pendingVel_.emplace_back(id, dv); }

void Fluid::applyVelocityChanges() {
  if (pendingVel_.empty()) return;
  std::unordered_map<uint32_t, size_t> index;  // ids are unique among the live particles
  index.reserve(id_.size());
  for (size_t i = 0; i < id_.size(); ++i) index.emplace(id_[i], i);
  for (const auto& pv : pendingVel_) {
    auto it = index.find(pv.first);
    if (it != index.end()) vel_[it->second] += pv.second;
  }
  pendingVel_.clear();
}

float particleMass(const FluidParams& P) {
  return P.restDensity / (P.restParticlesPerMeter * P.restParticlesPerMeter * P.restParticlesPerMeter);
}

Vec3 forceVelocityChange(ForceMode mode, const Vec3& f, float simTime, float mass) {
  switch (mode) {
    case ForceMode::Impulse: return f * (1.0f / mass);
    case ForceMode::VelocityChange: return f;
    case ForceMode::Acceleration: return f * simTime;
    default: return f * (simTime / mass);  // Force, and the unsupported smooth modes
  }
}

void Fluid::removeAllParticles() { pendingRemoveAll_ = true; }

void Fluid::compact(const std::vector<uint8_t>& kill) {
  const size_t N = pos_.size();
  size_t w = 0;
  for (size_t r = 0; r < N; ++r) {
    if (kill[r]) {
      deleted_.push_back(id_[r]);
      freeIds_.insert(id_[r]);
      continue;
    }
    if (w != r) {
      pos_[w] = pos_[r];
      vel_[w] = vel_[r];
      force_[w] = force_[r];
      nrm_[w] = nrm_[r];
      rho_[w] = rho_[r];
      press_[w] = press_[r];
      id_[w] = id_[r];
      flags_[w] = flags_[r];
      prevVel_[w] = prevVel_[r];
      cache_[w] = cache_[r];
      expiry_[w] = expiry_[r];
      life_[w] = life_[r];
      seq_[w] = seq_[r];
      born_[w] = born_[r];
      owed_[w] = owed_[r];
      owedBody_[w] = owedBody_[r];
    }
    ++w;
  }
  pos_.resize(w);
  vel_.resize(w);
  force_.resize(w);
  nrm_.resize(w);
  rho_.resize(w);
  press_.resize(w);
  id_.resize(w);
  flags_.resize(w);
  prevVel_.resize(w);
  cache_.resize(w);
  expiry_.resize(w);
  life_.resize(w);
  seq_.resize(w);
  born_.resize(w);
  owed_.resize(w);
  owedBody_.resize(w);
}

void Fluid::applyPending() {
  if (pendingDel_.empty()) return;
  std::sort(pendingDel_.begin(), pendingDel_.end());
  std::vector<uint8_t> kill(pos_.size(), 0);
  for (size_t i = 0; i < pos_.size(); ++i)
    kill[i] = std::binary_search(pendingDel_.begin(), pendingDel_.end(), id_[i]);
  pendingDel_.clear();
  compact(kill);
}

void Fluid::evictPriority() {
  if (!P_.priorityMode) return;
  const size_t N = pos_.size();
  const size_t target = P_.maxParticles > P_.numReserveParticles ? P_.maxParticles - P_.numReserveParticles : 0;
  // Decided from the count at the end of the previous step (adds since then are only counted from this step on,
  // the reserve is their headroom) [V: api_priority, api_priority2]. Removes exactly that excess.
  if (lastCount_ <= target) return;
  const size_t excess = std::min(lastCount_ - target, N);
  if (excess == 0) return;
  // Finite lifetimes first, smallest remaining life first, ties oldest first; infinite lifetimes only after all
  // finite ones (oldest first).
  std::vector<uint32_t> order(N);
  for (uint32_t i = 0; i < N; ++i) order[i] = i;
  std::sort(order.begin(), order.end(), [&](uint32_t a, uint32_t b) {
    const bool fa = expiry_[a] != 0, fb = expiry_[b] != 0;
    if (fa != fb) return fa;
    if (fa && expiry_[a] != expiry_[b]) return expiry_[a] < expiry_[b];
    return seq_[a] < seq_[b];
  });
  std::vector<uint8_t> kill(N, 0);
  for (size_t k = 0; k < excess; ++k) kill[order[k]] = 1;
  compact(kill);
}

void Fluid::expire() {
  const size_t N = pos_.size();
  std::vector<uint8_t> kill(N, 0);
  bool any = false;
  for (size_t i = 0; i < N; ++i) {
    if (expiry_[i] == 0) {
      life_[i] = 0;
      continue;
    }
    const float rem = expiry_[i] - time_;
    if (rem <= 0) {
      kill[i] = 1;
      any = true;
    } else {
      life_[i] = rem;
    }
  }
  if (any) compact(kill);
}

void Fluid::computeBounds() {
  bLo_ = Vec3(std::numeric_limits<float>::max(), std::numeric_limits<float>::max(), std::numeric_limits<float>::max());
  bHi_ = Vec3(-std::numeric_limits<float>::max(), -std::numeric_limits<float>::max(), -std::numeric_limits<float>::max());
  for (const Vec3& p : pos_) {
    bLo_ = vmin(bLo_, p);
    bHi_ = vmax(bHi_, p);
  }
}

// SPH density and forces. The reference version (computeSphReference, PXF_SPH_REF=1) visits each pair (i < j) once
// in the order i ascending, j in cell order (cells z, y, x around i, ascending index within a cell), and adds the
// pair's term to both particles. This version gives every particle the same float operations in the same order but
// gathers them per particle, so particles are independent (parallel chunks): first the pairs with a lower partner
// (ascending partner index, as the reference's outer loop reaches them), then its own pairs in cell order. A pair's
// terms are evaluated from the lower particle's side, as in the reference (r2 is the same from both sides; the
// sums and products are commutative). The neighbour scan runs over cell-sorted coordinate arrays (vectorisable).
// Bit-identical to the reference.
// Splits n candidates (cell-ordered coordinates X/Y/Z, indices J) of particle i at (px, py, pz) into its neighbours
// (r2 < hw2): upper partners (j > i) in candidate order to UJ/UW, lower partners (j < i) to LO, each with the density
// term w = poly6m * (h2 - r2 * s2)^3. Same expressions as the reference (r2 = len2 of the difference, the product order
// of w). Returns the counts in nu/nl. The SSE2 path needs the arrays padded to a multiple of 4 (padding never passes:
// its coordinates are far away) and only touches the hits after the compare; the scalar loop is the portable fallback.
static void sphSplit(const float* __restrict X, const float* __restrict Y, const float* __restrict Z,
                     const uint32_t* __restrict J, uint32_t n, uint32_t i, float px, float py, float pz, float s2,
                     float h2, float pm, float hw2, uint32_t* __restrict UJ, float* __restrict UW,
                     std::pair<uint32_t, float>* __restrict LO, size_t& nuOut, size_t& nlOut) {
  size_t nu = 0, nl = 0;
#if defined(__SSE2__)
  const __m128 vpx = _mm_set1_ps(px), vpy = _mm_set1_ps(py), vpz = _mm_set1_ps(pz);
  const __m128 vs2 = _mm_set1_ps(s2), vh2 = _mm_set1_ps(h2), vpm = _mm_set1_ps(pm), vhw2 = _mm_set1_ps(hw2);
  const __m128i vi = _mm_set1_epi32((int32_t)i);  // particle indices < 2^31: signed compares are exact
  alignas(16) float w4[4];
  for (uint32_t k = 0; k < n; k += 4) {
    const __m128 ax = _mm_sub_ps(vpx, _mm_loadu_ps(X + k)), ay = _mm_sub_ps(vpy, _mm_loadu_ps(Y + k)),
                 az = _mm_sub_ps(vpz, _mm_loadu_ps(Z + k));
    const __m128 r2 = _mm_add_ps(_mm_add_ps(_mm_mul_ps(ax, ax), _mm_mul_ps(ay, ay)), _mm_mul_ps(az, az));
    const __m128 in = _mm_cmplt_ps(r2, vhw2);
    int mIn = _mm_movemask_ps(in);
    if (!mIn) continue;
    const __m128i vj = _mm_loadu_si128(reinterpret_cast<const __m128i*>(J + k));
    int mUp = mIn & _mm_movemask_ps(_mm_castsi128_ps(_mm_cmpgt_epi32(vj, vi)));
    int mLo = mIn & _mm_movemask_ps(_mm_castsi128_ps(_mm_cmpgt_epi32(vi, vj)));
    const __m128 kk = _mm_sub_ps(vh2, _mm_mul_ps(r2, vs2));
    _mm_store_ps(w4, _mm_mul_ps(_mm_mul_ps(_mm_mul_ps(vpm, kk), kk), kk));
    for (; mUp; mUp &= mUp - 1) {
      const int b = __builtin_ctz((unsigned)mUp);
      UJ[nu] = J[k + b], UW[nu] = w4[b], ++nu;
    }
    for (; mLo; mLo &= mLo - 1) {
      const int b = __builtin_ctz((unsigned)mLo);
      LO[nl++] = {J[k + b], w4[b]};
    }
  }
#else
  for (uint32_t k = 0; k < n; ++k) {
    const float ax = px - X[k], ay = py - Y[k], az = pz - Z[k];
    const float r2 = ax * ax + ay * ay + az * az;
    if (!(r2 < hw2) || J[k] == i) continue;
    const float kk = h2 - r2 * s2;
    const float w = pm * kk * kk * kk;
    if (J[k] > i) UJ[nu] = J[k], UW[nu] = w, ++nu;
    else LO[nl++] = {J[k], w};
  }
#endif
  nuOut = nu, nlOut = nl;
}

// Particles per SPH chunk (whole cells): small enough to balance 7-8 threads on a few thousand particles.
static constexpr uint32_t kSphGrain = 128;
void Fluid::computeSph() {
  static const bool ref = getenv("PXF_SPH_REF") != nullptr;
  if (ref) { computeSphReference(); return; }
  const uint32_t N = (uint32_t)pos_.size();
  if (N == 0) return;
  auto pack = [](int64_t x, int64_t y, int64_t z) {
    return (uint64_t)(((x & 0x1FFFFF) << 42) | ((y & 0x1FFFFF) << 21) | (z & 0x1FFFFF));
  };
  SphScratch& W = sph_;
  W.cell.resize(3 * (size_t)N);
  W.pc.resize(N);
  int64_t mn[3] = {std::numeric_limits<int64_t>::max(), std::numeric_limits<int64_t>::max(), std::numeric_limits<int64_t>::max()}, mx[3] = {std::numeric_limits<int64_t>::min(), std::numeric_limits<int64_t>::min(), std::numeric_limits<int64_t>::min()};
  static_assert(sizeof(Vec3) == 3 * sizeof(float), "flat coordinate access");
  // p / hw for all coordinates in one flat loop (vectorised; divps rounds like divss), then the floors.
  W.q.resize(3 * (size_t)N);
  {
    const float* __restrict P = &pos_[0].x;
    float* __restrict Q = W.q.data();
    const float hw = hw_;
    for (size_t k = 0; k < 3 * (size_t)N; ++k) Q[k] = P[k] / hw;
  }
  for (uint32_t i = 0; i < N; ++i) {
    const float* q = &W.q[3 * (size_t)i];
    const int64_t c[3] = {pxm::floorToInt64(q[0]), pxm::floorToInt64(q[1]), pxm::floorToInt64(q[2])};
    for (int a = 0; a < 3; ++a) {
      W.cell[3 * i + a] = (int32_t)c[a];
      mn[a] = std::min(mn[a], c[a]), mx[a] = std::max(mx[a], c[a]);
    }
    W.pc[i].i = i;
  }
  // Cell key = index of the cell in the particles' bounding box. Any numbering works: results only depend on which
  // particles share a cell and on their ascending index within it, which the stable LSD radix sort below keeps.
  const uint64_t R1 = (uint64_t)(mx[1] - mn[1] + 1), R2 = (uint64_t)(mx[2] - mn[2] + 1);
  const uint64_t span = (uint64_t)(mx[0] - mn[0] + 1) * R1 * R2;
  for (uint32_t i = 0; i < N; ++i) {
    const int32_t* c = &W.cell[3 * i];
    W.pc[i].key = (((uint64_t)(c[0] - mn[0]) * R1 + (uint64_t)(c[1] - mn[1])) * R2) + (uint64_t)(c[2] - mn[2]);
  }
  unsigned bits = 0;
  while (bits < 64 && (span - 1) >> bits) ++bits;
  W.pcTmp.resize(N);
  for (unsigned shift = 0; shift < bits; shift += 11) {
    uint32_t hist[2049] = {0};
    for (uint32_t k = 0; k < N; ++k) hist[((W.pc[k].key >> shift) & 2047) + 1]++;
    for (uint32_t d = 0; d < 2048; ++d) hist[d + 1] += hist[d];
    for (uint32_t k = 0; k < N; ++k) W.pcTmp[hist[(W.pc[k].key >> shift) & 2047]++] = W.pc[k];
    W.pc.swap(W.pcTmp);
  }
  W.keys.clear(), W.start.clear();
  W.sorted.resize(N), W.sx.resize(N), W.sy.resize(N), W.sz.resize(N);
  for (uint32_t k = 0; k < N; ++k) {
    const uint32_t i = W.pc[k].i;
    if (k == 0 || W.pc[k].key != W.pc[k - 1].key) {
      const int32_t* c = &W.cell[3 * i];
      W.keys.push_back(pack(c[0], c[1], c[2])), W.start.push_back(k);
    }
    W.sorted[k] = i, W.sx[k] = pos_[i].x, W.sy[k] = pos_[i].y, W.sz[k] = pos_[i].z;
  }
  W.start.push_back(N);
  // Cell lookup: a dense array over the particles' bounding box when it is small (direct index, no hashing), else the
  // hash table of packed keys. Both find the same cells: with every axis spanning < 2^20 cells, the 21-bit packed keys
  // of a cell and its neighbours equal only for equal coordinates.
  const uint64_t R0 = (uint64_t)(mx[0] - mn[0] + 1);
  W.dense.clear();
  if (span <= 8 * (uint64_t)N + 4096 && R0 < (1u << 20) && R1 < (1u << 20) && R2 < (1u << 20) &&
      mn[0] >= INT32_MIN + 1 && mn[1] >= INT32_MIN + 1 && mn[2] >= INT32_MIN + 1 && mx[0] < INT32_MAX &&
      mx[1] < INT32_MAX && mx[2] < INT32_MAX) {
    W.dense.assign((size_t)span, 0);
    for (int a = 0; a < 3; ++a) W.dmn[a] = (int32_t)mn[a], W.dext[a] = (uint32_t)(mx[a] - mn[a] + 1);
    for (uint32_t c = 0, k = 0; c < W.keys.size(); k = W.start[++c]) W.dense[(size_t)W.pc[k].key] = c + 1;
  } else {
    uint32_t tsize = 16;
    while (tsize < 2 * W.keys.size()) tsize <<= 1;
    W.mask = tsize - 1;
    W.table.assign(tsize, 0);
    for (uint32_t c = 0; c < W.keys.size(); ++c) {
      uint32_t h = SphScratch::hash(W.keys[c]) & W.mask;
      while (W.table[h]) h = (h + 1) & W.mask;
      W.table[h] = c + 1;
    }
  }
  // Chunks of whole cells (~kSphGrain particles each), in cell order: a cell's 27 neighbour cells are looked up once for
  // all its particles, and neighbouring particles share the cache.
  W.chunkCell.clear();
  W.chunkCell.push_back(0);
  for (uint32_t c = 0, acc = 0; c < (uint32_t)W.keys.size(); ++c) {
    acc += W.start[c + 1] - W.start[c];
    if (acc >= kSphGrain) W.chunkCell.push_back(c + 1), acc = 0;
  }
  if (W.chunkCell.back() != (uint32_t)W.keys.size()) W.chunkCell.push_back((uint32_t)W.keys.size());
  const size_t nChunks = W.chunkCell.size() - 1;
  W.chunks.resize(nChunks);
  W.upOff.resize(N), W.upCnt.resize(N), W.loOff.resize(N), W.loCnt.resize(N), W.inv.resize(N);
  auto runChunks = [&](void (*fn)(void*, size_t)) {
    if (parallelFor_ && nChunks > 1) parallelFor_(nChunks, fn, this);
    else for (size_t c = 0; c < nChunks; ++c) fn(this, c);
  };

  // 1) Neighbours of each particle (r2 < hw2): upper partners in cell order, lower partners ascending, with the
  // density term. A cell's candidates (its 27 neighbour cells, in the reference's order) are gathered once for all its
  // particles; each particle then splits them without branches (both lists are written, the counters advance only for
  // the ones that apply).
  runChunks([](void* p, size_t c) {
    Fluid& F = *static_cast<Fluid*>(p);
    SphScratch& W = F.sph_;
    SphScratch::Chunk& ch = W.chunks[c];
    ch.nUp = ch.nLo = 0;
    const float s2 = F.sigma_ * F.sigma_, h2 = F.h2_, pm = F.poly6m_, hw2 = F.hw2_;
    for (uint32_t cell = W.chunkCell[c]; cell < W.chunkCell[c + 1]; ++cell) {
      const uint32_t first = W.sorted[W.start[cell]];
      const int64_t cx = W.cell[3 * first], cy = W.cell[3 * first + 1], cz = W.cell[3 * first + 2];
      uint32_t nb[27], nn = 0;  // neighbour cells in the reference's z, y, x order
      if (!W.dense.empty()) {
        const uint32_t ux = (uint32_t)(cx - W.dmn[0]), uy = (uint32_t)(cy - W.dmn[1]), uz = (uint32_t)(cz - W.dmn[2]);
        for (int dz = -1; dz <= 1; ++dz) {
          if (uz + dz >= W.dext[2]) continue;  // outside the box (unsigned wrap for -1)
          for (int dy = -1; dy <= 1; ++dy) {
            if (uy + dy >= W.dext[1]) continue;
            for (int dx = -1; dx <= 1; ++dx) {
              if (ux + dx >= W.dext[0]) continue;
              const uint32_t ci = W.dense[((size_t)(ux + dx) * W.dext[1] + (uy + dy)) * W.dext[2] + (uz + dz)];
              if (ci) nb[nn++] = ci - 1;
            }
          }
        }
      } else {
        for (int64_t dz = -1; dz <= 1; ++dz)
          for (int64_t dy = -1; dy <= 1; ++dy)
            for (int64_t dx = -1; dx <= 1; ++dx) {
              const uint64_t key = (uint64_t)((((cx + dx) & 0x1FFFFF) << 42) | (((cy + dy) & 0x1FFFFF) << 21) | ((cz + dz) & 0x1FFFFF));
              uint32_t ci = 0;
              for (uint32_t h = SphScratch::hash(key) & W.mask; W.table[h]; h = (h + 1) & W.mask)
                if (W.keys[W.table[h] - 1] == key) { ci = W.table[h]; break; }
              if (ci) nb[nn++] = ci - 1;
            }
      }
      uint32_t M = 0;  // candidates of the cell, neighbour cells in order
      for (uint32_t m = 0; m < nn; ++m) M += W.start[nb[m] + 1] - W.start[nb[m]];
      const uint32_t M4 = (M + 3) & ~3u;  // padded to whole SIMD groups
      if (ch.cj.size() < M4 + 4) {
        ch.cj.resize(2 * M4 + 64), ch.cx.resize(2 * M4 + 64), ch.cy.resize(2 * M4 + 64), ch.cz.resize(2 * M4 + 64);
        ch.lo.resize(2 * M4 + 64);
      }
      for (uint32_t m = 0, o = 0; m < nn; ++m) {
        const uint32_t s = W.start[nb[m]], n = W.start[nb[m] + 1] - s;
        for (uint32_t k = 0; k < n; ++k, ++o)
          ch.cj[o] = W.sorted[s + k], ch.cx[o] = W.sx[s + k], ch.cy[o] = W.sy[s + k], ch.cz[o] = W.sz[s + k];
      }
      for (uint32_t o = M; o < M4; ++o) ch.cj[o] = 0, ch.cx[o] = ch.cy[o] = ch.cz[o] = FLT_MAX;  // r2 = inf: never a hit
      for (uint32_t si = W.start[cell]; si < W.start[cell + 1]; ++si) {
        const uint32_t i = W.sorted[si];
        const float px = W.sx[si], py = W.sy[si], pz = W.sz[si];
        if (ch.upJ.size() < ch.nUp + M + 1) ch.upJ.resize(2 * (ch.nUp + M) + 64), ch.upW.resize(2 * (ch.nUp + M) + 64);
        uint32_t* UJ = ch.upJ.data() + ch.nUp;
        float* UW = ch.upW.data() + ch.nUp;
        std::pair<uint32_t, float>* LO = ch.lo.data();
        size_t nu, nl;
        sphSplit(ch.cx.data(), ch.cy.data(), ch.cz.data(), ch.cj.data(), M4, i, px, py, pz, s2, h2, pm, hw2, UJ, UW, LO,
                 nu, nl);
        // lower partners ascending (insertion sort: a few dozen entries; the indices are distinct)
        for (size_t a = 1; a < nl; ++a) {
          const std::pair<uint32_t, float> v = LO[a];
          size_t b = a;
          for (; b > 0 && LO[b - 1].first > v.first; --b) LO[b] = LO[b - 1];
          LO[b] = v;
        }
        if (ch.loJ.size() < ch.nLo + nl) ch.loJ.resize(2 * (ch.nLo + nl) + 64), ch.loW.resize(2 * (ch.nLo + nl) + 64);
        W.upOff[i] = (uint32_t)ch.nUp;
        W.upCnt[i] = (uint32_t)nu;
        ch.nUp += nu;
        W.loOff[i] = (uint32_t)ch.nLo;
        W.loCnt[i] = (uint32_t)nl;
        for (size_t q = 0; q < nl; ++q) ch.loJ[ch.nLo + q] = LO[q].first, ch.loW[ch.nLo + q] = LO[q].second;
        ch.nLo += nl;
        // 2) Density and pressure (needs only this particle's lists): lower pairs ascending, then upper pairs in
        // cell order.
        float rho = F.selfRho_;
        for (size_t q = 0; q < nl; ++q) rho += LO[q].second;
        for (size_t q = 0; q < nu; ++q) rho += UW[q];
        F.rho_[i] = rho;
        W.inv[i] = 1.0f / rho;
        F.press_[i] = std::max(0.0f, (rho - F.P_.restDensity) * F.P_.stiffness);
        if (!nl && !nu) F.flags_[i] |= PF_SEPARATED;
      }
    }
  });

  // 3) Forces in the same order: force -= T / rho_lower over the lower pairs, += T / rho_upper over the upper ones,
  // with T(lo, hi) = d * (pt * sigma) + (v_hi - v_lo) * (sigma * vt), d = x_lo - x_hi.
  runChunks([](void* p, size_t c) {
    Fluid& F = *static_cast<Fluid*>(p);
    SphScratch& W = F.sph_;
    const SphScratch::Chunk& ch = W.chunks[c];
    auto pairT = [&](uint32_t a, uint32_t b, Vec3& T) {  // a < b
      const Vec3 d = F.pos_[a] - F.pos_[b];
      const float r2 = d.len2();
      const float r = std::sqrt(r2 * (F.sigma_ * F.sigma_));
      if (r == 0.0f) return false;
      const float pt = (F.press_[a] + F.press_[b]) * ((1.0f / r) * F.h2_ - (F.h_ + F.h_) + r) * F.pressC_;
      const float vt = (F.h_ - r) * F.viscC_;
      T = d * (pt * F.sigma_) + (F.vel_[b] - F.vel_[a]) * (F.sigma_ * vt);
      return true;
    };
    for (uint32_t si = W.start[W.chunkCell[c]]; si < W.start[W.chunkCell[c + 1]]; ++si) {
      const uint32_t k = W.sorted[si];
      Vec3 f, T;
      for (uint32_t q = W.loOff[k], qe = q + W.loCnt[k]; q < qe; ++q) {
        const uint32_t j = ch.loJ[q];
        if (pairT(j, k, T)) f -= T * W.inv[j];
      }
      for (uint32_t q = W.upOff[k], qe = q + W.upCnt[k]; q < qe; ++q) {
        const uint32_t j = ch.upJ[q];
        if (pairT(k, j, T)) f += T * W.inv[j];
      }
      F.force_[k] = f;
    }
  });
}

void Fluid::computeSphReference() {
  const size_t N = pos_.size();
  // uniform grid, cell = world kernel radius
  // exact cell key: 21 bits per axis (no collisions within ±1M cells)
  auto pack = [](int64_t x, int64_t y, int64_t z) {
    return ((x & 0x1FFFFF) << 42) | ((y & 0x1FFFFF) << 21) | (z & 0x1FFFFF);
  };
  auto key = [&](const Vec3& p) {
    return pack((int64_t)pxm::floorExact(p.x / hw_), (int64_t)pxm::floorExact(p.y / hw_), (int64_t)pxm::floorExact(p.z / hw_));
  };
  std::unordered_map<int64_t, std::vector<uint32_t>> grid;
  grid.reserve(N);
  for (uint32_t i = 0; i < N; ++i) grid[key(pos_[i])].push_back(i);
  auto forNeighbours = [&](uint32_t i, auto&& fn) {
    const Vec3& p = pos_[i];
    int64_t cx = (int64_t)pxm::floorExact(p.x / hw_), cy = (int64_t)pxm::floorExact(p.y / hw_), cz = (int64_t)pxm::floorExact(p.z / hw_);
    for (int64_t dz = -1; dz <= 1; ++dz)
      for (int64_t dy = -1; dy <= 1; ++dy)
        for (int64_t dx = -1; dx <= 1; ++dx) {
          auto it = grid.find(pack(cx + dx, cy + dy, cz + dz));
          if (it == grid.end()) continue;
          for (uint32_t j : it->second)
            if (j > i) fn(j);
        }
  };
  const float s2 = sigma_ * sigma_;
  for (size_t i = 0; i < N; ++i) { rho_[i] = selfRho_; force_[i] = Vec3(); }
  std::vector<uint8_t> hasNeighbour(N, 0);
  for (uint32_t i = 0; i < N; ++i)
    forNeighbours(i, [&](uint32_t j) {
      float r2 = (pos_[i] - pos_[j]).len2();
      if (r2 < hw2_) {
        float k = h2_ - r2 * s2;
        float w = poly6m_ * k * k * k;
        rho_[i] += w;
        rho_[j] += w;
        hasNeighbour[i] = hasNeighbour[j] = 1;
      }
    });
  for (size_t i = 0; i < N; ++i) {
    press_[i] = std::max(0.0f, (rho_[i] - P_.restDensity) * P_.stiffness);
    if (!hasNeighbour[i]) flags_[i] |= PF_SEPARATED;
  }
  for (uint32_t i = 0; i < N; ++i)
    forNeighbours(i, [&](uint32_t j) {
      Vec3 d = pos_[i] - pos_[j];
      float r2 = d.len2();
      if (r2 >= hw2_) return;
      float r = std::sqrt(r2 * s2);
      if (r == 0.0f) return;
      float pt = (press_[i] + press_[j]) * ((1.0f / r) * h2_ - (h_ + h_) + r) * pressC_;
      float vt = (h_ - r) * viscC_;
      Vec3 T = d * (pt * sigma_) + (vel_[j] - vel_[i]) * (sigma_ * vt);
      force_[i] += T * (1.0f / rho_[j]);
      force_[j] -= T * (1.0f / rho_[i]);
    });
}

void Fluid::integrate(float dt, const Vec3& gravity, bool sph) {
  struct Ctx {
    Fluid* f;
    Vec3 aExt;
    float dt, damp;
    bool sph;
  } ctx{this, P_.externalAcceleration + (P_.disableGravity ? Vec3() : gravity), dt,
        P_.damping * dt >= 1.0f ? 0.0f : 1.0f - P_.damping * dt, sph};
  auto range = [](void* p, size_t c) {  // per particle: chunks of 1024 may run in parallel
    const Ctx& x = *static_cast<const Ctx*>(p);
    Fluid& F = *x.f;
    const size_t e = std::min(F.pos_.size(), (c + 1) * 1024);
    for (size_t i = c * 1024; i < e; ++i) {
      F.prevVel_[i] = F.vel_[i];
      Vec3 a = x.aExt;
      if (x.sph) a += F.force_[i] * (F.invSigma_ / F.rho_[i]);
      Vec3 v = (F.vel_[i] + a * x.dt) * x.damp;
      float m = std::sqrt(v.len2()) * x.dt;
      if (m > F.motionLimit_) v = v * (F.motionLimit_ / m);
      F.vel_[i] = v;
    }
  };
  const size_t nChunks = (pos_.size() + 1023) / 1024;
  if (parallelFor_ && nChunks > 1) parallelFor_(nChunks, range, &ctx);
  else for (size_t c = 0; c < nChunks; ++c) range(&ctx, c);
}

void Fluid::collide(float dt, const std::vector<const StaticShape*>& statics, const std::vector<DynamicShape>& dynamics) {
  const float particleMass = P_.restDensity / (P_.restParticlesPerMeter * P_.restParticlesPerMeter * P_.restParticlesPerMeter);
  // Proximity distance (collide context +0x30 = PxsFluid +0xe4 = 2·collisionDistance [V], probe_prox2*/probe_prox2box/
  // probe_prox2shape): box, sphere and capsule cache a plane out to 2·cd. The plane shape behaves as if its proximity
  // distance were cd (probe_proxdist: nothing cached at 1.005·cd). The epsilon only absorbs float rounding of resting
  // contacts: probe/slide_* need it >= ~5e-7 (1.0 fails), and mesh_drop_plane needs it < 1.2e-5 (its second contact
  // ends at 1.000012·cd, which the original does not cache). 3e-6 is inside that bracket [V]. [A: why the plane
  // differs; 0x101ac060 reads the same two distances].
  const float proxPlane = collDist_ * 1.000003f, proxOther = collDist_ * 2.0f;
  // Friction of the cached-plane (sustained contact) pass, rescaled to the step size with refDt (see FluidParams).
  float susFricStatic = P_.frictionStatic, susFricDynamic = P_.frictionDynamic;
  if (P_.refDt > 0.0f && dt != P_.refDt) {
    const float r = dt / P_.refDt;
    auto decay = [r](float f) { return f >= 1.0f ? f : f <= 0.0f ? 0.0f : 1.0f - std::pow(1.0f - f, r); };
    susFricStatic = decay(susFricStatic);
    susFricDynamic = decay(susFricDynamic);
  }
  std::vector<DynCull> cull(dynamics.size());
  const bool noCull = getenv("PXF_NOCULL") != nullptr;  // exactness check: test every pair
  for (size_t k = 0; k < dynamics.size(); ++k) cull[k] = makeCull(dynamics[k], 2.0f * proxOther + collDist_);
  std::vector<StaticCull> scull(statics.size());
  for (size_t k = 0; k < statics.size(); ++k) {
    scull[k] = staticBounds(*statics[k]);
    if (noCull) scull[k].always = true;
  }
  const float sreach = 2.0f * proxOther + collDist_;
  // Broadphase grids over the particles' start positions. A static is listed where its bounds grown by sreach + dMax
  // reach, a dynamic where its cull sphere for a move of up to dMax reaches; a particle whose move this step is
  // longer than dMax (or that starts outside the grid) scans every shape. dMax covers the integrated motion (motion
  // limit) and the push-outs of the cached-plane and dynamic passes in the usual case.
  const float dMax = motionLimit_ * 1.01f + 4.0f * collDist_;
  ShapeGrid sgrid, dgrid;
  const bool useGrid = !noCull && getenv("PXF_NOGRID") == nullptr && pos_.size() >= 32;
  if (useGrid && (statics.size() >= 8 || dynamics.size() >= 8)) {
    Vec3 plo = pos_[0], phi = pos_[0];
    for (const Vec3& p : pos_) plo = vmin(plo, p), phi = vmax(phi, p);
    const float cell = 2.0f * (sreach + dMax) + motionLimit_;
    std::vector<Vec3> blo, bhi;
    std::vector<uint8_t> all;
    if (statics.size() >= 8) {
      const float g = sreach + dMax;
      blo.resize(statics.size()), bhi.resize(statics.size()), all.resize(statics.size());
      for (size_t k = 0; k < statics.size(); ++k) {
        all[k] = scull[k].always;
        blo[k] = scull[k].lo - Vec3(g, g, g), bhi[k] = scull[k].hi + Vec3(g, g, g);
      }
      sgrid.build(plo, phi, cell, blo, bhi, all);
    }
    if (dynamics.size() >= 8) {
      blo.resize(dynamics.size()), bhi.resize(dynamics.size()), all.resize(dynamics.size());
      for (size_t k = 0; k < dynamics.size(); ++k) {
        const DynCull& c = cull[k];
        all[k] = c.keep <= 0 || c.base == FLT_MAX;
        // culled() keeps the pair while |old - tb| * keep <= (base + move) * 1.001
        const float r = all[k] ? 0.0f : (c.base + dMax) * 1.001f / c.keep * 1.0001f + 1e-6f;
        blo[k] = c.tb - Vec3(r, r, r), bhi[k] = c.tb + Vec3(r, r, r);
      }
      dgrid.build(plo, phi, cell, blo, bhi, all);
    }
  }
  static thread_local std::vector<uint32_t> cand;
  drainHit_.assign(pos_.size(), 0);
  auto one = [&](size_t i, std::vector<BodyImpulse>& imp) {
    Vec3 old = pos_[i], v = vel_[i];
    Vec3 motion = v * dt;
    bool drainHit = false;  // a drain shape was in contact or proximity with this particle this step
    const bool drainArmed = steps_ - born_[i] >= P_.drainLatencySteps;
    // Analytic shape tests wait for the particle's first newParticleShapeLatency steps (mesh tests do not; see FluidParams).
    const bool shapeLatent = steps_ - born_[i] < P_.newParticleShapeLatency;
    // 1) previous step's contact planes (see docs: 0x10199aa0)
    Cache& K = cache_[i];
    bool hitPlane[2] = {false, false};
    if (K.p[0].valid || K.p[1].valid) {
      if (!K.p[0].valid) { K.p[0] = K.p[1]; K.p[1].valid = false; }
      const CachedPlane& A = K.p[0];
      const CachedPlane& B = K.p[1];
      bool two = B.valid;
      float mlen = std::sqrt(motion.len2());
      Vec3 q = old + motion;
      float fA = A.n.dot(q), fB = two ? B.n.dot(q) : 0;
      bool hitA = fA < A.d, hitB = two && fB < B.d;
      hitPlane[0] = hitA; hitPlane[1] = hitB;
      bool touched = false;
      const Vec3 motionIn = motion;
      const CachedPlane& C = hitA ? A : B;
      if (hitA && hitB) {
        motion = motion * (1.0f - (A.dyn ? susFricDynamic : susFricStatic));
        touched = true;
      } else if (hitA || hitB) {
        Vec3 vv;
        if (C.dyn)  // relative to the surface velocity stored with the plane [A: same form as the main pass]
          vv = C.vs + respond(motion * (1.0f / dt) - C.vs, prevVel_[i] - C.vs, C.n, susFricDynamic, P_.restitutionDynamic);
        else
          vv = respond(motion * (1.0f / dt), prevVel_[i], C.n, susFricStatic, P_.restitutionStatic);
        motion = vv * dt;
        touched = true;
      }
      if (touched) {
        flags_[i] |= C.dyn ? PF_DYNAMIC_CONTACT : PF_STATIC_CONTACT;
        Vec3 p = old + motion;
        float nAB = two ? A.n.dot(B.n) : 1.0f;
        if (!two || std::fabs(nAB) > 0.999999f) {
          float fp = A.n.dot(p);
          if (fp < A.d) {
            p = p + A.n * ((A.d - fp) * 1.01f);
            motion = p - old;
          }
        } else {
          float gA = A.d - A.n.dot(p), gB = B.d - B.n.dot(p);
          if (!(gA <= 0 && gB <= 0)) {
            float fAlongB = A.n.dot(B.n * gB + p);
            if (A.d <= fAlongB || B.d <= B.n.dot(A.n * gA + p)) {
              if (A.d <= fAlongB) p = p + B.n * (gB * 1.0001f);
              else p = p + A.n * (gA * 1.0001f);
              motion = p - old;
            } else {
              float inv = 1.0f / (1.0f - nAB * nAB);
              float a = (gA - gB * nAB) * inv, b = (gB - gA * nAB) * inv;
              motion = (p + A.n * a + B.n * b) - old;
              float m2 = motion.len2();
              motion += (A.n + B.n) * (0.5f * 1e-4f * m2);
            }
          }
        }
        float ml = std::sqrt(motion.len2());
        if (ml > mlen && ml > 0) motion = motion * (mlen / ml);
        v = motion * (1.0f / dt);
        // two-way: the cached-plane path stores the displacement change / dt [re-notes-fluid-mesh 2.2]
        if (P_.twoWay && C.dyn && C.twoWay) owe(i, C.body, (motionIn - motion) * (1.0f / dt) * (P_.collisionResponseCoefficient * particleMass), old, imp);
      }
    }
    Vec3 target = old + motion;
    // 2) dynamic shapes, resolved first with the dynamic coefficients and the surface velocity; the static pass
    // below starts from that result, so a static shape has the last word (dyn_order) [V].
    bool dynHit = false;
    Contact cdyn;  // also holds the dynamic shapes' cache candidates
    if (!dynamics.empty()) {
      Contact& c = cdyn;
      bool near = false;
      const float move = std::sqrt((target - old).len2());
      auto testDyn = [&](size_t k) {
        if (!groups_.pass(group_, dynamics[k].group)) return;
        if (shapeLatent && dynamics[k].geom->type != StaticShape::Mesh) return;
        bool nearK = false;  // this shape's contact or proximity
        testDynamic(dynamics[k], old, target, collDist_, proxOther, c, nearK);
        if (nearK) {
          near = true;
          if (dynamics[k].drain) drainHit = true;
        }
      };
      if (dgrid.active && move <= dMax && dgrid.query(old, cand)) {
        for (uint32_t k : cand)
          if (!culled(cull[k], old, move)) testDyn(k);
      } else {
        for (size_t k = 0; k < dynamics.size(); ++k)
          if (noCull || !culled(cull[k], old, move)) testDyn(k);
      }
      Vec3 n, p, vs;
      if (c.impact) { n = c.n; p = c.p; vs = c.vs; dynHit = true; }
      else if (c.count) { n = normalize(c.nSum); p = c.pSum * (1.0f / c.count); vs = c.vsSum * (1.0f / c.count); dynHit = true; }
      if (dynHit) {
        const Vec3 vr = v - vs, vBefore = v;
        const float vn = n.dot(vr);
        if (vn < 0) v = vs + (vr - n * vn) * (1.0f - P_.frictionDynamic) - n * (vn * P_.restitutionDynamic);
        nrm_[i] = n;
        target = p;
        // two-way: the particle owes the body the momentum the dynamic pass took from it
        if (P_.twoWay && c.shape && c.shape->twoWay)
          owe(i, c.shape->body, (vBefore - v) * (P_.collisionResponseCoefficient * particleMass), old, imp);
      }
      if (dynHit || near) flags_[i] |= PF_DYNAMIC_CONTACT;
    }
    Contact c;
    const Vec3 slo = Vec3(std::min(old.x, target.x), std::min(old.y, target.y), std::min(old.z, target.z)) - Vec3(sreach, sreach, sreach);
    const Vec3 shi = Vec3(std::max(old.x, target.x), std::max(old.y, target.y), std::max(old.z, target.z)) + Vec3(sreach, sreach, sreach);
    auto testStatic = [&](size_t k) {
      const StaticCull& b = scull[k];
      if (!b.always && (b.lo.x > shi.x || b.lo.y > shi.y || b.lo.z > shi.z || b.hi.x < slo.x || b.hi.y < slo.y || b.hi.z < slo.z)) return;
      const StaticShape& s = *statics[k];
      if (!groups_.pass(group_, s.group)) return;
      if (shapeLatent && s.type != StaticShape::Mesh) return;
      c.curType = (int)s.type;
      const int nPl = c.nPlanes, cnt = c.count;
      const bool imp = c.impact;
      const float t0 = c.t;
      testShape(s, old, target, collDist_, s.type == StaticShape::Plane ? proxPlane : proxOther, c);
      if (s.drain && (c.nPlanes != nPl || c.count != cnt || c.impact != imp || c.t != t0)) drainHit = true;
    };
    const Vec3 dmove = target - old;
    if (sgrid.active && std::fabs(dmove.x) <= dMax && std::fabs(dmove.y) <= dMax && std::fabs(dmove.z) <= dMax &&
        sgrid.query(old, cand)) {
      for (uint32_t k : cand) testStatic(k);
    } else {
      for (size_t k = 0; k < statics.size(); ++k) testStatic(k);
    }
    Vec3 n, p;
    bool hit = false;
    if (c.impact) { n = c.n; p = c.p; hit = true; }
    else if (c.count) { n = normalize(c.nSum); p = c.pSum * (1.0f / c.count); hit = true; }
    if (hit) {
      float vn = n.dot(v);
      if (vn < 0) {
        Vec3 vt = (v - n * vn) * (1.0f - P_.frictionStatic);
        v = vt - n * (vn * P_.restitutionStatic);
      }
      flags_[i] |= PF_STATIC_CONTACT;
      nrm_[i] = n;
      target = p;
    } else if (!dynHit) {
      nrm_[i] = Vec3();
    }
    // Retention [A, fitted to probe_rebound and sph_dambreak particle 198; the mechanism is not visible in
    // re_fluidCollidePacket]: a cached plane that was not hit this step survives while the particle ends
    // within collDist of it; a plane that was hit is consumed and only new contacts re-create it. Plane-shape planes
    // only (retainKind): box planes are not retained in the original (steam_ceiling).
    if (keepCache_ && !c.nPlanes && !cdyn.nPlanes)
      for (int k = 0; k < 2; ++k)
        if (K.p[k].valid && !hitPlane[k] && retainKind(K.p[k].kind) && K.p[k].n.dot(target) - K.p[k].d <= collDist_)
          { c.curType = K.p[k].kind; c.addPlane(K.p[k].n, K.p[k].n * K.p[k].d); }
    // new cache: keep the two candidate planes nearest to the start position
    K.p[0].valid = K.p[1].valid = false;
    for (int k = 0; k < cdyn.nPlanes + c.nPlanes; ++k) {
      const Contact& src = k < cdyn.nPlanes ? cdyn : c;
      const int j = k < cdyn.nPlanes ? k : k - cdyn.nPlanes;
      CachedPlane cp{src.cn[j], src.cd[j], true, src.ck[j]};
      if (const DynamicShape* d = src.cdyn[j]) {
        cp.dyn = true;
        cp.twoWay = d->twoWay;
        cp.body = d->body;
        cp.vs = src.cvs[j];
      }
      float dist = cp.n.dot(old) - cp.d;
      if (!K.p[0].valid) K.p[0] = cp;
      else if (!K.p[1].valid) K.p[1] = cp;
      else {
        float d0 = K.p[0].n.dot(old) - K.p[0].d, d1 = K.p[1].n.dot(old) - K.p[1].d;
        int far = d0 > d1 ? 0 : 1;
        if (dist < std::max(d0, d1)) K.p[far] = cp;
      }
    }
    Vec3 disp = target - old;
    float dl = std::sqrt(disp.len2());
    if (dl > motionLimit_) target = old + disp * (motionLimit_ / dl);
    if (dl >= motionLimit_ * 0.99999f) flags_[i] |= PF_MOTION_LIMIT;
    pos_[i] = target;
    vel_[i] = v;
    drainHit_[i] = drainHit && drainArmed;
  };
  // Particles are independent here; chunks of a fixed size may run on several threads (setParallelFor). Two-way
  // settlements are gathered per chunk and appended in chunk order, so the result does not depend on the threads.
  const size_t n = pos_.size(), grain = 256, nChunks = (n + grain - 1) / grain;
  if (nChunks <= 1 || !parallelFor_) {
    for (size_t i = 0; i < n; ++i) one(i, impulses_);
    return;
  }
  std::vector<std::vector<BodyImpulse>> chunkImp(nChunks);
  struct Ctx {
    decltype(one)* fn;
    std::vector<std::vector<BodyImpulse>>* imp;
    size_t n, grain;
  } ctx{&one, &chunkImp, n, grain};
  parallelFor_(nChunks, [](void* p, size_t c) {
    Ctx& x = *static_cast<Ctx*>(p);
    const size_t e = std::min(x.n, (c + 1) * x.grain);
    for (size_t i = c * x.grain; i < e; ++i) (*x.fn)(i, (*x.imp)[c]);
  }, &ctx);
  for (const auto& v : chunkImp) impulses_.insert(impulses_.end(), v.begin(), v.end());
}

void Fluid::step(float dt, const Vec3& gravity, const std::vector<StaticShape>& statics) {
  std::vector<const StaticShape*> ptrs;
  ptrs.reserve(statics.size());
  for (const StaticShape& sh : statics) ptrs.push_back(&sh);
  step(dt, gravity, ptrs);
}

void Fluid::owe(size_t i, uint32_t body, const Vec3& j, const Vec3& at, std::vector<BodyImpulse>& out) {
  if (owedBody_[i] != body && owed_[i].len2() > 0) {
    out.push_back({owedBody_[i], owed_[i], at});  // switching bodies: settle the old one at once [A]
    owed_[i] = Vec3();
  }
  owed_[i] += j;
  owedBody_[i] = body;
}

void Fluid::transferImpulses() {
  // 2.8.1 oracle [V]: the body receives 10% of the owed momentum per step, starting on the contact step, and the
  // rest stays with the particle (0.9 per step) even without further contact; it stops when the particle is gone.
  for (size_t i = 0; i < pos_.size(); ++i) {
    if (owed_[i].len2() == 0) continue;
    impulses_.push_back({owedBody_[i], owed_[i] * 0.1f, pos_[i]});
    owed_[i] = owed_[i] * 0.9f;
    if (owed_[i].len2() < 1e-20f) owed_[i] = Vec3();
  }
}

void Fluid::step(float dt, const Vec3& gravity, const std::vector<const StaticShape*>& statics,
                 const std::vector<DynamicShape>& dynamics) {
  deleted_.clear();
  impulses_.clear();
  // removeAll is applied at the end of this step (after the bounds), as in the original: the world bounds of
  // that step still include the removed particles (api_life step 40). Deletes by id apply now.
  if (!pendingRemoveAll_) applyPending();
  applyVelocityChanges();
  evictPriority();
  for (auto& f : flags_) f = 0;
  bool sph = P_.method == SimMethod::SPH || (P_.method == SimMethod::Mixed && !mixedPhase_);
  if (sph) computeSph();
  else
    for (auto& f : flags_) f |= PF_SEPARATED;
  integrate(dt, gravity, sph);
  collide(dt, statics, dynamics);
  // Drain: a particle that touched a drain shape (contact or proximity) this step is deleted at the start of the next
  // step, as deleteParticle does; it still took its collision response in this step.
  for (size_t i = 0; i < drainHit_.size(); ++i)
    if (drainHit_[i]) pendingDel_.push_back(id_[i]);
  if (P_.twoWay) transferImpulses();
  if (P_.method == SimMethod::Mixed) mixedPhase_ = !mixedPhase_;
  time_ += dt;
  ++steps_;
  expire();
  if (!pendingDel_.empty() && !deleted_.empty()) {  // a drained particle that expired in this step is already gone
    std::vector<uint32_t> gone = deleted_;
    std::sort(gone.begin(), gone.end());
    pendingDel_.erase(std::remove_if(pendingDel_.begin(), pendingDel_.end(),
                                     [&](uint32_t id) { return std::binary_search(gone.begin(), gone.end(), id); }),
                      pendingDel_.end());
  }
  computeBounds();
  if (pendingRemoveAll_) {
    pendingRemoveAll_ = false;
    pendingDel_.clear();
    compact(std::vector<uint8_t>(pos_.size(), 1));
  }
  lastCount_ = pos_.size();
}

}  // namespace pxr
