#!/usr/bin/env pwsh
<#
.SYNOPSIS
  Packages the built WM_NIGHT binaries as a full-trust MSIX (sideload / Store).

.DESCRIPTION
  Stages WM_NIGHT.exe + WM_NIGHThook.dll, generates the PNG logo set + a resources.pri
  index from the app icon master, fills in the AppxManifest template, runs makeappx, and
  (unless -NoSign) signs the package with Azure Trusted Signing -- reusing the same
  ARTIFACT_SIGNING_* env vars as the .vcxproj sign step.

  Adapted from ../clipp/scripts/package_windows_msix.ps1.

  The manifest declares the rescap:uiAccess and rescap:unvirtualizedResources restricted
  capabilities, so a packaged WM_NIGHT keeps uiAccess (it can still theme elevated regedit/mmc)
  and its HKCU writes reach the injected DLL + the logon-scan autostart (registry write
  virtualization disabled) -- the SAME code as the loose build, no refactor. Both are RESTRICTED
  capabilities: sideloading a signed package is fine; the Store needs approval; and
  unvirtualizedResources may constrain install to PowerShell/dev-mode. See the manifest header.

.NOTES
  Requires the Windows 10/11 SDK (makeappx.exe, makepri.exe) and, to sign, sign.exe
  (dotnet tool install --global --prerelease sign). Run the logo step in Windows
  PowerShell if System.Drawing is missing in pwsh. Build the matching arch first:
    msbuild WM_NIGHT.sln /p:Configuration=Release /p:Platform=x64
#>
[CmdletBinding()]
param(
    [ValidateSet('amd64', 'arm64')]
    [string]$Arch = 'amd64',

    # MSIX version (W.X.Y.Z). Omit to read it from the built WM_NIGHT.exe's VERSIONINFO.
    [string]$Version = '',

    [string]$BuildDir = 'build\Release\x64',
    [string]$OutDir   = 'build\msix',

    # Package identity. Publisher must equal the signing cert subject; it is read from the
    # signed WM_NIGHT.exe automatically, so normally leave it unset.
    [string]$IdentityName = 'WM-NIGHT',   # Identity Name: pattern [-.A-Za-z0-9]+, NO underscore (DisplayName below keeps WM_NIGHT)
    [string]$Publisher = '',
    [string]$PublisherDisplayName = 'Marton Anka',

    # The icon master (1024x1024 transparent PNG) the logo set is downscaled from.
    [string]$MasterImage = 'resources\moon01.png',

    # Pack only, leaving the .msix unsigned (CI signs it). Locally you normally omit this.
    [switch]$NoSign
)

$ErrorActionPreference = 'Stop'
$repoRoot = Split-Path -Parent $PSScriptRoot
Set-Location $repoRoot
# child tools (makeappx/makepri/sign) resolve relative paths against the .NET CWD; keep it
# in sync and make working paths absolute so nothing depends on it.
[Environment]::CurrentDirectory = $repoRoot
if (-not [IO.Path]::IsPathRooted($BuildDir))    { $BuildDir = Join-Path $repoRoot $BuildDir }
if (-not [IO.Path]::IsPathRooted($OutDir))      { $OutDir = Join-Path $repoRoot $OutDir }
if (-not [IO.Path]::IsPathRooted($MasterImage)) { $MasterImage = Join-Path $repoRoot $MasterImage }
$manifestTemplate = Join-Path $repoRoot 'packaging\AppxManifest.xml.in'

# MSIX ProcessorArchitecture names differ from our msbuild Platform names.
$msixArch = if ($Arch -eq 'amd64') { 'x64' } else { 'arm64' }

# Trusted Signing is mandatory unless -NoSign. Fail fast before building the layout.
if (-not $NoSign) {
    if (-not (Get-Command sign.exe -ErrorAction SilentlyContinue)) {
        throw "sign.exe (the Trusted Signing CLI) is not on PATH. Install it: dotnet tool install --global --prerelease sign"
    }
    foreach ($name in 'ARTIFACT_SIGNING_ENDPOINT', 'ARTIFACT_SIGNING_ACCOUNT', 'ARTIFACT_SIGNING_CERTIFICATE_PROFILE') {
        if (-not [Environment]::GetEnvironmentVariable($name)) {
            throw "Signing requires $name. Run 'az login' and set the ARTIFACT_SIGNING_* vars (same as the .vcxproj sign step)."
        }
    }
}

function Find-SdkTool {
    param([Parameter(Mandatory)][string]$Name)
    $onPath = Get-Command $Name -ErrorAction SilentlyContinue
    if ($onPath) { return $onPath.Source }
    $kitsBin = Join-Path ${env:ProgramFiles(x86)} 'Windows Kits\10\bin'
    if (-not (Test-Path $kitsBin)) {
        throw "Windows SDK not found at '$kitsBin'. Install the Windows 10/11 SDK (ships makeappx.exe and makepri.exe)."
    }
    $hostArch = if ($env:PROCESSOR_ARCHITECTURE -eq 'ARM64') { 'arm64' } else { 'x64' }
    $versions = Get-ChildItem -Path $kitsBin -Directory |
        Where-Object { $_.Name -like '10.*' } | Sort-Object Name -Descending
    foreach ($v in $versions) {
        foreach ($a in @($hostArch, 'x64', 'x86')) {
            $candidate = Join-Path $v.FullName "$a\$Name"
            if (Test-Path $candidate) { return $candidate }
        }
    }
    throw "Could not locate $Name under '$kitsBin'."
}

