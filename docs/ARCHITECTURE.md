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

WAL replay applies every complete record with a valid checksum. If a crash leaves a partial final header or payload, replay stops at the complete prefix. A full record with a checksum mismatch is treated as corruption.

### Read Path

Reads follow this order:

1. mutable memtable
2. newest-to-oldest manifest-listed SSTables by key range
3. future leveled SSTables once multi-level compaction is introduced

Deletes are tombstones, not immediate removals from history. Compaction decides when a tombstone is safe to drop.

Database iterators take a stable snapshot of the current source set and perform
an incremental k-way merge using a min-heap frontier. This makes each emitted
key logarithmic in the number of active sources instead of scanning every
source. The mutable memtable is copied at iterator creation,
while immutable table readers are retained by shared ownership. The iterator
keeps only its current entry per source and the active decoded SSTable blocks in
memory; it does not materialize all table contents. Newer sources win when keys
overlap, tombstones hide older values, and block I/O or checksum failures are
reported through `Iterator::status()` when traversal reaches the affected block.
When compaction retires an SSTable, physical deletion is deferred until the
last reader is released. Iterators can therefore continue lazy block reads from
their pre-compaction snapshot, and obsolete files are reclaimed when the final
snapshot releases them.
`ReadOptions` can provide an inclusive lower bound, an exclusive upper bound,
and a prefix. These constraints are intersected, and each child iterator seeks
directly to the effective start key so selective scans avoid decoding preceding
blocks. The merge stops as soon as the upper bound or prefix range is exhausted.

### SSTables

`src/sstable.*` implements the immutable sorted table format. Writers split
sorted entries into target-sized data blocks. Each data block has its own
checksum, and an index maps each block's largest key to its byte range:

```text
data block 0: repeated sorted key/value entries, checksum
data block 1: repeated sorted key/value entries, checksum
...
index block: largest key, block offset, block size, entry count
footer: total entry count, index location, index checksum, format magic
```

The reader loads and verifies only the footer, index, and first data block at
open. Point lookups binary-search the index and read the selected data block on
demand. Decoded blocks are retained in a database-wide LRU cache keyed by
table path and block byte range. `Options::block_cache_size` is a single memory
budget shared by every open table, preventing memory use from scaling with the
SSTable count. `DB::GetBlockCacheStats` reports hits, misses, evictions, usage,
and capacity for diagnostics and benchmarks; checksum, ordering, and
index-boundary failures
found during lazy reads are returned to the caller. Full scans validate blocks
as they traverse them. Readers retain
compatibility with the original single-block `STKV0001` format, while new
tables use the indexed `STKV0002` format. Prefix compression remains future
work.

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
- WAL recovery tolerates a torn final record but rejects checksummed data corruption.
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
- WAL replay across reopen, torn-tail recovery, and checksum corruption detection
- SSTable round trips, sorted iteration, key ordering validation, and checksum corruption detection
- Multi-block SSTable boundaries, index corruption, and legacy format compatibility
- Lazy block I/O, cache hits after file removal, and deferred I/O failures
- Shared-cache reuse across readers and cache hit/miss accounting
- Streaming iterator seek, version selection, tombstone hiding, and deferred
  block-error propagation across memtable and SSTables
- Inclusive/exclusive range bounds and prefix filtering across table versions
- High-table-count heap merging with newest-version selection
- Iterator snapshot lifetime across compaction and deferred obsolete-file cleanup
- Memtable flush, SSTable-backed reads, flushed tombstones, and reopen from table files
- Manifest replay, invalid metadata rejection, checksum corruption detection, and missing table handling
- Compaction merging, tombstone handling, obsolete-file cleanup, and reopen from compacted state

Next test layers should add:

- WAL replay limits for very large records and injected I/O failures
- Multi-level compaction correctness with overwritten keys and tombstones
- Fault injection around file creation, rename, and manifest updates

The project starts with a tiny local harness to avoid dependency friction. Once behavior broadens, moving to GoogleTest is reasonable.

## Benchmark Strategy

The current benchmark measures local `Put` performance with automatic
compaction disabled to retain many scan sources, then reopens the database
and runs cold and warm random `Get` passes plus full and bounded streaming scans
against flushed SSTables and the final WAL tail. It reports throughput, latency
percentiles, and shared block-cache hits, misses, evictions, and usage.

Future benchmark tracks:

- sequential write throughput with sync off and sync on
- prefix-scan throughput and selectivity
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

- Add prefix compression and golden encoding tests

### Milestone 3: Flush and Recovery

- Compact manifest snapshots
- Add fault-injection tests around manifest/table installation

### Milestone 4: Iterators and Compaction

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
