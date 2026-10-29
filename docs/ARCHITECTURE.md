# Architecture

## Write path

`DB::Write` serializes calls through a leader/queue. A group is assigned
consecutive sequence numbers, encoded as one checksummed WAL logical record,
appended, and then applied to the mutable memtable. The memtable is a
writer-serialized skiplist with acquire/release publication for readers.
When its size threshold is reached, it becomes immutable and a new mutable
memtable is installed. The background worker writes the immutable contents to
an L0 SSTable and logs the file metadata in MANIFEST.

## Read path

`DB::Get` captures a snapshot sequence, checks mutable and immutable
memtables, and seeks each installed table for the first internal key at or
after the requested user-key/sequence boundary. The newest visible value wins;
a visible tombstone returns NotFound. Table filters are used to reject
definitely absent keys. A `DBIterator` materializes all visible entries at
creation, so it remains stable but has O(number of visible keys) memory cost.

## Versions and background work

CURRENT names the active WAL-framed MANIFEST. Manifest edits track the sequence,
next file number, and per-level table ranges. L0 files may overlap; non-L0 file
ranges are checked for overlap. A single worker flushes immutable memtables
and compacts when requested or when the L0 trigger is reached. Compaction
retains versions newer than the oldest active snapshot and one base version at
or below it. The current scheduler merges all installed tables into one L1
table; it does not yet implement bounded multi-level selection or size tiers.

## TickStore

TickStore uses a reserved DB-key namespace, an escaped symbol, then a
big-endian timestamp. Each `AppendBatch` is one DB record. Its value stores a
count and varint/zig-zag price, size, and timestamp deltas. Appends to each
symbol must be strictly increasing. Range and last-tick queries currently
materialize DB iterators and decode stored batches; the DB-level materialized
iterator limits large-dataset scalability.
