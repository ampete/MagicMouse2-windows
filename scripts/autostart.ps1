<#
.SYNOPSIS
    Starts the Magic Mouse 2 gesture service (hidden) when you log in.

.DESCRIPTION
    Adds (or with -Remove, deletes) the value "MM2Gestures" under
    HKCU\Software\Microsoft\Windows\CurrentVersion\Run, pointing at
    build\x64\Release\mm2gesturesw.exe - the gesture service without a
    console window. Per-user; needs no admin rights.

    Settings stay in build\x64\Release\mm2gestures.ini.
    To stop the hidden copy (e.g. before rebuilding or to see --verbose output):
        .\build\x64\Release\mm2gestures.exe --stop

.EXAMPLE
    .\scripts\autostart.ps1            # enable autostart
    .\scripts\autostart.ps1 -Remove    # disable autostart
#>
param([switch]$Remove)

$ErrorActionPreference = 'Stop'
$runKey = 'HKCU:\Software\Microsoft\Windows\CurrentVersion\Run'
$name   = 'MM2Gestures'

if ($Remove) {
    Remove-ItemProperty -Path $runKey -Name $name -ErrorAction SilentlyContinue
    Write-Host "Autostart removed. (A copy that is running now keeps running until you log off"
    Write-Host "or run: mm2gestures.exe --stop)"
    return
}

$exe = Join-Path $PSScriptRoot '..\build\x64\Release\mm2gesturesw.exe' | Resolve-Path -ErrorAction SilentlyContinue
if (-not $exe) {
    throw "mm2gesturesw.exe not found - build tools\gestures\mm2gesturesw.vcxproj (Release|x64) first."
}

Set-ItemProperty -Path $runKey -Name $name -Value "`"$($exe.Path)`""
Write-Host "Autostart enabled: $($exe.Path)"
Write-Host "It starts hidden at your next login. Stop it any time with: mm2gestures.exe --stop"
