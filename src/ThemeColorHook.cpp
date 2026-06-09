// SPDX-License-Identifier: MIT
//
// ThemeColorHook.cpp — process-wide uxtheme dark hooks (Tier 1).
//   (umbra-hook harness — the application / interception half.)
//
// DUI (dui70.dll) paints via uxtheme theme parts, so the light residue on DUI
// surfaces (the file-dialog address/search band, the Advanced Security label
// band, ...) comes from uxtheme — not from GetSysColor and not from any
// subclassable control. The GetThemeColor probe showed app dark mode already
// darkens most DUI *fills* (GetThemeColor returns dark); the residual light bands
// are painted by DrawThemeBackground from theme parts with no dark variant, which
// no colour query can reach.
//
// This file:
//   * Hooks OpenThemeData / OpenThemeDataEx / OpenThemeDataForDpi / OpenNcThemeData
//     (ordinal 49 — the shell's DUI opens NC themes through it, bypassing the others)
//     + CloseThemeData, to maintain an HTHEME -> class-list map (the only way to name
//     a theme). A handle drawn with but never seen opened => opened via a path we miss.
//   * Hooks GetThemeColor: pass-through + logs (class, part, state, prop, colour)
//     once per distinct query. (The override hook-up is marked below, for when we
//     flip the now-dark bands' text colours.)
//   * Hooks DrawThemeBackground / DrawThemeBackgroundEx: logs (class, part, state,
//     size), and where umbra::darkThemeBackground owns the decision, replaces the
//     themed (light) part with a flat dark FillRect. The library keeps the decision
//     (which class/part -> dark); this file keeps the interception.
//
// Every distinct query/draw is logged once to themecolor.log — read it after
// exercising the DUI dialogs to extend umbra::darkThemeBackground to the exact
// parts (e.g. the command-module band) that still paint light.

#include <windows.h>
#include <uxtheme.h>
#include <detours/detours.h>

#include <shared_mutex>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <strsafe.h>

#include "umbra.h"
#include "hook.h"

#pragma comment(lib, "detours.lib")
#pragma comment(lib, "uxtheme.lib")

namespace
{
    using fnOpenThemeData       = HTHEME  (WINAPI*)(HWND, LPCWSTR);
    using fnOpenThemeDataEx     = HTHEME  (WINAPI*)(HWND, LPCWSTR, DWORD);
    using fnOpenThemeDataForDpi = HTHEME  (WINAPI*)(HWND, LPCWSTR, UINT);
    using fnOpenNcThemeData     = HTHEME  (WINAPI*)(HWND, LPCWSTR);   // uxtheme ordinal 49
    using fnCloseThemeData      = HRESULT (WINAPI*)(HTHEME);
    using fnGetThemeColor       = HRESULT (WINAPI*)(HTHEME, int, int, int, COLORREF*);
    using fnDrawThemeBackground   = HRESULT (WINAPI*)(HTHEME, HDC, int, int, LPCRECT, LPCRECT);
    using fnDrawThemeBackgroundEx = HRESULT (WINAPI*)(HTHEME, HDC, int, int, LPCRECT, const DTBGOPTS*);

    // Trampolines; Detours rewrites these to the relocated originals on attach.
    fnOpenThemeData         g_realOpenThemeData         = ::OpenThemeData;
    fnOpenThemeDataEx       g_realOpenThemeDataEx       = ::OpenThemeDataEx;
    fnOpenThemeDataForDpi   g_realOpenThemeDataForDpi   = ::OpenThemeDataForDpi;
    fnOpenNcThemeData       g_realOpenNcThemeData       = nullptr;   // resolved by ordinal at install
    fnCloseThemeData        g_realCloseThemeData        = ::CloseThemeData;
    fnGetThemeColor         g_realGetThemeColor         = ::GetThemeColor;
    fnDrawThemeBackground   g_realDrawThemeBackground   = ::DrawThemeBackground;
    fnDrawThemeBackgroundEx g_realDrawThemeBackgroundEx = ::DrawThemeBackgroundEx;

