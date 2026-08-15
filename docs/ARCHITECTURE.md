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
- `src/manifest.*`: checksummed metadata log and compact snapshots for installed SSTables
- `src/version_set.*`: active table levels, ordering, and overlap invariants
- `src/memtable.*`: sorted mutable map storing latest value or tombstone
- `src/compaction.*`: merge logic for flushed SSTables
- `src/file_system.*`: durability-critical file sync, rename, directory sync,
  and injectable failure boundaries
- `src/db.cc`: coordinates WAL append, optional sync, memtable apply, flush, table loading, and recovery

Current write order is WAL first, memtable second. Synchronous writes flush the
stream and sync the WAL file descriptor before acknowledgement. When the
memtable crosses the configured write buffer size, it is written to a numbered
SSTable and recorded in the manifest. WAL rotation then closes and renames
`current.log` to `previous.log`, syncs the WAL directory, creates and syncs a
new `current.log`, syncs the directory again, and finally removes the retired
generation with a final directory sync.

WAL replay applies every complete record with a valid checksum. Recovery replays
`previous.log` before `current.log` when rotation was interrupted; duplicate
records already present in manifest-installed SSTables are harmless because
newer sources shadow older ones. If a crash leaves a partial final header or
payload, replay stops at the complete prefix. A full record with a checksum
mismatch is treated as corruption. The length prefix is checked against
`Options::max_wal_record_size` before allocating the payload buffer, preventing
damaged or hostile headers from forcing unbounded recovery allocation. Writers
apply the same limit so a database cannot produce records it is configured to
reject later. Descriptor-based WAL append and positional reads are exposed by
the filesystem interface, allowing deterministic propagation tests for I/O
failures as well as sync and rename failures.

### Read Path

Reads follow this order:

1. mutable memtable
2. newest-to-oldest manifest-listed SSTables by key range
3. sorted non-overlapping levels, from lower to higher level numbers

