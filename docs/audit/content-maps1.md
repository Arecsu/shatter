# Content scan, maps part 1: cloth, PhysX fluids and PhysX gates (audit step 2b)

Historical content audit, second group of 227 maps under `TdGame/CookedPC/Maps`. Parse failures: none. Timeouts: none (per-package timeout 900 s, six workers, total scan time 36 s). The tables use the same columns as the other map groups.

Scope: SkeletalMesh exports, SkeletalMeshComponent and TdSkeletalMeshComponent exports (any subclass name ending in
SkeletalMeshComponent), PhysXParticleSystem exports, and objects whose class name contains Cloth or PhysXGate. Script-package name tables are covered by [content-pkgs.md](content-pkgs.md).

## Method

- Reader: `tools/ue3pkg.py`, unchanged, plus a wrapper (scratch `content-maps1/decode.py`). [V]
- Tagged properties: the export body starts with an int prefix before the first tag. The prefix length differs by object:
  4 bytes for PhysXParticleSystem exports and class defaults, 8 bytes for map components, 16 bytes for component templates
  inside Engine.u / TdGame.u (`Default__<Class>.<Component>`). The wrapper tries offsets 4, 8, 0, 12, then 16 to 64, and
  accepts only a parse in which every tag has a known type name and the body ends at a `None` tag. 0 objects failed. [V]
- `ue3pkg.export_props` is wrong for these objects: on map components it reads the first tag 4 bytes early and returns
  names such as `ByteProperty_993` (checked on Cranes_Puzz_Spt.me1). `tools/physx_asset_inventory.py` uses it, so its
  component and cloth-mesh results should be re-checked. [V on these 4 components; A for the rest of the packages]
- Effective values: the archetype chain (an import or export), or the class default object `Default__<Class>` when the export
  has no archetype. Own tags override. A value marked `inh` is inherited; an absent bool/int/float in the whole chain is the
  zero value (false, 0). Archetype resolution succeeded for all 417 map components, and the class default was used for
  176 SkeletalMesh, 34 PhysXParticleSystem, 21 gate and 6 TdSkeletalMeshComponent objects. [V]
- Declared properties were read from the property export chains of SkeletalMesh, SkeletalMeshComponent,
  PhysXParticleSystem (Engine.u) and TdSkeletalMeshComponent, SeqAct_TdPhysXGate (TdGame.u)
  (scratch `content-maps1/declared_props.txt`). Declared bools never tagged in any map are listed as "absent" below. [V]
- Cross-check: the non-map scan's independent decoder returns the
  same (name, value) list as the decoder above for all 423 map components. Only the SkelMeshGUID struct representation differs
  on the SkeletalMesh exports. [V]
- Raw per-object dumps and one-off scan helpers are local-only and are not distributed. Later decoder and placement corrections are summarized in [GAPS.md](GAPS.md).

## Counts

Columns: distinct assets = distinct object names (SkeletalMesh exports are carried by the map packages, so a name can recur
in several maps); exports / instances = objects in these 227 maps; maps = distinct maps containing a hit.

