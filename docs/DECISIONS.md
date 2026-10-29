# Design decisions

## Platform and scope

The implementation is being developed and verified on Windows 10 with MSYS2
GCC 16.2.0. CMake, Ninja, MSVC, Clang, and a configured Rust toolchain are not
available in this environment. The initial storage environment uses the C++
standard filesystem and C runtime, with a platform-conditional durable-sync
implementation (`_commit` on Windows and `fsync` on POSIX). POSIX behavior is
not claimed as verified on this host. Builds here use the provided Makefile and
GCC; CMake project files are included for environments where CMake is
available.

The requested design is substantially larger than can be validated as a
production storage engine in one pass. Each phase is committed separately and
implemented behavior is described without claiming unimplemented features or
unmeasured performance.

## Phase 1

- Status is represented as an explicit code and message; StatusOr is used for
  value-or-error results.
- The default Env uses portable C++ filesystem operations and platform C
  runtime calls for file sync. A future native mmap/pread path is outside this
  foundational phase.
- FaultInjectingEnv stages file contents until SyncFile, allowing deterministic
  unsynced-write loss, write tearing, and injected I/O failures.
- CRC32C currently uses a compile-time generated software table. Hardware
  dispatch will only be added if it can be safely verified on the build host.
- Histogram quantiles use logarithmic buckets with 256 sub-buckets per power
  of two; results are upper bounds, not exact order statistics.

## Phase 2

- The skiplist has a fixed maximum height of 12, deterministic writer-only
  xorshift height selection, and immutable arena-copied key/value bytes.
- The memtable initially used the phase-2 little-endian internal-key layout.
  Phase 4 replaces it with an escaped user key terminator and a big-endian
  inverted sequence/type tag, allowing bytewise ordering in SSTables to match
  user-key ascending and sequence descending even with binary keys.
- WAL checksums cover the record-type byte and payload. Recovery stops at the
  first invalid physical record, retains only complete logical records, and
  synchronously truncates the invalid suffix.
- Group commit combines concurrently queued records into one append and one
  required sync per processing batch. Batching depends on scheduling and is
  not guaranteed for every concurrent run.

## Phase 3

- Data, index, and filter blocks use the same restart-compressed block encoding
  and each block is followed by a little-endian CRC32C trailer.
- The table footer is a fixed 40-byte record containing a magic value and the
  index and filter block handles. Bloom filters use MurmurHash3 double hashing.
- Prefix filters are optional and index the first configured number of key
  bytes; callers must use the same key-prefix convention as the table writer.
- The table reader currently loads the whole table through Env rather than
  using mmap or pread. This portable implementation is not represented as a
  zero-copy or memory-mapped reader.

## Phase 4

- The first DB version keeps one WAL for the lifetime of the database and
  records complete version edits in a checksummed manifest. On recovery, WAL
  operations at or below the manifest's durable sequence are skipped.
- Iterators materialize the visible key/value set at creation. They provide
  stable snapshot traversal but use memory proportional to the visible data.
- Compaction currently merges all installed tables into one level-1 table
  rather than selecting bounded level ranges. It drops obsolete versions
  only after considering the oldest active snapshot; it is not a production
  leveled-compaction scheduler.
- The database oracle test currently runs 10,000 randomized API operations.
  The one-million-operation target, randomized crash-point matrix, and
  concurrent snapshot/compaction stress remain outstanding.

## Phase 5

- TickStore keys reserve a five-byte namespace, followed by a zero-escaped
  symbol and big-endian nanosecond timestamp. This keeps arbitrary user keys
  in the same DB from being mistaken for tick records, while retaining
  bytewise symbol/time order within the namespace.
- Each AppendBatch stores one record: the key gives its first timestamp and
  the value contains a varint count, zig-zag fixed-point price, varint size,
  then timestamp and price deltas. Appends for a symbol must be strictly
  increasing; out-of-order writes are rejected rather than rewriting earlier
  batches.
- Fuzz targets and CI workflows are provided for supported Clang/Linux
  environments, but cannot be run with the Windows GCC toolchain available in
  this session. Native sanitizer and POSIX results remain unverified.
