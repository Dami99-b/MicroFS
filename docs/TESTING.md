# Testing

The self-contained test executable is registered with CTest when built via
CMake. On Windows with the currently available toolchain, run
`.\scripts\build.ps1 test` from the repository root to compile with GCC and
execute the available test suite. `.\scripts\build.ps1 build` compiles without
running tests; `.\scripts\build.ps1 cli` builds the command-line tool.
`.\scripts\build.ps1 bench` builds the benchmark executable and
`.\scripts\run_bench.ps1 -Operations 10000` runs five measured repetitions
after a warmup.
`.\build\microfs_cli.exe <db-directory> put KEY VALUE`, `get KEY`, `del KEY`,
`scan`, `stats`, `tick SYMBOL NS PRICE SIZE`, `ticks SYMBOL FROM TO`, and
`last SYMBOL` exercise the public engine and TickStore APIs.

The CI workflow includes GCC/Clang Debug/Release jobs, sanitizer jobs, and
short libFuzzer runs. Fuzz targets require Clang and are enabled with
`-DMICROFS_BUILD_FUZZERS=ON`; their input corpora live under `fuzz/corpus`.

Sanitizers, CMake/Ninja builds, POSIX builds, and compiler-matrix results are
not claimed until they have been run in a suitable environment.
