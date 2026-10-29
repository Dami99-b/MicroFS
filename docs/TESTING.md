# Testing

The self-contained test executable is registered with CTest when built via
CMake. On Windows with the currently available toolchain, run
`.\scripts\build.ps1 test` from the repository root to compile with GCC and
execute the available test suite. `.\scripts\build.ps1 build` compiles without
running tests.

Sanitizers, CMake/Ninja builds, POSIX builds, and compiler-matrix results are
not claimed until they have been run in a suitable environment.
