<#
.SYNOPSIS
  Removes MM2Filter from the driver store and the Magic Mouse 2 devnode.

.DESCRIPTION
  Requires an elevated PowerShell. Finds the oemNN.inf that pnputil assigned
  to MM2Filter.inf, deletes it with /uninstall (devices fall back to plain
  hidbth), restarts the mouse, and optionally removes the test certificate.
  Does not change boot configuration; turn test-signing off yourself.
#>
#Requires -RunAsAdministrator
[CmdletBinding(SupportsShouldProcess, ConfirmImpact = 'High')]
param(
    [switch] $RemoveCertificate
)
$ErrorActionPreference = 'Stop'
$hwid = 'BTHENUM\{00001124-0000-1000-8000-00805f9b34fb}_VID&0001004c_PID&0269'

$published = (pnputil /enum-drivers | Out-String) -split '(\r?\n){2,}' |
    Where-Object { $_ -match 'Original Name:\s+mm2filter\.inf' } |
    ForEach-Object { if ($_ -match 'Published Name:\s+(oem\d+\.inf)') { $Matches[1] } }

if (-not $published) { Write-Host 'MM2Filter is not in the driver store.' }
foreach ($oem in $published) {
    if ($PSCmdlet.ShouldProcess($oem, 'pnputil /delete-driver /uninstall')) {
        pnputil /delete-driver $oem /uninstall /force
    }
}

foreach ($d in Get-PnpDevice -PresentOnly | Where-Object { $_.HardwareID -contains $hwid }) {
    if ($PSCmdlet.ShouldProcess($d.InstanceId, 'Restart device')) {
        pnputil /restart-device "$($d.InstanceId)"
    }
}

if ($RemoveCertificate) {
    foreach ($store in 'Root', 'TrustedPublisher') {
        Get-ChildItem "Cert:\LocalMachine\$store" | Where-Object Subject -eq 'CN=MM2Filter Test Signing' |
            ForEach-Object {
                if ($PSCmdlet.ShouldProcess("LocalMachine\$store $($_.Thumbprint)", 'Remove certificate')) { Remove-Item $_.PSPath }
            }
    }
}
