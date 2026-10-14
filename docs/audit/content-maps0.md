# Content scan: maps part 00 (226 .me1): PhysX, cloth, fluid objects

Historical content audit, first group of 226 `.me1` maps. Parse failures: none. Timeouts: none. Object decode failures: none (1,166 relevant exports: 737 SkeletalMeshComponent, 241 SkeletalMesh, 122 TdSkeletalMeshComponent, 43 PhysXParticleSystem, 23 SeqAct_TdPhysXGate).

Scope: skeletal meshes, skeletal components, fluid systems and Kismet objects inside these maps. Script-package name tables are covered by [content-pkgs.md](content-pkgs.md). Component counts were redone after the prefix fix (below); the first pass that used `export_props` is discarded.

Raw per-object dumps and one-off scan helpers are local-only and are not distributed. This report records their aggregate findings; component-decoder and placement corrections are summarized in [GAPS.md](GAPS.md).

## Method

- Reader: `tools/ue3pkg.py` (not modified). Maps are LZO packages (version 536, licensee 43). Six worker processes, one map each, 300 s alarm per map.
- Every tagged value is recorded per object. A value that is not tagged is marked **inherited** when it comes from the archetype chain (local template in the map, or an import resolved through the package file: `Engine.u`, `TdGame.u`, `P_Buildings.upk`). Class defaults come from `Default__SkeletalMesh`, `Default__SkeletalMeshComponent`, `Default__PhysXParticleSystem` in `Engine.u`. All component chains resolved (no unresolved archetype).
- **Tag-stream start (fixed decoder).** `tools/ue3pkg.py` `export_props` starts the tags 4 bytes in (after NetIndex). That is wrong for components. Their tags start after an extra 4-byte TemplateOwnerClass (instances), and templates carry a further 8-byte TemplateName (inferred layout, consistent with the measurements here). Measured: the 737 SkeletalMeshComponent exports decode at 8 bytes, the 122 `Default__` TdSkeletalMeshComponent templates at 16 bytes, the 106 map-local SkeletalMeshComponent template copies at 8 bytes (smallest start that parses exactly; no 16-byte parse for them). SkeletalMesh, PhysXParticleSystem and Kismet exports decode at 4 bytes. `scan.py` tries 4, 8, ... 64 bytes and keeps the first start where every tag type is known and the stream ends with `None` or at the export end. The start used is stored per object (`prefix_bytes`). Some exports also end exactly at a larger start; the smallest is taken [A].
- **Consequence [V]:** with `export_props` (4-byte start) every component export decodes to garbage (e.g. names `ClothBones_1057`, types `02_827`), so `tools/physx_asset_inventory.py` (cloth_components) cannot see `bEnableClothSimulation` on components. Checked on every fourth map: none of 246 component exports decodes at the 4-byte start. Earlier component counts from `export_props` are invalid.
- Cloth fields are UPROPERTYs of SkeletalMesh (72 properties) and SkeletalMeshComponent (119), listed from the class exports of `Engine.u`, so the tag stream carries them; native render data after the tags is not decoded.

## Counts

Distinct = by object path. Instances = occurrences in maps. Maps = number of the 226 maps with at least one hit.

### Cloth meshes (SkeletalMesh objects stored in the maps; item A)

| count | distinct | instances | maps |
|---|---|---|---|
| SkeletalMesh objects in maps (all) | 79 | 241 | 66 |
| cloth meshes (ClothIndexBuffer or ClothBones non-empty) | 39 | 66 | 29 |
| bEnableClothPressure true | 0 | 0 | 0 |
| bEnableClothTwoWayCollision true | 0 | 0 | 0 |
| bClothMetal true | 0 | 0 | 0 |
| ClothMetal* tagged (MaxDeformationDistance, PenetrationDepth, ImpulseThreshold) | 0 | 0 | 0 |
| ClothSpecialBones non-empty (attachment table) | 0 | 0 | 0 |
| bEnableClothTearing true | 14 | 15 | 6 |
| bEnableClothSelfCollision true | 17 | 19 | 11 |
| bEnableClothLineChecks true | 23 | 25 | 8 |
| bEnableClothDamping true | 6 | 7 | 4 |

Tearing values on the 14 distinct local tearing meshes: ClothTearFactor 1.3 to 2.5 (CDO 3.5), ClothTearReserve 128 to 1400 (CDO 128). ClothPressure: every mesh at the CDO value 1.0. ClothCollisionResponseCoefficient: CDO 0.2 everywhere.