| item | distinct assets | exports / instances | maps | note |
|---|---|---|---|---|
| SkeletalMesh exports in maps | 69 | 176 | 49 | all have CDO cloth defaults (ClothPressure 1.0 etc.) |
| cloth data present (ClothBones non-empty, own tag) | 32 | 43 | 21 | ClothIndexBuffer, ClothToGraphicsVertMap, ClothWeldingDomain tagged on the same 43 |
| bEnableClothPressure true | 0 | 0 | 0 | declared, never tagged; ClothPressure never tagged (CDO 1.0) |
| bEnableClothTwoWayCollision true | 0 | 0 | 0 | declared, never tagged; ClothCollisionResponseCoefficient never tagged |
| bClothMetal true / any ClothMetal* tagged | 0 | 0 | 0 | ClothMetalImpulseThreshold CDO 10.0 only |
| ClothSpecialBones non-empty (attachment table) | 0 | 0 | 0 | declared array, never tagged |
| bEnableClothTearing true | 13 | 15 | 5 | ClothTearFactor or ClothTearReserve tagged on 16 exports (ClothTearFactor 1.2 to 2.5, ClothTearReserve 256 or 512) |
| bEnableClothSelfCollision true | 12 | 14 | 7 | |
| bEnableClothLineChecks true | 21 | 23 | 8 | |
| bEnableClothBendConstraints or bEnableClothDamping true | 8 | 17 | 14 | |
| bEnableClothOrthoBendConstraints / bUseClothCOMDamping / bForceNoWelding true | 0 | 0 | 0 | declared, never tagged |
| SkeletalMeshComponent + TdSkeletalMeshComponent exports | - | 423 | 52 | 336 SkeletalMeshComponent, 87 TdSkeletalMeshComponent |
| bEnableClothSimulation true | - | 107 | 21 | owner is SkeletalMeshActor in all 107; 81 of them also tag bAutoFreezeClothWhenNotRendered |
| bDisableClothCollision true | 2 | 8 | 7 | all on flag meshes (P_Flags.SK_Flag_02, P_Buildings flag poles); bEnableClothSimulation true on all 8 |
| bClothFrozen true | 0 | 0 | 0 | declared, never tagged |
| bAttachClothVertsToBaseBody true | 0 | 0 | 0 | declared, never tagged |
| bEnableValidBounds true | 5 | 5 | 1 | Subway_Roof-RenCo_Spt only; bounds +-800 (CDO +-500) |
| bAutoFreezeClothWhenNotRendered true | - | 81 | 18 | 26 cloth components use the default (false) |
| bUseCompartment true | - | 86 | - | 86 of the 107 cloth-enabled components; the other 21 have no tag |
| ClothRBChannel differs from template value RBCC_Cloth | 0 | 0 | 0 | all 423 resolve to RBCC_Cloth |
| ClothRBCollideWithChannels tagged | - | 98 | 15 | most common tagged sets: {Default, Pawn} (79), {Default, Pawn, Untitled4} (6), {Pawn} (6) |
| ClothAttachmentTearFactor differs from CDO (-1) | 0 | 0 | 0 | |
| ClothBlendWeight tagged != 1.0 | - | 7 | 2 | values 0.1, 0.3, 0.7 (Subway_MoPu_Spt, Subway_Ren_Spt) |
| ClothMin/MaxBlendDistance tagged | - | 64 | 9 | not in GAPS.md, the exe cloth docs or the shim (open question 2) |
| ClothWind tagged | - | 94 | 19 | |
| ClothForceScale tagged | - | 91 | 12 | 74 of them 1.0 |
| PhysXParticleSystem exports | 14 | 34 | 24 | 14 distinct configs |
| CollisionResponseCoefficient != 0.2 | 0 | 0 | 0 | all 34 inherit the CDO 0.2; none tagged |
| bTwoWayCollision true | 0 | 0 | 0 | absent in all 34 (declared) |
| bTwoWayCollision true and CRC != 0.2 | 0 | 0 | 0 | |
| SimulationMethod (own tag) | - | 6 | - | SPH 4 (4 configs), MIXED_MODE 2 (1 config); 28 inherit ESM_NO_PARTICLE_INTERACTION |
| bStaticCollision false | 1 | 2 | 2 | PX_LevelEffects.GroundFog (Factory_Facto_Spt, Factory_Lbay) |
| bDynamicCollision false | 0 | 0 | 0 | CDO true everywhere |
| bDisableGravity true | 2 | 4 | 4 | Dustmotes (MIXED, 2 maps) and GroundFog (2 maps) |
| SeqAct_TdPhysXGate exports | - | 21 | 4 | Mall_Mall_Lw 18, Cranes_Puzz-Off_Building 1, Subway_Roof-RenCo_Spt 1, Mall_HW_Spt 1 |
| bPhysXNotGate true | 0 | 0 | 0 | absent (false) in all 21; only links are `In` and `Out` |
| classes with Cloth in the name | - | 0 | 0 | no cloth Kismet class is placed in these maps |

