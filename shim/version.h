// Shatter version (the DLL's version resource and the log header).
#pragma once
#define SHATTER_VERSION_MAJOR 1
#define SHATTER_VERSION_MINOR 0
#define SHATTER_VERSION_PATCH 7
#define SHATTER_STR2(x) #x
#define SHATTER_STR(x) SHATTER_STR2(x)
#define SHATTER_VERSION_STR \
  SHATTER_STR(SHATTER_VERSION_MAJOR) "." SHATTER_STR(SHATTER_VERSION_MINOR) "." SHATTER_STR(SHATTER_VERSION_PATCH)

// The MirrorsEdge.exe build the game-side hooks (shim/game_hooks.cpp) were made for: GOG 1.0.1.0, PE timestamp.
#define SHATTER_GAME_EXE_TIMESTAMP 0x4965FFF1u
