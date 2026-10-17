#include "bitkv/clock.hpp"
#include "bitkv/listener.hpp"
#include "bitkv/status.hpp"
#include "bitkv/store.hpp"

#include <gtest/gtest.h>

#include <chrono>
#include <cstdint>
#include <limits>
#include <list>
#include <optional>
#include <random>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace {

using bitkv::ManualClock;
using bitkv::MutationListener;
using bitkv::Status;
using bitkv::Store;
using bitkv::SystemClock;

class RecordingListener final : public MutationListener {
 public:
  enum class Kind { Set, Del };

  struct Event {
    Kind kind;
    std::string key;
    std::string value;
    std::int64_t expire_at_ms = 0;
  };

  void on_set(std::string_view key,
              std::string_view value,
              std::int64_t expire_at_ms) noexcept override {
    events_.push_back(
        Event{Kind::Set, std::string(key), std::string(value), expire_at_ms});
  }

  void on_del(std::string_view key) noexcept override {
    events_.push_back(Event{Kind::Del, std::string(key), {}, 0});
  }

  [[nodiscard]] const std::vector<Event>& events() const noexcept {
    return events_;
  }

  void clear() {
    events_.clear();
  }

 private:
  std::vector<Event> events_;
};

/// Naive reference model mirroring Store LRU semantics for differential tests.
class ReferenceStore {
 public:
  explicit ReferenceStore(std::size_t capacity) : capacity_(capacity) {
    if (capacity_ == 0) {
      throw std::invalid_argument("capacity");
    }
  }

  Status set(std::string_view key,
             std::string_view value,
             std::int64_t expire_at_ms = 0) {
    if (key.empty() || key.size() > Store::kMaxKeyBytes ||
        value.size() > Store::kMaxValueBytes) {
      return Status::InvalidArgument;
    }

    const auto found = index_.find(std::string(key));
    if (found != index_.end()) {
      auto it = found->second;
      it->value.assign(value.data(), value.size());
      it->expire_at_ms = expire_at_ms;
      entries_.splice(entries_.begin(), entries_, it);
      return Status::Ok;
    }

    if (entries_.size() == capacity_) {
      index_.erase(entries_.back().key);
      entries_.pop_back();
      ++evictions_;
    }

    entries_.push_front(
        Entry{std::string(key), std::string(value), expire_at_ms});
    index_.emplace(entries_.front().key, entries_.begin());
    return Status::Ok;
  }

  std::optional<std::string> get(std::string_view key) {
    const auto found = index_.find(std::string(key));
    if (found == index_.end()) {
      return std::nullopt;
    }
    entries_.splice(entries_.begin(), entries_, found->second);
    return found->second->value;
  }

  bool del(std::string_view key) {
    const auto found = index_.find(std::string(key));
    if (found == index_.end()) {
      return false;
    }
    entries_.erase(found->second);
    index_.erase(found);
    return true;
  }

  [[nodiscard]] bool exists(std::string_view key) const {
    return index_.find(std::string(key)) != index_.end();
  }

  [[nodiscard]] std::size_t size() const noexcept {
    return entries_.size();
  }

  [[nodiscard]] std::vector<std::string> mru_to_lru_keys() const {
    std::vector<std::string> keys;
    keys.reserve(entries_.size());
    for (const auto& e : entries_) {
      keys.push_back(e.key);
    }
    return keys;
  }

  [[nodiscard]] std::uint64_t evictions() const noexcept {
    return evictions_;
  }

 private:
  struct Entry {
    std::string key;
    std::string value;
    std::int64_t expire_at_ms = 0;
  };

  std::size_t capacity_;
  std::list<Entry> entries_;
  std::unordered_map<std::string, std::list<Entry>::iterator> index_;
  std::uint64_t evictions_ = 0;
};

}  // namespace

TEST(StoreDeath, CapacityZeroThrows) {
  ManualClock clock;
  EXPECT_THROW(Store(0, clock), std::invalid_argument);
}

