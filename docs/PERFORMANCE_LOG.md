# Performance log

## Baseline

The reproducible baseline is recorded in
[`docs/BENCHMARKS.md`](BENCHMARKS.md) and its raw JSON/CSV files. It contains
only single-threaded random writes with WAL sync disabled and random hit reads
from the in-memory memtable. No profiling-driven optimization has been made.

## Profiling status

`perf` is Linux-specific and unavailable on the Windows host used for this
work. No Windows profiler was available or run. Consequently, this log has no
claimed hotspot, optimization, or before/after result. Profiling and the wider
configuration sweeps remain open work.
