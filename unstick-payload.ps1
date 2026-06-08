# unstick-payload.ps1 — pre-build helper for umbra-payload.vcxproj.
#
# A global WH_CBT hook DLL lingers, still mapped, in every process it touched until
# that process pumps a message after we unhook — which locks the file so the linker
# can't overwrite it on the next build (LNK1168). A mapped image CAN still be renamed,
# so: if the current umbra-payload.dll is locked, move it aside to .dll.<6 digits> so a
# fresh one links; then sweep aside-copies that have since been released. Run only when
# the DLL is actually locked, so an up-to-date incremental build keeps its output (and a
# locked-but-stale build self-heals: the now-missing output forces a relink).
[CmdletBinding()]
param([Parameter(Mandatory = $true)][string]$Dll)

$ErrorActionPreference = 'SilentlyContinue'

$dir  = Split-Path -Parent $Dll
$name = Split-Path -Leaf   $Dll
if (-not $dir -or -not (Test-Path -LiteralPath $dir)) { return }

if (Test-Path -LiteralPath $Dll) {
    $locked = $false
    try { $fs = [IO.File]::Open($Dll, 'Open', 'ReadWrite', 'None'); $fs.Close() }
    catch { $locked = $true }
    if ($locked) {
        $suffix = '{0:D6}' -f [int]([DateTime]::UtcNow.Ticks % 1000000)
        Move-Item -LiteralPath $Dll -Destination ("{0}.{1}" -f $Dll, $suffix) -Force
        Write-Host "unstick: umbra-payload.dll was locked (still mapped); moved aside as .$suffix"
    }
}

# Delete aside-copies that have since been released (locked ones silently skip).
Get-ChildItem -LiteralPath $dir -Filter "$name.*" -ErrorAction SilentlyContinue | ForEach-Object {
    if ($_.Name -match '\.\d{6}$') {
        try { Remove-Item -LiteralPath $_.FullName -Force -ErrorAction Stop } catch { }
    }
}