TEST(StoreLru, GetPromotesToMru) {
  ManualClock clock;
  Store store(3, clock);

  ASSERT_EQ(store.set("a", "1"), Status::Ok);
  ASSERT_EQ(store.set("b", "2"), Status::Ok);
  ASSERT_EQ(store.set("c", "3"), Status::Ok);

  // Touch "a" so it becomes MRU; next eviction should remove "b" (oldest untouched).
  ASSERT_EQ(store.get("a"), std::optional<std::string>{"1"});
  ASSERT_EQ(store.set("d", "4"), Status::Ok);

  EXPECT_TRUE(store.exists("a"));
  EXPECT_FALSE(store.exists("b"));
  EXPECT_TRUE(store.exists("c"));
  EXPECT_TRUE(store.exists("d"));
  EXPECT_EQ(store.counters().evictions, 1U);
  EXPECT_EQ(store.counters().hits, 1U);
}

TEST(StoreLru, ExistsDoesNotPromote) {
  ManualClock clock;
  Store store(3, clock);

  ASSERT_EQ(store.set("a", "1"), Status::Ok);
  ASSERT_EQ(store.set("b", "2"), Status::Ok);
  ASSERT_EQ(store.set("c", "3"), Status::Ok);

  EXPECT_TRUE(store.exists("a"));
  ASSERT_EQ(store.set("d", "4"), Status::Ok);

  // Without promote, "a" remains LRU and is evicted.
  EXPECT_FALSE(store.exists("a"));
  EXPECT_TRUE(store.exists("b"));
  EXPECT_TRUE(store.exists("c"));
  EXPECT_TRUE(store.exists("d"));
  EXPECT_EQ(store.counters().hits, 0U);
  EXPECT_EQ(store.counters().misses, 0U);
}

TEST(StoreLru, OverwriteAtFullCapacityDoesNotEvict) {
  ManualClock clock;
  Store store(2, clock);
  RecordingListener listener;
  store.set_listener(&listener);

  ASSERT_EQ(store.set("a", "1"), Status::Ok);
  ASSERT_EQ(store.set("b", "2"), Status::Ok);
  listener.clear();

  ASSERT_EQ(store.set("a", "updated", 100), Status::Ok);

  EXPECT_EQ(store.size(), 2U);
  EXPECT_EQ(store.counters().evictions, 0U);
  ASSERT_EQ(store.get("a"), std::optional<std::string>{"updated"});

  // "a" was promoted; inserting "c" should evict "b".
  ASSERT_EQ(store.set("c", "3"), Status::Ok);
  EXPECT_FALSE(store.exists("b"));
  EXPECT_TRUE(store.exists("a"));
  EXPECT_TRUE(store.exists("c"));
}

TEST(StoreLru, CapacityOneInsertAndOverwrite) {
  ManualClock clock;
  Store store(1, clock);

  ASSERT_EQ(store.set("a", "1"), Status::Ok);
  EXPECT_EQ(store.size(), 1U);

  ASSERT_EQ(store.set("b", "2"), Status::Ok);
  EXPECT_EQ(store.size(), 1U);
  EXPECT_FALSE(store.exists("a"));
  EXPECT_EQ(store.get("b"), std::optional<std::string>{"2"});
  EXPECT_EQ(store.counters().evictions, 1U);

  ASSERT_EQ(store.set("b", "3"), Status::Ok);
  EXPECT_EQ(store.size(), 1U);
  EXPECT_EQ(store.counters().evictions, 1U);
  EXPECT_EQ(store.get("b"), std::optional<std::string>{"3"});
}

TEST(StoreLru, EvictionOrderLongSequence) {
  ManualClock clock;
  Store store(3, clock);

  for (int i = 0; i < 10; ++i) {
    const std::string key = "k" + std::to_string(i);
    ASSERT_EQ(store.set(key, std::to_string(i)), Status::Ok);
  }

  EXPECT_EQ(store.size(), 3U);
  EXPECT_EQ(store.counters().evictions, 7U);
  EXPECT_FALSE(store.exists("k6"));
  EXPECT_TRUE(store.exists("k7"));
  EXPECT_TRUE(store.exists("k8"));
  EXPECT_TRUE(store.exists("k9"));
}