## What the W/M paths see in these maps

- W row 4 (valid bounds): reached. 5 components in Subway_Roof-RenCo_Spt.me1, each on a tearing cloth mesh, with bounds
  +-800 instead of the CDO +-500 (table below). [V]
- W row 5 (live DISABLE_COLLISION): the content sets bDisableClothCollision on 8 components (7 maps, all flags). Whether it is
  toggled at run time was not checked here. [V on the data]
- M rows 7, 8, 9 (pressure, two-way collision, attachToCore/metal) and W row 1 (fluid collisionResponseCoefficient), and the
  fluid two-way flag: no map instance sets any of them. [V on maps; other packages are not in this scan]
- Frozen (bClothFrozen) and base body (bAttachClothVertsToBaseBody): no map instance. [V]
- Fluids: every map fluid uses the CDO collision response (0.2). The SPH and MIXED fluids in maps are 6 instances of 5
  configs (4 SPH configs, 1 MIXED config), the other 28 instances are collide-only. [V]
- Kismet: no map contains a cloth action class. The 21 PhysX gates have no property beyond bPhysXNotGate (false) and the
  generic links; what the gate tests is not in the map data. [V on data; A that it is the PhysX-enhanced check]
- Cloth features in use that are not W/M rows: tearing (13 assets), self-collision (12), line checks (21), wind (ClothWind on
  94 components; the shim implements wind), per-component ClothForceScale (74 at 1.0), bUseCompartment (86 cloth components),
  auto-freeze when not rendered (81 components). These are join keys for step 5 and for GAPS rows 11, 26 and 27.

## Per-asset and per-instance lists

### Cloth meshes in maps (SkeletalMesh exports with ClothBones, one row per distinct asset)

