#include "bitkv/clock.hpp"

#include <chrono>
#include <cstdint>
#include <limits>
#include <type_traits>

namespace bitkv {

namespace {

using MsRep = std::chrono::milliseconds::rep;

static_assert(sizeof(MsRep) >= 8,
              "std::chrono::milliseconds::rep must be at least 8 bytes");
static_assert(std::is_convertible_v<MsRep, std::int64_t>,
              "milliseconds::rep must be convertible to std::int64_t");

}  // namespace

std::int64_t SystemClock::now_ms() const {
  const auto now = std::chrono::system_clock::now().time_since_epoch();
  return std::chrono::duration_cast<std::chrono::milliseconds>(now).count();
}

ManualClock::ManualClock(std::int64_t now_ms) noexcept : now_ms_(now_ms) {}

void ManualClock::set_now_ms(std::int64_t now_ms) noexcept {
  now_ms_ = now_ms;
}

void ManualClock::advance_ms(std::int64_t delta_ms) noexcept {
  constexpr std::int64_t kMax = std::numeric_limits<std::int64_t>::max();
  constexpr std::int64_t kMin = std::numeric_limits<std::int64_t>::min();

  if (delta_ms > 0) {
    if (now_ms_ > kMax - delta_ms) {
      now_ms_ = kMax;
      return;
    }
  } else if (delta_ms < 0) {
    if (now_ms_ < kMin - delta_ms) {
      now_ms_ = kMin;
      return;
    }
  }
  now_ms_ += delta_ms;
}

std::int64_t ManualClock::now_ms() const {
  return now_ms_;
}

}  // namespace bitkv
