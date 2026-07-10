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
# On any timeout the script enumerates every window the process owns and writes a full
# minidump of the live process into -DumpDir (defaults to $env:WER_DUMP_DIR) before killing
# it, so a hang is as diagnosable as a crash.
#
# The exe must be launchable in the calling environment. Note that a uiAccess-manifested
# build will not launch AT ALL where UAC is disabled (GitHub-hosted runners: EnableLUA=0
# means Windows cannot mint a UIAccess token, so CreateProcess fails with access denied) —
# the CI workflow strips uiAccess from the release binary first. On a normal desktop, point
# this at the installed package exe or any signed build. The app bounces explorer.exe on
# exit — harmless on a runner, mildly annoying on a desktop.

param(
    [Parameter(Mandatory)] [string] $ExePath,
    [string] $DumpDir = $env:WER_DUMP_DIR,
    [int] $SettingsTimeoutSec = 90,
    [int] $ExitTimeoutSec = 60
)

$ErrorActionPreference = 'Stop'

Add-Type -TypeDefinition @'
using System;
using System.Collections.Generic;
using System.Runtime.InteropServices;
using System.Text;

public static class Smoke
{
    [DllImport("user32.dll", CharSet = CharSet.Unicode, EntryPoint = "FindWindowW")]
    public static extern IntPtr FindWindow(string lpClassName, string lpWindowName);

    [DllImport("user32.dll", EntryPoint = "PostMessageW")]
    public static extern bool PostMessage(IntPtr hWnd, uint Msg, UIntPtr wParam, IntPtr lParam);

    [DllImport("user32.dll")]
    public static extern bool IsWindow(IntPtr hWnd);

    delegate bool EnumWindowsProc(IntPtr hWnd, IntPtr lParam);
    [DllImport("user32.dll")]
    static extern bool EnumWindows(EnumWindowsProc lpEnumFunc, IntPtr lParam);
    [DllImport("user32.dll")]
    static extern uint GetWindowThreadProcessId(IntPtr hWnd, out uint pid);
    [DllImport("user32.dll", CharSet = CharSet.Unicode)]
    static extern int GetClassNameW(IntPtr hWnd, StringBuilder buf, int max);
    [DllImport("user32.dll", CharSet = CharSet.Unicode)]
    static extern int GetWindowTextW(IntPtr hWnd, StringBuilder buf, int max);
    [DllImport("user32.dll")]
    static extern bool IsWindowVisible(IntPtr hWnd);

    // Every top-level window owned by pid: "class | title | visible".
    public static List<string> GetProcessWindows(uint pid)
    {
        var result = new List<string>();
        EnumWindows((h, l) =>
        {
            uint wpid;
            GetWindowThreadProcessId(h, out wpid);
            if (wpid == pid)
            {
                var cls = new StringBuilder(256);
                GetClassNameW(h, cls, 256);
                var txt = new StringBuilder(256);
                GetWindowTextW(h, txt, 256);
                result.Add(string.Format("class={0} | title=\"{1}\" | visible={2}",
                                         cls, txt, IsWindowVisible(h)));
            }
            return true;
        }, IntPtr.Zero);
        return result;
    }

    [DllImport("dbghelp.dll", SetLastError = true)]
    public static extern bool MiniDumpWriteDump(IntPtr hProcess, uint ProcessId, IntPtr hFile,
        int DumpType, IntPtr ExceptionParam, IntPtr UserStreamParam, IntPtr CallbackParam);
}
'@

$WM_CLOSE   = 0x0010
$WM_COMMAND = 0x0111
$IDM_EXIT   = 40003        # src/resource.h

