# Content scan, maps part 2

Historical content audit, third group of 226 `.me1` maps under `CookedPC/Maps`. No parse failures or timeouts. The read-only scan used `tools/ue3pkg.py` plus a local preamble-aware decoder. Later decoder and placement corrections are summarized in [GAPS.md](GAPS.md).

Method notes [V]:
- Only tagged properties are stored per object; inherited values are resolved through the archetype chain (component
  template, class default object in Engine.u / TdGame.u, or the same-package archetype).
- Some component exports (SkeletalMeshComponent, TdSkeletalMeshComponent) carry an extra preamble before the first tag
  that `ue3pkg.export_props` does not skip; the local decoder finds the tag start (4 or 12 bytes after NetIndex).
  Actors (SkeletalMeshActor) and SkeletalMesh assets with native data after the tags are handled separately or ignored
  (see Open questions).

Parse status [V]: 226 parsed, 0 failures, 0 timeouts. Undecodable objects: 0 of the
SkeletalMesh, SkeletalMeshComponent, TdSkeletalMeshComponent and PhysXParticleSystem exports. Skipped by design: 275 actor
exports (SkeletalMeshActor, SkeletalMeshActorMAT, TdValveSkeletalMeshActor) whose body does not decode with the tag rules
above; they are used only as component owners (class name from the outer export).

Decoder check [V]: the preamble rule matches the non-map scan's independent decoder on every object of these
classes in the packages: SkeletalMeshComponent 322 objects at +4 bytes after NetIndex (instance), TdSkeletalMeshComponent
130 at +12 (template, all `Default__` weapon and bot templates), SkeletalMesh 158 and PhysXParticleSystem 37 with no preamble.

Raw per-object dumps, class schemas and one-off scan helpers are local-only and are not distributed.

Column convention for merging: `tagged` = stored in this map package; `eff` = effective value after the archetype
chain; `cdo-fallback` = not tagged on the chain, class default used; `absent` = not tagged anywhere, type default.

## Counts (this third, 226 maps)

"exports" = placed or template export objects in the maps (one per placement; a repeated name counts once per map).
"distinct" = distinct object full names [A: same name = same asset].

### A. SkeletalMesh assets (158 exports, 13 distinct cloth meshes, 22 cloth-tagged exports)

| item | exports | distinct | maps |
|---|---|---|---|
| bEnableClothPressure true / ClothPressure non-default | 0 | 0 | 0 |
| bEnableClothTwoWayCollision true / ClothCollisionResponseCoefficient non-default | 0 | 0 | 0 |
| bClothMetal true / any ClothMetal* tagged / ClothMetalImpulseThreshold non-default | 0 | 0 | 0 |
| ClothSpecialBones non-empty (attachment table) | 0 | 0 | 0 |
| ClothWeldedIndices / ClothMesh tagged | 0 | 0 | 0 |
| bEnableClothTearing true (all 9 have ClothTearFactor != 3.5; ClothTearReserve != 128 on 4) | 9 | 9 | 2 |
| bEnableClothSelfCollision true | 5 | 5 | 2 |
| bEnableClothLineChecks true | 10 | 10 | 2 |
| cloth-tagged SkeletalMesh exports (ClothBones with 1 entry on all) | 22 | 13 | 12 |
| ValidBounds on SkeletalMesh | not a declared property of SkeletalMesh (checked in Engine.u) | | |

### B. SkeletalMeshComponent family (322 instances in 39 maps, 130 templates)

| item | exports | maps |
|---|---|---|
| bEnableClothSimulation true (instances; all tagged) | 35 | 12 |
| bAutoFreezeClothWhenNotRendered true (instances; all tagged, same objects) | 35 | 12 |
| bDisableClothCollision true (all tagged; all on P_Flags.SK_Flag_02) | 13 | 9 |
| bClothFrozen true | 0 | 0 |
| bEnableValidBounds true (ValidBoundsMin/Max stay at the class default +-500) | 0 | 0 |
| bAttachClothVertsToBaseBody true | 0 | 0 |
| ClothRBChannel non-default (all instances and templates are RBCC_Cloth by default) | 0 | 0 |
| ClothRBCollideWithChannels tagged (Default+Pawn: 21; Default only: 9) | 30 | 12 |
| ClothAttachmentTearFactor non-default / ClothBlendWeight non-default | 0 / 1 (0.8: Factory_Roof_Spt.me1, SkeletalMeshActor_5.SkeletalMeshComponent_1) | 0 / 1 |
| Template components (TdSkeletalMeshComponent, weapon and bot `Default__` templates): any cloth flag true | 0 | 0 |

