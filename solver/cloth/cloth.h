// Our NxCloth-compatible position-based cloth solver, v0 (behaviour described in docs/re-notes-cloth.md).
// Plain C++17, no dependencies. Static colliders, plus moving shapes through setShapeMotion (cloth.cpp).
#pragma once

#include <cfloat>
#include <algorithm>
#include <cstdint>
#include <utility>
#include <unordered_map>
#include <vector>
#include <map>

namespace pxc {

struct V3 {
  float x = 0, y = 0, z = 0;
  V3() = default;
  V3(float a, float b, float c) : x(a), y(b), z(c) {}
  V3 operator+(const V3& o) const { return {x + o.x, y + o.y, z + o.z}; }
  V3 operator-(const V3& o) const { return {x - o.x, y - o.y, z - o.z}; }
  V3 operator*(float s) const { return {x * s, y * s, z * s}; }
  V3 operator-() const { return {-x, -y, -z}; }
  V3& operator+=(const V3& o) { x += o.x; y += o.y; z += o.z; return *this; }
  V3& operator-=(const V3& o) { x -= o.x; y -= o.y; z -= o.z; return *this; }
  float dot(const V3& o) const { return x * o.x + y * o.y + z * o.z; }
  V3 cross(const V3& o) const { return {y * o.z - z * o.y, z * o.x - x * o.z, x * o.y - y * o.x}; }
  float len2() const { return dot(*this); }
};

struct ClothParams {
  float thickness = 0.01f, density = 1.0f;
  float bending = 1.0f, stretching = 1.0f, damping = 0.5f, friction = 0.5f;
  uint32_t solverIterations = 5;
  float sleepLinearVelocity = 0.15f, wakeUpCounter = 0.4f;  // defaults [A]: see generateContacts/step notes
  V3 externalAcceleration, windAcceleration;
  bool useGravity = true, useBending = false, bendingOrtho = false, useDamping = false, useComDamping = false;
  bool useTearing = false;  // NX_CLF_TEARABLE (needs a tearable mesh; vertices copy on tear)
  bool useSelfCollision = false;  // NX_CLF_SELFCOLLISION (0x8): particle-particle prepass, see cloth_self.cpp
  float tearFactor = 1.5f;  // edge tears when |x_i - x_j| >= tearFactor * rest length
  uint32_t maxVertices = 0; // tearing cap (NxMeshData::maxVertices): a split that would exceed it is skipped; 0 = none
  float attachmentResponse = 0.2f;   // impulse factor for two-way attachments (NxClothDesc default)
  float attachmentTearFactor = 1.5f; // tearable attachment: tears when edge length > factor * rest length
  bool disableDynamicCcd = false;    // NX_CLF_DISABLE_DYNAMIC_CCD (0x20000): moving shapes at their current pose only
  // NX_CLF_DISABLE_COLLISION (0x4): no shape contacts (the original skips GenerateContacts and the collision-plane
  // projection); the shapes stay for attachments.
  bool disableCollision = false;
  // NX_CLF_VALIDBOUNDS (0x8000): after each step a particle strictly outside [validLo, validHi] (world space) is removed for
  // good (cloth.cpp, "Valid bounds"). The default box is empty, so the flag without setValidBounds removes everything.
  bool useValidBounds = false;
  float validLo[3] = {FLT_MAX, FLT_MAX, FLT_MAX}, validHi[3] = {-FLT_MAX, -FLT_MAX, -FLT_MAX};
  // Step-rate independence (not in the original, whose per-step coefficients make the cloth stiffer and more damped the
  // more steps run per second). refDt > 0: the coefficients above mean what they do in steps of refDt, and a step of
  // another dt rescales them (see Cloth::step). 0 = off, every step uses them as given (the original's behaviour).
  float refDt = 0.f;
  // Particle-triangle self contact (not in the original, cloth_selftri.cpp): > 0 keeps every vertex at least this far
  // from the cloth's other triangles, on the side it started on. 0 = off (the original's behaviour).
  float selfContactDist = 0.f;
  // Edge-shape contact (not in the original, cloth_edgeshape.cpp): edges whose two particles touch a sphere, capsule or
  // box keep their closest point a thickness from it, on the side they started on. Particle-only contact let a bar
  // thinner than the particle spacing sit between two particles and hold the cloth. false = off (the original's).
  bool edgeShapeContact = false;
};

// Triangle mesh in its shape's frame (a static mesh: world space), for ShapeType::Mesh (cloth_mesh.cpp). Front face =
// (v1 - v0) x (v2 - v0). Triangle meshes, convexes and heightfields of the scene all come as one.
struct TriMesh {
  std::vector<V3> v;  // 3 per triangle
  struct Node {       // AABB tree: leaf count > 0 (tris order[first..first+count)); inner: left = next, right = first
    V3 lo, hi;
    uint32_t first = 0, count = 0;
  };
  std::vector<Node> nodes;
  std::vector<uint32_t> order;
  void build();  // after v is filled
  size_t triCount() const { return v.size() / 3; }
  // Calls f(triangle index) for every triangle whose box overlaps [lo, hi]. Thread-safe (const).
  template <class F>
  void query(const V3& lo, const V3& hi, F&& f) const;
};

// Calls f(item) for every item of a box tree (buildBoxTree) whose box overlaps [lo, hi].
template <class F>
void queryBoxTree(const std::vector<TriMesh::Node>& nodes, const std::vector<uint32_t>& order, const V3& lo,
                  const V3& hi, F&& f) {
  if (nodes.empty()) return;
  uint32_t stack[64];
  int sp = 0;
  stack[sp++] = 0;
  while (sp > 0) {
    const uint32_t k = stack[--sp];
    const TriMesh::Node& n = nodes[k];
    if (n.hi.x < lo.x || n.lo.x > hi.x || n.hi.y < lo.y || n.lo.y > hi.y || n.hi.z < lo.z || n.lo.z > hi.z) continue;
    if (n.count) {
      for (uint32_t i = 0; i < n.count; ++i) f(order[n.first + i]);
    } else if (sp + 2 <= 64) {
      stack[sp++] = n.first;
      stack[sp++] = k + 1;
    }
  }
}
template <class F>
void TriMesh::query(const V3& lo, const V3& hi, F&& f) const {
  queryBoxTree(nodes, order, lo, hi, f);
}

// Median-split AABB tree over boxes [lo[i], hi[i]] (TriMesh::Node layout; query as TriMesh::query). cloth_mesh.cpp.
void buildBoxTree(const std::vector<V3>& lo, const std::vector<V3>& hi, std::vector<TriMesh::Node>& nodes,
                  std::vector<uint32_t>& order);

// Closest point of triangle abc to p (Ericson, Real-Time Collision Detection 5.1.5).
inline V3 closestPointOnTriangle(const V3& p, const V3& a, const V3& b, const V3& c) {
  const V3 ab = b - a, ac = c - a, ap = p - a;
  const float d1 = ab.dot(ap), d2 = ac.dot(ap);
  if (d1 <= 0.f && d2 <= 0.f) return a;
  const V3 bp = p - b;
  const float d3 = ab.dot(bp), d4 = ac.dot(bp);
  if (d3 >= 0.f && d4 <= d3) return b;
  const float vc = d1 * d4 - d3 * d2;
  if (vc <= 0.f && d1 >= 0.f && d3 <= 0.f) return a + ab * (d1 / (d1 - d3));
  const V3 cp = p - c;
  const float d5 = ab.dot(cp), d6 = ac.dot(cp);
  if (d6 >= 0.f && d5 <= d6) return c;
  const float vb = d5 * d2 - d1 * d6;
  if (vb <= 0.f && d2 >= 0.f && d6 <= 0.f) return a + ac * (d2 / (d2 - d6));
  const float va = d3 * d6 - d5 * d4;
  if (va <= 0.f && (d4 - d3) >= 0.f && (d5 - d6) >= 0.f) return b + (c - b) * ((d4 - d3) / ((d4 - d3) + (d5 - d6)));
  const float den = 1.f / (va + vb + vc);
  return a + ab * (vb * den) + ac * (vc * den);
}

enum class ShapeType { Plane, Sphere, Box, Capsule, Mesh };
struct StaticShape {
  ShapeType type;
  V3 a;       // plane normal | sphere/box/capsule centre
  float d = 0;  // plane offset | sphere radius | capsule radius
  V3 half;    // box half extents | capsule.y = half of the straight segment (axis = local y)
  const TriMesh* mesh = nullptr;  // Mesh: triangles in the shape frame (owned by the caller, outlives the cloth)
};

// Attachments (solver/cloth/cloth_attach.cpp). Shape ids are the creation indices of addShape().
struct ShapePose {  // world = t + R * local, R row-major (r[3*row + col]); set = pose given by setShapePose
  V3 t;
  bool set = false;
  float r[9] = {1, 0, 0, 0, 1, 0, 0, 0, 1};
};
struct AttachInfo {  // per vertex; shape < 0 = not attached to a shape (global pins live in hard_/pinPos_)
  int shape = -1;
  V3 local;        // attachment position in the shape frame
  V3 target;       // world attachment position used in the last projection
  V3 impulse;      // two-way: impulse transferred to the body in the last projection (before the response factor)
  bool tearable = false, twoWay = false;
};
struct AttachReaction {  // impulse for the shape's body (apply at point with NX_IMPULSE)
  int shape;
  V3 point, impulse;
};

struct Edge {
  int i, j;
  int k, l;        // opposite vertices (l = -1 on border)
  float L0 = 0;    // rest length
  float kl0 = 0;   // rest distance k-l
  float phi0 = 0;  // rest dihedral angle (paper convention)
};

// Contents of a cooked cloth stream (NXS/CLTH chunk written by the 2.8 cooker; layout in docs/re-notes-cloth.md).
struct CookedCloth {
  std::vector<V3> pos;        // rest positions, original order
  std::vector<uint32_t> tri;  // triangle indices, original order
  std::vector<Edge> edges;    // stream order; i,j,k,l in original numbering (l = -1 on border); L0/kl0/phi0 as stored
};
// Returns false (and leaves out unspecified) for anything other than the layout described in the notes.
bool parseCookedCloth(const std::vector<uint8_t>& bytes, CookedCloth& out);

class Cloth {
 public:
  // rest = cooked rest positions; tri = triangle indices; x0 = initial positions (may differ from rest).
  // stored: edges in cooked-stream order (topology only unless storedRest, then L0/kl0/phi0 too). Without it the edges
  // are taken in first-appearance order from tri [A fallback].
  Cloth(const ClothParams& p, const std::vector<V3>& rest, const std::vector<uint32_t>& tri, const std::vector<V3>& x0,
        const std::vector<Edge>* stored = nullptr, bool storedRest = false);
  struct ContactSlot;  // one particle's contact search result (cloth.cpp)
  void addShape(const StaticShape& s) {
    shapes_.push_back(s);
    hasMesh_ = hasMesh_ || s.type == ShapeType::Mesh;
  }
  void pinHard(int vertex, const V3& worldPos) { hard_[vertex] = 1; pinPos_[vertex] = worldPos; w_[vertex] = 0; }
  // attachments (cloth_attach.cpp); flags: NX_CLOTH_ATTACHMENT_TWOWAY = 1, _TEARABLE = 2
  void setShapePose(int shape, const V3& t, const float rot[9]);  // shape pose for this step (default identity)
  void setShapeInvMass(int shape, float invMass);                 // body inverse mass for two-way reactions (0 static)
  // Moving shape for contacts (cloth.cpp): current pose t/R and, when given, the pose at the start of this step
  // (prevT/prevR; without it the shape is tested at its current pose only, as on the first step). Also sets the
  // attachment pose (setShapePose). Shapes without this call keep the static geometry from addShape.
  // dynamicBody: the body is dynamic, so the swept test runs from prevT/prevR (dynamic CCD); kinematic shapes are tested
  // at their current pose only (oracle: dyn_capsule_sweep vs static capsule at the same pose).
  void setShapeMotion(int shape, const V3& t, const float R[9], const V3* prevT = nullptr, const float* prevR = nullptr,
                      bool dynamicBody = false);
  // Surface velocity of a moving shape's body for contact friction (linear, angular, world centre of mass); zero (the
  // default) for statics and parked shapes. The original's friction works on v - v_s (docs/re-notes-cloth.md).
  void setShapeVelocity(int shape, const V3& lin, const V3& ang, const V3& com);
  void attachToShape(int shape, uint32_t flags);                  // vertices inside or touching the shape now
  void attachToCollidingShapes(uint32_t flags);                   // first shape (addShape order) that touches each
  void detachFromShape(int shape);
  void attachVertexToShape(int vertex, int shape, const V3& localPos, uint32_t flags);  // refused if shape-attached
  void attachVertexToGlobalPosition(int vertex, const V3& worldPos);  // pin; refused if shape-attached
  void attachVertexToInitialPosition(int vertex);                 // NX_CLOTH_VERTEX_ATTACHED (rest position)
  void freeVertex(int vertex);
  // Live parameter changes: every field is read per step except density (mass) and the use*/flag booleans that shape
  // the edge set (useBending, bendingOrtho, useTearing); change those through a rebuild.
  ClothParams& params() { return P_; }
  // Work inside one step split over a worker pool (the shim's): fn(ctx, i) for i < n, any order, all done on return.
  using ParallelFor = void (*)(size_t n, void (*fn)(void* ctx, size_t i), void* ctx);
  void setParallelFor(ParallelFor pf) { parallelFor_ = pf; }
  void wakeUp(float counter) { asleep_ = false; wakeCounter_ = counter; }
  // The attach and free calls wake the cloth with the desc wakeUpCounter, as the 2.8.0 core does (its wakeUp, Cloth slot
  // 75, called from slots 86, 87, 89, 90, 91 and detachFromShape). UE3 re-pins its fixed vertices every frame, so the
  // original never lets such a cloth sleep.
  void wakeFromCall() { wakeUp(P_.wakeUpCounter); }
  void putToSleep() { asleep_ = true; for (V3& v : v_) v = V3(); }
  // NxCloth::setVelocity(ies): writes the velocity only; a sleeping cloth stays asleep (2.8.0 Cloth slots 58 and 60,
  // 0x100dd520 / 0x100dd5d0: no wake-up). Waking here let the game's per-frame velocity writes (UE3, after unfreezing a
  // cloth) wake cloths the original keeps asleep, and they fell.
  void setVelocity(size_t i, const V3& v) { if (i < v_.size()) v_[i] = v; }
  // External forces (NxCloth::addForceAtVertex / addForceAtPos / addDirectedForceAtPos), applied to the velocities at
  // the call and waking the cloth. mode = NxForceMode: 0 force, 1 impulse, 2 velocity change, 5 acceleration; the
  // smooth modes (3, 4) do nothing, as in the original. dt = the scene step the force acts over (force, acceleration).
  // AtPos: particles within radius get weight 1 - d^2/r^2; force and impulse are divided by their summed mass.
  // dir = null: radial (away from p, magnitude mag); otherwise along *dir with mag = |*dir|. (cloth.cpp)
  void addForceAtVertex(size_t i, const V3& f, uint32_t mode, float dt);
  void addForceAtPos(const V3& p, const V3* dir, float mag, float radius, uint32_t mode, float dt);
  void setAttachmentResponse(float c) { P_.attachmentResponse = c; }
  void setAttachmentTearFactor(float f) { P_.attachmentTearFactor = f; }
  bool isAttachedToShape(size_t v) const { return v < att_.size() && att_[v].shape >= 0; }
  std::vector<AttachReaction> attachReactions() const;  // two-way vertices, after the last step
  void step(float dt, V3 gravity);
  const std::vector<V3>& pos() const { return x_; }
  const std::vector<V3>& vel() const { return v_; }
  bool isSleeping() const { return asleep_; }
  // Sleep speed in use: a negative sleepLinearVelocity means the SDK default (desc +0xa0, re-notes-cloth.md). The game sets
  // -1 on live cloths (setSleepLinearVelocity); squared as given it became a 1 m/s threshold, and the self-contact rest
  // rule stopped every folded torn piece slower than that in mid-air (game run 20261012-180216, cloth002).
  float sleepVelocity() const { return P_.sleepLinearVelocity >= 0.f ? P_.sleepLinearVelocity : kDefaultSleepVelocity; }
  static constexpr float kDefaultSleepVelocity = 0.15f;  // [A] as the shim's create path (cloth_ours.cpp)
  // Sleep state as a whole (replays that emulate an older build: cloth_replay --dual).
  float wakeCounter() const { return wakeCounter_; }
  void setSleepState(bool asleep, float counter) { asleep_ = asleep, wakeCounter_ = counter; }
  const std::vector<uint32_t>& tri() const { return tri_; }
  // Removed particles (NX_CLF_VALIDBOUNDS; sticky): one byte per particle, 1 = removed. A removed particle stays where it
  // is and takes part in nothing; triangles with a removed corner are dead (the shim collapses them in the output indices).
  // setRemoved restores a mask (after a rebuild); the edges follow at the next step. tri() stays whole (tearing needs it).
  const std::vector<uint8_t>& removed() const { return removed_; }
  size_t removedCount() const { return removedCount_; }
  bool isRemoved(size_t i) const { return i < removed_.size() && removed_[i]; }
  void setRemoved(const std::vector<uint8_t>& m);
  bool triDead(size_t t) const {  // t = first index of the triangle in tri()
    return anyRemoved_ && (removed_[tri_[t]] || removed_[tri_[t + 1]] || removed_[tri_[t + 2]]);
  }
  // Debug (cloth_replay --probe): particle i's pin, shape attachment and this step's contacts (normal, distance, shape).
  const std::vector<Edge>& edgesDebug() const { return edges_; }
  // Debug (cloth_replay --hook): edges that pass through a shape with both particles outside it (the cloth hangs on a
  // shape thinner than its particle spacing). Uses this step's shape bounds; counts per shape go to perShape.
  long edgesThroughShapes(std::map<int, long>* perShape, std::vector<std::pair<int, int>>* which = nullptr) const;
  const std::vector<StaticShape>& shapesDebug() const { return shapes_; }
  float invMassOf(size_t i) const { return i < w_.size() ? w_[i] : -1.f; }
  bool isPinned(size_t i) const { return i < hard_.size() && hard_[i]; }
  size_t contactCount(size_t i) const { return i < contacts_.size() ? contacts_[i].size() : 0; }
  void contactInfo(size_t i, size_t k, V3& n, float& dist, int& shape) const {
    const auto& c = contacts_[i][k];
    n = c.n, dist = c.dist, shape = c.shape;
  }
  int splits() const { return splits_; }  // total vertex copies made by tearing
  int origin(size_t i) const { return i < orig_.size() ? orig_[i] : (int)i; }  // rest vertex a particle came from (copies: the torn one)

