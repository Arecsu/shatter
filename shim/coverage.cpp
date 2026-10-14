// Coverage counters (coverage.h).
#include "coverage.h"

#include <stdio.h>
#include <string.h>

#include <atomic>

#include "shim.h"

namespace shim {
namespace {

struct Info {
  const char* name;
  const char* row;  // GAPS.md row
};
const Info kInfo[kCovCount] = {
    {"fluid_response_nondefault", "1"},  {"fluid_response_twoway", "1"},     {"fluid_twoway_created", "1"},
    {"fluid_moving_body_steps", "12/13"}, {"fluid_convex_near_steps", "14"}, {"fluid_static_friction", "15"},
    {"fluid_add_over_4096", "17"},       {"fluid_setter_after_create", "21"}, {"fluid_emitter_create", "21"},
    {"fluid_hardware", "22"},            {"scene_get_fluids", "31"},
    {"cloth_moving_body_substeps", "2"}, {"cloth_set_positions", "3"},       {"cloth_outside_static_box", "3"},
    {"cloth_statics_requery", "3"},      {"cloth_validbounds_flag", "4"},    {"cloth_set_validbounds", "4"},
    {"cloth_outside_validbounds", "4"},  {"cloth_disable_coll_toggle", "5"}, {"cloth_body_slot_new", "6"},
    {"cloth_body_slot_park", "6"},       {"cloth_body_slot_reuse", "6"},     {"cloth_pressure_flag", "7"},
    {"cloth_set_pressure", "7"},         {"cloth_twoway_flag", "8"},         {"cloth_set_coll_response", "8"},
    {"cloth_attach_to_core", "9"},       {"shape_cloth_flags", "10"},        {"shape_fluid_gates", "11"},
    {"cloth_convex_near", "14"},         {"cloth_hardware", "22"},           {"cloth_attach_colliding", "29"},
    {"cloth_attach_shape_null", "29"},   {"scene_get_cloths", "31"},         {"cloth_adhere", "32"},
    {"cloth_unsupported_calls", "33"},
};

std::atomic<uint32_t> g_count[kCovCount];
uint32_t g_last[kCovCount];  // values at the previous sinceLast dump (game thread)

}  // namespace

void cov(Cov id, uint32_t n) { g_count[id].fetch_add(n, std::memory_order_relaxed); }

void covLog(const char* why, bool sinceLast) {
  static const bool on = env("PXSHIM_COVERAGE")[0] != '0';
  char line[2048];
  size_t len = 0;
  for (uint32_t i = 0; i < kCovCount; ++i) {
    const uint32_t now = g_count[i].load(std::memory_order_relaxed);
    const uint32_t v = sinceLast ? now - g_last[i] : now;
    if (sinceLast) g_last[i] = now;
    if (!v || len >= sizeof line - 96) continue;
    const int k = snprintf(line + len, sizeof line - len, " %s[%s]=%u", kInfo[i].name, kInfo[i].row, v);
    if (k > 0) len += (size_t)k;
  }
  if (on) logLine("coverage (%s, %s):%s", why, sinceLast ? "since the last dump" : "run total", len ? line : " none");
}

}  // namespace shim