| asset (SkeletalMesh) | maps | exports | tearing | self-coll | line checks | bend constr. | damping | ClothTearFactor (tagged) | ClothTearReserve (tagged) | welding domain | cloth-enabled components |
|---|---|---|---|---|---|---|---|---|---|---|---|
| P_Cloth2.PaperStrips_01.PX_SK_PaperStrip_04 | 1 | 1 | 0 | 0 | 0 | 0 | 0 | - | - | 32 x1 | 21 |
| P_Cloth2.SubwayCloth_01.PX_SK_SubwayCloth_01 | 1 | 1 | 1 | 1 | 1 | 0 | 0 | 2 x1 | 364 x1 | 445 x1 | 12 |
| P_Cloth.Meshes.5x1_460vert | 1 | 1 | 0 | 1 | 1 | 0 | 1 | - | - | 460 x1 | 11 |
| P_Maintenance.PX_PlasticDivider_01.PX_SK_PlasticDividerMall_01 | 1 | 1 | 0 | 0 | 0 | 0 | 1 | - | - | 168 x1 | 9 |
| P_Flags.SK_Flag_02 | 7 | 7 | 0 | 0 | 0 | 7 | 0 | - | - | 6 x7 | 8 |
| P_Cloth.Meshes.5x1_SP05_Del_Stretch_Tearing | 1 | 1 | 1 | 1 | 1 | 0 | 0 | 1.2 x1 | 256 x1 | 328 x1 | 6 |
| P_Cloth2.PlasticSheet_01.PX_SK_PlasticSheet_01 | 1 | 1 | 0 | 0 | 1 | 0 | 0 | - | - | 288 x1 | 4 |
| P_Cloth.Meshes.Flag_650vert_2lock | 1 | 1 | 0 | 1 | 1 | 0 | 0 | - | - | 651 x1 | 3 |
| P_Cloth2.PlasticSheet_01.PX_SK_PlasticSheet_02_Tearable | 2 | 2 | 2 | 2 | 2 | 0 | 0 | 2 x2 | 512 x2 | 989 x2 | 3 |
| P_Maintenance.PX_PlasticDivider_01.PX_SK_PlasticDividerFactory_01 | 1 | 1 | 0 | 0 | 0 | 0 | 1 | - | - | 96 x1 | 3 |
| P_Cloth.Meshes.Crane_Tarp_tearing_fullsides | 1 | 1 | 1 | 0 | 1 | 1 | 1 | 2.5 x1 | 512 x1 | 961 x1 | 2 |
| P_Cloth2.PaperStrips_01.PX_SK_PaperStrip_03 | 1 | 1 | 0 | 0 | 0 | 1 | 1 | - | - | 20 x1 | 2 |
| P_Cloth.Meshes.Crane_Square_Tearing_high | 1 | 1 | 1 | 0 | 1 | 0 | 0 | 2.4 x1 | 512 x1 | 1680 x1 | 2 |
| P_Cloth2.PlasticSheet_01.PX_SK_PlasticSheet_02 | 1 | 1 | 0 | 0 | 1 | 0 | 0 | - | - | 289 x1 | 2 |
| P_Cloth2.WarningStripeCloth_01.PX_SK_WarningStripeCloth_01 | 1 | 1 | 0 | 0 | 0 | 0 | 0 | - | - | 53 x1 | 2 |
| P_Buildings.PX_Awning_01.PX_SK_Awning_02 | 1 | 1 | 0 | 0 | 0 | 0 | 0 | - | - | 272 x1 | 1 |
| P_Cloth2.PaperStrips_01.PX_SK_PaperStrip_01 | 1 | 1 | 0 | 0 | 0 | 0 | 0 | - | - | 10 x1 | 1 |
| P_Cloth2.PaperStrips_01.PX_SK_PaperStrip_02 | 1 | 1 | 0 | 0 | 0 | 0 | 0 | - | - | 22 x1 | 1 |
| P_Cloth2.PlasticSheet_01.PX_SK_PlasticSheet_01_Tearable | 2 | 2 | 2 | 2 | 2 | 0 | 0 | 1.6 x2 | 512 x2 | 986 x2 | 1 |
| P_Cloth2.PlasticSheet_01.PX_SK_PlasticSheet_05_Tearable | 1 | 1 | 1 | 1 | 1 | 0 | 0 | 2 x1 | 256 x1 | 491 x1 | 1 |
| P_Cloth2.PlasticSheet_01.PX_SK_PlasticSheet_05 | 1 | 1 | 0 | 0 | 1 | 0 | 0 | - | - | 152 x1 | 1 |
| P_Cloth2.PlasticSheet_01.PX_SK_PlasticSheet_06 | 1 | 1 | 0 | 0 | 1 | 0 | 0 | 2.2 x1 | - | 442 x1 | 1 |
| P_Flags.SK_Flag_01 | 4 | 4 | 0 | 0 | 0 | 4 | 0 | - | - | 84 x4 | 1 |
| P_Cloth.Meshes.5x1_460vert_tearing | 1 | 1 | 1 | 0 | 1 | 0 | 0 | 2 x1 | 256 x1 | 460 x1 | 1 |
| P_Cloth2.ClothMall_01.PX_SK_ClothMall_01 | 1 | 1 | 1 | 1 | 1 | 0 | 0 | 2.2 x1 | 512 x1 | 837 x1 | 1 |
| P_Cloth2.ClothMall_01.PX_SK_ClothMall_02 | 1 | 1 | 1 | 0 | 1 | 0 | 0 | 1.7 x1 | 512 x1 | 985 x1 | 1 |
| P_Cloth2.ClothMall_01.PX_SK_ClothMall_03 | 1 | 1 | 0 | 0 | 1 | 0 | 0 | - | - | 49 x1 | 1 |
| P_Cloth2.PlasticSheet_01.PX_SK_PlasticSheet_03_Tearable | 1 | 1 | 1 | 1 | 1 | 0 | 0 | 2 x1 | 256 x1 | 493 x1 | 1 |
| P_Cloth2.PlasticSheet_01.PX_SK_PlasticSheet_04_Tearable | 1 | 1 | 1 | 1 | 1 | 0 | 0 | 1.9 x1 | - | 442 x1 | 1 |
| P_Cloth2.PlasticSheet_01.PX_SK_PlasticSheet_06_Tearable | 1 | 1 | 1 | 1 | 1 | 0 | 0 | 1.9 x1 | - | 442 x1 | 1 |
| P_Maintenance.PX_PlasticDivider_01.PX_SK_PlasticDividerMall_02 | 1 | 1 | 0 | 0 | 0 | 0 | 1 | - | - | 42 x1 | 1 |
| P_Maintenance.PX_PlasticDivider_01.PX_SK_PlasticDivider_02 | 1 | 1 | 0 | 1 | 1 | 0 | 0 | - | - | 384 x1 | 0 |