 private:
  struct Contact { V3 n; float dist; int shape = -1; };  // shape: moving shape of the contact (-1: static)  // one per particle and step (see generateContacts)
  void generateContacts();
  struct CollideScratch { std::vector<uint32_t> cand; std::vector<uint8_t> skip; };
  void collide(const V3& x, const V3& pe, float r, bool meshesOnly, ContactSlot& s, CollideScratch* cs = nullptr) const;
  void solveEdges(float sStretch, float sBend);
  void bendDihedral(const Edge& e, float s);
  void comDamping(float keep);  // rigid-motion (COM) velocity damping, before p* = x + v dt
  void validBoundsStep();               // bounds test of the particles, before tearStep
  void removedFixup(bool force);        // edges from the alive triangles; vertices left in none are removed too
  // tears stretched edges after the position update; returns true if topology changed. *purged: a full rebuild in the
  // pass took this step's removed particles out of the edges
  bool tearStep(bool* purged = nullptr);
  void rebuildEdges();
  void edgeRest(Edge& e) const;
  // Edge pair (min << 32 | max) -> edge slot, open addressing. splitEdges refills it after most splits (a dropped slot
  // shifts the ones after it); as an unordered_map that refill was ~0.15 ms of a ~0.2 ms split.
  class PairSlots {
   public:
    bool empty() const { return live_ == 0; }
    void clear() {
      std::fill(keys_.begin(), keys_.end(), kEmpty);
      live_ = used_ = 0;
    }
    int find(uint64_t k) const {  // -1: absent
      if (keys_.empty()) return -1;
      for (size_t m = keys_.size() - 1, h = hash(k) & m;; h = (h + 1) & m) {
        if (keys_[h] == k) return vals_[h];
        if (keys_[h] == kEmpty) return -1;
      }
    }
    void set(uint64_t k, int v) {
      if ((used_ + 1) * 2 > keys_.size()) grow();
      size_t m = keys_.size() - 1, h = hash(k) & m, tomb = SIZE_MAX;
      for (;; h = (h + 1) & m) {
        if (keys_[h] == k) { vals_[h] = v; return; }
        if (keys_[h] == kTomb && tomb == SIZE_MAX) tomb = h;
        if (keys_[h] == kEmpty) break;
      }
      if (tomb != SIZE_MAX) h = tomb;
      else ++used_;
      keys_[h] = k, vals_[h] = v, ++live_;
    }
    void erase(uint64_t k) {
      if (keys_.empty()) return;
      for (size_t m = keys_.size() - 1, h = hash(k) & m;; h = (h + 1) & m) {
        if (keys_[h] == k) { keys_[h] = kTomb, --live_; return; }
        if (keys_[h] == kEmpty) return;
      }
    }

