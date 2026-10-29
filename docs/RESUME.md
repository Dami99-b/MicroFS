# Resume material

## Project summary

On Windows 11 Pro with MSYS2 GCC 16.2.0, a fixed-seed MicroFS benchmark
measured a median of 1855.50809619 random `Put` operations per second and
293887.723134 random hit `Get` operations per second over five measured
10,000-operation repetitions; WAL sync was disabled and all timed data was
served from the memtable. The raw samples and environment are in
[`bench/results/windows-gcc-2026-10-29`](../bench/results/windows-gcc-2026-10-29).

## Resume bullets

- Implemented the measured `fillrandom` workload at a median
  **1855.50809619 ops/s**; five raw repetitions are recorded in
  [`results.json`](../bench/results/windows-gcc-2026-10-29/results.json).
- Measured random hit reads at a median **293887.723134 ops/s** over five
  repetitions; see the `readrandom-hit` samples in
  [`results.json`](../bench/results/windows-gcc-2026-10-29/results.json).
- Recorded a median `fillrandom` p50 latency of **359423 ns** across the
  five 10,000-operation runs; evidence:
  [`results.json`](../bench/results/windows-gcc-2026-10-29/results.json).
- Recorded a median `readrandom-hit` p99 latency of **7215 ns** across the
  same five repetitions; evidence:
  [`results.json`](../bench/results/windows-gcc-2026-10-29/results.json).
- Repeated each measured workload **5 times** after a warmup, with **10,000
  operations per repetition**, and captured compiler, flags, CPU, OS, and
  memory in [`environment.json`](../bench/results/windows-gcc-2026-10-29/environment.json).

These are narrowly scoped, local numbers with WAL syncing disabled; they are
not durable throughput claims or a comparison against another database.

## Interview questions and honest answers

1. **Why use a WAL?** To recover writes that were appended before in-memory
   state was published. The current benchmark disables sync, so its write
   results do not demonstrate crash durability.
2. **How do snapshots work?** They capture a sequence number; Get and iterator
   selection choose the newest internal-key version at or below it.
3. **How are internal keys ordered?** Escaped user bytes sort first, then an
   inverted big-endian sequence tag puts newer versions before older ones.
4. **How are torn WAL records handled?** CRC32C and fragment framing stop
   recovery at the first invalid logical suffix, which is truncated.
5. **How is a tick batch compressed?** A record stores count, varint size,
   zig-zag price and timestamp deltas; the first tick establishes absolute
   timestamp/price values.
6. **What is the largest current scaling limitation?** Iterators materialize
   the complete visible keyspace, and compaction merges all installed tables
   rather than selecting bounded ranges.
7. **Does SSTable reading use mmap?** No. The current implementation reads
   each entire table through Env; mmap and pread are not implemented.
8. **What compaction does the current DB implement?** It flushes L0 files and
   merges installed tables into one L1 output. It is not a production leveled
   scheduler.
9. **What does the benchmark establish?** Only a five-repeat, single-threaded
   memtable write/read baseline on the recorded Windows/GCC host; it says
   nothing about durable writes or disk-read amplification.
10. **Which validation remains outstanding?** The million-operation oracle,
    broad randomized crash matrix, sanitizers, POSIX/Clang builds, expanded
    workload sweeps, and profiler-guided tuning.
