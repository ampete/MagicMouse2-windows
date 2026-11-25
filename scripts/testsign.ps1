<#
.SYNOPSIS
  Test-signs the built driver package (MM2Filter.sys + catalog).

.DESCRIPTION
  Creates (once) a self-signed code-signing certificate "CN=MM2Filter Test Signing"
  in CurrentUser\My, exports its public part to build\MM2FilterTest.cer, signs
  MM2Filter.sys, regenerates the catalog with Inf2Cat, and signs the catalog.

  Runs as a normal user. It changes nothing system-wide: the certificate is NOT
  trusted by the machine until install.ps1 imports it (with your confirmation).

.EXAMPLE
  .\scripts\testsign.ps1 -Configuration Release
#>
param(
    [ValidateSet('Debug', 'Release')] [string] $Configuration = 'Release'
)
$ErrorActionPreference = 'Stop'

$root    = Split-Path -Parent $PSScriptRoot
$pkg     = Join-Path $root "build\x64\$Configuration\MM2Filter"
$sdkBin  = Get-ChildItem (Join-Path $root 'packages\Microsoft.Windows.SDK.CPP.*\c\bin\*\x64\signtool.exe') | Select-Object -Last 1
$inf2cat = Get-ChildItem (Join-Path $root 'packages\Microsoft.Windows.WDK.x64.*\c\bin\*\x86\Inf2Cat.exe') | Select-Object -Last 1
$subject = 'CN=MM2Filter Test Signing'
$cerPath = Join-Path $root 'build\MM2FilterTest.cer'

if (-not (Test-Path (Join-Path $pkg 'MM2Filter.sys'))) { throw "Build first: $pkg\MM2Filter.sys not found." }
if (-not $sdkBin -or -not $inf2cat) { throw 'signtool/Inf2Cat not found. Restore NuGet packages first (see README).' }

$cert = Get-ChildItem Cert:\CurrentUser\My -CodeSigningCert | Where-Object Subject -eq $subject | Select-Object -First 1
if (-not $cert) {
    Write-Host "Creating test certificate '$subject' in CurrentUser\My"
    $cert = New-SelfSignedCertificate -Type CodeSigningCert -Subject $subject `
        -CertStoreLocation Cert:\CurrentUser\My -KeyUsage DigitalSignature `
        -HashAlgorithm sha256 -NotAfter (Get-Date).AddYears(3)
}
Export-Certificate -Cert $cert -FilePath $cerPath -Force | Out-Null
Write-Host "Certificate: $($cert.Thumbprint)  ->  $cerPath"

# 1. Sign the binary (the catalog hashes the signed file, so this comes first).
& $sdkBin.FullName sign /fd sha256 /sha1 $cert.Thumbprint /s My (Join-Path $pkg 'MM2Filter.sys')
if ($LASTEXITCODE) { throw 'signtool failed on MM2Filter.sys' }

# 2. Rebuild the catalog for the signed binary.
Remove-Item (Join-Path $pkg '*.cat') -ErrorAction SilentlyContinue
& $inf2cat.FullName /driver:$pkg /os:10_X64 /uselocaltime
if ($LASTEXITCODE) { throw 'Inf2Cat failed' }

# 3. Sign the catalog.
& $sdkBin.FullName sign /fd sha256 /sha1 $cert.Thumbprint /s My (Join-Path $pkg 'MM2Filter.cat')
if ($LASTEXITCODE) { throw 'signtool failed on MM2Filter.cat' }

Write-Host "`nTest-signed package ready in $pkg"
