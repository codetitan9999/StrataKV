# StrataKV

StrataKV is a compact C++ key-value store that explores how LSM-tree storage engines work: append-only writes, memtables, immutable sorted files, recovery, compaction, and measurement.

The API is small on purpose. The interesting part is inside the engine: how data moves from memory to disk, how crashes are recovered, and how performance changes as the storage layout evolves.

## What Works Today

- C++20/CMake project structure
- Public `stratakv::DB` API with `Put`, `Get`, `Delete`, and bounded streaming iterators
- Sorted in-memory memtable
- Descriptor-synced binary write-ahead log with per-record checksums and
  bounded recovery allocation
- Delete tombstones
- WAL replay on reopen, including torn-tail recovery
- Incrementally built, prefix-compressed multi-block SSTables with restart
  points, per-block checksums, an online Bloom filter, and an on-disk index
- Bloom-filtered, restart-indexed SSTable point reads with a database-wide
  encoded-block LRU cache and metrics
- Heap-merged streaming range and prefix scans across the memtable and SSTables
- Iterator snapshots that remain readable across compaction cleanup
- Memtable flush to SSTables with WAL rotation
- Reads from both memtable and flushed SSTables
- Checksummed manifest records with atomic snapshot replacement during compaction
- Manifest-persisted table levels with validated version-set invariants
- Durable SSTable and manifest installation with injectable filesystem failures
- Byte-scored, overlap-selected compaction through configurable sorted levels, with
  size-limited outputs, safe tombstone retention, and obsolete-file cleanup
- Fair compaction scheduling between level 0 and scored sorted levels
- Background compaction with streaming heap merges, incremental size-limited
  output, version-checked installation, bounded level-0 write stalls, and
  shutdown draining
- Cumulative and per-level compaction metrics for jobs, file counts, physical
  bytes, elapsed time, and benchmark write amplification
- Dependency-free unit tests
- Local benchmark harness for throughput and latency

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
4. Once the memtable crosses the write buffer limit, it is flushed to an immutable SSTable.
5. The table file is synced and installed with a directory sync, then a synced
   manifest record makes it discoverable on restart.
6. The WAL is rotated through a recoverable renamed generation with directory
   syncs after a successful flush.
7. When enough level-0 tables accumulate, compaction merges the oldest trigger
   batch with only its overlapping level-1 files and emits size-limited level-1
   tables. Sorted levels are scored against geometric byte targets; the most
   over-budget level compacts one bounded table with all overlaps in the next
   level. When both paths are ready, scheduling alternates between level 0 and
   sorted-level work so flush pressure cannot starve deeper levels. A dedicated
   worker executes this queue. It pins immutable inputs, heap-merges their lazy
   iterators directly into an incremental SSTable builder, and installs each
   size-limited output before producing the next.
   It releases the database mutex during that work, then validates the selected
   version and publishes a snapshot that includes any concurrent flushes.
   Foreground writes only stop when level 0 reaches
   `Options::level0_write_stall_trigger`. Each edit atomically replaces the
   manifest, and shutdown drains scheduled work before releasing database state.
8. On restart, manifest-listed SSTables are loaded first, then the WAL is replayed up to the last complete record.

`Options::max_wal_record_size` bounds both newly written records and recovery
payload allocation. WAL append and positional read operations share the
injectable filesystem boundary used by durability operations, making I/O
failures reproducible in tests.

The current database directory looks like this:

```text
db/
  wal/
    current.log
    previous.log  # only while rotation is incomplete
  sst/
    000001.sst
  MANIFEST
```

The larger design is documented in [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md).

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

- More recovery and corruption tests
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
