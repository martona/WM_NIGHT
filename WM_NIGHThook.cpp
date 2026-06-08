// SPDX-License-Identifier: MIT
//
// WM_NIGHThook.dll — injected dark-theming payload (WM_NIGHT harness).
//
// WM_NIGHT.exe installs ONE global WH_CBT hook whose proc lives here, so this DLL
// maps into target processes as early as their first window — before WM_NCCREATE.
// On the allowlist (regedit) we, once per process, init umbra + install the process-wide
// GetSysColor/uxtheme inline hooks (the proven ProcessColorHook/ThemeColorHook), and,
// once per thread, self-install WH_CALLWNDPROC[RET] on the current thread so umbra themes
// every window at creation (prepDarkModeForNewWindow on WM_NCCREATE, applyDarkToNewWindow
// on WM_CREATE).
//
// HCBT_CREATEWND fires before WM_NCCREATE, so the window that triggered our per-thread
// install is itself caught — even regedit's frame themes through the normal path. And the
// CBT hook's per-thread firing IS the thread coverage, so there is NO CreateThread detour
// and NO child-walk.

#include <windows.h>
#include <commctrl.h>
#include <richedit.h>
#include <dwmapi.h>
#include <strsafe.h>

#include <mutex>
#include <string>
#include <unordered_set>

#include <umbra.h>
#include <DarkMode.h>
#include "hook.h"   // setProcessWideColorHook / setProcessWideThemeColorHook (sample-hook)

namespace
{
    bool          g_isTarget = false;                 // are we inside a whitelisted process?
    HMODULE       g_self     = nullptr;               // this DLL (for SetWindowsHookEx hMod)
    INIT_ONCE     g_initOnce = INIT_ONCE_STATIC_INIT; // once-per-process umbra + inline-hook init
    volatile LONG g_pinned   = 0;                     // module self-pin latch

    thread_local bool t_threadHooked = false;         // CALLWNDPROC[RET] installed on this thread?
    thread_local bool t_theming      = false;         // re-entrancy guard (theming sends messages)

    // ---- TEMP DIAGNOSTIC: top-level theming trace -> <repo>\logs\WM_NIGHThook.log ----
    HANDLE           g_dbg     = INVALID_HANDLE_VALUE;
    CRITICAL_SECTION g_dbgCs{};
    INIT_ONCE        g_dbgOnce = INIT_ONCE_STATIC_INIT;

    std::mutex                       g_seenMutex;
    std::unordered_set<std::wstring> g_seenClasses;   // dedup the themed-class trace

