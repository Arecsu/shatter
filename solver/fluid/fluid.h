// Our NxFluid-compatible particle solver (behavioural reimplementation, see docs/re-notes-fluid.md).
// Plain C++17, no dependencies. World units, same parameters as NxFluidDesc.
#pragma once

#include <cstdint>
#include <limits>
#include <set>
#include <utility>
#include <vector>

namespace pxr {

struct Vec3 {
  float x = 0, y = 0, z = 0;
  Vec3() = default;
  Vec3(float a, float b, float c) : x(a), y(b), z(c) {}
  Vec3 operator+(const Vec3& o) const { return {x + o.x, y + o.y, z + o.z}; }
  Vec3 operator-(const Vec3& o) const { return {x - o.x, y - o.y, z - o.z}; }
  Vec3 operator*(float s) const { return {x * s, y * s, z * s}; }
  Vec3& operator+=(const Vec3& o) { x += o.x; y += o.y; z += o.z; return *this; }
  Vec3& operator-=(const Vec3& o) { x -= o.x; y -= o.y; z -= o.z; return *this; }
  float dot(const Vec3& o) const { return x * o.x + y * o.y + z * o.z; }
  float len2() const { return dot(*this); }
};

enum class SimMethod { SPH, NoInteraction, Mixed };

// NxFluid::updateParticles force modes (NxForceMode numbering). The original treats SMOOTH_IMPULSE and
// SMOOTH_VELOCITY_CHANGE as NX_FORCE on fluids (it logs "mode not supported"); forceVelocityChange does the same.
enum class ForceMode : uint32_t { Force = 0, Impulse = 1, VelocityChange = 2, SmoothImpulse = 3, SmoothVelocityChange = 4, Acceleration = 5 };

struct FluidParams {
  uint32_t maxParticles = 32767;
  float restParticlesPerMeter = 50.0f;
  float restDensity = 1000.0f;
  float kernelRadiusMultiplier = 1.2f;
  float motionLimitMultiplier = 3.6f;
  float collisionDistanceMultiplier = 0.12f;
  uint32_t packetSizeMultiplier = 16;
  float stiffness = 20.0f;
  float viscosity = 6.0f;
  float damping = 0.0f;
  Vec3 externalAcceleration;
  float restitutionStatic = 0.5f, frictionStatic = 0.05f;
  float restitutionDynamic = 0.5f, frictionDynamic = 0.5f;
  SimMethod method = SimMethod::SPH;
  bool disableGravity = false;
  uint32_t numReserveParticles = 0;
  bool priorityMode = false;          // NX_FF_PRIORITY_MODE
  bool twoWay = false;                // NX_FF_COLLISION_TWOWAY
  float collisionResponseCoefficient = 0.2f;
  // Drain shapes test a particle only from its (drainLatencySteps)-th step on: the original has no shape pairs for a
  // new particle's first steps [V: probe_drain/drain_prox_* remove at step 3 for a particle added at step 0; contacts
  // are not gated]. Collision responses are not delayed.
  uint32_t drainLatencySteps = 2;
  // Analytic shapes (plane, box, sphere, capsule; not mesh) skip a new particle's first newParticleShapeLatency simulate
  // steps: ballistic motion, no contact or proximity. The original does this [V: probe_band, age 0-1 latent for plane, box,
  // sphere, capsule; mesh has no latency]. 0 = collide from the first step (default; the probes compare with 2, PXF_AGE_LATENCY).
  uint32_t newParticleShapeLatency = 0;
  // Step-rate independence (not in the original). refDt > 0: friction on a sustained contact (a particle still on the
  // plane it touched the step before) keeps the decay per second it has in steps of refDt, (1 - f)^(dt / refDt) kept
  // per step, instead of slowing sliding particles faster the more steps run per second. Impacts keep f.
  // 0 = off (the original's behaviour).
  float refDt = 0.0f;
};

enum ParticleFlag : uint32_t {
  PF_STATIC_CONTACT = 1u << 0,
  PF_DYNAMIC_CONTACT = 1u << 1,
  PF_SEPARATED = 1u << 2,
  PF_MOTION_LIMIT = 1u << 3,
};

// One world-space triangle of a static mesh. Front face = (v1 - v0) x (v2 - v0); back faces are ignored.
struct MeshTri {
  Vec3 v0, v1, v2;
};

// Mesh midphase node (see buildMeshBVH). Leaf: count > 0, first = offset into bvhTris. Inner: count == 0,
// first = index of the right child; the left child is the next node.
struct MeshBVHNode {
  Vec3 lo, hi;
  uint32_t first = 0;
  uint32_t count = 0;
};

// Inflated bounds of one triangle (the midphase's leaf test).
struct TriBox {
  Vec3 lo, hi;
};

// Rigid transform local -> world (NxMat34 layout: row-major rotation, then translation).
struct Pose {
  float m[3][3] = {{1, 0, 0}, {0, 1, 0}, {0, 0, 1}};
  Vec3 t;
  Vec3 rotate(const Vec3& v) const {
    return Vec3(m[0][0] * v.x + m[0][1] * v.y + m[0][2] * v.z, m[1][0] * v.x + m[1][1] * v.y + m[1][2] * v.z,
                m[2][0] * v.x + m[2][1] * v.y + m[2][2] * v.z);
  }
  Vec3 apply(const Vec3& v) const { return rotate(v) + t; }
  // world -> local (the rotation is orthonormal)
  Vec3 applyInv(const Vec3& w) const {
    const Vec3 d = w - t;
    return Vec3(m[0][0] * d.x + m[1][0] * d.y + m[2][0] * d.z, m[0][1] * d.x + m[1][1] * d.y + m[2][1] * d.z,
                m[0][2] * d.x + m[1][2] * d.y + m[2][2] * d.z);
  }
};

// Static collision geometry, world space: axis-aligned boxes, capsules along y. A rotated box or tilted capsule is
// `oriented`: its geometry is given in its own frame (c = origin, capsule along local y) and `frame` maps it to the
// world; it is tested with the same rules in that frame.
struct StaticShape {
  enum Type { Plane, Sphere, Box, Capsule, Mesh } type;
  bool oriented = false;
  uint16_t group = 0;  // NxShapeDesc.group: pairs with the fluid's group only if the scene allows it (CollisionGroups)
  bool drain = false;  // NX_SF_FLUID_DRAIN: a particle in contact or proximity is removed at the next step
  bool convex = false;  // Mesh made from an NxConvexShape (tested as its triangles; coverage counters only)
  Pose frame;    // oriented box/capsule: local -> world
  Vec3 n;        // plane normal
  float d = 0;   // plane: n.x = d
  Vec3 c;        // centre (sphere/box/capsule)
  Vec3 half;     // box half extents
  float r = 0;   // sphere/capsule radius
  float h = 0;   // capsule cylinder height
  std::vector<MeshTri> tris;  // mesh: all triangles of one static triangle-mesh actor
  // Mesh midphase, filled by buildMeshBVH() once the triangles are final. Without it a mesh is tested brute force.
  std::vector<MeshBVHNode> bvh;
  std::vector<uint32_t> bvhTris;    // triangle indices in leaf order
  std::vector<TriBox> triBox;       // per triangle, indexed like tris
  // Hashed uniform grid over the triangle boxes (built with the midphase): a query returns the same triangles as
  // the tree, faster for the small boxes a particle step asks for. Cells: sorted keys, CSR lists (ascending
  // triangle index), open-addressing table of cell index + 1.
  struct Grid {
    Vec3 org;
    float inv = 0;
    std::vector<uint64_t> keys;
    std::vector<uint32_t> start, items, table, big;  // big: triangles covering too many cells (always candidates)
    uint32_t mask = 0;
  } grid;
};

// Builds the midphase of a mesh shape from its current triangles. Call once after the mesh is complete.
void buildMeshBVH(StaticShape& s);

// World bounds of a static shape (oriented ones included; a mesh uses its midphase root once built). Returns false
// for a plane (unbounded; lo/hi untouched).
bool staticShapeBounds(const StaticShape& s, Vec3& lo, Vec3& hi);

// A shape of a dynamic or kinematic actor for one step (docs/re-notes-fluid-mesh.md 2.1). The geometry is in the
// shape's local frame (box/sphere centred at the origin, capsule along local y, convex as a local triangle mesh).
// The particle's start is taken into the frame with `begin`, its target with `end`, so the shape's motion over the
// step is swept as relative motion; results go back to world space with `end`.
struct DynamicShape {
  const StaticShape* geom = nullptr;
  Pose begin, end;
  Vec3 linVel, angVel, com;  // surface velocity at world x: linVel + angVel x (x - com)
  // Two-way coupling (NX_SF_FLUID_TWOWAY on a non-kinematic body): impulses are reported per body index.
  bool twoWay = false;
  uint32_t body = 0;
  uint32_t shape = 0;  // NxShape* (stable across geometry rebuilds; the cloth keys its body slots on it)
  uint16_t group = 0;  // see StaticShape::group
  bool drain = false;  // see StaticShape::drain
  Vec3 pointVelocity(const Vec3& x) const;
};

// NxShapeDesc / NxActorDesc flag bits (values from the 2.8.1 SDK headers, Nxp.h and NXU_schema.h). They decide whether
// a shape takes part in fluid collision at all. The solver does not read them: the caller passes only the shapes for
// which shapeCollidesWithFluid() is true (docs/re-notes-fluid-drain.md, "pair gating").
enum : uint32_t {
  kShapeDisableCollision = 1u << 4,
  kShapeFluidDrain = 1u << 8,
  kShapeFluidDisableCollision = 1u << 10,
  kShapeDisableResponse = 1u << 12,
  kShapeTriggerEnable = 7u,  // NX_TRIGGER_ENABLE = ON_ENTER | ON_LEAVE | ON_STAY
  kActorDisableCollision = 1u << 0,
  kActorDisableResponse = 1u << 1,
  kActorFluidDisableCollision = 1u << 3,
};

// False for a shape that never collides with fluid: triggers, DISABLE_COLLISION / FLUID_DISABLE_COLLISION /
// DISABLE_RESPONSE on the shape or its actor, and a static in a rigid-body compartment (invisible to fluid).
inline bool shapeCollidesWithFluid(uint32_t shapeFlags, uint32_t actorFlags, bool rbCompartment) {
  if (rbCompartment) return false;
  if (shapeFlags & (kShapeDisableCollision | kShapeFluidDisableCollision | kShapeDisableResponse | kShapeTriggerEnable))
    return false;
  return !(actorFlags & (kActorDisableCollision | kActorDisableResponse | kActorFluidDisableCollision));
}

// Scene group collision table (NxScene::setGroupCollisionFlag, groups 0..31). Every pair is enabled by default.
struct CollisionGroups {
  uint32_t allow[32];
  CollisionGroups() {
    for (uint32_t& a : allow) a = ~0u;
  }
  void set(uint32_t a, uint32_t b, bool on) {
    a &= 31, b &= 31;
    allow[a] = on ? (allow[a] | (1u << b)) : (allow[a] & ~(1u << b));
    allow[b] = on ? (allow[b] | (1u << a)) : (allow[b] & ~(1u << a));
  }
  bool pass(uint32_t a, uint32_t b) const { return (allow[a & 31] >> (b & 31)) & 1u; }
};

// Impulse handed to a body during the last step (two-way coupling), applied at a world position (NX_IMPULSE).
struct BodyImpulse {
  uint32_t body;
  Vec3 impulse, pos;
};

// Mass of one particle: restDensity / restParticlesPerMeter^3 (the SDK reports the same value).
float particleMass(const FluidParams& P);
// Velocity change of one updateParticles force entry f. simTime: the simulated time of the last simulate, i.e. its
// substeps times the substep length, but at least one substep (0 before the first simulate). NX_FORCE scales it by
// 1/mass, NX_IMPULSE by 1/mass too but without time, NX_VELOCITY_CHANGE is f itself, NX_ACCELERATION by simTime.
// Derived from the core's scale (docs/re-notes-fluid-api.md, updateParticles).
Vec3 forceVelocityChange(ForceMode mode, const Vec3& f, float simTime, float mass);

class Fluid {
 public:
  explicit Fluid(const FluidParams& p);
  uint32_t addParticles(const std::vector<Vec3>& pos, const std::vector<Vec3>& vel);
  // Adds up to n particles (capped at maxParticles - size()). life may be null (= 0 = never expires); vel may be
  // null (zero). Ids are the lowest free ids (freed ids are reused lowest first). Writes the new ids to newIds (if
  // non-null) and returns the count.
  uint32_t addParticles(const Vec3* pos, const Vec3* vel, const float* life, uint32_t n, uint32_t* newIds);
  // Takes effect at the start of the next step(); unknown ids are ignored.
  void deleteParticle(uint32_t id);
  // Velocity change for one particle, added to its velocity at the start of the next step() (before gravity, SPH and
  // collision, as an initial velocity would be). Pending changes wait while steps do not run. Unknown ids are ignored.
  void addVelocityChange(uint32_t id, const Vec3& dv);
  // Takes effect at the end of the next step(), after the world bounds (the original keeps the particles in that
  // step's bounds); the particles are not in positions() after it.
  void removeAllParticles();
  void step(float dt, const Vec3& gravity, const std::vector<StaticShape>& statics);
  // Same, with the shapes given by pointer (the shim passes only the cached shapes near the fluid), plus the
  // dynamic actors' shapes for this step (resolved before the statics, with the dynamic coefficients).
  void step(float dt, const Vec3& gravity, const std::vector<const StaticShape*>& statics,
            const std::vector<DynamicShape>& dynamics = {});
  // Runs fn(ctx, c) for every c in [0, n), possibly on several threads, and returns when all are done. collide()
  // uses it for chunks of 256 particles; results are identical with or without it. Null (default) = inline.
  using ParallelFor = void (*)(size_t n, void (*fn)(void* ctx, size_t c), void* ctx);
  void setParallelFor(ParallelFor pf) { parallelFor_ = pf; }
  // Two-way coupling: impulses for the bodies from the last step (empty unless params.twoWay).
  const std::vector<BodyImpulse>& bodyImpulses() const { return impulses_; }

