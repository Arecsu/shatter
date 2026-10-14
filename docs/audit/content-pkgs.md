# Content scan, non-map packages (historical audit)

Status: **DONE** (all 1745 non-map packages in the list parsed; 0 parse errors, 0 timeouts).
Packages scanned: all 1745 in the list (Props, Effects, Script, Characters, Interiors, TdGame/TdSharedContent/... .u, Engine.u, etc.).
Method: a read-only scan with `tools/ue3pkg.py` (not modified), one process per CPU, 240 s alarm per package. Scripts and
raw dumps are in the scratch dir (see "Raw dumps" at the end). Values are the tagged value of the object, or the inherited
value from its archetype / class default chain (source is recorded per value in the dumps). [V] = read from the package,
[A] = inferred.

## Important: tools/ue3pkg.py mis-decodes component exports [V]
`Package.export_props` returns garbage for about 170 non-default SkeletalMeshComponent / TdSkeletalMeshComponent exports:
- 75 instance components (Props/Characters) start their tags 4 bytes later than `export_props` assumes (after NetIndex);
- 95 class-default component templates (`Default__X.Comp`) start 12 bytes later;
- 12 more templates decode to no known property type at any start offset up to 160 bytes (listed below).
This scan uses its own start-offset search (`scan.py: decode()`, statuses `ok`, `shiftN`, `empty`, `insane`). The
existing `tools/physx_asset_inventory.py` therefore misses the cloth-enabled components (its SkeletalMeshComponent check reads
the same wrong offsets). The scan proposed the same start-offset search for the inventory tool; later decoder fixes are recorded in [GAPS.md](GAPS.md).

## Counts

### Cloth meshes (SkeletalMesh with ClothBones or ClothIndexBuffer, [V])
| item | assets | packages | note |
|---|---|---|---|
| SkeletalMesh with cloth data | **88** distinct | P_Cloth 39, P_Cloth2 37, P_Maintenance 8, P_Flags 2, P_Buildings 2 | The initial inventory counted 87; the +1 is not resolved (rule here: ClothBones or ClothIndexBuffer count > 0) |
| bEnableClothPressure true | **0** | | ClothPressure is 1.0 on all 88 (class default, inherited) |
| bEnableClothTwoWayCollision true | **0** | | ClothCollisionResponseCoefficient = 0.2 on all 88 (inherited) |
| ClothCollisionResponseCoefficient != 0.2 | **0** | | |
| bClothMetal true | **0** | | ClothMetalImpulseThreshold = 10.0 default on all; penetration/max-deform not tagged |
| ClothSpecialBones non-empty (attachment table) | **0** | | property exists, never tagged |
| ClothAttachmentResponseCoefficient != 0.2 / ClothAttachmentTearFactor != 1.5 | **0** / **0** | | defaults on all 88 |
| bEnableClothTearing true | **37** | P_Cloth 19, P_Cloth2 17, P_Maintenance 1 | tear enabled; ClothTearFactor 1.5..3.5 (mostly 3.5 default) |
| bEnableClothSelfCollision true | **39** | P_Cloth 19, P_Cloth2 15, P_Maintenance 5 | |
| bEnableClothLineChecks true | 60 | | not in GAPS |
| bEnableClothOrthoBendConstraints true | 0 | | |
| ClothWeldedIndices / ClothWeldingMap tagged | 1 | | welding data present on one mesh |

### SkeletalMeshComponent family (non-CDO objects: 182; CDO and class templates decoded per section above, [V])
| item | count | note |
|---|---|---|
| bEnableClothSimulation true | **7** instances | all in Props/P_Buildings.upk; 3 distinct meshes |
| bDisableClothCollision true | **4** | all with bEnableClothSimulation; 4 of the 7 |
| bClothFrozen true | **0** | |
| bEnableValidBounds true | **0** | ValidBounds stay at the class default +-500 |
| bAttachClothVertsToBaseBody true | **0** | |
| ClothRBChannel set (non-default) | **0** | class default RBCC_Cloth everywhere |
| ClothRBCollideWithChannels set | **3** | tagged struct holds only `Default` = True; no Cloth bit tagged [V]. Default of the Cloth bit [A] |
| bEnableClothSimulation true on templates (Default__) | 0 | |

Placed instances: the 7 component instances are in P_Buildings.upk prefab exports (names `*_Arc0`, `*_Arc1`), not in maps.
Owner actors are the outer export of each component (the name without `.SkeletalMeshComponent0`). No map package is in this scan.

### PhysXParticleSystem (41 non-CDO objects in this scan, [V])
| item | count | note |
|---|---|---|
| CollisionResponseCoefficient != 0.2 | **0** | all 41 at the class default 0.2 (never tagged) |
| bTwoWayCollision true | **0** | never tagged; default false |
| bStaticCollision false (tagged) | **3** | GroundFog, Smoke_InLights, WaterFall (PX_LevelEffects.upk) |
| bDynamicCollision false (tagged) | **4** | SP09_ExploHeliDebris, WaterFall, weapon_debris_sparks (PX_ImpactEffects.upk and a copy in TdGame.u) |
| bDisableGravity true | 5 | |
| SimulationMethod histogram | ESM_NO_PARTICLE_INTERACTION 27, ESM_SPH 12, ESM_MIXED_MODE 2 | matches the initial inventory's 12 SPH and 2 mixed configs |
| PacketSizeMultiplier | EPSM_16 29, EPSM_4 7, EPSM_8 5 | |
| ParticleReserve non-zero | 4 | weapon_debris 300, weapon_debris_sparks 200 |