   private:
    static constexpr uint64_t kEmpty = ~0ull, kTomb = ~0ull - 1;  // no pair has these (indices < 2^32 - 1)
    static size_t hash(uint64_t k) { return (size_t)((k * 0x9E3779B97F4A7C15ull) >> 32); }
    void grow() {  // rehash without tombstones, at most half full
      std::vector<uint64_t> ok;
      std::vector<int> ov;
      ok.swap(keys_), ov.swap(vals_);
      size_t cap = 64;
      while (cap < (live_ + 1) * 4) cap *= 2;
      keys_.assign(cap, kEmpty), vals_.assign(cap, 0);
      live_ = used_ = 0;
      for (size_t q = 0; q < ok.size(); ++q)
        if (ok[q] != kEmpty && ok[q] != kTomb) set(ok[q], ov[q]);
    }
    std::vector<uint64_t> keys_;
    std::vector<int> vals_;
    size_t live_ = 0, used_ = 0;  // used_: live + tombstones
  };
  void splitEdges(const std::vector<uint64_t>& pOld, const std::vector<uint64_t>& pNew,
                  const std::vector<std::vector<uint32_t>>& fans, const std::vector<std::vector<int>>& fam, PairSlots& slotOf);
  void buildSchedule();
  void recomputeMass();
  void selfCollision(float dt);  // NX_CLF_SELFCOLLISION prepass, before gravity (cloth_self.cpp)
  void selfPair(size_t i, size_t c, float dt);
  void selfContactFind(const V3& gravity, float dt);  // cloth_selftri.cpp: this step's vertex-triangle contacts (grid search)
  void selfContactSolve();                  // one projection pass over them (per solver iteration)
  void untangleFind(float dt);              // regions of the cloth that passed through itself (called by selfContactFind)
  void untangleSolve();                     // one pass pulling them back (after the solver iterations)
  void selfContactVelocity();               // after the velocity update: inelastic, with friction (pairs that acted)
  void selfContactRest();                   // slow particles of acting pairs stop (piles come to rest)
  void edgeShapeFind();                     // cloth_edgeshape.cpp: this step's edge-shape pairs and their sides
  void edgeShapeSolve();                    // one projection pass over them (per solver iteration)
  void selfEdgeFind(const V3& up);          // edge-edge pairs (called by selfContactFind)
  void selfEdgeSolve();
  void selfEdgeVelocity();
  void attachProject(float dt);       // attachment projection, after the edge solve in each iteration
  void attachTearEdge(const Edge& e);  // attachment tearing, inside the stretch loop
  void setAttachment(int v, int shape, const V3& local, uint32_t flags);
  void release(int v);  // attachment or pin dropped, no wake-up (attachment tearing; freeVertex adds the wake-up)
  V3 shapeWorld(int shape, const V3& local) const;
  V3 shapeLocal(int shape, const V3& world) const;
  bool touching(int shape, const V3& x, float r) const;  // inside or within r of the shape (in its frame)
  std::vector<AttachInfo> att_;
  std::vector<ShapePose> pose_;
  std::vector<float> shapeInvMass_;
  struct Motion { bool moving = false, hasPrev = false, dynamic = false, hasVel = false; V3 t, pt, lin, ang, com; float r[9] = {1, 0, 0, 0, 1, 0, 0, 0, 1}, pr[9] = {1, 0, 0, 0, 1, 0, 0, 0, 1}; };
  std::vector<Motion> motion_;  // per shape (setShapeMotion)
  bool anyAttachTear_ = false;
  bool hasMesh_ = false;
  // Shape tree (generateContacts, more than 16 shapes): boxes of the cullable shapes for this step, so a particle's
  // contact search visits only the shapes near its path (in index order, so results do not change).
  bool shapeTree_ = false;
  std::vector<TriMesh::Node> shapeNodes_;
  std::vector<uint32_t> shapeOrder_, shapeIdx_, shapeAlways_;
  std::vector<V3> shapeLo_, shapeHi_;
  mutable CollideScratch collideScratch_;      // collide() without a scratch of its own (serial callers)
  std::vector<CollideScratch> contactScratch_;  // generateContacts, per chunk
  // Swept bodies among shapeAlways_ (every particle tests them): their ball test of collide() precomputed per step in
  // groups of 4 lanes (Rp 9, R 9, tp 3, t 3, rho^2: 25 floats per lane), so 4 are rejected at a time (same arithmetic
  // as the scalar test, so the same shapes pass).
  std::vector<float> swept4_;
  std::vector<uint32_t> sweptAt_;  // index in shapeAlways_ of each swept lane