    BOOL CALLBACK DbgInitOnce(PINIT_ONCE, PVOID, PVOID*) noexcept
    {
#if UMBRA_DIAG
        // One fixed repo log dir (see umbraLogPath in hook.h). FILE_APPEND_DATA so the global
        // hook's several processes — and a crash-and-restart — append rather than truncate each
        // other's capture; just delete the file for a clean run.
        wchar_t path[MAX_PATH];
        umbraLogPath(L"WM_NIGHThook.log", path, ARRAYSIZE(path));
        g_dbg = ::CreateFileW(path, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE,
                              nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
#endif
        ::InitializeCriticalSection(&g_dbgCs);
        return TRUE;
    }

    void Dbg(const wchar_t* line) noexcept
    {
        ::InitOnceExecuteOnce(&g_dbgOnce, DbgInitOnce, nullptr, nullptr);
        if (g_dbg == INVALID_HANDLE_VALUE)
            return;
        char u[512];
        const int n = ::WideCharToMultiByte(CP_UTF8, 0, line, -1, u, sizeof(u), nullptr, nullptr);
        if (n <= 1)
            return;
        ::EnterCriticalSection(&g_dbgCs);
        DWORD w = 0; ::WriteFile(g_dbg, u, static_cast<DWORD>(n - 1), &w, nullptr); ::FlushFileBuffers(g_dbg);
        ::LeaveCriticalSection(&g_dbgCs);
    }
    // ------------------------------------------------------------------------------------

    const wchar_t* BaseName(const wchar_t* path) noexcept
    {
        const wchar_t* base = path;
        for (const wchar_t* p = path; *p != L'\0'; ++p)
            if (*p == L'\\' || *p == L'/')
                base = p + 1;
        return base;
    }

    bool SameI(const wchar_t* a, const wchar_t* b) noexcept
    {
        return a != nullptr && b != nullptr &&
               ::CompareStringOrdinal(a, -1, b, -1, TRUE) == CSTR_EQUAL;
    }

    void InitProcessFlags() noexcept
    {
        wchar_t path[MAX_PATH]{};
        const DWORD n = ::GetModuleFileNameW(nullptr, path, ARRAYSIZE(path));
        const wchar_t* name = (n != 0 && n < ARRAYSIZE(path)) ? BaseName(path) : L"";
        g_isTarget = SameI(name, L"regedit.exe")  || 
                     SameI(name, L"regedt32.exe") ||
//                   SameI(name, L"umbra-sample.exe") ||
                     SameI(name, L"7zFM.exe") ||
                     SameI(name, L"7zG.exe") ||
                     SameI(name, L"spyxx_amd64.exe") ||
                     SameI(name, L"explorer.exe") ||
                     SameI(name, L"mmc.exe");
    }

    // Pin so we survive the host's UnhookWindowsHookEx: regedit keeps its theming (and our
    // per-thread CALLWNDPROC hooks stay valid) until it closes, never a dangling pointer.
    void PinModule() noexcept
    {
        if (::InterlockedCompareExchange(&g_pinned, 1, 0) != 0)
            return;
        HMODULE mod = nullptr;
        if (::GetModuleHandleExW(
                GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN,
                reinterpret_cast<LPCWSTR>(&PinModule), &mod) == FALSE)
            ::InterlockedExchange(&g_pinned, 0);
    }

    BOOL CALLBACK InitProcessOnce(PINIT_ONCE, PVOID, PVOID*) noexcept
    {
        umbra::initDarkMode();
        umbra::setDarkModeConfig(static_cast<UINT>(umbra::DarkModeType::dark));
        setProcessWideColorHook();        // GetSysColor / GetSysColorBrush inline (Detours)
        setProcessWideThemeColorHook();   // uxtheme Open/GetThemeColor/DrawThemeBackground (Detours)
        return TRUE;
    }

    // TEMP DIAGNOSTIC: log each DISTINCT window class we subclass (once), with its parent
    // class + top/child flag. In explorer this names the shell / input-experience windows
    // we shouldn't be touching (cross-ref the InputHost fail-fast). Dedup'd, so it's the
    // class universe, not a per-window flood.
    void LogThemedWindow(HWND hwnd) noexcept
    {
        wchar_t cls[96] = L"?"; ::GetClassNameW(hwnd, cls, ARRAYSIZE(cls));
        {
            std::lock_guard<std::mutex> lock(g_seenMutex);
            if (!g_seenClasses.insert(cls).second)
                return;   // class already logged
        }
        wchar_t parent[96] = L"-";
        const HWND hp = ::GetParent(hwnd);
        if (hp != nullptr) { ::GetClassNameW(hp, parent, ARRAYSIZE(parent)); }
        const bool top = (::GetWindowLongPtrW(hwnd, GWL_STYLE) & WS_CHILD) == 0;
        wchar_t line[320];
        ::StringCchPrintfW(line, ARRAYSIZE(line), L"themed %s %-34s parent=%s\r\n",
                           top ? L"[TOP]" : L"     ", cls, parent);
        Dbg(line);
    }

    // Classes we must NOT touch: XAML/WinUI/Composition input islands + hidden, message-only,
    // input, and system-plumbing windows. Subclassing these and perturbing their
    // WM_CTLCOLOR/NOTIFY/ERASE either fail-fasts (InputHost / CTF input site) or is simply
    // pointless (no visible UI). None are classic controls umbra themes. Grown empirically per
    // target (explorer's InputSite; mmc's CTF / clipboard / GDI+ / .NET broadcast windows).
    // The cleaner long-term shape is a per-target allow-list; this deny-list is the interim.
    bool IsThemingBlacklisted(HWND hwnd) noexcept
    {
        wchar_t cls[128];
        if (::GetClassNameW(hwnd, cls, ARRAYSIZE(cls)) == 0)
            return false;

        // Prefix matches (variable per-instance suffixes).
        static const wchar_t* const kPrefixes[] = {
            L"Windows.UI.",                // XAML/WinUI/Composition islands (InputHost fail-fast)
            L".NET-BroadcastEventWindow",  // .NET runtime WM_SETTINGCHANGE broadcast window
        };
        for (const wchar_t* p : kPrefixes)
            if (::wcsncmp(cls, p, ::wcslen(p)) == 0)
                return true;

        // Exact matches.
        static const wchar_t* const kExacts[] = {
            L"GDI+ Hook Window Class",
            L"CLIPBRDWNDCLASS",
            L"CicMarshalWndClass",
            L"SystemUserAdapterWindowClass",
            L"SnapMessageWindow",
            L"MSCTFIME UI",
            L"IME",
            L"OperationStatusWindow",
            L"OleMainThreadWndClass",      // OLE/COM hidden marshalling window
            L"Event Viewer Snapin Synch",  // mmc Event Viewer snap-in hidden sync window
        };
        for (const wchar_t* e : kExacts)
            if (::wcscmp(cls, e) == 0)
                return true;

        return false;
    }

    // A recognized leaf control (one umbra class-themes via its own subclass). These must
    // NOT get the parent-helper subclasses (ctl-color / notify-customdraw) — those belong on
    // dialog/frame HOSTS. The control's own subclass handles its internal children; its items'
    // custom-draw is handled by its host's helper. Over-stacking the helpers on a control is
    // redundant and, on an MFC host (mmc), faults during the WM_NCDESTROY reflection.
    bool IsLeafControl(HWND hwnd) noexcept
    {
        wchar_t cls[64];
        if (::GetClassNameW(hwnd, cls, ARRAYSIZE(cls)) == 0)
            return false;
        static const wchar_t* const kControls[] = {
            WC_BUTTON, WC_STATIC, WC_COMBOBOX, WC_EDIT, WC_LISTBOX, WC_LISTVIEW,
            WC_TREEVIEW, WC_TABCONTROL, WC_SCROLLBAR, WC_COMBOBOXEX, WC_LINK,
            REBARCLASSNAME, TOOLBARCLASSNAME, UPDOWN_CLASS, STATUSCLASSNAME,
            PROGRESS_CLASS, TRACKBAR_CLASS, RICHEDIT_CLASS, MSFTEDIT_CLASS,
            L"SizeableRebar",
        };
        for (const wchar_t* c : kControls)
            if (::CompareStringOrdinal(cls, -1, c, -1, TRUE) == CSTR_EQUAL)
                return true;
        static const wchar_t* const kWinForms[] = {
            L"WindowsForms10.BUTTON.", 
            L"WindowsForms10.STATIC.", 
            L"WindowsForms10.COMBOBOX.", 
            L"WindowsForms10.SysTabControl32.", 
            L"WindowsForms10.SCROLLBAR.", 
        };
        for (const wchar_t* c : kWinForms)
            if (DarkModeHelper::check_prefix(cls, c))
                return true;
        return false;
    }

    // EnumChildWindows callback: clear the flag (and stop) on the first child that is
    // neither a static nor a button.
    BOOL CALLBACK MsgBoxShapeProc(HWND child, LPARAM param)
    {
        wchar_t cls[32];
        if (::GetClassNameW(child, cls, ARRAYSIZE(cls)) == 0)
            return TRUE;
        const bool staticOrButton =
            ::CompareStringOrdinal(cls, -1, WC_STATIC, -1, TRUE) == CSTR_EQUAL ||
            ::CompareStringOrdinal(cls, -1, WC_BUTTON, -1, TRUE) == CSTR_EQUAL;
        if (!staticOrButton)
        {
            *reinterpret_cast<bool*>(param) = false;
            return FALSE;
        }
        return TRUE;
    }

    // A classic MessageBox is a #32770 whose only children are static text/icon and push
    // buttons — no Edit/Combo/List/Tab/etc. Its lower button band is filled from a private
    // brush in WM_PAINT that no colour/theme hook reaches, so it needs umbra's client
    // backfill — but the backfill clips only child HWNDs, so on a dialog that paints content
    // straight onto its own surface (a wizard / property-sheet header band) it would blank
    // that content. Scoping to message-box shape keeps the backfill where it is safe. The
    // children exist by WM_INITDIALOG (not yet at WM_CREATE), so classify there.
    bool IsMessageBoxShaped(HWND hwnd)
    {
        wchar_t cls[16];
        if (::GetClassNameW(hwnd, cls, ARRAYSIZE(cls)) == 0)
            return false;
        if (::CompareStringOrdinal(cls, -1, L"#32770", -1, TRUE) != CSTR_EQUAL)
            return false;

        bool onlyStaticButton = true;
        ::EnumChildWindows(hwnd, MsgBoxShapeProc, reinterpret_cast<LPARAM>(&onlyStaticButton));
        return onlyStaticButton;
    }

    // Per-window theming — identical to sample-hook's AutoDarkMode, minus the EXE-only
    // self-hook/CreateThread machinery (the CBT bootstrap below replaces it).
    LRESULT CALLBACK CallWndProc(int code, WPARAM wParam, LPARAM lParam)
    {
        if (code == HC_ACTION && !t_theming)
        {
            const auto* info = reinterpret_cast<const CWPSTRUCT*>(lParam);
            if (info->message == WM_NCCREATE && info->hwnd != nullptr
                && !IsThemingBlacklisted(info->hwnd))
            {
                t_theming = true;
                umbra::prepDarkModeForNewWindow(info->hwnd);   // early AllowDarkModeForWindow
                t_theming = false;
            }
            else if (info->message == WM_DESTROY && info->hwnd != nullptr)
            {
                // Pull umbra's subclasses BEFORE the window proc handles WM_DESTROY — so we're
                // out of the chain before an MFC host's nested WM_NCDESTROY view-teardown
                // reflects through us and derefs a stale -1 (mmc). Each remove is a no-op when
                // the subclass isn't present.
                umbra::removeListViewCtrlSubclass(info->hwnd);
                umbra::removeWindowCtlColorSubclass(info->hwnd);
                umbra::removeWindowNotifyCustomDrawSubclass(info->hwnd);
                umbra::removeWindowMenuBarSubclass(info->hwnd);
                umbra::removeWindowEraseBgSubclass(info->hwnd);
                umbra::removeWindowBackfillSubclass(info->hwnd);
            }
        }
        return ::CallNextHookEx(nullptr, code, wParam, lParam);
    }

    LRESULT CALLBACK CallWndRetProc(int code, WPARAM wParam, LPARAM lParam)
    {
        if (code == HC_ACTION && !t_theming)
        {
            const auto* info = reinterpret_cast<const CWPRETSTRUCT*>(lParam);
            if (info->message == WM_CREATE && info->hwnd != nullptr
                && !IsThemingBlacklisted(info->hwnd))
            {
                t_theming = true;
                // Leaf controls: class-specific subclass only (no parent-helper ctl-color/notify).
                // Hosts (dialogs/frames/containers): full treatment.
                if (IsLeafControl(info->hwnd))
                    umbra::setDarkChildCtrl(info->hwnd);
                else
                    umbra::applyDarkToNewWindow(info->hwnd);
                t_theming = false;
                LogThemedWindow(info->hwnd);
            }
            else if (info->message == WM_INITDIALOG && info->hwnd != nullptr
                     && !IsThemingBlacklisted(info->hwnd)
                     && IsMessageBoxShaped(info->hwnd))
            {
                // Classic MessageBox: scope umbra's client backfill to message-box-shaped
                // dialogs (their children exist now, not yet at WM_CREATE). Installed here,
                // after the WM_CREATE applyDarkToNewWindow pass, so it sits OUTERMOST and its
                // WM_PAINT runs DefSubclassProc first, then fills the leftover light button band.
                umbra::setWindowBackfillSubclass(info->hwnd);
            }
            else if (info->message == WM_ACTIVATE && info->hwnd != nullptr
                     && (::GetWindowLongPtrW(info->hwnd, GWL_STYLE) & WS_CHILD) == 0
                     && !IsThemingBlacklisted(info->hwnd))
            {
                // Some dialogs (comdlg32's Save/Export) clear the dark title-bar attribute
                // during their own init — after our WM_CREATE pass — and repaint the caption
                // light. Re-assert once, when we find it cleared, on activation (after init).
                BOOL dark = FALSE;
                ::DwmGetWindowAttribute(info->hwnd, 20 /*DWMWA_USE_IMMERSIVE_DARK_MODE*/,
                                        &dark, sizeof(dark));
                if (!dark && ::GetPropW(info->hwnd, L"umbra.capFix") == nullptr)
                {
                    ::SetPropW(info->hwnd, L"umbra.capFix", reinterpret_cast<HANDLE>(1));
                    t_theming = true;
                    umbra::setDarkTitleBarEx(info->hwnd, true);
                    umbra::redrawWindowFrame(info->hwnd);
                    t_theming = false;

                    wchar_t cls[80] = L"?"; ::GetClassNameW(info->hwnd, cls, ARRAYSIZE(cls));
                    wchar_t line[256];
                    ::StringCchPrintfW(line, ARRAYSIZE(line),
                                       L"WM_ACTIVATE re-assert dark caption (was cleared): %s\r\n", cls);
                    Dbg(line);
                }
            }
        }
        return ::CallNextHookEx(nullptr, code, wParam, lParam);
    }

    // Install the per-window theming hooks on the CURRENT thread, once. Driven by the CBT
    // hook's per-thread firing (every window-creating thread fires CBT) — this is what
    // retires the CreateThread detour + the EXE self-hook trick.
    void EnsureThreadHooks() noexcept
    {
        if (t_threadHooked)
            return;
        t_threadHooked = true;
        const DWORD tid = ::GetCurrentThreadId();
        ::SetWindowsHookExW(WH_CALLWNDPROC,    CallWndProc,    g_self, tid);
        ::SetWindowsHookExW(WH_CALLWNDPROCRET, CallWndRetProc, g_self, tid);
    }

    // Attach the dui70 Element::PaintBackground hook once it can be — i.e. once dui70 is mapped
    // in this process. Unlike GetSysColor/uxtheme (loaded early), dui70 loads LAZILY (only when
    // the shell shows DUI — Control Panel, the navigation pane, ...), so a one-shot at init
    // would miss it. We retry on each window-creation CBT fire until it attaches, then latch.
    // A permanent failure (no host RVA / stamp mismatch) makes setProcessWideDuiPaintHook a
    // cheap flag-check no-op, so retrying costs nothing.
    void EnsureDuiPaintHook() noexcept
    {
        static volatile LONG s_done = 0;
        if (::InterlockedCompareExchange(&s_done, 0, 0) != 0)
            return;
        if (setProcessWideDuiPaintHook())   // true once attached (transient false while no dui70)
            ::InterlockedExchange(&s_done, 1);
    }
}

extern "C" __declspec(dllexport)
LRESULT CALLBACK UmbraCbtHook(int code, WPARAM wParam, LPARAM lParam)
{
    // First CBT fire on a thread is its first window's HCBT_CREATEWND (before WM_NCCREATE);
    // bootstrap process + thread there so that very window is caught by CallWndProc.
    if (g_isTarget && code >= 0 && !t_theming)
    {
        ::InitOnceExecuteOnce(&g_initOnce, InitProcessOnce, nullptr, nullptr);
        PinModule();
        EnsureThreadHooks();
        EnsureDuiPaintHook();   // attaches when dui70 (lazily) loads — e.g. Control Panel opens
    }
    return ::CallNextHookEx(nullptr, code, wParam, lParam);
}

BOOL APIENTRY DllMain(HMODULE module, DWORD reason, LPVOID)
{
    if (reason == DLL_PROCESS_ATTACH)
    {
        g_self = module;
        ::DisableThreadLibraryCalls(module);
        InitProcessFlags();
    }
    return TRUE;
}
