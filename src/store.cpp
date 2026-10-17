#include "bitkv/store.hpp"

#include <cassert>
#include <optional>
#include <stdexcept>
#include <type_traits>
#include <utility>

namespace bitkv {

namespace {

static_assert(std::is_nothrow_move_assignable_v<std::string>,
              "std::string move-assign must be noexcept for set() commit path");

}  // namespace

Store::Store(std::size_t capacity, const Clock& clock)
    : capacity_(capacity), clock_(clock) {
  if (capacity_ == 0) {
    throw std::invalid_argument("bitkv::Store capacity must be > 0");
  }
  // clock_ is retained for later expiry sweeping; silence unused in phase 1.
  (void)clock_;
}

void Store::set_listener(MutationListener* listener) noexcept {
  assert_not_in_callback();
  listener_ = listener;
}

void Store::assert_not_in_callback() const noexcept {
  assert(!in_callback_ &&
         "MutationListener must not call back into Store/Engine");
}

bool Store::valid_key(std::string_view key) noexcept {
  return key.size() >= kMinKeyBytes && key.size() <= kMaxKeyBytes;
}

bool Store::valid_value(std::string_view value) noexcept {
  return value.size() <= kMaxValueBytes;
}

void Store::promote(EntryList::iterator it) noexcept {
  entries_.splice(entries_.begin(), entries_, it);
}

void Store::emit_set(std::string_view key,
                     std::string_view value,
                     std::int64_t expire_at_ms) const noexcept {
  if (listener_ == nullptr) {
    return;
  }
  in_callback_ = true;
  listener_->on_set(key, value, expire_at_ms);
  in_callback_ = false;
}

void Store::emit_del(std::string_view key) const noexcept {
  if (listener_ == nullptr) {
    return;
  }
  in_callback_ = true;
  listener_->on_del(key);
  in_callback_ = false;
}

Status Store::set(std::string_view key,
                  std::string_view value,
                  std::int64_t expire_at_ms) {
  assert_not_in_callback();

  if (!valid_key(key) || !valid_value(value) || expire_at_ms < 0) {
    return Status::InvalidArgument;
  }

  const auto found = index_.find(std::string(key));
  if (found != index_.end()) {
    // Allocate the replacement value before mutating; move-assign is noexcept.
    std::string new_value(value);
    EntryList::iterator it = found->second;
    it->value = std::move(new_value);
    it->expire_at_ms = expire_at_ms;
    promote(it);
    emit_set(it->key, it->value, it->expire_at_ms);
    return Status::Ok;
  }

  // --- New-key path: allocate everything that can throw first ---
  EntryList staging;
  staging.push_front(Entry{std::string(key), std::string(value), expire_at_ms});

  index_.reserve(index_.size() + 1);

  // If full, copy the victim key before any commit so a throw leaves Store intact.
  std::optional<std::string> victim_key;
  if (entries_.size() == capacity_) {
    victim_key = entries_.back().key;
  }

  // Map slot for the new key (points at staging). Throws => Store unchanged.
  const auto [map_it, inserted] =
      index_.emplace(staging.front().key, staging.begin());
  (void)map_it;
  if (!inserted) {
    // Unreachable on a single-threaded Store after the find-miss above: emplace
    // did not insert, so the Store is already unchanged.
    assert(false && "duplicate key after find miss");
    return Status::IoError;
  }

  // --- Commit ---
  // Every operation between the successful emplace above and the final splice
  // below is noexcept: unordered_map::erase(key) does not throw for std::string
  // keys; list::pop_back / splice do not throw; counter bump is trivial;
  // emit_* are noexcept (listener callbacks are noexcept).
  if (victim_key.has_value()) {
    index_.erase(*victim_key);  // noexcept for std::string key
    entries_.pop_back();        // noexcept
    ++counters_.evictions;      // noexcept
  }

  entries_.splice(entries_.begin(), staging);  // noexcept

  if (victim_key.has_value()) {
    emit_del(*victim_key);
  }
  emit_set(entries_.front().key, entries_.front().value,
           entries_.front().expire_at_ms);
  return Status::Ok;
}

std::optional<std::string> Store::get(std::string_view key) {
  assert_not_in_callback();

  const auto found = index_.find(std::string(key));
  if (found == index_.end()) {
    ++counters_.misses;
    return std::nullopt;
  }

  EntryList::iterator it = found->second;
  // Copy first so a throwing copy leaves LRU order and counters unchanged.
  std::string value_copy = it->value;
  promote(it);
  ++counters_.hits;
  return value_copy;
}

bool Store::del(std::string_view key) {
  assert_not_in_callback();

  const auto found = index_.find(std::string(key));
  if (found == index_.end()) {
    return false;
  }

  EntryList::iterator it = found->second;
  const std::string erased_key = it->key;
  index_.erase(found);
  entries_.erase(it);
  emit_del(erased_key);
  return true;
}

bool Store::exists(std::string_view key) const {
  assert_not_in_callback();
  return index_.find(std::string(key)) != index_.end();
}

std::size_t Store::size() const noexcept {
  assert_not_in_callback();
  return entries_.size();
}

std::size_t Store::capacity() const noexcept {
  assert_not_in_callback();
  return capacity_;
}

const Counters& Store::counters() const noexcept {
  assert_not_in_callback();
  return counters_;
}

}  // namespace bitkv