External cloth meshes (read from the asset packages `Props/P_Cloth.upk`, `P_Cloth2.upk`, `P_Maintenance.upk`; referenced by components in the maps): 34 distinct, all with cloth index buffers. bEnableClothPressure, bEnableClothTwoWayCollision, bClothMetal, ClothSpecialBones: **0 of 34**. bEnableClothTearing: 13 of 34.

### Cloth components (SkeletalMeshComponent; item B)

Placed instances (object path under `TheWorld.PersistentLevel`): 631 SkeletalMeshComponent instances in 47 maps. Map-local template copies: 106 SkeletalMeshComponent (copies of `P_Buildings` / flag archetypes) in 29 maps and 122 TdSkeletalMeshComponent `Default__` templates in 27 maps.

| item | placed instances: count | maps | source | note |
|---|---|---|---|---|
| bEnableClothSimulation true | 93 | 12 | tagged 93 | all 93 reference an external cloth mesh (34 distinct; P_Cloth, P_Cloth2, P_Maintenance) |
| bAutoFreezeClothWhenNotRendered true | 93 | 12 | tagged 93 | same 93 components |
| bDisableClothCollision true | 0 | 0 | - | none on a placed component (see W5) |
| bEnableValidBounds true | 20 | 2 | tagged 20 | explicit ValidBoundsMin/Max on all 20 (W4) |
| bClothFrozen true | 0 | 0 | - | none |
| bAttachClothVertsToBaseBody true | 0 | 0 | - | none |
| ClothRBChannel != RBCC_Cloth (CDO) | 1 | 1 | tagged | RBCC_Default on Edge_Pt1_Spt SkeletalMeshActor_0.SkeletalMeshComponent_17 |
| ClothRBCollideWithChannels tagged | 84 | 11 | tagged | channel sets Default / Pawn / Untitled1-4, all true |

Map-local template copies (not placed; the placed actors do not use them, see W5):

| item | template copies: count | maps | objects |
|---|---|---|---|
| bEnableClothSimulation true | 21 | 17 | FlagPole_01.PF_FlagPole_01.PF_FlagPole_01_1Arc0 (10), FlagMounted_01.PF_FlagMounted_01.PF_FlagMounted_01_2Arc0 (9), PF_BigBanner_01.PF_BigBanner_01_Arc0 (2) |
| bDisableClothCollision true | 19 | 16 | FlagPole_01.PF_FlagPole_01.PF_FlagPole_01_1Arc0 (10), FlagMounted_01.PF_FlagMounted_01.PF_FlagMounted_01_2Arc0 (9) |
| TdSkeletalMeshComponent `Default__` templates, any cloth flag | 0 | 0 | none |

Other cloth component fields tagged on placed instances (not W/M rows): ClothWind on 94, ClothForceScale on 87 (values 1.0 x49, 7.0 x9, 4.0 x7, 0.2 x4; CDO has none, so 0 otherwise), ClothMaxBlendDistance on 63, ClothMinBlendDistance on 63, ClothBlendWeight on 6 (0.2). Counts are over all SkeletalMeshComponent objects including templates; see Open questions.

### PhysXParticleSystem (fluid; item C)

43 instances in 24 maps; 19 distinct object paths. Each instance uses the CDO (`Default__PhysXParticleSystem`) for untagged fields.

| item | result |
|---|---|
| CollisionResponseCoefficient != 0.2 | 0 of 43 (all 0.2) |
| bTwoWayCollision true | 0 of 43 |
| SimulationMethod | ESM_NO_PARTICLE_INTERACTION 36, ESM_SPH 6, ESM_MIXED_MODE 1 |
| bStaticCollision false | 2 (GroundFog in Stormdrain_Std-StdP_Slc_Spt, WaterFall in Stormdrain_StdP-StdE_slc) |
| bDynamicCollision false | 1 (WaterFall) |
| bDisableGravity true | 2 |

Non-default fluid objects (with the fields that differ from the CDO) are in `pxs.json`; the 27-property map is in [ue3-prop-map.md](ue3-prop-map.md).

### Kismet / script (item D, maps only)