### Cloth-enabled components (bEnableClothSimulation true): instances by mesh, and by map

| map | components | meshes |
|---|---|---|
| Mall_MallLoadingBay_Spt.me1 | 21 | P_Cloth2.PaperStrips_01.PX_SK_PaperStrip_04 x21 |
| Mall_Mall_Spt.me1 | 15 | P_Cloth.Meshes.5x1_SP05_Del_Stretch_Tearing x6, P_Cloth2.PlasticSheet_01.PX_SK_PlasticSheet_02_Tearable x2, None x1, P_Cloth2.ClothMall_01.PX_SK_ClothMall_02 x1, P_Cloth2.PlasticSheet_01.PX_SK_PlasticSheet_04_Tearable x1, P_Cloth2.ClothMall_01.PX_SK_ClothMall_03 x1, P_Cloth2.ClothMall_01.PX_SK_ClothMall_01 x1, P_Cloth2.PlasticSheet_01.PX_SK_PlasticSheet_03_Tearable x1, P_Cloth2.PlasticSheet_01.PX_SK_PlasticSheet_06_Tearable x1 |
| Subway_Plat_Spt.me1 | 14 | P_Cloth2.SubwayCloth_01.PX_SK_SubwayCloth_01 x12, P_Cloth.Meshes.Crane_Tarp_tearing_fullsides x2 |
| Mall_HW_Spt.me1 | 12 | P_Cloth.Meshes.5x1_460vert x11, P_Cloth.Meshes.5x1_460vert_tearing x1 |
| Mall_R2-Mall_Slc_Spt.me1 | 10 | P_Maintenance.PX_PlasticDivider_01.PX_SK_PlasticDividerMall_01 x9, P_Maintenance.PX_PlasticDivider_01.PX_SK_PlasticDividerMall_02 x1 |
| Subway_Stat_Spt.me1 | 6 | P_Cloth2.PlasticSheet_01.PX_SK_PlasticSheet_01 x4, P_Cloth2.PlasticSheet_01.PX_SK_PlasticSheet_05 x1, P_Cloth2.PlasticSheet_01.PX_SK_PlasticSheet_06 x1 |
| Subway_Roof-RenCo_Spt.me1 | 5 | P_Cloth.Meshes.Crane_Square_Tearing_high x2, P_Cloth2.PlasticSheet_01.PX_SK_PlasticSheet_05_Tearable x1, P_Cloth2.PlasticSheet_01.PX_SK_PlasticSheet_01_Tearable x1, P_Cloth2.PlasticSheet_01.PX_SK_PlasticSheet_02_Tearable x1 |
| Subway_Ren_Spt.me1 | 4 | P_Cloth2.PaperStrips_01.PX_SK_PaperStrip_03 x2, P_Cloth2.PaperStrips_01.PX_SK_PaperStrip_02 x1, P_Cloth2.PaperStrips_01.PX_SK_PaperStrip_01 x1 |
| Subway_MoPu_Spt.me1 | 3 | P_Cloth.Meshes.Flag_650vert_2lock x3 |
| Factory_Facto_Spt.me1 | 3 | P_Maintenance.PX_PlasticDivider_01.PX_SK_PlasticDividerFactory_01 x3 |
| Subway_Roof_Spt.me1 | 2 | P_Cloth2.PlasticSheet_01.PX_SK_PlasticSheet_02 x2 |
| Mall_R1_Lw.me1 | 2 | P_Flags.SK_Flag_02 x2 |
| Factory_Facto-Arena_Spt.me1 | 2 | P_Cloth2.WarningStripeCloth_01.PX_SK_WarningStripeCloth_01 x2 |
| Cranes_Puzz-Off_Building.me1 | 1 | P_Buildings.PX_Awning_01.PX_SK_Awning_02 x1 |
| Subway_Bac.me1 | 1 | P_Flags.SK_Flag_02 x1 |
| Subway_BacRen.me1 | 1 | P_Flags.SK_Flag_02 x1 |
| Subway_BacSub.me1 | 1 | P_Flags.SK_Flag_02 x1 |
| Subway_Roof.me1 | 1 | P_Flags.SK_Flag_02 x1 |
| Mall_HW_Lw.me1 | 1 | P_Flags.SK_Flag_02 x1 |
| Mall_Mall_Lw.me1 | 1 | P_Flags.SK_Flag_01 x1 |
| Mall_R2_LW.me1 | 1 | P_Flags.SK_Flag_02 x1 |

