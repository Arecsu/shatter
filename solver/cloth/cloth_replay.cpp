// Replays a cloth recording from the game (PhysXLoader_cloth<id>.pxcr, see cloth_rec.h) on the native solver.
//   cloth_replay file.pxcr [--frames a b out.bin]
// Checks the position hashes the shim recorded (the replay is only trusted while they match), prints the bullet hits
// with what follows each of them, and the steps where triangles are stretched far past their rest size.
// --frames: positions and triangles of steps a..b in the PXCL format of run_scenario (for the renderers).
// --fall: particles that went through a floor. Per particle, its last contact with a static shape whose normal points up
// (against gravity); a particle that is later more than 0.5 m below that contact while within 0.25 m of it sideways
// (straight down, not off a ledge) is reported once, as are particles the valid bounds removed below their last floor.
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <memory>
#include <tuple>
#include <unordered_map>
#include <string>
#include <vector>

#include "cloth.h"
#include "cloth_rec.h"
#include "../fluid/thread_pool.h"

using namespace pxc;

namespace {
struct Reader {
  std::vector<uint8_t> b;
  size_t o = 0;
  bool ok = true;
  void raw(void* p, size_t n) {
    if (o + n > b.size()) { ok = false; memset(p, 0, n); return; }
    memcpy(p, b.data() + o, n);
    o += n;
  }
  uint8_t u8() { uint8_t v; raw(&v, 1); return v; }
  uint32_t u32() { uint32_t v; raw(&v, 4); return v; }
  float f() { float v; raw(&v, 4); return v; }
  V3 v3() { float a = f(), c = f(), d = f(); return V3(a, c, d); }
  template <class T> std::vector<T> vec() {
    std::vector<T> v(u32());
    if (!v.empty()) raw(v.data(), v.size() * sizeof(T));
    return v;
  }
};

ClothParams paramsFrom(const std::vector<uint8_t>& blob) {
  Reader r;
  r.b = blob;
  ClothParams p;
  p.thickness = r.f(), p.density = r.f(), p.bending = r.f(), p.stretching = r.f(), p.damping = r.f(), p.friction = r.f();
  p.solverIterations = r.u32(), p.sleepLinearVelocity = r.f(), p.wakeUpCounter = r.f();
  p.externalAcceleration = r.v3(), p.windAcceleration = r.v3();
  bool* fl[7] = {&p.useGravity, &p.useBending, &p.bendingOrtho, &p.useDamping, &p.useComDamping, &p.useTearing,
                 &p.useSelfCollision};
  for (bool* q : fl) *q = r.u8() != 0;
  p.tearFactor = r.f(), p.maxVertices = r.u32(), p.attachmentResponse = r.f(), p.attachmentTearFactor = r.f();
  p.disableDynamicCcd = r.u8() != 0, p.refDt = r.f(), p.selfContactDist = r.f();
  if (r.o < r.b.size()) p.disableCollision = r.u8() != 0;
  if (r.o < r.b.size()) {  // appended after disableCollision
    p.useValidBounds = r.u8() != 0;
    for (int k = 0; k < 3; ++k) p.validLo[k] = r.f();
    for (int k = 0; k < 3; ++k) p.validHi[k] = r.f();
  }
  if (r.o < r.b.size()) p.edgeShapeContact = r.u8() != 0;  // appended after the valid bounds
  return p;
}

// Front/back swaps of resting vertices against other pieces' triangles (the floor flicker): per vertex, the nearest
// triangle within 3 cm that is not one of its neighbours' (by rest vertex, so tear copies of a neighbour count too) and
// the side it is on; a flip of that side between two steps while both are slow is a swap.
struct Flicker {
  std::map<uint64_t, int8_t> side;  // (vertex << 32 | triangle) -> side last step
  long swaps = 0;
  int verbose = 0;
  std::vector<std::pair<uint64_t, long>> perStep;  // (step, swaps) for steps with any
  void scan(const Cloth& c, uint64_t step) {
    const auto& x = c.pos();
    const auto& v = c.vel();
    const auto& t = c.tri();
    const float cell = 0.25f, R = 0.03f;
    std::map<std::tuple<int, int, int>, std::vector<uint32_t>> grid;
    std::vector<std::vector<int>> nb(x.size());
    for (size_t k = 0; k + 2 < t.size(); k += 3)
      for (int e = 0; e < 3; ++e)
        for (int f = 0; f < 3; ++f) nb[t[k + e]].push_back(c.origin(t[k + f]));
    for (size_t k = 0; k + 2 < t.size(); k += 3) {
      V3 m = (x[t[k]] + x[t[k + 1]] + x[t[k + 2]]) * (1.f / 3.f);
      grid[{(int)std::floor(m.x / cell), (int)std::floor(m.y / cell), (int)std::floor(m.z / cell)}].push_back((uint32_t)k);
    }
    std::map<uint64_t, int8_t> now;
    long sw = 0;
    for (uint32_t i = 0; i < x.size(); ++i) {
      if (v[i].len2() > 0.25f * 0.25f) continue;
      const int gx = (int)std::floor(x[i].x / cell), gy = (int)std::floor(x[i].y / cell), gz = (int)std::floor(x[i].z / cell);
      float best = R;
      uint64_t bk = 0;
      int8_t bs = 0;
      for (int dx = -1; dx <= 1; ++dx)
        for (int dy = -1; dy <= 1; ++dy)
          for (int dz = -1; dz <= 1; ++dz) {
            auto it = grid.find({gx + dx, gy + dy, gz + dz});
            if (it == grid.end()) continue;
            for (uint32_t k : it->second) {
              const uint32_t a = t[k], b = t[k + 1], d = t[k + 2];
              bool skip = false;
              for (uint32_t w : {a, b, d})
                for (int o : nb[i]) skip = skip || c.origin(w) == o;
              if (skip) continue;
              V3 n = (x[b] - x[a]).cross(x[d] - x[a]);
              const float l = std::sqrt(n.len2());
              if (l < 1e-9f) continue;
              n = n * (1.f / l);
              const float dist = (x[i] - x[a]).dot(n);
              if (std::fabs(dist) >= best) continue;
              const V3 q = x[i] - n * dist;
              if (n.dot((x[b] - x[a]).cross(q - x[a])) < 0 || n.dot((x[d] - x[b]).cross(q - x[b])) < 0 ||
                  n.dot((x[a] - x[d]).cross(q - x[d])) < 0)
                continue;
              best = std::fabs(dist), bk = ((uint64_t)i << 32) | (k / 3), bs = dist >= 0 ? 1 : -1;
            }
          }
      if (bs == 0) continue;
      now[bk] = bs;
      auto p = side.find(bk);
      if (p != side.end() && p->second != bs) {
        ++sw;
        if (getenv("FLICKER_VERBOSE") && verbose++ < 40) {
          const uint32_t k = (uint32_t)(bk & 0xffffffffu) * 3;
          printf("    swap step %llu: vertex %u (rest %d) z %.3f |v| %.3f vs triangle %u (%u %u %u) dist %.4f\n",
                 (unsigned long long)step, i, c.origin(i), x[i].z, std::sqrt(v[i].len2()), k / 3, t[k], t[k + 1],
                 t[k + 2], best * bs);
        }
      }
    }
    side.swap(now);
    swaps += sw;
    if (sw) perStep.push_back({step, sw});
  }
};

struct Stats {
  size_t n = 0;
  float vmax = 0, vmean = 0, stretch = 0;  // stretch: longest triangle edge / its rest length
  int fast = 0, stretched = 0;              // > 20 m/s; triangles with an edge over 3x rest
  V3 lo, hi;
};
Stats stats(const Cloth& c, const std::vector<V3>& rest) {
  Stats s;
  const auto& x = c.pos();
  const auto& v = c.vel();
  const auto& t = c.tri();
  s.n = x.size();
  s.lo = V3(1e30f, 1e30f, 1e30f), s.hi = V3(-1e30f, -1e30f, -1e30f);
  for (size_t i = 0; i < x.size(); ++i) {
    const float sp = std::sqrt(v[i].len2());
    s.vmax = std::max(s.vmax, sp);
    s.vmean += sp;
    s.fast += sp > 20.f;
    s.lo = V3(std::min(s.lo.x, x[i].x), std::min(s.lo.y, x[i].y), std::min(s.lo.z, x[i].z));
    s.hi = V3(std::max(s.hi.x, x[i].x), std::max(s.hi.y, x[i].y), std::max(s.hi.z, x[i].z));
  }
  if (!x.empty()) s.vmean /= x.size();
  for (size_t k = 0; k + 2 < t.size(); k += 3) {
    float worst = 0;
    for (int e = 0; e < 3; ++e) {
      const uint32_t a = t[k + e], b = t[k + (e + 1) % 3];
      const int ra = c.origin(a), rb = c.origin(b);
      const float L0 = std::sqrt((rest[ra] - rest[rb]).len2());
      if (L0 > 1e-6f) worst = std::max(worst, std::sqrt((x[a] - x[b]).len2()) / L0);
    }
    s.stretch = std::max(s.stretch, worst);
    s.stretched += worst > 3.f;
  }
  return s;
}

// Self crossings: edges of the cloth that pierce one of its own triangles (no shared vertex), on a hashed grid of
// triangle boxes. --isect n counts them every n steps (light: nothing is written; analysis/cloth_isect.py does the
// same on a --frames dump).
long crossings(const std::vector<V3>& x, const std::vector<uint32_t>& tri) {
  std::vector<uint32_t> T;
  for (size_t t = 0; t + 2 < tri.size(); t += 3)
    if (!(tri[t] == tri[t + 1] && tri[t + 1] == tri[t + 2])) T.push_back((uint32_t)t);
  if (T.empty()) return 0;
  std::vector<float> L;
  for (uint32_t t : T) L.push_back(std::sqrt((x[tri[t]] - x[tri[t + 1]]).len2()));
  std::nth_element(L.begin(), L.begin() + L.size() / 2, L.end());
  const float cell = std::max(2.f * L[L.size() / 2], 1e-4f), inv = 1.f / cell;
  auto key = [](int a, int b, int c) { return ((uint64_t)(uint32_t)(a & 0x1fffff) << 42) | ((uint64_t)(uint32_t)(b & 0x1fffff) << 21) | (uint32_t)(c & 0x1fffff); };
  std::unordered_map<uint64_t, std::vector<uint32_t>> grid;
  auto lohi = [&](const V3& a, const V3& b, int lo[3], int hi[3]) {
    const float pa[3] = {a.x, a.y, a.z}, pb[3] = {b.x, b.y, b.z};
    for (int k = 0; k < 3; ++k) lo[k] = (int)std::floor(std::min(pa[k], pb[k]) * inv), hi[k] = (int)std::floor(std::max(pa[k], pb[k]) * inv);
  };
  for (uint32_t t : T) {
    const V3 &a = x[tri[t]], &b = x[tri[t + 1]], &c = x[tri[t + 2]];
    int lo[3], hi[3], l2[3], h2[3];
    lohi(a, b, lo, hi), lohi(c, c, l2, h2);
    for (int k = 0; k < 3; ++k) lo[k] = std::min(lo[k], l2[k]), hi[k] = std::max(hi[k], h2[k]);
    if ((long)(hi[0] - lo[0] + 1) * (hi[1] - lo[1] + 1) * (hi[2] - lo[2] + 1) > 64) continue;  // a stretched spike: skipped
    for (int i = lo[0]; i <= hi[0]; ++i)
      for (int j = lo[1]; j <= hi[1]; ++j)
        for (int k = lo[2]; k <= hi[2]; ++k) grid[key(i, j, k)].push_back(t);
  }
  std::vector<uint64_t> E;
  for (uint32_t t : T)
    for (int k = 0; k < 3; ++k) {
      uint32_t a = tri[t + k], b = tri[t + (k + 1) % 3];
      if (a > b) std::swap(a, b);
      E.push_back((uint64_t)a << 32 | b);
    }
  std::sort(E.begin(), E.end());
  E.erase(std::unique(E.begin(), E.end()), E.end());
  long n = 0;
  std::vector<uint32_t> seen;
  for (uint64_t e : E) {
    const uint32_t ia = (uint32_t)(e >> 32), ib = (uint32_t)e;
    const V3 &P = x[ia], &Q = x[ib], d = Q - P;
    int lo[3], hi[3];
    lohi(P, Q, lo, hi);
    if ((long)(hi[0] - lo[0] + 1) * (hi[1] - lo[1] + 1) * (hi[2] - lo[2] + 1) > 64) continue;
    seen.clear();
    for (int i = lo[0]; i <= hi[0]; ++i)
      for (int j = lo[1]; j <= hi[1]; ++j)
        for (int k = lo[2]; k <= hi[2]; ++k) {
          auto it = grid.find(key(i, j, k));
          if (it == grid.end()) continue;
          for (uint32_t t : it->second) {
            if (std::find(seen.begin(), seen.end(), t) != seen.end()) continue;
            seen.push_back(t);
            const uint32_t u = tri[t], v = tri[t + 1], w = tri[t + 2];
            if (u == ia || v == ia || w == ia || u == ib || v == ib || w == ib) continue;
            const V3 e1 = x[v] - x[u], e2 = x[w] - x[u], h = d.cross(e2);
            const float det = e1.dot(h);
            if (std::fabs(det) < 1e-12f) continue;
            const float f = 1.f / det;
            const V3 s = P - x[u];
            const float bu = s.dot(h) * f;
            if (bu < 0.f || bu > 1.f) continue;
            const V3 q = s.cross(e1);
            const float bv = d.dot(q) * f;
            if (bv < 0.f || bu + bv > 1.f) continue;
            const float tt = e2.dot(q) * f;
            if (tt > 0.f && tt < 1.f) ++n;
          }
        }
  }
  return n;
}
}  // namespace

