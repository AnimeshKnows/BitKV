#pragma once

#include "bitkv/clock.hpp"
#include "bitkv/listener.hpp"
#include "bitkv/status.hpp"

#include <cstddef>
#include <cstdint>
#include <list>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>

namespace bitkv {

/// Runtime counters exposed for observability and tests.
struct Counters {
  std::uint64_t hits = 0;
  std::uint64_t misses = 0;
  std::uint64_t evictions = 0;
  std::uint64_t expired_cleaned = 0;  // reserved for a later sweeper phase
};

/// Single-threaded in-memory LRU key-value store.
///
/// NOT thread-safe. Phase 3 Engine wraps per-shard Store instances with mutexes.
///
/// EXISTS does not promote to MRU. GET does.
/// Expiry fields are stored but not actively cleaned in phase 1; get/exists may
/// still return entries whose expire_at_ms is in the past until a sweeper lands.
/// expire_at_ms < 0 is invalid (set returns InvalidArgument); 0 means no expiry.
///
/// Exception model: operations may throw std::bad_alloc only. set() offers the
/// strong guarantee. get/del/exists never leave partial state on failure.
///
/// MutationListener rules:
/// - on_set / on_del are noexcept: must not throw and must not call back into
///   this Store (or a future Engine). Debug builds assert on re-entrancy.
/// - An implementation that can fail (e.g. WAL) must record the failure
///   internally and surface it through its own channel.
/// - Caller owns the listener and must keep it alive until set_listener(nullptr)
///   or until this Store is destroyed.
class Store {
 public:
  static constexpr std::size_t kMinKeyBytes = 1;
  static constexpr std::size_t kMaxKeyBytes = 1024;
  static constexpr std::size_t kMaxValueBytes = 1U * 1024U * 1024U;  // 1 MiB

  /// \throws std::invalid_argument if capacity == 0.
  /// \param clock Caller-owned; must outlive this Store.
  Store(std::size_t capacity, const Clock& clock);

  Store(const Store&) = delete;
  Store& operator=(const Store&) = delete;
  Store(Store&&) = delete;
  Store& operator=(Store&&) = delete;

  /// \param listener Caller-owned; must remain valid until set_listener(nullptr)
  ///        or Store destruction. Pass nullptr to detach.
  void set_listener(MutationListener* listener) noexcept;

  [[nodiscard]] Status set(std::string_view key,
                           std::string_view value,
                           std::int64_t expire_at_ms = 0);

  [[nodiscard]] std::optional<std::string> get(std::string_view key);

  [[nodiscard]] bool del(std::string_view key);

  /// Presence check only. Does NOT promote to MRU and does not touch counters.
  [[nodiscard]] bool exists(std::string_view key) const;

  [[nodiscard]] std::size_t size() const noexcept;
  [[nodiscard]] std::size_t capacity() const noexcept;
  [[nodiscard]] const Counters& counters() const noexcept;

 private:
  struct Entry {
    std::string key;
    std::string value;
    std::int64_t expire_at_ms = 0;
  };

  using EntryList = std::list<Entry>;
  using Index = std::unordered_map<std::string, EntryList::iterator>;

  [[nodiscard]] static bool valid_key(std::string_view key) noexcept;
  [[nodiscard]] static bool valid_value(std::string_view value) noexcept;

  void assert_not_in_callback() const noexcept;

  void promote(EntryList::iterator it) noexcept;
  void emit_set(std::string_view key,
                std::string_view value,
                std::int64_t expire_at_ms) const noexcept;
  void emit_del(std::string_view key) const noexcept;

  std::size_t capacity_;
  const Clock& clock_;
  MutationListener* listener_ = nullptr;
  EntryList entries_;
  Index index_;
  Counters counters_{};
  mutable bool in_callback_ = false;
};

}  // namespace bitkv
