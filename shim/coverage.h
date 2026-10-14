// Coverage counters for the PhysX usage audit (docs/audit/GAPS.md): one or more per gap row, counting how often the
// game reaches a path where Shatter differs from the original (or did before a fix). Lock-free, any thread.
// Logged as "coverage (<why>)" lines in PhysXLoader_shim.log at every releaseScene (the counts since the previous
// dump, i.e. per level) and at detach (run totals). PXSHIM_COVERAGE=0 turns the logging off (counting stays on).
#pragma once

#include <stdint.h>

namespace shim {

enum Cov : uint32_t {
  // fluid
  kCovFluidResponseNonDefault,  // row 1: fluid created with collisionResponseCoefficient != 0.2
  kCovFluidResponseTwoWay,      // row 1: ... and NX_FF_COLLISION_TWOWAY (only then does the coefficient matter)
  kCovFluidTwoWayCreated,       // row 1: two-way fluids created
  kCovFluidMovingBodySteps,     // rows 12/13: fluid steps with a moving dynamic/kinematic shape near
  kCovFluidConvexNear,          // row 14: fluid steps with a convex shape near
  kCovFluidStaticFriction,      // row 15: fluid created with staticFriction (static or dynamic shapes) != 0
  kCovFluidAddOver4096,         // row 17: addParticles calls adding more than 4096 particles
  kCovFluidSetterAfterCreate,   // row 21: fluid property setters called on a hosted fluid
  kCovFluidEmitterCreate,       // row 21: NxFluid::createEmitter
  kCovFluidHardware,            // row 22: fluid desc with NX_FF_HARDWARE
  kCovSceneGetFluids,           // row 31: NxScene::getNbFluids / getFluids
  // cloth
  kCovClothMovingBodySubsteps,  // row 2: cloth substeps x moving body shapes near a cloth (contacts use v, not v - v_shape)
  kCovClothSetPositions,        // row 3: setPositions / setPosition (SetClothPosition, ResetClothVertsToRefPose)
  kCovClothOutsideStaticBox,    // row 3: fetches where a cloth's particles left the box its statics were collected in
  kCovClothStaticsRequery,      // row 3: statics re-collected (teleport / travel)
  kCovClothValidBoundsFlag,     // row 4: cloths created or set with NX_CLF_VALIDBOUNDS
  kCovClothSetValidBounds,      // row 4: setValidBounds calls
  kCovClothOutsideValidBounds,  // row 4: particles removed (or, before the fix, that would be) by the valid bounds
  kCovClothDisableCollToggle,   // row 5: setFlags toggling NX_CLF_DISABLE_COLLISION on a built cloth
  kCovClothBodySlotNew,         // row 6: body shapes added to a cloth's solver
  kCovClothBodySlotPark,        // row 6: body shapes parked (left the cloth's neighbourhood)
  kCovClothBodySlotReuse,       // row 6: body shapes that came back to their old slot (stable identity)
  kCovClothPressureFlag,        // row 7: cloths created or set with NX_CLF_PRESSURE
  kCovClothSetPressure,         // row 7: setPressure calls with the pressure flag on
  kCovClothTwoWayFlag,          // row 8: cloths created or set with NX_CLF_COLLISION_TWOWAY
  kCovClothSetCollResponse,     // row 8: setCollisionResponseCoefficient calls with the two-way flag on
  kCovClothAttachToCore,        // row 9: NxCloth::attachToCore
  kCovShapeClothFlags,          // row 10: scene shapes with NX_SF_CLOTH_DISABLE_COLLISION or NX_SF_CLOTH_DRAIN
  kCovShapeFluidGates,          // row 11: scene shapes with fluid-only gate flags (also gate cloth)
  kCovClothConvexNear,          // row 14: convex shapes in a cloth's solver
  kCovClothHardware,            // row 22: cloth desc with NX_CLF_HARDWARE
  kCovClothAttachColliding,     // row 29: attachToCollidingShapes
  kCovClothAttachShapeNull,     // row 29: getVertexAttachmentShape with no NxShape to return (colliding-shape attachment)
  kCovSceneGetCloths,           // row 31: NxScene::getNbCloths / getCloths
  kCovClothAdhere,              // row 32: setMinAdhereVelocity calls or NX_CLF_ADHERE
  kCovClothUnsupported,         // row 33: calls of stub / stored-only cloth slots (names in the log, once each)
  kCovCount
};

void cov(Cov id, uint32_t n = 1);
void covLog(const char* why, bool sinceLast);

}  // namespace shim