static pxr::ThreadPool* g_pool = nullptr;
static void poolRun(size_t n, void (*fn)(void*, size_t), void* ctx) { g_pool->run(n, fn, ctx); }

int main(int argc, char** argv) {
  bool dual = false;
  // --keep: a later Create (a shim rebuild) starts from this replay's positions and velocities, not the recorded ones,
  // and the velocity writes that restore them are skipped (the recording's state is not carried in): a behaviour change
  // stays in effect past the shim's rebuilds.
  bool keepState = false;
  bool skipRestoreVel = false;
  // --to-delta out.pxcr: writes the recording with each velocity write replaced by the change it made on the recording
  // build (kRecAddVel), so replays with other rules (this build, the original core) take the game's change, not the old
  // solver's state.
  const char* deltaOut = nullptr;
  std::vector<uint8_t> dbuf;
  V3 lastDelta;
  if (argc < 2) {
    fprintf(stderr, "usage: cloth_replay file.pxcr [--frames a b out.bin] [--quiet]\n");
    return 2;
  }
  long fa = -1, fb = -1;
  const char* fout = nullptr;
  bool quiet = false;
  std::vector<std::pair<long, long>> isectLog;  // (step, crossings)
  long isectEvery = 0;
  long stretchEvery = 0;
  long hookEvery = 0, hookAt = -1;  // --hook-at step: the edges through shapes at that step, and the shapes near them  // --hook n: edges passing through a shape every n steps
  std::vector<std::pair<long, long>> hookLog;
  const char* edgesOut = nullptr; long edgesAt = -1;  // --edges-at step file: the solver's edges (i j L0) after that step  // --stretch n: largest edge stretch every n steps, with the step length (slow motion)
  struct StretchRec { long step; float dt, s, sFree, sBoth; };  // all edges; no pinned end; both ends pinned
  std::vector<StretchRec> stretchLog;
  float lastDt = 0.f;
  long gvA = -1, gvB = -1;  // --gvstat a b: fit the game's velocity changes as d = -k v + w over steps a..b
  double gvS[4][4] = {};   // per axis: sum v, sum d, sum v v, sum v d (index 3: count)
  double gvDt = 0; long gvSteps = 0;
  double gvLostZ = 0, gvDz = 0, gvCurZ = 0;  // sums of (value - current).z, change.z, current.z
  float probeZ = -1e30f;
  bool fallOn = false;  // --fall
  struct FloorHit { V3 at; bool has = false, flagged = false, removed = false; };
  std::vector<FloorHit> floorOf;
  std::map<long, std::pair<int, float>> fallSteps, fallRemoved;  // step -> (particles, deepest drop below the floor)
  long fallTotal = 0, fallRemovedTotal = 0, removedNoFloor = 0;
  V3 fallUp(0.f, 0.f, 1.f);
  std::map<std::string, int> removedFaces;
  int removedDetail = 0;
  float dropSum = 0.f, dropMax = 0.f;
  std::vector<V3> snapOld, snapNew; double simT = 0, snapOldT = 0, snapNewT = 0;  // --probe: positions 200-400 steps back  // --probe z: at the end, the still particles above z (pins, attachments, contacts)  // --isect n: self crossings every n steps (summary at the end)
  long ta = -1, tb = -1;  // --trace a b: every call made during steps a..b
  long shapesFrom = -1;
  float ovSelf = -1, ovRefDt = -1;  // --self h, --refdt t: override those parameters (experiments)
  int ovEdge = -1;                  // --edge-shape 0|1: override ClothParams::edgeShapeContact
  bool noMesh = false;              // --no-mesh: scene meshes left out
  std::vector<long> skipShapes;     // --skip-shapes i,j,..: these shapes (add order) left out
  long shapeIdx = 0;
  float poseM[12];                  // --pose m00 m01 .. m22 tx ty tz: start positions = M rest + t (row-major M)
  bool poseOv = false;
  float clockDt = 0;                // --clock hz: re-time as the shim's fixed cloth clock (accumulate, step 1/hz)
  double clockAcc = 0;
  std::vector<V3> velH1, velH2;  // velocities at the end of the previous two steps (--trace diagnostics)
  std::vector<V3> posH1, posH2;  // positions likewise
  float dtH1 = 0, dtH2 = 0;
  long flA = -1, flB = -1;          // --flicker a b: front/back swap scan over steps a..b
  Flicker fl;   // --drop-shapes n: shapes added from step n on are replaced by a far-away point
  for (int i = 2; i < argc; ++i) {
    if (!strcmp(argv[i], "--veldelta") && i + 2 < argc) { i += 2; continue; }
    if (!strcmp(argv[i], "--dual")) { dual = true; continue; }
    if (!strcmp(argv[i], "--keep")) { keepState = true; continue; }
    if (!strcmp(argv[i], "--threads") && i + 1 < argc) { const int t = atoi(argv[++i]); if (t > 1) g_pool = new pxr::ThreadPool(t); continue; }
    if (!strcmp(argv[i], "--to-delta") && i + 1 < argc) { deltaOut = argv[++i]; dual = true; continue; }
    if (!strcmp(argv[i], "--self") && i + 1 < argc) { ovSelf = (float)atof(argv[++i]); continue; }
    if (!strcmp(argv[i], "--refdt") && i + 1 < argc) { ovRefDt = (float)atof(argv[++i]); continue; }
    if (!strcmp(argv[i], "--edge-shape") && i + 1 < argc) { ovEdge = atoi(argv[++i]); continue; }
    if (!strcmp(argv[i], "--no-mesh")) { noMesh = true; continue; }
    if (!strcmp(argv[i], "--pose") && i + 12 < argc) {
      for (int k = 0; k < 12; ++k) poseM[k] = (float)atof(argv[++i]);
      poseOv = true;
      continue;
    }
    if (!strcmp(argv[i], "--skip-shapes") && i + 1 < argc) {
      for (char* t = strtok(argv[++i], ","); t; t = strtok(nullptr, ",")) {
        long a = atol(t), b = a;
        if (const char* d = strchr(t, '-')) b = atol(d + 1);
        for (long k = a; k <= b; ++k) skipShapes.push_back(k);
      }
      continue;
    }
    if (!strcmp(argv[i], "--clock") && i + 1 < argc) { clockDt = 1.f / (float)atof(argv[++i]); continue; }
    if (!strcmp(argv[i], "--flicker") && i + 2 < argc) { flA = atol(argv[i + 1]), flB = atol(argv[i + 2]), i += 2; continue; }
    if (!strcmp(argv[i], "--drop-shapes") && i + 1 < argc) { shapesFrom = atol(argv[++i]); continue; }
    if (!strcmp(argv[i], "--trace") && i + 2 < argc) { ta = atol(argv[i + 1]), tb = atol(argv[i + 2]), i += 2; continue; }
    if (!strcmp(argv[i], "--frames") && i + 3 < argc) fa = atol(argv[i + 1]), fb = atol(argv[i + 2]), fout = argv[i + 3], i += 3;
    else if (!strcmp(argv[i], "--quiet")) quiet = true;
    else if (!strcmp(argv[i], "--isect") && i + 1 < argc) isectEvery = atol(argv[++i]);
    else if (!strcmp(argv[i], "--gvstat") && i + 2 < argc) gvA = atol(argv[i + 1]), gvB = atol(argv[i + 2]), i += 2;
    else if (!strcmp(argv[i], "--edges-at") && i + 2 < argc) edgesAt = atol(argv[i + 1]), edgesOut = argv[i + 2], i += 2;
    else if (!strcmp(argv[i], "--hook-at") && i + 1 < argc) hookAt = atol(argv[++i]);
    else if (!strcmp(argv[i], "--hook") && i + 1 < argc) hookEvery = atol(argv[++i]);
    else if (!strcmp(argv[i], "--stretch") && i + 1 < argc) stretchEvery = atol(argv[++i]);
    else if (!strcmp(argv[i], "--probe") && i + 1 < argc) probeZ = (float)atof(argv[++i]);
    else if (!strcmp(argv[i], "--fall")) fallOn = true;
  }
  Reader r;
  {
    FILE* f = fopen(argv[1], "rb");
    if (!f) { perror(argv[1]); return 1; }
    fseek(f, 0, SEEK_END);
    r.b.resize(ftell(f));
    fseek(f, 0, SEEK_SET);
    if (fread(r.b.data(), 1, r.b.size(), f) != r.b.size()) return 1;
    fclose(f);
  }
  if (r.u32() != 0x52435850u || r.u32() != kRecVersion) { fprintf(stderr, "not a PXCR v1 file\n"); return 1; }
  std::vector<std::unique_ptr<TriMesh>> meshes;
  std::unique_ptr<Cloth> c;
  // --dual: c runs this build; a second cloth `a` runs the recording build's call rules (attach and free calls did not
  // wake the cloth) and so reproduces the recording (the hash checks run on it). The game's velocity writes reach c as
  // the change they made on `a` (the recorded values carry the recording solver's state).
  std::unique_ptr<Cloth> a;
  auto both = [&](auto f) { f(*c); if (a) f(*a); };
  auto legacy = [&](auto f) {  // a call on `a` that must not change its sleep state
    if (!a) return;
    const bool s0 = a->isSleeping();
    const float w0 = a->wakeCounter();
    f(*a);
    a->setSleepState(s0, w0);
  };
  std::vector<V3> rest;
  uint64_t step = 0;
  int creates = 0, checks = 0, mismatches = 0;
  long firstBad = -1;
  long watchUntil = -1;  // print every step until here (after a hit)
  FILE* fo = nullptr;
  uint32_t framesWritten = 0;
  std::vector<uint32_t> lastTri;
  long frameHdrPos = 0;
  auto line = [&](const char* tag) {
    Stats s = stats(*c, rest);
    printf("  %s step %6llu: n %5zu splits %4d vmax %7.1f vmean %6.2f fast %4d stretch %6.1f (>3x: %3d tris) box %.1fx%.1fx%.1f%s\n",
           tag, (unsigned long long)step, s.n, c->splits(), s.vmax, s.vmean, s.fast, s.stretch, s.stretched,
           s.hi.x - s.lo.x, s.hi.y - s.lo.y, s.hi.z - s.lo.z, c->isSleeping() ? " asleep" : "");
  };
  long opCount[23] = {};
  double stepMs = 0, stepMax = 0;
  bool vdOn = false;  // --veldelta a b: mean (written - current) velocity of the game's writes per step
  long vdA = -1, vdB = -1;
  V3 vdSum;
  long vdN = 0;
  for (int i = 2; i < argc; ++i)
    if (!strcmp(argv[i], "--veldelta") && i + 2 < argc) vdA = atol(argv[i + 1]), vdB = atol(argv[i + 2]);
  long attSinceStep = 0, sleepSteps = 0, sleepWithAtt = 0, sleepRuns = 0;
  bool wasAsleep = false;
  if (deltaOut) dbuf.assign(r.b.begin(), r.b.begin() + r.o);
  while (r.ok && r.o < r.b.size()) {
    const size_t opStart = r.o;
    const uint8_t op = r.u8();
    const bool tr = (long)step >= ta && (long)step <= tb;
    static const char* names[] = {"?", "create", "mesh", "addShape", "pin", "invMass", "motion", "detach", "attachShape",
                                  "attachGlobal", "wake", "sleep", "setVel", "forceV", "forcePos", "attResp", "attTear",
                                  "params", "step", "check", "note", "addVel", "gameVel"};
    if (op < 23) ++opCount[op];
    if (tr && op != kRecStep && op != kRecCheck && op < 23) printf("    [step %llu] %s\n", (unsigned long long)step, names[op]);
    switch (op) {
      case kRecNote: {
        std::vector<char> s(r.u32());
        r.raw(s.data(), s.size());
        printf("note: %.*s\n", (int)s.size(), s.data());
        break;
      }
      case kRecCreate: {
        ClothParams p = paramsFrom(r.vec<uint8_t>());
        if (ovSelf >= 0) p.selfContactDist = ovSelf;
        if (ovRefDt >= 0) p.refDt = ovRefDt;
        if (ovEdge >= 0) p.edgeShapeContact = ovEdge != 0;
        rest = r.vec<V3>();
        const auto tri = r.vec<uint32_t>();
        auto x0 = r.vec<V3>();
        std::vector<V3> keepV;
        if (keepState && c && c->pos().size() == x0.size()) {
          x0 = c->pos();
          keepV = c->vel();
          skipRestoreVel = true;
        }
        if (poseOv) {
          const float* M = poseM;
          for (size_t i = 0; i < x0.size(); ++i) {
            const V3& q = rest[i];
            x0[i] = V3(M[0] * q.x + M[1] * q.y + M[2] * q.z + M[9], M[3] * q.x + M[4] * q.y + M[5] * q.z + M[10],
                       M[6] * q.x + M[7] * q.y + M[8] * q.z + M[11]);
          }
        }
        std::vector<Edge> edges;
        const bool hasE = r.u8();
        if (hasE) edges = r.vec<Edge>();
        const bool storedRest = r.u8();
        c.reset(new Cloth(p, rest, tri, x0, hasE ? &edges : nullptr, storedRest));
        if (dual) a.reset(new Cloth(p, rest, tri, x0, hasE ? &edges : nullptr, storedRest));
        if (g_pool) c->setParallelFor(poolRun);
        for (size_t i = 0; i < keepV.size(); ++i) c->setVelocity(i, keepV[i]);
        ++creates;
        printf("create #%d at step %llu: %zu vertices, %zu triangles, flags tear %d bend %d, self %.3f, thickness %.3f\n",
               creates, (unsigned long long)step, x0.size(), tri.size() / 3, p.useTearing, p.useBending,
               p.selfContactDist, p.thickness);
        break;
      }
      case kRecMesh: {
        const uint32_t id = r.u32();
        if (meshes.size() <= id) meshes.resize(id + 1);
        meshes[id].reset(new TriMesh());
        meshes[id]->v = r.vec<V3>();
        meshes[id]->build();
        break;
      }
      case kRecAddShape: {
        StaticShape s{ShapeType::Plane, V3(), 0, V3()};
        s.type = (ShapeType)r.u32();
        s.a = r.v3(), s.d = r.f(), s.half = r.v3();
        const uint32_t id = r.u32();
        s.mesh = id < meshes.size() ? meshes[id].get() : nullptr;
        if (tr) printf("      shape type %u at (%.2f %.2f %.2f) d %g half (%.2f %.2f %.2f) mesh %d (%zu tris)\n", (unsigned)s.type,
                       s.a.x, s.a.y, s.a.z, s.d, s.half.x, s.half.y, s.half.z, (int)id, s.mesh ? s.mesh->triCount() : 0);
        const bool skip = std::find(skipShapes.begin(), skipShapes.end(), shapeIdx++) != skipShapes.end();
        if (skip || (shapesFrom >= 0 && (long)step >= shapesFrom) || (noMesh && s.type == ShapeType::Mesh)) s = StaticShape{ShapeType::Sphere, V3(1e7f, 1e7f, 1e7f), 0.f, V3()};
        both([&](Cloth& k) { k.addShape(s); });
        break;
      }
      case kRecPin: { const uint32_t v = r.u32(); const V3 p = r.v3(); both([&](Cloth& k) { k.pinHard(v, p); }); break; }
      case kRecInvMass: { const uint32_t s = r.u32(); const float w = r.f(); both([&](Cloth& k) { k.setShapeInvMass(s, w); }); break; }
      case kRecShapeVel: {
        const uint32_t s = r.u32();
        const V3 lin = r.v3(), ang = r.v3(), com = r.v3();
        both([&](Cloth& k) { k.setShapeVelocity(s, lin, ang, com); });
        break;
      }
      case kRecRemoved: {
        const std::vector<uint8_t> m = r.vec<uint8_t>();
        both([&](Cloth& k) { k.setRemoved(m); });
        break;
      }
      case kRecMotion: {
        const uint32_t s = r.u32();
        const V3 t = r.v3();
        float R[9], pR[9];
        r.raw(R, 36);
        V3 pt;
        const bool hasPt = r.u8();
        if (hasPt) pt = r.v3();
        const bool hasPR = r.u8();
        if (hasPR) r.raw(pR, 36);
        const bool dyn = r.u8();
        both([&](Cloth& k) { k.setShapeMotion(s, t, R, hasPt ? &pt : nullptr, hasPR ? pR : nullptr, dyn); });
        break;
      }
      case kRecDetach: { const uint32_t s = r.u32(); c->detachFromShape(s); legacy([&](Cloth& k) { k.detachFromShape(s); }); break; }
      case kRecAttachShape: {
        const uint32_t v = r.u32(), s = r.u32();
        const V3 l = r.v3();
        const uint32_t fl = r.u32();
        c->attachVertexToShape(v, s, l, fl);
        legacy([&](Cloth& k) { k.attachVertexToShape(v, s, l, fl); });
        break;
      }
      case kRecAttachGlobal: {
        const uint32_t v = r.u32();
        const V3 p = r.v3();
        if (tr) printf("      pin %u at (%.4f %.4f %.4f), now at (%.4f %.4f %.4f)\n", v, p.x, p.y, p.z, c->pos()[v].x, c->pos()[v].y, c->pos()[v].z);
        c->attachVertexToGlobalPosition(v, p);
        legacy([&](Cloth& k) { k.attachVertexToGlobalPosition(v, p); });
        ++attSinceStep;
        break;
      }
      case kRecWake: { const float w = r.f(); both([&](Cloth& k) { k.wakeUp(w); }); break; }
      case kRecSleep: both([](Cloth& k) { k.putToSleep(); }); break;
      case kRecSetVel: {
        const uint32_t i = r.u32();
        const V3 v = r.v3();
        if (skipRestoreVel) break;  // --keep: the shim restoring the recorded velocities after a rebuild
        if (tr) {
          auto dd = [&](const std::vector<V3>& h) { return i < h.size() ? std::sqrt((h[i] - v).len2()) : -1.f; };
          printf("      v[%u] = (%g %g %g) | now (%g %g %g) | diff now %.3f prev1 %.3f prev2 %.3f\n", i, v.x, v.y, v.z,
                 c->vel()[i].x, c->vel()[i].y, c->vel()[i].z, dd(c->vel()), dd(velH1), dd(velH2));
          if (i < posH1.size() && i < posH2.size() && dtH1 > 0) {
            const V3 fd = (c->pos()[i] - posH1[i]) * (1.f / dtH1), fd2 = (posH1[i] - posH2[i]) * (1.f / dtH2);
            printf("      posdiff now-prev1 %.3f prev1-prev2 %.3f\n", std::sqrt((fd - v).len2()), std::sqrt((fd2 - v).len2()));
          }
        }
        const Cloth& ref = a ? *a : *c;
        lastDelta = i < ref.vel().size() ? ref.vel()[i] : v;
        if (vdOn && i < ref.vel().size()) vdSum += v - ref.vel()[i], ++vdN;
        if (a) {
          if (i < a->vel().size() && i < c->vel().size()) c->setVelocity(i, c->vel()[i] + (v - a->vel()[i]));
          a->setVelocity(i, v);
        } else {
          c->setVelocity(i, v);
        }
        break;
      }
      case kRecForceV: {
        const uint32_t i = r.u32();
        const V3 f = r.v3();
        const uint32_t mode = r.u32();
        const float fdt = r.f();
        both([&](Cloth& k) { k.addForceAtVertex(i, f, mode, fdt); });
        break;
      }
      case kRecForcePos: {
        const V3 p = r.v3();
        const bool hasDir = r.u8();
        const V3 d = r.v3();
        const float mag = r.f(), rad = r.f();
        const uint32_t mode = r.u32();
        const float dt = r.f();
        if (!quiet) {
          printf("hit at step %llu: pos (%.2f %.2f %.2f) dir (%.2f %.2f %.2f) |%g| r %g mode %u\n", (unsigned long long)step,
                 p.x, p.y, p.z, d.x, d.y, d.z, mag, rad, mode);
          line("before");
        }
        both([&](Cloth& k) { k.addForceAtPos(p, hasDir ? &d : nullptr, mag, rad, mode, dt); });
        watchUntil = (long)step + 90;
        break;
      }
      case kRecAttResp: { const float v = r.f(); both([&](Cloth& k) { k.setAttachmentResponse(v); }); break; }
      case kRecAttTear: { const float v = r.f(); both([&](Cloth& k) { k.setAttachmentTearFactor(v); }); break; }
      case kRecParams: {
        const ClothParams np = paramsFrom(r.vec<uint8_t>());
        if (tr) {
          const ClothParams& o = c->params();
          printf("      thickness %g->%g ext (%g %g %g) wind (%g %g %g) damping %g friction %g stretch %g bend %g iters %u gravity %d tear %d\n",
                 o.thickness, np.thickness, np.externalAcceleration.x, np.externalAcceleration.y,
                 np.externalAcceleration.z, np.windAcceleration.x, np.windAcceleration.y, np.windAcceleration.z,
                 np.damping, np.friction, np.stretching, np.bending, np.solverIterations, np.useGravity, np.useTearing);
        }
        if (a) a->params() = np;
        c->params() = np;
        if (ovSelf >= 0) c->params().selfContactDist = ovSelf;
        if (ovRefDt >= 0) c->params().refDt = ovRefDt;
        if (ovEdge >= 0) c->params().edgeShapeContact = ovEdge != 0;
        break;
      }
      case kRecStep: {
        float dt = r.f();
        skipRestoreVel = false;
        if (gvA >= 0 && (long)step >= gvA && (long)step <= gvB) gvDt += dt, ++gvSteps;
        simT += dt;
        lastDt = dt;
        if (probeZ > -1e29f && step % 200 == 0 && c) snapOld.swap(snapNew), snapOldT = snapNewT, snapNew = c->pos(), snapNewT = simT;
        const V3 g = r.v3();
        if (g.len2() > 0.f) fallUp = g * (-1.f / std::sqrt(g.len2()));
        if (vdA >= 0 && (long)step >= vdA && (long)step <= vdB) {
          if (vdN) printf("veldelta step %llu dt %.5f n %ld mean (%.4f %.4f %.4f) asleep %d\n", (unsigned long long)step, dt,
                          vdN, vdSum.x / vdN, vdSum.y / vdN, vdSum.z / vdN, c->isSleeping());
        }
        vdOn = vdA >= 0 && (long)step + 1 >= vdA && (long)step + 1 <= vdB;
        vdSum = V3(), vdN = 0;
        if (c->isSleeping()) {
          ++sleepSteps;
          if (attSinceStep) ++sleepWithAtt;
          if (!wasAsleep) ++sleepRuns;
        }
        wasAsleep = c->isSleeping();
        attSinceStep = 0;
        if (tr) velH2 = velH1, velH1 = c->vel(), posH2 = posH1, posH1 = c->pos(), dtH2 = dtH1, dtH1 = dt;
        if (clockDt > 0.f) {  // the recording's steps are frames here: step the fixed clock inside them
          clockAcc += dt;
          int n = (int)(clockAcc / clockDt);
          if (n > 5) n = 5, clockAcc = 0;
          else clockAcc -= n * clockDt;
          for (int k = 0; k < n; ++k) c->step(clockDt, g);
          if (a) a->step(dt, g);
          dt = clockDt;
        } else {
          const auto t0 = std::chrono::steady_clock::now();
          both([&](Cloth& k) { k.step(dt, g); });
          const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
          stepMs += ms, stepMax = std::max(stepMax, ms);
          if (ms > 8.0) printf("slow step %llu: %.1f ms, n %zu, splits %d\n", (unsigned long long)step, ms, c->pos().size(), c->splits());
        }
        ++step;
        if (fallOn && c) {
          const auto& x = c->pos();
          if (floorOf.size() > x.size()) floorOf.clear();  // a new cloth
          while (floorOf.size() < x.size()) {  // tear copies start with the floor of the vertex they came from (approx.)
            const size_t i = floorOf.size();
            const int o = c->origin(i);
            FloorHit f;
            if (o >= 0 && (size_t)o < i) f.at = floorOf[o].at, f.has = floorOf[o].has;
            floorOf.push_back(f);
          }
          for (size_t i = 0; i < x.size(); ++i) {
            FloorHit& f = floorOf[i];
            if (c->isRemoved(i)) {
              if (!f.removed) {
                f.removed = true;
                if (!f.has) {
                  ++removedNoFloor;
                  const ClothParams& q = c->params();
                  const V3& X = x[i];
                  const float lo[3] = {q.validLo[0], q.validLo[1], q.validLo[2]}, hi[3] = {q.validHi[0], q.validHi[1], q.validHi[2]};
                  const float xv[3] = {X.x, X.y, X.z};
                  char face[8] = "?";
                  for (int ax = 0; ax < 3; ++ax) {
                    if (xv[ax] < lo[ax]) snprintf(face, sizeof face, "%c-", "xyz"[ax]);
                    if (xv[ax] > hi[ax]) snprintf(face, sizeof face, "%c+", "xyz"[ax]);
                  }
                  const int o = c->origin(i);
                  const float drop = o >= 0 && (size_t)o < rest.size() ? (rest[o] - X).dot(fallUp) : 0.f;
                  removedFaces[std::string(face)]++;
                  if (removedFaces.size() < 8 && removedDetail++ < 6)
                    printf("fall: removed at step %llu: (%.2f %.2f %.2f) left the box %s (box %.2f..%.2f %.2f..%.2f %.2f..%.2f), %.2f m below its rest place, speed %.2f\n",
                           (unsigned long long)step, X.x, X.y, X.z, face, lo[0], hi[0], lo[1], hi[1], lo[2], hi[2], drop, std::sqrt(c->vel()[i].len2()));
                  dropSum += drop, dropMax = std::max(dropMax, drop);
                }
                else if ((x[i] - f.at).dot(fallUp) < 0.f) {
                  auto& e = fallRemoved[(long)step];
                  ++e.first, e.second = std::max(e.second, -(x[i] - f.at).dot(fallUp));
                  ++fallRemovedTotal;
                }
              }
              continue;
            }
            for (size_t k = 0; k < c->contactCount(i); ++k) {
              V3 n; float d; int sh;
              c->contactInfo(i, k, n, d, sh);
              if (sh < 0 && n.dot(fallUp) > 0.5f) f.at = x[i], f.has = true, f.flagged = false;
            }
            if (!f.has || f.flagged) continue;
            const V3 dlt = x[i] - f.at;
            const float down = -dlt.dot(fallUp);
            const V3 side = dlt + fallUp * down;
            if (down > 0.5f && side.len2() < 0.25f * 0.25f) {
              f.flagged = true;
              auto& e = fallSteps[(long)step];
              ++e.first, e.second = std::max(e.second, down);
              ++fallTotal;
            }
          }
        }
        if ((long)step >= flA && (long)step <= flB) fl.scan(*c, step);
        if (!quiet && (long)step <= watchUntil && ((long)step - watchUntil + 90 <= 10 || step % 10 == 0)) line("after ");
        if (!quiet && watchUntil < (long)step) {
          static uint64_t lastWarn = 0;
          Stats s = stats(*c, rest);
          if (s.stretched > 0 && step - lastWarn >= 60) {
            lastWarn = step;
            line("STRETCH");
          }
        }
        if (edgesOut && (long)step == edgesAt && c)
          if (FILE* fe = fopen(edgesOut, "w")) {
            for (const Edge& e : c->edgesDebug()) fprintf(fe, "%d %d %.6f %d %d\n", e.i, e.j, e.L0, (int)c->isPinned(e.i), (int)c->isPinned(e.j));
            for (size_t i = 0; i < c->removed().size(); ++i) if (c->removed()[i]) fprintf(fe, "removed %zu\n", i);
            fclose(fe);
          }
        if ((long)step == hookAt && c) {
          std::map<int, long> per;
          std::vector<std::pair<int, int>> wh;
          c->edgesThroughShapes(&per, &wh);
          for (auto& e : per) printf("hook-at %ld: shape %d (-1: static) %ld edges\n", hookAt, e.first, e.second);
          for (auto& e : per)
            if (e.first >= 0 && (size_t)e.first < c->shapesDebug().size()) {
              const StaticShape& q = c->shapesDebug()[e.first];
              printf("  shape %d: type %d (0 plane 1 sphere 2 box 3 capsule 4 mesh) half (%.3f %.3f %.3f) radius %.3f mesh tris %zu\n", e.first, (int)q.type,
                     q.half.x, q.half.y, q.half.z, q.d, q.mesh ? q.mesh->v.size() / 3 : (size_t)0);
              if (q.mesh && !q.mesh->v.empty()) {
                V3 lo = q.mesh->v[0], hi = q.mesh->v[0];
                for (const V3& v : q.mesh->v) lo = V3(std::min(lo.x, v.x), std::min(lo.y, v.y), std::min(lo.z, v.z)), hi = V3(std::max(hi.x, v.x), std::max(hi.y, v.y), std::max(hi.z, v.z));
                printf("    mesh box (%.3f %.3f %.3f) .. (%.3f %.3f %.3f)\n", lo.x, lo.y, lo.z, hi.x, hi.y, hi.z);
              }
            }
          for (auto& e : wh) {
            const V3 m = (c->pos()[e.first] + c->pos()[e.second]) * 0.5f;
            printf("  edge %d-%d mid (%.2f %.2f %.2f) len %.2f\n", e.first, e.second, m.x, m.y, m.z, std::sqrt((c->pos()[e.first] - c->pos()[e.second]).len2()));
            const auto& S = c->shapesDebug();
            for (size_t k = 0; k < S.size(); ++k) {
              const StaticShape& q = S[k];
              const V3 d = m - q.a;
              const float r = std::sqrt(q.half.len2()) + q.d + 0.5f;
              if (q.type != ShapeType::Mesh && q.type != ShapeType::Plane && d.len2() < r * r)
                printf("    shape %zu type %d centre (%.2f %.2f %.2f) half (%.3f %.3f %.3f) radius %.3f\n", k, (int)q.type, q.a.x, q.a.y, q.a.z, q.half.x, q.half.y, q.half.z, q.d);
            }
          }
        }
        if (hookEvery > 0 && step % (uint64_t)hookEvery == 0 && c) hookLog.emplace_back((long)step, c->edgesThroughShapes(nullptr));
        if (stretchEvery > 0 && step % (uint64_t)stretchEvery == 0 && c) {
          float m = 0.f, mf = 0.f, mb = 0.f;
          for (const Edge& e : c->edgesDebug()) {
            if (!(e.L0 > 0.f)) continue;
            const float q = std::sqrt((c->pos()[e.i] - c->pos()[e.j]).len2()) / e.L0;
            const int np = c->isPinned(e.i) + c->isPinned(e.j);
            m = std::max(m, q);
            if (np == 0) mf = std::max(mf, q);
            if (np == 2) mb = std::max(mb, q);
          }
          stretchLog.push_back({(long)step, lastDt, m, mf, mb});
        }
        if (isectEvery > 0 && step % (uint64_t)isectEvery == 0) isectLog.emplace_back((long)step, crossings(c->pos(), c->tri()));
        if (fout && (long)step >= fa && (long)step <= fb) {
          if (!fo) {
            fo = fopen(fout, "wb");
            const uint32_t hdr[6] = {0x4C435850u, 1, 0, (uint32_t)rest.size() * 4, (uint32_t)rest.size(),
                                     (uint32_t)(c->tri().size() / 3)};
            fwrite(hdr, 4, 6, fo);
            frameHdrPos = 8;
            const float dtf = dt;
            fwrite(&dtf, 4, 1, fo);
            fwrite(c->tri().data(), 4, c->tri().size(), fo);
            fwrite(rest.data(), 12, rest.size(), fo);
          }
          const uint32_t nv = (uint32_t)c->pos().size(), nt = (uint32_t)(c->tri().size() / 3);
          const double tm = 0;
          const uint32_t has = c->tri() != lastTri;
          lastTri = c->tri();
          fwrite(&nv, 4, 1, fo), fwrite(&nt, 4, 1, fo), fwrite(&tm, 8, 1, fo), fwrite(&has, 4, 1, fo);
          if (has) fwrite(c->tri().data(), 4, c->tri().size(), fo);
          for (uint32_t i = 0; i < nv; ++i) fwrite(&c->pos()[i], 12, 1, fo), fwrite(&c->vel()[i], 12, 1, fo);
          ++framesWritten;
        }
        break;
      }
      case kRecCheck: {
        const uint32_t h = r.u32();
        ++checks;
        if (h != recHash((a ? *a : *c).pos())) {
          ++mismatches;
          if (firstBad < 0) firstBad = (long)step;
        }
        break;
      }
      case kRecGameVel: {  // value for an exact replay; with --dual the change reaches c, the value `a`
        const uint32_t i = r.u32();
        const V3 v = r.v3(), d = r.v3();
        if (a) {
          if (i < c->vel().size()) c->setVelocity(i, c->vel()[i] + d);
          a->setVelocity(i, v);
        } else {
          if (gvA >= 0 && (long)step >= gvA && (long)step <= gvB && i < c->vel().size()) {
            const V3 u = c->vel()[i];
            const float uv[3] = {u.x, u.y, u.z}, dv[3] = {d.x, d.y, d.z};
            for (int k = 0; k < 3; ++k)
              gvS[k][0] += uv[k], gvS[k][1] += dv[k], gvS[k][2] += (double)uv[k] * uv[k], gvS[k][3] += (double)uv[k] * dv[k];
            gvS[3][0] += 1;
            gvLostZ += v.z - u.z, gvDz += d.z, gvCurZ += u.z;
          }
          c->setVelocity(i, v);
        }
        break;
      }
      case kRecAddVel: {
        const uint32_t i = r.u32();
        const V3 d = r.v3();
        if (i < c->vel().size()) c->setVelocity(i, c->vel()[i] + d);
        if (a && i < a->vel().size()) a->setVelocity(i, a->vel()[i] + d);
        break;
      }
      default:
        fprintf(stderr, "unknown op %u at %zu\n", op, r.o - 1);
        return 1;
    }
    if (deltaOut) {
      if (op == kRecSetVel) {
        uint32_t i;
        V3 v;
        memcpy(&i, &r.b[opStart + 1], 4), memcpy(&v, &r.b[opStart + 5], 12);
        const V3 d = v - lastDelta;  // lastDelta: the write's base on `a` (set in kRecSetVel)
        dbuf.push_back(kRecAddVel);
        const uint8_t* q = reinterpret_cast<const uint8_t*>(&i);
        dbuf.insert(dbuf.end(), q, q + 4);
        q = reinterpret_cast<const uint8_t*>(&d);
        dbuf.insert(dbuf.end(), q, q + 12);
      } else {
        dbuf.insert(dbuf.end(), r.b.begin() + opStart, r.b.begin() + r.o);
      }
    }
  }
  if (deltaOut) {
    FILE* f = fopen(deltaOut, "wb");
    if (!f || fwrite(dbuf.data(), 1, dbuf.size(), f) != dbuf.size()) { perror(deltaOut); return 1; }
    fclose(f);
  }
  if (probeZ > -1e29f && c) {
    const ClothParams& q = c->params();
    printf("probe params: wind (%.3f %.3f %.3f) ext (%.3f %.3f %.3f) damping %.3f friction %.3f thickness %.3f refDt %.4f sleepV %.3f\n",
           q.windAcceleration.x, q.windAcceleration.y, q.windAcceleration.z, q.externalAcceleration.x,
           q.externalAcceleration.y, q.externalAcceleration.z, q.damping, q.friction, q.thickness, q.refDt, q.sleepLinearVelocity);
  }
  if (probeZ > -1e29f && c) {  // visible pieces (connected alive triangles) at the end of the replay
    const auto& x = c->pos();
    const auto& v = c->vel();
    const auto& T = c->tri();
    std::vector<uint32_t> par(x.size());
    for (uint32_t i = 0; i < par.size(); ++i) par[i] = i;
    auto root = [&](uint32_t i) { while (par[i] != i) i = par[i] = par[par[i]]; return i; };
    std::vector<uint8_t> used(x.size(), 0);
    for (size_t t = 0; t + 2 < T.size(); t += 3) {
      if (c->triDead(t) || (T[t] == T[t + 1] && T[t + 1] == T[t + 2])) continue;
      for (int k = 0; k < 3; ++k) used[T[t + k]] = 1;
      par[root(T[t])] = root(T[t + 1]), par[root(T[t + 1])] = root(T[t + 2]);
    }
    {  // solver edges joining two visible pieces (or a piece and a vertex in no alive triangle)
      long cross = 0, crossPin = 0, toHidden = 0;
      float lmax = 0.f;
      for (const Edge& e : c->edgesDebug()) {
        if (!used[e.i] || !used[e.j]) { ++toHidden; continue; }
        if (root(e.i) == root(e.j)) continue;
        ++cross;
        crossPin += c->isPinned(e.i) || c->isPinned(e.j);
        lmax = std::max(lmax, std::sqrt((x[e.i] - x[e.j]).len2()) / std::max(e.L0, 1e-6f));
      }
      printf("probe edges: %zu, joining two pieces %ld (pinned end %ld, max stretch %.1fx), to a vertex in no triangle %ld\n",
             c->edgesDebug().size(), cross, crossPin, lmax, toHidden);
    }
    std::map<uint32_t, std::vector<uint32_t>> pieces;
    for (uint32_t i = 0; i < x.size(); ++i)
      if (used[i]) pieces[root(i)].push_back(i);
    for (auto& e : pieces) {
      const auto& ids = e.second;
      float zlo = 1e30f, zhi = -1e30f, sp = 0.f;
      int still = 0, pinned = 0, att = 0, inContact = 0, w0 = 0;
      float wsum = 0.f;
      V3 cn;
      for (uint32_t i : ids) {
        zlo = std::min(zlo, x[i].z), zhi = std::max(zhi, x[i].z);
        sp += std::sqrt(v[i].len2());
        w0 += c->invMassOf(i) == 0.f, wsum += c->invMassOf(i);
        still += v[i].len2() == 0.f, pinned += c->isPinned(i), att += c->isAttachedToShape(i);
        if (c->contactCount(i)) {
          ++inContact;
          float d; int sh; V3 n;
          c->contactInfo(i, 0, n, d, sh);
          cn += n;
        }
      }
      if (zhi < probeZ) continue;
      double sink = 0; int ns = 0;
      for (uint32_t i : ids) if (i < snapOld.size()) sink += snapOld[i].z - x[i].z, ++ns;
      const double sinkRate = ns && simT > snapOldT ? sink / ns / (simT - snapOldT) : 0.0;
      printf("probe piece: %zu verts, z %.2f..%.2f, speed mean %.3f, still %d, pinned %d, attached %d, in contact %d "
             "(mean normal %.2f %.2f %.2f), invMass 0: %d, mean %.3g, sinking %.3f m/s\n", ids.size(), zlo, zhi, sp / ids.size(), still, pinned, att, inContact,
             inContact ? cn.x / inContact : 0.f, inContact ? cn.y / inContact : 0.f, inContact ? cn.z / inContact : 0.f, w0,
             wsum / ids.size(), sinkRate);
    }
  }
  if (gvA >= 0 && gvS[3][0] > 0) {
    const double n = gvS[3][0];
    printf("gvstat: %.0f writes over %ld steps, mean dt %.5f; fit change = -k v + w per axis:", n, gvSteps, gvSteps ? gvDt / gvSteps : 0.0);
    for (int k = 0; k < 3; ++k) {
      const double mv = gvS[k][0] / n, md = gvS[k][1] / n, var = gvS[k][2] / n - mv * mv, cov = gvS[k][3] / n - mv * md;
      const double slope = var > 1e-12 ? cov / var : 0.0;
      printf(" [%c] k %.4f w %.4f", "xyz"[k], -slope, md - slope * mv);
    }
    printf("\ngvstat z: mean current %.4f, mean (value - current) %.4f, mean change %.4f\n", gvCurZ / n, gvLostZ / n, gvDz / n);
  }
  if (!hookLog.empty()) {
    long hooked = 0;
    for (auto& e : hookLog) hooked += e.second > 0;
    printf("hook: %ld of %zu samples with an edge through a shape; max per 1/25 of the run:", hooked, hookLog.size());
    for (size_t w = 0; w < 25; ++w) {
      long m = 0, at = -1;
      for (size_t k = w * hookLog.size() / 25; k < (w + 1) * hookLog.size() / 25; ++k)
        if (hookLog[k].second > m) m = hookLog[k].second, at = hookLog[k].first;
      printf(" %ld", m);
      if (m) printf("@%ld", at);
    }
    printf("\n");
  }
  if (!stretchLog.empty()) {  // 25 windows: step range, dt range, largest stretch
    printf("stretch: max %.2fx\n", std::max_element(stretchLog.begin(), stretchLog.end(), [](const StretchRec& a, const StretchRec& b) { return a.s < b.s; })->s);
    const size_t W = 25;
    for (size_t w = 0; w < W; ++w) {
      const size_t a = w * stretchLog.size() / W, b = (w + 1) * stretchLog.size() / W;
      if (a >= b) continue;
      float dlo = 1e9f, dhi = 0.f, sm = 0.f, sf = 0.f, sb = 0.f;
      for (size_t k = a; k < b; ++k)
        dlo = std::min(dlo, stretchLog[k].dt), dhi = std::max(dhi, stretchLog[k].dt), sm = std::max(sm, stretchLog[k].s),
        sf = std::max(sf, stretchLog[k].sFree), sb = std::max(sb, stretchLog[k].sBoth);
      printf("  steps %6ld..%6ld dt %.5f..%.5f stretch %.2fx (no pinned end %.2fx, both ends pinned %.2fx)\n", stretchLog[a].step,
             stretchLog[b - 1].step, dlo, dhi, sm, sf, sb);
    }
  }
  if (!isectLog.empty()) {  // 20 windows: the most crossings in each
    long mx = 0, at = 0;
    double sum = 0;
    for (auto& e : isectLog) { sum += e.second; if (e.second > mx) mx = e.second, at = e.first; }
    printf("isect: %zu samples, max %ld at step %ld, mean %.1f, last %ld; per twentieth:", isectLog.size(), mx, at,
           sum / isectLog.size(), isectLog.back().second);
    for (size_t w = 0; w < 20; ++w) {
      long m = 0;
      for (size_t k = w * isectLog.size() / 20; k < (w + 1) * isectLog.size() / 20; ++k) m = std::max(m, isectLog[k].second);
      printf(" %ld", m);
    }
    printf("\n");
  }
  if (fo) {
    fseek(fo, frameHdrPos, SEEK_SET);
    fwrite(&framesWritten, 4, 1, fo);
    fclose(fo);
  }
  if (flA >= 0) {
    printf("flicker: %ld front/back swaps over steps %ld..%ld, in %zu steps\n", fl.swaps, flA, flB, fl.perStep.size());
    long shown = 0;
    for (auto& e : fl.perStep) if (e.second >= 2 && shown++ < 15) printf("  step %llu: %ld swaps\n", (unsigned long long)e.first, e.second);
  }
  {
    static const char* nm[] = {"?", "create", "mesh", "addShape", "pin", "invMass", "motion", "detach", "attachShape",
                               "attachGlobal", "wake", "sleep", "setVel", "forceV", "forcePos", "attResp", "attTear",
                               "params", "step", "check", "note", "addVel", "gameVel"};
    printf("asleep at %ld steps (%ld of them after attachVertexToGlobalPosition calls), %ld sleep spells\n", sleepSteps, sleepWithAtt, sleepRuns);
    printf("solver: %.3f ms per step, max %.2f ms\n", step ? stepMs / step : 0.0, stepMax);
    if (fallOn) {
      printf("fall: %ld particles went > 0.5 m straight below their last floor contact, %ld removed below it, %ld removed "
             "without ever touching a floor\n", fallTotal, fallRemovedTotal, removedNoFloor);
      for (auto& e : removedFaces) printf("fall: removed without a floor through box side %s: %d\n", e.first.c_str(), e.second);
      if (removedNoFloor) printf("fall: those were on average %.2f m (max %.2f) below their rest place\n", dropSum / removedNoFloor, dropMax);
      int shown = 0;
      for (auto& e : fallSteps)
        if (shown++ < 25) printf("fall: step %ld: %d particles through the floor (deepest %.2f m)\n", e.first, e.second.first, e.second.second);
      shown = 0;
      for (auto& e : fallRemoved)
        if (shown++ < 25) printf("fall: step %ld: %d removed below their floor (deepest %.2f m)\n", e.first, e.second.first, e.second.second);
    }
    printf("ops:");
    for (int k = 1; k < 23; ++k) if (opCount[k]) printf(" %s %ld", nm[k], opCount[k]);
    printf("\n");
  }
  printf("%s: %llu steps, %d creates, hash checks %d, mismatches %d (first at step %ld)%s\n", argv[1],
         (unsigned long long)step, creates, checks, mismatches, firstBad, r.ok ? "" : " [truncated]");
  return 0;
}
