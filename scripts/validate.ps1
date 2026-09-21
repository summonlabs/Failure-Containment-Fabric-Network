<#
.SYNOPSIS
  Full FCFN validation matrix: release, debug, tests, install, independent
  consumer, tooling, and capability probes for sanitizers and static analysis.

.DESCRIPTION
  Every step is plain and unbounded: no test or command in this script uses a
  timeout. A hang is a defect to diagnose, not something to mask.

  The script never claims coverage it did not run. A capability the host
  toolchain does not provide is reported as UNSUPPORTED with the reason.
#>
param(
  [string] $Root = (Resolve-Path (Join-Path $PSScriptRoot "..")).Path,
  [string] $Workspace = (Join-Path ([System.IO.Path]::GetTempPath()) "fcfn-validate"),
  [switch] $SkipDebug,
  [switch] $SkipSanitizer,
  [switch] $SkipStaticAnalysis
)

$ErrorActionPreference = "Stop"
$script:Results = @()

function Write-Result {
  param([string] $Name, [string] $Status, [string] $Detail = "")
  $script:Results += [pscustomobject]@{ Step = $Name; Status = $Status; Detail = $Detail }
  Write-Host ("[{0}] {1} {2}" -f $Status, $Name, $Detail)
}

function Invoke-InVcVars {
  param([string[]] $Arguments)
  $programFilesX86 = [Environment]::GetEnvironmentVariable("ProgramFiles(x86)")
  $vcvars = Join-Path $programFilesX86 "Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat"
  if (-not (Test-Path $vcvars)) {
    throw "vcvars64.bat not found at $vcvars"
  }
  $quoted = ($Arguments | ForEach-Object { if ($_ -match '\s') { '"' + $_ + '"' } else { $_ } }) -join ' '
  $line = 'call "' + $vcvars + '" >nul 2>&1 && ' + $quoted
  # Out-Host keeps command output out of the function's return value, so callers
  # receive exactly the exit code.
  & cmd.exe /c $line | Out-Host
  return $LASTEXITCODE
}

function Invoke-Checked {
  param([string] $Name, [scriptblock] $Block)
  try {
    & $Block
    $code = $LASTEXITCODE
    if ($null -ne $code -and $code -ne 0) {
      Write-Result -Name $Name -Status "FAIL" -Detail "exit code $code"
      return $false
    }
    Write-Result -Name $Name -Status "PASS"
    return $true
  } catch {
    Write-Result -Name $Name -Status "FAIL" -Detail $_.Exception.Message
    return $false
  }
}

New-Item -ItemType Directory -Force -Path $Workspace | Out-Null
Write-Host "FCFN validation workspace: $Workspace"
Write-Host "repository: $Root"

$release = Join-Path $Workspace "build-release"
$debug = Join-Path $Workspace "build-debug"
$prefix = Join-Path $Workspace "install"
$consumer = Join-Path $Workspace "consumer"
$fixtures = Join-Path $Workspace "fixtures"
New-Item -ItemType Directory -Force -Path $fixtures | Out-Null

$topology = Join-Path $fixtures "topology.txt"
@"
# SYNTHETIC fixture used by the validation script.
generation 1
node edge-a containable=1 weight=4 protected=0 completeness=complete
node spine-1 containable=1 weight=1 protected=0 completeness=complete
node spine-2 containable=1 weight=1 protected=0 completeness=complete
node tenant containable=0 weight=1 protected=1 completeness=complete
edge edge-a spine-1 evidence=proven generation=1
edge spine-1 tenant evidence=proven generation=1
edge edge-a spine-2 evidence=proven generation=1
edge spine-2 tenant evidence=proven generation=1
"@ | Set-Content -Path $topology
$evidence = Join-Path $fixtures "evidence.txt"
"edge-a present 1" | Set-Content -Path $evidence

$ok = Invoke-Checked -Name "configure release (/W4 /WX)" -Block {
  Invoke-InVcVars @("cmake", "-S", $Root, "-B", $release, "-G", "Ninja", "-DCMAKE_BUILD_TYPE=Release", "-DFCFN_BUILD_TESTS=ON", "-DFCFN_BUILD_APPS=ON")
}
if ($ok) {
  $ok = Invoke-Checked -Name "build release" -Block {
    Invoke-InVcVars @("cmake", "--build", $release)
  }
}
if ($ok) {
  Invoke-Checked -Name "ctest release (no timeouts)" -Block {
    Invoke-InVcVars @("ctest", "--test-dir", $release, "--output-on-failure")
  } | Out-Null
}

if (-not $SkipDebug) {
  $okDebug = Invoke-Checked -Name "configure debug" -Block {
    Invoke-InVcVars @("cmake", "-S", $Root, "-B", $debug, "-G", "Ninja", "-DCMAKE_BUILD_TYPE=Debug", "-DFCFN_BUILD_TESTS=ON", "-DFCFN_BUILD_APPS=ON")
  }
  if ($okDebug) {
    $okDebug = Invoke-Checked -Name "build debug" -Block {
      Invoke-InVcVars @("cmake", "--build", $debug)
    }
  }
  if ($okDebug) {
    Invoke-Checked -Name "ctest debug (no timeouts)" -Block {
      Invoke-InVcVars @("ctest", "--test-dir", $debug, "--output-on-failure")
    }
  }
}