Owners of the instances: SkeletalMeshActor 295, SkeletalMeshActorMAT 15, TdValveSkeletalMeshActor 12 (outer export class).

### C. PhysXParticleSystem (37 exports, 21 maps, none is a template)

| item | exports | maps |
|---|---|---|
| CollisionResponseCoefficient != 0.2 (eff; none tagged) | 0 | 0 |
| bTwoWayCollision true | 0 | 0 |
| CollisionResponseCoefficient != 0.2 and bTwoWayCollision (both) | 0 | 0 |
| SimulationMethod: SPH 7, NO_PARTICLE_INTERACTION 25, MIXED 5 (12 tagged, rest from CDO) | 37 | 21 |
| bStaticCollision false | 1 | 1 |
| bDynamicCollision false | 1 | 1 |
| bDisableGravity true | 8 | 6 |

### D. Kismet / script objects (in maps)

| item | result |
|---|---|
| objects with class name containing Cloth or PhysXGate | 1 (SeqAct_TdPhysXGate, Factory_Ren.me1, Main_Sequence; only generic Kismet props, no cloth or fluid target seen) |
| name tables of the 226 maps containing SetClothValidBounds, EnableClothValidBounds, SetClothFrozen, SetClothFlags, SetClothPosition, ResetClothVertsToRefPose, AttachClothToCollidingShapes, SetAttachClothVertsToBaseBody, SetEnableClothSimulation, SetClothPressure, UpdateClothParams, AddImpulseAtClothPos, SetClothExternalForce, PhysXGate | 0 of 14 names in any map |
| cloth-related property names present in map name tables (maps) | bEnableClothSimulation 12, ClothRBCollideWithChannels 12, bDisableClothCollision 9, bEnableClothTearing 2, bEnableClothSelfCollision 2, bForceApplyToCloth 4 (owner not traced) |

## Per-asset lists for non-default hits

### W/M paths (the ones that matter for the gap list)

Disable collision (`bDisableClothCollision` = True, tagged). All 13 are `P_Flags.SK_Flag_02` components on
`SkeletalMeshActor` (name `P_Buildings.FlagPole_01...` or `P_Buildings.FlagMounted_01...`) [V]:

| map | component (object) | mesh | RBCollideWithChannels tagged |
|---|---|---|---|
| Convoy_Chase-Mall_Slc_Lw.me1 | P_Buildings.FlagPole_01.PF_FlagPole_01.PF_FlagPole_01_1Arc0.SkeletalMeshComponent0 | P_Flags.SK_Flag_02 | Default |
| Convoy_Conv_Lw.me1 | P_Buildings.FlagMounted_01.PF_FlagMounted_01.PF_FlagMounted_01_2Arc0.SkeletalMeshComponent0 | P_Flags.SK_Flag_02 | no |
| Convoy_Conv_Lw.me1 | P_Buildings.FlagPole_01.PF_FlagPole_01.PF_FlagPole_01_1Arc0.SkeletalMeshComponent0 | P_Flags.SK_Flag_02 | Default |
| Convoy_Mall_Lw.me1 | P_Buildings.FlagPole_01.PF_FlagPole_01.PF_FlagPole_01_1Arc0.SkeletalMeshComponent0 | P_Flags.SK_Flag_02 | Default |
| Convoy_Roof_Lw.me1 | P_Buildings.FlagMounted_01.PF_FlagMounted_01.PF_FlagMounted_01_2Arc0.SkeletalMeshComponent0 | P_Flags.SK_Flag_02 | no |
| Convoy_Roof_Lw.me1 | P_Buildings.FlagPole_01.PF_FlagPole_01.PF_FlagPole_01_1Arc0.SkeletalMeshComponent0 | P_Flags.SK_Flag_02 | Default |
| Convoy_Snipe_Lw.me1 | P_Buildings.FlagPole_01.PF_FlagPole_01.PF_FlagPole_01_1Arc0.SkeletalMeshComponent0 | P_Flags.SK_Flag_02 | Default |
| Factory_Pursu.me1 | P_Buildings.FlagPole_01.PF_FlagPole_01.PF_FlagPole_01_1Arc0.SkeletalMeshComponent0 | P_Flags.SK_Flag_02 | Default |
| Factory_Pursu_Bac.me1 | P_Buildings.FlagMounted_01.PF_FlagMounted_01.PF_FlagMounted_01_2Arc0.SkeletalMeshComponent0 | P_Flags.SK_Flag_02 | no |
| Factory_Pursu_Bac.me1 | P_Buildings.FlagPole_01.PF_FlagPole_01.PF_FlagPole_01_1Arc0.SkeletalMeshComponent0 | P_Flags.SK_Flag_02 | Default |
| Factory_Roof.me1 | P_Buildings.FlagMounted_01.PF_FlagMounted_01.PF_FlagMounted_01_2Arc0.SkeletalMeshComponent0 | P_Flags.SK_Flag_02 | no |
| Factory_Roof.me1 | P_Buildings.FlagPole_01.PF_FlagPole_01.PF_FlagPole_01_1Arc0.SkeletalMeshComponent0 | P_Flags.SK_Flag_02 | Default |
| Factory_Roof_Bac.me1 | P_Buildings.FlagPole_01.PF_FlagPole_01.PF_FlagPole_01_1Arc0.SkeletalMeshComponent0 | P_Flags.SK_Flag_02 | Default |

