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
