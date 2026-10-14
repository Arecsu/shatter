// Game-side hooks for MirrorsEdge.exe (vtable slots of the game's own classes), only for the exe build the addresses
// were taken from (GOG 1.0.1.0, PE timestamp SHATTER_GAME_EXE_TIMESTAMP); any other build, or a slot that does not hold the expected function,
// leaves the game untouched.
//
// Slide steering fix (off by default, it changes player movement; slide_steering_fix = on in shatter.ini, the
// installer's Game settings checkbox, or PXSHIM_SLIDEFIX=1): on steep slide
// surfaces (the chapter 1 building slide) left/right steering is applied by the current move's tick (0x01208D70) only
// while a pawn flag (+0x41E bit 0) is set. performPhysics (0x012B0960) clears it at the start of every step and the
// player's falling physics (0x012C0980, via 0x012BEB80) sets it again, with the surface normal at +0x71C, when that step
// touches a slide surface. At 62 fps the pawn touches the slope every frame; at high frame rates it floats a fraction of
// a unit above it between touches (300 fps: 27 touches/s, gaps up to ~165 ms, at most ~0.5 units off the plane) and
// steering works only on touch frames, so it all but disappears [V: runs 20261013-120725/120804]. The hops start on the
// touching step itself, which leaves the pawn moving off the slope (300 fps: +22 to +50 units/s along the normal; 62 fps:
// about -1). After each top-level physics step: on a touching step, remove that small outward speed so the next step
// touches again (a jump, faster than kSlideKeepAwaySpeed, is left alone); on a step without a touch, keep the flag on
// while the pawn is still falling, within kSlideKeepHeight of the plane it last touched, not moving away from it faster
// than kSlideKeepAwaySpeed, and within kSlideKeepTime of game time since that touch. At 62 fps every frame is a touch
// pressed into the slope, so nothing changes.
//
// Developer movement/aim logger (PXSHIM_MOVELOG=1): records per call what the player and enemy movement and aim code
// computed (re/out/me_exe/summary.md), to movelog.bin next to the DLL, fixed 176-byte records (MoveRec), read by
// tools/movelog.py.
#include <windows.h>

#include <math.h>
#include <stdio.h>
#include <string.h>

#include "shim.h"
#include "version.h"

