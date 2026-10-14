// Box trees for cloth collision (cloth.h): a median-split AABB tree over a TriMesh's triangles, or over shapes.
// The per-triangle contact rules live with the other shape tests (cloth.cpp, meshTests).
#include <algorithm>
#include <cmath>

#include "cloth.h"

namespace pxc {

namespace {
inline V3 vmin(const V3& a, const V3& b) { return {std::min(a.x, b.x), std::min(a.y, b.y), std::min(a.z, b.z)}; }
inline V3 vmax(const V3& a, const V3& b) { return {std::max(a.x, b.x), std::max(a.y, b.y), std::max(a.z, b.z)}; }
inline float axisOf(const V3& v, int a) { return a == 0 ? v.x : a == 1 ? v.y : v.z; }

constexpr uint32_t kLeafTris = 4;

struct Builder {
  const std::vector<V3>& tlo;
  const std::vector<V3>& thi;
  std::vector<V3> cen;
  std::vector<TriMesh::Node>& nodes;
  std::vector<uint32_t>& order;
  void node(uint32_t first, uint32_t count, int depth) {
    const uint32_t k = (uint32_t)nodes.size();
    nodes.emplace_back();
    V3 lo = tlo[order[first]], hi = thi[order[first]], clo = cen[order[first]], chi = clo;
    for (uint32_t i = first; i < first + count; ++i) {
      lo = vmin(lo, tlo[order[i]]);
      hi = vmax(hi, thi[order[i]]);
      clo = vmin(clo, cen[order[i]]);
      chi = vmax(chi, cen[order[i]]);
    }
    nodes[k].lo = lo;
    nodes[k].hi = hi;
    const V3 ext = chi - clo;
    // depth cap keeps the query stack (64) safe: a split adds at most one pending entry per level
    if (count <= kLeafTris || depth >= 48 || (ext.x <= 0 && ext.y <= 0 && ext.z <= 0)) {
      nodes[k].first = first;
      nodes[k].count = count;
      return;
    }
    const int ax = ext.x >= ext.y && ext.x >= ext.z ? 0 : ext.y >= ext.z ? 1 : 2;
    const uint32_t mid = first + count / 2;
    std::nth_element(order.begin() + first, order.begin() + mid, order.begin() + first + count,
                     [&](uint32_t a, uint32_t b) { return axisOf(cen[a], ax) < axisOf(cen[b], ax); });
    node(first, mid - first, depth + 1);  // left child = k + 1
    nodes[k].first = (uint32_t)nodes.size();
    nodes[k].count = 0;
    node(mid, first + count - mid, depth + 1);
  }
};
}  // namespace

void buildBoxTree(const std::vector<V3>& lo, const std::vector<V3>& hi, std::vector<TriMesh::Node>& nodes,
                  std::vector<uint32_t>& order) {
  nodes.clear();
  order.clear();
  const size_t n = lo.size();
  if (n == 0) return;
  Builder b{lo, hi, {}, nodes, order};
  b.cen.resize(n);
  order.resize(n);
  for (size_t t = 0; t < n; ++t) {
    b.cen[t] = (lo[t] + hi[t]) * 0.5f;
    order[t] = (uint32_t)t;
  }
  nodes.reserve(2 * n / kLeafTris + 1);
  b.node(0, (uint32_t)n, 0);
}

void TriMesh::build() {
  const size_t n = triCount();
  std::vector<V3> lo(n), hi(n);
  for (size_t t = 0; t < n; ++t) {
    const V3 &a = v[3 * t], &c1 = v[3 * t + 1], &c2 = v[3 * t + 2];
    lo[t] = vmin(a, vmin(c1, c2));
    hi[t] = vmax(a, vmax(c1, c2));
  }
  buildBoxTree(lo, hi, nodes, order);
}

}  // namespace pxc
