// Cloth::saveState / loadState: the whole solver state as tagged fields, for test cases cut out of game recordings
// (cloth_replay --cut). visitState lists every member once; the saver and the loader walk the same list.
//
// Layout: "PXCS", u32 version, u32 field count, then per field: u8 name length, name, u32 payload bytes, payload.
// Payloads: a value as its bytes; a vector as u32 element size, then the elements; a vector of vectors as u32 element
// size, u32 count, then per inner vector u32 count and its elements; size_t values and vectors as u64 (32- and 64-bit
// builds agree). 8-byte struct members are alignas(8), so i686 Clang/GCC and MSVC lay the structs out alike. A field
// whose element size differs from this build's (a struct that changed) is an error, not a guess.
// Not saved: per-call scratch (collideScratch_, contactScratch_, scChunks_), the thread hook, and what selfContactFind
// and untangleFind rebuild before reading it: their grids (sc*/scE*/ut* boxes, buckets, planes, stamps) and the vertex
// neighbours (scAdj*, scEdges_: rebuilt because scAdjTris_ starts empty, so it differs from tri_). scPrevTris_ (a copy
// of tri_ from the step that made scPrev_, which is forgotten when the triangles changed) is saved as whether it equals
// tri_.
#include <cstring>
#include <map>
#include <string>
#include <type_traits>
#include <vector>

#include "cloth.h"

