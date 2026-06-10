// SPDX-License-Identifier: MIT
//
// WM_NIGHT.exe — GUI (tray-resident) host / controller for the WM_NIGHThook DLL.
//
// Installs ONE global WH_CBT hook whose proc lives in WM_NIGHThook.dll, then lives in the
// notification area pumping messages so the hook stays live. WH_CBT maps the DLL into
// each target process as early as its first window; the DLL whitelists its targets and
// themes from there. uiAccess (asInvoker + signed, see WM_NIGHT.vcxproj) lets this
// medium-integrity host reach an elevated regedit without elevating itself.

// Common-Controls v6: the dark-mode opt-in. With this dependency embedded in the manifest +
// umbra::initDarkMode() at startup, the system draws our popup menu and message boxes dark
// automatically — no owner-draw needed. (The linker merges this with the UAC trustInfo it
// already generates, so there is still no separate .manifest file.)
#pragma comment(linker,                                  \
    "\"/manifestdependency:type='win32' "                \
    "name='Microsoft.Windows.Common-Controls' "          \
    "version='6.0.0.0' processorArchitecture='*' "       \
    "publicKeyToken='6595b64144ccf1df' language='*'\"")

#define _CRT_RAND_S      // enable rand_s (random staged-DLL name)
#include <cstdarg>
#include <cstdlib>       // rand_s
#include <string>
#include <vector>

#include <windows.h>
#include <appmodel.h>   // GetCurrentPackageFullName (detect MSIX packaging)
#include <shellapi.h>   // Shell_NotifyIcon / NOTIFYICONDATA (tray icon)
#include <commctrl.h>   // LoadIconMetric, InitCommonControlsEx
#include <shlobj.h>     // SHGetKnownFolderPath, SHCreateDirectoryExW
#include <strsafe.h>    // StringCch* helpers
#include <dbghelp.h>    // symbol resolution for dui70's non-exported Element::PaintBackground
#include <tlhelp32.h>   // process snapshot, to bounce explorer.exe on shutdown

#include "umbra.h"      // dark-mode library (linked into the host for its own UI)
#include "resource.h"
#include "version.h"
#include "SettingsWindow.h"   // XAML-Islands Settings window
#include "AboutDialog.h"      // About box (Win32 dialog)
#include "HostDiag.h"         // startup diagnostics shown in the Settings window

#pragma comment(lib, "dbghelp.lib")
#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "advapi32.lib")   // registry: autostart + single-instance
#pragma comment(lib, "umbra.lib")      // dark-mode library (vcpkg: WM_UMBRA git registry)

namespace
{
    constexpr UINT     kTrayCallback = WM_APP + 1;     // tray icon → WndProc notification
    constexpr UINT     kTrayIconId   = 1;
    constexpr wchar_t  kWndClassName[] = L"WM_NIGHT_Tray";

    HINSTANCE g_hInst          = nullptr;
    HWND      g_hWnd           = nullptr;   // hidden top-level owner of the tray icon
    HMODULE   g_hookModule     = nullptr;
    HHOOK     g_cbtHook        = nullptr;
    HICON     g_trayIcon       = nullptr;
    UINT      g_taskbarCreated = 0;         // RegisterWindowMessage("TaskbarCreated")

    // Startup-diagnostics snapshot, surfaced in the Settings window via the HostDiag accessors.
    bool      g_diagUiAccess   = false;
    bool      g_diagDuiEnabled = false;   // EnableDuiHook registry gate state (default OFF)
    bool      g_diagDuiOk      = false;
    DWORD     g_diagDuiRva     = 0;

    // Did we actually receive uiAccess? (Only granted to a signed binary in a trusted path.)
    bool HasUiAccess()
    {
        HANDLE token = nullptr;
        if (!::OpenProcessToken(::GetCurrentProcess(), TOKEN_QUERY, &token))
            return false;
        DWORD uiAccess = 0, returned = 0;
        const bool ok = ::GetTokenInformation(token, TokenUIAccess, &uiAccess,
                                              sizeof(uiAccess), &returned) != FALSE && uiAccess != 0;
        ::CloseHandle(token);
        return ok;
    }

    // Diagnostics now go to the debugger — a GUI-subsystem app has no console. All of this is
    // best-effort: uiAccess is assumed to work, and the dui70 resolve below may fail silently.
    void Dbg(const wchar_t* fmt, ...)
    {
        wchar_t buf[1024];
        va_list ap;
        va_start(ap, fmt);
        ::wvsprintfW(buf, fmt, ap);
        va_end(ap);
        ::OutputDebugStringW(buf);
    }

