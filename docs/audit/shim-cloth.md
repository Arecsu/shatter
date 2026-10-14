# Audit A2: what the shim implements for cloth (PXSHIM_CLOTH=ours)

Date 2026-10-11. Read-only audit: no source edited, the game not run. Working tree as found (uncommitted changes
present in shim/cloth_ours.cpp and solver/cloth/*; see section 5).

Legend. [V] = seen in code, header or the 2.8.0 export (file:line given). [A] = assumption, mostly about what the
original core does where no header or export settles it. Status values: implemented / partial / stored only /
pass-through / stub / not hooked / not read / ignored. "stub" = the entry exists and does nothing useful (logs or
returns a neutral value). Line numbers are into the current working tree.

## 0. How the cloth hosting is wired (context for the tables)

- Game-side object: `Obj { vt, h }` (shim/cloth_ours.cpp:111-114). `vt` points at `g_vt+1`, a 92-entry table built in
  `ensureVtable()` (cloth_ours.cpp:1555-1610) in the 2.8.1 NxCloth.h declaration order. Slot 0 = scalar deleting dtor.
  The table is shim-owned: no core vtable is patched for hosted cloths.
- Scene hook: `hkCreateCloth` (shim/cloth_wrap.cpp:332-348) calls `oursCreate` first; a hosted cloth never reaches the
  core, so the core's cloth lists do not contain it. Fallback to the core when `oursCreate` returns 0 (mesh unreadable,
  PXSHIM_CLOTH=off/0): then `installClothVtable` (cloth_wrap.cpp:293-314) patches the core's 92 slots with counting
  thunks that forward (cloth_wrap.cpp:219-243, 245-274) = pass-through with logging.
- Slot dispatch: none unknown. All 92 table entries are implemented functions (real or stub). A call with an index
  >= 92 would read past `g_vt` (cloth_ours.cpp:244): not expected, because the 2.8.0 NpCloth vtable has 92 slots
  (docs/abi/vtables-PhysXCore-v2.8.0.json, `.?AVNpCloth@@+0` = 92 [V]). Calls on core-owned NxClothMesh and
  NxPhysicsSDK/NxScene objects are never intercepted except the 4 SDK/scene slots listed in section 1, so they run the
  core's code.
- Slot numbering: the 2.8.1 declaration order. The 2.8.0 export labels every NpCloth vtable function `NpCloth_vfN`
  (re/out/PhysXCore-280/decomp.c). The 42 named slots line up with the 2.8.1 order [V: labels mapped by the name string
  in each body, e.g. vf3 setBendingStiffness, vf22 setSolverIterations, vf45 setPosition, vf79 setFlags, vf82
  addForceAtVertex, vf91 setForceFieldMaterial]. Unnamed getter positions are matched by position [A]; the export has a
  label for every slot except 88, which is unlabelled [V]. NpScene: createCloth 131 / releaseCloth 132 / setClothUserNotify
  95 named [V]; 96, 133, 134 unnamed [A]. NpPhysicsSDK: createClothMesh 21 / releaseClothMesh 22 named [V]; 23, 24 [A].
  NpClothMesh slot 1 unnamed [A].

## 1. Methods

### 1a. NxCloth (92 slots, 2.8.1 NxCloth.h order = shim table order)

Hosted cloths use the shim table. The "core" column in the notes says what a core-created (fallback) cloth does.

| slot | method | status | what is missing or differs | file:line |
|---|---|---|---|---|
| 0 | ~NxCloth (scalar deleting dtor) | stub | No-op. Release goes through NxScene::releaseCloth (oursRelease unregisters). A direct `delete` on a hosted cloth would leak it and leave it stepping [A: game does not do this; the core calls slot 0 from releaseCloth, as the logs show]. | cloth_ours.cpp:1010-1012 |
| 1 | saveToDesc | stub | Logs once, returns false. The original fills an NxClothDesc. | cloth_ours.cpp:1013-1016 |
| 2 | getClothMesh | implemented | Returns the core NxClothMesh (created by the core, hooked for the tap). | cloth_ours.cpp:1018-1020 |
| 3 | setBendingStiffness | implemented | Live (setLive); used when NX_CLF_BENDING is set. | cloth_ours.cpp:1022-1026 |
| 4 | getBendingStiffness | implemented | Returns stored P.bending. | cloth_ours.cpp:1027 |
| 5 | setStretchingStiffness | implemented | Live. | cloth_ours.cpp:1028-1032 |
| 6 | getStretchingStiffness | implemented | | cloth_ours.cpp:1033 |
| 7 | setDampingCoefficient | implemented | Live; used with NX_CLF_DAMPING. | cloth_ours.cpp:1034-1038 |
| 8 | getDampingCoefficient | implemented | | cloth_ours.cpp:1039 |
| 9 | setFriction | implemented | Live; solver reads it per step (cloth.cpp:1483). | cloth_ours.cpp:1040-1044 |
| 10 | getFriction | implemented | | cloth_ours.cpp:1045 |
| 11 | setPressure | stored only | Pressure is not in the solver (ClothParams has no pressure, solver/cloth/cloth.h:18-37). Logs "stored only" once. Game calls it (captures, section 6). | cloth_ours.cpp:1046-1050 |
| 12 | getPressure | stored only | Returns the desc or set value. | cloth_ours.cpp:1051 |
| 13 | setTearFactor | implemented | Live; tearStep uses it (cloth.cpp:1044). Tearing needs NX_CLF_TEARABLE and a tearable mesh (applyFlags). | cloth_ours.cpp:1052-1056; 325 |
| 14 | getTearFactor | implemented | | cloth_ours.cpp:1057 |
| 15 | setAttachmentTearFactor | implemented | Applied to the running solver (cloth_ours.cpp:1058-1062) and at build (cloth_ours.cpp:592). | cloth_ours.cpp:1058-1062 |
| 16 | getAttachmentTearFactor | implemented | | cloth_ours.cpp:1063 |
| 17 | setThickness | implemented | Live. Thickness also pads getWorldBounds (cloth_ours.cpp:1083-1090) [A on the original padding]. | cloth_ours.cpp:1064-1068 |
| 18 | getThickness | implemented | | cloth_ours.cpp:1069 |
| 19 | getDensity | implemented | Returns the desc value. No setter exists in the header; mass is fixed at build (cloth.cpp:1379). | cloth_ours.cpp:1070 |
| 20 | getRelativeGridSpacing | stored only | Returns the desc value; the solver has no grid parameter from it. | cloth_ours.cpp:1071 |
| 21 | getSolverIterations | implemented | | cloth_ours.cpp:1072-1075 |
| 22 | setSolverIterations | implemented | 0 becomes 1. Live. | cloth_ours.cpp:1076-1082 |
| 23 | getWorldBounds | implemented | Particle bounds grown by thickness. During a job it reports the snapshot taken at simulate. | cloth_ours.cpp:1083-1091 |
| 24 | attachToShape | partial | Queued (PendingOp) and run at the next step with that step's poses; original is immediate. Shapes that are not a collision shape of this cloth are dropped with a log (unsupported). | cloth_ours.cpp:1092-1095; 778-797 |
| 25 | attachToCollidingShapes | partial | Queued; first touching shape per free vertex (shim rule, cloth_ours.cpp:798-816). Recorded with nx=0 (see slot 32). | cloth_ours.cpp:1096-1099; 798-816 |
| 26 | detachFromShape | partial | Queued; shape not in this cloth's shape list is silently ignored (no log). | cloth_ours.cpp:1100-1103; 824-833 |
| 27 | attachVertexToShape | partial | Queued. Refused when the vertex is already shape-attached. A pinned vertex is unpinned [A: core behaviour]. Unknown shape logs unsupported. | cloth_ours.cpp:1104-1108; 834-847 |
| 28 | attachVertexToGlobalPosition | implemented | Pins immediately when the solver is live, or sets the wake-up for the build. Refused for shape-attached vertices. Wakes the cloth (UE3 calls it every frame). | cloth_ours.cpp:1109-1123 |
| 29 | freeVertex | partial | Pin dropped immediately; the shape attachment is released only at the next step (queued). | cloth_ours.cpp:1124-1128 |
| 30 | dominateVertex | stub | Logs unsupported; no effect. | cloth_ours.cpp:1129-1132 |
| 31 | getVertexAttachmentStatus | partial | Reads the shim's records (pins = GLOBAL, att = SHAPE). Queued attaches are not visible until the next step. | cloth_ours.cpp:1133-1138 |
| 32 | getVertexAttachmentShape | partial | Returns the NxShape* the game passed. NULL for attachToCollidingShapes attachments (recorded with nx = 0). | cloth_ours.cpp:1139-1143; 812-813 |
| 33 | getVertexAttachmentPosition | implemented | Pins: stored position. Shape: world position from the shape's pose at the call. | cloth_ours.cpp:1144-1153 |
| 34 | attachToCore | stub | Logs unsupported; no effect. | cloth_ours.cpp:1154-1157 |
| 35 | tearVertex | stub | Logs unsupported; returns false. | cloth_ours.cpp:1158-1162 |
| 36 | raycast | implemented | Own two-sided triangle test over the particle positions, nearest hit, nearest corner id. Snapshot during a job. | cloth_ours.cpp:1228-1243 (rule text 1163-1168) |
| 37 | setGroup | partial | Stores the group. Static shapes were chosen at the first build (staticsDone), so a later change does not re-select statics. Dynamic shapes use the new group per frame. | cloth_ours.cpp:1244-1247; 510-512 |
| 38 | getGroup | implemented | | cloth_ours.cpp:1248-1251 |
| 39 | setGroupsMask | partial | Same as setGroup: statics are not re-selected. | cloth_ours.cpp:1252-1255 |
| 40 | getGroupsMask | implemented | | cloth_ours.cpp:1256-1260 |
| 41 | setMeshData | implemented | Copies the 68-byte NxMeshData; sets the tear cap (maxVertices) live. NxMeshData.flags/name not kept. | cloth_ours.cpp:1261-1267 |
| 42 | getMeshData | implemented | Returns the copy (zeros when none). | cloth_ours.cpp:1268-1273 |
| 43 | setValidBounds | stored only | Logs once. NX_CLF_VALIDBOUNDS has no effect (no particle removal). The game sets the flag and calls this (captures, section 6). | cloth_ours.cpp:1274-1278 |
| 44 | getValidBounds | stored only | Returns the stored box. | cloth_ours.cpp:1279-1282 |
| 45 | setPosition | partial | Writes h->x and forces a full solver rebuild at the next simulate (markRebuild). After a tear the write is silently dropped (log once). | cloth_ours.cpp:1283-1288; 336-347; 561-571 |
| 46 | setPositions | partial | As setPosition, for all vertices; a rebuild per call. | cloth_ours.cpp:1289-1295 |
| 47 | getPosition | implemented | From the solver (or h->x before the build). | cloth_ours.cpp:1296-1300 |
| 48 | getPositions | implemented | Stride defaults to 12. | cloth_ours.cpp:1301-1306 |
| 49 | setVelocity | implemented | Writes the solver velocity directly, no wake-up (as solver note). | cloth_ours.cpp:1307-1317 |
| 50 | setVelocities | partial | Heuristic: a write after a step since the last read is applied as the game's change on top of current velocities (not a plain write). Behaviour differs from the original by design. | cloth_ours.cpp:1318-1336 |
| 51 | getVelocity | implemented | | cloth_ours.cpp:1338-1342 |
| 52 | getVelocities | implemented | Also records the read for the heuristic above. | cloth_ours.cpp:1343-1350 |
| 53 | getNumberOfParticles | implemented | Includes tear copies. | cloth_ours.cpp:1351-1354 |
| 54 | queryShapePointers | stub | Returns 0. The original returns the shape count. | cloth_ours.cpp:1355-1358 |
| 55 | getStateByteSize | stub | Returns 0. | cloth_ours.cpp:1359-1362 |
| 56 | getShapePointers | stub | No-op: does not write the shape array nor the flags. A caller reading them gets uninitialised data. | cloth_ours.cpp:1363-1365 |
| 57 | setShapePointers | stub | Logs unsupported; no effect. | cloth_ours.cpp:1366-1369 |
| 58 | saveStateToStream | stub | Logs unsupported; writes nothing. | cloth_ours.cpp:1370-1373 |
| 59 | loadStateFromStream | stub | Logs unsupported; reads nothing. | cloth_ours.cpp:1374-1378 |
| 60 | setCollisionResponseCoefficient | stored only | Silent (no log). No two-way contact reactions exist (harvest only takes attachment reactions, cloth_ours.cpp:850-863). | cloth_ours.cpp:1379-1382 |
| 61 | getCollisionResponseCoefficient | stored only | | cloth_ours.cpp:1383 |
| 62 | setAttachmentResponseCoefficient | implemented | Applied to the solver (cloth_ours.cpp:1384-1388; build 591). | cloth_ours.cpp:1384-1388 |
| 63 | getAttachmentResponseCoefficient | implemented | | cloth_ours.cpp:1389 |
| 64 | setFromFluidResponseCoefficient | stored only | No cloth-fluid coupling exists anywhere in cloth code. Silent. | cloth_ours.cpp:1390-1393 |
| 65 | getFromFluidResponseCoefficient | stored only | | cloth_ours.cpp:1394 |
| 66 | setToFluidResponseCoefficient | stored only | As 64. | cloth_ours.cpp:1395-1398 |
| 67 | getToFluidResponseCoefficient | stored only | | cloth_ours.cpp:1399 |
| 68 | setExternalAcceleration | implemented | Live. | cloth_ours.cpp:1400-1404 |
| 69 | getExternalAcceleration | implemented | | cloth_ours.cpp:1405-1409 |
| 70 | setMinAdhereVelocity | stored only | Logs unsupported; NX_CLF_ADHERE has no effect. Desc default 1.0 is logged as unsupported on every cloth (captures). | cloth_ours.cpp:1410-1414 |
| 71 | getMinAdhereVelocity | stored only | | cloth_ours.cpp:1415 |
| 72 | setWindAcceleration | implemented | Live; applied along area-weighted vertex normals in the solver (cloth.cpp:1509) [A: sign/normal convention vs the original]. | cloth_ours.cpp:1416-1420 |
| 73 | getWindAcceleration | implemented | | cloth_ours.cpp:1421-1425 |
| 74 | isSleeping | implemented | Solver state, or the stored sleep flag when no solver exists. | cloth_ours.cpp:1426-1429 |
| 75 | getSleepLinearVelocity | implemented | | cloth_ours.cpp:1430 |
| 76 | setSleepLinearVelocity | implemented | Live. | cloth_ours.cpp:1431-1435 |
| 77 | wakeUp | implemented | Solver if live, else stored for the build. | cloth_ours.cpp:1436-1444 |
| 78 | putToSleep | implemented | Zeroes velocities (solver or stored). | cloth_ours.cpp:1445-1454 |
| 79 | setFlags | partial | Per-flag live updates. Changing NX_CLF_DISABLE_COLLISION rebuilds, but statics and body shapes stay in the solver when it is turned on (section 4, item 3). Game calls it often (captures). | cloth_ours.cpp:1455-1476 |
| 80 | getFlags | implemented | Returns stored flags. | cloth_ours.cpp:1477-1480 |
| 81 | setName | implemented | Copies the string. The header says the SDK stores the pointer [V: NxClothDesc.h:698-701]; a pointer-based getter would differ [A]. | cloth_ours.cpp:1481-1486 |
| 82 | addForceAtVertex | implemented | Applies to the running solver's velocities at the call (dt = scene's last substep). Dropped (log) while a rebuild is pending; ignored for NX_CLF_STATIC. | cloth_ours.cpp:1503-1508; 1487-1494 |
| 83 | addForceAtPos | implemented | As 82. Mass floor is ours, not the original (cloth.cpp:1416-1419). | cloth_ours.cpp:1509-1514 |
| 84 | addDirectedForceAtPos | implemented | As 82. Game calls it for bullet hits (captures). | cloth_ours.cpp:1515-1522 |
| 85 | overlapAABBTriangles | stub | Sets nb = 0, returns false, logs unsupported. | cloth_ours.cpp:1523-1529 |
| 86 | getScene | implemented | Returns the NxScene* given at create. | cloth_ours.cpp:1530-1533 |
| 87 | getName | implemented | | cloth_ours.cpp:1534-1537 |
| 88 | getCompartment | stub | Always 0 (no hardware compartments). | cloth_ours.cpp:1538-1541 |
| 89 | getPPUTime | stub | Always 0. | cloth_ours.cpp:1542-1545 |
| 90 | getForceFieldMaterial | stub | Always 0; the desc value is not read (section 2). | cloth_ours.cpp:1546-1549 |
| 91 | setForceFieldMaterial | stub | Logs unsupported; no effect. | cloth_ours.cpp:1550-1554 |

Also: `NxCloth::userData` is a public data member at object offset +4, after the vtable pointer
(NxCloth.h:1573-1574, the class has no other data) [V header]. In the hosted object offset +4 is `Hosted* h`
(cloth_ours.cpp:111-114). The game reading or writing `cloth->userData` would see or overwrite the shim pointer. See
section 4, item 1.

### 1b. NxClothMesh (3 slots; core-owned objects, nothing hosted)

| slot | method | status | what is missing or differs | file:line |
|---|---|---|---|---|
| 0 | ~NxClothMesh | pass-through | Core object, not hooked. | (none) |
| 1 | saveToDesc (const) | pass-through | Not hooked; the shim calls it itself to read the mesh (readMesh) and to log it (createClothMesh). 2.8.0 slot 1 is unnamed in the export [A]. | cloth_ours.cpp:1617-1626 (call at 1623); cloth_wrap.cpp:424-428 |
| 2 | getReferenceCount | pass-through | Core object, not hooked. | (none) |

### 1c. NxPhysicsSDK, cloth-mesh methods

| slot | method | status | what is missing or differs | file:line |
|---|---|---|---|---|
| 21 | createClothMesh(NxStream&) | pass-through (tap) | Forwards to the core with a StreamTap that records the bytes the core reads (the cooked stream). Hooked only when the core is 2.8.0 or 2.8.1 and PXSHIM_CLOTH_MESH is not 0. The IStream class copies the 2.8.1 header order [A for 2.8.0; the export confirms the signature, cloth_wrap.cpp:6-9]. | cloth_wrap.cpp:60, 366-438, 460-473 |
| 22 | releaseClothMesh(NxClothMesh&) | pass-through (logging) | Forwards; logs. | cloth_wrap.cpp:440-446 |
| 23 | getNbClothMeshes | not hooked | Core list (fine: meshes are core objects) [A slot]. | (none) |
| 24 | getClothMeshes | not hooked | Core list [A slot]. | (none) |

### 1d. NxScene, cloth methods

| slot | method | status | what is missing or differs | file:line |
|---|---|---|---|---|
| 131 | createCloth(const NxClothDesc&) | implemented (hosted; core fallback) | No NxClothDesc::isValid() check (the header says it must hold, NxScene.h:2885-2886); a non-null compartment and NX_CLF_HARDWARE are ignored; hosted cloths are not in the core lists (see 133/134). | cloth_wrap.cpp:332-348, 59; cloth_ours.cpp:1819-1863; PhysXLoader.cpp:231 |
| 132 | releaseCloth(NxCloth&) | implemented (hosted; core fallback) | Hosted: joins the job, then unregisters and writes the recording. Core cloths: forwarded. | cloth_wrap.cpp:350-362; cloth_ours.cpp:1865-1873 |
| 133 | getNbCloths | not hooked | Forwards to the core: hosted cloths are missing from the count [V: hosted cloths never reach the core, cloth_wrap.cpp:337-342]. 2.8.0 slot unnamed [A]. | (none) |
| 134 | getCloths | not hooked | As 133: array lacks hosted cloths. | (none) |
| 95 | setClothUserNotify | not hooked | Forwards. No code in shim/ or solver/ calls the user notify, so hosted cloths never report to it [V: grep]. | (none) |
| 96 | getClothUserNotify | not hooked | Forwards. 2.8.0 slot unnamed [A]. | (none) |

Count check (2.8.1 header declaration indices, computed by counting `virtual` lines in class NxScene/NxPhysicsSDK, which
reproduce 131/132 and 21/22 [V]); 133/134/95/96 and NxPhysicsSDK 23/24 by the same count [V 2.8.1 header, A 2.8.0].

## 2. Descriptor fields

Offsets are the byte offsets the shim reads (kOff* in shim/cloth_ours.cpp:74-79; cloth_wrap.cpp:68-74 for the logger).
They were checked against the 2.8.1 layout: NxMat34 = 3x3 + vec3 (48 bytes), so pose 4..51, thickness at 52, desc size
280 [V: NxClothDesc.h field order; sizes from NxMat34.h, NxMat33.h, Nx9F32.h]. The 2.8.0 layout is the same for these
fields [V: docs/re-notes-cloth.md:18-31].

### 2a. NxClothDesc

| offset | field | status | what is missing or differs | file:line |
|---|---|---|---|---|
| 0 | clothMesh | implemented | Read; the mesh is read through saveToDesc (readMesh). A null or unreadable mesh refuses the hosted path. | cloth_ours.cpp:1830-1836; 1617-1672 |
| 4..51 | globalPose (NxMat34) | implemented | Read at create only. Rotation read row-major (section 5). Applied to the rest vertices to give the start positions; the solver's rest stays in mesh frame. | cloth_ours.cpp:1711-1721 |
| 52 | thickness | implemented | Read; used by contacts (cloth.cpp:655, 1460, 1576), pins, bounds. | cloth_ours.cpp:1676 |
| 56 | density | implemented | Read; mass per corner (cloth.cpp:1379) and force mass floor (cloth.cpp:1419). Fixed at build. | cloth_ours.cpp:1677 |
| 60 | bendingStiffness | implemented | Read; used when NX_CLF_BENDING (cloth.cpp:903-915). | cloth_ours.cpp:1678 |
| 64 | stretchingStiffness | implemented | Read; used by the edge solve. | cloth_ours.cpp:1679 |
| 68 | dampingCoefficient | implemented | Read; used with NX_CLF_DAMPING (cloth.cpp:1612). | cloth_ours.cpp:1680 |
| 72 | friction | implemented | Read; cloth.cpp:1483. | cloth_ours.cpp:1681 |
| 76 | pressure | stored only | Read into X.pressure; not used (no pressure in the solver). | cloth_ours.cpp:1690 |
| 80 | tearFactor | implemented | Read; tearStep (cloth.cpp:1044). | cloth_ours.cpp:1682 |
| 84 | collisionResponseCoefficient | stored only | Read; no two-way contact reactions. | cloth_ours.cpp:1693 |
| 88 | attachmentResponseCoefficient | implemented | Read; solver attachment response (build, cloth_ours.cpp:591). | cloth_ours.cpp:1692 |
| 92 | attachmentTearFactor | implemented | Read; build (cloth_ours.cpp:592). | cloth_ours.cpp:1691 |
| 96 | toFluidResponseCoefficient | stored only | Read; no fluid coupling. Logged as "fluid responses" when > 0 (default 1.0). | cloth_ours.cpp:1695; 1735-1736 |
| 100 | fromFluidResponseCoefficient | stored only | As 96. | cloth_ours.cpp:1694 |
| 104 | minAdhereVelocity | stored only | Read; NX_CLF_ADHERE not implemented. Logged on every cloth (default 1.0). | cloth_ours.cpp:1696; 1735 |
| 108 | solverIterations | implemented | Read; 0 becomes 1. | cloth_ours.cpp:1683 |
| 112 | externalAcceleration | implemented | Read into P; live. | cloth_ours.cpp:1688 |
| 124 | windAcceleration | implemented | Read into P; used (cloth.cpp:1509). | cloth_ours.cpp:1689 |
| 136 | wakeUpCounter | implemented | Read (default 0.4 when negative, cloth_ours.cpp:1687); used by wake/attach calls. Not used to decide the initial sleep state [A]. | cloth_ours.cpp:1687 |
| 140 | sleepLinearVelocity | implemented | Read; -1 maps to 0.15 [A: SDK default]. | cloth_ours.cpp:1686 |
| 144..211 | meshData (NxMeshData, 68 bytes) | implemented | Copied; drives output writes (section 2c) and the tear cap. | cloth_ours.cpp:1705-1707 |
| 212 | collisionGroup | implemented | Read as 16 bits; used by the collision filter (filterOf, cloth_ours.cpp:490-498). | cloth_ours.cpp:1700 |
| 216..231 | groupsMask (NxGroupsMask, 4 x u32) | implemented | Read; used by the filter with scene filter ops. Statics chosen at first build only. | cloth_ours.cpp:1701 |
| 232 | forceFieldMaterial | not read | Not read; getForceFieldMaterial returns 0. Non-zero desc values are lost. | (no read; the logger reads it at cloth_wrap.cpp:188 only) |
| 236..259 | validBounds (NxBounds3) | stored only | Read into validBounds; NX_CLF_VALIDBOUNDS logged as ignored; no particle removal. Game sets validBounds in captures. | cloth_ours.cpp:1708; 1731 |
| 260 | relativeGridSpacing | stored only | Read; not used by the solver's own broad phase. | cloth_ours.cpp:1697 |
| 264 | flags | implemented | Read; applyFlags (cloth_ours.cpp:319-327); per flag in section 3a. | cloth_ours.cpp:1698-1699 |
| 268 | userData | stored only | Read into Hosted::userData (not exposed, not at object +4). See section 4, item 1. | cloth_ours.cpp:1702 |
| 272 | name | implemented | Read as a string and copied (IsBadStringPtrA guard). | cloth_ours.cpp:1704 |
| 276 | compartment | not read | Not read; hardware compartments unsupported. | (no read) |

Not called: NxClothDesc::isValid (see 1d/131).

### 2b. NxClothMeshDesc (read via the core's saveToDesc into a 48-byte buffer; inherits NxSimpleTriangleMesh)

Offsets are the 2.8.1 layout (28-byte NxSimpleTriangleMesh base, then 20 bytes), checked against the header
[V: NxSimpleTriangleMesh.h base fields; NxClothMeshDesc.h:79-106]. Shim indices: md[k] = byte offset 4k.

| offset | field | status | what is missing or differs | file:line |
|---|---|---|---|---|
| 0 | numVertices | implemented | Read; bound checks (<= 2^20). | cloth_ours.cpp:1621-1628 |
| 4 | numTriangles | implemented | Read. | cloth_ours.cpp:1628 |
| 8 | pointStrideBytes | implemented | Read (default 12). | cloth_ours.cpp:1628-1629 |
| 12 | triangleStrideBytes | implemented | Read (default 12). | cloth_ours.cpp:1628-1629 |
| 16 | points | implemented | Read via saveToDesc (the cooked order, not the original input order). | cloth_ours.cpp:1643-1646 |
| 20 | triangles | implemented | Read; indices checked against numVertices. | cloth_ours.cpp:1648-1650 |
| 24 | flags (NxMeshFlags + NX_CLOTH_MESH_*) | partial | Only NX_CLOTH_MESH_TEARABLE is read (cloth_ours.cpp:1630). NX_MF_FLIPNORMALS, NX_MF_16_BIT_INDICES, NX_MF_HARDWARE_MESH and NX_CLOTH_MESH_WELD_VERTICES are not used [A: cooked output already has them applied]. | cloth_ours.cpp:1630 |
| 28 | vertexMassStrideBytes | not read | Vertex masses ignored; mass comes from density only. | (no read) |
| 32 | vertexFlagStrideBytes | implemented | Read; stride default 4. | cloth_ours.cpp:1633-1635 |
| 36 | vertexMasses | not read | Ignored (see 28). | (no read) |
| 40 | vertexFlags | partial | NX_CLOTH_VERTEX_ATTACHED: pinned at creation (cloth_ours.cpp:1639, 1838-1839). NX_CLOTH_VERTEX_TEARABLE: only counted and logged (cloth_ours.cpp:1640, 1840-1842). | cloth_ours.cpp:1633-1642 |
| 44 | weldingDistance | not read | Welding not applied on the shim side [A: cooker applies it before saveToDesc]. | (no read) |

### 2c. NxMeshData (NxClothDesc.meshData, 68 bytes; output buffers)

| offset (in desc: 144 + k) | field | status | what is missing or differs | file:line |
|---|---|---|---|---|
| 0 | verticesPosBegin | implemented | Positions written at fetch (positions only if non-null). | cloth_ours.cpp:605-620 |
| 4 | verticesNormalBegin | implemented | Area-weighted vertex normals written at fetch [A: normal definition vs original]. | cloth_ours.cpp:621-643 |
| 8 | verticesPosByteStride | implemented | Default 12 when 0. | cloth_ours.cpp:616-618 |
| 12 | verticesNormalByteStride | implemented | Default 12. | cloth_ours.cpp:621-624 |
| 16 | maxVertices | implemented | Caps output and the tear count (tearCapOf, cloth_ours.cpp:350-353). | cloth_ours.cpp:611-615; 1707 |
| 20 | numVerticesPtr | implemented | Written with the count. | cloth_ours.cpp:639 |
| 24 | indicesBegin | implemented | Triangle indices written (capped at maxIndices). | cloth_ours.cpp:644-648 |
| 28 | indicesByteStride | implemented | Default 4. | cloth_ours.cpp:645-646 |
| 32 | maxIndices | implemented | Index cap; a truncated index list can leave partial triangles [A]. | cloth_ours.cpp:641-643 |
| 36 | numIndicesPtr | implemented | Written. | cloth_ours.cpp:649 |
| 40 | parentIndicesBegin | implemented | Rest vertex each particle came from (tear copies included). | cloth_ours.cpp:651-659 |
| 44 | parentIndicesByteStride | implemented | Default 4. | cloth_ours.cpp:652-654 |
| 48 | maxParentIndices | implemented | Cap. | cloth_ours.cpp:655-656 |
| 52 | numParentIndicesPtr | implemented | Written. | cloth_ours.cpp:660 |
| 56 | dirtyBufferFlagsPtr | implemented | Dirty bits (pos 1, normal 2, indices 4, parent 8) OR-ed in. | cloth_ours.cpp:663 |
| 60 | flags | not read | Ignored. | (no read) |
| 64 | name | not read | Ignored. | (no read) |

## 3. Flags

### 3a. NxClothFlag (NxClothDesc.h:26-302). Shim constants: cloth_ours.cpp:87-92.

Values in the table are the header's `1<<n`. Flags can also change at run time through setFlags (cloth_ours.cpp:1455).

| flag | value | status | what is missing or differs | file:line |
|---|---|---|---|---|
| NX_CLF_PRESSURE | 1<<0 | not implemented (logged) | No pressure model. Logged as "pressure" at create. | cloth_ours.cpp:1727 |
| NX_CLF_STATIC | 1<<1 | implemented | Whole step skipped, velocities kept; forces ignored. Game toggles it (captures). | cloth_ours.cpp:875-878; 1488 |
| NX_CLF_DISABLE_COLLISION | 1<<2 | partial | At create: no statics or dynamic shapes (cloth_ours.cpp:512, 690, 714). Set at run time: statics and body shapes stay in the solver (section 4, item 3). | cloth_ours.cpp:512; 690; 714; 1464-1465 |
| NX_CLF_SELFCOLLISION | 1<<3 | implemented | Particle-particle prepass (solver useSelfCollision, cloth.cpp:1504; cloth_self.cpp). Also see the always-on particle-triangle self contact (section 4, item 2). | cloth_ours.cpp:326 (applyFlags); cloth.cpp:1504 |
| NX_CLF_VISUALIZATION | 1<<4 | not read | Debug drawing; no shim effect expected. | (none) |
| NX_CLF_GRAVITY | 1<<5 | implemented | useGravity (cloth.cpp:1506). | cloth_ours.cpp:320 |
| NX_CLF_BENDING | 1<<6 | implemented | useBending (cloth.cpp:903, 910). | cloth_ours.cpp:321 |
| NX_CLF_BENDING_ORTHO | 1<<7 | implemented | Dihedral bending (cloth.cpp:904, 910-). | cloth_ours.cpp:322 |
| NX_CLF_DAMPING | 1<<8 | implemented | Velocity damping (cloth.cpp:1612-1620). | cloth_ours.cpp:323 |
| NX_CLF_COLLISION_TWOWAY | 1<<9 | not implemented (logged) | No reactions from contacts (only attachments). | cloth_ours.cpp:1730 |
| NX_CLF_TRIANGLE_COLLISION | 1<<11 | not implemented (logged) | Header says not supported in this release (NxClothDesc.h:174-185). Logged. | cloth_ours.cpp:1729 |
| NX_CLF_TEARABLE | 1<<12 | implemented | useTearing = flag AND mesh TEARABLE (applyFlags; readMesh 1630). Tear cap from meshData.maxVertices. | cloth_ours.cpp:325; cloth.cpp:1040, 1621 |
| NX_CLF_HARDWARE | 1<<13 | not read | No PPU/hardware path; a hardware cloth is hosted anyway [A]. | (none) |
| NX_CLF_COMDAMPING | 1<<14 | implemented | useComDamping (cloth.cpp:1520). | cloth_ours.cpp:324 |
| NX_CLF_VALIDBOUNDS | 1<<15 | not implemented (logged) | No particle removal by validBounds. Game sets this bit at run time (0x8000 in captured setFlags). | cloth_ours.cpp:1731 |
| NX_CLF_FLUID_COLLISION | 1<<16 | not implemented (logged) | No cloth-fluid collision. | cloth_ours.cpp:1732 |
| NX_CLF_DISABLE_DYNAMIC_CCD | 1<<17 | implemented | disableDynamicCcd (cloth.cpp:562, 720). | cloth_ours.cpp:93, 327 |
| NX_CLF_ADHERE | 1<<18 | not implemented (logged) | minAdhere not used. | cloth_ours.cpp:1733 |

applyFlags is cloth_ours.cpp:319-328 (one line per flag, 320-327).

### 3b. NX_CLOTH_MESH_* and NxMeshFlags (NxClothMeshDesc.h:22-41; NxSimpleTriangleMesh.h:19-37)

| flag | value | status | what is missing or differs | file:line |
|---|---|---|---|---|
| NX_CLOTH_MESH_TEARABLE | 1<<8 | implemented | Gates useTearing with NX_CLF_TEARABLE. | cloth_ours.cpp:93, 1630 |
| NX_CLOTH_MESH_WELD_VERTICES | 1<<9 | not read | Welding is the cooker's job; not checked shim-side [A]. | (none) |
| NX_MF_FLIPNORMALS | 1<<0 | not read | Triangle output is the cooked order; not re-flipped [A]. | (none) |
| NX_MF_16_BIT_INDICES | 1<<1 | not read | Shim copies NxSimpleTriangleMesh output as u32 [V: readMesh 1621-1650]. | (none) |
| NX_MF_HARDWARE_MESH | 1<<2 | not read | Hardware only. | (none) |

### 3c. NxClothVertexFlags (NxClothMeshDesc.h:49-61)

| flag | value | status | what is missing or differs | file:line |
|---|---|---|---|---|
| NX_CLOTH_VERTEX_ATTACHED | 1<<0 | implemented | Vertex pinned at its start position at creation (global attachment). | cloth_ours.cpp:96-97, 1639, 1838-1839 |
| NX_CLOTH_VERTEX_TEARABLE | 1<<7 | partial | Counted and logged only ("%u tearable [A: flags come from saveToDesc]"); no per-vertex tearable behaviour. | cloth_ours.cpp:97, 1640, 1840-1842 |

### 3d. NxClothAttachmentFlag (NxClothDesc.h:307-330) and vertex attachment status (NxClothDesc.h:335-354)

| item | value | status | what is missing or differs | file:line |
|---|---|---|---|---|
| NX_CLOTH_ATTACHMENT_TWOWAY | 1<<0 | implemented | Solver twoWay (cloth_attach.cpp:15, 98); two-way reactions go to dynamic bodies only (harvest, cloth_ours.cpp:850-863). Kinematic and static targets get none. | cloth_attach.cpp:15, 98; cloth_ours.cpp:850-863 |
| NX_CLOTH_ATTACHMENT_TEARABLE | 1<<1 | implemented | Solver tearable (cloth_attach.cpp:15, 97; attachTearEdge cloth_attach.cpp:198); dropped records after tear (harvest, cloth_ours.cpp:866-869). | cloth_attach.cpp:97, 198; cloth_ours.cpp:866-869 |
| NX_CLOTH_VERTEX_ATTACHMENT_NONE | 0 | implemented | Returned when no pin or record. | cloth_ours.cpp:98, 1137 |
| NX_CLOTH_VERTEX_ATTACHMENT_GLOBAL | 1 | implemented | Pins. | cloth_ours.cpp:97-98, 1135 |
| NX_CLOTH_VERTEX_ATTACHMENT_SHAPE | 2 | partial | Only for vertices with a record; queued ops invisible until the step (1d/31). | cloth_ours.cpp:98, 1136 |

### 3e. NxForceMode (Nxp.h:313-322, used by NxCloth force calls 82-84)

| mode | value | status | what is missing or differs | file:line |
|---|---|---|---|---|
| NX_FORCE | 0 | implemented | v += f invMass dt (per-mode code, cloth.cpp:1391-1446). | cloth.cpp:1391-1446 |
| NX_IMPULSE | 1 | implemented | v += f invMass; divided by summed mass for AtPos. | cloth.cpp:1391-1446 |
| NX_VELOCITY_CHANGE | 2 | implemented | v += f. | cloth.cpp:1391-1446 |
| NX_SMOOTH_IMPULSE | 3 | stub | Does nothing, as the solver comment says for the original (cloth.h External forces comment) [A on the original]. | cloth.cpp:1391-1446 |
| NX_SMOOTH_VELOCITY_CHANGE | 4 | stub | As 3. | cloth.cpp:1391-1446 |
| NX_ACCELERATION | 5 | implemented | v += f dt. | cloth.cpp:1391-1446 |

## 4. Known behavioural differences (code comments, grep, and reading)

Grep for TODO/FIXME/XXX/HACK in shim/cloth_*, shim/PhysXLoader.cpp and solver/cloth: none [V]. The explicit
"not supported/ignored/unsupported" sites are in section 1-3. Further differences (documented in comments as "ours"
or "not in the original", or found while reading):

1. NxCloth::userData overlaps the shim pointer. Hosted object = {vt, Hosted*} (cloth_ours.cpp:111-114); the header's
   public userData member sits at +4 (NxCloth.h:1573-1574). The desc userData is kept in Hosted::userData (cloth_ours.cpp:1702)
   and never written to +4. Game use of `->userData` would hit the shim pointer (a write would break the cloth) [A: whether
   the game touches the member directly].
2. Particle-triangle self contact is always on for every hosted cloth, not gated by NX_CLF_SELFCOLLISION: setSelfContact
   runs unconditionally at create (cloth_ours.cpp:1837; body 1796-1818; default frac 0.2 at 1798-1800). Not in the original
   (solver/cloth/cloth_selftri.cpp:1-2; cloth.h:45-47).
3. NX_CLF_DISABLE_COLLISION set at run time (setFlags, cloth_ours.cpp:1455-1465): the rebuild re-adds every known static
   and body slot (build, cloth_ours.cpp:593), statics are not re-collected (staticsDone, 510-512), and body slots are
   no longer moved or parked (syncShapes returns at cloth_ours.cpp:714). The cloth keeps colliding with statics and with the
   bodies' last poses. At create time this flag works (no statics collected, 512).
4. Group changes after the first build do not re-select statics: setGroup/setGroupsMask only store (cloth_ours.cpp:1244-1256;
   staticsDone at 510-512).
5. setPosition/setPositions after a tear are silently dropped: build keeps a torn solver and skips the rebuild
   (cloth_ours.cpp:561-571); posOf then returns the solver's positions (posOf, cloth_ours.cpp:331). Log once: "torn,
   parameter/position changes not applied". Parameters set in the same case still go to the running solver (setLive).
6. Every rebuild (setPosition(s), and a collision on/off change in setFlags) makes a new RecCloth (build,
   cloth_ours.cpp:561-604). Solver-internal memory (self-contact pairs, stuck counters, depenetration state) resets
   [A: effect size].
7. Attach, detach and free are queued to the next step (cloth_ours.cpp:1092-1128; processPending 778-842). Query results
   (31-33) lag one step; the original applies at call time [A on the original's timing; the header comment says so, 24-25].
8. Force calls with a pending rebuild are dropped with a log (cloth_ours.cpp:1487-1494). Forces also ignore NX_CLF_STATIC
   (1488).
9. Step timing: P.refDt = 1/62 s by default (cloth_ours.cpp:1684; PhysXLoader.cpp:62-70). Coefficients are rescaled to the
   game's frame rate, so stiffness and damping differ from the original's per-step behaviour by design (cloth.h:40-44;
   cloth.cpp:1486-1500).
10. Depenetration at refDt is ours (cloth.cpp:1533-1540). Post-solve mesh sweep is "not in the original" (cloth.cpp:1570).
11. Force mass floor is ours (cloth.cpp:1416-1419); a cloth that cannot tear gets a speed cap ("ours, not in the original",
    cloth.cpp:1422-1427).
12. Velocity-write heuristic (setVelocities after a step since the read is applied as a change) is ours (cloth_ours.cpp:1318-1336).
13. Normals are area-weighted, definition [A] (cloth_ours.cpp:621-643).
14. Sleep defaults: sleepLinearVelocity 0.15 and wakeUp 0.4 when the desc value is negative are [A] (cloth.h:31;
    cloth_ours.cpp:1686-1687).
15. Descriptor not validated: no NxClothDesc::isValid (cloth_ours.cpp:1819-1863; NxScene.h:2885-2886 requires it).
16. Hosted cloths invisible to the core: getNbCloths/getCloths (133/134) and NxClothUserNotify (95/96) do not see them
    (cloth_wrap.cpp:337-342 never hands a hosted cloth to the core; no user-notify code in shim/ or solver/ [V grep]).
17. Mesh vertex masses ignored; mass from density only (readMesh 1617-1672; cloth.cpp:1379).
18. Pressure, validBounds, minAdhere, triangle and fluid collision, two-way contact reactions and hardware are not
    implemented (section 1-3; logs at cloth_ours.cpp:1724-1737).
19. Recording: PXSHIM_CLOTH_REC defaults on, writing PhysXLoader_cloth<NNN>.pxcr next to the log (cloth_ours.cpp:1742-1749,
    writeRec 1744-1749). Runtime side effect, not a behaviour difference.
20. Log messages present in older captures that the current code no longer emits: "dynamic or kinematic mesh/convex
    shape near a cloth (not in the solver)", "addDirectedForceAtPos (the solver has no force input)", "NxCloth::raycast is
    not supported", "attachToShape (needs the NxShape geometry)". grep of shim/ and solver/ finds none of them [V]. The
    captures are from an older build; do not read them as current gaps.

## 5. globalPose: matrix order (current code)

- Descriptor pose (NxClothDesc.globalPose, NxMat34 = NxMat33 M + NxVec3 t): M is read as row-major, i.e. M[3r+c] is row r,
  column c. Basis: Nx9F32.h:27-41 defines m[3][3] with the _11.._33 order when TRANSPOSED_MAT33 is not defined, and
  the shim keeps that assumption [V: comment, cloth_ours.cpp:1711-1713; the macro is not defined in this build's headers
  as far as the shim knows, A for the game's build].
- Applied: x' = M x + t for each rest vertex: cloth_ours.cpp:1714-1721 (`M[0]*r.x + M[1]*r.y + M[2]*r.z + t.x` etc.).
  So the rotation is R r (not Rᵀ r) [V].
- Fix history: the committed code (HEAD 9243aad, and acbe2dd before it) read M column-major (HEAD line 1492-1499:
  `M[0]*r.x + M[3]*r.y + M[6]*r.z` ...). The working tree changes it to row-major (uncommitted; `git diff
  shim/cloth_ours.cpp`). The comment in the working tree records the banners that were created mirrored and shredded
  on the game's first re-pin [V: cloth_ours.cpp:1711-1713]. Current code = row-major.
- Shape poses (scene_read Pose, used for statics and bodies): poseOf writes R[3r+k] = p.m[r][k] (row-major)
  (cloth_ours.cpp:426-432). localOf/worldOf use the same row-major R (cloth_ours.cpp:434-450). Body poses in syncShapes go
  through poseOf (cloth_ours.cpp:750). Consistent with the descriptor path [V].
- Solver API: ShapePose (cloth.h:127) documents R row-major (r[3*row + col]); setShapeMotion (cloth.h:182) takes the same layout.

## 6. Game usage seen in captured logs (evidence for the gap ranking)

From captures/game-run/20261011-010124/Binaries_shim.log and captures/game-run/20261011-001011/Binaries_shim.log
(cloth trace lines, PXSHIM_CLOTH_TRACE=1 runs) [V]:
- Runtime setFlags values include 0x8000 (NX_CLF_VALIDBOUNDS), 0x0002 (STATIC toggling), 0x0008 (SELFCOLLISION), 0x1000
  (TEARABLE). The game sets VALIDBOUNDS at run time while setValidBounds is also called (stored only).
- Called in those runs: setPressure, setValidBounds, setCollisionResponseCoefficient, setAttachmentResponseCoefficient,
  setSleepLinearVelocity, putToSleep, setMeshData, addDirectedForceAtPos (about 89 trace events), raycast (about 500),
  getWorldBounds, getVelocities/setVelocities, getFlags/setFlags, getScene.
- Captured desc flags at create: 0x00000020 (275 desc lines) and 0x00001020 (630): gravity, plus tearable. The desc notes
  show no validBounds; 38 logs list selfCollision among the desc's unsupported fields (captures/game-run/*/Binaries_shim.log).

