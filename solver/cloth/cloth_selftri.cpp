// Particle-triangle self contact (not in the original; ClothParams::selfContactDist > 0). The original's self-collision
// (cloth_self.cpp) only keeps particles a thickness apart from other particles; the game's cloths are 0.01 thick with
// vertices far further apart, so torn pieces lying on each other and wind folds passed straight through each other.
//
// Per step (selfContactFind, after the prediction p = x + v dt): every vertex is matched against the triangles of the
// same cloth near its path through a hashed grid of triangle boxes (built once per step). A pair is kept when the vertex
// is over the triangle (its projection inside, at the start or the end of the step) and either closer than h to the
// triangle's plane or crossing it during the step. The side it starts on is recorded (a vertex lying exactly in the
// plane, e.g. two torn pieces settled on the same floor, takes the side against gravity).
// Per iteration (selfContactSolve, after the edge solve): each pair is projected so the vertex stays on its side at
// least h from the triangle, moving the vertex and the triangle's corners by their inverse masses and barycentric
// weights (a point-triangle distance constraint).
// Triangles touching the vertex or one of its mesh neighbours are skipped (the cloth's own bending would otherwise fight
// the constraint); the pieces of a torn cloth are no longer neighbours and collide everywhere.
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <mutex>
#include <vector>

#include "../fastfloor.h"
#include "cloth.h"

#if defined(__SSE2__)
#include <emmintrin.h>
#include <xmmintrin.h>
#endif

