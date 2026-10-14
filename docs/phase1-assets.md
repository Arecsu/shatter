# What PhysX content Mirror's Edge actually uses (from the cooked packages)

Source: `tools/physx_asset_inventory.py` scanning all 2424 packages (679 maps), no parse errors.
Full per-asset data: `captures/assets/physx_inventory.json` (local, gitignored: game data).
Values are UE3 asset properties (Unreal units, 1 uu ≈ 2 cm [A]). The conversion into
`NxFluidDesc`/`NxClothDesc` happens in `MirrorsEdge.exe`; later mappings are in [audit/ue3-prop-map.md](audit/ue3-prop-map.md). This is the initial inventory summary; later scans report a still-unresolved 87 vs 88 cloth-mesh count.

## Fluids (NxFluid): the main target
- 43 distinct `PhysXParticleSystem` configs; 114 instances embedded across 69 maps.
- Emitters: 1619 `ParticleModuleTypeDataMeshPhysX` (fluid particles rendered as meshes: glass
  shards, debris, trash, leaves) in 202 maps; 67 `ParticleModuleTypeDataPhysX` (sprites: smoke, steam,
  sparks) in 32 maps.
- Simulation methods: 29 × `ESM_NO_PARTICLE_INTERACTION` (collide with world only, no SPH),
  12 × `ESM_SPH` (smoke, steam, waterfall), 2 × `ESM_MIXED_MODE` (Dustmotes, GroundFog_high_movement).
- MaxParticles per system: 100 to 10000. Engine default 32767 is used only by `DefaultPhysXParSys`.
- Engine defaults (`Default__PhysXParticleSystem`): RestParticleDistance 64, KernelRadiusMultiplier 2,
  Stiffness 20, Viscosity 6, RestDensity 1000, CollisionResponseCoefficient 0.2, MaxMotionDistance 64,
  CollisionDistance 10, restitution 0.5/0.5, friction 0.05 (static)/0.5 (dynamic), PacketSizeMultiplier 16.
- Glass-related fluids: `Glass`, `Glass_big`, `Glass_Bridge`, `Glass_Sculpture`, `glass_slide`.
  They're no-interaction particles with damping 0–0.1 and CollisionDistance 2.

## Cloth (NxCloth)
- 87 distinct cloth `SkeletalMesh`es (mostly `Props/P_Cloth.upk`, `P_Cloth2.upk`, plus some map-local ones).
  Each has 1 cloth bone and about 200–850 free verts.
- Feature use: tearing 37, self-collision 38, line checks 59, bend constraints 10, damping 14.
  Pressure and "metal" cloth are unused [V: no non-default values found].
- Engine defaults: stretch 1, bend 1, density 1, thickness 0.5, damping 0.5, iterations 5, friction 0.5,
  tear factor 3.5, tear reserve 128, attachment tear 1.5, collision/attachment response 0.2.
- Placed via `SkeletalMeshActor` (1228 in 124 maps, not all cloth), `SkeletalMeshActorMAT`, and
  `TdValveSkeletalMeshActor`.

## Glass and breakables
- `FracturedStaticMeshActor`: 26 instances in 19 maps, with templates in `Script/SPT_*Glass*.upk`.
  The fragments are rigid bodies (they stay on the CPU path). The PhysX-enhanced part is the
  fluid-particle shards above.
- `GameBreakableActor` 102 in 15 maps; `KActor` 117 in 17 maps.

## PhysX-only gameplay content
- `SeqAct_TdPhysXGate` Kismet nodes: 45 in 11 maps. These gate content on `PhysXEnhanced`, so they
  point to the "PhysX on" set pieces in each level.
