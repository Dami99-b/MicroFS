import json
import pathlib
import sys

import matplotlib.pyplot as plt


def main():
    if len(sys.argv) != 3:
        raise SystemExit("usage: plot_benchmarks.py RESULTS.json OUTPUT.png")
    source = pathlib.Path(sys.argv[1])
    target = pathlib.Path(sys.argv[2])
    workloads = json.loads(source.read_text(encoding="utf-8"))["workloads"]
    names = []
    values = []
    for name in sorted({run["workload"] for run in workloads}):
        samples = [
            run["ops_per_second"]
            for run in workloads
            if run["workload"] == name
        ]
        names.append(name)
        values.append(sorted(samples)[len(samples) // 2])
    figure, axis = plt.subplots(figsize=(8, 4))
    axis.bar(names, values)
    axis.set_yscale("log")
    axis.set_ylabel("Operations per second (log scale; median of five runs)")
    axis.set_title("MicroFS Windows/GCC benchmark")
    axis.tick_params(axis="x", rotation=20)
    figure.tight_layout()
    target.parent.mkdir(parents=True, exist_ok=True)
    figure.savefig(target, dpi=160)


if __name__ == "__main__":
    main()
