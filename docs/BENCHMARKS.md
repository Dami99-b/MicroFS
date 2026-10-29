# Benchmarks

The current benchmark is a deliberately narrow, reproducible smoke baseline:
`fillrandom` (single-threaded `Put`) and `readrandom-hit` (single-threaded
`Get`) against the same key set. It uses a fixed seed, one warmup, five
measured repetitions, and a logarithmic latency histogram. It writes JSON and
CSV raw samples under `bench/results`; machine and compiler metadata are stored
in `environment.json`.

The initial Windows run uses 10,000 operations per workload. The exact
measurements, including every repetition and latency percentile, are in
[`bench/results/windows-gcc-2026-10-29/results.json`](../bench/results/windows-gcc-2026-10-29/results.json)
and the matching CSV. Environment details are in the adjacent
[`environment.json`](../bench/results/windows-gcc-2026-10-29/environment.json).

This is not a storage-engine comparison or a durability result. The benchmark
sets WAL sync policy to `never`, keeps the measured key set in the memtable,
and does not time the final close/flush. Its throughput must not be presented
as durable write throughput or as a general performance claim. The wider
workload matrix, experiments, profiling, and before/after optimization study
remain unimplemented. No LevelDB/RocksDB comparison was available or run.

Reproduce on this Windows GCC host with:

```powershell
.\scripts\run_bench.ps1 -Operations 10000
python scripts\plot_benchmarks.py bench\results\windows-gcc-2026-10-29\results.json docs\img\microfs-bench.png
```

The chart generator uses only matplotlib. Chart generation was not verified
on this host because matplotlib is not installed. Do not treat repeated runs
on another host or build as directly comparable without recording its
environment and commit.