    bool g_installed = false;

    // ---- HTHEME -> class-list map ------------------------------------------
    // The class list a theme was opened with is the only handle we have on what
    // a GetThemeColor call is actually theming.
    std::shared_mutex                        g_mapMutex;
    std::unordered_map<HTHEME, std::wstring> g_themeClasses;

    void RememberTheme(HTHEME hTheme, LPCWSTR classList)
    {
        if (hTheme == nullptr || classList == nullptr)
            return;
        std::unique_lock lock(g_mapMutex);
        g_themeClasses[hTheme] = classList;
    }

    void ForgetTheme(HTHEME hTheme)
    {
        if (hTheme == nullptr)
            return;
        std::unique_lock lock(g_mapMutex);
        g_themeClasses.erase(hTheme);
    }

    std::wstring ClassOf(HTHEME hTheme)
    {
        std::shared_lock lock(g_mapMutex);
        const auto it = g_themeClasses.find(hTheme);
        return it != g_themeClasses.end() ? it->second : std::wstring(L"?");
    }

    // ---- solid-brush cache for the dark fills -------------------------------
    // FillRect needs a brush; cache one per fill colour for the lifetime of the
    // hook (rebuilt only across install cycles).
    std::mutex                           g_brushMutex;
    std::unordered_map<COLORREF, HBRUSH> g_brushes;

    HBRUSH BrushFor(COLORREF clr)
    {
        std::lock_guard<std::mutex> lock(g_brushMutex);
        const auto it = g_brushes.find(clr);
        if (it != g_brushes.end())
            return it->second;
        const HBRUSH b = ::CreateSolidBrush(clr);
        g_brushes[clr] = b;
        return b;
    }

    void DestroyBrushes()
    {
        std::lock_guard<std::mutex> lock(g_brushMutex);
        for (const auto& kv : g_brushes)
            ::DeleteObject(kv.second);
        g_brushes.clear();
    }

    // ---- dedup logger -> themecolor.log ------------------------------------
    HANDLE                           g_log = INVALID_HANDLE_VALUE;
    std::mutex                       g_logMutex;
    std::unordered_set<std::wstring> g_seen;