namespace pxc {

namespace {
inline V3 vmin(const V3& a, const V3& b) { return {std::min(a.x, b.x), std::min(a.y, b.y), std::min(a.z, b.z)}; }
inline V3 vmax(const V3& a, const V3& b) { return {std::max(a.x, b.x), std::max(a.y, b.y), std::max(a.z, b.z)}; }
inline float lenOf(const V3& a) { return std::sqrt(a.len2()); }
inline uint32_t cellHash(int x, int y, int z) {
  return ((uint32_t)x * 73856093u) ^ ((uint32_t)y * 19349663u) ^ ((uint32_t)z * 83492791u);
}
constexpr uint16_t kGiveUp = 30;   // steps a pair may stay unresolved before the solve leaves it alone
constexpr uint16_t kSideMemory = 30;  // steps a pair's side is remembered after it leaves the contact range
// Edge-edge pairs (selfEdgeFind): OFF. In a violently torn pile they fought the cloth's bending at tight creases
// (constant agitation, runaway tearing, 9 ms per step). See docs/re-notes-cloth.md for the rejected experiment.
constexpr bool kEdgeEdge = false;
constexpr float kBaryTol = 0.02f;  // projection may sit this far outside the triangle (closes gaps between triangles)
constexpr uint32_t kNoTri = 0xffffffffu;  // padding entry of a grid bucket

// Barycentric coordinates (b0, b1, b2) of q's projection onto the plane of triangle (a, b, c) with unit normal n.
inline void bary(const V3& q, const V3& a, const V3& b, const V3& c, float out[3]) {
  const V3 e0 = b - a, e1 = c - a, e2 = q - a;
  const float d00 = e0.dot(e0), d01 = e0.dot(e1), d11 = e1.dot(e1), d20 = e2.dot(e0), d21 = e2.dot(e1);
  const float den = d00 * d11 - d01 * d01;
  if (!(den > 0.f)) {
    out[0] = out[1] = out[2] = -1.f;
    return;
  }
  const float inv = 1.f / den;
  out[1] = (d11 * d20 - d01 * d21) * inv;
  out[2] = (d00 * d21 - d01 * d20) * inv;
  out[0] = 1.f - out[1] - out[2];
}
// Box overlap of four SoA boxes (B: lo x/y/z, then hi x/y/z, four lanes each) with lo..hi, as a lane mask. A NaN box
// passes, as in the scalar test it stands in front of.
inline int overlap4(const float* B, const V3& lo, const V3& hi) {
#if defined(__SSE2__)
  const __m128 ok = _mm_and_ps(
      _mm_and_ps(_mm_and_ps(_mm_cmpnlt_ps(_mm_loadu_ps(B + 12), _mm_set1_ps(lo.x)),
                            _mm_cmpnlt_ps(_mm_loadu_ps(B + 16), _mm_set1_ps(lo.y))),
                 _mm_and_ps(_mm_cmpnlt_ps(_mm_loadu_ps(B + 20), _mm_set1_ps(lo.z)),
                            _mm_cmpngt_ps(_mm_loadu_ps(B + 0), _mm_set1_ps(hi.x)))),
      _mm_and_ps(_mm_cmpngt_ps(_mm_loadu_ps(B + 4), _mm_set1_ps(hi.y)), _mm_cmpngt_ps(_mm_loadu_ps(B + 8), _mm_set1_ps(hi.z))));
  return _mm_movemask_ps(ok);
#else
  int m = 0;
  for (int j = 0; j < 4; ++j)
    if (!(B[12 + j] < lo.x || B[16 + j] < lo.y || B[20 + j] < lo.z || B[0 + j] > hi.x || B[4 + j] > hi.y || B[8 + j] > hi.z))
      m |= 1 << j;
  return m;
#endif
}
// Boxes of the triangles in `ids` (kNoTri: padding, a box nothing overlaps) as SoA groups of four.
template <class Box>
void packBoxes(const std::vector<Box>& tb, const uint32_t* ids, size_t n, std::vector<float>& out) {
  const size_t ng = (n + 3) / 4;
  out.resize(ng * 24);
  for (size_t g = 0; g < ng; ++g) {
    float* B = &out[g * 24];
    for (int j = 0; j < 4; ++j) {
      const size_t k = g * 4 + j;
      const uint32_t t = k < n ? ids[k] : 0xffffffffu;
      const bool pad = t == 0xffffffffu;
      const V3 lo = pad ? V3(1e30f, 1e30f, 1e30f) : tb[t].lo, hi = pad ? V3(-1e30f, -1e30f, -1e30f) : tb[t].hi;
      B[0 * 4 + j] = lo.x, B[1 * 4 + j] = lo.y, B[2 * 4 + j] = lo.z;
      B[3 * 4 + j] = hi.x, B[4 * 4 + j] = hi.y, B[5 * 4 + j] = hi.z;
    }
  }
}
inline bool overTri(const float b[3]) { return b[0] >= -kBaryTol && b[1] >= -kBaryTol && b[2] >= -kBaryTol; }
// A vertex-triangle pair moves each particle by its share (w for the vertex, w b for a corner) over W; this is the
// smallest W that keeps every share at most 1 (no particle moves further than the pair's correction).
inline float shareCap(float wv, float w0, float w1, float w2, const float b[3]) {
  return std::max(std::max(wv, w0 * b[0]), std::max(w1 * b[1], w2 * b[2]));
}
}  // namespace

void Cloth::selfContactFind(const V3& gravity, float dt) {
  // Sides of last step's pairs: a pair that persists keeps its side. Re-deriving it from the start positions let a
  // vertex the other constraints had pushed just past a triangle switch sides, and be pushed back the next step:
  // pieces piled on the floor jittered forever.
  // Pairs that left the range are remembered for kSideMemory steps: a vertex drifting in and out of a triangle's reach
  // (wind on a crumpled piece) came back with the side of the moment and was pushed through (game replay: cloth001,
  // a fold flickering for seconds).
  {
    std::vector<PrevPair>& old = scPrevOld_;
    old.swap(scPrev_);
    scPrev_.clear();
    for (const SelfContact& c : sc_) scPrev_.push_back({((uint64_t)c.v << 32) | c.t, c.s, c.stuck, 0});
    auto byKey = [](const PrevPair& a, const PrevPair& b) { return a.key < b.key; };
    std::sort(scPrev_.begin(), scPrev_.end(), byKey);
    const size_t live = scPrev_.size();
    for (const PrevPair& o : old)
      if (o.age + 1 < kSideMemory &&
          !std::binary_search(scPrev_.begin(), scPrev_.begin() + live, o, byKey))
        scPrev_.push_back({o.key, o.side, o.stuck, (uint16_t)(o.age + 1)});
    std::inplace_merge(scPrev_.begin(), scPrev_.begin() + live, scPrev_.end(), byKey);
  }
  if (scPrevTris_ != tri_) scPrev_.clear();  // triangles renumbered by tearing: forget
  scPrevTris_ = tri_;
  sc_.clear();
  const float h = P_.selfContactDist;
  const size_t nt = tri_.size() / 3, nv = x_.size();
  if (!(h > 0.f) || nt == 0) {
    ut_.clear(), utReg_.clear();
    return;
  }
  // Per triangle, once per step: its box over the step (start and predicted corners) grown by h, and its plane at the
  // start (n0, o0) and at the prediction (n1, o1). A degenerate triangle gets a box no query can meet.
  std::vector<TriBox>& tb = scTriBox_;
  std::vector<TriPlanes>& pl = scPlanes_;
  tb.resize(nt);
  pl.resize(nt);
  float ext = 0.f;
  size_t live = 0;
  const uint8_t* rm = anyRemoved_ ? removed_.data() : nullptr;  // a triangle with a removed corner is degenerate
  for (size_t t = 0; t < nt; ++t) {
    const uint32_t i0 = tri_[3 * t], i1 = tri_[3 * t + 1], i2 = tri_[3 * t + 2];
    const V3 &a = x_[i0], &b = x_[i1], &c = x_[i2], &pa = p_[i0], &pb = p_[i1], &pc = p_[i2];
    V3 n0 = (b - a).cross(c - a), n1 = (pb - pa).cross(pc - pa);
    const float l0 = lenOf(n0), l1 = lenOf(n1);
    TriPlanes& q = pl[t];
    if ((rm && (rm[i0] || rm[i1] || rm[i2])) || !(l0 > 1e-20f) || !(l1 > 1e-20f)) {
      tb[t].lo = V3(1e30f, 1e30f, 1e30f);
      tb[t].hi = V3(-1e30f, -1e30f, -1e30f);
      q = TriPlanes{};
      continue;
    }
    n0 = n0 * (1.f / l0);
    n1 = n1 * (1.f / l1);
    q = TriPlanes{{n0.x, n0.y, n0.z, n0.dot(a)}, {n1.x, n1.y, n1.z, n1.dot(pa)}};
    V3 lo = vmin(vmin(a, b), vmin(c, vmin(pa, vmin(pb, pc))));
    V3 hi = vmax(vmax(a, b), vmax(c, vmax(pa, vmax(pb, pc))));
    tb[t].lo = lo - V3(h, h, h);
    tb[t].hi = hi + V3(h, h, h);
    const V3 e = tb[t].hi - tb[t].lo;
    ext += std::max(e.x, std::max(e.y, e.z));
    ++live;
  }
  if (live == 0) {
    ut_.clear(), utReg_.clear();
    return;
  }
  const float cell = std::max(ext / (float)live, 1e-6f);
  const float inv = 1.f / cell;
  uint32_t T = 16;
  while (T < 2 * nt) T <<= 1;
  const uint32_t mask = T - 1;
  // Counting sort of (cell hash, triangle) entries. A triangle spanning more than 64 cells (a stretched outlier) goes
  // to `big`, a candidate of every vertex.
  std::vector<uint32_t>& start = scStart_;
  std::vector<uint32_t>& items = scItems_;
  std::vector<uint32_t>& big = scBig_;
  std::vector<uint8_t>& isBig = scIsBig_;
  start.assign(T + 1, 0);
  big.clear();
  isBig.assign(nt, 0);
  auto cellRange = [&](const V3& lo, const V3& hi, int c0[3], int c1[3]) {
    c0[0] = (int)pxm::floorExact(lo.x * inv), c1[0] = (int)pxm::floorExact(hi.x * inv);
    c0[1] = (int)pxm::floorExact(lo.y * inv), c1[1] = (int)pxm::floorExact(hi.y * inv);
    c0[2] = (int)pxm::floorExact(lo.z * inv), c1[2] = (int)pxm::floorExact(hi.z * inv);
    return (int64_t)(c1[0] - c0[0] + 1) * (c1[1] - c0[1] + 1) * (c1[2] - c0[2] + 1);
  };
  for (size_t t = 0; t < nt; ++t) {
    if (tb[t].lo.x > tb[t].hi.x) continue;  // degenerate
    int c0[3], c1[3];
    if (cellRange(tb[t].lo, tb[t].hi, c0, c1) > 64) {
      isBig[t] = 1;
      big.push_back((uint32_t)t);
      continue;
    }
    for (int z = c0[2]; z <= c1[2]; ++z)
      for (int y = c0[1]; y <= c1[1]; ++y)
        for (int x = c0[0]; x <= c1[0]; ++x) ++start[(cellHash(x, y, z) & mask) + 1];
  }
  // Buckets padded to a multiple of 4 entries (padding: a box nothing overlaps), so a vertex tests 4 at a time.
  for (uint32_t k = 0; k < T; ++k) start[k + 1] = start[k] + ((start[k + 1] + 3) & ~3u);
  items.assign(start[T], kNoTri);
  std::vector<uint32_t>& fill = scFill_;
  fill.assign(start.begin(), start.end() - 1);
  for (size_t t = 0; t < nt; ++t) {
    if (isBig[t] || tb[t].lo.x > tb[t].hi.x) continue;
    int c0[3], c1[3];
    cellRange(tb[t].lo, tb[t].hi, c0, c1);
    for (int z = c0[2]; z <= c1[2]; ++z)
      for (int y = c0[1]; y <= c1[1]; ++y)
        for (int x = c0[0]; x <= c1[0]; ++x) items[fill[cellHash(x, y, z) & mask]++] = (uint32_t)t;
  }
  std::vector<float>& ib = scItemBox_;
  packBoxes(tb, items.data(), items.size(), ib);
  // The big triangles (every vertex's candidates) and all triangles (a vertex whose path spans too many cells) as SoA
  // boxes too, so those vertices reject four at a time like the grid path: a heavily torn cloth has dozens of big
  // triangles and fast pieces, and testing them one by one was most of its search.
  packBoxes(tb, big.data(), big.size(), scBigBox_);
  const float* bigBox = scBigBox_.data();
  const size_t nBigG = (big.size() + 3) / 4, nAllG = (nt + 3) / 4;
  std::once_flag allPacked;  // all triangles' boxes: only when a vertex needs them (most steps none does)
  auto allBoxes = [&]() -> const float* {
    std::call_once(allPacked, [&] {
      std::vector<uint32_t>& all = scCand_;  // scratch: 0..nt-1
      all.resize(nt);
      for (size_t t = 0; t < nt; ++t) all[t] = (uint32_t)t;
      packBoxes(tb, all.data(), nt, scAllBox_);
    });
    return scAllBox_.data();
  };
  // Mesh neighbours of every vertex (rebuilt when tearing changes the triangles): a vertex never collides with a
  // triangle that touches it or one of its neighbours, so the cloth's own bending is never fought, while the pieces of a
  // torn cloth (no longer neighbours) collide everywhere, their torn edges included.
  if (scAdjTris_ != tri_ || scAdjStart_.size() != nv + 1) {
    scAdjTris_ = tri_;
    std::vector<std::pair<uint32_t, uint32_t>> e;
    e.reserve(tri_.size() * 2);
    for (size_t k = 0; k + 2 < tri_.size(); k += 3)
      for (int c = 0; c < 3; ++c) {
        const uint32_t a = tri_[k + c], b = tri_[k + (c + 1) % 3];
        e.push_back({a, b});
        e.push_back({b, a});
      }
    std::sort(e.begin(), e.end());
    e.erase(std::unique(e.begin(), e.end()), e.end());
    scAdjStart_.assign(nv + 1, 0);
    for (const auto& q : e) ++scAdjStart_[q.first + 1];
    for (size_t v = 0; v < nv; ++v) scAdjStart_[v + 1] += scAdjStart_[v];
    scAdj_.resize(e.size());
    for (size_t k = 0; k < e.size(); ++k) scAdj_[k] = e[k].second;
    scEdges_.clear();
    for (const auto& q : e)
      if (q.first < q.second) scEdges_.push_back({q.first, q.second});
  }
  untangleFind(dt);
  const uint32_t* reg = utReg_.empty() ? nullptr : utReg_.data();
  if (reg) {  // pairs across regions are not kept (see untangleFind), nor are their remembered sides
    auto across = [&](uint64_t key) {
      const uint32_t v = (uint32_t)(key >> 32), t = (uint32_t)key, r = reg[v];
      return reg[tri_[3 * t]] != r || reg[tri_[3 * t + 1]] != r || reg[tri_[3 * t + 2]] != r;
    };
    scPrev_.erase(std::remove_if(scPrev_.begin(), scPrev_.end(), [&](const PrevPair& o) { return across(o.key); }),
                  scPrev_.end());
  }
  const float upL = lenOf(gravity);
  const V3 up = upL > 0.f ? gravity * (-1.f / upL) : V3(0, 1, 0);
  // Exact test of a candidate that passed the plane-distance filter (|d0| < h, |d1| < h or a sign change).
  auto finish = [&](std::vector<SelfContact>& out, uint32_t v, uint32_t t, float d0, float d1) {
    const TriPlanes& q = pl[t];
    const bool crossing = (d0 > 0.f) != (d1 > 0.f) &&
                          q.p0[0] * q.p1[0] + q.p0[1] * q.p1[1] + q.p0[2] * q.p1[2] > 0.f;
    if (!crossing && std::fabs(d0) >= h && std::fabs(d1) >= h) return;
    // Neighbour triangles (a corner joined to the vertex by an edge) are candidates only without bending (see below):
    // a piece lying folded flat along an edge z-fought with itself. Restricting them to slow, flat folds at h / 2 brought
    // the floor flicker back in game and did not change the banner hit (same with self contact off), so they are plain pairs.
    const uint32_t i0 = tri_[3 * t], i1 = tri_[3 * t + 1], i2 = tri_[3 * t + 2];
    if (reg && (reg[i0] != reg[v] || reg[i1] != reg[v] || reg[i2] != reg[v])) return;
    float b0[3], b1[3];
    bary(x_[v], x_[i0], x_[i1], x_[i2], b0);
    bary(p_[v], p_[i0], p_[i1], p_[i2], b1);
    if (!overTri(b0) && !overTri(b1)) return;
    float sd;
    const uint64_t key = ((uint64_t)v << 32) | t;
    auto it = std::lower_bound(scPrev_.begin(), scPrev_.end(), key, [](const PrevPair& a, uint64_t k) { return a.key < k; });
    const bool known = it != scPrev_.end() && it->key == key;
    uint16_t stuck = known ? it->stuck : 0;
    if (known) sd = it->side;  // a persisting pair keeps its side
    else if (std::fabs(d0) > 1e-3f * h) sd = d0 > 0.f ? 1.f : -1.f;
    else sd = q.p0[0] * up.x + q.p0[1] * up.y + q.p0[2] * up.z >= 0.f ? 1.f : -1.f;  // in the plane: against gravity
    out.push_back({v, t, sd, false, stuck, h});
  };
  // The vertices in chunks (the shim's worker pool when it gave one, see setParallelFor), each with its own scratch and
  // pair list; the lists are joined in chunk order, so the pairs and their order are the same for any thread count.
  // 64 vertices per chunk: tear copies are appended, so the torn and fast vertices (the expensive ones) sit together at
  // the end of the array, and 256-vertex chunks left one thread with most of the work.
  const size_t K = 64, nch = (nv + K - 1) / K;
  if (scChunks_.size() < nch) scChunks_.resize(nch);
  auto runChunk = [&](size_t ci) {
    ScChunk& C = scChunks_[ci];
    std::vector<uint32_t>& stamp = C.stamp;
    std::vector<uint32_t>& cand = C.cand;
    std::vector<SelfContact>& out = C.out;
    out.clear();
    stamp.assign(nt, 0);
    const uint32_t vEnd = (uint32_t)std::min(nv, (ci + 1) * K);
    for (uint32_t v = (uint32_t)(ci * K); v < vEnd; ++v) {
      if (rm && rm[v]) continue;
      const V3 xv = x_[v], pv = p_[v];
      const V3 vlo = vmin(xv, pv) - V3(h, h, h), vhi = vmax(xv, pv) + V3(h, h, h);
      // candidates: box overlap, not one of the vertex's own triangles, each triangle once
      cand.clear();
      const uint32_t mark = v + 1;
      auto consider = [&](uint32_t t) {
        if (stamp[t] == mark) return;
        stamp[t] = mark;
        const TriBox& bt = tb[t];
        if (bt.hi.x < vlo.x || bt.lo.x > vhi.x || bt.hi.y < vlo.y || bt.lo.y > vhi.y || bt.hi.z < vlo.z || bt.lo.z > vhi.z)
          return;
        for (int c = 0; c < 3; ++c) {  // the vertex's own triangles; its neighbours' only with bending (see above)
          const uint32_t w = tri_[3 * t + c];
          if (w == v) return;
          if (P_.useBending)
            for (uint32_t a = scAdjStart_[v]; a < scAdjStart_[v + 1]; ++a)
              if (scAdj_[a] == w) return;
        }
        cand.push_back(t);
      };
      int c0[3], c1[3];
      if (cellRange(vlo, vhi, c0, c1) > 64) {  // a vertex flying across many cells: every triangle is a candidate
        const float* allBox = allBoxes();
        for (size_t g = 0; g < nAllG; ++g)
          for (int m = overlap4(allBox + g * 24, vlo, vhi), j = 0; m; ++j, m >>= 1)
            if (m & 1) consider((uint32_t)(g * 4 + j));
      } else {
        for (int z = c0[2]; z <= c1[2]; ++z)
          for (int y = c0[1]; y <= c1[1]; ++y)
            for (int x = c0[0]; x <= c1[0]; ++x) {
              const uint32_t k = cellHash(x, y, z) & mask;
              // box overlap 4 entries at a time (the test consider() repeats; NaN boxes pass, as there), then the rest
              // of consider() for the entries that overlap
              for (uint32_t e = start[k]; e < start[k + 1]; e += 4) {
                for (int m = overlap4(&ib[(size_t)e * 6], vlo, vhi), j = 0; m; ++j, m >>= 1)
                  if ((m & 1) && items[e + j] != kNoTri) consider(items[e + j]);
              }
            }
        for (size_t g = 0; g < nBigG; ++g)
          for (int m = overlap4(bigBox + g * 24, vlo, vhi), j = 0; m; ++j, m >>= 1)
            if ((m & 1) && g * 4 + j < big.size()) consider(big[g * 4 + j]);
      }
      // Triangle order, so the pairs (and the order they are projected in) do not depend on how the grid was walked.
      std::sort(cand.begin(), cand.end());
      // plane distances at the start and the end of the step, 4 triangles at a time
      size_t k = 0;
  #if defined(__SSE2__)
      const __m128 hx = _mm_set1_ps(h), absMask = _mm_castsi128_ps(_mm_set1_epi32(0x7fffffff));
      const __m128 vx0 = _mm_set1_ps(xv.x), vy0 = _mm_set1_ps(xv.y), vz0 = _mm_set1_ps(xv.z);
      const __m128 vx1 = _mm_set1_ps(pv.x), vy1 = _mm_set1_ps(pv.y), vz1 = _mm_set1_ps(pv.z);
      for (; k + 4 <= cand.size(); k += 4) {
        const TriPlanes &qa = pl[cand[k]], &qb = pl[cand[k + 1]], &qc = pl[cand[k + 2]], &qd = pl[cand[k + 3]];
        __m128 a0 = _mm_loadu_ps(qa.p0), b0 = _mm_loadu_ps(qb.p0), c0v = _mm_loadu_ps(qc.p0), d0v = _mm_loadu_ps(qd.p0);
        _MM_TRANSPOSE4_PS(a0, b0, c0v, d0v);  // a0 = nx, b0 = ny, c0v = nz, d0v = offset
        const __m128 dist0 = _mm_sub_ps(
            _mm_add_ps(_mm_add_ps(_mm_mul_ps(a0, vx0), _mm_mul_ps(b0, vy0)), _mm_mul_ps(c0v, vz0)), d0v);
        __m128 a1 = _mm_loadu_ps(qa.p1), b1 = _mm_loadu_ps(qb.p1), c1v = _mm_loadu_ps(qc.p1), d1v = _mm_loadu_ps(qd.p1);
        _MM_TRANSPOSE4_PS(a1, b1, c1v, d1v);
        const __m128 dist1 = _mm_sub_ps(
            _mm_add_ps(_mm_add_ps(_mm_mul_ps(a1, vx1), _mm_mul_ps(b1, vy1)), _mm_mul_ps(c1v, vz1)), d1v);
        // near the plane at either end, or on different sides (sign bits differ)
        const __m128 near = _mm_or_ps(_mm_cmplt_ps(_mm_and_ps(dist0, absMask), hx),
                                      _mm_cmplt_ps(_mm_and_ps(dist1, absMask), hx));
        const __m128 flip = _mm_castsi128_ps(_mm_srai_epi32(_mm_castps_si128(_mm_xor_ps(dist0, dist1)), 31));
        const int m = _mm_movemask_ps(_mm_or_ps(near, flip));
        if (!m) continue;
        alignas(16) float e0[4], e1[4];
        _mm_store_ps(e0, dist0);
        _mm_store_ps(e1, dist1);
        for (int j = 0; j < 4; ++j)
          if (m & (1 << j)) finish(out, v, cand[k + j], e0[j], e1[j]);
      }
  #endif
      for (; k < cand.size(); ++k) {
        const TriPlanes& q = pl[cand[k]];
        const float d0 = q.p0[0] * xv.x + q.p0[1] * xv.y + q.p0[2] * xv.z - q.p0[3];
        const float d1 = q.p1[0] * pv.x + q.p1[1] * pv.y + q.p1[2] * pv.z - q.p1[3];
        finish(out, v, cand[k], d0, d1);
      }
    }
  };
  parallel(nch, runChunk);
  for (size_t ci = 0; ci < nch; ++ci) sc_.insert(sc_.end(), scChunks_[ci].out.begin(), scChunks_[ci].out.end());
  if (kEdgeEdge) selfEdgeFind(up);
  else se_.clear();
}

void Cloth::selfContactSolve() {
  for (SelfContact& c : sc_) {
    if (c.stuck >= kGiveUp) continue;  // a fight the local pushes cannot win (see selfContactVelocity)
    const uint32_t i0 = tri_[3 * c.t], i1 = tri_[3 * c.t + 1], i2 = tri_[3 * c.t + 2];
    V3 &pv = p_[c.v], &pa = p_[i0], &pb = p_[i1], &pc = p_[i2];
    V3 nrm = (pb - pa).cross(pc - pa);
    const float nl = lenOf(nrm);
    if (!(nl > 1e-20f)) continue;
    nrm = nrm * (c.s / nl);  // toward the vertex's side
    const float d = nrm.dot(pv - pa);
    const float h = c.dist;
    if (d >= h) continue;
    float b[3];
    bary(pv, pa, pb, pc, b);
    if (!overTri(b)) continue;
    for (float& x : b) x = std::max(0.f, x);
    const float sum = b[0] + b[1] + b[2];
    if (!(sum > 0.f)) continue;
    for (float& x : b) x /= sum;
    const float wv = w_[c.v], w0 = w_[i0], w1 = w_[i1], w2 = w_[i2];
    const float W = wv + b[0] * b[0] * w0 + b[1] * b[1] * w1 + b[2] * b[2] * w2;
    if (!(W > 0.f)) continue;
    // At most h per projection, and no particle moved by more than that: with the vertex and two corners pinned, W is
    // b^2 w of the free corner and its share w b / W = 1 / b grows without bound as b -> 0 (game run 20261012-230328,
    // Escape_p: a corner below a pinned row thrown 1.4 km in one step of 4.9 ms). Equal free masses never reach the cap.
    const float lam = std::min(h - d, h) / std::max(W, shareCap(wv, w0, w1, w2, b));
    c.active = true;
    pv += nrm * (wv * lam);
    pa -= nrm * (w0 * b[0] * lam);
    pb -= nrm * (w1 * b[1] * lam);
    pc -= nrm * (w2 * b[2] * lam);
  }
}

// The projection moves positions, and the velocity update turns every move into velocity: a pair pushed apart flew
// apart, fell back and was pushed again, so piles of torn pieces never came to rest (the flicker seen in game). For
// every pair that acted this step: the separating normal velocity between the vertex and the triangle point is removed
// (inelastic), and the sliding velocity is damped by the cloth's friction, both shared by inverse masses and
// barycentric weights like the projection.
void Cloth::selfContactVelocity() {
  for (SelfContact& c : sc_) {
    if (!c.active) continue;
    const uint32_t i0 = tri_[3 * c.t], i1 = tri_[3 * c.t + 1], i2 = tri_[3 * c.t + 2];
    const V3 &xa = x_[i0], &xb = x_[i1], &xc = x_[i2];
    V3 nrm = (xb - xa).cross(xc - xa);
    const float nl = lenOf(nrm);
    if (!(nl > 1e-20f)) continue;
    nrm = nrm * (c.s / nl);
    // Still less than h / 2 on its side after the solve: the pair fights the rest of the cloth (a tight fold, a pinned
    // corner inside the gap). After kGiveUp such steps in a row the solve leaves it alone while it persists (its side
    // is kept, and remembered for kSideMemory steps after it leaves); it would otherwise push every step and the piece
    // would never come to rest. (Holding stuck pairs a few mm apart instead cut the game flicker as much but agitated
    // piles of torn pieces.)
    const float dNow = nrm.dot(x_[c.v] - xa);
    if (dNow < 0.5f * c.dist) {
      if (c.stuck < 0xffff) ++c.stuck;
      // Still on the wrong side long after it went stuck: a real tangle (the vertex passed through unseen). Take the
      // side it is on, once; the pair then holds it there.
      if (c.stuck >= 4 * kGiveUp && dNow < 0.f) c.s = -c.s, c.stuck = 0;
    } else {
      c.stuck = 0;
    }
    float b[3];
    bary(x_[c.v], xa, xb, xc, b);
    for (float& x : b) x = std::max(0.f, x);
    const float sum = b[0] + b[1] + b[2];
    if (!(sum > 0.f)) continue;
    for (float& x : b) x /= sum;
    const float wv = w_[c.v], w0 = w_[i0], w1 = w_[i1], w2 = w_[i2];
    const float W = std::max(wv + b[0] * b[0] * w0 + b[1] * b[1] * w1 + b[2] * b[2] * w2, shareCap(wv, w0, w1, w2, b));
    if (!(W > 0.f)) continue;
    const V3 rel = v_[c.v] - (v_[i0] * b[0] + v_[i1] * b[1] + v_[i2] * b[2]);
    const float vn = rel.dot(nrm);
    V3 dv = (rel - nrm * vn) * friction_;  // sliding part, damped by friction
    if (vn > 0.f) dv += nrm * vn;  // separating part, removed
    const V3 imp = dv * (1.f / W);
    v_[c.v] -= imp * wv;
    v_[i0] += imp * (w0 * b[0]);
    v_[i1] += imp * (w1 * b[1]);
    v_[i2] += imp * (w2 * b[2]);
  }
}


// Resting contact: a particle of an acting pair that moves slower than the cloth's sleep speed stops. The pushes of a
// pile otherwise kept every layer creeping at a few cm/s, so a torn pile never fell asleep as the original's does.
// Only for pairs that rest on something: a particle with a shape contact this step, or (two passes) a pair partner of
// one. A piece folded in mid-air has active pairs too; stopped every step (gravity adds only ~0.05 m/s per step at the
// game's 150-230 FPS) it hung in the air (game run 20261012-180216, torn banner pieces next to walls).
void Cloth::selfContactRest() {
  const float lim2 = sleepVelocity() * sleepVelocity();
  std::vector<uint8_t>& sup = restSupported_;
  sup.assign(x_.size(), 0);
  for (size_t i = 0; i < x_.size(); ++i) sup[i] = !contacts_[i].empty();
  for (int pass = 0; pass < 2; ++pass)
    for (const SelfContact& c : sc_) {
      if (!c.active) continue;
      const uint32_t q[4] = {c.v, tri_[3 * c.t], tri_[3 * c.t + 1], tri_[3 * c.t + 2]};
      if (!(sup[q[0]] | sup[q[1]] | sup[q[2]] | sup[q[3]])) continue;
      for (uint32_t i : q) {
        sup[i] = 1;
        if (v_[i].len2() < lim2) v_[i] = V3();
      }
    }
}

// ---- untangling ----
// A part of the cloth that went through the cloth (a bullet throws a flap through the banner, a corner pokes through a
// fold) stayed there: the vertex-triangle pairs keep each vertex on the side it is on, and the original has no self
// contact at all (game run 20261013-130704: hit banners folded through themselves; ours kept 7 crossings to the end,
// the original 41). Per step, at the start positions, the edges that pierce a triangle of the same cloth are found; the
// piercing points chain into the intersection curves (two piercings meet in each intersecting triangle pair). Then:
// - Global intersection analysis (after Baraff, Witkin and Kass, "Untangling cloth", 2003): the piercing edges cut the
//   mesh along the curves. Each piece (connected by edges) falls into regions connected by uncut edges; the largest
//   is the piece's main part (region 0), the others went through. Vertex-triangle pairs between different regions are
//   dropped with their remembered sides (they held the crossed state), and a piercing edge between regions pulls its
//   vertex in the smaller region to the other vertex's side of the triangle.
// - Contour shortening (after Volino and Magnenat-Thalmann, "Resolving surface collisions through intersection contour
//   minimization", 2006): each piercing point slides in the pierced triangle's plane toward its neighbours on the curve
//   (the edge one way, the triangle the other). A curve that cuts nothing off (two borders hooked through each other:
//   cloth002 of that run, 2 piercings for 2500 steps) shrinks until the borders slip apart.
// Both move at most kUntangleSpeed per step. The search runs every kUntangleEveryTangled steps (a crumpled torn pile:
// ~17 triangle boxes per edge, the search cost as much as the rest of the step), every kUntangleEvery while nothing
// pierces; without piercings nothing changes (ut_ and utReg_ stay empty). A tangle that does not shrink (no fewer
// piercings, no shorter curve) is searched every 4, then 8 steps; the pulls of the last search keep acting in between
// (dropping them stalled hooked banners: a slide shortens the curve while the piercing count stays the same).
namespace {
constexpr float kUntangleSpeed = 2.f;  // m/s: most a piercing moves per step (over the step length)
constexpr uint8_t kUntangleEvery = 4;         // steps between piercing searches while nothing pierces
constexpr uint8_t kUntangleEveryTangled = 2;  // and while something does (the result is kept in between)
constexpr int kUntangleEveryStuck = 8;       // longest interval while the tangle does not shrink
constexpr float kUntangleShorter = 0.9f;     // a curve this much shorter than the best so far is progress
constexpr float kPoked = 0.5f;               // a vertex with at least this share of its edges piercing went through
// Segment p-q against triangle (a, b, c): true when it crosses the triangle's interior; tau: where on the segment,
// w: barycentric of the point.
inline bool segTri(const V3& p, const V3& q, const V3& a, const V3& b, const V3& c, float& tau, float w[3]) {
  const V3 e1 = b - a, e2 = c - a, d = q - p;
  const V3 h = d.cross(e2);
  const float det = e1.dot(h);
  if (std::fabs(det) < 1e-20f) return false;
  const float inv = 1.f / det;
  const V3 s = p - a;
  const float u = s.dot(h) * inv;
  if (u < 0.f || u > 1.f) return false;
  const V3 qv = s.cross(e1);
  const float v = d.dot(qv) * inv;
  if (v < 0.f || u + v > 1.f) return false;
  tau = e2.dot(qv) * inv;
  if (tau <= 0.f || tau >= 1.f) return false;
  w[0] = 1.f - u - v, w[1] = u, w[2] = v;
  return true;
}
inline uint32_t findRoot(std::vector<uint32_t>& f, uint32_t i) {
  while (f[i] != i) i = f[i] = f[f[i]];
  return i;
}
}  // namespace

void Cloth::untangleFind(float dt) {
  const size_t ne = scEdges_.size(), nv = x_.size(), nt = tri_.size() / 3;
  if (utIdle_ > 0 && (utReg_.empty() || utReg_.size() == nv)) {  // between searches: the last one's result stays
    --utIdle_;
    return;
  }
  const uint8_t* rm = anyRemoved_ ? removed_.data() : nullptr;
  utIdle_ = 0;
  ut_.clear();
  utReg_.clear();
  if (ne == 0) return;
  const std::vector<TriPlanes>& pl = scPlanes_;
  // Its own grid: triangle boxes at the start positions only (the contact grid's are grown by h and the step's motion,
  // about three times the overlaps).
  std::vector<TriBox>& tb = utBox_;
  tb.resize(nt);
  float ext = 0.f;
  size_t live = 0;
  for (size_t t = 0; t < nt; ++t) {
    const float* q = pl[t].p0;
    if (q[0] == 0.f && q[1] == 0.f && q[2] == 0.f) {  // degenerate (see selfContactFind): never a candidate
      tb[t].lo = V3(1e30f, 1e30f, 1e30f), tb[t].hi = V3(-1e30f, -1e30f, -1e30f);
      continue;
    }
    const V3 &a = x_[tri_[3 * t]], &b = x_[tri_[3 * t + 1]], &c = x_[tri_[3 * t + 2]];
    tb[t].lo = vmin(a, vmin(b, c)), tb[t].hi = vmax(a, vmax(b, c));
    const V3 e = tb[t].hi - tb[t].lo;
    ext += std::max(e.x, std::max(e.y, e.z));
    ++live;
  }
  if (live == 0) return;
  const float inv = 1.f / std::max(ext / (float)live, 1e-6f);
  uint32_t T = 16;
  while (T < 2 * nt) T <<= 1;
  const uint32_t mask = T - 1;
  auto cellRange = [&](const V3& lo, const V3& hi, int c0[3], int c1[3]) {
    c0[0] = (int)pxm::floorExact(lo.x * inv), c1[0] = (int)pxm::floorExact(hi.x * inv);
    c0[1] = (int)pxm::floorExact(lo.y * inv), c1[1] = (int)pxm::floorExact(hi.y * inv);
    c0[2] = (int)pxm::floorExact(lo.z * inv), c1[2] = (int)pxm::floorExact(hi.z * inv);
    return (int64_t)(c1[0] - c0[0] + 1) * (c1[1] - c0[1] + 1) * (c1[2] - c0[2] + 1);
  };
  std::vector<uint32_t>& start = utStart_;
  std::vector<uint32_t>& items = utItems_;
  std::vector<uint32_t>& big = utBig_;
  start.assign(T + 1, 0);
  big.clear();
  for (size_t t = 0; t < nt; ++t) {
    if (tb[t].lo.x > tb[t].hi.x) continue;
    int c0[3], c1[3];
    if (cellRange(tb[t].lo, tb[t].hi, c0, c1) > 64) {
      big.push_back((uint32_t)t);
      continue;
    }
    for (int z = c0[2]; z <= c1[2]; ++z)
      for (int y = c0[1]; y <= c1[1]; ++y)
        for (int x = c0[0]; x <= c1[0]; ++x) ++start[(cellHash(x, y, z) & mask) + 1];
  }
  // buckets padded to a multiple of 4 (padding: a box nothing overlaps), boxes 4 at a time (SoA, as selfContactFind)
  for (uint32_t k = 0; k < T; ++k) start[k + 1] = start[k] + ((start[k + 1] + 3) & ~3u);
  items.assign(start[T], kNoTri);
  {
    std::vector<uint32_t> fill(start.begin(), start.end() - 1);
    for (size_t t = 0; t < nt; ++t) {
      if (tb[t].lo.x > tb[t].hi.x) continue;
      int c0[3], c1[3];
      if (cellRange(tb[t].lo, tb[t].hi, c0, c1) > 64) continue;
      for (int z = c0[2]; z <= c1[2]; ++z)
        for (int y = c0[1]; y <= c1[1]; ++y)
          for (int x = c0[0]; x <= c1[0]; ++x) items[fill[cellHash(x, y, z) & mask]++] = (uint32_t)t;
    }
  }
  std::vector<float>& ib = utItemBox_;
  ib.resize(items.size() * 6);
  for (size_t g = 0; g < items.size(); g += 4) {
    float* B = &ib[g * 6];
    for (int j = 0; j < 4; ++j) {
      const uint32_t t = items[g + j];
      const V3 lo = t == kNoTri ? V3(1e30f, 1e30f, 1e30f) : tb[t].lo, hi = t == kNoTri ? V3(-1e30f, -1e30f, -1e30f) : tb[t].hi;
      B[0 * 4 + j] = lo.x, B[1 * 4 + j] = lo.y, B[2 * 4 + j] = lo.z;
      B[3 * 4 + j] = hi.x, B[4 * 4 + j] = hi.y, B[5 * 4 + j] = hi.z;
    }
  }
  // piercings, per chunk of edges, joined in edge order
  struct Pierce { uint32_t e, t; float tau, w[3]; };
  const size_t K = 512, nch = (ne + K - 1) / K;
  std::vector<std::vector<Pierce>> out(nch);
  auto runChunk = [&](size_t ci) {
    std::vector<Pierce>& o = out[ci];
    std::vector<uint32_t> stamp(nt, 0);
    const size_t kEnd = std::min(ne, (ci + 1) * K);
    for (size_t k = ci * K; k < kEnd; ++k) {
      const uint32_t a = scEdges_[k].first, b = scEdges_[k].second;
      if (rm && (rm[a] || rm[b])) continue;
      const V3 &P = x_[a], &Q = x_[b];
      const V3 lo = vmin(P, Q), hi = vmax(P, Q);
      const uint32_t mark = (uint32_t)k + 1;
      const size_t first = o.size();
      // a candidate: boxes overlap, the start plane separates the edge's ends, no shared corner; then the exact test
      auto overlaps = [&](uint32_t t) {
        const TriBox& bt = tb[t];
        return !(bt.hi.x < lo.x || bt.lo.x > hi.x || bt.hi.y < lo.y || bt.lo.y > hi.y || bt.hi.z < lo.z || bt.lo.z > hi.z);
      };
      auto consider = [&](uint32_t t) {  // boxes overlap
        if (stamp[t] == mark) return;
        stamp[t] = mark;
        const float* q = pl[t].p0;
        const float dp = q[0] * P.x + q[1] * P.y + q[2] * P.z - q[3], dq = q[0] * Q.x + q[1] * Q.y + q[2] * Q.z - q[3];
        if ((dp > 0.f) == (dq > 0.f)) return;
        const uint32_t i0 = tri_[3 * t], i1 = tri_[3 * t + 1], i2 = tri_[3 * t + 2];
        if (i0 == a || i1 == a || i2 == a || i0 == b || i1 == b || i2 == b) return;
        Pierce pc{(uint32_t)k, t, 0.f, {}};
        if (segTri(P, Q, x_[i0], x_[i1], x_[i2], pc.tau, pc.w)) o.push_back(pc);
      };
      int c0[3], c1[3];
      if (cellRange(lo, hi, c0, c1) > 64) {
        for (uint32_t t = 0; t < (uint32_t)nt; ++t)
          if (overlaps(t)) consider(t);
      } else {
        for (int z = c0[2]; z <= c1[2]; ++z)
          for (int y = c0[1]; y <= c1[1]; ++y)
            for (int x = c0[0]; x <= c1[0]; ++x) {
              const uint32_t c = cellHash(x, y, z) & mask;
              for (uint32_t e = start[c]; e < start[c + 1]; e += 4) {
                const float* B = &ib[(size_t)e * 6];
                int m;
#if defined(__SSE2__)
                const __m128 ok = _mm_and_ps(
                    _mm_and_ps(_mm_and_ps(_mm_cmpnlt_ps(_mm_loadu_ps(B + 12), _mm_set1_ps(lo.x)),
                                          _mm_cmpnlt_ps(_mm_loadu_ps(B + 16), _mm_set1_ps(lo.y))),
                               _mm_and_ps(_mm_cmpnlt_ps(_mm_loadu_ps(B + 20), _mm_set1_ps(lo.z)),
                                          _mm_cmpngt_ps(_mm_loadu_ps(B + 0), _mm_set1_ps(hi.x)))),
                    _mm_and_ps(_mm_cmpngt_ps(_mm_loadu_ps(B + 4), _mm_set1_ps(hi.y)),
                               _mm_cmpngt_ps(_mm_loadu_ps(B + 8), _mm_set1_ps(hi.z))));
                m = _mm_movemask_ps(ok);
#else
                m = 0;
                for (int j = 0; j < 4; ++j)
                  if (!(B[12 + j] < lo.x || B[16 + j] < lo.y || B[20 + j] < lo.z || B[0 + j] > hi.x ||
                        B[4 + j] > hi.y || B[8 + j] > hi.z))
                    m |= 1 << j;
#endif
                for (int j = 0; m; ++j, m >>= 1)
                  if ((m & 1) && items[e + j] != kNoTri) consider(items[e + j]);
              }
            }
        for (uint32_t t : big)
          if (overlaps(t)) consider(t);
      }
      // triangle order within the edge (the grid walk order is fixed anyway; this keeps the list readable)
      std::sort(o.begin() + first, o.end(), [](const Pierce& u, const Pierce& v) { return u.t < v.t; });
    }
  };
  parallel(nch, runChunk);
  std::vector<Pierce> pierce;
  for (const auto& o : out) pierce.insert(pierce.end(), o.begin(), o.end());
  const size_t np = pierce.size();
  if (np == 0) {
    utIdle_ = kUntangleEvery - 1, utEvery_ = kUntangleEveryTangled, utBest_ = utLast_ = 0;
    return;
  }
  // regions: pieces (all edges) split by the piercing edges
  std::vector<uint8_t> cut(ne, 0);
  for (const Pierce& q : pierce) cut[q.e] = 1;
  std::vector<uint32_t> piece(nv), region(nv);
  for (uint32_t i = 0; i < nv; ++i) piece[i] = region[i] = i;
  for (size_t k = 0; k < ne; ++k) {
    const uint32_t a = scEdges_[k].first, b = scEdges_[k].second;
    if (rm && (rm[a] || rm[b])) continue;
    const uint32_t pa = findRoot(piece, a), pb = findRoot(piece, b);
    if (pa != pb) piece[std::max(pa, pb)] = std::min(pa, pb);
    if (cut[k]) continue;
    const uint32_t ra = findRoot(region, a), rb = findRoot(region, b);
    if (ra != rb) region[std::max(ra, rb)] = std::min(ra, rb);
  }
  std::vector<uint32_t> size(nv, 0), best(nv, kNoTri);  // per region root: vertices; per piece root: largest region
  for (uint32_t i = 0; i < nv; ++i) ++size[findRoot(region, i)];
  for (uint32_t i = 0; i < nv; ++i) {
    const uint32_t r = findRoot(region, i), p = findRoot(piece, i);
    if (best[p] == kNoTri || size[r] > size[best[p]] || (size[r] == size[best[p]] && r < best[p])) best[p] = r;
  }
  bool anyRegion = false;
  utReg_.assign(nv, 0);
  for (uint32_t i = 0; i < nv; ++i) {
    const uint32_t r = findRoot(region, i);
    if (r != best[findRoot(piece, i)]) utReg_[i] = r + 1, anyRegion = true;
  }
  if (!anyRegion) utReg_.clear();

  // curves: the triangles on each side of every piercing edge; a piercing of edge e (triangles T) into triangle t lies
  // on the segment of the pair (T, t), whose other end is the other piercing of that pair
  std::vector<std::pair<uint64_t, uint32_t>> ends;  // (triangle pair, piercing)
  {
    std::vector<std::pair<uint64_t, uint32_t>> edgeTri;  // (edge a << 32 | b, triangle) of the piercing edges
    for (const Pierce& q : pierce) edgeTri.push_back({((uint64_t)scEdges_[q.e].first << 32) | scEdges_[q.e].second, kNoTri});
    std::sort(edgeTri.begin(), edgeTri.end());
    edgeTri.erase(std::unique(edgeTri.begin(), edgeTri.end()), edgeTri.end());
    const size_t nKeys = edgeTri.size();
    for (uint32_t t = 0; t < (uint32_t)nt; ++t)
      for (int c = 0; c < 3; ++c) {
        uint32_t a = tri_[3 * t + c], b = tri_[3 * t + (c + 1) % 3];
        if (a == b) continue;
        if (a > b) std::swap(a, b);
        const uint64_t key = ((uint64_t)a << 32) | b;
        auto it = std::lower_bound(edgeTri.begin(), edgeTri.begin() + nKeys, std::make_pair(key, 0u));
        if (it != edgeTri.begin() + nKeys && it->first == key) edgeTri.push_back({key, t});
      }
    std::sort(edgeTri.begin() + nKeys, edgeTri.end());
    for (uint32_t k = 0; k < (uint32_t)np; ++k) {
      const uint64_t key = ((uint64_t)scEdges_[pierce[k].e].first << 32) | scEdges_[pierce[k].e].second;
      for (auto it = std::lower_bound(edgeTri.begin() + nKeys, edgeTri.end(), std::make_pair(key, 0u));
           it != edgeTri.end() && it->first == key; ++it) {
        const uint32_t T = it->second, t = pierce[k].t;
        ends.push_back({((uint64_t)std::min(T, t) << 32) | std::max(T, t), k});
      }
    }
    std::sort(ends.begin(), ends.end());
  }
  auto pointOf = [&](const Pierce& q) {
    const uint32_t i0 = tri_[3 * q.t], i1 = tri_[3 * q.t + 1], i2 = tri_[3 * q.t + 2];
    return x_[i0] * q.w[0] + x_[i1] * q.w[1] + x_[i2] * q.w[2];
  };
  std::vector<V3> g(np);
  std::vector<float> near(np, 1e30f);  // distance to the nearest neighbour on the curve
  float curveLen = 0.f;
  for (size_t s = 0; s < ends.size();) {
    size_t e = s;
    while (e < ends.size() && ends[e].first == ends[s].first) ++e;
    for (size_t i = s; i < e; ++i)  // normally 2; any more are joined to each other
      for (size_t j = s; j < e; ++j) {
        if (i == j || ends[i].second == ends[j].second) continue;
        const V3 d = pointOf(pierce[ends[j].second]) - pointOf(pierce[ends[i].second]);
        const float l = lenOf(d);
        if (l > 1e-9f) g[ends[i].second] += d * (1.f / l);
        near[ends[i].second] = std::min(near[ends[i].second], l);
        if (i < j) curveLen += l;
      }
    s = e;
  }
  // a tangle that does not shrink is searched less and less often (crumpled torn piles stay tangled; the search cost
  // as much as the rest of their step); fewer piercings or a shorter curve than the best so far, or a new tangle,
  // resets the rate
  if (utLast_ == 0 || np > utLast_ + std::max<size_t>(2, utLast_ / 4) || np + std::max<size_t>(1, utBest_ / 8) <= utBest_ ||
      curveLen < kUntangleShorter * utBestLen_)
    utEvery_ = kUntangleEveryTangled, utBest_ = (uint32_t)np, utBestLen_ = curveLen;
  else
    utEvery_ = (uint8_t)std::min<int>(2 * utEvery_, kUntangleEveryStuck);
  utLast_ = (uint32_t)np;
  utIdle_ = utEvery_ - 1;

  ut_.reserve(np);
  for (uint32_t k = 0; k < (uint32_t)np; ++k) {
    const Pierce& q = pierce[k];
    const uint32_t a = scEdges_[q.e].first, b = scEdges_[q.e].second;
    const uint32_t i0 = tri_[3 * q.t], i1 = tri_[3 * q.t + 1], i2 = tri_[3 * q.t + 2];
    V3 n = (x_[i1] - x_[i0]).cross(x_[i2] - x_[i0]);
    n = n * (1.f / std::max(lenOf(n), 1e-30f));
    Untangle u{a, b, i0, i1, i2, -1, q.tau, 1.f, {q.w[0], q.w[1], q.w[2]}, g[k] - n * n.dot(g[k]), 0.f};
    const float gl = lenOf(u.g);
    u.g = gl > 1e-6f ? u.g * (1.f / gl) : V3();
    if (!utReg_.empty() && utReg_[a] != utReg_[b]) {  // the vertex in the smaller region goes (region 0: the largest)
      const uint32_t sa = utReg_[a] ? size[utReg_[a] - 1] : 0xffffffffu, sb = utReg_[b] ? size[utReg_[b] - 1] : 0xffffffffu;
      const uint32_t in = sa < sb || (sa == sb && a < b) ? a : b, keep = in == a ? b : a;
      u.in = (int)in;
      u.s = n.dot(x_[keep] - x_[i0]) >= 0.f ? 1.f : -1.f;
    }
    ut_.push_back(u);
  }
  // Poked vertices (only once the tangle stopped shrinking: acting earlier undid progress the other pulls were making;
  // game run 20261013-171200 cloth002, a border vertex through the two triangles next to its neighbour for 2500 steps):
  // a vertex goes through the cloth on its own and nothing is cut off when the curve ends at the corner of a pierced
  // triangle (edges sharing that corner are not tested). The vertex has most of its edges piercing, the edge's other end
  // few: it is pulled through to that end's side, in a region of its own (its self contact pairs, which held it on the
  // wrong side, are dropped while it is through).
  if (utEvery_ > kUntangleEveryTangled) {
    std::vector<uint16_t> deg(nv, 0), pc(nv, 0);
    for (size_t k = 0; k < ne; ++k) {
      const uint32_t a = scEdges_[k].first, b = scEdges_[k].second;
      if (rm && (rm[a] || rm[b])) continue;
      ++deg[a], ++deg[b];
      if (cut[k]) ++pc[a], ++pc[b];
    }
    auto share = [&](uint32_t v) { return deg[v] ? (float)pc[v] / deg[v] : 0.f; };
    for (Untangle& u : ut_) {
      if (u.in >= 0) continue;
      const float fa = share(u.e0), fb = share(u.e1);
      const uint32_t v = fa >= fb ? u.e0 : u.e1, o = v == u.e0 ? u.e1 : u.e0;
      if (std::max(fa, fb) < kPoked || std::min(fa, fb) > 0.5f * std::max(fa, fb)) continue;
      const V3 n = (x_[u.b] - x_[u.a]).cross(x_[u.c] - x_[u.a]);
      u.in = (int)v;
      u.s = n.dot(x_[o] - x_[u.a]) >= 0.f ? 1.f : -1.f;
      if (utReg_.empty()) utReg_.assign(nv, 0);
      utReg_[v] = (uint32_t)nv + 1 + v;
    }
  }
  utCap_ = kUntangleSpeed * dt;
  for (uint32_t k = 0; k < (uint32_t)np; ++k) ut_[k].lim = std::min(utCap_, 0.5f * near[k]);  // no overshoot
}

void Cloth::untangleSolve() {
  const float margin = 0.25f * P_.selfContactDist;
  for (const Untangle& u : ut_) {
    V3 &pa = p_[u.a], &pb = p_[u.b], &pc = p_[u.c];
    const float w0 = w_[u.a], w1 = w_[u.b], w2 = w_[u.c];
    const float Wt = u.w[0] * u.w[0] * w0 + u.w[1] * u.w[1] * w1 + u.w[2] * u.w[2] * w2;
    if (u.in >= 0) {  // pull through: the vertex to the other side of the triangle at the piercing point
      V3& pv = p_[u.in];
      V3 nrm = (pb - pa).cross(pc - pa);
      const float nl = lenOf(nrm);
      if (nl > 1e-20f) {
        nrm = nrm * (u.s / nl);
        const float d = nrm.dot(pv - (pa * u.w[0] + pb * u.w[1] + pc * u.w[2]));
        const float wv = w_[u.in], W = wv + Wt;
        if (d < margin && W > 0.f) {
          const float lam = std::min(margin - d, utCap_) / std::max(W, shareCap(wv, w0, w1, w2, u.w));
          pv += nrm * (wv * lam);
          pa -= nrm * (w0 * u.w[0] * lam);
          pb -= nrm * (w1 * u.w[1] * lam);
          pc -= nrm * (w2 * u.w[2] * lam);
        }
      }
    }
    if (u.g.len2() > 0.f) {  // slide: the edge's point along g, the triangle's point against it
      const float t0 = 1.f - u.tau, t1 = u.tau, we0 = w_[u.e0], we1 = w_[u.e1];
      const float W = t0 * t0 * we0 + t1 * t1 * we1 + Wt;
      if (!(W > 0.f)) continue;
      const float cap = std::max(std::max(we0 * t0, we1 * t1), std::max(std::max(w0 * u.w[0], w1 * u.w[1]), w2 * u.w[2]));
      const float lam = u.lim / std::max(W, cap);
      p_[u.e0] += u.g * (we0 * t0 * lam);
      p_[u.e1] += u.g * (we1 * t1 * lam);
      pa -= u.g * (w0 * u.w[0] * lam);
      pb -= u.g * (w1 * u.w[1] * lam);
      pc -= u.g * (w2 * u.w[2] * lam);
    }
  }
}

// ---- edge-edge ----
// Vertex-triangle pairs stop vertices from passing through triangles, but two edges can still pass through each other
// with no vertex crossing a triangle; crumpled pieces then cut through each other at shallow angles and z-fight along
// the cut (the remaining flicker in game: 965 intersecting triangle pairs in a torn pile without this).
// Pairs: edges sharing no vertex and not joined by an edge (neighbours), closest points inside both segments. The
// direction between the closest points at the step start is the side kept (as for vertex-triangle pairs); a pair
// is kept when the edges come within h at the end of the step or pass through each other during it.
namespace {
// Closest points of segments p0p1 and q0q1: parameters s, t in [0, 1] (Ericson, RTCD 5.1.9).
inline void segSeg(const V3& p0, const V3& p1, const V3& q0, const V3& q1, float& s, float& t) {
  const V3 d1 = p1 - p0, d2 = q1 - q0, r = p0 - q0;
  const float a = d1.dot(d1), e = d2.dot(d2), f = d2.dot(r);
  if (a <= 1e-20f && e <= 1e-20f) { s = t = 0.f; return; }
  if (a <= 1e-20f) { s = 0.f; t = std::min(1.f, std::max(0.f, f / e)); return; }
  const float c = d1.dot(r);
  if (e <= 1e-20f) { t = 0.f; s = std::min(1.f, std::max(0.f, -c / a)); return; }
  const float b = d1.dot(d2), den = a * e - b * b;
  s = den > 0.f ? std::min(1.f, std::max(0.f, (b * f - c * e) / den)) : 0.f;
  t = (b * s + f) / e;
  if (t < 0.f) { t = 0.f; s = std::min(1.f, std::max(0.f, -c / a)); }
  else if (t > 1.f) { t = 1.f; s = std::min(1.f, std::max(0.f, (b - c) / a)); }
}
constexpr float kEdgeEnd = 0.02f;  // closest points this close to an end are the vertex-triangle pairs' business
}  // namespace

void Cloth::selfEdgeFind(const V3& up) {
  scPrevE_.clear();
  for (const EdgeContact& c : se_) scPrevE_.push_back({((uint64_t)c.e1 << 32) | c.e2, 0.f, c.stuck, c.n});
  std::sort(scPrevE_.begin(), scPrevE_.end(), [](const PrevEdge& a, const PrevEdge& b) { return a.key < b.key; });
  if (scPrevEdges_ != scEdges_) scPrevE_.clear();  // renumbered by tearing: forget
  scPrevEdges_ = scEdges_;
  se_.clear();
  const uint8_t* rm = anyRemoved_ ? removed_.data() : nullptr;
  const float h = P_.selfContactDist;
  const size_t ne = scEdges_.size();
  if (ne < 2) return;
  // edge boxes over the step, grown by h; hashed grid as for the triangles
  std::vector<TriBox>& eb = scEdgeBox_;
  eb.resize(ne);
  float ext = 0.f;
  for (size_t k = 0; k < ne; ++k) {
    const uint32_t a = scEdges_[k].first, b = scEdges_[k].second;
    V3 lo = vmin(vmin(x_[a], x_[b]), vmin(p_[a], p_[b])), hi = vmax(vmax(x_[a], x_[b]), vmax(p_[a], p_[b]));
    eb[k].lo = lo - V3(h, h, h);
    eb[k].hi = hi + V3(h, h, h);
    const V3 d = eb[k].hi - eb[k].lo;
    ext += std::max(d.x, std::max(d.y, d.z));
  }
  const float inv = 1.f / std::max(ext / (float)ne, 1e-6f);
  auto cellRange = [&](const V3& lo, const V3& hi, int c0[3], int c1[3]) {
    c0[0] = (int)pxm::floorExact(lo.x * inv), c1[0] = (int)pxm::floorExact(hi.x * inv);
    c0[1] = (int)pxm::floorExact(lo.y * inv), c1[1] = (int)pxm::floorExact(hi.y * inv);
    c0[2] = (int)pxm::floorExact(lo.z * inv), c1[2] = (int)pxm::floorExact(hi.z * inv);
    return (int64_t)(c1[0] - c0[0] + 1) * (c1[1] - c0[1] + 1) * (c1[2] - c0[2] + 1);
  };
  uint32_t T = 16;
  while (T < 4 * ne) T <<= 1;
  const uint32_t mask = T - 1;
  std::vector<uint32_t>& start = scEStart_;
  std::vector<uint32_t>& items = scEItems_;
  std::vector<uint32_t>& fill = scEFill_;
  std::vector<uint32_t>& big = scEBig_;
  start.assign(T + 1, 0);
  big.clear();
  for (size_t k = 0; k < ne; ++k) {
    int c0[3], c1[3];
    if (cellRange(eb[k].lo, eb[k].hi, c0, c1) > 64) {
      big.push_back((uint32_t)k);
      continue;
    }
    for (int z = c0[2]; z <= c1[2]; ++z)
      for (int y = c0[1]; y <= c1[1]; ++y)
        for (int x = c0[0]; x <= c1[0]; ++x) ++start[(cellHash(x, y, z) & mask) + 1];
  }
  for (uint32_t k = 0; k < T; ++k) start[k + 1] += start[k];
  items.resize(start[T]);
  fill.assign(start.begin(), start.end() - 1);
  for (size_t k = 0; k < ne; ++k) {
    int c0[3], c1[3];
    if (cellRange(eb[k].lo, eb[k].hi, c0, c1) > 64) continue;
    for (int z = c0[2]; z <= c1[2]; ++z)
      for (int y = c0[1]; y <= c1[1]; ++y)
        for (int x = c0[0]; x <= c1[0]; ++x) items[fill[cellHash(x, y, z) & mask]++] = (uint32_t)k;
  }
  std::vector<uint32_t>& stamp = scEStamp_;
  stamp.assign(ne, 0);
  auto neighbours = [&](uint32_t a, uint32_t w) {  // w == a or joined to a by an edge
    if (w == a) return true;
    for (uint32_t k = scAdjStart_[a]; k < scAdjStart_[a + 1]; ++k)
      if (scAdj_[k] == w) return true;
    return false;
  };
  const float lo = kEdgeEnd, hiP = 1.f - kEdgeEnd;
  auto test = [&](uint32_t k1, uint32_t k2) {
    const TriBox &b1 = eb[k1], &b2 = eb[k2];
    if (b1.hi.x < b2.lo.x || b1.lo.x > b2.hi.x || b1.hi.y < b2.lo.y || b1.lo.y > b2.hi.y || b1.hi.z < b2.lo.z ||
        b1.lo.z > b2.hi.z)
      return;
    const uint32_t a = scEdges_[k1].first, b = scEdges_[k1].second, c = scEdges_[k2].first, d = scEdges_[k2].second;
    if (rm && (rm[a] || rm[b] || rm[c] || rm[d])) return;
    if (neighbours(a, c) || neighbours(a, d) || neighbours(b, c) || neighbours(b, d)) return;
    float s0, t0, s1, t1;
    segSeg(x_[a], x_[b], x_[c], x_[d], s0, t0);
    segSeg(p_[a], p_[b], p_[c], p_[d], s1, t1);
    const bool in0 = s0 > lo && s0 < hiP && t0 > lo && t0 < hiP, in1 = s1 > lo && s1 < hiP && t1 > lo && t1 < hiP;
    if (!in0 && !in1) return;
    const V3 g0 = (x_[a] + (x_[b] - x_[a]) * s0) - (x_[c] + (x_[d] - x_[c]) * t0);
    const V3 g1 = (p_[a] + (p_[b] - p_[a]) * s1) - (p_[c] + (p_[d] - p_[c]) * t1);
    const V3 g1s = (p_[a] + (p_[b] - p_[a]) * s0) - (p_[c] + (p_[d] - p_[c]) * t0);  // the start's points, at the end
    // crossing is only meaningful when the start's closest points are inside both edges
    const bool crossing = in0 && g0.dot(g1s) < 0.f;
    if (!crossing && (!in1 || g1.len2() >= h * h)) return;
    const uint64_t key = ((uint64_t)k1 << 32) | k2;
    auto it = std::lower_bound(scPrevE_.begin(), scPrevE_.end(), key, [](const PrevEdge& q, uint64_t kk) { return q.key < kk; });
    const bool known = it != scPrevE_.end() && it->key == key;
    V3 n;
    if (known) {
      n = it->n;  // a persisting pair keeps its side
    } else {
      const float l0 = lenOf(g0), l1 = lenOf(g1);
      if (!in0 && l1 > 1e-3f * h) {
        n = g1 * (1.f / l1);  // start points at an end: the side they are on now
      } else if (in0 && l0 > 1e-3f * h) {
        n = g0 * (1.f / l0);
      } else {  // touching at the start: across both edges, the first one against gravity
        n = (x_[b] - x_[a]).cross(x_[d] - x_[c]);
        const float l = lenOf(n);
        if (!(l > 1e-20f)) return;
        n = n * (1.f / l);
        if (n.dot(up) < 0.f) n = -n;
      }
    }
    se_.push_back({k1, k2, n, false, known ? it->stuck : (uint16_t)0});
  };
  for (uint32_t k1 = 0; k1 < (uint32_t)ne; ++k1) {
    const uint32_t mark = k1 + 1;
    auto consider = [&](uint32_t k2) {
      if (k2 <= k1 || stamp[k2] == mark) return;
      stamp[k2] = mark;
      test(k1, k2);
    };
    int c0[3], c1[3];
    if (cellRange(eb[k1].lo, eb[k1].hi, c0, c1) > 64) {
      for (uint32_t k2 = 0; k2 < (uint32_t)ne; ++k2) consider(k2);
      continue;
    }
    for (int z = c0[2]; z <= c1[2]; ++z)
      for (int y = c0[1]; y <= c1[1]; ++y)
        for (int x = c0[0]; x <= c1[0]; ++x) {
          const uint32_t k = cellHash(x, y, z) & mask;
          for (uint32_t e = start[k]; e < start[k + 1]; ++e) consider(items[e]);
        }
    for (uint32_t k2 : big) consider(k2);
  }
}

// Projection: the closest points (current positions) at least h apart along the pair's direction n.
void Cloth::selfEdgeSolve() {
  const float h = P_.selfContactDist;
  for (EdgeContact& c : se_) {
    if (c.stuck >= kGiveUp) continue;
    const uint32_t a = scEdges_[c.e1].first, b = scEdges_[c.e1].second, cc = scEdges_[c.e2].first,
                   d = scEdges_[c.e2].second;
    float s, t;
    segSeg(p_[a], p_[b], p_[cc], p_[d], s, t);
    const V3 g = (p_[a] + (p_[b] - p_[a]) * s) - (p_[cc] + (p_[d] - p_[cc]) * t);
    const float dn = c.n.dot(g);
    if (dn >= h) continue;
    const float wa = w_[a] * (1.f - s), wb = w_[b] * s, wc = w_[cc] * (1.f - t), wd = w_[d] * t;
    const float W = wa * (1.f - s) + wb * s + wc * (1.f - t) + wd * t;
    if (!(W > 0.f)) continue;
    const float lam = std::min(h - dn, h) / W;  // at most h per projection: a wrong pair cannot fling the cloth
    c.active = true;
    p_[a] += c.n * (wa * lam);
    p_[b] += c.n * (wb * lam);
    p_[cc] -= c.n * (wc * lam);
    p_[d] -= c.n * (wd * lam);
  }
}

// After the velocity update, as selfContactVelocity: separating velocity removed, sliding damped by friction.
void Cloth::selfEdgeVelocity() {
  const float h = P_.selfContactDist;
  for (EdgeContact& c : se_) {
    if (!c.active) continue;
    const uint32_t a = scEdges_[c.e1].first, b = scEdges_[c.e1].second, cc = scEdges_[c.e2].first,
                   d = scEdges_[c.e2].second;
    float s, t;
    segSeg(x_[a], x_[b], x_[cc], x_[d], s, t);
    const V3 g = (x_[a] + (x_[b] - x_[a]) * s) - (x_[cc] + (x_[d] - x_[cc]) * t);
    if (c.n.dot(g) < 0.5f * h) {
      if (c.stuck < 0xffff) ++c.stuck;
    } else {
      c.stuck = 0;
    }
    const float wa = w_[a] * (1.f - s), wb = w_[b] * s, wc = w_[cc] * (1.f - t), wd = w_[d] * t;
    const float W = wa * (1.f - s) + wb * s + wc * (1.f - t) + wd * t;
    if (!(W > 0.f)) continue;
    const V3 rel = (v_[a] * (1.f - s) + v_[b] * s) - (v_[cc] * (1.f - t) + v_[d] * t);
    const float vn = rel.dot(c.n);
    V3 dv = (rel - c.n * vn) * friction_;
    if (vn > 0.f) dv += c.n * vn;
    const V3 imp = dv * (1.f / W);
    v_[a] -= imp * wa;
    v_[b] -= imp * wb;
    v_[cc] += imp * wc;
    v_[d] += imp * wd;
  }
}

}  // namespace pxc
