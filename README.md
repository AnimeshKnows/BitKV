# BitKV

Showcase-grade, zero-dependency in-memory key-value engine in C++20
(Redis-style TTL/LRU + Bitcask-style append-only WAL).

**Status:** Phase 1 — single-threaded `Store` core. `Store` is **not**
thread-safe; a thread-safe sharded `Engine` arrives in a later phase.

## Requirements

- CMake 3.20+
- C++20 compiler (MSVC, GCC, or Clang)
- Ninja (optional; use `*-ninja` presets when available)

## Build

Default generator (works without Ninja):

```bash
cmake --preset release
cmake --build --preset release
```

Ninja:

```bash
cmake --preset release-ninja
cmake --build --preset release-ninja
```

Windows MSVC multi-config:

```bash
cmake --preset release-msvc
cmake --build --preset release-msvc
```

## Test

```bash
ctest --preset release --output-on-failure
```

Debug:

```bash
cmake --preset debug
cmake --build --preset debug
ctest --preset debug --output-on-failure
```

Address + Undefined sanitizers (GCC/Clang; uses Ninja):

```bash
cmake --preset asan
cmake --build --preset asan
ctest --preset asan --output-on-failure
```

## Layout

| Path | Purpose |
|------|---------|
| `include/bitkv/` | Public headers |
| `src/` | Library + CLI sources |
| `tests/` | GoogleTest suite |
| `docs/ARCHITECTURE.md` | Binding design contract |
| `bench/`, `examples/`, `scripts/` | Reserved for later phases |

## License

MIT — see [LICENSE](LICENSE).