### Components with bEnableValidBounds true (all instances)

| map | owner actor | component | mesh | ValidBoundsMin | ValidBoundsMax | own tag |
|---|---|---|---|---|---|---|
| Subway_Roof-RenCo_Spt.me1 | SkeletalMeshActor_0 | SkeletalMeshComponent_7 | P_Cloth.Meshes.Crane_Square_Tearing_high | [-800.0, -800.0, -800.0] | [800.0, 800.0, 800.0] | yes |
| Subway_Roof-RenCo_Spt.me1 | SkeletalMeshActor_1 | SkeletalMeshComponent_1 | P_Cloth2.PlasticSheet_01.PX_SK_PlasticSheet_05_Tearable | [-800.0, -800.0, -800.0] | [800.0, 800.0, 800.0] | yes |
| Subway_Roof-RenCo_Spt.me1 | SkeletalMeshActor_2 | SkeletalMeshComponent_1 | P_Cloth2.PlasticSheet_01.PX_SK_PlasticSheet_01_Tearable | [-800.0, -800.0, -800.0] | [800.0, 800.0, 800.0] | yes |
| Subway_Roof-RenCo_Spt.me1 | SkeletalMeshActor_4 | SkeletalMeshComponent_1 | P_Cloth2.PlasticSheet_01.PX_SK_PlasticSheet_02_Tearable | [-800.0, -800.0, -800.0] | [800.0, 800.0, 800.0] | yes |
| Subway_Roof-RenCo_Spt.me1 | SkeletalMeshActor_5 | SkeletalMeshComponent_7 | P_Cloth.Meshes.Crane_Square_Tearing_high | [-800.0, -800.0, -800.0] | [800.0, 800.0, 800.0] | yes |

### Components with bDisableClothCollision true (all instances)

