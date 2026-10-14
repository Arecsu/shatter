// floor() without the libm call: same value as std::floor for every float (NaN and +-inf pass through; -0 comes back
// as +0, a sign no caller sees since they all convert to an integer). 32-bit glibc floorf saves and restores the x87 environment on every call, which
// was ~30% of an SPH step; the UCRT one is an out-of-line call too.
#pragma once

#include <cmath>
#include <cstdint>

namespace pxm {

inline float floorExact(float x) {
  if (!(std::fabs(x) < 8388608.0f)) return x;  // |x| >= 2^23 is already integral (or NaN/inf)
  const float t = (float)(int32_t)x;           // truncation, exact in this range
  return t > x ? t - 1.0f : t;
}

}  // namespace pxm

namespace pxm {

// (int64_t)std::floor(x) without the x87 float->int64 conversion 32-bit code uses (fistp with two control-word
// switches): through int32 when x fits, else the old conversion (|x| >= 2^31 is integral already, NaN/inf as before).
inline int64_t floorToInt64(float x) {
  if (std::fabs(x) < 2147483648.0f) {
    const int32_t i = (int32_t)x;
    return i - (int32_t)((float)i > x);
  }
  return (int64_t)x;
}

}  // namespace pxm