  size_t size() const { return pos_.size(); }
  const std::vector<Vec3>& positions() const { return pos_; }
  const std::vector<Vec3>& velocities() const { return vel_; }
  const std::vector<float>& densities() const { return rho_; }
  const std::vector<Vec3>& normals() const { return nrm_; }
  const std::vector<uint32_t>& ids() const { return id_; }
  const std::vector<uint32_t>& flags() const { return flags_; }
  // Ids removed during the last step() (deletes, removeAll, priority eviction, expiry), in any order.
  const std::vector<uint32_t>& deletedIds() const { return deleted_; }
  // Remaining life per particle (0 = infinite), same order as positions().
  const std::vector<float>& lifetimes() const { return life_; }
  // Tight AABB of the positions after the last step; empty: lo = +FLT_MAX, hi = -FLT_MAX.
  void worldBounds(Vec3& lo, Vec3& hi) const { lo = bLo_; hi = bHi_; }
  // The fluid's NxFluidDesc collision group and the scene's group table; a shape whose group the table disables
  // is skipped in collide(). Default: group 0, all pairs enabled.
  void setCollisionGroups(uint32_t fluidGroup, const CollisionGroups& table) {
    group_ = fluidGroup;
    groups_ = table;
  }

 private:
  void computeSph();
  void computeSphReference();  // the original pair loop (PXF_SPH_REF=1), for exactness checks
  void integrate(float dt, const Vec3& gravity, bool sph);
  void collide(float dt, const std::vector<const StaticShape*>& statics, const std::vector<DynamicShape>& dynamics);
  void transferImpulses();
  // two-way: add to particle i's debt (a debt to another body is settled into `out` at once)
  void owe(size_t i, uint32_t body, const Vec3& impulse, const Vec3& at, std::vector<BodyImpulse>& out);
  uint32_t allocId();
  // Removes the particles with kill[i] != 0 (stable), frees their ids and appends them to deleted_.
  void compact(const std::vector<uint8_t>& kill);
  void applyPending();   // deleteParticle calls of this step
  void evictPriority();  // NX_FF_PRIORITY_MODE: keep size() <= maxParticles - numReserveParticles
  void expire();         // lifetime: removes particles whose remaining life reached 0 and updates life_
  void computeBounds();

