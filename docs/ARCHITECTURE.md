# StrataKV Architecture

StrataKV is designed as a small but credible storage system. The guiding principle is to build the single-node storage engine first, then add distribution only after persistence, recovery, and compaction are testable.

## Project Name

**StrataKV** was chosen because the system is organized in storage strata: WAL, mutable memory state, immutable sorted files, compaction levels, and later replication layers. It is specific enough to be memorable without claiming compatibility with LevelDB, Bigtable, or Pebble.

## Major Modules

### Public API

- `include/stratakv/db.h`: user-facing database interface
- `include/stratakv/options.h`: read/write/open configuration
- `include/stratakv/iterator.h`: range-scan abstraction
- `include/stratakv/status.h`: explicit error propagation

The public API is deliberately small. It should remain stable while internals evolve.

### Write Path

- `src/wal.*`: append-only binary log with per-record checksums
- `src/manifest.*`: append-only metadata log for installed SSTables
- `src/memtable.*`: sorted mutable map storing latest value or tombstone
- `src/compaction.*`: merge logic for flushed SSTables
- `src/db.cc`: coordinates WAL append, optional sync, memtable apply, flush, table loading, and recovery

Current write order is WAL first, memtable second. When the memtable crosses the configured write buffer size, it is written to a numbered SSTable, recorded in the manifest, and then the WAL is rotated.

### Read Path

Reads follow this order:

1. mutable memtable
2. newest-to-oldest manifest-listed SSTables by key range
3. future leveled SSTables once multi-level compaction is introduced

Deletes are tombstones, not immediate removals from history. Compaction decides when a tombstone is safe to drop.

### SSTables

`src/sstable.*` implements the first immutable sorted table format. The current format stores one sorted data block and a fixed footer:

```text
data block: repeated sorted key/value entries
footer: entry count, data block size, checksum, magic
```

The reader verifies the data-block checksum before decoding entries, rejects unsorted or malformed tables, supports binary-search point lookups, and exposes an iterator. Multi-block tables, index blocks, and prefix compression remain future work.

### Compaction

`src/compaction.*` merges flushed SSTables in oldest-to-newest order. The first strategy is intentionally simple: when the number of flushed tables reaches `level0_compaction_trigger`, StrataKV compacts all active tables into one replacement table, records the replacement in the manifest, records old tables as deleted, and removes obsolete files.

Because this compaction covers every active table, tombstones that only protect against older tables can be dropped from the compacted output.

### Manifest

`src/manifest.*` stores checksummed table metadata records. On open, StrataKV replays table-add and table-delete edits to decide which SSTables are installed, then replays the WAL for any writes that were not flushed. Directory scans are no longer the source of truth for table membership.

## Storage Model

StrataKV uses an LSM-tree model:

- Writes are sequential and durable through the WAL.
- Recent state lives in a sorted memtable.
- Flushes produce immutable sorted table files.
- Reads merge state from memory and disk.
- Compaction reorganizes files to control read amplification and disk usage.

The core invariants are:

- Sequence numbers define write order.
- WAL replay must reconstruct all acknowledged writes.
- Newer records shadow older records for the same key.
- Tombstones shadow older values until compaction proves they are obsolete.
- Installed SSTables must be discoverable after crash recovery.

## File Layout

Repository layout:

```text
include/stratakv/   public API
src/                storage engine internals
tests/              dependency-free unit tests
benchmarks/         local benchmark executables
docs/               architecture and milestone notes
```

Runtime database layout:

```text
db/
  wal/
    current.log
  sst/
    000001.sst
    000002.sst
  MANIFEST
```

The manifest currently records table creation and table deletion. Later work can compact the manifest itself into a smaller snapshot.

## Test Strategy

Current tests cover:

- Put/Get/Delete semantics
- Iterator ordering and tombstone hiding
- WAL replay across reopen
- SSTable round trips, sorted iteration, key ordering validation, and checksum corruption detection
- Memtable flush, SSTable-backed reads, flushed tombstones, and reopen from table files
- Manifest replay, invalid metadata rejection, checksum corruption detection, and missing table handling
- Compaction merging, tombstone handling, obsolete-file cleanup, and reopen from compacted state

Next test layers should add:

- WAL corruption and partial-record recovery behavior
- Streaming iterator merge correctness across memtable and SSTables
- Multi-level compaction correctness with overwritten keys and tombstones
- Fault injection around file creation, rename, and manifest updates

The project starts with a tiny local harness to avoid dependency friction. Once behavior broadens, moving to GoogleTest is reasonable.

## Benchmark Strategy

The current benchmark measures local `Put` and random `Get` performance on the WAL + memtable path. It reports write throughput, read throughput, and get latency percentiles.

Future benchmark tracks:

- sequential write throughput with sync off and sync on
- random point-read latency against warm and cold SSTables
- range-scan throughput
- recovery time by WAL size
- compaction throughput and write amplification
- network request latency after Phase 2

Benchmarks should use fixed seeds, report configuration, and preserve enough metadata to compare runs over time.

## Phased Roadmap

### Milestone 1: Storage Skeleton Hardening

- Add version-set types
- Add structured logging around open/recovery
- Add fault-injection hooks for filesystem operations

### Milestone 2: SSTable Format

- Add multi-block tables and index blocks
- Add golden tests for table encoding

### Milestone 3: Flush and Recovery

- Compact manifest snapshots
- Add fault-injection tests around manifest/table installation

### Milestone 4: Iterators and Compaction

- Merge iterators across memory and SSTables
- Implement range scans
- Add overlap-aware leveled compaction
- Track read/write amplification in benchmarks

### Milestone 5: Networked Store

- Add request/response protocol
- Build a simple server and client
- Expose metrics
- Benchmark local and network paths separately

### Milestone 6: Replication

- Add leader/follower roles
- Replicate log entries before acknowledgement under configurable policy
- Recover followers from log/table state
- Document consistency tradeoffs and failure cases