| map | owner actor | component | mesh | bEnableClothSimulation | bClothFrozen |
|---|---|---|---|---|---|
| Subway_Bac.me1 | PF_FlagPole_01_1Arc0 | SkeletalMeshComponent0 | P_Flags.SK_Flag_02 | True | False |
| Subway_BacRen.me1 | PF_FlagPole_01_1Arc0 | SkeletalMeshComponent0 | P_Flags.SK_Flag_02 | True | False |
| Subway_BacSub.me1 | PF_FlagPole_01_1Arc0 | SkeletalMeshComponent0 | P_Flags.SK_Flag_02 | True | False |
| Subway_Roof.me1 | PF_FlagMounted_01_2Arc0 | SkeletalMeshComponent0 | P_Flags.SK_Flag_02 | True | False |
| Mall_HW_Lw.me1 | PF_FlagMounted_01_2Arc0 | SkeletalMeshComponent0 | P_Flags.SK_Flag_02 | True | False |
| Mall_R1_Lw.me1 | PF_FlagMounted_01_2Arc0 | SkeletalMeshComponent0 | P_Flags.SK_Flag_02 | True | False |
| Mall_R1_Lw.me1 | PF_FlagPole_01_1Arc0 | SkeletalMeshComponent0 | P_Flags.SK_Flag_02 | True | False |
| Mall_R2_LW.me1 | PF_FlagMounted_01_2Arc0 | SkeletalMeshComponent0 | P_Flags.SK_Flag_02 | True | False |

### PhysXParticleSystem per distinct config (absent = declared bool with no tag, read as false)

| PhysXParticleSystem (distinct object) | maps | instances | SimulationMethod | bStaticCollision | bDynamicCollision | bTwoWayCollision | CollisionResponseCoefficient | bDisableGravity |
|---|---|---|---|---|---|---|---|---|
| PX_SlideTrashEffects.Slide_Heavy | 6 | 6 | NO_PARTICLE_INTERACTION | True | True | =absent (False) | 0.2 | absent (False) |
| PX_GlassEffects.Glass | 5 | 5 | NO_PARTICLE_INTERACTION | True | True | =absent (False) | 0.2 | absent (False) |
| PX_LevelEffects.Fluid.FlyingTrash_Light | 4 | 4 | NO_PARTICLE_INTERACTION | True | True | =absent (False) | 0.2 | absent (False) |
| PX_LevelEffects.SteamSmoke | 4 | 4 | NO_PARTICLE_INTERACTION | True | True | =absent (False) | 0.2 | absent (False) |
| PX_GlassEffects.Glass_big | 3 | 3 | NO_PARTICLE_INTERACTION | True | True | =absent (False) | 0.2 | absent (False) |
| PX_LevelEffects.Fluid.Sparks | 2 | 2 | NO_PARTICLE_INTERACTION | True | True | =absent (False) | 0.2 | absent (False) |
| PX_LevelEffects.Fluid.Dustmotes | 2 | 2 | MIXED_MODE | True | True | =absent (False) | 0.2 | True |
| PX_LevelEffects.GroundFog | 2 | 2 | NO_PARTICLE_INTERACTION | False | True | =absent (False) | 0.2 | True |
| PX_LevelEffects2.Heli_Smoke | 1 | 1 | SPH | True | True | =absent (False) | 0.2 | absent (False) |
| PX_LevelEffects.Fluid.SmokeGrenade | 1 | 1 | SPH | True | True | =absent (False) | 0.2 | absent (False) |
| PX_LevelEffects.Fluid.VentSmokeRb | 1 | 1 | SPH | True | True | =absent (False) | 0.2 | absent (False) |
| PX_GlassEffects.Glass_Sculpture | 1 | 1 | NO_PARTICLE_INTERACTION | True | True | =absent (False) | 0.2 | absent (False) |
| PX_LevelEffects.Fluid.SteamSmokeGround | 1 | 1 | SPH | True | True | =absent (False) | 0.2 | absent (False) |
| PX_LevelEffects2.SteamSmoke_highmove | 1 | 1 | NO_PARTICLE_INTERACTION | True | True | =absent (False) | 0.2 | absent (False) |

### PhysXParticleSystem instances with a non-default SimulationMethod or bStaticCollision

