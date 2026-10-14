// Hosted cloth (default on; PXSHIM_CLOTH=off disables): NxScene::createCloth objects are built on solver/cloth instead of the core.
// Used by cloth_wrap.cpp (the NxScene and NxCloth entry points) and PhysXLoader.cpp (simulate/fetch/scene release).
#pragma once

#include <stddef.h>
#include <stdint.h>

namespace shim {

void oursInit();     // reads PXSHIM_CLOTH (called by clothInit)
bool oursEnabled();

// createClothMesh tap (cloth_wrap.cpp): the cooked stream the core read for NxClothMesh `mesh`.
void oursRegisterCookedMesh(uint32_t mesh, const uint8_t* bytes, size_t n);

// NxScene::createCloth. Returns our NxCloth object, or 0 when the cloth cannot be hosted (the core then creates it).
uint32_t oursCreate(void* scene, const void* desc);
bool oursOwns(uint32_t cloth);             // one of our NxCloth objects
void oursRelease(uint32_t cloth);          // NxScene::releaseCloth for one of ours
void oursOnSimulate(void* scene, float elapsed);  // before the core's simulate: steps the scene's hosted cloths
void oursOnFetched(void* scene, bool ok);         // after the core's fetchResults: writes the receive buffers
void oursOnSceneRelease(void* scene);
void oursLogSummary(const char* why);
void oursStatsLine(char* buf, size_t size);  // the cloth stepping costs since the last call, then reset (buf "" when off)

}  // namespace shim