  // Contact planes cached from the previous step (impact or proximity), up to two per particle.
  struct CachedPlane {
    Vec3 n;
    float d = 0;  // plane: n.x = d (already offset by the collision distance)
    bool valid = false;
    int kind = 0;  // StaticShape::Type of the shape that produced the plane
    // dynamic shape: resolved with the dynamic coefficients against the surface velocity stored with the plane
    // (dyn_rest step 38 [V]); two-way charges `body`
    bool dyn = false, twoWay = false;
    Vec3 vs;
    uint32_t body = 0;
  };
  struct Cache {
    CachedPlane p[2];
  };

  static constexpr float kMinFriction = 0.001f;
  FluidParams P_;
  // derived (see docs/re-notes-fluid.md "parameter block")
  float spacing_, sigma_, invSigma_, hw_, hw2_, h_, h2_, poly6m_, pressC_, viscC_, selfRho_;
  float motionLimit_, collDist_;
  uint32_t group_ = 0;
  CollisionGroups groups_;
  std::vector<uint8_t> drainHit_;  // per particle, set by collide(): in contact or proximity of a drain shape this step
  std::vector<uint64_t> born_;     // per particle: steps_ when it was added
  uint64_t steps_ = 0;             // completed step() calls

  std::vector<Vec3> pos_, vel_, force_, nrm_;
  std::vector<float> rho_, press_;
  std::vector<uint32_t> id_, flags_;
  std::vector<Vec3> prevVel_;  // velocity before this step's integration
  std::vector<Cache> cache_;
  // Two-way: momentum the particle still owes its last dynamic contact's body (handed over 10% per step, 2.8.1
  // oracle [V: dyn_twoway_float, dyn_twoway_float_rm]) and that body's index.
  std::vector<Vec3> owed_;
  std::vector<uint32_t> owedBody_;
  std::vector<BodyImpulse> impulses_;
  // lifetime: expiry_ = absolute time of expiry (0 = never), life_ = remaining life (lifetimes())
  std::vector<float> expiry_, life_;
  float time_ = 0;  // simulated time, advanced at the end of each step
  size_t lastCount_ = 0;     // size() at the end of the previous step: the priority eviction decision (adds since then
                             // have the reserve as headroom until the next step)
  uint64_t nextSeq_ = 0;     // creation sequence (priority eviction ties: oldest first)
  std::vector<uint64_t> seq_;  // per particle creation sequence
  uint32_t nextId_ = 0;
  std::set<uint32_t> freeIds_;          // ids below nextId_ that are not in use
  std::vector<uint32_t> pendingDel_;    // deleteParticle calls not yet applied
  std::vector<std::pair<uint32_t, Vec3>> pendingVel_;  // addVelocityChange calls not yet applied
  void applyVelocityChanges();
  bool pendingRemoveAll_ = false;
  std::vector<uint32_t> deleted_;       // ids removed during the last step
  Vec3 bLo_{std::numeric_limits<float>::max(), std::numeric_limits<float>::max(), std::numeric_limits<float>::max()};
  Vec3 bHi_{-std::numeric_limits<float>::max(), -std::numeric_limits<float>::max(), -std::numeric_limits<float>::max()};
  // Mixed mode alternates SPH (phase 0) and no-interaction (phase 1) every step; Fluid post-step
  // 0x10102e00 flips the phase byte. A lone mixed fluid starts in phase 0.
  bool mixedPhase_ = false;
  bool keepCache_ = true;
  ParallelFor parallelFor_ = nullptr;
  // computeSph scratch (kept between steps to avoid reallocation)
  struct SphScratch {
    struct PC { uint64_t key; uint32_t i; };
    struct Chunk {  // per chunk of whole cells: neighbour lists of its particles
      std::vector<uint32_t> upJ, loJ;
      std::vector<float> upW, loW, cx, cy, cz;  // cx..: candidates of the current cell
      std::vector<uint32_t> cj;
      std::vector<std::pair<uint32_t, float>> lo;
      size_t nUp = 0, nLo = 0;  // used entries of upJ/upW and loJ/loW
    };
    static uint32_t hash(uint64_t k) { return (uint32_t)((k * 0x9E3779B97F4A7C15ull) >> 32); }
    std::vector<int32_t> cell;
    std::vector<PC> pc, pcTmp;
    std::vector<uint64_t> keys;
    std::vector<uint32_t> start, sorted, table, upOff, upCnt, loOff, loCnt, chunkCell;
    std::vector<float> sx, sy, sz, inv, q;  // inv: 1 / rho per particle; q: positions / cell size
    uint32_t mask = 0;
    std::vector<uint32_t> dense;  // cell index + 1 by position in the bounding box (empty: use table)
    int32_t dmn[3] = {0, 0, 0};
    uint32_t dext[3] = {0, 0, 0};
    std::vector<Chunk> chunks;
  } sph_;  // PXR_FLUID_KEEPCACHE=0 disables untouched-cache retention (A/B testing)
};

}  // namespace pxr
