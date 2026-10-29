param(
  [ValidateSet('build', 'test', 'cli', 'bench', 'clean')]
  [string]$Action = 'test'
)

$ErrorActionPreference = 'Stop'
$buildDirectory = Join-Path $PSScriptRoot '..\build'
$testExecutable = Join-Path $buildDirectory 'microfs_core_tests.exe'
$cliExecutable = Join-Path $buildDirectory 'microfs_cli.exe'
$benchExecutable = Join-Path $buildDirectory 'microfs_bench.exe'

if ($Action -eq 'clean') {
  foreach ($target in @($testExecutable, $cliExecutable, $benchExecutable)) {
    if (Test-Path $target) {
      Remove-Item -LiteralPath $target -Force
    }
  }
  exit 0
}

$compiler = Get-Command g++ -ErrorAction SilentlyContinue
if ($null -eq $compiler) {
  throw 'g++ was not found on PATH; install a C++20 GCC toolchain.'
}

New-Item -ItemType Directory -Force -Path $buildDirectory | Out-Null
$env:PATH = (Split-Path $compiler.Source) + ';' + $env:PATH
$sources = @(
  (Join-Path $PSScriptRoot '..\src\block.cc')
  (Join-Path $PSScriptRoot '..\src\block_cache.cc')
  (Join-Path $PSScriptRoot '..\src\bloom_filter.cc')
  (Join-Path $PSScriptRoot '..\src\db.cc')
  (Join-Path $PSScriptRoot '..\src\env.cc')
  (Join-Path $PSScriptRoot '..\src\table.cc')
  (Join-Path $PSScriptRoot '..\src\tick_store.cc')
  (Join-Path $PSScriptRoot '..\src\version_set.cc')
  (Join-Path $PSScriptRoot '..\src\wal.cc')
)

if ($Action -eq 'cli' -or $Action -eq 'bench') {
  $toolExecutable = $cliExecutable
  $toolSource = Join-Path $PSScriptRoot '..\tools\microfs_cli.cc'
  if ($Action -eq 'bench') {
    $toolExecutable = $benchExecutable
    $toolSource = Join-Path $PSScriptRoot '..\bench\microfs_bench.cc'
  }
  if (Test-Path $toolExecutable) {
    Remove-Item -LiteralPath $toolExecutable -Force
  }
  & $compiler.Source -std=c++20 -O2 -Wall -Wextra -Wpedantic -Werror `
    -I (Join-Path $PSScriptRoot '..\include') `
    $sources $toolSource -o $toolExecutable
  exit $LASTEXITCODE
}

if (Test-Path $testExecutable) {
  Remove-Item -LiteralPath $testExecutable -Force
}
& $compiler.Source -std=c++20 -O2 -Wall -Wextra -Wpedantic -Werror `
  -I (Join-Path $PSScriptRoot '..\include') `
  $sources (Join-Path $PSScriptRoot '..\tests\core_test.cc') `
  -o $testExecutable
if ($LASTEXITCODE -ne 0) {
  exit $LASTEXITCODE
}

if ($Action -eq 'test') {
  & $testExecutable
  exit $LASTEXITCODE
}
