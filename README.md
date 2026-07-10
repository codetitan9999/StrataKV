# StrataKV

StrataKV is a compact C++ key-value store that explores how LSM-tree storage engines work: append-only writes, memtables, immutable sorted files, recovery, compaction, and measurement.

The API is small on purpose. The interesting part is inside the engine: how data moves from memory to disk, how crashes are recovered, and how performance changes as the storage layout evolves.

## What Works Today

- C++20/CMake project structure
- Public `stratakv::DB` API with `Put`, `Get`, `Delete`, and iterator support
- Sorted in-memory memtable
- Binary write-ahead log with per-record checksums
- Delete tombstones
- WAL replay on reopen
- Dependency-free unit tests
- Local benchmark harness for throughput and latency
- Initial interfaces for SSTables and compaction

## Quick Start

```sh
cmake -S . -B build
cmake --build build
ctest --test-dir build --output-on-failure
```

Run a simple local benchmark:

```sh
./build/stratakv_kv_bench 100000
```

## Design

The write path is intentionally straightforward:

1. Writes append to the WAL before updating the mutable memtable.
2. The memtable keeps keys sorted for point lookups and scans.
3. Deletes are stored as tombstones.
4. On restart, the WAL is replayed to rebuild the latest in-memory state.

The current database directory looks like this:

```text
db/
  wal/
    current.log
  sst/
```

The `sst/` directory is reserved for immutable table files. The larger design is documented in [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md).

## Repository Layout

```text
include/stratakv/   public API
src/                storage engine internals
tests/              unit tests
benchmarks/         benchmark entry points
docs/               architecture notes
```

## Roadmap

### Phase 1: Storage Engine

- SSTable writer/reader with block checksums
- Memtable flush and WAL rotation
- Manifest metadata for crash-safe file installation
- Reads across memtables and SSTables
- Range scans through merged iterators
- Compaction with tombstone handling
- Recovery and corruption tests
- Benchmarks for writes, reads, scans, recovery, and compaction

### Phase 2: Distributed Layer

- Network server and client
- Explicit request/response serialization
- Leader/follower replication model
- Restart recovery for followers
- Read consistency options
- Metrics for latency, throughput, WAL bytes, compaction work, and replication lag

## Non-Goals

- No consensus protocol until the storage engine is mature
- No web UI as a first milestone
- No large dependency stack unless it clearly improves the system