## 7. Top gaps at audit time

1. NxCloth::userData (public member at +4) collides with Hosted* in the hosted object: cloth_ours.cpp:111-114; NxCloth.h:1573-1574 [A usage].
2. NX_CLF_VALIDBOUNDS and setValidBounds are stored only, yet the game sets the flag and calls the setter: cloth_ours.cpp:1274-1278, 1731, 1708 (section 6).
3. setPressure stored only (NX_CLF_PRESSURE not modelled), and the game calls it: cloth_ours.cpp:1046-1050, 1727 (section 6).
4. setFlags(NX_CLF_DISABLE_COLLISION) at run time keeps statics and freezes body shapes (still colliding): cloth_ours.cpp:593, 714, 1455-1465.
5. No two-way contact reactions (NX_CLF_COLLISION_TWOWAY and setCollisionResponseCoefficient): harvest uses attachments only, cloth_ours.cpp:850-863; coefficient stored at 1379-1382 and 1693.
6. Attach/detach/free are applied at the next step, so 31-33 lag one step: cloth_ours.cpp:1092-1128; 778-842; 1133-1153.
7. getVertexAttachmentShape returns NULL for attachToCollidingShapes attachments (nx = 0): cloth_ours.cpp:812-813 (recordAttach 0); 1139-1143.
8. Hosted cloths are absent from NxScene::getNbCloths/getCloths and from NxClothUserNotify: cloth_wrap.cpp:332-342 (no core call), slots 133/134/95/96 unhooked.
9. After a tear, setPosition(s) are dropped silently (log once): cloth_ours.cpp:561-571, 1283-1295.
10. Stubs that return 0 or nothing where the original returns data: queryShapePointers, getStateByteSize, getShapePointers, getCompartment, getForceFieldMaterial, saveToDesc, overlapAABBTriangles, saveStateToStream/loadStateFromStream: cloth_ours.cpp:1010-1016, 1355-1378, 1523-1529, 1538-1553.
11. Minor: no isValid on create (cloth_ours.cpp:1819-1863), vertex masses and welding ignored (1617-1672), forceFieldMaterial and compartment not read (section 2a), setGroup/setGroupsMask do not re-select statics (cloth_ours.cpp:1244-1256).