Per package: Effects/PX_LevelEffects.upk 25, PX_LevelEffects2.upk 6, PX_ImpactEffects.upk 4, PX_SlideTrashEffects.upk 3,
TdGame.u 2 (copies of PX_ImpactEffects objects), EngineResources.upk 1 (DefaultPhysXParSys, no tags).

### Kismet / script
| item | result |
|---|---|
| SeqAct_SetClothFrozen | class in Engine.u (CDO only). **0** instances in these packages |
| SeqAct_TdPhysXGate | 1 instance in Script/SPT_GlassSculpture.upk (not cloth, no cloth props); CDO in TdGame.u |
| Native names SetClothValidBounds, EnableClothValidBounds, SetClothFrozen, SetClothFlags, SetClothPosition, ResetClothVertsToRefPose, AttachClothToCollidingShapes, SetAttachClothVertsToBaseBody, SetEnableClothSimulation, SetClothPressure, UpdateClothParams, AddImpulseAtClothPos, SetClothExternalForce | in the name table of **Engine.u only** (as Function exports of SkeletalMeshComponent). No other package in the list has any of these 13 names, neither as import nor name. [V] |
| Get*Cloth* getters (GetClothPressure, GetClothTearFactor, ...) | Engine.u only [V] |

## Per-asset list: non-default hits on W/M paths

Only these hits exist in the scanned packages. Everything else in the counts table is zero.

### W path: bDisableClothCollision on a live cloth (GAPS row 5) [V]
All four are SkeletalMeshComponent instances with bEnableClothSimulation = True and bDisableClothCollision = True (tagged),
package Props/P_Buildings.upk, mesh P_Flags.SK_Flag_02:
1. `FlagMounted_01.PF_FlagMounted_01.PF_FlagMounted_01_2Arc0.SkeletalMeshComponent0` (outer: same name without the component)
2. `FlagMounted_01.PF_FlagMountedCloth_01.PF_FlagMounted_01_2Arc0.SkeletalMeshComponent0`
3. `FlagPole_01.PF_FlagPole_01.PF_FlagPole_01_1Arc0.SkeletalMeshComponent0` (also ClothRBCollideWithChannels: Default = True)
4. `FlagPole_01.PF_FlagPoleCloth_01.PF_FlagPole_01_1Arc0.SkeletalMeshComponent0` (also ClothRBCollideWithChannels)

### Cloth simulation on, no W/M flag (for context) [V]
- `PF_BigBanner_01.PF_BigBanner_01_Arc0.SkeletalMeshComponent0`, mesh P_Flags.SK_Flag_01 (bEnableClothSimulation tagged)
- `PF_BigBannerCloth_01.PF_BigBanner_01_Arc0.SkeletalMeshComponent0`, mesh P_Flags.SK_Flag_01
- `PX_Awning_01.PF_PX_Awning_01.PF_PX_Awning_01_Arc1.SkeletalMeshComponent0`, mesh PX_Awning_01.PX_SK_Awning_02
  (also ClothRBCollideWithChannels: Default = True)

### Paths with zero non-default hits in this scan [V]
- Pressure (row 7): 0 meshes with bEnableClothPressure. ClothPressure never differs from 1.0.
- Two-way cloth contacts and coefficient (row 8): 0 meshes.
- Metal / attachToCore parameters (row 9): 0 meshes with bClothMetal; 0 with non-default attachment response/tear.
  Mapping of the attachToCore params to these UE3 names is [A].
- Valid bounds (row 4): 0 components with bEnableValidBounds (SetClothValidBounds / EnableClothValidBounds not called in any scanned script).
- Frozen cloth (SetClothFrozen, bClothFrozen): 0 components.
- Base body attach (bAttachClothVertsToBaseBody, SetAttachClothVertsToBaseBody): 0 components.
- Cloth attachment table (ClothSpecialBones): 0 meshes.
- Fluid collision response != 0.2 (row 1): 0 of 41 PhysXParticleSystem.
- Fluid two-way (bTwoWayCollision): 0.

### Other non-default hits (not W/M, for the record)
- Tearing enabled: 37 meshes (list in the dump, `cloth_skelmesh_eff.json`, field `tearing`).
- Self-collision enabled: 39 meshes (field `self_coll`); reference accuracy varies after folding; see [re-notes-cloth.md](../re-notes-cloth.md).
- Fluid collide-only overrides: static off 3, dynamic off 4, SPH 12, mixed 2 (see counts).

