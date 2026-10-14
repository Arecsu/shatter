// Replays the NxFluid API calls of a shim trace (PXFT, format in shim/fluid_wrap.cpp) through our solver.
//   replay_trace <scenario.txt> <in.trace> <out.trace>
// The scenario gives the statics, gravity and dt; FluidParams come from the CREATE record (NxFluidDesc).
// API calls (CREATE, ADD, UPDATE, REMOVEALL, SIMULATE, RELEASE) are copied through and replayed; the output adds
// what the shim records at level 2 (FETCHED, PARTICLES, DELETED_IDS, WORLD_BOUNDS after each SIMULATE and
// CREATED_AFTER_ADD after each ADD). The input's own output records (FETCHED..WORLD_BOUNDS) are dropped and regenerated.
// Only fluid index 0 is replayed (the first CREATE); records of other fluids are skipped.
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "fluid.h"
#include "scenario.h"

using namespace pxr;

namespace {

enum Tag : uint32_t {
  kCreate = 1, kRelease = 2, kAdd = 3, kUpdate = 4, kRemoveAll = 5, kSimulate = 6, kFetched = 7, kParticles = 8,
  kCreatedIds = 9, kDeletedIds = 10, kPackets = 11, kCreatedAfterAdd = 12, kWorldBounds = 13
};
constexpr uint32_t kNoFluid = 0xFFFFFFFFu;
constexpr size_t kRec = 52;

struct Out {
  std::vector<uint8_t> buf;
  void rec(uint32_t tag, uint32_t fluid, const void* p, size_t n) {
    uint32_t h[3] = {tag, fluid, (uint32_t)n};
    const uint8_t* hp = reinterpret_cast<const uint8_t*>(h);
    buf.insert(buf.end(), hp, hp + 12);
    if (n) {
      const uint8_t* b = static_cast<const uint8_t*>(p);
      buf.insert(buf.end(), b, b + n);
    }
  }
};

// Reads dword / float i of a payload without assuming alignment.
template <class T>
T rd(const uint8_t* p, size_t byteOff) {
  T v;
  memcpy(&v, p + byteOff, sizeof v);
  return v;
}

// NxFluidDesc (352 bytes, dword indices in docs/re-notes-fluid-mesh.md 3.1) -> FluidParams.
FluidParams paramsFromDesc(const uint8_t* d) {
  FluidParams P;
  auto u = [&](int dw) { return rd<uint32_t>(d, 4 * dw); };
  auto f = [&](int dw) { return rd<float>(d, 4 * dw); };
  P.maxParticles = u(0x10);
  P.numReserveParticles = u(0x11);
  P.restParticlesPerMeter = f(0x12);
  P.restDensity = f(0x13);
  P.kernelRadiusMultiplier = f(0x14);
  P.motionLimitMultiplier = f(0x15);
  P.collisionDistanceMultiplier = f(0x16);
  P.packetSizeMultiplier = u(0x17);
  P.stiffness = f(0x18);
  P.viscosity = f(0x19);
  P.damping = f(0x1b);
  P.externalAcceleration = Vec3(f(0x1d), f(0x1e), f(0x1f));
  P.restitutionStatic = f(0x24);
  P.frictionStatic = f(0x25);
  P.restitutionDynamic = f(0x28);
  P.frictionDynamic = f(0x29);
  switch (u(0x2d)) {
    case 2: P.method = SimMethod::NoInteraction; break;
    case 4: P.method = SimMethod::Mixed; break;
    default: P.method = SimMethod::SPH; break;  // 1
  }
  const uint32_t flags = u(0x4f);
  P.disableGravity = flags & 0x2;
  P.priorityMode = flags & 0x20;
  return P;
}

}  // namespace

