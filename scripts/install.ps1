<#
.SYNOPSIS
  Installs the test-signed MM2Filter package for the paired Magic Mouse 2.

.DESCRIPTION
  Requires an elevated PowerShell and test-signing mode already ON (see README;
  this script never changes boot configuration). Steps, each confirmed:
    1. Import build\MM2FilterTest.cer into LocalMachine\Root and
       LocalMachine\TrustedPublisher.
    2. pnputil /add-driver MM2Filter.inf /install
    3. Restart the Magic Mouse 2 devnode so the filter loads.

.EXAMPLE
  .\scripts\install.ps1 -Configuration Release
#>
#Requires -RunAsAdministrator
[CmdletBinding(SupportsShouldProcess, ConfirmImpact = 'High')]
param(
    [ValidateSet('Debug', 'Release')] [string] $Configuration = 'Release'
)
$ErrorActionPreference = 'Stop'

$root    = Split-Path -Parent $PSScriptRoot
$inf     = Join-Path $root "build\x64\$Configuration\MM2Filter\MM2Filter.inf"
$cerPath = Join-Path $root 'build\MM2FilterTest.cer'
$hwid    = 'BTHENUM\{00001124-0000-1000-8000-00805f9b34fb}_VID&0001004c_PID&0269'

if (-not (Test-Path $inf))     { throw "Package not found: $inf (build, then run testsign.ps1)" }
if (-not (Test-Path $cerPath)) { throw "Certificate not found: $cerPath (run testsign.ps1)" }

$bcd = bcdedit /enum '{current}' | Out-String
if ($bcd -notmatch 'testsigning\s+Yes') {
    throw 'Test-signing mode is OFF. Enable it yourself (README, "Test-signing setup"), reboot, then rerun.'
}

if ($PSCmdlet.ShouldProcess('LocalMachine\Root + TrustedPublisher', "Trust test certificate $cerPath")) {
    Import-Certificate -FilePath $cerPath -CertStoreLocation Cert:\LocalMachine\Root | Out-Null
    Import-Certificate -FilePath $cerPath -CertStoreLocation Cert:\LocalMachine\TrustedPublisher | Out-Null
}

if ($PSCmdlet.ShouldProcess($inf, 'pnputil /add-driver /install')) {
    pnputil /add-driver $inf /install
    # 0 = installed, 3010 = installed (reboot advised), 259 = this exact package is already installed
    if ($LASTEXITCODE -and $LASTEXITCODE -notin 3010, 259) { throw "pnputil /add-driver failed ($LASTEXITCODE)" }
}

$dev = Get-PnpDevice -PresentOnly | Where-Object { $_.HardwareID -contains $hwid }
foreach ($d in $dev) {
    if ($PSCmdlet.ShouldProcess($d.InstanceId, 'Restart device')) {
        pnputil /restart-device "$($d.InstanceId)"
    }
    $filters = (Get-PnpDeviceProperty -InstanceId $d.InstanceId -KeyName DEVPKEY_Device_LowerFilters -ErrorAction SilentlyContinue).Data
    Write-Host "LowerFilters on $($d.InstanceId): $($filters -join ', ')"
}
if (-not $dev) { Write-Warning 'Magic Mouse 2 is not connected; the filter loads the next time it connects.' }

Write-Host "`nNext: run build\x64\$Configuration\rawtouch.exe and touch the mouse."
