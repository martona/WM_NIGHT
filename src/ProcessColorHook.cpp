// SPDX-License-Identifier: MIT
//
// ProcessColorHook.cpp — process-wide GetSysColor / GetSysColorBrush hook.
//   (umbra-hook harness — the application / interception half.)
//
// The mechanism that used to live in the umbra library: inline-hook the user32
// exports GetSysColor and GetSysColorBrush (via Microsoft Detours) so every
// caller in the process that goes through those exports receives UMBRA's dark
// palette. The colour DECISION is the library's (umbra::darkSysColor); this file
// owns only the interception, the trampolines, and the brush cache — so the
// Detours dependency stays out of umbra.
//
// Scope & caveats:
//  * Hooking the export only catches code that CALLS the export. user32's own
//    non-client painting and DefWindowProc fills read the shared sys-color
//    table directly and are unaffected; uxtheme/theme-part drawing gets colors
//    from theme parts, not these APIs.
//  * GetSysColor is context-free (no HWND), so this is necessarily all-or-
//    nothing per process: while installed it serves UMBRA's current palette.
//    Install/uninstall (set/unset) is the on/off switch; there is no separate
//    enabled flag, so the host controls effect purely by installing the hook.
//  * Independent of the comctl32 IAT hook in umbra's SysColorHook.cpp; the two
//    coexist (that IAT proc's fall-through to the real GetSysColor lands here).
//
// Returned colors track UMBRA's live palette (umbra::darkSysColor reads the
// get*Color accessors), so the host must have initialised the palette
// (initDarkMode / setDarkModeConfig / setDefaultColors) for the values to be
// meaningful.

#include <windows.h>
#include <detours/detours.h>

#include <mutex>
#include <vector>

#include "umbra.h"
#include "hook.h"

#pragma comment(lib, "detours.lib")

namespace
{
    using fnGetSysColor      = DWORD  (WINAPI*)(int);
    using fnGetSysColorBrush = HBRUSH (WINAPI*)(int);

    // Trampolines to the real exports; Detours rewrites these to point at the
    // relocated originals on attach. Seeded with the export so a stray call
    // before/after attach still routes correctly.
    fnGetSysColor      g_realGetSysColor      = ::GetSysColor;
    fnGetSysColorBrush g_realGetSysColorBrush = ::GetSysColorBrush;

    bool g_installed = false;

    // ---- brush cache --------------------------------------------------------
    // GetSysColorBrush must return a stable, process-lifetime brush per index
    // (callers must never delete it). We cache one brush per index and rebuild
    // it when the mapped color changes (palette/mode switch), retiring the old
    // handle for deletion at unset rather than deleting it live.
    std::mutex          g_brushMutex;
    std::vector<HBRUSH> g_retiredBrushes;

    struct BrushEntry { HBRUSH hbr; COLORREF clr; bool valid; };
    BrushEntry g_brushCache[COLOR_MENUBAR + 1]{};   // indices 0..30

    HBRUSH CachedBrush(int nIndex, COLORREF clr)
    {
        if (nIndex < 0 || nIndex >= static_cast<int>(ARRAYSIZE(g_brushCache)))
            return ::CreateSolidBrush(clr);   // outside cache range (mapped indices are within)

        std::lock_guard<std::mutex> lock(g_brushMutex);
        BrushEntry& e = g_brushCache[nIndex];
        if (e.valid && e.clr == clr)
            return e.hbr;

        HBRUSH nb = ::CreateSolidBrush(clr);
        if (e.valid && e.hbr != nullptr)
            g_retiredBrushes.push_back(e.hbr);   // defer delete; a caller may still hold it
        e.hbr = nb;
        e.clr = clr;
        e.valid = true;
        return nb;
    }

    void DestroyBrushes()
    {
        std::lock_guard<std::mutex> lock(g_brushMutex);
        for (BrushEntry& e : g_brushCache)
        {
            if (e.hbr != nullptr)
                ::DeleteObject(e.hbr);
            e = BrushEntry{};
        }
        for (HBRUSH h : g_retiredBrushes)
            ::DeleteObject(h);
        g_retiredBrushes.clear();
    }

    // ---- hook bodies --------------------------------------------------------
    // The colour decision is the library's (umbra::darkSysColor); we only serve
    // it through the export and fall through to the trampoline otherwise.
    DWORD WINAPI HookGetSysColor(int nIndex)
    {
        COLORREF c{};
        if (umbra::darkSysColor(nIndex, c))
            return c;
        return g_realGetSysColor(nIndex);     // trampoline, NOT ::GetSysColor (would self-recurse)
    }

    HBRUSH WINAPI HookGetSysColorBrush(int nIndex)
    {
        COLORREF c{};
        if (umbra::darkSysColor(nIndex, c))
            return CachedBrush(nIndex, c);
        return g_realGetSysColorBrush(nIndex);
    }
}

bool setProcessWideColorHook() noexcept
{
    if (g_installed)
        return true;

    if (::DetourTransactionBegin() != NO_ERROR)
        return false;
    ::DetourUpdateThread(::GetCurrentThread());

    LONG err = ::DetourAttach(reinterpret_cast<PVOID*>(&g_realGetSysColor),
                              reinterpret_cast<PVOID>(HookGetSysColor));
    if (err == NO_ERROR)
        err = ::DetourAttach(reinterpret_cast<PVOID*>(&g_realGetSysColorBrush),
                             reinterpret_cast<PVOID>(HookGetSysColorBrush));

    if (err != NO_ERROR)
    {
        ::DetourTransactionAbort();
        return false;
    }

    err = ::DetourTransactionCommit();
    g_installed = (err == NO_ERROR);
    return g_installed;
}

void unsetProcessWideColorHook() noexcept
{
    if (!g_installed)
        return;

    if (::DetourTransactionBegin() == NO_ERROR)
    {
        ::DetourUpdateThread(::GetCurrentThread());
        ::DetourDetach(reinterpret_cast<PVOID*>(&g_realGetSysColor),
                       reinterpret_cast<PVOID>(HookGetSysColor));
        ::DetourDetach(reinterpret_cast<PVOID*>(&g_realGetSysColorBrush),
                       reinterpret_cast<PVOID>(HookGetSysColorBrush));
        ::DetourTransactionCommit();
    }

    g_installed = false;
    DestroyBrushes();
}