function New-LogoPng {
    # Downscale the master app image to a Size x Size transparent PNG at an absolute path.
    param([Parameter(Mandatory)][string]$Master, [Parameter(Mandatory)][string]$Path, [Parameter(Mandatory)][int]$Size)
    $src = [System.Drawing.Image]::FromFile($Master)
    try {
        $dst = New-Object System.Drawing.Bitmap $Size, $Size
        try {
            $g = [System.Drawing.Graphics]::FromImage($dst)
            $g.InterpolationMode = [System.Drawing.Drawing2D.InterpolationMode]::HighQualityBicubic
            $g.PixelOffsetMode = [System.Drawing.Drawing2D.PixelOffsetMode]::HighQuality
            $g.SmoothingMode = [System.Drawing.Drawing2D.SmoothingMode]::HighQuality
            $g.Clear([System.Drawing.Color]::Transparent)
            $g.DrawImage($src, 0, 0, $Size, $Size)
            $g.Dispose()
            $dst.Save($Path, [System.Drawing.Imaging.ImageFormat]::Png)
        } finally { $dst.Dispose() }
    } finally { $src.Dispose() }
}

$makeappx = Find-SdkTool 'makeappx.exe'
$makepri = Find-SdkTool 'makepri.exe'
Write-Host "[*] makeappx: $makeappx"
Write-Host "[*] makepri:  $makepri"

# --- locate the built binaries -----------------------------------------------
$exe = Join-Path $BuildDir 'WM_NIGHT.exe'        # /SUBSYSTEM:WINDOWS tray host
$dll = Join-Path $BuildDir 'WM_NIGHThook.dll'    # payload (host LoadLibrary's it at runtime)
if (-not (Test-Path $exe)) {
    throw "WM_NIGHT.exe not found at '$exe'. Build the $Arch slice first: msbuild WM_NIGHT.sln /p:Configuration=Release /p:Platform=$msixArch"
}
if (-not (Test-Path $dll)) { throw "WM_NIGHThook.dll not found at '$dll' (the payload must ship in the package)." }
if (-not (Test-Path $MasterImage)) { throw "Master app image not found: $MasterImage" }
if (-not (Test-Path $manifestTemplate)) { throw "Missing manifest template: $manifestTemplate" }

# Default the package version to the exe's VERSIONINFO so it always matches the binary.
if (-not $Version) {
    $vi = [Diagnostics.FileVersionInfo]::GetVersionInfo($exe)
    $Version = '{0}.{1}.{2}.{3}' -f $vi.FileMajorPart, $vi.FileMinorPart, $vi.FileBuildPart, $vi.FilePrivatePart
    Write-Host "[*] Version (from WM_NIGHT.exe): $Version"
}

# The manifest Publisher must equal the Trusted Signing cert subject. Read it off the signed
# exe (avoids a hand-typed DN that won't match). Pass -Publisher if the exe isn't signed yet.
$sig = Get-AuthenticodeSignature $exe -ErrorAction SilentlyContinue
if ($sig -and $sig.SignerCertificate -and $sig.Status -ne 'NotSigned') {
    $detected = $sig.SignerCertificate.Subject
    if ($Publisher -and $Publisher -ne $detected) {
        Write-Warning "Overriding -Publisher '$Publisher' with the signed exe's cert subject '$detected' (MSIX requires Publisher == signing cert subject)."
    }
    $Publisher = $detected
    Write-Host "[*] Publisher (from signed WM_NIGHT.exe): $Publisher"
}
elseif (-not $Publisher) {
    throw "WM_NIGHT.exe at '$exe' is not signed, so the cert subject can't be auto-detected. Sign it first (ARTIFACT_SIGNING_* set), or pass -Publisher with the exact subject."
}

# --- stage the package layout ------------------------------------------------
$layout = Join-Path $OutDir "layout-$Arch"
if (Test-Path $layout) { Remove-Item -Recurse -Force $layout }
$layoutImages = Join-Path $layout 'Images'
New-Item -ItemType Directory -Force -Path $layoutImages | Out-Null

Copy-Item $exe (Join-Path $layout 'WM_NIGHT.exe') -Force
Copy-Item $dll (Join-Path $layout 'WM_NIGHThook.dll') -Force