namespace {

constexpr uint32_t kImageBase = 0x400000, kTimestamp = SHATTER_GAME_EXE_TIMESTAMP;

// Pawn classes sharing ATdPawn's movement code (TdPawn and its three native subclasses) [V: slots 0x3E8/0x418 equal].
constexpr uint32_t kPawnVtables[] = {0x01D05268, 0x01D05700, 0x01D05BC0, 0x01D29130};
constexpr uint32_t kSlotPerformPhysics = 0x3E8, kSlotWalkAccel = 0x414, kSlotSprintAccel = 0x418;
constexpr uint32_t kFnPerformPhysics = 0x012B0960, kFnWalkAccel = 0x012B57F0, kFnSprintAccel = 0x012BD920;
// UTdAimBot: UpdateDispersion(DeltaTime) [V].
constexpr uint32_t kAimBotVtable = 0x01D0D018, kSlotUpdateDispersion = 0x11C, kFnUpdateDispersion = 0x012537F0;

enum RecType : uint16_t { kPhys = 1, kWalkAccel = 2, kSprintAccel = 3, kDispersion = 4, kSlideFlag = 5 };

#pragma pack(push, 1)
struct MoveRec {
  uint16_t type, iter;
  uint32_t self;
  double t;  // seconds since the log was opened (QPC)
  float a[40];
};
#pragma pack(pop)
static_assert(sizeof(MoveRec) == 176, "record size");

FILE* g_out = nullptr;
LARGE_INTEGER g_t0, g_freq;
double g_lastFlush = 0;

using PerformPhysicsFn = void(__thiscall*)(void*, float, int);
using AccelFn = float*(__thiscall*)(void*, float*, float, float, int, float);
using UpdateDispersionFn = void(__thiscall*)(void*, float);
PerformPhysicsFn g_performPhysics[4];
AccelFn g_walkAccel[4], g_sprintAccel[4];
UpdateDispersionFn g_updateDispersion;

double now() {
  LARGE_INTEGER c;
  QueryPerformanceCounter(&c);
  return double(c.QuadPart - g_t0.QuadPart) / double(g_freq.QuadPart);
}

const uint8_t* bytes(const void* p) { return static_cast<const uint8_t*>(p); }
float fieldF(const void* p, uint32_t off) { return *reinterpret_cast<const float*>(bytes(p) + off); }
uint32_t fieldU(const void* p, uint32_t off) { return *reinterpret_cast<const uint32_t*>(bytes(p) + off); }
float asF(uint32_t u) {
  float f;
  memcpy(&f, &u, 4);
  return f;
}

void write(MoveRec& r) {
  r.t = now();
  fwrite(&r, sizeof r, 1, g_out);
  if (r.t - g_lastFlush > 1.0) g_lastFlush = r.t, fflush(g_out);
}

// Pawn fields [V from the movement code]: Physics byte +0x68, Location +0xE8, Velocity +0x100, current move +0x8F4,
// class (UObject) +0x34; +0x6B4/+0x6C4/+0x6D0/+0x6F4 are read by the acceleration code (meaning not yet named).
void pawnState(const void* pawn, float* a) {
  for (int i = 0; i < 3; ++i) a[i] = fieldF(pawn, 0x100 + 4 * i), a[3 + i] = fieldF(pawn, 0xE8 + 4 * i);
  a[6] = float(bytes(pawn)[0x68]);
}

// PXSHIM_MOVEWATCH=1: hardware write watchpoints (DR0/DR1) on the player's Velocity.X/Y, armed on the game thread
// whenever the player pawn (the last one to ask for a walk/sprint acceleration) starts falling. Writes made inside performPhysics are
// ignored; every other write is counted by its code address and the first .text return addresses on the stack, and
// the table goes to the shim log (PXSHIM_DEBUG=1) every 10 s and at exit. Finds who changes the velocity between
// physics steps (the slide steering and speed limit). DR2 watches the dword holding the slide-steering flag
// (+0x41E bit 0, tested by the steering move tick 0x01208D70); each change of the bit goes to movelog.bin (kSlideFlag).
struct WatchHit {
  uint32_t key[7];  // eip, then up to 6 stack values that point into the exe's .text
  uint32_t count;
};
WatchHit g_hits[128];
int g_hitCount = 0;
bool g_watchOn = false, g_watchArmed = false, g_inPhys = false;
void* g_watched = nullptr;    // pawn the watchpoints are on
int g_flagLast = -1;
void* g_accelPawn = nullptr;  // last pawn that asked for a walk/sprint acceleration: the player
uint32_t g_textLo = 0, g_textHi = 0;
double g_lastDump = 0;

void dumpWatch(const char* why) {
  if (!g_watchArmed) return;
  shim::logLine("movewatch (%s): %d writers of player Velocity.X/Y outside performPhysics", why, g_hitCount);
  for (int i = 0; i < g_hitCount; ++i) {
    const WatchHit& h = g_hits[i];
    shim::logLine("  %8u  eip %08X  stack %08X %08X %08X %08X %08X %08X", h.count, h.key[0], h.key[1], h.key[2],
                  h.key[3], h.key[4], h.key[5], h.key[6]);
  }
}

LONG CALLBACK watchHandler(EXCEPTION_POINTERS* e) {
  if (e->ExceptionRecord->ExceptionCode != EXCEPTION_SINGLE_STEP || !(e->ContextRecord->Dr6 & 7))
    return EXCEPTION_CONTINUE_SEARCH;
  CONTEXT* c = e->ContextRecord;
  const DWORD hit = c->Dr6;
  c->Dr6 = 0;
  if (hit & 4) {  // the steering flag dword (+0x41C, bit 16 = byte +0x41E bit 0): log each change of the bit
    const uint32_t v = fieldU(g_watched, 0x41C);
    const int bit = (v >> 16) & 1;
    if (bit != g_flagLast) {
      g_flagLast = bit;
      MoveRec r = {kSlideFlag, uint16_t(g_inPhys), shim::ptr32(g_watched)};
      r.a[0] = float(bit);
      r.a[1] = asF(c->Eip);
      const uint32_t* sp = reinterpret_cast<const uint32_t*>(static_cast<uintptr_t>(c->Esp));
      for (int i = 0, n = 2; i < 96 && n < 12; ++i)
        if (sp[i] >= g_textLo && sp[i] < g_textHi) r.a[n++] = asF(sp[i]);
      write(r);
    }
    return EXCEPTION_CONTINUE_EXECUTION;
  }
  if (!g_inPhys) {
    uint32_t key[7] = {c->Eip};
    const uint32_t* sp = reinterpret_cast<const uint32_t*>(static_cast<uintptr_t>(c->Esp));
    for (int i = 0, n = 1; i < 96 && n < 7; ++i)
      if (sp[i] >= g_textLo && sp[i] < g_textHi) key[n++] = sp[i];
    int k = 0;
    while (k < g_hitCount && memcmp(g_hits[k].key, key, sizeof key)) ++k;
    if (k == g_hitCount && g_hitCount < 128) memcpy(g_hits[g_hitCount++].key, key, sizeof key);
    if (k < g_hitCount) ++g_hits[k].count;
  }
  return EXCEPTION_CONTINUE_EXECUTION;
}

void armWatch(void* pawn) {
  if (!g_watchArmed) AddVectoredExceptionHandler(1, watchHandler);
  g_watchArmed = true;
  g_watched = pawn;
  g_hitCount = 0;  // a new pawn: restart the table
  CONTEXT c = {};
  c.ContextFlags = CONTEXT_DEBUG_REGISTERS;
  c.Dr0 = shim::ptr32(pawn) + 0x100, c.Dr1 = shim::ptr32(pawn) + 0x104, c.Dr2 = shim::ptr32(pawn) + 0x41C;
  c.Dr7 = 0x0DDD0015;  // DR0/DR1/DR2 local enable, break on write, 4 bytes
  g_flagLast = -1;
  const BOOL ok = SetThreadContext(GetCurrentThread(), &c);
  shim::logLine("movewatch: watching pawn %08X Velocity (+0x100/+0x104): %s", shim::ptr32(pawn),
                ok ? "armed" : "SetThreadContext failed");
}

bool g_slideFix = false;
constexpr float kSlideKeepTime = 0.25f;   // s of game time without a touch (300 fps: ~30 ms with the fix, 0.17 s before)
constexpr float kSlideKeepHeight = 4.0f;  // units off the slope plane (300 fps: 0.13 with the fix, 2 before)
// Away speed: a touch at high frame rates finds the pawn 0.05-0.06 units under the slope, pushes it out ~0.166 units and
// leaves it moving out at <= 50 units/s (300 fps, runs 20261013-183858/184244: half of all launches at 49.9-50.0, steps
// of 3.7-5 ms still ~47, so a speed cap rather than 0.166/dt [A]; read as 0.166/dt it would be 100 at 600 fps and pass
// 150 only near 1000 fps). A jump: hundreds.
constexpr float kSlideKeepAwaySpeed = 150;  // units/s away from the plane
constexpr float kPathStep = 10;     // units between stored touch locations
constexpr float kPathMin = 30;      // shortest touch path that corrects the normal
constexpr float kPathAge = 0.5f;    // s: older touch locations are dropped

struct SlideState {
  void* pawn;
  float airTime;  // game time since the last touch
  float clock;    // game time while tracked (touch path ages)
  float p[3], n[3];  // location and slope normal at the last touch
  float tp[8][4];    // touch path: locations at least kPathStep apart, with their clock, oldest first
  int tpCount;
  bool valid;
};
SlideState g_slide[16];
float g_slideCorrection = 0;  // outward normal speed removed in the last step (movelog)
int g_slideSource = 0;        // the steering flag after the last step: 0 off, 1 a touch, 2 kept by slideKeep (movelog)

SlideState* slideState(void* pawn) {
  SlideState* free = nullptr;
  for (SlideState& s : g_slide) {
    if (s.pawn == pawn) return &s;
    if (!free && !s.valid) free = &s;
  }
  if (free) *free = SlideState{pawn};
  return free;
}

// After a top-level performPhysics step of a pawn.
void slideKeep(void* pawn, float dt) {
  uint32_t& flags = *reinterpret_cast<uint32_t*>(const_cast<uint8_t*>(bytes(pawn)) + 0x41C);
  const bool falling = bytes(pawn)[0x68] == 2;
  g_slideSource = 0;
  if (flags & 0x10000) {  // touched a slide surface in this step
    g_slideSource = 1;
    SlideState* s = slideState(pawn);
    if (!s) return;
    if (!s->valid) s->tpCount = 0, s->clock = 0;
    s->valid = true, s->airTime = 0, s->clock += dt;
    float nc[3];
    for (int i = 0; i < 3; ++i) s->p[i] = fieldF(pawn, 0xE8 + 4 * i), nc[i] = fieldF(pawn, 0x71C + 4 * i);
    // Slope normal: the stored one is the collision normal, which at speed leans ~3 degrees toward the motion (300 fps,
    // run 20261013-183858: the touch locations lie on a plane within 0.06 units, while the velocity the game leaves
    // after a touch is +50 units/s off that plane yet not outward along the collision normal, so nothing was removed
    // and every touch launched a 2-unit, 160 ms hop). Touch locations lie on the slope, so the path between them is in
    // its plane: the lean along the path is removed from the collision normal.
    while (s->tpCount > 0 && s->clock - s->tp[0][3] > kPathAge) {
      for (int k = 1; k < s->tpCount; ++k) memcpy(s->tp[k - 1], s->tp[k], sizeof s->tp[k]);
      --s->tpCount;
    }
    if (s->tpCount > 0) {
      const float* o = s->tp[0];
      float d[3], dd = 0;
      for (int i = 0; i < 3; ++i) d[i] = s->p[i] - o[i], dd += d[i] * d[i];
      if (dd >= kPathMin * kPathMin) {
        const float k = (nc[0] * d[0] + nc[1] * d[1] + nc[2] * d[2]) / dd;
        for (int i = 0; i < 3; ++i) nc[i] -= k * d[i];
      }
    }
    float m = sqrtf(nc[0] * nc[0] + nc[1] * nc[1] + nc[2] * nc[2]);
    m = m > 1e-6f ? 1 / m : 0;
    for (int i = 0; i < 3; ++i) s->n[i] = nc[i] * m;
    const float* last = s->tpCount ? s->tp[s->tpCount - 1] : nullptr;
    float step2 = 0;
    for (int i = 0; last && i < 3; ++i) step2 += (s->p[i] - last[i]) * (s->p[i] - last[i]);
    if (!last || step2 >= kPathStep * kPathStep) {
      if (s->tpCount == 8) {
        for (int k = 1; k < 8; ++k) memcpy(s->tp[k - 1], s->tp[k], sizeof s->tp[k]);
        --s->tpCount;
      }
      float* e = s->tp[s->tpCount++];
      e[0] = s->p[0], e[1] = s->p[1], e[2] = s->p[2], e[3] = s->clock;
    }
    // Stay on the surface: at high frame rates a touching step leaves the pawn moving off the slope (300 fps: +22 to
    // +50 units/s along the slope normal, then a ~160 ms hop); at 62 fps it leaves at about -1, pressed into it [V: runs
    // 20261013-120725/120804/121535/183858]. Removing the small outward part keeps every frame a touch, as at 62. A jump
    // (outward speed above kSlideKeepAwaySpeed) is left alone.
    float* v = reinterpret_cast<float*>(const_cast<uint8_t*>(bytes(pawn)) + 0x100);
    const float vn = v[0] * s->n[0] + v[1] * s->n[1] + v[2] * s->n[2];
    if (falling && vn > 0 && vn <= kSlideKeepAwaySpeed) {
      for (int i = 0; i < 3; ++i) v[i] -= vn * s->n[i];
      g_slideCorrection = vn;
    }
    return;
  }
  SlideState* s = nullptr;
  for (SlideState& e : g_slide)
    if (e.pawn == pawn && e.valid) s = &e;
  if (!s) return;
  s->airTime += dt, s->clock += dt;
  float h = 0, vn = 0;  // height above the slope plane at the last touch, speed away from it
  for (int i = 0; i < 3; ++i)
    h += (fieldF(pawn, 0xE8 + 4 * i) - s->p[i]) * s->n[i], vn += fieldF(pawn, 0x100 + 4 * i) * s->n[i];
  // Not leaving the plane: a jump off the slide (hundreds of units/s along the normal) must not be pulled back into it,
  // since TdPawn's tick 0x012BED30 forces the slide move (0x26) while the flag is on. The move itself is not checked:
  // about 0.12 s into each hop the game changes it (seen at 300 fps: the kept flag stopped at 119-121 ms after the
  // touch in every hop), which at 62 fps never happens because every frame touches.
  const bool sliding = vn <= kSlideKeepAwaySpeed;
  if (!falling || !sliding || s->airTime > kSlideKeepTime || h > kSlideKeepHeight || h < -kSlideKeepHeight) {
    s->valid = false;
    return;
  }
  flags |= 0x10000;  // the normal at +0x71C still holds the last touch's
  g_slideSource = 2;
}

// One template instance per vtable so each hook calls the original it replaced.
template <int I>
void __fastcall hkPerformPhysics(void* self, void*, float dt, int iterations) {
  if (!g_out) {
    g_performPhysics[I](self, dt, iterations);
    if (g_slideFix && iterations == 0) slideKeep(self, dt);
    return;
  }
  MoveRec r = {kPhys, uint16_t(iterations), shim::ptr32(self)};
  r.a[0] = dt;
  pawnState(self, r.a + 1);  // before: vel 1-3, loc 4-6, physics 7
  float accelBefore[3];
  for (int i = 0; i < 3; ++i) accelBefore[i] = fieldF(self, 0x10C + 4 * i);
  if (g_watchOn && self == g_accelPawn && self != g_watched && bytes(self)[0x68] == 2) armWatch(self);
  g_inPhys = true;
  g_performPhysics[I](self, dt, iterations);
  g_inPhys = iterations > 0;  // a substep returns into the outer performPhysics
  g_slideCorrection = 0;
  g_slideSource = (fieldU(self, 0x41C) >> 16) & 1;
  if (g_slideFix && iterations == 0) slideKeep(self, dt);
  pawnState(self, r.a + 8);  // after: vel 8-10, loc 11-13, physics 14
  r.a[15] = asF(fieldU(self, 0x8F4));
  r.a[16] = asF(fieldU(self, 0x34));
  r.a[17] = asF(uint32_t(I));
  r.a[18] = float(int32_t(fieldU(self, 0x2B8)));  // Health [V: RegenerateHealth 0x012B18F0]
  r.a[19] = fieldF(self, 0x880);                // time since last damage (regen timer) [V same]
  // Steering while falling (physFalling 0x00F07B30 / player 0x012C0980) [V offsets from those]: Acceleration +0x10C
  // (before the step), AccelRate +0x274, AirControl +0x284, AirSpeed +0x264, WalkableFloorZ +0x1C8.
  for (int i = 0; i < 3; ++i) r.a[20 + i] = accelBefore[i];
  r.a[23] = fieldF(self, 0x274), r.a[24] = fieldF(self, 0x284), r.a[25] = fieldF(self, 0x264);
  r.a[26] = fieldF(self, 0x1C8);
  // Slide fix state after the step: steering flag, MovementState, active move's vtable (moves array +0x5FC), and the
  // outward normal speed the fix removed.
  r.a[27] = float((fieldU(self, 0x41C) >> 16) & 1);
  const uint8_t move = bytes(self)[0x4FE];
  r.a[28] = float(move);
  const uint32_t moves = fieldU(self, 0x5FC), moveObj = moves ? *shim::fromPtr32<uint32_t>(moves + 4u * move) : 0;
  r.a[29] = asF(moveObj ? *shim::fromPtr32<uint32_t>(moveObj) : 0);
  r.a[30] = g_slideCorrection;
  // Slide steering inputs read by the move tick 0x01208D70: side byte +0x505 (1 one way, 2 the other, else from an
  // object at +0x1CC), the move's strength (+0x198, times dt) and speed cap (+0x194); where the flag came from.
  r.a[31] = float(bytes(self)[0x505]);
  const bool slideMove = moveObj && *shim::fromPtr32<uint32_t>(moveObj) == 0x01CF7A08u;  // the steering move's class
  r.a[32] = slideMove ? *shim::fromPtr32<float>(moveObj + 0x198) : 0.f;
  r.a[33] = slideMove ? *shim::fromPtr32<float>(moveObj + 0x194) : 0.f;
  r.a[34] = float(g_slideSource);
  for (int i = 0; i < 3; ++i) r.a[35 + i] = fieldF(self, 0x71C + 4 * i);  // collision normal of the last touch
  write(r);
  if (g_watchArmed && r.t - g_lastDump > 10) g_lastDump = r.t, dumpWatch("periodic");
}

template <int I, bool Sprint>
float* __fastcall hkAccel(void* self, void*, float* out, float forward, float strafe, int deltaRotation, float dt) {
  float* res = (Sprint ? g_sprintAccel : g_walkAccel)[I](self, out, forward, strafe, deltaRotation, dt);
  g_accelPawn = self;
  MoveRec r = {Sprint ? kSprintAccel : kWalkAccel, 0, shim::ptr32(self)};
  r.a[0] = forward, r.a[1] = strafe, r.a[2] = float(deltaRotation), r.a[3] = dt;
  for (int i = 0; i < 3; ++i) r.a[4 + i] = res[i];
  pawnState(self, r.a + 7);  // vel 7-9, loc 10-12, physics 13
  r.a[14] = asF(fieldU(self, 0x8F4));
  r.a[15] = fieldF(self, 0x6B4), r.a[16] = fieldF(self, 0x6C4), r.a[17] = fieldF(self, 0x6D0);
  r.a[18] = fieldF(self, 0x6F4);
  r.a[19] = asF(fieldU(self, 0x34));
  write(r);
  return res;
}

// The aim bot's first 0x90 bytes after the update: dispersion components at +0x7C/+0x80/+0x84, their limits at
// +0x3C..+0x4C, rates at +0x50..+0x74 [V: UpdateDispersion/GetImprovementRate]; Outer (+0x28) is the owning pawn.
void __fastcall hkUpdateDispersion(void* self, void*, float dt) {
  g_updateDispersion(self, dt);
  MoveRec r = {kDispersion, 0, shim::ptr32(self)};
  r.a[0] = dt;
  memcpy(r.a + 1, self, 0x90);
  write(r);
}

template <int I>
bool hookPawnVtable(uint32_t vt) {
  uint32_t o = 0;
  bool ok = shim::patchSlot(vt, kSlotPerformPhysics / 4, shim::ptr32((void*)&hkPerformPhysics<I>), &o);
  g_performPhysics[I] = reinterpret_cast<PerformPhysicsFn>(o);
  if (!g_out) return ok;
  ok = ok && shim::patchSlot(vt, kSlotWalkAccel / 4, shim::ptr32((void*)&hkAccel<I, false>), &o);
  g_walkAccel[I] = reinterpret_cast<AccelFn>(o);
  ok = ok && shim::patchSlot(vt, kSlotSprintAccel / 4, shim::ptr32((void*)&hkAccel<I, true>), &o);
  g_sprintAccel[I] = reinterpret_cast<AccelFn>(o);
  return ok;
}

}  // namespace