int main(int argc, char** argv) {
  if (argc < 4) {
    fprintf(stderr, "usage: replay_trace <scenario.txt> <in.trace> <out.trace>\n");
    return 1;
  }
  Scenario sc;
  if (!loadScenario(argv[1], sc, true)) {
    fprintf(stderr, "cannot open %s\n", argv[1]);
    return 1;
  }
  if (!sc.unknown.empty()) {
    fprintf(stderr, "unknown directive %s\n", sc.unknown.c_str());
    return 1;
  }

  FILE* fi = fopen(argv[2], "rb");
  if (!fi) {
    fprintf(stderr, "cannot open %s\n", argv[2]);
    return 1;
  }
  std::vector<uint8_t> in;
  {
    uint8_t chunk[1 << 16];
    size_t k;
    while ((k = fread(chunk, 1, sizeof chunk, fi)) > 0) in.insert(in.end(), chunk, chunk + k);
    fclose(fi);
  }
  if (in.size() < 8 || memcmp(in.data(), "PXFT", 4) != 0) {
    fprintf(stderr, "bad magic in %s\n", argv[2]);
    return 1;
  }

  Out out;
  out.buf.insert(out.buf.end(), {'P', 'X', 'F', 'T', 1, 0, 0, 0});
  Fluid* fluid = nullptr;
  int warnings = 0;
  size_t off = 8;
  size_t steps = 0;
  while (off + 12 <= in.size()) {
    const uint32_t tag = rd<uint32_t>(in.data(), off);
    const uint32_t fl = rd<uint32_t>(in.data(), off + 4);
    const uint32_t n = rd<uint32_t>(in.data(), off + 8);
    off += 12;
    if (off + n > in.size()) {
      fprintf(stderr, "truncated record at %zu\n", off - 12);
      return 1;
    }
    const uint8_t* p = in.data() + off;
    off += n;

    // Output records of the recording (FETCHED and later) are regenerated below.
    if (tag >= kFetched && tag != kRelease) continue;
    if (fl != 0 && fl != kNoFluid) continue;  // single fluid: index 0 only

    switch (tag) {
      case kCreate: {
        if (fluid) break;  // a second fluid is not replayed
        FluidParams P = paramsFromDesc(p);
        fluid = new Fluid(P);
        out.rec(tag, fl, p, n);
        break;
      }
      case kRelease:
        out.rec(tag, fl, p, n);
        break;
      case kAdd: {
        out.rec(tag, fl, p, n);
        if (!fluid) break;
        const uint32_t req = rd<uint32_t>(p, 0), ret = rd<uint32_t>(p, 8), streams = rd<uint32_t>(p, 12);
        std::vector<Vec3> pos(req), vel(req);
        std::vector<float> life(req);
        for (uint32_t i = 0; i < req; ++i) {
          const size_t b = 16 + 32 * (size_t)i;
          pos[i] = Vec3(rd<float>(p, b), rd<float>(p, b + 4), rd<float>(p, b + 8));
          vel[i] = Vec3(rd<float>(p, b + 12), rd<float>(p, b + 16), rd<float>(p, b + 20));
          life[i] = rd<float>(p, b + 24);
        }
        std::vector<uint32_t> ids(req);
        const uint32_t got = fluid->addParticles(pos.data(), (streams & 2) ? vel.data() : nullptr,
                                                 (streams & 4) ? life.data() : nullptr, req, ids.data());
        if (got != ret) {
          warnings++;
          fprintf(stderr, "add %zu: returned %u, recorded %u\n", steps, got, ret);
        }
        std::vector<uint8_t> pay(4 + 4 * (size_t)got);
        const uint32_t cnt = got;
        memcpy(pay.data(), &cnt, 4);
        memcpy(pay.data() + 4, ids.data(), 4 * (size_t)got);
        out.rec(kCreatedAfterAdd, 0, pay.data(), pay.size());
        break;
      }
      case kUpdate: {
        out.rec(tag, fl, p, n);
        if (!fluid) break;
        const uint32_t cnt = rd<uint32_t>(p, 4);
        for (uint32_t i = 0; i < cnt; ++i) {
          const size_t b = 12 + 20 * (size_t)i;
          const uint32_t id = rd<uint32_t>(p, b), flag = rd<uint32_t>(p, b + 4);
          if (flag & 1) fluid->deleteParticle(id);  // NX_FP_DELETE
        }
        break;
      }
      case kRemoveAll:
        out.rec(tag, fl, p, n);
        if (fluid) fluid->removeAllParticles();
        break;
      case kSimulate: {
        out.rec(tag, fl, p, n);
        if (!fluid) break;
        const float elapsed = rd<float>(p, 0);
        if (elapsed != sc.dt) {
          warnings++;
          fprintf(stderr, "simulate %zu: dt %g in trace, %g in scenario (scenario used)\n", steps, elapsed, sc.dt);
        }
        fluid->step(sc.dt, sc.gravity, sc.statics);
        steps++;
        const uint32_t fetched[2] = {1, 1};
        out.rec(kFetched, kNoFluid, fetched, sizeof fetched);

        const size_t N = fluid->size();
        std::vector<uint8_t> pay(4 + kRec * N);
        const uint32_t cnt = (uint32_t)N;
        memcpy(pay.data(), &cnt, 4);
        const auto& P0 = fluid->positions();
        const auto& V0 = fluid->velocities();
        const auto& R0 = fluid->densities();
        const auto& L0 = fluid->lifetimes();
        const auto& N0 = fluid->normals();
        const auto& I0 = fluid->ids();
        const auto& F0 = fluid->flags();
        for (size_t i = 0; i < N; ++i) {
          const float rec[11] = {P0[i].x, P0[i].y, P0[i].z, V0[i].x, V0[i].y, V0[i].z, R0[i], L0[i],
                                 N0[i].x, N0[i].y, N0[i].z};
          uint8_t* q = pay.data() + 4 + kRec * i;
          memcpy(q, rec, sizeof rec);
          memcpy(q + 44, &I0[i], 4);
          memcpy(q + 48, &F0[i], 4);
        }
        out.rec(kParticles, 0, pay.data(), pay.size());

        const auto& del = fluid->deletedIds();
        std::vector<uint8_t> dp(4 + 4 * del.size());
        const uint32_t dn = (uint32_t)del.size();
        memcpy(dp.data(), &dn, 4);
        if (dn) memcpy(dp.data() + 4, del.data(), 4 * del.size());
        out.rec(kDeletedIds, 0, dp.data(), dp.size());

        Vec3 lo, hi;
        fluid->worldBounds(lo, hi);
        const float wb[6] = {lo.x, lo.y, lo.z, hi.x, hi.y, hi.z};
        out.rec(kWorldBounds, 0, wb, sizeof wb);
        break;
      }
      default:
        fprintf(stderr, "unknown record tag %u (copied)\n", tag);
        out.rec(tag, fl, p, n);
        break;
    }
  }
  if (!fluid) {
    fprintf(stderr, "no CREATE record for fluid 0\n");
    return 1;
  }

  FILE* fo = fopen(argv[3], "wb");
  if (!fo) {
    fprintf(stderr, "cannot write %s\n", argv[3]);
    return 1;
  }
  fwrite(out.buf.data(), 1, out.buf.size(), fo);
  fclose(fo);
  printf("{\"ok\":true,\"steps\":%zu,\"particles\":%zu,\"warnings\":%d}\n", steps, fluid->size(), warnings);
  return 0;
}