    std::wstring ModuleDir()
    {
        wchar_t path[MAX_PATH]{};
        const DWORD n = ::GetModuleFileNameW(nullptr, path, ARRAYSIZE(path));
        std::wstring p(path, (n != 0 && n < ARRAYSIZE(path)) ? n : 0);
        const size_t slash = p.find_last_of(L"\\/");
        return slash == std::wstring::npos ? L"." : p.substr(0, slash);
    }

    // Register / unregister logon autostart (HKCU Run). The command carries /tray so the launched
    // instance stays quietly in the tray (no Settings pop). Best-effort.
    void SetAutostart(bool enable)
    {
        HKEY key = nullptr;
        if (::RegOpenKeyExW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Run",
                            0, KEY_SET_VALUE, &key) != ERROR_SUCCESS)
            return;
        if (enable)
        {
            wchar_t path[MAX_PATH]{};
            const DWORD n = ::GetModuleFileNameW(nullptr, path, ARRAYSIZE(path));
            if (n != 0 && n < ARRAYSIZE(path))
            {
                const std::wstring cmd = L"\"" + std::wstring(path) + L"\" /tray";
                ::RegSetValueExW(key, L"WM_NIGHT", 0, REG_SZ,
                    reinterpret_cast<const BYTE*>(cmd.c_str()),
                    static_cast<DWORD>((cmd.size() + 1) * sizeof(wchar_t)));
            }
        }
        else
        {
            ::RegDeleteValueW(key, L"WM_NIGHT");
        }
        ::RegCloseKey(key);
    }

    // --- dui70 Element::PaintBackground RVA resolution (host-side) -----------
    // The DLL wants to hook DirectUI::Element::PaintBackground in loaded shell processes.
    // Turns out the member is EXPORTED by dui70.dll, so we resolve its RVA once HERE off the export
    // table (no .pdb / symbol server / network needed — see ResolveDuiPaintBgRva) and hand the
    // DLL only an RVA + the dui70 PE identity, through the DLL's exported UmbraSetDuiPaintBg()
    // (a shared section forwards it to every loaded copy). The DLL adds the RVA to the live
    // dui70 base (validating the identity first). Assumes a matching arch: build the host x64 to
    // theme x64 shells (this box's case).

    // dui70's TimeDateStamp + SizeOfImage off disk — the DLL confirms its loaded copy matches
    // before trusting the RVA (a wrong RVA would crash on attach).
    bool ReadPeIdentityFromDisk(const wchar_t* path, DWORD& stamp, DWORD& sizeOfImage)
    {
        const HANDLE f = ::CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, nullptr,
                                       OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (f == INVALID_HANDLE_VALUE)
            return false;
        bool ok = false;
        const HANDLE map = ::CreateFileMappingW(f, nullptr, PAGE_READONLY, 0, 0, nullptr);
        if (map != nullptr)
        {
            auto* const p = static_cast<const BYTE*>(::MapViewOfFile(map, FILE_MAP_READ, 0, 0, 0));
            if (p != nullptr)
            {
                auto* const dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(p);
                if (dos->e_magic == IMAGE_DOS_SIGNATURE)
                {
                    auto* const nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(p + dos->e_lfanew);
                    if (nt->Signature == IMAGE_NT_SIGNATURE)
                    {
                        stamp       = nt->FileHeader.TimeDateStamp;
                        sizeOfImage = nt->OptionalHeader.SizeOfImage;
                        ok          = true;
                    }
                }
                ::UnmapViewOfFile(p);
            }
            ::CloseHandle(map);
        }
        ::CloseHandle(f);
        return ok;
    }

    struct SymHit { DWORD64 addr; std::string name; };

    BOOL CALLBACK EnumSymProc(PSYMBOL_INFO sym, ULONG, PVOID ctx)
    {
        if (sym != nullptr && ctx != nullptr)
            static_cast<std::vector<SymHit>*>(ctx)->push_back(
                SymHit{ sym->Address, std::string(sym->Name, sym->NameLen) });
        return TRUE;
    }

    // %LocalLow%\WM_NIGHT — our per-user data dir. Under MSIX, filesystem write-virtualization is
    // disabled (see the AppxManifest), so writes here hit the REAL path — which OUT-of-package
    // targets (explorer/regedit) can read. That is what makes the staged DLL reachable.
    std::wstring LocalLowAppDir()
    {
        // FOLDERID_LocalLow = {A520A1A4-1780-4FF6-BD18-167343C5AF16}, spelled out so we don't
        // depend on the KnownFolders.h GUID export (absent in this SDK include config).
        const GUID localLow =
            { 0xA520A1A4, 0x1780, 0x4FF6, { 0xBD, 0x18, 0x16, 0x73, 0x43, 0xC5, 0xAF, 0x16 } };
        PWSTR p = nullptr;
        std::wstring dir;
        if (SUCCEEDED(::SHGetKnownFolderPath(localLow, 0, nullptr, &p)) && p != nullptr)
            dir.assign(p).append(L"\\WM_NIGHT");
        ::CoTaskMemFree(p);
        return dir;
    }

    // --- DLL staging (MSIX) ----------------------------------------------
    // A packaged host's WM_NIGHThook.dll lives in WindowsApps, which the loaded targets cannot
    // read — so a global hook installed from it never lands. We copy it to real %LocalLow% (which
    // those targets CAN read) and hook from there. The copy takes a RANDOM name because a hook DLL
    // stays mapped — and the file stays locked — in any target still running from a prior session;
    // a fresh name dodges that lock instead of fighting it.
    bool IsPackaged()
    {
        UINT32 len = 0;
        return ::GetCurrentPackageFullName(&len, nullptr) != APPMODEL_ERROR_NO_PACKAGE;
    }

    // Best-effort sweep of previously staged copies. Ones still mapped in a live target fail to
    // delete (locked) and are simply left for a future run.
    void SweepStagedDlls(const std::wstring& dir)
    {
        WIN32_FIND_DATAW fd{};
        const std::wstring pattern = dir + L"\\WM_NIGHThook.*.dll";
        const HANDLE h = ::FindFirstFileW(pattern.c_str(), &fd);
        if (h == INVALID_HANDLE_VALUE)
            return;
        do
        {
            const std::wstring f = dir + L"\\" + fd.cFileName;
            ::DeleteFileW(f.c_str());
        } while (::FindNextFileW(h, &fd));
        ::FindClose(h);
    }

    // Copy the in-package DLL to %LocalLow%\WM_NIGHT\WM_NIGHThook.<rand>.dll; returns that path,
    // or empty on failure (the caller then falls back to the in-package DLL).
    std::wstring StageDll(const std::wstring& source)
    {
        const std::wstring dir = LocalLowAppDir();
        if (dir.empty())
            return {};
        ::SHCreateDirectoryExW(nullptr, dir.c_str(), nullptr);
        SweepStagedDlls(dir);

        unsigned int r = 0;
        if (::rand_s(&r) != 0)
            r = ::GetTickCount();   // fallback; the name only needs to be unlikely to hit a lock
        wchar_t name[64];
        ::StringCchPrintfW(name, ARRAYSIZE(name), L"WM_NIGHThook.%010u.dll", r);
        const std::wstring dest = dir + L"\\" + name;

        if (!::CopyFileW(source.c_str(), dest.c_str(), FALSE))
        {
            Dbg(L"[stage] CopyFile to %ls failed: %lu\n", dest.c_str(), ::GetLastError());
            return {};
        }
        return dest;
    }

    // Where to LoadLibrary the DLL from. Loose build: next to the exe (already target-readable).
    // Packaged build: a staged copy in %LocalLow% (the WindowsApps copy isn't target-readable).
    std::wstring ResolveDllPath()
    {
        const std::wstring inPackage = ModuleDir() + L"\\WM_NIGHThook.dll";
        if (!IsPackaged())
            return inPackage;
        const std::wstring staged = StageDll(inPackage);
        return staged.empty() ? inPackage : staged;
    }

    // Resolve DirectUI::Element::PaintBackground's RVA in dui70.dll. The member is EXPORTED, so
    // dbghelp finds it straight from the export table with an EMPTY symbol search path — no symbol
    // server, no _NT_SYMBOL_PATH, no .pdb, no network. Offline and instant. We require the EXACT
    // undecorated name (SYMOPT_UNDNAME); a looser/wildcard match could grab a wrong address.
    bool ResolveDuiPaintBgRva(DWORD& outRva, DWORD& outStamp, DWORD& outSize)
    {
        wchar_t dui[MAX_PATH];
        const UINT n = ::GetSystemDirectoryW(dui, ARRAYSIZE(dui));
        if (n == 0 || n >= ARRAYSIZE(dui))
            return false;
        ::StringCchCatW(dui, ARRAYSIZE(dui), L"\\dui70.dll");

        DWORD diskStamp = 0, diskSize = 0;
        if (!ReadPeIdentityFromDisk(dui, diskStamp, diskSize))
        {
            Dbg(L"[dui] cannot read %ls\n", dui);
            return false;
        }

        const HANDLE proc = ::GetCurrentProcess();
        ::SymSetOptions(SYMOPT_UNDNAME | SYMOPT_FAIL_CRITICAL_ERRORS | SYMOPT_NO_PROMPTS);
        if (!::SymInitializeW(proc, L"", FALSE))   // empty path: export table only, never the network
        {
            Dbg(L"[dui] SymInitialize failed: %lu\n", ::GetLastError());
            return false;
        }

        bool wrote = false;
        const DWORD64 base = ::SymLoadModuleExW(proc, nullptr, dui, nullptr, 0, 0, nullptr, 0);
        if (base == 0)
        {
            Dbg(L"[dui] SymLoadModuleEx failed: %lu\n", ::GetLastError());
        }
        else
        {
            std::vector<SymHit> hits;
            ::SymEnumSymbols(proc, base, "DirectUI::Element::PaintBackground", EnumSymProc, &hits);
            DWORD64 chosen = 0;
            for (const SymHit& h : hits)
                if (h.name == "DirectUI::Element::PaintBackground")
                    chosen = h.addr;

            if (chosen != 0)
            {
                outRva   = static_cast<DWORD>(chosen - base);
                outStamp = diskStamp;
                outSize  = diskSize;
                wrote    = true;
                Dbg(L"[dui] resolved (export) rva=0x%08lX (dui70 stamp=%08lX size=%08lX)\n",
                    outRva, diskStamp, diskSize);
            }
            else
            {
                Dbg(L"[dui] DirectUI::Element::PaintBackground not found in dui70 exports\n");
            }
            ::SymUnloadModule64(proc, base);
        }

        ::SymCleanup(proc);
        return wrote;
    }

    // Registry gate for the dui70 Element::PaintBackground hook. DUI/Control-Panel theming is the
    // most invasive surface we touch — it Detours a private DirectUI member located only by RVA — so
    // it ships OFF and the user opts in via HKCU\Software\WM_NIGHT\EnableDuiHook (REG_DWORD) == 1.
    // Absent / 0 / wrong type => disabled (the default).
    bool IsDuiHookEnabled() noexcept
    {
        DWORD val = 0, sz = sizeof(val);
        return ::RegGetValueW(HKEY_CURRENT_USER, L"Software\\WM_NIGHT", L"EnableDuiHook",
                              RRF_RT_REG_DWORD, nullptr, &val, &sz) == ERROR_SUCCESS && val == 1;
    }

    // Resolve the dui70 RVA and hand it to the DLL via UmbraSetDuiPaintBg (a shared section
    // forwards it to every loaded copy). Synchronous and instant — it's an export-table lookup, no
    // network. Best-effort: on failure the DLL simply runs without the DUI paint hook.
    //
    // Gated: when EnableDuiHook is off (the default) we never hand over an RVA, so the shared
    // section stays 0 and no loaded copy attaches the hook (setProcessWideDuiPaintHook no-ops).
    void HandOffDuiPaintBg()
    {
        g_diagDuiEnabled = IsDuiHookEnabled();
        if (!g_diagDuiEnabled)
        {
            Dbg(L"[dui] EnableDuiHook off (default); DUI paint hook disabled. Set "
                L"HKCU\\Software\\WM_NIGHT\\EnableDuiHook=1 (DWORD) to enable.\n");
            return;
        }
        const auto setDui = reinterpret_cast<void (*)(unsigned long, unsigned long, unsigned long)>(
            ::GetProcAddress(g_hookModule, "UmbraSetDuiPaintBg"));
        if (setDui == nullptr)
            return;
        DWORD rva = 0, stamp = 0, size = 0;
        if (ResolveDuiPaintBgRva(rva, stamp, size))
        {
            setDui(rva, stamp, size);
            g_diagDuiRva = rva;
            g_diagDuiOk  = true;
        }
        else
            Dbg(L"[dui] unresolved; DLL runs WITHOUT the DUI paint hook.\n");
    }

    // Terminate explorer.exe so the still-pinned DLL is dropped from the shell and the next
    // run starts clean — the automated form of the manual "restart Explorer" between test cycles.
    // Windows' AutoRestartShell (on by default) brings a fresh shell straight back; we run at the
    // same user + integrity as explorer, so we are permitted to terminate it. Open File Explorer
    // windows are lost with the bounce — expected, the same as doing it by hand.
    void BounceExplorer()
    {
        const HANDLE snap = ::CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
        if (snap == INVALID_HANDLE_VALUE)
            return;
        PROCESSENTRY32W pe{};
        pe.dwSize = sizeof(pe);
        int bounced = 0;
        if (::Process32FirstW(snap, &pe))
        {
            do
            {
                if (::CompareStringOrdinal(pe.szExeFile, -1, L"explorer.exe", -1, TRUE) == CSTR_EQUAL)
                {
                    const HANDLE proc = ::OpenProcess(PROCESS_TERMINATE, FALSE, pe.th32ProcessID);
                    if (proc != nullptr)
                    {
                        if (::TerminateProcess(proc, 0))
                            ++bounced;
                        ::CloseHandle(proc);
                    }
                }
            } while (::Process32NextW(snap, &pe));
        }
        ::CloseHandle(snap);

        Dbg(bounced > 0 ? L"Bounced explorer.exe (%d); the shell auto-restarts clean.\n"
                        : L"explorer.exe not found; nothing to bounce.\n", bounced);
    }

    // --- Tray icon -----------------------------------------------------------
    void AddTrayIcon(HWND hWnd)
    {
        NOTIFYICONDATAW nid{};
        nid.cbSize           = sizeof(nid);
        nid.hWnd             = hWnd;
        nid.uID              = kTrayIconId;
        nid.uFlags           = NIF_ICON | NIF_MESSAGE | NIF_TIP;
        nid.uCallbackMessage = kTrayCallback;
        nid.hIcon            = g_trayIcon;
        ::StringCchCopyW(nid.szTip, ARRAYSIZE(nid.szTip), L"WM_NIGHT");
        ::Shell_NotifyIconW(NIM_ADD, &nid);
    }

    void RemoveTrayIcon(HWND hWnd)
    {
        NOTIFYICONDATAW nid{};
        nid.cbSize = sizeof(nid);
        nid.hWnd   = hWnd;
        nid.uID    = kTrayIconId;
        ::Shell_NotifyIconW(NIM_DELETE, &nid);
    }

    void ShowTrayMenu(HWND hWnd)
    {
        HMENU menu = ::CreatePopupMenu();
        if (menu == nullptr)
            return;
        ::AppendMenuW(menu, MF_STRING, IDM_SETTINGS, L"Settings...");
        ::AppendMenuW(menu, MF_STRING, IDM_ABOUT,    L"About...");
        ::AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
        ::AppendMenuW(menu, MF_STRING, IDM_EXIT,     L"Exit");
        ::SetMenuDefaultItem(menu, IDM_SETTINGS, FALSE);

        POINT pt{};
        ::GetCursorPos(&pt);
        // The foreground dance: make our window foreground so the menu dismisses when the user
        // clicks elsewhere, and post a benign message afterward so it closes cleanly (the classic
        // TrackPopupMenu tray-menu fix). TPM_RETURNCMD routes the choice back through WM_COMMAND.
        ::SetForegroundWindow(hWnd);
        const UINT cmd = static_cast<UINT>(::TrackPopupMenu(menu,
            TPM_RIGHTBUTTON | TPM_RETURNCMD | TPM_NONOTIFY, pt.x, pt.y, 0, hWnd, nullptr));
        ::PostMessageW(hWnd, WM_NULL, 0, 0);
        ::DestroyMenu(menu);
        if (cmd != 0)
            ::SendMessageW(hWnd, WM_COMMAND, cmd, 0);
    }

    LRESULT CALLBACK WndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam)
    {
        // Explorer (re)started: the taskbar was recreated, so re-add our icon. This broadcast only
        // reaches top-level windows — which is why the tray owner is NOT an HWND_MESSAGE window.
        if (msg == g_taskbarCreated && g_taskbarCreated != 0)
        {
            AddTrayIcon(hWnd);
            return 0;
        }

        switch (msg)
        {
        case WM_CREATE:
            AddTrayIcon(hWnd);
            return 0;

        case kTrayCallback:
            switch (LOWORD(lParam))
            {
            case WM_LBUTTONDBLCLK:
                ::SendMessageW(hWnd, WM_COMMAND, IDM_SETTINGS, 0);
                break;
            case WM_RBUTTONUP:
            case WM_CONTEXTMENU:
                ShowTrayMenu(hWnd);
                break;
            }
            return 0;

        case WM_COMMAND:
            switch (LOWORD(wParam))
            {
            case IDM_SETTINGS:
                ShowSettingsWindow(g_hInst);
                return 0;
            case IDM_ABOUT:
                ShowAboutDialog(hWnd);
                return 0;
            case IDM_EXIT:
                SetAutostart(false);   // manual exit: stop auto-running, then tear down
                ::DestroyWindow(hWnd);
                return 0;
            }
            return 0;

        case WM_DESTROY:
            RemoveTrayIcon(hWnd);
            ::PostQuitMessage(0);
            return 0;
        }
        return ::DefWindowProcW(hWnd, msg, wParam, lParam);
    }
}