| class | instances in maps | note |
|---|---|---|
| SeqAct_TdPhysXGate | 23 | tagged data is editor layout only (ObjPosX/Y, DrawWidth/Height, ObjInstanceVersion, links). Behaviour is native, not visible here. |
| SeqAct_SetClothFrozen and other Cloth* action classes | 0 | the class exists in `Engine.u` only as `Default__SeqAct_SetClothFrozen`; no instance in these maps |
| SeqAct_SetParticleSysParam | 1 (Edge_Pt1_Spt) | generic particle-system parameter setter; its target and parameter names are not decoded |
| SeqAct_SetSkeletalMesh | 1116 | not a PhysX class; listed because it swaps a component's mesh at run time (cloth targets not checked) |

Script name-table checks are in [content-pkgs.md](content-pkgs.md).

## Per-asset list: non-default hits on the W/M paths

W1 (fluid CollisionResponseCoefficient != 0.2), fluid two-way: none. M7 (cloth pressure), M8 (cloth two-way collision), M9 (attachToCore / metal): none in the maps and none in the 34 external meshes. Attach-to-base (bAttachClothVertsToBaseBody) and bClothFrozen: none. Rows 2 and 3 (contact with moving shapes, static re-query after teleport) are runtime behaviour and are not visible in tags.

### W4: bEnableValidBounds true, placed instances (20 in 2 maps)

| map | component (object path) | cloth mesh (package) | ValidBoundsMin | ValidBoundsMax |
|---|---|---|---|---|
| Cranes_Off_Spt.me1 | TheWorld.PersistentLevel.SkeletalMeshActor_12.SkeletalMeshComponent_8 | P_Cloth.Meshes.4by1_long_tear (P_Cloth) | (-800, -800, -1400) | (800, 800, 800) |
| Cranes_Off_Spt.me1 | TheWorld.PersistentLevel.SkeletalMeshActor_15.SkeletalMeshComponent_8 | P_Cloth.Meshes.4by1_long_tear (P_Cloth) | (-800, -800, -1400) | (800, 800, 800) |
| Cranes_Off_Spt.me1 | TheWorld.PersistentLevel.SkeletalMeshActor_29.SkeletalMeshComponent_8 | P_Cloth.Meshes.4by1_long_tear (P_Cloth) | (-800, -800, -1400) | (800, 800, 800) |
| Cranes_Off_Spt.me1 | TheWorld.PersistentLevel.SkeletalMeshActor_64.SkeletalMeshComponent_8 | P_Cloth.Meshes.4by1_long_tear (P_Cloth) | (-800, -800, -1400) | (800, 800, 800) |
| Cranes_Off_Spt.me1 | TheWorld.PersistentLevel.SkeletalMeshActor_65.SkeletalMeshComponent_8 | P_Cloth.Meshes.4by1_long_tear (P_Cloth) | (-800, -800, -1400) | (800, 800, 800) |
| Cranes_Off_Spt.me1 | TheWorld.PersistentLevel.SkeletalMeshActor_66.SkeletalMeshComponent_8 | P_Cloth.Meshes.4by1_long_tear (P_Cloth) | (-800, -800, -1400) | (800, 800, 800) |
| Cranes_Off_Spt.me1 | TheWorld.PersistentLevel.SkeletalMeshActor_67.SkeletalMeshComponent_8 | P_Cloth.Meshes.4by1_long_tear (P_Cloth) | (-800, -800, -1400) | (800, 800, 800) |
| Cranes_Off_Spt.me1 | TheWorld.PersistentLevel.SkeletalMeshActor_68.SkeletalMeshComponent_8 | P_Cloth.Meshes.4by1_long_tear (P_Cloth) | (-800, -800, -1400) | (800, 800, 800) |
| Cranes_Off_Spt.me1 | TheWorld.PersistentLevel.SkeletalMeshActor_69.SkeletalMeshComponent_8 | P_Cloth.Meshes.4by1_long_tear (P_Cloth) | (-800, -800, -1400) | (800, 800, 800) |
| Cranes_Plaza_Spt.me1 | TheWorld.PersistentLevel.SkeletalMeshActor_0.SkeletalMeshComponent_7 | P_Cloth.PlasticSheet_01.PX_SK_Crane_Cloth (P_Cloth) | (-800, -800, -800) | (800, 800, 400) |
| Cranes_Plaza_Spt.me1 | TheWorld.PersistentLevel.SkeletalMeshActor_1.SkeletalMeshComponent_7 | P_Cloth.Meshes.SP03_Crane_Tearthru (P_Cloth) | (-500, -500, -6000) | (500, 800, 500) |
| Cranes_Plaza_Spt.me1 | TheWorld.PersistentLevel.SkeletalMeshActor_12.SkeletalMeshComponent_7 | P_Cloth.Meshes.Crane_3wide_700vert2 (P_Cloth) | (-800, -800, -800) | (800, 800, 400) |
| Cranes_Plaza_Spt.me1 | TheWorld.PersistentLevel.SkeletalMeshActor_14.SkeletalMeshComponent_7 | P_Cloth.Meshes.Crane_3wide_700vert2 (P_Cloth) | (-800, -800, -800) | (800, 800, 400) |
| Cranes_Plaza_Spt.me1 | TheWorld.PersistentLevel.SkeletalMeshActor_3.SkeletalMeshComponent_7 | P_Cloth.PlasticSheet_01.PX_SK_Crane_Cloth (P_Cloth) | (-800, -800, -800) | (800, 800, 400) |
| Cranes_Plaza_Spt.me1 | TheWorld.PersistentLevel.SkeletalMeshActor_35.SkeletalMeshComponent_7 | P_Cloth.Meshes.Crane_3wide_700vert2 (P_Cloth) | (-800, -800, -800) | (800, 800, 400) |
| Cranes_Plaza_Spt.me1 | TheWorld.PersistentLevel.SkeletalMeshActor_5.SkeletalMeshComponent_7 | P_Cloth.PlasticSheet_01.PX_SK_Crane_Cloth (P_Cloth) | (-800, -800, -800) | (800, 800, 400) |
| Cranes_Plaza_Spt.me1 | TheWorld.PersistentLevel.SkeletalMeshActor_6.SkeletalMeshComponent_7 | P_Cloth.Meshes.Crane_Tarps_tearing_flap (P_Cloth) | (-800, -800, -800) | (800, 800, 400) |
| Cranes_Plaza_Spt.me1 | TheWorld.PersistentLevel.SkeletalMeshActor_7.SkeletalMeshComponent_7 | P_Cloth.PlasticSheet_01.PX_SK_Crane_Cloth (P_Cloth) | (-800, -800, -800) | (800, 800, 400) |
| Cranes_Plaza_Spt.me1 | TheWorld.PersistentLevel.SkeletalMeshActor_8.SkeletalMeshComponent_7 | P_Cloth.Meshes.SP03_Crane_Tearthru (P_Cloth) | (-500, -500, -6000) | (500, 800, 500) |
| Cranes_Plaza_Spt.me1 | TheWorld.PersistentLevel.SkeletalMeshActor_9.SkeletalMeshComponent_7 | P_Cloth.Meshes.Crane_3wide_700vert (P_Cloth) | (-800, -800, -800) | (800, 800, 400) |

