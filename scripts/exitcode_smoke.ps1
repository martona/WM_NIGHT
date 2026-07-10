# SPDX-License-Identifier: MIT
#
# Exit-code smoke test: replays the winget validation harness's launch sequence against a
# WM_NIGHT.exe and fails if the process exits nonzero (the harness treats any nonzero exit
# code as a failed run — see winget-pkgs PR #393605, exit code 0xC000027B).
#
# Sequence: launch the exe with NO arguments (that opens the XAML-Islands Settings window,
# the riskiest surface); close Settings via WM_CLOSE; exit via the tray window
# (WM_COMMAND/IDM_EXIT, the full SettingsShutdown teardown path); assert exit code 0.
#
# The exe must be launchable in the calling environment. Note that a uiAccess-manifested
# build will not launch AT ALL where UAC is disabled (GitHub-hosted runners: EnableLUA=0
# means Windows cannot mint a UIAccess token, so CreateProcess fails with access denied) —
# the CI workflow strips uiAccess from the release binary first. On a normal desktop, point
# this at the installed package exe or any signed build. The app bounces explorer.exe on
# exit — harmless on a runner, mildly annoying on a desktop.

param(
    [Parameter(Mandatory)] [string] $ExePath,
    [int] $SettingsTimeoutSec = 90,
    [int] $ExitTimeoutSec = 60
)

$ErrorActionPreference = 'Stop'

Add-Type -Namespace Smoke -Name Native -MemberDefinition @'
[DllImport("user32.dll", CharSet = CharSet.Unicode, EntryPoint = "FindWindowW")]
public static extern IntPtr FindWindow(string lpClassName, string lpWindowName);
[DllImport("user32.dll", EntryPoint = "PostMessageW")]
public static extern bool PostMessage(IntPtr hWnd, uint Msg, UIntPtr wParam, IntPtr lParam);
[DllImport("user32.dll")]
public static extern bool IsWindow(IntPtr hWnd);
'@

$WM_CLOSE   = 0x0010
$WM_COMMAND = 0x0111
$IDM_EXIT   = 40003        # src/resource.h

if (-not (Test-Path $ExePath)) { throw "Not found: $ExePath" }

# Launch like the winget harness: by path, no arguments.
Write-Host "Launching $ExePath"
$p = Start-Process -FilePath $ExePath -PassThru

# A no-arg launch opens the Settings window; XAML Islands cold start can be slow on a runner.
$settings = [IntPtr]::Zero
$deadline = (Get-Date).AddSeconds($SettingsTimeoutSec)
while ((Get-Date) -lt $deadline) {
    $settings = [Smoke.Native]::FindWindow('WM_NIGHT_Settings', $null)
    if ($settings -ne [IntPtr]::Zero) { break }
    if ($p.HasExited) {
        throw ("App exited before the Settings window appeared: exit code {0} (0x{1:X8})" -f $p.ExitCode, $p.ExitCode)
    }
    Start-Sleep -Milliseconds 500
}
if ($settings -eq [IntPtr]::Zero) {
    # The app's XAML-init catch path shows an error MessageBox instead of a Settings window.
    $mbox = [Smoke.Native]::FindWindow('#32770', 'WM_NIGHT')
    Stop-Process -Id $p.Id -Force -ErrorAction SilentlyContinue
    if ($mbox -ne [IntPtr]::Zero) {
        throw 'XAML Islands failed to initialize (the app showed its error dialog).'
    }
    throw "Settings window did not appear within ${SettingsTimeoutSec}s."
}
Write-Host 'Settings window is up.'

# Let the async content settle (exe icons load via fire_and_forget coroutines).
Start-Sleep -Seconds 5

Write-Host 'Closing the Settings window (WM_CLOSE)...'
[void][Smoke.Native]::PostMessage($settings, $WM_CLOSE, [UIntPtr]::Zero, [IntPtr]::Zero)
$deadline = (Get-Date).AddSeconds(15)
while ((Get-Date) -lt $deadline -and [Smoke.Native]::IsWindow($settings)) {
    Start-Sleep -Milliseconds 250
}
if ([Smoke.Native]::IsWindow($settings)) {
    Stop-Process -Id $p.Id -Force -ErrorAction SilentlyContinue
    throw 'Settings window did not close on WM_CLOSE.'
}

Write-Host 'Exiting via the tray window (WM_COMMAND / IDM_EXIT)...'
$tray = [Smoke.Native]::FindWindow('WM_NIGHT_Tray', $null)
if ($tray -eq [IntPtr]::Zero) {
    Stop-Process -Id $p.Id -Force -ErrorAction SilentlyContinue
    throw 'Tray window not found after closing Settings.'
}
[void][Smoke.Native]::PostMessage($tray, $WM_COMMAND, [UIntPtr]::new($IDM_EXIT), [IntPtr]::Zero)

if (-not $p.WaitForExit($ExitTimeoutSec * 1000)) {
    Stop-Process -Id $p.Id -Force -ErrorAction SilentlyContinue
    throw "App did not exit within ${ExitTimeoutSec}s of IDM_EXIT (teardown hang?)."
}

$code = $p.ExitCode
Write-Host ("Exit code: {0} (0x{1:X8})" -f $code, $code)
if ($code -ne 0) {
    if ($code -eq -1073741189) {
        throw 'FAIL: exit code 0xC000027B (stowed exception) — the winget validation failure, reproduced.'
    }
    throw ("FAIL: nonzero exit code {0} (0x{1:X8})" -f $code, $code)
}
Write-Host 'PASS: clean exit.'
