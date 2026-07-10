# StrataKV

StrataKV is a C++ storage-engine project aimed at demonstrating real systems depth: persistent writes, LSM-style storage internals, crash recovery, compaction, benchmarking, and eventually a small distributed replication layer.

The project is intentionally scoped like a serious engineering portfolio piece rather than a broad CRUD app. Phase 1 focuses on a single-node persistent key-value store inspired by LevelDB/Bigtable concepts. Phase 2 adds networking and replication once the storage core has earned it.

## Current Status

Milestone 0 scaffolds the foundation:

- CMake-based C++20 project layout
- Public `stratakv::DB` API with `Put`, `Get`, `Delete`, and iterators
- In-memory memtable backed by a binary write-ahead log
- WAL replay on database reopen
- Internal extension points for SSTable read/write and compaction
- Dependency-free unit tests
- Simple local benchmark harness for throughput and latency

## Build

```sh
cmake -S . -B build
cmake --build build
```

## Test

```sh
ctest --test-dir build --output-on-failure
```

## Benchmark

```sh
./build/stratakv_kv_bench 100000
```

The current benchmark measures the in-process WAL + memtable path. Later milestones will add separate benchmarks for recovery, SSTable reads, compaction, range scans, and networked operations.

## Architecture Snapshot

The first implementation uses a classic LSM-tree shape:

1. Writes append to the WAL before updating the mutable memtable.
2. Reads check the memtable.
3. Deletes are represented as tombstones.
4. Reopening the database replays the WAL to reconstruct the memtable.
5. Future flushes will write immutable SSTables under `sst/`.
6. Future compaction will merge SSTables, discard shadowed values, and preserve tombstone semantics until safe.

On disk, a database directory currently looks like this:

```text
db/
  wal/
    current.log
  sst/
```

See [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md) for the full plan.

## Roadmap

### Phase 1: Single-Node Storage Engine

- Durable WAL format with checksums and recovery tests
- Mutable memtable and immutable memtable flush path
- SSTable writer/reader with sorted blocks, index block, footer, and checksums
- Point reads across memtable and SSTables
- Iterators and range scans across levels
- Size-tiered or leveled compaction
- Manifest/version metadata and crash-safe file installation
- Fault-injection tests for partial writes and recovery
- Benchmarks for write throughput, read latency, range scans, recovery, and compaction

### Phase 2: Networked Replicated Store

- TCP/gRPC-like server API with explicit request/response serialization
- Replicated write path with leader/follower roles
- Recovery after follower restart
- Configurable consistency tradeoffs for reads
- Metrics for latency, throughput, WAL bytes, compaction work, and replication lag

### Non-Goals For Now

- No Raft or multi-leader design until the single-node storage core is mature
- No web UI until the systems behavior is worth visualizing
- No large dependency stack unless a dependency earns its complexity

## Resume Signal

The intended end state should support resume bullets around:

- Implemented an LSM-tree key-value store in modern C++ with WAL, SSTables, compaction, and crash recovery.
- Built benchmark suites tracking write throughput, read latency percentiles, range scan performance, and recovery time.
- Designed a simplified distributed replication layer with explicit consistency and failure-recovery tradeoffs.