TEST(StoreListener, EventSequenceSetDelEviction) {
  ManualClock clock;
  Store store(2, clock);
  RecordingListener listener;
  store.set_listener(&listener);

  ASSERT_EQ(store.set("a", "1", 0), Status::Ok);
  ASSERT_EQ(store.set("b", "2", 50), Status::Ok);
  EXPECT_TRUE(store.del("a"));
  ASSERT_EQ(store.set("c", "3"), Status::Ok);  // no eviction (size 1 after del)
  ASSERT_EQ(store.set("d", "4"), Status::Ok);  // evicts "b"

  const auto& events = listener.events();
  ASSERT_EQ(events.size(), 6U);

  EXPECT_EQ(events[0].kind, RecordingListener::Kind::Set);
  EXPECT_EQ(events[0].key, "a");
  EXPECT_EQ(events[0].value, "1");
  EXPECT_EQ(events[0].expire_at_ms, 0);

  EXPECT_EQ(events[1].kind, RecordingListener::Kind::Set);
  EXPECT_EQ(events[1].key, "b");
  EXPECT_EQ(events[1].expire_at_ms, 50);

  EXPECT_EQ(events[2].kind, RecordingListener::Kind::Del);
  EXPECT_EQ(events[2].key, "a");

  EXPECT_EQ(events[3].kind, RecordingListener::Kind::Set);
  EXPECT_EQ(events[3].key, "c");

  EXPECT_EQ(events[4].kind, RecordingListener::Kind::Del);
  EXPECT_EQ(events[4].key, "b");

  EXPECT_EQ(events[5].kind, RecordingListener::Kind::Set);
  EXPECT_EQ(events[5].key, "d");
}

TEST(StoreValidation, InvalidKeyAndValueSizes) {
  ManualClock clock;
  Store store(4, clock);

  EXPECT_EQ(store.set("", "v"), Status::InvalidArgument);
  EXPECT_EQ(store.set(std::string(Store::kMaxKeyBytes + 1, 'k'), "v"),
            Status::InvalidArgument);
  EXPECT_EQ(store.set("k", std::string(Store::kMaxValueBytes + 1, 'v')),
            Status::InvalidArgument);

  EXPECT_EQ(store.set("ok", ""), Status::Ok);
  EXPECT_EQ(store.get("ok"), std::optional<std::string>{""});
  EXPECT_EQ(store.size(), 1U);
}

TEST(StoreValidation, BinaryValuesWithEmbeddedNulAndNewline) {
  ManualClock clock;
  Store store(2, clock);

  const std::string value = std::string("a\0b\nc", 5);
  ASSERT_EQ(store.set("bin", value), Status::Ok);

  const auto got = store.get("bin");
  ASSERT_TRUE(got.has_value());
  EXPECT_EQ(*got, value);
  EXPECT_EQ(got->size(), 5U);
}

TEST(StoreDifferential, RandomizedAgainstReferenceModel) {
  constexpr std::size_t kCapacity = 32;
  constexpr int kOps = 10000;
  constexpr std::uint32_t kSeed = 0xB17C0001u;

  ManualClock clock;
  Store store(kCapacity, clock);
  ReferenceStore ref(kCapacity);

  std::mt19937 rng(kSeed);
  std::uniform_int_distribution<int> op_dist(0, 3);
  std::uniform_int_distribution<int> key_dist(0, 63);
  std::uniform_int_distribution<int> val_dist(0, 255);
  std::uniform_int_distribution<int> ttl_dist(0, 1);

  for (int i = 0; i < kOps; ++i) {
    const std::string key = "k" + std::to_string(key_dist(rng));
    const int op = op_dist(rng);

    switch (op) {
      case 0: {
        const std::string value(1, static_cast<char>(val_dist(rng)));
        const std::int64_t ttl = ttl_dist(rng) == 0 ? 0 : 1'000'000;
        EXPECT_EQ(store.set(key, value, ttl), ref.set(key, value, ttl));
        break;
      }
      case 1: {
        EXPECT_EQ(store.get(key), ref.get(key));
        break;
      }
      case 2: {
        EXPECT_EQ(store.del(key), ref.del(key));
        break;
      }
      case 3: {
        EXPECT_EQ(store.exists(key), ref.exists(key));
        break;
      }
      default:
        break;
    }

    EXPECT_EQ(store.size(), ref.size());
  }

  EXPECT_EQ(store.counters().evictions, ref.evictions());
}

