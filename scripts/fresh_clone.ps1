<#
.SYNOPSIS
  Fresh-clone closure: prove that the committed sources reproduce the release.

.DESCRIPTION
  Clones the repository (from its own committed history, or from the configured
  remote when -Remote is given) into an empty directory outside the working tree,
  builds it, runs every suite, installs it, and builds and runs the independent
  consumer against that install. No step uses a timeout.

  Run this AFTER committing: it proves the commit, not the working tree.
#>
param(
  [string] $Repository = (Resolve-Path (Join-Path $PSScriptRoot "..")).Path,
  [string] $CloneRoot = (Join-Path ([System.IO.Path]::GetTempPath()) "fcfn-fresh-clone"),
  [string] $Remote = "",
  [string] $Revision = "HEAD"
)

$ErrorActionPreference = "Stop"

function Invoke-InVcVars {
  param([string[]] $Arguments, [string] $WorkingDirectory)
  $programFilesX86 = [Environment]::GetEnvironmentVariable("ProgramFiles(x86)")
  $vcvars = Join-Path $programFilesX86 "Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat"
  $quoted = ($Arguments | ForEach-Object { if ($_ -match '\s') { '"' + $_ + '"' } else { $_ } }) -join ' '
  $line = 'cd /d "' + $WorkingDirectory + '" && call "' + $vcvars + '" >nul 2>&1 && ' + $quoted
  & cmd.exe /c $line | Out-Host
  if ($LASTEXITCODE -ne 0) {
    throw "command failed with exit code $LASTEXITCODE"
  }
}

if (Test-Path $CloneRoot) { Remove-Item -Recurse -Force $CloneRoot }
New-Item -ItemType Directory -Force -Path $CloneRoot | Out-Null
$checkout = Join-Path $CloneRoot "checkout"

if ($Remote -ne "") {
  git clone --quiet $Remote $checkout
  if ($LASTEXITCODE -ne 0) { throw "clone from remote failed" }
} else {
  git clone --quiet $Repository $checkout
  if ($LASTEXITCODE -ne 0) { throw "local clone failed" }
}
Push-Location $checkout
try {
  git checkout --quiet $Revision
  if ($LASTEXITCODE -ne 0) { throw "checkout of $Revision failed" }
  $commit = (git rev-parse HEAD).Trim()
  $dirty = (git status --porcelain)
  if ($dirty) { throw "fresh clone is not clean" }
  Write-Host "fresh clone commit: $commit"

  $build = Join-Path $checkout "build"
  Invoke-InVcVars -WorkingDirectory $checkout -Arguments @("cmake", "-S", ".", "-B", "build", "-G", "Ninja", "-DCMAKE_BUILD_TYPE=Release", "-DFCFN_BUILD_TESTS=ON", "-DFCFN_BUILD_APPS=ON")
  Invoke-InVcVars -WorkingDirectory $checkout -Arguments @("cmake", "--build", "build")
  Invoke-InVcVars -WorkingDirectory $checkout -Arguments @("ctest", "--test-dir", "build", "--output-on-failure")

  $prefix = Join-Path $CloneRoot "install"
  Invoke-InVcVars -WorkingDirectory $checkout -Arguments @("cmake", "--install", "build", "--prefix", $prefix)

  $consumer = Join-Path $CloneRoot "consumer"
  Copy-Item -Recurse (Join-Path $checkout "examples\downstream_consumer") $consumer
  Invoke-InVcVars -WorkingDirectory $consumer -Arguments @("cmake", "-S", ".", "-B", "build", "-G", "Ninja", "-DCMAKE_BUILD_TYPE=Release", "-DCMAKE_PREFIX_PATH=$prefix")
  Invoke-InVcVars -WorkingDirectory $consumer -Arguments @("cmake", "--build", "build")
  & (Join-Path $consumer "build\fcfn_consumer.exe")
  if ($LASTEXITCODE -ne 0) { throw "consumer failed" }
  Write-Host "fresh clone closure PASSED for commit $commit"
} finally {
  Pop-Location
}
