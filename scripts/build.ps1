param(
  [ValidateSet('build', 'test', 'clean')]
  [string]$Action = 'test'
)

$ErrorActionPreference = 'Stop'
$buildDirectory = Join-Path $PSScriptRoot '..\build'
$executable = Join-Path $buildDirectory 'microfs_core_tests.exe'

if ($Action -eq 'clean') {
  if (Test-Path $executable) {
    Remove-Item -LiteralPath $executable -Force
  }
  exit 0
}

$compiler = Get-Command g++ -ErrorAction SilentlyContinue
if ($null -eq $compiler) {
  throw 'g++ was not found on PATH; install a C++20 GCC toolchain.'
}

New-Item -ItemType Directory -Force -Path $buildDirectory | Out-Null
if (Test-Path $executable) {
  Remove-Item -LiteralPath $executable -Force
}
$env:PATH = (Split-Path $compiler.Source) + ';' + $env:PATH
& $compiler.Source -std=c++20 -O2 -Wall -Wextra -Wpedantic `
  -I (Join-Path $PSScriptRoot '..\include') `
  (Join-Path $PSScriptRoot '..\src\block.cc') `
  (Join-Path $PSScriptRoot '..\src\block_cache.cc') `
  (Join-Path $PSScriptRoot '..\src\bloom_filter.cc') `
  (Join-Path $PSScriptRoot '..\src\env.cc') `
  (Join-Path $PSScriptRoot '..\src\table.cc') `
  (Join-Path $PSScriptRoot '..\src\wal.cc') `
  (Join-Path $PSScriptRoot '..\tests\core_test.cc') `
  -o $executable
if ($LASTEXITCODE -ne 0) {
  exit $LASTEXITCODE
}

if ($Action -eq 'test') {
  & $executable
  exit $LASTEXITCODE
}