TEST(StoreCounters, MissIncrements) {
  ManualClock clock;
  Store store(2, clock);
  EXPECT_EQ(store.get("missing"), std::nullopt);
  EXPECT_EQ(store.counters().misses, 1U);
  EXPECT_EQ(store.counters().hits, 0U);
}

TEST(ManualClock, Advances) {
  ManualClock clock(100);
  EXPECT_EQ(clock.now_ms(), 100);
  clock.advance_ms(50);
  EXPECT_EQ(clock.now_ms(), 150);
  clock.set_now_ms(0);
  EXPECT_EQ(clock.now_ms(), 0);
}

TEST(ManualClock, AdvanceSaturates) {
  ManualClock clock(std::numeric_limits<std::int64_t>::max() - 10);
  clock.advance_ms(100);
  EXPECT_EQ(clock.now_ms(), std::numeric_limits<std::int64_t>::max());

  clock.set_now_ms(std::numeric_limits<std::int64_t>::min() + 10);
  clock.advance_ms(-100);
  EXPECT_EQ(clock.now_ms(), std::numeric_limits<std::int64_t>::min());
}

TEST(StoreTtl, OverwriteWithZeroClearsExpireAtMs) {
  ManualClock clock;
  Store store(2, clock);
  RecordingListener listener;
  store.set_listener(&listener);

  ASSERT_EQ(store.set("k", "v", 999), Status::Ok);
  ASSERT_EQ(listener.events().back().expire_at_ms, 999);

  ASSERT_EQ(store.set("k", "v2", 0), Status::Ok);
  ASSERT_EQ(listener.events().back().kind, RecordingListener::Kind::Set);
  EXPECT_EQ(listener.events().back().key, "k");
  EXPECT_EQ(listener.events().back().value, "v2");
  EXPECT_EQ(listener.events().back().expire_at_ms, 0);
}

TEST(StoreValidation, ExactLimitSizesAccepted) {
  ManualClock clock;
  Store store(2, clock);

  const std::string key(Store::kMaxKeyBytes, 'k');
  const std::string value(Store::kMaxValueBytes, 'v');
  EXPECT_EQ(key.size(), Store::kMaxKeyBytes);
  EXPECT_EQ(value.size(), Store::kMaxValueBytes);

  ASSERT_EQ(store.set(key, "x"), Status::Ok);
  ASSERT_EQ(store.set("short", value), Status::Ok);
  EXPECT_EQ(store.get(key), std::optional<std::string>{"x"});
  EXPECT_EQ(store.get("short"), std::optional<std::string>{value});
}

TEST(StoreListener, NullListenerStopsEvents) {
  ManualClock clock;
  Store store(2, clock);
  RecordingListener listener;
  store.set_listener(&listener);

  ASSERT_EQ(store.set("a", "1"), Status::Ok);
  EXPECT_EQ(listener.events().size(), 1U);

  store.set_listener(nullptr);
  listener.clear();
  ASSERT_EQ(store.set("b", "2"), Status::Ok);
  EXPECT_TRUE(store.del("a"));
  EXPECT_TRUE(listener.events().empty());
}

TEST(StoreDel, MissingKeyReturnsFalseWithoutListenerEvent) {
  ManualClock clock;
  Store store(2, clock);
  RecordingListener listener;
  store.set_listener(&listener);

  ASSERT_EQ(store.set("a", "1"), Status::Ok);
  listener.clear();

  EXPECT_FALSE(store.del("missing"));
  EXPECT_TRUE(listener.events().empty());
  EXPECT_TRUE(store.exists("a"));
}