## 8. Counts

Methods (section 1, counted per table):
- NxCloth (1a, 92 slots): implemented 50, partial 13, stored only 13, stub 16 (sum 92).
- NxClothMesh (1b, 3): pass-through 3.
- NxPhysicsSDK cloth-mesh (1c, 4): pass-through 2, not hooked 2.
- NxScene cloth (1d, 6): implemented (hosted, core fallback) 2, not hooked 4.

Descriptors (section 2): NxClothDesc (2a, 31): implemented 21, stored only 8, not read 2. NxClothMeshDesc (2b, 12):
implemented 7, partial 2, not read 3. NxMeshData (2c, 17): implemented 15, not read 2.

Flags and enums (section 3, 36 rows): implemented 19, partial 3, stub 2 (NxForceMode smooth modes), not implemented
(logged) 6, not read 6.

Status words: "implemented" in 1a means the behaviour exists in the solver or is an exact getter/setter. "partial" lists
the rows in section 4 items 3-7.

## Independent review (spot-checks)
- Gap 1 (userData): confirmed that the object handed to the game is {vtable, Hosted*} (cloth_ours.cpp:111-114), so a game write to the public NxCloth::userData (+4) would overwrite our Hosted pointer. Whether MirrorsEdge.exe writes or reads it is for B2 to check.
- Gaps 2/3 (setValidBounds, setPressure stored only): confirmed (cloth_ours.cpp:1046-1050, 1274-1278). Run 20261011-010124's log shows the game calling both (o_setPressure x11 and o_setValidBounds x11 in a trace window, once per cloth created).
