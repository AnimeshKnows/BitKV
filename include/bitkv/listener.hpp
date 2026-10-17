#pragma once

#include <cstdint>
#include <string_view>

namespace bitkv {

class MutationListener {
 public:
  virtual ~MutationListener() = default;

  /// Must not throw and must not call back into Store/Engine. Failures (e.g. WAL
  /// I/O) must be recorded internally and surfaced through the listener's own
  /// channel.
  virtual void on_set(std::string_view key,
                      std::string_view value,
                      std::int64_t expire_at_ms) noexcept = 0;

  /// Must not throw and must not call back into Store/Engine. Failures must be
  /// recorded internally and surfaced through the listener's own channel.
  virtual void on_del(std::string_view key) noexcept = 0;
};

}  // namespace bitkv