namespace pxc {

namespace {
constexpr uint32_t kMagic = 0x53435850u;  // "PXCS"
constexpr uint32_t kVersion = 1;

struct Saver {
  static constexpr bool kLoading = false;
  std::vector<uint8_t> b;
  uint32_t count = 0;
  uint32_t (*meshId)(const TriMesh*, void*) = nullptr;
  void* ctx = nullptr;
  void raw(const void* p, size_t n) {
    const uint8_t* q = static_cast<const uint8_t*>(p);
    b.insert(b.end(), q, q + n);
  }
  void u32(uint32_t v) { raw(&v, 4); }
  size_t begin(const char* name) {
    const uint8_t n = (uint8_t)strlen(name);
    raw(&n, 1), raw(name, n);
    u32(0);
    ++count;
    return b.size();
  }
  void end(size_t at) {
    const uint32_t n = (uint32_t)(b.size() - at);
    memcpy(&b[at - 4], &n, 4);
  }
  template <class T> void field(const char* name, const T& v) {
    static_assert(std::is_trivially_copyable<T>::value, "plain value");
    const size_t at = begin(name);
    raw(&v, sizeof v);
    end(at);
  }
  void field(const char* name, const size_t& v) {
    const uint64_t w = v;
    const size_t at = begin(name);
    raw(&w, 8);
    end(at);
  }
  template <class T> void field(const char* name, const std::vector<T>& v) {
    static_assert(std::is_trivially_copyable<T>::value, "plain elements");
    const size_t at = begin(name);
    u32((uint32_t)sizeof(T));
    if (!v.empty()) raw(v.data(), v.size() * sizeof(T));
    end(at);
  }
  void field(const char* name, const std::vector<size_t>& v) {
    const size_t at = begin(name);
    u32(8);
    for (size_t x : v) {
      const uint64_t w = x;
      raw(&w, 8);
    }
    end(at);
  }
  template <class A, class B> void field(const char* name, const std::vector<std::pair<A, B>>& v) {
    static_assert(std::is_trivially_copyable<A>::value && std::is_trivially_copyable<B>::value, "plain pair");
    const size_t at = begin(name);
    u32((uint32_t)(sizeof(A) + sizeof(B)));
    for (const auto& e : v) raw(&e.first, sizeof(A)), raw(&e.second, sizeof(B));
    end(at);
  }
  template <class T> void field(const char* name, const std::vector<std::vector<T>>& v) {
    static_assert(std::is_trivially_copyable<T>::value, "plain elements");
    const size_t at = begin(name);
    u32((uint32_t)sizeof(T));
    u32((uint32_t)v.size());
    for (const auto& e : v) {
      u32((uint32_t)e.size());
      if (!e.empty()) raw(e.data(), e.size() * sizeof(T));
    }
    end(at);
  }
  void field(const char* name, const std::vector<StaticShape>& v) {  // the mesh pointer as its id
    const size_t at = begin(name);
    u32(4 + 12 + 4 + 12 + 4);
    for (const StaticShape& s : v) {
      u32((uint32_t)s.type);
      raw(&s.a, 12), raw(&s.d, 4), raw(&s.half, 12);
      u32(s.mesh ? meshId(s.mesh, ctx) : 0xffffffffu);
    }
    end(at);
  }
};

struct Loader {
  static constexpr bool kLoading = true;
  std::map<std::string, std::pair<const uint8_t*, uint32_t>> f;
  std::vector<std::string>* missing = nullptr;
  std::string error;
  const TriMesh* (*meshOf)(uint32_t, void*) = nullptr;
  void* ctx = nullptr;
  bool get(const char* name, const uint8_t*& p, uint32_t& n) {
    auto it = f.find(name);
    if (it == f.end()) {
      if (missing) missing->push_back(name);
      return false;
    }
    p = it->second.first, n = it->second.second;
    return true;
  }
  void fail(const char* name, const char* what) {
    if (error.empty()) error = std::string(name) + ": " + what;
  }
  // a vector payload: element size, then n elements; returns the element bytes or null
  const uint8_t* vec(const char* name, uint32_t elem, uint32_t& count) {
    const uint8_t* p;
    uint32_t n;
    if (!get(name, p, n)) return nullptr;
    uint32_t es;
    if (n < 4) return fail(name, "short"), nullptr;
    memcpy(&es, p, 4);
    if (es != elem || (n - 4) % elem) return fail(name, "element size differs from this build"), nullptr;
    count = (n - 4) / elem;
    return p + 4;
  }
  template <class T> void field(const char* name, T& v) {
    const uint8_t* p;
    uint32_t n;
    if (!get(name, p, n)) return;
    if (n != sizeof v) return fail(name, "size differs from this build");
    memcpy(&v, p, sizeof v);
  }
  void field(const char* name, size_t& v) {
    const uint8_t* p;
    uint32_t n;
    if (!get(name, p, n)) return;
    if (n != 8) return fail(name, "size");
    uint64_t w;
    memcpy(&w, p, 8);
    v = (size_t)w;
  }
  template <class T> void field(const char* name, std::vector<T>& v) {
    uint32_t c = 0;
    const uint8_t* p = vec(name, sizeof(T), c);
    if (!p) return;
    v.resize(c);
    if (c) memcpy(v.data(), p, (size_t)c * sizeof(T));
  }
  void field(const char* name, std::vector<size_t>& v) {
    uint32_t c = 0;
    const uint8_t* p = vec(name, 8, c);
    if (!p) return;
    v.resize(c);
    for (uint32_t i = 0; i < c; ++i) {
      uint64_t w;
      memcpy(&w, p + 8 * (size_t)i, 8);
      v[i] = (size_t)w;
    }
  }
  template <class A, class B> void field(const char* name, std::vector<std::pair<A, B>>& v) {
    uint32_t c = 0;
    const uint8_t* p = vec(name, sizeof(A) + sizeof(B), c);
    if (!p) return;
    v.resize(c);
    for (uint32_t i = 0; i < c; ++i, p += sizeof(A) + sizeof(B)) memcpy(&v[i].first, p, sizeof(A)), memcpy(&v[i].second, p + sizeof(A), sizeof(B));
  }
  template <class T> void field(const char* name, std::vector<std::vector<T>>& v) {
    const uint8_t* p;
    uint32_t n;
    if (!get(name, p, n)) return;
    const uint8_t* e = p + n;
    uint32_t es, c;
    if (n < 8) return fail(name, "short");
    memcpy(&es, p, 4), memcpy(&c, p + 4, 4);
    if (es != sizeof(T)) return fail(name, "element size differs from this build");
    p += 8;
    v.assign(c, {});
    for (uint32_t i = 0; i < c; ++i) {
      uint32_t k;
      if (e - p < 4) return fail(name, "short");
      memcpy(&k, p, 4);
      p += 4;
      if ((size_t)(e - p) < (size_t)k * sizeof(T)) return fail(name, "short");
      v[i].resize(k);
      if (k) memcpy(v[i].data(), p, (size_t)k * sizeof(T));
      p += (size_t)k * sizeof(T);
    }
  }
  void field(const char* name, std::vector<StaticShape>& v) {
    uint32_t c = 0;
    const uint8_t* p = vec(name, 36, c);
    if (!p) return;
    v.assign(c, StaticShape{ShapeType::Plane, V3(), 0, V3()});
    for (uint32_t i = 0; i < c; ++i, p += 36) {
      uint32_t t, id;
      memcpy(&t, p, 4), memcpy(&v[i].a, p + 4, 12), memcpy(&v[i].d, p + 16, 4), memcpy(&v[i].half, p + 20, 12);
      memcpy(&id, p + 32, 4);
      v[i].type = (ShapeType)t;
      if (id != 0xffffffffu) {
        v[i].mesh = meshOf(id, ctx);
        if (!v[i].mesh) fail(name, "unknown mesh id");
      }
    }
  }
};
}  // namespace

#define F(m) a.field(#m, m)
template <class A> void Cloth::visitState(A& a) {
  static_assert(sizeof(PrevPair) == 16 && sizeof(PrevEdge) == 32, "same layout in every build");
  F(P_); F(tri_); F(x_); F(v_); F(p_); F(pinPos_); F(rest_); F(orig_); F(splits_); F(w_); F(mass_); F(edgeMedian_);
  F(hard_); F(tip_); F(removed_); F(anyRemoved_); F(removedDirty_); F(removedChecked_); F(hitPending_);
  F(trackParents_); F(parent_); F(removedCount_); F(edges_); F(bounds_); F(edgesRebuilt_); F(deferEdgeUpkeep_);
  F(sedges_); F(levelStart_); F(shapes_); F(contacts_); F(wakeCounter_); F(asleep_); F(friction_);
  // attachments and shape motion
  F(att_); F(pose_); F(shapeInvMass_); F(motion_); F(anyAttachTear_); F(hasMesh_);
  // shape broadphase (rebuilt when the shapes change; kept so a loaded cloth takes the same path)
  F(shapeTree_); F(shapeNodes_); F(shapeOrder_); F(shapeIdx_); F(shapeAlways_); F(shapeLo_); F(shapeHi_); F(swept4_);
  F(sweptAt_);
  // self contact, untangling, edge-edge pairs: remembered sides and counters (the grids are not state, see the top)
  F(sc_); F(ut_); F(utReg_); F(utCap_); F(utIdle_);
  F(utEvery_); F(utBest_); F(utLast_); F(utBestLen_); F(se_); F(scPrevE_); F(scPrevEdges_); F(scPrev_);
  F(scPrevOld_);
  uint8_t prevTrisCurrent = scPrevTris_ == tri_;
  a.field("scPrevTrisCurrent", prevTrisCurrent);
  if (A::kLoading) scPrevTris_ = prevTrisCurrent ? tri_ : std::vector<uint32_t>();
  // per-step scratch that is reused across steps
  F(nrm_); F(depen_); F(selfBin_); F(selfStart_); F(selfFill_); F(selfOrder_); F(restSupported_); F(eh_); F(ehNarrow_);
  F(ehBox_);
}
#undef F

std::vector<uint8_t> Cloth::saveState(uint32_t (*meshId)(const TriMesh*, void*), void* ctx) const {
  Saver s;
  s.meshId = meshId, s.ctx = ctx;
  s.u32(kMagic), s.u32(kVersion), s.u32(0);
  const_cast<Cloth*>(this)->visitState(s);  // the saver only reads
  memcpy(&s.b[8], &s.count, 4);
  return s.b;
}

Cloth* Cloth::loadState(const std::vector<uint8_t>& b, const TriMesh* (*meshOf)(uint32_t, void*), void* ctx,
                        std::vector<std::string>* missing, std::string* error) {
  auto bad = [&](const char* why) -> Cloth* {
    if (error) *error = why;
    return nullptr;
  };
  Loader l;
  l.meshOf = meshOf, l.ctx = ctx, l.missing = missing;
  uint32_t magic = 0, ver = 0, n = 0;
  if (b.size() < 12) return bad("short state");
  memcpy(&magic, &b[0], 4), memcpy(&ver, &b[4], 4), memcpy(&n, &b[8], 4);
  if (magic != kMagic || ver != kVersion) return bad("not a PXCS v1 state");
  size_t o = 12;
  for (uint32_t i = 0; i < n; ++i) {
    if (o + 1 > b.size()) return bad("truncated");
    const uint8_t len = b[o++];
    if (o + len + 4 > b.size()) return bad("truncated");
    std::string name(reinterpret_cast<const char*>(&b[o]), len);
    o += len;
    uint32_t sz;
    memcpy(&sz, &b[o], 4);
    o += 4;
    if (o + sz > b.size()) return bad("truncated");
    l.f[name] = {b.data() + o, sz};
    o += sz;
  }
  Cloth* c = new Cloth();
  c->visitState(l);
  if (!l.error.empty()) {
    delete c;
    if (error) *error = l.error;
    return nullptr;
  }
  c->contacts_.resize(c->x_.size());
  return c;
}

}  // namespace pxc