# On a timeout: list the process's windows, dump it live, kill it (unless -KeepAlive).
function Invoke-HangAutopsy([System.Diagnostics.Process] $proc, [string] $label, [switch] $KeepAlive)
{
    Write-Host "--- $label ---"
    if ($proc.HasExited) {
        Write-Host ("Process already exited: {0} (0x{1:X8})" -f $proc.ExitCode, $proc.ExitCode)
        return
    }
    Write-Host "Windows owned by PID $($proc.Id):"
    $wins = [Smoke]::GetProcessWindows([uint32]$proc.Id)
    if ($wins.Count -eq 0) { Write-Host '  (none)' }
    else { $wins | ForEach-Object { Write-Host "  $_" } }

    if ($DumpDir) {
        New-Item -ItemType Directory -Force $DumpDir | Out-Null
        $dumpPath = Join-Path $DumpDir "WM_NIGHT-hang-$label.dmp"
        $file = [System.IO.File]::Create($dumpPath)
        try {
            # 2 = MiniDumpWithFullMemory
            $ok = [Smoke]::MiniDumpWriteDump($proc.Handle, [uint32]$proc.Id,
                $file.SafeFileHandle.DangerousGetHandle(), 2,
                [IntPtr]::Zero, [IntPtr]::Zero, [IntPtr]::Zero)
        } finally { $file.Dispose() }
        if ($ok) { Write-Host "Live minidump written: $dumpPath" }
        else     { Write-Host "MiniDumpWriteDump failed (Win32 $([Runtime.InteropServices.Marshal]::GetLastWin32Error()))" }
    }
    if (-not $KeepAlive) {
        Stop-Process -Id $proc.Id -Force -ErrorAction SilentlyContinue
    }
}

if (-not (Test-Path $ExePath)) { throw "Not found: $ExePath" }

# Launch like the winget harness: by path, no arguments.
Write-Host "Launching $ExePath"
$p = Start-Process -FilePath $ExePath -PassThru

# A no-arg launch opens the Settings window; XAML Islands cold start can be slow on a runner.
$settings = [IntPtr]::Zero
$deadline = (Get-Date).AddSeconds($SettingsTimeoutSec)
while ((Get-Date) -lt $deadline) {
    $settings = [Smoke]::FindWindow('WM_NIGHT_Settings', $null)
    if ($settings -ne [IntPtr]::Zero) { break }
    if ($p.HasExited) {
        throw ("App exited before the Settings window appeared: exit code {0} (0x{1:X8})" -f $p.ExitCode, $p.ExitCode)
    }
    Start-Sleep -Milliseconds 500
}
if ($settings -eq [IntPtr]::Zero) {
    Invoke-HangAutopsy $p 'no-settings-window' -KeepAlive
    # Even without Settings, the teardown path is testable: if the tray window exists, try a
    # graceful exit anyway and report the exit code — that's the number winget cares about.
    $tray = [Smoke]::FindWindow('WM_NIGHT_Tray', $null)
    if ($tray -ne [IntPtr]::Zero -and -not $p.HasExited) {
        Write-Host 'Tray window exists; attempting graceful exit (IDM_EXIT) for an exit-code reading...'
        [void][Smoke]::PostMessage($tray, $WM_COMMAND, [UIntPtr]::new($IDM_EXIT), [IntPtr]::Zero)
        if ($p.WaitForExit(30000)) {
            Write-Host ("Graceful exit code without Settings: {0} (0x{1:X8})" -f $p.ExitCode, $p.ExitCode)
        } else {
            Write-Host 'No exit within 30s of IDM_EXIT either.'
        }
    }
    Stop-Process -Id $p.Id -Force -ErrorAction SilentlyContinue
    throw "Settings window did not appear within ${SettingsTimeoutSec}s (see window list / minidump above)."
}
Write-Host 'Settings window is up.'

# Let the async content settle (exe icons load via fire_and_forget coroutines).
Start-Sleep -Seconds 5

Write-Host 'Closing the Settings window (WM_CLOSE)...'
[void][Smoke]::PostMessage($settings, $WM_CLOSE, [UIntPtr]::Zero, [IntPtr]::Zero)
$deadline = (Get-Date).AddSeconds(15)
while ((Get-Date) -lt $deadline -and [Smoke]::IsWindow($settings)) {
    Start-Sleep -Milliseconds 250
}
if ([Smoke]::IsWindow($settings)) {
    Invoke-HangAutopsy $p 'settings-wont-close'
    throw 'Settings window did not close on WM_CLOSE.'
}

Write-Host 'Exiting via the tray window (WM_COMMAND / IDM_EXIT)...'
$tray = [Smoke]::FindWindow('WM_NIGHT_Tray', $null)
if ($tray -eq [IntPtr]::Zero) {
    Invoke-HangAutopsy $p 'no-tray-window'
    throw 'Tray window not found after closing Settings.'
}
[void][Smoke]::PostMessage($tray, $WM_COMMAND, [UIntPtr]::new($IDM_EXIT), [IntPtr]::Zero)

if (-not $p.WaitForExit($ExitTimeoutSec * 1000)) {
    Invoke-HangAutopsy $p 'teardown-hang'
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
