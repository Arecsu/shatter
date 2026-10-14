// Cloth attachments (NxCloth::attachToShape, attachToCollidingShapes, attachVertexToShape, detachFromShape, freeVertex,
// attachVertexToGlobalPosition). Behaviour from the 2.8.0 core (attachToShape 0x100dca30, attachToCollidingShapes
// 0x100e8cd0, projection 0x100e0d90, tearing in the stretch loop); see docs/re-notes-cloth.md "Attachments".
//
// Flags: bit 0 two-way, bit 1 tearable. As decompiled, a one-way attachment is hard (vertex = target, w = 0, solved like a
// pin); a two-way attachment is soft (pulled towards the target with w / (w + wb) and it reports a reaction impulse).
// Hard attachments reuse hard_/pinPos_, so the rest of the solver treats them as pins (no contacts, no stretch weight).
#include <algorithm>
#include <cmath>

#include "cloth.h"

namespace pxc {
namespace {
constexpr uint32_t kTwoWay = 1, kTearable = 2;
inline float len(const V3& a) { return std::sqrt(a.len2()); }
inline V3 rotate(const float* r, const V3& v) {  // R v
  return {r[0] * v.x + r[1] * v.y + r[2] * v.z, r[3] * v.x + r[4] * v.y + r[5] * v.z, r[6] * v.x + r[7] * v.y + r[8] * v.z};
}
inline V3 rotateT(const float* r, const V3& v) {  // R^T v
  return {r[0] * v.x + r[3] * v.y + r[6] * v.z, r[1] * v.x + r[4] * v.y + r[7] * v.z, r[2] * v.x + r[5] * v.y + r[8] * v.z};
}
}  // namespace

void Cloth::setShapePose(int shape, const V3& t, const float rot[9]) {
  if (shape < 0) return;
  if ((size_t)shape >= pose_.size()) pose_.resize(shape + 1);
  pose_[shape].t = t;
  pose_[shape].set = true;
  std::copy(rot, rot + 9, pose_[shape].r);
}

void Cloth::setShapeInvMass(int shape, float invMass) {
  if (shape < 0) return;
  if ((size_t)shape >= shapeInvMass_.size()) shapeInvMass_.resize(shape + 1, 0.f);
  shapeInvMass_[shape] = invMass;
}

// Shape frame: without setShapePose the origin is the geometric centre (sphere, box, capsule; the plane origin for
// planes) and the rotation is identity, the same frame the core uses for a shape with an identity actor pose. The
// arithmetic then matches the core's (x - t) + t, so attachment targets agree to the last bit.
V3 Cloth::shapeWorld(int shape, const V3& local) const {
  if ((size_t)shape < pose_.size() && pose_[shape].set) return pose_[shape].t + rotate(pose_[shape].r, local);
  const StaticShape& s = shapes_[shape];
  return (s.type == ShapeType::Plane ? V3() : s.a) + local;
}

V3 Cloth::shapeLocal(int shape, const V3& world) const {
  if ((size_t)shape < pose_.size() && pose_[shape].set) return rotateT(pose_[shape].r, world - pose_[shape].t);
  const StaticShape& s = shapes_[shape];
  return world - (s.type == ShapeType::Plane ? V3() : s.a);
}

// Inside-or-touching test with radius r, in the shape frame. Geometry as in cloth.cpp: plane (a = normal, d = offset),
// sphere (a = centre, d = radius), box (a = centre, half = half extents), capsule (a = centre, d = radius, half.y =
// half segment along local y). [A] the boundary (<= vs <) and the exact per-type rule are not checked against the core.
bool Cloth::touching(int shape, const V3& x, float r) const {
  const StaticShape& s = shapes_[shape];
  const V3 q = shapeLocal(shape, x);  // origin at the centre (plane: at the origin)
  switch (s.type) {
    case ShapeType::Plane:
      return s.a.dot(q) - s.d <= r;
    case ShapeType::Sphere:
      return len(q) <= s.d + r;
    case ShapeType::Box: {
      auto out = [](float v, float h) { return std::max(std::fabs(v) - h, 0.f); };
      const V3 o(out(q.x, s.half.x), out(q.y, s.half.y), out(q.z, s.half.z));
      return len(o) <= r;
    }
    case ShapeType::Capsule: {
      const float h = s.half.y;
      V3 p = q;
      p.y = std::max(-h, std::min(h, p.y));
      return len(q - p) <= s.d + r;
    }
    case ShapeType::Mesh: {  // any triangle within r (either side) [A: the core's rule for meshes is not read]
      if (!s.mesh) return false;
      bool hit = false;
      const V3 g(r, r, r);
      s.mesh->query(q - g, q + g, [&](uint32_t t) {
        if (hit) return;
        const V3 &a = s.mesh->v[3 * t], &b = s.mesh->v[3 * t + 1], &c = s.mesh->v[3 * t + 2];
        hit = (closestPointOnTriangle(q, a, b, c) - q).len2() <= r * r;
      });
      return hit;
    }
  }
  return false;
}

void Cloth::setAttachment(int v, int shape, const V3& local, uint32_t flags) {
  if (att_.size() < x_.size()) att_.resize(x_.size());
  AttachInfo& a = att_[v];
  a.shape = shape;
  a.local = local;
  a.impulse = V3();
  a.tearable = (flags & kTearable) != 0;
  a.twoWay = (flags & kTwoWay) != 0;
  anyAttachTear_ = anyAttachTear_ || a.tearable;
  hard_[v] = a.twoWay ? 0 : 1;
  w_[v] = (a.twoWay && mass_[v] > 0) ? 1.f / mass_[v] : 0.f;
}

void Cloth::attachToShape(int shape, uint32_t flags) {
  if (shape < 0 || (size_t)shape >= shapes_.size()) return;
  const float r = P_.thickness * 0.5f;
  for (size_t i = 0; i < x_.size() && i < mass_.size(); ++i) {
    if (isAttachedToShape(i)) continue;
    if (hard_[i]) continue;  // global pins (attached before) keep their attachment
    if (!touching(shape, x_[i], r)) continue;
    setAttachment(i, shape, shapeLocal(shape, x_[i]), flags);
  }
  wakeFromCall();
}

void Cloth::attachToCollidingShapes(uint32_t flags) {
  const float r = P_.thickness * 0.5f;
  for (size_t i = 0; i < x_.size() && i < mass_.size(); ++i) {
    if (isAttachedToShape(i) || hard_[i]) continue;
    for (size_t k = 0; k < shapes_.size(); ++k) {
      if (!touching((int)k, x_[i], r)) continue;
      setAttachment(i, (int)k, shapeLocal((int)k, x_[i]), flags);
      break;
    }
  }
  wakeFromCall();
}

void Cloth::attachVertexToShape(int vertex, int shape, const V3& localPos, uint32_t flags) {
  if (vertex < 0 || (size_t)vertex >= mass_.size() || shape < 0 || (size_t)shape >= shapes_.size()) return;
  if (isAttachedToShape(vertex)) return;  // 0x100dd0b0: a vertex attached to a shape keeps that attachment
  setAttachment(vertex, shape, localPos, flags);
  wakeFromCall();
}

// 0x100e5800: refused for a vertex attached to a shape; otherwise the vertex is pinned (hard) and the cloth woken.
void Cloth::attachVertexToGlobalPosition(int vertex, const V3& worldPos) {
  if (vertex < 0 || (size_t)vertex >= hard_.size()) return;
  if (isAttachedToShape(vertex)) return;
  pinHard(vertex, worldPos);
  wakeFromCall();
}

void Cloth::attachVertexToInitialPosition(int vertex) {
  if (vertex < 0 || (size_t)vertex >= rest_.size()) return;
  attachVertexToGlobalPosition(vertex, rest_[vertex]);
}

void Cloth::release(int vertex) {
  if ((size_t)vertex < att_.size()) att_[vertex] = AttachInfo();
  hard_[vertex] = 0;
  w_[vertex] = (mass_[vertex] > 0) ? 1.f / mass_[vertex] : 0.f;
}

void Cloth::freeVertex(int vertex) {  // 0x100dd1d0: wakes the cloth
  if (vertex < 0 || (size_t)vertex >= hard_.size()) return;
  release(vertex);
  wakeFromCall();
}

void Cloth::detachFromShape(int shape) {  // 0x100dcf00: wakes the cloth if it freed a vertex
  bool any = false;
  for (size_t i = 0; i < att_.size(); ++i)
    if (att_[i].shape == shape) release((int)i), any = true;
  if (any) wakeFromCall();
}

// Attachment projection (0x100e0d90), once per solver iteration after the edge constraints and before the collision
// projection. Hard: p = target (pins use the same path through pinPos_). Soft: p += (target - p) w / (w + wb), reaction
// -(target - p) / (w + wb) / dt is kept for the body (wb = body inverse mass, 0 for static or kinematic shapes).
void Cloth::attachProject(float dt) {
  if (att_.empty()) return;
  const float invDt = 1.f / dt;
  const size_t n = std::min(att_.size(), x_.size());
  const uint8_t* rm = anyRemoved_ ? removed_.data() : nullptr;
  for (size_t i = 0; i < n; ++i) {
    AttachInfo& a = att_[i];
    if (a.shape < 0) continue;
    if (rm && rm[i]) {  // removed (valid bounds): no projection, no reaction
      a.impulse = V3();
      continue;
    }
    a.target = shapeWorld(a.shape, a.local);
    if (!a.twoWay) {
      pinPos_[i] = a.target;  // applied by the hard-pin projection that follows
      a.impulse = V3();
      continue;
    }
    const float wb = ((size_t)a.shape < shapeInvMass_.size()) ? shapeInvMass_[a.shape] : 0.f;
    const float w = w_[i], s = w + wb;
    if (s <= 0.f) {
      a.impulse = V3();
      continue;
    }
    const V3 d = a.target - p_[i];
    p_[i] += d * (w / s);
    a.impulse = d * (-invDt / s);
  }
}

// Attachment tearing (inside the stretch loop, 0x100dd9e0): an attached end of an edge whose length exceeds
// attachmentTearFactor * rest length is detached, if its attachment is tearable.
void Cloth::attachTearEdge(const Edge& e) {
  if (!anyAttachTear_) return;
  const bool iT = (size_t)e.i < att_.size() && att_[e.i].shape >= 0 && att_[e.i].tearable;
  const bool jT = (size_t)e.j < att_.size() && att_[e.j].shape >= 0 && att_[e.j].tearable;
  if (!iT && !jT) return;
  if (len(p_[e.j] - p_[e.i]) <= P_.attachmentTearFactor * e.L0) return;
  if (iT) release(e.i);
  if (jT) release(e.j);
}

std::vector<AttachReaction> Cloth::attachReactions() const {
  std::vector<AttachReaction> out;
  for (const AttachInfo& a : att_)
    if (a.shape >= 0 && a.twoWay) out.push_back({a.shape, a.target, a.impulse * P_.attachmentResponse});
  return out;
}

}  // namespace pxc