All 13 have bEnableClothSimulation = True (tagged). This agrees with the non-map scan ([content-pkgs.md](content-pkgs.md), `Props/P_Buildings.upk`), which reports the same flag on P_Flags.SK_Flag_02 components.

Other W/M paths, zero hits in this third [V]: pressure, two-way (mesh and fluid), metal / attachToCore inputs, valid bounds
(component and mesh), frozen (bClothFrozen), attach-verts-to-base-body, fluid collisionResponseCoefficient != 0.2.

### Other cloth flags (not on the W/M list, for the gap table)

Tearing (`bEnableClothTearing` = True; mesh, TearFactor and TearReserve non-default):

| map | mesh | TearFactor | TearReserve | self-collision |
|---|---|---|---|---|
| Boat_PDeck_Spt.me1 | P_Cloth2.BoatCloth_01.PX_SK_BoatCloth_02 | 2.0 | 512 | no |
| Convoy_Chase_Spt.me1 | P_Cloth2.ConvoyCloth_01.PX_SK_ConvoyCloth_01 | 2.0 | 128 | no |
| Convoy_Chase_Spt.me1 | P_Cloth2.ConvoyCloth_01.PX_SK_ConvoyCloth_02 | 1.5 | 256 | no |
| Convoy_Chase_Spt.me1 | P_Cloth2.ConvoyCloth_01.PX_SK_ConvoyCloth_03 | 2.0 | 128 | no |
| Convoy_Chase_Spt.me1 | P_Cloth2.ConvoyCloth_01.PX_SK_ConvoyCloth_04 | 2.0 | 128 | no |
| Convoy_Chase_Spt.me1 | P_Cloth2.PlasticSheet_01.PX_SK_PlasticSheet_01_Tearable | 1.6 | 512 | yes |
| Convoy_Chase_Spt.me1 | P_Cloth2.PlasticSheet_01.PX_SK_PlasticSheet_03_Tearable | 2.0 | 256 | yes |
| Convoy_Chase_Spt.me1 | P_Cloth2.PlasticSheet_01.PX_SK_PlasticSheet_04_Tearable | 1.9 | 128 | yes |
| Convoy_Chase_Spt.me1 | P_Cloth2.PlasticSheet_01.PX_SK_PlasticSheet_06_Tearable | 1.9 | 128 | yes |

Self-collision also on P_Cloth.Meshes.35x1_SP02_del (Factory_Roof_Spt.me1), not tearing.

Cloth meshes with neither tearing nor self-collision: P_Cloth2.BoatCloth_01.PX_SK_BoatCloth_01 (Boat_PDeck_Spt.me1),
P_Flags.SK_Flag_01 (Factory_Roof.me1, Factory_Roof_Spt.me1), P_Flags.SK_Flag_02 (9 maps).

Cloth components enabled (bEnableClothSimulation = True, all with ClothRBChannel = RBCC_Cloth by default):