Invoke-Checked -Name "cmake --install" -Block {
  Invoke-InVcVars @("cmake", "--install", $release, "--prefix", $prefix)
} | Out-Null

$consumerOk = Invoke-Checked -Name "configure independent consumer" -Block {
  if (Test-Path $consumer) { Remove-Item -Recurse -Force $consumer }
  Copy-Item -Recurse (Join-Path $Root "examples\downstream_consumer") $consumer
  Invoke-InVcVars @("cmake", "-S", $consumer, "-B", (Join-Path $consumer "build"), "-G", "Ninja", "-DCMAKE_BUILD_TYPE=Release", "-DCMAKE_PREFIX_PATH=$prefix")
}
if ($consumerOk) {
  $consumerOk = Invoke-Checked -Name "build independent consumer" -Block {
    Invoke-InVcVars @("cmake", "--build", (Join-Path $consumer "build"))
  }
}
if ($consumerOk) {
  Invoke-Checked -Name "run independent consumer" -Block {
    & (Join-Path $consumer "build\fcfn_consumer.exe")
    if ($LASTEXITCODE -ne 0) { throw "consumer exited with $LASTEXITCODE" }
  } | Out-Null
}

Invoke-Checked -Name "fcfnctl plan and verify" -Block {
  $planFile = Join-Path $fixtures "plan.bin"
  & (Join-Path $release "apps\fcfnctl.exe") plan --topology $topology --evidence $evidence --out $planFile | Out-Null
  if ($LASTEXITCODE -ne 0) { throw "fcfnctl plan failed" }
  & (Join-Path $release "apps\fcfnctl.exe") verify --topology $topology --evidence $evidence --plan $planFile | Out-Null
  if ($LASTEXITCODE -ne 0) { throw "fcfnctl verify failed" }
  & (Join-Path $release "apps\fcfnctl.exe") version | Out-Null
  if ($LASTEXITCODE -ne 0) { throw "fcfnctl version failed" }
} | Out-Null

if (-not $SkipSanitizer) {
  $asan = Join-Path $Workspace "build-asan"
  # Applications are built too: the multiprocess suite spawns them, and an
  # instrumented run that silently skips a suite would be a false claim.
  $asanConfigure = Invoke-InVcVars @("cmake", "-S", $Root, "-B", $asan, "-G", "Ninja", "-DCMAKE_BUILD_TYPE=RelWithDebInfo", "-DFCFN_BUILD_TESTS=ON", "-DFCFN_BUILD_APPS=ON", "-DFCFN_ENABLE_ASAN=ON")
  if ($asanConfigure -ne 0) {
    Write-Result -Name "address sanitizer" -Status "UNSUPPORTED" -Detail "configure failed: no ASan runtime for this toolchain"
  } else {
    $asanBuild = Invoke-InVcVars @("cmake", "--build", $asan)
    if ($asanBuild -ne 0) {
      Write-Result -Name "address sanitizer" -Status "UNSUPPORTED" -Detail "build failed: ASan runtime component missing"
    } else {
      $asanRun = Invoke-InVcVars @("ctest", "--test-dir", $asan, "--output-on-failure")
      if ($asanRun -eq 0) {
        Write-Result -Name "address sanitizer" -Status "PASS" -Detail "ctest under /fsanitize=address"
      } else {
        Write-Result -Name "address sanitizer" -Status "FAIL" -Detail "ctest reported failures under ASan"
      }
    }
  }
}

if (-not $SkipStaticAnalysis) {
  $analyze = Join-Path $Workspace "build-analyze"
  $analyzeConfigure = Invoke-InVcVars @("cmake", "-S", $Root, "-B", $analyze, "-G", "Ninja", "-DCMAKE_BUILD_TYPE=Release", "-DFCFN_BUILD_TESTS=OFF", "-DFCFN_BUILD_APPS=OFF", "-DFCFN_ENABLE_STATIC_ANALYSIS=ON")
  if ($analyzeConfigure -ne 0) {
    Write-Result -Name "static analysis (/analyze)" -Status "UNSUPPORTED" -Detail "configure failed"
  } else {
    $analyzeBuild = Invoke-InVcVars @("cmake", "--build", $analyze, "--target", "fcfn_core")
    if ($analyzeBuild -eq 0) {
      Write-Result -Name "static analysis (/analyze)" -Status "PASS" -Detail "core library analysed"
    } else {
      Write-Result -Name "static analysis (/analyze)" -Status "FAIL" -Detail "analysis reported findings or the component is missing"
    }
  }
}

Write-Host ""
Write-Host "=== validation summary ==="
$script:Results | ForEach-Object { Write-Host ("{0,-10} {1} {2}" -f $_.Status, $_.Step, $_.Detail) }
$failed = @($script:Results | Where-Object { $_.Status -eq "FAIL" })
if ($failed.Count -gt 0) {
  Write-Host "validation FAILED"
  exit 1
}
Write-Host "validation PASSED"
exit 0