  struct SelfContact {  // vertex, triangle (tri_ / 3), side (+1 / -1 along its normal); active: projected this step
    uint32_t v, t;
    float s;
    bool active;
    uint16_t stuck;  // consecutive steps left unresolved (selfContactVelocity); dropped at kGiveUp
    float dist;      // separation kept (h; h / 2 for neighbour pairs)
  };
  std::vector<SelfContact> sc_;
  struct TriBox { V3 lo, hi; };
  // Untangling (untangleFind): region of every vertex (0: the main part of its piece; others: parts cut off by edges
  // that pierce the cloth, i.e. that went through it), empty when nothing pierces. Per piercing (edge e0-e1 at tau
  // through triangle a b c at barycentric w): in: the edge's vertex to pull through (smaller region; -1: none) and the
  // side it goes to (s, along the triangle's normal); g: the in-plane slide that shortens the intersection curve.
  struct Untangle { uint32_t e0, e1, a, b, c; int in; float tau, s, w[3]; V3 g; float lim; };  // lim: slide cap
  std::vector<Untangle> ut_;
  std::vector<uint32_t> utReg_;
  float utCap_ = 0.f;  // most a piercing moves per step
  std::vector<TriBox> utBox_;  // its grid (triangle boxes at the step start)
  std::vector<uint32_t> utStart_, utItems_, utBig_;
  std::vector<float> utItemBox_;
  uint8_t utIdle_ = 0;  // steps left before the next piercing search (after one that found none)
  uint8_t utEvery_ = 2;  // steps between searches while tangled (grows while the tangle does not shrink)
  uint32_t utBest_ = 0, utLast_ = 0;  // fewest piercings of this tangle, piercings at the last search
  float utBestLen_ = 0.f;  // intersection curve length when utBest_ was set
  struct PrevPair { uint64_t key; float side; uint16_t stuck, age; };  // key = vertex << 32 | triangle; age: steps unseen
  struct EdgeContact {  // edge pair (indices into scEdges_), side direction from e2 to e1, active, stuck as above
    uint32_t e1, e2;
    V3 n;
    bool active;
    uint16_t stuck;
  };
  struct PrevEdge { uint64_t key; float unused; uint16_t stuck; V3 n; };
  std::vector<EdgeContact> se_;
  std::vector<PrevEdge> scPrevE_;
  std::vector<std::pair<uint32_t, uint32_t>> scEdges_, scPrevEdges_;  // unique mesh edges (a < b)
  std::vector<TriBox> scEdgeBox_;
  std::vector<uint32_t> scEStart_, scEItems_, scEFill_, scEBig_, scEStamp_;
  std::vector<PrevPair> scPrev_, scPrevOld_;  // last step's pairs plus remembered ones, sorted by key (old: scratch)
  std::vector<uint32_t> scPrevTris_;
  struct TriPlanes { float p0[4], p1[4]; };  // unit normal + offset at the step start and at the prediction
  std::vector<TriBox> scTriBox_;  // triangle boxes over the step, grown by the contact distance
  std::vector<TriPlanes> scPlanes_;
  std::vector<uint32_t> scStart_, scItems_, scFill_, scBig_, scStamp_, scCand_;
  std::vector<float> scItemBox_;  // four grid items: lo x/y/z, then hi x/y/z; four lanes each (SoA, 24 floats)
  struct ScChunk { std::vector<uint32_t> stamp, cand; std::vector<SelfContact> out; };  // selfContactFind per chunk
  std::vector<ScChunk> scChunks_;
  ParallelFor parallelFor_ = nullptr;
  template <class F> void parallel(size_t n, F& f) {  // f(i) for i < n, on the pool when there is one
    if (parallelFor_ && n > 1) parallelFor_(n, [](void* c, size_t i) { (*static_cast<F*>(c))(i); }, &f);
    else for (size_t i = 0; i < n; ++i) f(i);
  }
  std::vector<uint8_t> scIsBig_;
  std::vector<uint32_t> scAdjTris_, scAdjStart_, scAdj_;  // vertex neighbours (CSR) for the triangles in scAdjTris_
  ClothParams P_;
  std::vector<uint32_t> tri_;
  std::vector<V3> x_, v_, p_, pinPos_;
  std::vector<V3> rest_;
  std::vector<int> orig_;                 // particle -> rest vertex (copies share the rest position)
  int splits_ = 0;
  std::vector<float> w_, mass_;  // mass_ is kept for pinned particles too (their w_ is 0 in the solver)
  float edgeMedian_ = 0;  // median rest edge length (hit cap on cloths that cannot tear, addForceAtPos)
  std::vector<uint8_t> hard_;
  std::vector<uint8_t> tip_;  // tearing: crack tip (the original's particle flag 0x8), the preferred end of the next tear
  std::vector<uint8_t> removed_;  // NX_CLF_VALIDBOUNDS removals (sticky); all zero and anyRemoved_ false until one happens
  bool anyRemoved_ = false, removedDirty_ = false, removedChecked_ = false;
  bool hitPending_ = false;      // an impulse hit since the last step (addForceAtPos): the next step is a reference step
  bool trackParents_ = false;    // that step records the vertex each tear copy came from (parent_)
  std::vector<uint32_t> parent_;
  size_t removedCount_ = 0;
  std::vector<Edge> edges_;
  // generateContacts culling: static shape box that can hold any contact; rho = radius of a ball around the shape's
  // own origin that holds any contact (moving and posed shapes, in their frame)
  struct ShapeBound { float lo[3], hi[3]; float rho; bool cull; };
  std::vector<ShapeBound> bounds_;
  bool edgesRebuilt_ = false;    // edges_ come from rebuildEdges (rest values computed there)
  bool deferEdgeUpkeep_ = false;  // rebuildEdges inside tearStep: schedule and masses once at its end
  std::vector<V3> nrm_;                                        // wind: vertex normals (scratch, reused per step)
  std::vector<std::pair<uint32_t, V3>> depen_;                 // refDt: particles starting inside a contact (step scratch)
  std::vector<int> selfBin_;                                   // self-collision scratch (reused per step)
  std::vector<size_t> selfStart_, selfFill_, selfOrder_;
  std::vector<size_t> levelStart_;  // sedges_ offsets of the levels, plus the end
  std::vector<uint8_t> restSupported_;  // selfContactRest scratch
  struct EdgeHook { uint32_t i, j; int k; V3 n; bool through; };  // edge, shape, side normal (shape frame), through at the start
  std::vector<EdgeHook> eh_;
  std::vector<uint32_t> ehNarrow_;  // edgeShapeFind scratch: shapes narrower than the longest edge
  std::vector<float> ehBox_;        // and their world boxes (lo xyz, hi xyz)
  std::vector<Edge> sedges_;  // edges_ reordered by conflict level (buildSchedule); the solve and edge damping use it
  std::vector<StaticShape> shapes_;
  std::vector<std::vector<Contact>> contacts_;
  float wakeCounter_ = 0;
  bool asleep_ = false;
  float friction_ = 0;  // this step's friction (P_.friction, rescaled with P_.refDt), contacts and self-collision
};

}  // namespace pxc