| mesh | instances | maps |
|---|---|---|
| P_Flags.SK_Flag_02 | 13 | 9 |
| P_Cloth2.BoatCloth_01.PX_SK_BoatCloth_01 | 4 | 1 (Boat_PDeck_Spt) |
| P_Cloth2.BoatCloth_01.PX_SK_BoatCloth_02 | 4 | 1 (Boat_PDeck_Spt) |
| P_Cloth2.ConvoyCloth_01.PX_SK_ConvoyCloth_01..04 | 2 + 2 + 3 + 1 | 1 (Convoy_Chase_Spt) |
| P_Cloth2.PlasticSheet_01.PX_SK_PlasticSheet_01/04/06_Tearable | 2 + 1 + 1 | 1 (Convoy_Chase_Spt) |
| P_Flags.SK_Flag_01 | 1 | 1 (Factory_Roof) |
| P_Cloth.Meshes.35x1_SP02_del | 1 | 1 (Factory_Roof_Spt) |

Note: the 13 SK_Flag_02 bDisableClothCollision placements are the only components that disable collision; the flag
does not appear on any cloth component in the Boat, Convoy or Factory_Roof_Spt maps.

### Fluid (PhysXParticleSystem, 37 exports)

Columns: map | object | SimulationMethod | bDisableGravity | bStaticCollision | bDynamicCollision | bTwoWayCollision.
All have CollisionResponseCoefficient 0.2 (CDO) and bTwoWayCollision false.

| map | object | method | noGrav | static | dyn | two-way |
|---|---|---|---|---|---|---|
| Boat_Chase_Spt.me1 | PX_LevelEffects.Fluid.SteamSmokeGround | SPH | F | T | T | F |
| Boat_Deck_Spt.me1 | PX_LevelEffects.Fluid.SteamSmokeGround | SPH | F | T | T | F |
| Boat_Harb.me1 | PX_LevelEffects.SteamSmoke | NO_PI | F | T | T | F |
| Boat_PDeck-Deck_Slc.me1 | PX_ImpactEffects.glass_slide | NO_PI | F | T | T | F |
| Boat_PDeck-Deck_Slc.me1 | PX_LevelEffects.Fluid.SteamSmokeGround | SPH | F | T | T | F |
| Boat_PDeck-Deck_Slc.me1 | PX_LevelEffects.SteamSmoke | NO_PI | F | T | T | F |
| Boat_PDeck.me1 | PX_LevelEffects.SteamSmoke | NO_PI | F | T | T | F |
| Boat_PDeck_Spt.me1 | PX_LevelEffects.Fluid.Dustmotes | MIXED | T | T | T | F |
| Convoy_Chase_Spt.me1 | PX_LevelEffects.Fluid.SP08_Convoy_Metal | NO_PI | F | T | T | F |
| Convoy_Conv-Snipe_Slc_Spt.me1 | PX_SlideTrashEffects.Slide_Heavy | NO_PI | F | T | T | F |
| Convoy_Conv_Spt.me1 | PX_SlideTrashEffects.Slide_Heavy | NO_PI | F | T | T | F |
| Convoy_Mall_Spt.me1 | PX_GlassEffects.Glass | NO_PI | F | T | T | F |
| Convoy_Mall_Spt.me1 | PX_GlassEffects.Glass_big | NO_PI | F | T | T | F |
| Convoy_Mall_Spt.me1 | PX_ImpactEffects.Leafs | NO_PI | F | T | T | F |
| Convoy_Roof_Spt.me1 | PX_SlideTrashEffects.Fluid.Heli_DownWash | NO_PI | F | T | T | F |
| Convoy_Snipe_Spt.me1 | PX_LevelEffects.Fluid.Dustmotes | MIXED | T | T | T | F |
| Convoy_Snipe_Spt.me1 | PX_LevelEffects.Fluid.FlyingTrash_FaithPush | SPH | F | T | T | F |
| Convoy_Snipe_Spt.me1 | PX_LevelEffects.Fluid.FlyingTrash_Light | NO_PI | F | T | T | F |
| Factory_Pursu_Spt.me1 | PX_GlassEffects.Glass | NO_PI | F | T | T | F |
| Factory_Pursu_Spt.me1 | PX_GlassEffects.Glass_big | NO_PI | F | T | T | F |
| Factory_Ren.me1 | PX_LevelEffects.Fluid.Dustmotes | MIXED | T | T | T | F |
| Factory_Ren.me1 | PX_SlideTrashEffects.Slide_Heavy | NO_PI | F | T | T | F |
| Scraper_Deck_Spt.me1 | PX_GlassEffects.Glass | NO_PI | F | T | T | F |
| Scraper_Deck_Spt.me1 | PX_GlassEffects.Glass_big | NO_PI | F | T | T | F |
| Scraper_Heli_Spt.me1 | PX_LevelEffects.Fluid.SP09_ExploHeliDebris | NO_PI | F | T | **F** | F |
| Scraper_Mill_Spt.me1 | PX_GlassEffects.Glass | NO_PI | F | T | T | F |
| Scraper_Mill_Spt.me1 | PX_GlassEffects.Glass_big | NO_PI | F | T | T | F |
| Scraper_Mill_Spt.me1 | PX_LevelEffects.Fluid.Sparks | NO_PI | F | T | T | F |
| Scraper_Mill_Spt.me1 | PX_LevelEffects2.GroundFog_high_movement | MIXED | T | T | T | F |
| Scraper_Out-Deck_Spt.me1 | PX_LevelEffects.Fluid.Embers | NO_PI | T | T | T | F |
| Scraper_Out-Deck_Spt.me1 | PX_LevelEffects.Fluid.SP09_CoorExplo_Smoke | SPH | F | T | T | F |
| Scraper_Out_Spt.me1 | PX_LevelEffects.GroundFog | NO_PI | T | **F** | T | F |
| Scraper_Out_Spt.me1 | PX_LevelEffects.SteamSmoke | NO_PI | F | T | T | F |
| Scraper_Out_Spt.me1 | PX_LevelEffects2.GroundFog_high_movement | MIXED | T | T | T | F |
| Scraper_Out_Spt.me1 | PX_LevelEffects2.GroundFog_high_movement_nograv | NO_PI | T | T | T | F |
| Scraper_Shaft.me1 | PX_LevelEffects.Fluid.SP09Shaft_SteamSmokeGround | SPH | F | T | T | F |
| Scraper_Shaft_Spt.me1 | PX_LevelEffects.Fluid.SP09Shaft_SteamSmokeGround | SPH | F | T | T | F |