    void OpenLog()
    {
        std::lock_guard<std::mutex> lock(g_logMutex);
        if (g_log != INVALID_HANDLE_VALUE)
            return;
#if UMBRA_DIAG
        wchar_t path[MAX_PATH];
        umbraLogPath(L"themecolor.log", path, ARRAYSIZE(path));

        g_log = ::CreateFileW(path, GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
            nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        g_seen.clear();

        if (g_log != INVALID_HANDLE_VALUE)
        {
            static const char hdr[] =
                "themecolor.log - uxtheme hooks (umbra-hook)\r\n"
                "  OPEN  <api> h=<HTHEME> <class>                 theme opened\r\n"
                "  CLOSE h=<HTHEME>                               theme closed\r\n"
                "  GTC   <class> part/state/prop clr=COLORREF(00BBGGRR) hr\r\n"
                "  DTB|DTBEx h=<HTHEME> <class> part/state @(x,y) WxH   bg draw\r\n"
                "  (distinct lines once; the handle traces open/close/reuse)\r\n"
                "----------------------------------------------------------------\r\n";
            DWORD w = 0;
            ::WriteFile(g_log, hdr, static_cast<DWORD>(sizeof(hdr) - 1), &w, nullptr);
        }
#endif
    }

    void CloseLog()
    {
        std::lock_guard<std::mutex> lock(g_logMutex);
        if (g_log != INVALID_HANDLE_VALUE)
        {
            ::CloseHandle(g_log);
            g_log = INVALID_HANDLE_VALUE;
        }
        g_seen.clear();
    }

    // Writes `line` once per distinct `key`.
    void LogOnce(const std::wstring& key, const wchar_t* line)
    {
        std::lock_guard<std::mutex> lock(g_logMutex);
        if (g_log == INVALID_HANDLE_VALUE)
            return;
        if (!g_seen.insert(key).second)
            return;

        char u[600];
        const int n = ::WideCharToMultiByte(CP_UTF8, 0, line, -1, u, sizeof(u), nullptr, nullptr);
        if (n <= 1)
            return;
        DWORD w = 0;
        ::WriteFile(g_log, u, static_cast<DWORD>(n - 1), &w, nullptr);
        ::FlushFileBuffers(g_log);
    }

    void LogThemeOpen(HTHEME h, LPCWSTR classList, const wchar_t* api)
    {
        if (classList == nullptr)
            return;
        wchar_t key[400];
        ::StringCchPrintfW(key, ARRAYSIZE(key), L"O|%s|%p|%s", api, reinterpret_cast<void*>(h), classList);
        wchar_t line[400];
        ::StringCchPrintfW(line, ARRAYSIZE(line), L"OPEN  %-8s h=%p  %s\r\n",
            api, reinterpret_cast<void*>(h), classList);
        LogOnce(key, line);
    }

    void LogThemeClose(HTHEME h)
    {
        wchar_t key[48];
        ::StringCchPrintfW(key, ARRAYSIZE(key), L"X|%p", reinterpret_cast<void*>(h));
        wchar_t line[64];
        ::StringCchPrintfW(line, ARRAYSIZE(line), L"CLOSE h=%p\r\n", reinterpret_cast<void*>(h));
        LogOnce(key, line);
    }

    void LogThemeColor(HTHEME hTheme, int part, int state, int prop, COLORREF clr, HRESULT hr)
    {
        const std::wstring cls = ClassOf(hTheme);
        wchar_t key[320];
        ::StringCchPrintfW(key, ARRAYSIZE(key), L"C|%s|%d|%d|%d", cls.c_str(), part, state, prop);
        wchar_t line[400];
        ::StringCchPrintfW(line, ARRAYSIZE(line),
            L"GTC  %-34s part=%-4d state=%-3d prop=%-5d clr=%06lX hr=0x%08lX\r\n",
            cls.c_str(), part, state, prop,
            static_cast<unsigned long>(clr & 0x00FFFFFF), static_cast<unsigned long>(hr));
        LogOnce(key, line);
    }

    // Diagnostic for the GetThemeColor override path: one line per theme "hole" we fill
    // (a failing query turned into a palette colour). Cheap — only the rare failed query
    // that umbra::darkThemeColor claims reaches here.
    void LogThemeColorOverride(const std::wstring& cls, int part, int state, int prop, COLORREF clr, HRESULT origHr)
    {
        wchar_t key[320];
        ::StringCchPrintfW(key, ARRAYSIZE(key), L"CO|%s|%d|%d|%d", cls.c_str(), part, state, prop);
        wchar_t line[400];
        ::StringCchPrintfW(line, ARRAYSIZE(line),
            L"GTC-OVR  %-30s part=%-4d state=%-3d prop=%-5d -> %06lX  (was hr=0x%08lX)\r\n",
            cls.c_str(), part, state, prop,
            static_cast<unsigned long>(clr & 0x00FFFFFF), static_cast<unsigned long>(origHr));
        LogOnce(key, line);
    }

    void LogThemeDraw(HTHEME hTheme, const std::wstring& cls, int part, int state, LPCRECT rc, const wchar_t* api, HDC hdc)
    {
        const int l   = (rc != nullptr) ? static_cast<int>(rc->left) : 0;
        const int t   = (rc != nullptr) ? static_cast<int>(rc->top) : 0;
        const int w   = (rc != nullptr) ? static_cast<int>(rc->right - rc->left) : -1;
        const int hgt = (rc != nullptr) ? static_cast<int>(rc->bottom - rc->top) : -1;

        // Attribute the draw to its target window, so a light band maps to a class we can fix
        // (the seam, the Actions container, real-vs-OCX scrollbars). A memory / double-buffered
        // DC has no owning window -> "memDC". Also the only handle we get for a theme opened
        // before our hook (logged class "?").
        wchar_t wnd[64] = L"memDC";
        const HWND hwnd = ::WindowFromDC(hdc);
        if (hwnd != nullptr)
            ::GetClassNameW(hwnd, wnd, ARRAYSIZE(wnd));

        wchar_t key[420];
        ::StringCchPrintfW(key, ARRAYSIZE(key), L"D|%s|%p|%s|%d|%d|%s", api,
            reinterpret_cast<void*>(hTheme), cls.c_str(), part, state, wnd);
        wchar_t line[520];
        ::StringCchPrintfW(line, ARRAYSIZE(line),
            L"%-5s h=%p %-30s part=%-4d state=%-3d @(%d,%d) %dx%d  wnd=%s\r\n",
            api, reinterpret_cast<void*>(hTheme), cls.c_str(), part, state, l, t, w, hgt, wnd);
        LogOnce(key, line);
    }

    // ---- hook bodies --------------------------------------------------------
    HTHEME WINAPI HookOpenThemeData(HWND hwnd, LPCWSTR classList)
    {
        const HTHEME h = g_realOpenThemeData(hwnd, classList);
        RememberTheme(h, classList);
        LogThemeOpen(h, classList, L"Data");
        return h;
    }

    HTHEME WINAPI HookOpenThemeDataEx(HWND hwnd, LPCWSTR classList, DWORD flags)
    {
        const HTHEME h = g_realOpenThemeDataEx(hwnd, classList, flags);
        RememberTheme(h, classList);
        LogThemeOpen(h, classList, L"DataEx");
        return h;
    }

    HTHEME WINAPI HookOpenThemeDataForDpi(HWND hwnd, LPCWSTR classList, UINT dpi)
    {
        const HTHEME h = g_realOpenThemeDataForDpi(hwnd, classList, dpi);
        RememberTheme(h, classList);
        LogThemeOpen(h, classList, L"DataDpi");
        return h;
    }

    HTHEME WINAPI HookOpenNcThemeData(HWND hwnd, LPCWSTR classList)
    {
        const HTHEME h = g_realOpenNcThemeData(hwnd, classList);
        RememberTheme(h, classList);
        LogThemeOpen(h, classList, L"NcData");
        return h;
    }

    HRESULT WINAPI HookCloseThemeData(HTHEME hTheme)
    {
        // Diagnostic baseline: log the close (with handle) and erase the mapping.
        // The handle in the open/close/draw lines lets us trace reuse across a
        // dialog's close/reopen and settle whether the reopen-light is an erase+reuse
        // or a theme opened via a path we don't hook — before fixing it for real.
        LogThemeClose(hTheme);
        const HRESULT hr = g_realCloseThemeData(hTheme);
        ForgetTheme(hTheme);
        return hr;
    }

    HRESULT WINAPI HookGetThemeColor(HTHEME hTheme, int iPartId, int iStateId, int iPropId, COLORREF* pColor)
    {
        const HRESULT hr = g_realGetThemeColor(hTheme, iPartId, iStateId, iPropId, pColor);
        LogThemeColor(hTheme, iPartId, iStateId, iPropId,
            (SUCCEEDED(hr) && pColor != nullptr) ? *pColor : 0x00FFFFFF, hr);

        // Fill theme "holes": when a colour query fails, the caller falls back to a colour
        // our GetSysColor hook can't reach (e.g. comctl's themed list-item text → dark-on-
        // dark in the breadcrumb ListviewPopup). umbra owns the decision; we carry it out
        // and log each hole we patch, so a single run shows whether the lever fired.
        if (pColor != nullptr)
        {
            const std::wstring cls = ClassOf(hTheme);
            COLORREF over{};
            if (umbra::darkThemeColor(cls.c_str(), iPartId, iStateId, iPropId, hr, over))
            {
                LogThemeColorOverride(cls, iPartId, iStateId, iPropId, over, hr);
                *pColor = over;
                return S_OK;
            }
        }
        return hr;
    }

    // Replace a themed background draw with a flat dark fill when UMBRA owns the
    // decision for this (class, part, state); otherwise draw normally. This is the
    // lever for DUI bands uxtheme paints light (no dark variant) — unreachable by
    // any colour query.
    // A theme opened before our hook installs is unmapped (ClassOf -> "?"). The common case is a
    // frame's WINDOW (NC) theme, opened process-wide at startup; its caption parts (WP_CAPTION=1 /
    // WP_MAXCAPTION=5) paint the light strip that shows as mmc's white MDI seam. When such a draw
    // targets a real caption-bearing window, name it "Window" so umbra::darkThemeBackground can
    // darken it. The WS_CAPTION gate excludes the other "?" handles (tree/list items, menus,
    // tooltips — none of which are caption windows), so the inference can't misfire onto them.
    std::wstring ResolveClass(HTHEME hTheme, HDC hdc, int partId)
    {
        std::wstring cls = ClassOf(hTheme);
        if (cls == L"?" && (partId == 1 || partId == 5))
        {
            const HWND w = ::WindowFromDC(hdc);
            if (w != nullptr && (::GetWindowLongPtrW(w, GWL_STYLE) & WS_CAPTION) == WS_CAPTION)
                cls = L"Window";
        }
        return cls;
    }

    HRESULT WINAPI HookDrawThemeBackground(HTHEME hTheme, HDC hdc, int iPartId, int iStateId,
        LPCRECT pRect, LPCRECT pClipRect)
    {
        const std::wstring cls = ResolveClass(hTheme, hdc, iPartId);
        LogThemeDraw(hTheme, cls, iPartId, iStateId, pRect, L"DTB", hdc);

        COLORREF fill{};
        if (pRect != nullptr && umbra::darkThemeBackground(cls.c_str(), iPartId, iStateId, fill))
        {
            ::FillRect(hdc, pRect, BrushFor(fill));
            umbra::paintDarkThemeEdge(hdc, cls.c_str(), iPartId, iStateId, *pRect);
            return S_OK;
        }
        return g_realDrawThemeBackground(hTheme, hdc, iPartId, iStateId, pRect, pClipRect);
    }

    HRESULT WINAPI HookDrawThemeBackgroundEx(HTHEME hTheme, HDC hdc, int iPartId, int iStateId,
        LPCRECT pRect, const DTBGOPTS* pOptions)
    {
        const std::wstring cls = ResolveClass(hTheme, hdc, iPartId);
        LogThemeDraw(hTheme, cls, iPartId, iStateId, pRect, L"DTBEx", hdc);

        COLORREF fill{};
        if (pRect != nullptr && umbra::darkThemeBackground(cls.c_str(), iPartId, iStateId, fill))
        {
            ::FillRect(hdc, pRect, BrushFor(fill));
            umbra::paintDarkThemeEdge(hdc, cls.c_str(), iPartId, iStateId, *pRect);
            return S_OK;
        }
        return g_realDrawThemeBackgroundEx(hTheme, hdc, iPartId, iStateId, pRect, pOptions);
    }
}

bool setProcessWideThemeColorHook() noexcept
{
    if (g_installed)
        return true;

    OpenLog();

    // OpenNcThemeData is exported only by ordinal (49); resolve it so we can hook it
    // too. The shell's DUI opens non-client themes through it — invisible to the
    // OpenThemeData* hooks — which is why those draws showed up unnamed ("?").
    g_realOpenNcThemeData = reinterpret_cast<fnOpenNcThemeData>(
        ::GetProcAddress(::GetModuleHandleW(L"uxtheme.dll"), MAKEINTRESOURCEA(49)));

    if (::DetourTransactionBegin() != NO_ERROR)
        return false;
    ::DetourUpdateThread(::GetCurrentThread());

    LONG err = ::DetourAttach(reinterpret_cast<PVOID*>(&g_realOpenThemeData),
                              reinterpret_cast<PVOID>(HookOpenThemeData));
    if (err == NO_ERROR)
        err = ::DetourAttach(reinterpret_cast<PVOID*>(&g_realOpenThemeDataEx),
                             reinterpret_cast<PVOID>(HookOpenThemeDataEx));
    if (err == NO_ERROR)
        err = ::DetourAttach(reinterpret_cast<PVOID*>(&g_realOpenThemeDataForDpi),
                             reinterpret_cast<PVOID>(HookOpenThemeDataForDpi));
    if (err == NO_ERROR && g_realOpenNcThemeData != nullptr)
        err = ::DetourAttach(reinterpret_cast<PVOID*>(&g_realOpenNcThemeData),
                             reinterpret_cast<PVOID>(HookOpenNcThemeData));
    if (err == NO_ERROR)
        err = ::DetourAttach(reinterpret_cast<PVOID*>(&g_realCloseThemeData),
                             reinterpret_cast<PVOID>(HookCloseThemeData));
    if (err == NO_ERROR)
        err = ::DetourAttach(reinterpret_cast<PVOID*>(&g_realGetThemeColor),
                             reinterpret_cast<PVOID>(HookGetThemeColor));
    if (err == NO_ERROR)
        err = ::DetourAttach(reinterpret_cast<PVOID*>(&g_realDrawThemeBackground),
                             reinterpret_cast<PVOID>(HookDrawThemeBackground));
    if (err == NO_ERROR)
        err = ::DetourAttach(reinterpret_cast<PVOID*>(&g_realDrawThemeBackgroundEx),
                             reinterpret_cast<PVOID>(HookDrawThemeBackgroundEx));

    if (err != NO_ERROR)
    {
        ::DetourTransactionAbort();
        CloseLog();
        return false;
    }

    err = ::DetourTransactionCommit();
    g_installed = (err == NO_ERROR);
    if (!g_installed)
        CloseLog();
    return g_installed;
}

void unsetProcessWideThemeColorHook() noexcept
{
    if (!g_installed)
        return;

    if (::DetourTransactionBegin() == NO_ERROR)
    {
        ::DetourUpdateThread(::GetCurrentThread());
        ::DetourDetach(reinterpret_cast<PVOID*>(&g_realOpenThemeData),
                       reinterpret_cast<PVOID>(HookOpenThemeData));
        ::DetourDetach(reinterpret_cast<PVOID*>(&g_realOpenThemeDataEx),
                       reinterpret_cast<PVOID>(HookOpenThemeDataEx));
        ::DetourDetach(reinterpret_cast<PVOID*>(&g_realOpenThemeDataForDpi),
                       reinterpret_cast<PVOID>(HookOpenThemeDataForDpi));
        if (g_realOpenNcThemeData != nullptr)
            ::DetourDetach(reinterpret_cast<PVOID*>(&g_realOpenNcThemeData),
                           reinterpret_cast<PVOID>(HookOpenNcThemeData));
        ::DetourDetach(reinterpret_cast<PVOID*>(&g_realCloseThemeData),
                       reinterpret_cast<PVOID>(HookCloseThemeData));
        ::DetourDetach(reinterpret_cast<PVOID*>(&g_realGetThemeColor),
                       reinterpret_cast<PVOID>(HookGetThemeColor));
        ::DetourDetach(reinterpret_cast<PVOID*>(&g_realDrawThemeBackground),
                       reinterpret_cast<PVOID>(HookDrawThemeBackground));
        ::DetourDetach(reinterpret_cast<PVOID*>(&g_realDrawThemeBackgroundEx),
                       reinterpret_cast<PVOID>(HookDrawThemeBackgroundEx));
        ::DetourTransactionCommit();
    }

    g_installed = false;
    DestroyBrushes();
    CloseLog();
}