TEST(SystemClockTest, ReturnsPlausibleEpochMs) {
  SystemClock clock;
  const std::int64_t now = clock.now_ms();

  // 2020-01-01 UTC approx through a far-future bound.
  constexpr std::int64_t kMinPlausible = 1'577'836'800'000LL;
  constexpr std::int64_t kMaxPlausible = 4'102'444'800'000LL;  // ~2100
  EXPECT_GE(now, kMinPlausible);
  EXPECT_LE(now, kMaxPlausible);

  // Should be within a generous skew of wall clock from <chrono>.
  const auto wall = std::chrono::duration_cast<std::chrono::milliseconds>(
                        std::chrono::system_clock::now().time_since_epoch())
                        .count();
  EXPECT_NEAR(static_cast<double>(now), static_cast<double>(wall), 5'000.0);
}

TEST(StoreSet, StrongGuaranteeDocumentedUntestableWithoutCustomAllocator) {
  // set() allocates the staging list node, Entry strings, and map slot before
  // any LRU eviction or in-place mutation. Injecting a hard std::bad_alloc at
  // those seams requires a replaceable global operator new / custom allocator
  // wired through std::list and std::unordered_map — not feasible with the
  // default STL containers without invading production APIs.
  //
  // What we can check: overwrite allocates the new value before mutating, so a
  // successful overwrite at full capacity never evicts (strong path for that
  // branch is covered by StoreLru.OverwriteAtFullCapacityDoesNotEvict).
  ManualClock clock;
  Store store(1, clock);
  ASSERT_EQ(store.set("a", "1"), Status::Ok);
  ASSERT_EQ(store.set("a", "2"), Status::Ok);
  EXPECT_EQ(store.size(), 1U);
  EXPECT_EQ(store.counters().evictions, 0U);
  EXPECT_EQ(store.get("a"), std::optional<std::string>{"2"});
  SUCCEED() << "OOM strong-guarantee path documented as untestable here";
}

TEST(StoreValidation, NegativeExpireAtMsRejected) {
  ManualClock clock;
  Store store(2, clock);
  EXPECT_EQ(store.set("k", "v", -1), Status::InvalidArgument);
  EXPECT_EQ(store.set("k", "v", -999), Status::InvalidArgument);
  EXPECT_FALSE(store.exists("k"));
  EXPECT_EQ(store.set("k", "v", 0), Status::Ok);
}

TEST(StoreValidation, KeyWithEmbeddedNulAndNewlineRoundTrips) {
  ManualClock clock;
  Store store(2, clock);
  const std::string key = std::string("a\0b\nc", 5);
  ASSERT_EQ(store.set(key, "val"), Status::Ok);
  EXPECT_TRUE(store.exists(key));
  EXPECT_EQ(store.get(key), std::optional<std::string>{"val"});
}

TEST(StoreValidation, ExactLimitKeyAndValueTogether) {
  ManualClock clock;
  Store store(1, clock);
  const std::string key(Store::kMaxKeyBytes, 'k');
  const std::string value(Store::kMaxValueBytes, 'v');
  ASSERT_EQ(store.set(key, value), Status::Ok);
  EXPECT_EQ(store.get(key), std::optional<std::string>{value});
}

// PHASE2_DELETE_OR_UPDATE: once expiry sweeping lands, past-due entries must
// not be returned by get/exists (or must be cleaned first).
TEST(StorePhase1, PastExpireAtMsStillReturnedByGetAndExistsUntilPhase2) {
  ManualClock clock(1'000);
  Store store(2, clock);
  ASSERT_EQ(store.set("ttl", "x", 1'500), Status::Ok);
  clock.set_now_ms(2'000);
  EXPECT_TRUE(store.exists("ttl"));
  EXPECT_EQ(store.get("ttl"), std::optional<std::string>{"x"});
}

TEST(StoreGet, HitMissCountersAndLruPromoteConsistent) {
  ManualClock clock;
  Store store(3, clock);
  ASSERT_EQ(store.set("a", "1"), Status::Ok);
  ASSERT_EQ(store.set("b", "2"), Status::Ok);
  ASSERT_EQ(store.set("c", "3"), Status::Ok);

  EXPECT_EQ(store.counters().hits, 0U);
  EXPECT_EQ(store.counters().misses, 0U);

  EXPECT_EQ(store.get("missing"), std::nullopt);
  EXPECT_EQ(store.counters().misses, 1U);
  EXPECT_EQ(store.counters().hits, 0U);

  EXPECT_EQ(store.get("a"), std::optional<std::string>{"1"});
  EXPECT_EQ(store.counters().hits, 1U);
  EXPECT_EQ(store.counters().misses, 1U);

  // "a" was promoted; inserting "d" at capacity should evict "b" (LRU), not "a".
  ASSERT_EQ(store.set("d", "4"), Status::Ok);
  EXPECT_FALSE(store.exists("b"));
  EXPECT_TRUE(store.exists("a"));
  EXPECT_EQ(store.counters().hits, 1U);
}