// --- Settings-window diagnostics (declared in HostDiag.h; defined here so they can read the host
// state above). Each returns a single small-print line. -----------------------------------------
std::wstring DiagDllLine()
{
    if (g_hookModule == nullptr)
        return L"WM_NIGHThook.dll: NOT loaded";
    const auto ver = reinterpret_cast<unsigned long (*)()>(
        ::GetProcAddress(g_hookModule, "UmbraHookVersion"));
    if (ver == nullptr)
        return L"WM_NIGHThook.dll: loaded (no version export)";
    const unsigned long v = ver();
    wchar_t buf[64];
    ::wsprintfW(buf, L"WM_NIGHThook.dll: loaded  v%lu.%lu.%lu.%lu",
                (v >> 24) & 0xFF, (v >> 16) & 0xFF, (v >> 8) & 0xFF, v & 0xFF);
    return buf;
}

std::wstring DiagUiAccessLine()
{
    return g_diagUiAccess ? L"uiAccess: granted" : L"uiAccess: NOT granted";
}

std::wstring DiagDuiLine()
{
    if (!g_diagDuiEnabled)
        return L"dui70 hook: off (EnableDuiHook)";
    if (!g_diagDuiOk)
        return L"dui70 RVA: unresolved";
    wchar_t buf[64];
    ::wsprintfW(buf, L"dui70 RVA: 0x%08lX (ok)", g_diagDuiRva);
    return buf;
}