### W5: bDisableClothCollision true: placed instances: **none**

The 19 hits in the first pass were map-local template copies, not placed actors: 19 template copies in 16 maps (`FlagPole_01.PF_FlagPole_01.PF_FlagPole_01_1Arc0` x10 and `FlagMounted_01.PF_FlagMounted_01.PF_FlagMounted_01_2Arc0` x9; both also set bEnableClothSimulation). Their tagged values match the P_Buildings flag templates (`PF_FlagPole_01_1Arc0`, `PF_FlagMounted_01_2Arc0`, `PF_FlagPoleCloth_01`, `PF_FlagMountedCloth_01` in `P_Buildings.upk`), which set bEnableClothSimulation=True and bDisableClothCollision=True [V]; the copies' own archetype link was not traced to them [A]. No placed SkeletalMeshComponent in these maps inherits or tags bDisableClothCollision. The placed flag components in the maps (archetypes `FlagPole_01.PF_FlagPole_01.PF_FlagPole_01_7Arc0`, `FlagMounted_01...9Arc0`, `FlagPoleCloseUp_01`) resolve to templates with no cloth flags [V].

Placement of P_Buildings SK_Flag_02 components with bDisableClothCollision: in these maps the answer is that the flag banners are placed through archetypes that do not carry the flag, so the live DISABLE_COLLISION path (GAPS row 5) is not reached by these maps' placements [V for the chains checked, A for the rest].

### Other non-default hits (not W/M)