Method note: SPH 7 + MIXED 5 = 12 of 37 rows; the 25 NO_PI rows are collide-only.

### Kismet

Only `SeqAct_TdPhysXGate` (Factory_Ren.me1, TheWorld.PersistentLevel.Main_Sequence.SeqAct_TdPhysXGate_0) [V]. It has
no cloth or fluid parameter tagged that I could map to a native call; its meaning is not traced [A].

## Summary

- In this third (226 maps) the W/M rows of the gap list (rows 4, 5, 7, 8, 9 and the fluid coefficient, row 1) have no
  hits except row 5 (below): no mesh sets pressure, two-way, metal/attachToCore or specials; no component sets valid
  bounds, frozen or attach-to-base-body; no fluid sets a collision response other than 0.2 or two-way collision.
- The one W path that content does reach here is `bDisableClothCollision` on 13 `SK_Flag_02` placements in 9 maps
  (tagged True on each). Those components are cloth-enabled (bEnableClothSimulation True), so they run with cloth
  collision off.
- Tearing (9 meshes in 2 maps) and self-collision (5 meshes in 2 maps) are on, with non-default tear factors on all
  tearing meshes. These are not in the W/M list but are the cloth paths this third actually exercises.
- 37 fluids in 21 maps: SPH 7, MIXED 5, collide-only 25. Only two objects have a collision flag other than the CDO
  default.

## Open questions

1. Cloth pressure, two-way, metal and valid-bounds content may exist in the other two thirds; this result only covers
   the third group of maps.
2. Actor-level data (SkeletalMeshActor / SkeletalMeshActorMAT / TdValveSkeletalMeshActor bodies, 275 exports) do not
   decode with the tag rules here; they are not needed for the component counts, but any actor-level cloth property
   would be missed.
3. `bForceApplyToCloth` appears in 4 map name tables; the owner class is not traced [A].
4. `ClothRBCollideWithChannels` is set on 30 components (Default, plus Pawn on 21). The absent case is taken as the
   struct default from the class (likely all channels off) [A]. Confirm in the exe struct defaults.
5. Mesh identity: distinct SkeletalMesh objects are counted by full name; no content hash was compared across maps [A].
6. Whether `bDisableClothCollision` on SK_Flag_02 is ever changed at run time (script set, or a Kismet toggle) is not
   answered by this scan; the Kismet part shows no cloth action in these maps (step 4 counters).
7. Whether the SeqAct_TdPhysXGate in Factory_Ren.me1 drives any fluid or cloth at run time is not traced.