Deletes are tombstones, not immediate removals from history. Compaction decides when a tombstone is safe to drop.
An L0/L1 or L1/L2 compaction retains tombstones whenever a deeper table overlaps
the selected key range. A tombstone is discarded only when no unselected older
level can still contain the deleted key.

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
data block 0: prefix-compressed sorted entries, restart array, checksum
data block 1: prefix-compressed sorted entries, restart array, checksum
...
index block: largest key, block offset, block size, entry count
filter block: version, probe count, bit count, Bloom filter bits
footer: total entry count, index location, index checksum, format magic
```

Within each data block, keys store the byte prefix shared with the previous key
plus an unshared suffix. Every sixteenth entry stores its full key and is listed
in a trailing restart array. Restart offsets and intervals are validated during
decode, bounding reconstruction chains and making malformed offsets explicit
corruption.

The reader loads and verifies only the footer, index, checksummed Bloom filter,
and first data block at open. A negative Bloom result returns an absent lookup
without reading a data block. Possible Bloom false positives continue through
the exact lookup path, so they cannot affect correctness. The filter block
stores its encoding version, probe count, and bit count; new tables allocate
ten bits per key and use seven probes. Point lookups that may be present
binary-search the block index, then binary-search the selected block's restart
keys and reconstruct at most one 16-entry prefix chain. They do not materialize
every entry in the block. Encoded blocks are
retained in a database-wide LRU cache keyed by table path and byte range;
iterators decode a cached block only while traversing it. This keeps cache
charges tied to on-disk bytes and lets point reads reuse cached I/O without
paying full-block key reconstruction. `Options::block_cache_size` is a single
memory budget shared by every open table, preventing memory use from scaling
with the SSTable count. `DB::GetBlockCacheStats` reports hits, misses,
evictions, usage, and capacity for diagnostics and benchmarks; checksum,
restart, ordering, and index-boundary failures found during lazy reads are
returned to the caller. Full scans validate blocks as they traverse them.
Readers retain
compatibility with the original single-block `STKV0001`, uncompressed indexed
`STKV0002`, and prefix-compressed `STKV0003` formats. New tables use the
Bloom-filtered `STKV0004` format.

### Compaction

`src/compaction.*` merges SSTables in oldest-to-newest order. Newly flushed
tables enter overlapping level 0. `VersionSet` counts those tables for the
compaction trigger and validates that every level above level 0 has sorted,
non-overlapping key ranges. When level 0 reaches `level0_compaction_trigger`,
StrataKV selects the oldest trigger-sized level-0 batch, computes its combined
key range, and adds every overlapping level-1 table. Disjoint level-1 files are
left installed. Existing level-1 inputs are merged first, followed by level-0
inputs from oldest to newest, so the newest record deterministically wins.

The merged key stream is partitioned into non-overlapping level-1 outputs using
`Options::max_compaction_output_file_size` as an approximate logical-size
target. Every output is synced and installed before one atomic manifest
snapshot publishes the retained and replacement files together. Only selected
inputs become obsolete, so readers pinned to either selected or retained files
continue to observe a valid snapshot.

`VersionSet` also accounts for the physical bytes in each level. When level 1
exceeds `Options::level1_compaction_trigger_bytes`, its oldest table is selected
with every overlapping level-2 table and rewritten as bounded level-2 outputs.
The compaction loop can cascade an L0/L1 result directly into level 2, keeping
the write path within both configured pressure thresholds.

Tombstones are dropped only when no table below the output level overlaps the
selected range. Otherwise they remain in the output so an unselected older
value cannot reappear. Destination-level inputs are merged before source-level
inputs, preserving newest-value precedence in both L0/L1 and L1/L2 jobs.

### Manifest

`src/manifest.*` stores checksummed table metadata records, including each
table's level. On open, StrataKV replays table-add and table-delete edits into a
validated version set, then replays the WAL for writes that were not flushed.
Legacy table-add records without a level remain readable and are interpreted as
level 0. Directory scans are no longer the source of truth for table membership.

Flushes append and flush a table-add record after installing the SSTable. Full
compaction instead writes the complete active table set to `MANIFEST.tmp`,
syncs and closes it, atomically renames it over `MANIFEST`, and syncs the
database directory. A crash before the rename leaves the old manifest
authoritative; a crash after the directory sync exposes the complete new
snapshot. The writer is then reopened for later flush edits. This bounds
manifest history at each compaction and makes orphaned pre-install files safe
to ignore during recovery. Flush and compaction SSTables follow the same
write/sync/rename/directory-sync installation sequence before their metadata is
published. These boundaries use an injectable filesystem interface so failures
can be tested deterministically.

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
- Level 0 may overlap; every higher level must contain sorted, non-overlapping
  key ranges.

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
    previous.log  # present only across an interrupted rotation
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
- WAL replay across reopen, torn-tail recovery, checksum corruption detection,
  descriptor sync, and interrupted rotation recovery
- SSTable round trips, sorted iteration, key ordering validation, and checksum corruption detection
- Multi-block SSTable boundaries, index corruption, and legacy format compatibility
- Golden prefix-compressed block encoding, restart-array corruption, and
  compression effectiveness for shared key prefixes
- Point lookups across restart boundaries and corruption in searched restart
  keys
- Bloom-filter negative lookups, present-key safety, and filter checksum
  corruption
- Lazy block I/O, cache hits after file removal, and deferred I/O failures
- Shared-cache reuse across readers and cache hit/miss accounting
- Streaming iterator seek, version selection, tombstone hiding, and deferred
  block-error propagation across memtable and SSTables
- Inclusive/exclusive range bounds and prefix filtering across table versions
- High-table-count heap merging with newest-version selection
- Iterator snapshot lifetime across compaction and deferred obsolete-file cleanup
- Memtable flush, SSTable-backed reads, flushed tombstones, and reopen from table files
- Manifest replay, snapshot replacement, post-snapshot appends, invalid metadata rejection, checksum corruption detection, and missing table handling
- Version-set read ordering, level-0 overlap, and sorted-level overlap rejection
- File-sync, rename, and directory-sync ordering plus injected SSTable,
  manifest, and WAL rotation failures
- Compaction merging, tombstone handling, obsolete-file cleanup, and reopen from compacted state
- Bounded level-0 selection, level-1 overlap expansion, size-limited outputs,
  disjoint-file retention, and manifest replay of the resulting version
- Per-level byte accounting, cascading L1/L2 overlap selection, and tombstone
  retention in the presence of deeper overlapping data

Next test layers should add:

- Compaction scoring across multiple over-budget levels
- Fault injection around file creation, rename, and manifest updates

The project starts with a tiny local harness to avoid dependency friction. Once behavior broadens, moving to GoogleTest is reasonable.

## Benchmark Strategy

The current benchmark measures local `Put` performance with automatic L0/L1 and
L1/L2 compaction enabled, then reopens the database
and runs cold and warm random `Get` passes, an absent-key pass, plus full and
bounded streaming scans against flushed SSTables and the final WAL tail. It
reports throughput, latency percentiles, negative-pass block misses, SSTable
bytes, and shared block-cache hits, misses, evictions, and usage.

Future benchmark tracks:

- sequential write throughput with sync off and sync on
- prefix-scan throughput and selectivity
- recovery time by WAL size
- compaction throughput and write amplification
- network request latency after Phase 2

Benchmarks should use fixed seeds, report configuration, and preserve enough metadata to compare runs over time.

## Phased Roadmap

### Milestone 1: Storage Skeleton Hardening

- Add structured compaction statistics and write-amplification accounting
- Add structured logging around open/recovery
- Extend filesystem fault injection beyond WAL operations to SSTable and
  manifest reads and writes

### Milestone 2: SSTable Format

- Add partitioned filters for very large tables if whole-table filters become
  a measurable open-time or memory cost

### Milestone 3: Flush and Recovery

- Add recovery benchmarks by WAL size and record distribution

### Milestone 4: Iterators and Compaction

- Generalize geometric size targets and compaction scoring beyond level 2
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
