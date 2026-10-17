# BitKV Architecture Contract

This document is the binding design contract for BitKV. Implementation phases must
conform to it. Cursor rule `.cursor/rules/bitkv.mdc` carries the same body.

## Language and dependencies

- **C++20** only. Portable to Linux, macOS, and Windows (MSVC, GCC, Clang).
- **STL only at runtime.** No Boost, Abseil, or other third-party runtime deps.
- **GoogleTest** is allowed **only** for tests, via CMake `FetchContent`, behind
  option `BITKV_BUILD_TESTS`.
- Do **not** use `std::jthread`, `std::barrier`, or `std::latch` (libc++ gaps).
  Use `std::thread` + atomic stop flag + `std::condition_variable`.

## Namespace and layout

- Namespace: `bitkv`.
- Layout:
  - `include/bitkv/*.hpp` — public headers
  - `src/*.cpp` — implementations and CLI
  - `tests/` — GoogleTest
  - `bench/`, `examples/`, `scripts/`, `docs/`

## Core data structure (per shard / Store)

Each shard (and the phase-1 single-threaded `Store`) holds:

```text
unordered_map<string, list<Entry>::iterator>  // O(1) lookup
std::list<Entry>                              // front = MRU, back = LRU
```

```cpp
struct Entry {
  std::string key;
  std::string value;
  std::int64_t expire_at_ms;  // 0 = no expiry; negative rejected at SET
};
```

## Time

Injectable clock interface:

```cpp
class Clock {
 public:
  virtual ~Clock() = default;
  [[nodiscard]] virtual std::int64_t now_ms() const = 0;
};
```

- `SystemClock` — `std::chrono::system_clock` epoch milliseconds.
- `ManualClock` — test-controlled time.
- All expiry is **absolute epoch ms**. Relative TTLs are converted at SET time by
  callers (later API layers), never stored as relative durations.

## Status and limits

```cpp
enum class Status { Ok, NotFound, InvalidArgument, IoError };
```

| Constraint | Limit |
|------------|-------|
| Key length | 1 .. 1024 bytes |
| Value size | 0 .. 1 MiB (empty value allowed) |

Violations return `Status::InvalidArgument` on hot paths (by design, not via
exceptions).

## MutationListener

```cpp
class MutationListener {
 public:
  virtual ~MutationListener() = default;
  virtual void on_set(std::string_view key, std::string_view value,
                      std::int64_t expire_at_ms) noexcept = 0;
  virtual void on_del(std::string_view key) noexcept = 0;
};
```

### When events fire

| Event | Emits |
|-------|-------|
| Explicit SET | `on_set` |
| Explicit DEL | `on_del` |
| LRU eviction | `on_del` (evicted key) |
| Expiry / TTL cleanup | **does not emit** |

### Why expiry does not emit

The WAL SET record stores the absolute `expire_at_ms`. On replay, expired records
are dropped without needing a separate DEL. Emitting `on_del` for expiry would
re-log deletes that are redundant with the absolute expiry already on disk, and
would amplify WAL traffic under TTL churn. LRU eviction **does** emit `on_del`
because the key is removed while still logically live (no expiry-based drop).

### Replay ordering

Replay of the WAL happens **before** any `MutationListener` is attached. Replay
therefore cannot re-log mutations through the listener.

### Listener lifetime and re-entrancy

- The caller owns the `MutationListener` and must keep it alive until
  `set_listener(nullptr)` or until the `Store` (later: `Engine`) is destroyed.
- Listeners **must not** throw and **must not** call back into the `Store` or
  `Engine`. Re-entrancy is a programming error; debug builds assert via an
  `in_callback_` guard. An implementation that can fail (e.g. the WAL listener)
  must record the failure internally and surface it through its own channel.

## Store (phase 1)

- `Store` is **not thread-safe**. Documented in the header.
- Phase 3 `Engine` wraps per-shard `Store` instances with mutexes.
- `EXISTS` does **not** promote to MRU (unlike `GET`).
- `capacity == 0` is invalid (`std::invalid_argument` in the constructor).
- SET on an existing key at full capacity **must not** evict; it overwrites,
  promotes, and resets `expire_at_ms` (0 clears TTL).
- `expire_at_ms < 0` is invalid (`Status::InvalidArgument`). `0` means no expiry.
- Expiry fields are stored in phase 1 but not actively swept; `get`/`exists` may
  still return expired entries until a later sweeper phase.
- `set()` provides the **strong exception guarantee** on allocation failure: if
  allocation throws before commit, the Store is unchanged. Listener callbacks
  run only after the in-memory mutation is committed.

## Engine sharding (phase 3+)

- Shard index: `shard = fnv1a64(key) & (N - 1)`.
- `N` is a power of two; default `N = 16`.
- Per-shard capacity = `ceil(total_capacity / N)`.
- LRU is **exact per shard** and **approximate globally**. Document this to users.
- `shards = 1` is the global-lock baseline (single mutex, exact global LRU).

### Lock order

1. Shard mutexes in **ascending** shard index.
2. Then the WAL mutex.

Never reverse this order (deadlock hazard).

## WAL (phase 2+)

- Single append-only file.
- Binary, little-endian, explicit serialization.
- CRC32 over each record payload.
- File header: magic + version.
- On replay, a torn tail (incomplete final record) is **truncated**.

## Coding style

- RAII only; no raw `new` / `delete`.
- Const-correct; prefer `[[nodiscard]]` on status-returning APIs.
- Store operations may throw `std::bad_alloc` only. `set()` offers the strong
  exception guarantee. `get`/`del`/`exists` never leave partial state. All
  listener callbacks are `noexcept`. Exceptions other than `bad_alloc` are
  reserved for startup I/O failures and invalid constructor arguments (such as
  `capacity == 0`).
- Warnings as errors:
  - GCC/Clang: `-Wall -Wextra -Wpedantic -Wconversion -Wshadow` (+ `-Werror`)
  - MSVC: `/W4 /WX`
- No `using namespace` in headers.