int WINAPI wWinMain(HINSTANCE hInstance, HINSTANCE, LPWSTR lpCmdLine, int)
{
    // Safety hatch: holding Shift while we start (e.g. at logon) makes us exit silently BEFORE
    // installing the global hook — a way out if a crash/conflict would otherwise hose explorer.
    if ((::GetAsyncKeyState(VK_SHIFT) & 0x8000) != 0)
        return 0;

    g_hInst = hInstance;
    ::SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);

    // Autostart launches us with /tray (stay quietly in the tray); a manual launch (no /tray) also
    // opens the Settings window once we are up.
    const bool trayMode = (lpCmdLine != nullptr && ::wcsstr(lpCmdLine, L"/tray") != nullptr);

    // Single instance: a second host would install a second global WH_CBT hook and double-load.
    // A manual relaunch surfaces the running instance's Settings; an autostart relaunch just exits.
    // The handle is intentionally kept for the process lifetime (freed on exit).
    ::CreateMutexW(nullptr, FALSE, L"Local\\WM_NIGHT_singleton");
    if (::GetLastError() == ERROR_ALREADY_EXISTS)
    {
        if (!trayMode)
            if (const HWND existing = ::FindWindowW(kWndClassName, nullptr))
                ::PostMessageW(existing, WM_COMMAND, IDM_SETTINGS, 0);
        return 0;
    }

    // Auto-run at logon. Only a manual Exit unregisters this; any other exit (Shift hatch, crash,
    // reboot) leaves it so we come back next logon.
    SetAutostart(true);

    const INITCOMMONCONTROLSEX icc{ sizeof(icc), ICC_STANDARD_CLASSES | ICC_LINK_CLASS };
    ::InitCommonControlsEx(&icc);
    umbra::initDarkMode();   // app-wide dark opt-in (also darkens our popup menu + message boxes)
    SettingsInit();          // COM apartment (STA) for the XAML-Islands Settings window

    g_taskbarCreated = ::RegisterWindowMessageW(L"TaskbarCreated");

    if (FAILED(::LoadIconMetric(hInstance, MAKEINTRESOURCEW(IDI_APPICON), LIM_SMALL, &g_trayIcon))
        || g_trayIcon == nullptr)
        g_trayIcon = ::LoadIconW(hInstance, MAKEINTRESOURCEW(IDI_APPICON));

    WNDCLASSEXW wc{};
    wc.cbSize        = sizeof(wc);
    wc.lpfnWndProc   = WndProc;
    wc.hInstance     = hInstance;
    wc.lpszClassName = kWndClassName;
    wc.hIcon         = ::LoadIconW(hInstance, MAKEINTRESOURCEW(IDI_APPICON));
    if (::RegisterClassExW(&wc) == 0)
        return 1;

    // Hidden, top-level (parent == nullptr) so it receives the TaskbarCreated broadcast; never
    // shown, and WS_EX_TOOLWINDOW keeps it off the taskbar / Alt-Tab. WM_CREATE adds the icon.
    g_hWnd = ::CreateWindowExW(WS_EX_TOOLWINDOW, kWndClassName, L"WM_NIGHT",
                               WS_OVERLAPPED, 0, 0, 0, 0, nullptr, nullptr, hInstance, nullptr);
    if (g_hWnd == nullptr)
        return 1;

    // Load the DLL (the global hook loads THIS copy into each target). Packaged builds stage
    // it to %LocalLow% first, since the in-package WindowsApps copy isn't readable by the targets.
    const std::wstring dllPath = ResolveDllPath();
    g_hookModule = ::LoadLibraryW(dllPath.c_str());
    if (g_hookModule == nullptr)
    {
        umbra::DarkMessageBox(g_hWnd, L"Could not load WM_NIGHThook.dll.",
                              L"WM_NIGHT", MB_OK | MB_ICONERROR);
        return 1;
    }

    const auto cbtProc = reinterpret_cast<HOOKPROC>(::GetProcAddress(g_hookModule, "UmbraCbtHook"));
    if (cbtProc == nullptr)
    {
        umbra::DarkMessageBox(g_hWnd, L"WM_NIGHThook.dll is missing UmbraCbtHook.",
                              L"WM_NIGHT", MB_OK | MB_ICONERROR);
        ::FreeLibrary(g_hookModule);
        return 1;
    }

    // Resolve dui70's Element::PaintBackground and hand it to the DLL before the hook goes in.
    HandOffDuiPaintBg();   // resolve the dui70 RVA (export-table lookup: instant, offline) and hand it off

    g_cbtHook = ::SetWindowsHookExW(WH_CBT, cbtProc, g_hookModule, 0);
    if (g_cbtHook == nullptr)
    {
        umbra::DarkMessageBox(g_hWnd, L"SetWindowsHookEx(WH_CBT) failed.",
                              L"WM_NIGHT", MB_OK | MB_ICONERROR);
        ::FreeLibrary(g_hookModule);
        return 1;
    }

    g_diagUiAccess = HasUiAccess();   // snapshot for the Settings diagnostics readout

    // A manual launch (no /tray) opens Settings immediately; autostart stays quiet in the tray.
    if (!trayMode)
        ShowSettingsWindow(g_hInst);

    MSG msg{};
    while (::GetMessageW(&msg, nullptr, 0, 0) > 0)
    {
        if (SettingsPreTranslateMessage(&msg))   // let an open XAML island consume keyboard/focus
            continue;
        ::TranslateMessage(&msg);
        ::DispatchMessageW(&msg);
    }

    // Teardown (Exit / WM_QUIT). Mirrors the old console path: unhook, release our DLL
    // reference, then bounce explorer so the copy still pinned in the shell is dropped and the
    // next launch loads a fresh DLL into a clean tree.
    ::UnhookWindowsHookEx(g_cbtHook);
    ::FreeLibrary(g_hookModule);
    if (g_trayIcon != nullptr)
        ::DestroyIcon(g_trayIcon);
    BounceExplorer();
    return static_cast<int>(msg.wParam);
}
