#pragma once

#include <cstdint>

namespace bitkv {

class Clock {
 public:
  virtual ~Clock() = default;

  [[nodiscard]] virtual std::int64_t now_ms() const = 0;
};

class SystemClock final : public Clock {
 public:
  [[nodiscard]] std::int64_t now_ms() const override;
};

class ManualClock final : public Clock {
 public:
  explicit ManualClock(std::int64_t now_ms = 0) noexcept;

  void set_now_ms(std::int64_t now_ms) noexcept;
  void advance_ms(std::int64_t delta_ms) noexcept;

  [[nodiscard]] std::int64_t now_ms() const override;

 private:
  std::int64_t now_ms_;
};

}  // namespace bitkv