- bEnableClothSimulation on placed instances: 93 in 12 maps: Stormdrain_All_SPT.me1 13, Escape_R1-St1_Spt.me1 13, Cranes_Plaza_Spt.me1 11, Cranes_Off_Spt.me1 9, Stormdrain_All-std_Slc_Spt.me1 9, Escape_Plaza_Spt.me1 9, Edge_Pt1-Pt2_Slc.me1 9, Stormdrain_StdE_Spt.me1 6, Stormdrain_boss_Spt.me1 6, Escape_R1_Spt.me1 4, Edge_Pt1_Spt.me1 3, Escape_Intro_Spt.me1 1. Full list in `smc_rows.json`.
- ClothRBChannel RBCC_Default: Edge_Pt1_Spt, SkeletalMeshActor_0.SkeletalMeshComponent_17 (CDO RBCC_Cloth).
- Fluid: bStaticCollision false (2), bDynamicCollision false (1), bDisableGravity true (2).

## Maps with W/M hits (placed instances)

| map | W4 valid bounds | bEnableClothSimulation (placed) | bDisableClothCollision (placed) |
|---|---|---|---|
| Cranes_Off_Spt.me1 | 9 | 9 | 0 |
| Cranes_Plaza_Spt.me1 | 11 | 11 | 0 |
| Edge_Pt1-Pt2_Slc.me1 | 0 | 9 | 0 |
| Edge_Pt1_Spt.me1 | 0 | 3 | 0 |
| Escape_Intro_Spt.me1 | 0 | 1 | 0 |
| Escape_Plaza_Spt.me1 | 0 | 9 | 0 |
| Escape_R1-St1_Spt.me1 | 0 | 13 | 0 |
| Escape_R1_Spt.me1 | 0 | 4 | 0 |
| Stormdrain_All-std_Slc_Spt.me1 | 0 | 9 | 0 |
| Stormdrain_All_SPT.me1 | 0 | 13 | 0 |
| Stormdrain_StdE_Spt.me1 | 0 | 6 | 0 |
| Stormdrain_boss_Spt.me1 | 0 | 6 | 0 |

## Parse failures

None. All 226 maps parsed (status ok). 0 timeouts. 0 objects with decode errors.

## Summary

- Local cloth meshes in the maps: 39 distinct (66 instances). Placed cloth components use 34 cloth meshes that live in `P_Cloth`, `P_Cloth2`, `P_Maintenance`.
- Placed components with cloth enabled: 93 in 12 maps. Placed components with explicit valid bounds: 20 in 2 maps (Cranes_Plaza_Spt, Cranes_Off_Spt; these are the W4 path).
- bDisableClothCollision (W5): 0 placed; 19 map-local copies of the P_Buildings flag templates carry it and are not placed. The first pass wrongly counted them as placements.
- Not reached by these maps (W/M): cloth pressure, two-way cloth collision, metal / attachToCore, special bones, bClothFrozen, bAttachClothVertsToBaseBody, fluid CollisionResponseCoefficient != 0.2, fluid two-way.
- Tooling: component exports need the 8/16-byte start (see Method). `export_props` counts for components are invalid.

## Open questions

1. The proposed interpretation of the extra words before component tags is TemplateOwnerClass (+4) and TemplateName (+8, templates). The decoder agrees for instances (8) and TdSkeletalMeshComponent templates (16), but map-local SkeletalMeshComponent copies decode at 8 as well. Confirm against the upk packages before trusting counts there.
2. Live DISABLE_COLLISION (GAPS row 5): no placed component in these maps uses it; the flag archetypes that carry it are not placed here. Check the other map parts and the upk placements (P_Buildings) for placed instances.
3. ClothRBChannel = RBCC_Default on one placed component (CDO RBCC_Cloth). Not a W/M row, but it changes the channel mask of that cloth (GAPS row 24).
4. ClothWind, ClothForceScale, Cloth*BlendDistance are tagged on components but are not in GAPS.md. Check whether the shim reads them.
5. SeqAct_SetSkeletalMesh (1116 instances) can change a component's mesh at run time. Cloth targets not checked.
6. SeqAct_SetParticleSysParam (1, Edge_Pt1_Spt): which particle system and parameter it sets is not decoded.
7. Cloth meshes in the upk packages (not in this scan) may still use pressure, two-way or metal; the maps show none, the package scans must confirm.