namespace shim {

void gameHooksInit() {
  g_slideFix = env("PXSHIM_SLIDEFIX")[0] == '1';
  const bool log = env("PXSHIM_MOVELOG")[0] == '1';
  if (!g_slideFix && !log) return;
  const uint8_t* exe = reinterpret_cast<const uint8_t*>(GetModuleHandleA(nullptr));
  const IMAGE_NT_HEADERS* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(
      exe + reinterpret_cast<const IMAGE_DOS_HEADER*>(exe)->e_lfanew);
  if (nt->FileHeader.TimeDateStamp != kTimestamp) {
    logLine("game hooks: exe timestamp %08X, expected %08X: slide steering fix and movelog off",
            (unsigned)nt->FileHeader.TimeDateStamp, (unsigned)kTimestamp);
    return;
  }
  const uint32_t delta = ptr32(exe) - kImageBase;
  auto slot = [&](uint32_t vt, uint32_t off) { return *fromPtr32<uint32_t>(vt + delta + off); };
  for (uint32_t vt : kPawnVtables)
    if (slot(vt, kSlotPerformPhysics) != kFnPerformPhysics + delta || slot(vt, kSlotWalkAccel) != kFnWalkAccel + delta ||
        slot(vt, kSlotSprintAccel) != kFnSprintAccel + delta) {
      logLine("game hooks: pawn vtable %08X does not hold the expected functions: not hooked", (unsigned)vt);
      return;
    }
  if (slot(kAimBotVtable, kSlotUpdateDispersion) != kFnUpdateDispersion + delta) {
    logLine("game hooks: aim bot vtable does not hold UpdateDispersion: not hooked");
    return;
  }
  char path[MAX_PATH];
  if (log) {
    snprintf(path, sizeof path, "%smovelog.bin", g_dir);
    g_out = fopen(path, "wb");
    if (!g_out) logLine("movelog: cannot open %s", path);
    else setvbuf(g_out, nullptr, _IOFBF, 1 << 20);
    g_watchOn = g_out && env("PXSHIM_MOVEWATCH")[0] == '1';
    g_textLo = ptr32(exe) + 0x1000, g_textHi = ptr32(exe) + 0x168B000;  // .text [V: PE section table]
    QueryPerformanceFrequency(&g_freq);
    QueryPerformanceCounter(&g_t0);
  }
  if (!g_slideFix && !g_out) return;
  bool ok = hookPawnVtable<0>(kPawnVtables[0] + delta) && hookPawnVtable<1>(kPawnVtables[1] + delta) &&
            hookPawnVtable<2>(kPawnVtables[2] + delta) && hookPawnVtable<3>(kPawnVtables[3] + delta);
  if (g_out) {
    uint32_t o = 0;
    ok = ok && patchSlot(kAimBotVtable + delta, kSlotUpdateDispersion / 4, ptr32((void*)&hkUpdateDispersion), &o);
    g_updateDispersion = reinterpret_cast<UpdateDispersionFn>(o);
  }
  logLine("game hooks: %s; slide steering fix %s, movelog %s", ok ? "hooked" : "PARTLY hooked (patch failed)",
          g_slideFix ? "on" : "off", g_out ? path : "off");
}

void gameHooksShutdown() {
  dumpWatch("exit");
  if (g_out) fclose(g_out), g_out = nullptr;
}

}  // namespace shim