# --- generate the PNG logo set ----------------------------------------------
# MSIX uses PNG (never .ico) for tile/Start/taskbar. The manifest names the base logos;
# Windows resolves the scale-*/targetsize-*/altform-unplated variants via resources.pri.
if (-not ('System.Drawing.Bitmap' -as [type])) {
    try { Add-Type -AssemblyName System.Drawing -ErrorAction Stop }
    catch { throw "System.Drawing is unavailable in this PowerShell. Run this script in Windows PowerShell (powershell.exe), where it is built in." }
}
$scales = @(125, 150, 200, 400)
# logical name => base (scale-100) pixel size. All square (the wide tile is skipped).
$squareLogos = [ordered]@{
    'StoreLogo'         = 50
    'Square44x44Logo'   = 44
    'Square71x71Logo'   = 71
    'Square150x150Logo' = 150
}
foreach ($name in $squareLogos.Keys) {
    $base = $squareLogos[$name]
    New-LogoPng -Master $MasterImage -Path (Join-Path $layoutImages "$name.png") -Size $base
    foreach ($s in $scales) {
        $px = [int][Math]::Round($base * $s / 100.0, [MidpointRounding]::AwayFromZero)
        New-LogoPng -Master $MasterImage -Path (Join-Path $layoutImages "$name.scale-$s.png") -Size $px
    }
}
# Square44x44 app-icon target sizes, plated + altform-unplated (the unplated set renders the
# taskbar/Start icon transparently, with no colored plate).
foreach ($t in @(16, 24, 32, 48, 256)) {
    New-LogoPng -Master $MasterImage -Path (Join-Path $layoutImages "Square44x44Logo.targetsize-$t.png") -Size $t
    New-LogoPng -Master $MasterImage -Path (Join-Path $layoutImages "Square44x44Logo.targetsize-${t}_altform-unplated.png") -Size $t
}
Write-Host "[*] Generated $((Get-ChildItem $layoutImages -Filter *.png).Count) logo PNGs in $layoutImages"

# --- substitute manifest tokens ----------------------------------------------
$manifest = (Get-Content -Raw $manifestTemplate).
    Replace('@IDENTITY_NAME@', $IdentityName).
    Replace('@PUBLISHER@', $Publisher).
    Replace('@PUBLISHER_DISPLAY_NAME@', $PublisherDisplayName).
    Replace('@VERSION@', $Version).
    Replace('@ARCH@', $msixArch)
Set-Content -Path (Join-Path $layout 'AppxManifest.xml') -Value $manifest -Encoding UTF8

# --- index resources (resources.pri) -----------------------------------------
# Without this, Windows ignores the scale/targetsize/unplated qualifier files and the
# Start/taskbar icon can render blank. makepri builds the index from the file names.
New-Item -ItemType Directory -Force -Path $OutDir | Out-Null
$priConfig = Join-Path $OutDir 'priconfig.xml'
& $makepri createconfig /cf $priConfig /dq en-US /o
if ($LASTEXITCODE -ne 0) { throw "makepri createconfig failed ($LASTEXITCODE)." }
& $makepri new /pr $layout /cf $priConfig /mn (Join-Path $layout 'AppxManifest.xml') /of (Join-Path $layout 'resources.pri') /o
if ($LASTEXITCODE -ne 0) { throw "makepri new failed ($LASTEXITCODE)." }

# --- pack --------------------------------------------------------------------
$msix = Join-Path $OutDir "wm_night-$Version-$Arch.msix"
if (Test-Path $msix) { Remove-Item -Force $msix }
& $makeappx pack /d $layout /p $msix /o
if ($LASTEXITCODE -ne 0) { throw "makeappx failed ($LASTEXITCODE)." }
Write-Host "[*] Packed: $msix"

# --- sign (Azure Trusted Signing) --------------------------------------------
if ($NoSign) {
    Write-Host '[*] -NoSign: leaving the .msix unsigned.'
}
else {
    $signArgs = @(
        'code', 'artifact-signing',
        '-b', $OutDir,
        '-ase', $env:ARTIFACT_SIGNING_ENDPOINT,
        '-asa', $env:ARTIFACT_SIGNING_ACCOUNT,
        (Split-Path -Leaf $msix),
        '-v', 'Information',
        '-ascp', $env:ARTIFACT_SIGNING_CERTIFICATE_PROFILE
    )
    & sign.exe @signArgs
    if ($LASTEXITCODE -ne 0) { throw "sign.exe (Trusted Signing) failed ($LASTEXITCODE)." }
    Write-Host '[*] Signed with Azure Trusted Signing.'
}

Write-Host ''
Write-Host "[*] Done: $msix"
Write-Host '[i] Manifest declares restricted capabilities (uiAccess, unvirtualizedResources):'
Write-Host '    sideload-signed is fine; the Store needs approval; unvirtualizedResources may need a'
Write-Host '    PowerShell/dev-mode install. The staged exe keeps its embedded uiAccess=true.'
if (-not $NoSign) {
    Write-Host ''
    Write-Host "Sideload-test:  Add-AppxPackage '$msix'"
    Write-Host 'Remove:         Get-AppxPackage *WM_NIGHT* | Remove-AppxPackage'
}
