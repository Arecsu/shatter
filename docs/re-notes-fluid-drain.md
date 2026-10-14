# Fluid drain, triggers and pair-gating flags (PhysXCore 2.8.0 decompile + 2.8.1 oracle probes)

Probes: `scenarios/fluid/probe_drain/*.txt`, oracle = 2.8.1 core (matched headers, option C). Shape/actor flag values
from `third_party/physx-sdk-2.8.1/.../Nxp.h`. Decompile: `re/out/PhysXCore-280/decomp.c`.
Marks: [V] = probe result, [A] = decompile reading only. Probe setup: fluid `simulationMethod none` unless noted,
collisionDistanceMultiplier 0.12 with spacing 0.05, so cd = 0.006 m; box top face at y = 0.2; plane at y = 0.

## Answers

**1. NX_SF_FLUID_DRAIN (1<<8)** (`shape+0x15c`, bit 8)
- A particle that is in the **proximity band** of a drain shape is removed. Proximity alone is enough: a particle
  at rest 0.0115 m above the top face was removed with no contact and no motion; at 0.0125 m it stayed [V].
  Threshold is 2·cd (cd = 0.006, so 0.012) [V to ±0.0005 m; A for the 2·cd form, from the prox = 2·cd notes].
- The drain test is the same "any contact incl. proximity" test (record flags `& 0x1c`) that sets the output flags bit 0
  [V: output flag 0x1 appears at proximity with no drain, `prox_ctrl_0.01`].
- **It still collides.** In the step where a particle first touches a drained box, its position and velocity are
  identical to the no-drain control (drop test: vel_y after the bounce 1.0628 in both runs) [V].
- **Removal timing:** the particle is flagged in step N (N = first contact or proximity step) and is **absent from the
  output of step N+1** (removed by the simulate that follows) [V, count and id trace]. The output flag word does not show
  the drain (0x5 with and without drain) [V]. The deletion-id buffer contents were not checked [A: reported as deleted
  through the normal deletion path].
- Mechanism [A from decompile]: pair flag bit 1 (value 2) = drain, read by `PxsFluidBodyContactManager_vf8` (0x1017de20).
  In `0x1019b800`, when the drain bit is set and the record's `& 0x1c` is non-zero, the particle's internal flag word
  (+0x2e) gets `|= 4`. The response is computed before this, so the particle still gets resolved that step.
- Not for triangle meshes: `NX_SF_FLUID_DRAIN` on a mesh is rejected by the SDK [V string; mesh notes].
- Untested: drain on dynamic shapes, drain with SPH (game case) [A: same pair path].

**2. Trigger shapes (NX_TRIGGER_ENABLE = bits 0..2 of shapeFlags)**
- Fluid particles do **not** collide with a trigger shape, and a drain on a trigger does nothing (pass-through to the
  floor) [V: `trigger_on`, `drain_trigger`]. Control without trigger rests on the box (y 0.207) [V].
- Location [A]: no trigger check in the pair gate `0x10105e80`. Shape bits `& 7` are tested in `Fluid_vf8` (0x10100300)
  and in the shape registration code; triggers are most likely never registered as fluid pairs.

**3. Pair-gating flags, all checked in `0x10105e80` (called from `FluidElementRbElementInteraction_vf0`, 0x10106150)** [V unless noted]
The pair is created only when the function returns 1. Skip if any of these is set:

| Flag | Where | Probe |
|---|---|---|
| NX_SF_DISABLE_COLLISION (1<<4) on shape | `shape+0x15c & 0x10` | pass-through [V] |
| NX_SF_FLUID_DISABLE_COLLISION (1<<10) on shape | `& 0x400` | pass-through [V] |
| NX_SF_DISABLE_RESPONSE (1<<12) on shape | `& 0x1000` | pass-through [V] (also gates the pair, not only the response) |
| NX_AF_DISABLE_COLLISION (1<<0) on actor | actor flags via vtable +0x204, `& 1` | pass-through [V] |
| NX_AF_DISABLE_RESPONSE (1<<1) on actor | same, `& 2` | pass-through [V] |
| NX_AF_FLUID_DISABLE_COLLISION (1<<3) on actor | same, `& 8` | pass-through [V] |

Then the collision-method check [A for the mapping, decompile + header values]: static shape (no body, `shape+0x134 == 0`)
needs `collisionMethod & NX_F_STATIC`; body shape needs `& NX_F_DYNAMIC` (fluid `+0xc4` vtable call). Not probed.

- **Scene group filter:** `scene->setGroupCollisionFlag(g_fluid, g_shape, false)` removes the fluid–shape pair [V:
  shape group 1 with flag (0,1) off passes through]. The fluid's own collisionGroup applies to **all** statics: with fluid
  group 1 and that flag off, particles also pass the plane (fell to y = -9) [V].
- **groupsMask (128-bit):** no effect on fluid–shape pairs for bits0 values tested: shape mask 2 with fluid mask 1
  (disjoint) and with fluid mask 3 (overlap) both collide [V, bits0 only]. Not used in the pair gate [A].
- **Actor compartment (NxActorDesc.compartment, NX_SCT_RIGIDBODY):** a static actor placed in a rigid-body compartment is
  invisible to the fluid: box in RB compartment → particles fall through it and rest on the plane; plane in RB
  compartment → particles rest on the box [V]. The exclusion check location is not located [A].
- **Fluid compartment (NX_SCT_FLUID):** no effect on collision with normal statics [V, api_life vs api_life_comp, earlier notes].
- **NX_SF_FLUID_TWOWAY (1<<11):** not a pair gate; it selects the reaction on the body (dynamic shapes only). See
  `re-notes-fluid-mesh.md` 2.2 and the `probe_dyn` results [V there].

## Oracle keywords added (harness/src/fluid_oracle.cpp, scenario header comment)
- `shapeflag DISABLE_COLLISION | DISABLE_RESPONSE | TRIGGER_ENABLE` (plus the existing FLUID_*), on the last shape.
- `actorflag DISABLE_COLLISION | DISABLE_RESPONSE | FLUID_DISABLE_COLLISION`, on the actor of the last static shape.
- `shapegroup G`, `shapemask bits0`, `fluid.collisionGroup G`, `fluid.groupsMask bits0`, `groupcollision a b 0|1`.
- `rbcompartment` on the last static shape (its actor goes into a rigid-body compartment).

## Scenario map (`scenarios/fluid/probe_drain/`)
drain_ctrl / drain_on / drain_trigger / trigger_on; drain_prox_{0.0115,0.0125,0.012,0.013,0.015,0.018,0.01,0.005,0.001,0.02,0.04};
prox_ctrl_0.01; shape_disable_coll / shape_fluid_disable_coll / shape_disable_resp; actor_disable_coll /
actor_fluid_disable_coll / actor_disable_resp; grp_scene_off / grp_fluid_group_scene_off / grp_scene_on_control /
grp_mask_*; comp_box_rb / comp_plane_rb. Outputs in `captures/fluid/<name>/`.

## Implementation notes
- Build the pair set with: shape flags & 0x1410 (dis-coll, fluid-dis-coll, dis-resp) and 0x7 (trigger) out; actor flags
  & 0xb out; RB-compartment actors out; scene group filter on fluid group vs shape group.
- Drain: on proximity (d ≤ 2·cd, face distance) or contact, flag the particle in the step; remove it at the next step,
  after its response in the flagged step has been applied.