| map | object | bStaticCollision | SimulationMethod | own tag | MaxParticles |
|---|---|---|---|---|---|
| Subway_MoPu-Ren_Spt.me1 | PX_LevelEffects.Fluid.Dustmotes | True | MIXED_MODE | True | 10000 |
| Subway_MoPu_Spt.me1 | PX_LevelEffects2.Heli_Smoke | True | SPH | True | 500 |
| Subway_Plat_Spt.me1 | PX_LevelEffects.Fluid.SmokeGrenade | True | SPH | True | 1000 |
| Subway_Ren_Spt.me1 | PX_LevelEffects.Fluid.Dustmotes | True | MIXED_MODE | True | 10000 |
| Subway_Roof.me1 | PX_LevelEffects.Fluid.VentSmokeRb | True | SPH | True | 500 |
| Mall_Mall-Roof_Spt.me1 | PX_LevelEffects.Fluid.SteamSmokeGround | True | SPH | True | 5000 |
| Factory_Facto_Spt.me1 | PX_LevelEffects.GroundFog | False | NO_PARTICLE_INTERACTION | False | 5000 |
| Factory_Lbay.me1 | PX_LevelEffects.GroundFog | False | NO_PARTICLE_INTERACTION | False | 5000 |

### SeqAct_TdPhysXGate instances

| map | instances |
|---|---|
| Mall_Mall_Lw.me1 | 18 |
| Cranes_Puzz-Off_Building.me1 | 1 |
| Subway_Roof-RenCo_Spt.me1 | 1 |
| Mall_HW_Spt.me1 | 1 |

All 21: bPhysXNotGate absent (false); InputLinks label `In`, OutputLinks label `Out` [V].

## Parse failures

None. 227 of 227 map packages parsed and scanned. No object failed the tag decoder. Packages with the same content as
other maps were not de-duplicated; each map counts separately.

## Summary

- Maps: 227 parsed, 654 objects of the target classes (176 SkeletalMesh, 423 skeletal components, 34 PhysXParticleSystem,
  21 SeqAct_TdPhysXGate). No cloth-named Kismet class is placed in these maps.
- Cloth content is in use: 32 cloth meshes (43 exports, 21 maps), 107 cloth-enabled components on SkeletalMeshActors (21 maps).
- W/M paths reached by map content: valid bounds (5 components, 1 map) and bDisableClothCollision (8 components, 7 maps,
  flag meshes). Pressure, two-way collision, metal, attach-to-core, frozen, base-body and fluid collision-response or
  two-way settings are not set by any map object.
- Other content in use with no GAPS row: ClothBlendWeight and ClothMin/MaxBlendDistance (9 maps), bUseCompartment on 86 cloth
  components, bAutoFreezeClothWhenNotRendered on 81.

## Open questions

1. The exe flag bits for pressure and two-way collision (ClothData+0x1dc bits 2 and 3) are assumed to be the declared
   bEnableClothPressure and bEnableClothTwoWayCollision. [A] Confirm in the exe (exe-cloth-create).
2. ClothBlendWeight, ClothMinBlendDistance, ClothMaxBlendDistance, ClothDynamicBlendWeight: used in 9 maps, but no GAPS row,
   exe doc or shim code covers them (grep of docs/audit, shim and re/out docs found nothing). Needs an exe trace.
3. bUseCompartment=True on 86 of the 107 cloth-enabled components: the exe use is not traced. Possibly the RB-compartment rule
   in GAPS row 11. [A]
4. bAutoFreezeClothWhenNotRendered on 81 cloth components: the runtime effect is not traced here.
5. The SeqAct_TdPhysXGate test (bPhysXNotGate, two output links) is not traced; the [A] guess is a PhysX-enhanced check.
6. One cloth-enabled component (Mall_Mall_Spt.me1) has no SkeletalMesh tag in the map; its mesh is set by script or inherited.
   Not resolved.
7. ClothForceScale is absent from the class defaults, so its zero value on the 16 untagged cloth components is an assumption
   [A] (the exe constructor is not checked).
8. The 69 SkeletalMesh names are copies inside the map packages (outer = the asset package, e.g. CH_Pigeon). Whether they match
   the stand-alone asset packages was not checked.
9. tools/ue3pkg.py export_props misreads component exports (see Method). The fix is not applied to the tool (not edited here).

