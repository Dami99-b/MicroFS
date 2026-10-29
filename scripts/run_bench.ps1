param(
  [int]$Operations = 10000,
  [string]$OutputDirectory = 'bench\results\windows-gcc-2026-10-29'
)

$ErrorActionPreference = 'Stop'
& (Join-Path $PSScriptRoot 'build.ps1') bench
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
$repo = Split-Path $PSScriptRoot -Parent
$output = Join-Path $repo $OutputDirectory
New-Item -ItemType Directory -Force -Path $output | Out-Null
$env:PATH = (Split-Path (Get-Command g++).Source) + ';' + $env:PATH
& (Join-Path $repo 'build\microfs_bench.exe') $Operations $output
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
$cpu = (Get-CimInstance Win32_Processor | Select-Object -First 1 -ExpandProperty Name)
$computer = Get-CimInstance Win32_ComputerSystem
$os = Get-CimInstance Win32_OperatingSystem
$compiler = (g++ --version | Select-Object -First 1)
$commit = git -C $repo rev-parse HEAD
$environment = [ordered]@{
  cpu = $cpu
  logical_processors = [Environment]::ProcessorCount
  physical_memory_bytes = [uint64]$computer.TotalPhysicalMemory
  os = $os.Caption
  os_version = $os.Version
  compiler = $compiler
  flags = '-std=c++20 -O2 -Wall -Wextra -Wpedantic -Werror'
  storage = 'Workspace volume; physical media type not queried'
  git_commit = $commit
}
$environment | ConvertTo-Json | Set-Content -LiteralPath (Join-Path $output 'environment.json') -Encoding utf8
