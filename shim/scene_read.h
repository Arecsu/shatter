// Scene readback for the hosted fluid solver (scene_read.cpp).
#pragma once

#include <stdint.h>

#include <vector>

#include "../solver/fluid/fluid.h"

namespace shim {

struct SceneSettings {
  pxr::Vec3 gravity;
  float maxTimestep = 0;
  uint32_t maxIter = 0, timestepMethod = 0;  // NxTimeStepMethod: 0 fixed, 1 variable, 2 inherit
};

// What decides which shapes a fluid collides with (from its NxFluidDesc).
struct FluidFilterDesc {
  uint32_t collisionGroup = 0;
  uint32_t groupsMask[4] = {0, 0, 0, 0};
  bool useGroupsMask = false;  // NxFluidDesc.groupsMask / the scene filter: no effect on fluid pairs [V, probe_drain grp_mask_*]
  bool includeStatic = true;   // collisionMethod & NX_F_STATIC
  bool includeDynamic = true;  // collisionMethod & NX_F_DYNAMIC
};

// Axis-aligned box (a fluid's particles plus margin, or a shape's bounds).
struct Box3 {
  pxr::Vec3 lo, hi;
};

uint32_t sceneActorCount(uint32_t scene);

// Collision geometry of one scene, shared by its hosted fluids.
//  - Static actors: one cached entry per NxShape (converted once, mesh midphase built once). The actor list is
//    walked every refresh (a hash lookup per actor); the static shapes are rescanned only when the set of static
//    actors changes (dynamic actors coming and going, e.g. glass shards, don't trigger it), plus a pose check
//    every 600 refreshes.
//  - Dynamic actors (kinematic included): read every refresh at their current pose. The shim refreshes before the
//    scene's simulate(), so that is the end of the previous rigid frame [A: the fluid runs one frame behind the
//    bodies; the original steps them together]. Per actor: the shape list and local-frame geometry are cached
//    (convexes shared per NxConvexMesh), the actor pose decides whether it is near any fluid, and only near actors
//    get their velocities, centre of mass and flags read. Earlier poses are the current one integrated back with
//    the actor's velocities (semi-implicit Euler, what the rigid step did).
class SceneGeometry {
 public:
  struct Timing {  // ms of the last refresh
    double actors = 0, statics = 0, dynamics = 0;
  };
  SceneGeometry();
  ~SceneGeometry();
  SceneGeometry(const SceneGeometry&) = delete;
  SceneGeometry& operator=(const SceneGeometry&) = delete;
  // nearBoxes: the hosted fluids' particle bounds (already grown by their margins); dynamic actors away from all of them
  // are skipped. frameDt: how far back the fluid steps of this frame reach (all substeps).
  void refresh(uint32_t scene, const std::vector<Box3>& nearBoxes, float frameDt, bool census);
  // The scene's group table as of the last refresh (NxScene::getGroupCollisionFlag), for the solver's setCollisionGroups.
  void collisionGroups(pxr::CollisionGroups& out) const;
  void selectStatic(const FluidFilterDesc& fluid, const Box3& box, std::vector<const pxr::StaticShape*>& out) const;
  // Dynamic shapes for one fluid step that sweeps them from backBegin to backEnd seconds before the refresh pose
  // (the frame's substeps laid over the bodies' last frame).
  void selectDynamic(const FluidFilterDesc& fluid, const Box3& box, float backBegin, float backEnd,
                     std::vector<pxr::DynamicShape>& out) const;
  // Two-way coupling: applies the impulses (body = NxActor*) to actors that still exist and are dynamic,
  // summed per actor into one linear and one angular impulse (NX_IMPULSE).
  void applyImpulses(const std::vector<pxr::BodyImpulse>& impulses) const;
  // Cloth attachments (additive): the geometry key of an NxShape the cache knows, i.e. the pointer selectStatic and
  // selectDynamic hand out (DynamicShape::geom). Statics: any usable one. Dynamic: shapes of actors built near a refresh.
  bool findShapeGeometry(uint32_t nxShape, const pxr::StaticShape*& geom) const;
  const Timing& timing() const { return timing_; }
  uint32_t staticCount() const;
  uint32_t dynamicNear() const;  // dynamic shapes read in the last refresh
  struct State;  // scene_read.cpp

 private:
  State* st_;
  Timing timing_;
};

void readSceneSettings(uint32_t scene, SceneSettings& s);

}  // namespace shim