## Undecodable objects (12, [V] they decode to nothing, [A] they are native-only)
Default__ component templates: GameFramework.u Default__GameVehicle.SVehicleMesh; TdSharedContent.u
Default__TdWeapon_FlashbangGrenade.FirstPersonMesh; TdGame.u Default__TdWeapon_Light.FirstPersonMesh and ThirdPersonMesh,
Default__TdProj_HeavyFlashbangGrenade.GrenadeMesh0, Default__TdVehicleSeatPawn.SVehicleMesh, Default__TdWeapon_Grenade
First/ThirdPersonMesh, Default__TdWeapon_Heavy First/ThirdPersonMesh, Default__TdWeapon_Pistol_Taser First/ThirdPersonMesh.
Their SkeletalMesh references are unknown (not decoded), so any cloth flag on them is unknown.

## Raw dumps (scratch dir, not in the repo)
These raw dumps and one-off scan helpers are local-only and are not distributed.
- `passA.jsonl`: per package: class hierarchy (class -> super), name-table hits (13 native names, cloth/PhysX names), function imports/exports.
- `passB.jsonl`: every object of the target classes (SkeletalMesh, SkeletalMeshComponent family, PhysXParticleSystem, SeqAct cloth/PhysX), with all tagged props (value, type, array count), archetype, decode status.
- `cloth_skelmesh_eff.json`: 88 cloth meshes, effective values with source (tagged / CDO chain).
- `raw_components_eff.json`: component objects with cloth-related keys, effective values with source.
- `psys_eff.json`: 41 PhysXParticleSystem objects, all 27 props with source.
- `cdo_values.json`: class default tagged values (Engine.u SkeletalMesh, SkeletalMeshComponent, PhysXParticleSystem; TdGame.u TdSkeletalMeshComponent).
- Scripts: `scan.py` (passes A/B), `analyze.py`, `final*.py`, `report*.py`.

Class defaults used for inherited values [V]:
- SkeletalMesh CDO (Engine.u): ClothPressure 1.0, ClothCollisionResponseCoefficient 0.2, ClothAttachmentResponseCoefficient 0.2,
  ClothAttachmentTearFactor 1.5, ClothTearFactor 3.5, ClothTearReserve 128, ClothMetalImpulseThreshold 10.0, ClothIterations 5,
  ClothSleepLinearVelocity -1.0. bEnable* flags not tagged in the CDO, so false.
- SkeletalMeshComponent CDO (Engine.u): ClothRBChannel RBCC_Cloth, ValidBoundsMin (-500,-500,-500), ValidBoundsMax (500,500,500),
  ClothAttachmentTearFactor -1.0, ClothBlendWeight 1.0, ClothMaxBlendDistance -1.0.
- TdSkeletalMeshComponent CDO (TdGame.u): no tags (inherits the above).
- PhysXParticleSystem CDO (Engine.u): MaxParticles 32767, CollisionResponseCoefficient 0.2, SimulationMethod ESM_NO_PARTICLE_INTERACTION,
  bDynamicCollision/bStaticCollision True, FrictionWithStaticShapes 0.05, FrictionWithDynamicShapes 0.5.

## Summary
- All 1745 packages parsed. 88 cloth SkeletalMeshes, 182 component objects, 41 PhysX particle systems, 1 SeqAct_TdPhysXGate instance and 1 SeqAct_SetClothFrozen class default in this set.
- On the content in these packages, the W/M paths that the GAPS table lists are mostly unused: no pressure, no two-way cloth,
  no metal, no attachment table, no valid bounds, no frozen cloth, no base-body attach, no fluid collision response != 0.2.
- The one live W path here is bDisableClothCollision (4 flag components in Props/P_Buildings.upk, mesh P_Flags.SK_Flag_02).
- Tearing (37 meshes) and self-collision (39 meshes) are used by a large part of the cloth set.
- Kismet cloth nodes: no placed SeqAct_SetClothFrozen in these packages. The cloth script functions exist only in Engine.u.
- tools/ue3pkg.py export_props mis-decodes ~170 component exports (see top); the inventory tool misses the flag components.

## Open questions
1. Which maps place the 7 cloth-enabled components (or the 4 with DisableClothCollision)? Compare the map scans; the packages here only hold prefab-style exports.
2. The +1 cloth mesh versus the initial inventory's 87: this scan counts 88 with a ClothBones or ClothIndexBuffer array. Which object is extra, and which rule the 87 used, is not checked.
3. ClothRBCollideWithChannels holds only `Default` = True; the default of the Cloth bit in RBCollisionChannelContainer is not verified [A]. Matters for the cloth-contact channel row 24.
4. The 12 undecodable Default__ component templates (weapon and vehicle meshes): their SkeletalMesh and any cloth flag are unknown. Cheap follow-up: find their native layout.
5. The 4-byte and 12-byte prefixes before tagged properties on instance and template components are not explained in the UE3 format we have. Current decode is empirical (validated by property-type sanity only). A check against one object's exact serialized layout would make it solid [A].
6. The mapping from exe ClothData bits/offsets to these UE3 names (bEnableClothPressure -> ClothData+0x1dc bit 2, etc.) is still [A] (ue3-prop-map.md is the place for it); this scan only reports UE3 values.
