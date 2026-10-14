// Self-collision (NX_CLF_SELFCOLLISION, flag 0x8): the prepass of the original's ComputeNewState (2.8.0 0x100ef1b0).
// Rules and evidence are in docs/re-notes-cloth.md, section "Self-collision". Written from that description.
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

#include "../fastfloor.h"
#include "cloth.h"

namespace pxc {

namespace {

// Spatial bin of a predicted position in pass p (0, 1, 2). Cells have side L = 10 D; the cell grid is offset by
// 0, +D and -2D in the three passes. Passes 1 and 2 bin only particles near one face of their cell. Returns the bin
// (0..255, an XOR hash of the cell coordinates) or -1 when the particle is not binned in this pass.
int selfBin(int pass, const float q[3], float L, float D) {
  const float invL = 1.f / L;
  const float off = pass == 0 ? 0.f : (pass == 1 ? D : -2.f * D);
  int c[3];
  float r[3];
  for (int k = 0; k < 3; ++k) {
    c[k] = (int)pxm::floorExact((q[k] - off) * invL);
    r[k] = q[k] - (float)c[k] * L;  // position relative to the cell origin (no offset)
  }
  if (pass == 1 && r[0] < L - 2.f * D && r[1] < L - 2.f * D && r[2] < L - 2.f * D) return -1;
  if (pass == 2 && (r[0] > 4.f * D || r[1] > 4.f * D || r[2] > 4.f * D)) return -1;
  const uint32_t h = (uint32_t)c[1] * 0x4bu ^ (uint32_t)c[2] * 0x19u ^ (uint32_t)c[0] * 0xfffffff7u;  // *0x4b ^ *0x19 ^ *(-9)
  return (int)(h & 0xffu);
}

}  // namespace

// One candidate pair (i, c), i binned before c. Pair order and the bins decide the Gauss-Seidel order; see the notes.
void Cloth::selfPair(size_t i, size_t c, float dt) {
  const float D = P_.thickness, D2 = D * D;
  // Prefilter on the cooked rest positions (the original's +0x70 field, set when the particles are built, not by
  // setPositions [A]): pairs closer than D at rest (neighbours, duplicated vertices) are never tested.
  const V3& ai = rest_[orig_[i]];
  const V3& ac = rest_[orig_[c]];
  const float dz0 = ac.z - ai.z, dy0 = ac.y - ai.y, dx0 = ac.x - ai.x;
  const float s0 = dz0 * dz0 + dy0 * dy0 + dx0 * dx0;
  if (s0 <= D2) return;
  // Predicted positions x + v dt with the current velocities (earlier pairs of this pass may have changed them).
  const V3 pi = x_[i] + v_[i] * dt, pc = x_[c] + v_[c] * dt;
  const V3 d = pc - pi;
  if (!(d.z * d.z + d.y * d.y + d.x * d.x < D2)) return;
  const float wi = w_[i], wc = w_[c];
  if (wi + wc == 0.f) return;
  const float inv = 1.f / (wi + wc);
  const float r = std::sqrt(d.x * d.x + d.y * d.y + d.z * d.z);
  if (r == 0.f) return;
  // Separation: both velocities move so that the predicted distance becomes D, mass-weighted (impulse / dt).
  const float s = ((r - D) / r) * inv * (1.f / dt);
  const V3 a = d * s;
  v_[i] += V3(a.x * wi, a.y * wi, a.z * wi);
  v_[c] -= V3(a.x * wc, a.y * wc, a.z * wc);
  // Relative velocity damping with the cloth friction coefficient (the original's +0x64 field), mass-weighted:
  // v_i += k w_i (v_c - v_i), v_c -= k w_c (v_c - v_i), k = friction / (w_i + w_c) / 2.
  const float k = friction_ * inv * 0.5f;
  const V3 g = (v_[c] - v_[i]) * k;
  v_[i] += V3(g.x * wi, g.y * wi, g.z * wi);
  v_[c] -= V3(g.x * wc, g.y * wc, g.z * wc);
}

// Prepass: three passes; each bins the particles by predicted position, then tests every pair inside a bin in
// particle-index order (bins in index order). Only velocities change. Cost O(n) per pass plus the pairs in each bin.
void Cloth::selfCollision(float dt) {
  const float D = P_.thickness;  // the original's 2 x collision radius (radius = thickness / 2)
  if (D == 0.f || dt == 0.f) return;
  const float L = 10.f * D;
  const size_t n = x_.size();
  const uint8_t* rm = anyRemoved_ ? removed_.data() : nullptr;  // removed particles are not binned
  std::vector<int>& bin = selfBin_;
  std::vector<size_t>& start = selfStart_;
  std::vector<size_t>& fillAt = selfFill_;
  std::vector<size_t>& order = selfOrder_;
  bin.resize(n);
  start.resize(257);
  fillAt.resize(256);
  order.resize(n);
  for (int pass = 0; pass < 3; ++pass) {
    std::fill(start.begin(), start.end(), 0);
    for (size_t i = 0; i < n; ++i) {
      const float q[3] = {x_[i].x + v_[i].x * dt, x_[i].y + v_[i].y * dt, x_[i].z + v_[i].z * dt};
      bin[i] = rm && rm[i] ? -1 : selfBin(pass, q, L, D);
      if (bin[i] >= 0) ++start[bin[i] + 1];
    }
    for (int b = 0; b < 256; ++b) start[b + 1] += start[b];
    for (int b = 0; b < 256; ++b) fillAt[b] = start[b];
    for (size_t i = 0; i < n; ++i)
      if (bin[i] >= 0) order[fillAt[bin[i]]++] = i;  // counting sort: index order within a bin
    for (int b = 0; b < 256; ++b)
      for (size_t a = start[b]; a < start[b + 1]; ++a)
        for (size_t c = a + 1; c < start[b + 1]; ++c) selfPair(order[a], order[c], dt);
  }
}

}  // namespace pxc
