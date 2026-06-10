// SPDX-License-Identifier: MIT
//
// DuiPaintHook.cpp — process-wide dui70 DirectUI::Element::PaintBackground hook.
//   (WM_NIGHThook — the DUI-element lever.)
//
// The Control Panel header/sidebar (and other DirectUI element backgrounds) are filled inside
// DirectUI::Element::PaintBackground, which runs BEFORE the offscreen memory-DC composite. Our
// uxtheme hooks miss them (they are not DrawThemeBackground draws) and window-level
// erase/backfill subclasses get blitted over (the buffer composites on top; there is no live
// HWND to target — the theme-draw log tags these wnd=memDC). This hook sits one layer up: it
// recolours the fill at its source, so the double-buffering is irrelevant.
//
// The member is non-exported, so the HOST (WM_NIGHT.exe) resolves its offset via symbols and
// hands it to us through the exported UmbraSetDuiPaintBg() (see the shared-section note below).
// We confirm the live dui70 matches (PE TimeDateStamp + SizeOfImage — a wrong offset would crash
// on attach), add the offset to the loaded base, and Detour-attach. No-ops if the host gave us no
// offset or dui70 is not mapped in this process.
//
// SPIKE / TEMP: LOGS every element paint (seq + decision) to dui-paint.log, and fills every
// non-overlay DUI background (Value TYPE tag != 1) with umbra's palette. The type tag is the real
// discriminant — a 15k-paint CP log split cleanly into type 9 = content, 11 = chrome, 1 = root +
// hover/hot overlays — so we fill 9/11 and LEAVE 1, which preserves hover/selection. Filling is
// idempotent, so it holds across repaints (breadcrumb-back). The `(raw+20)&7` "code" is a
// masked-away relabel (partly a pointer-alignment artifact), logged for reference only. Bit math
// from chip33's "Control Panel Color Fix" Windhawk mod; can shift across dui70 builds.

#include <windows.h>
#include <detours/detours.h>
#include <strsafe.h>

#include <mutex>
#include <string>
#include <unordered_set>

#include "umbra.h"
#include "hook.h"

#pragma comment(lib, "detours.lib")

#ifdef _WIN64
#  define DUI_CC __cdecl       // x64: the member is __cdecl (matches the mod's STDCALL macro)
#else
#  define DUI_CC __stdcall
#endif

// --- host->dll handoff: a PE SHARED section (no file, no named object) ------------------------
// This is a global-hook DLL: it has a SEPARATE image — and separate ordinary globals — in every
// process it maps into, so an exported setter called in the HOST would set only the host's copy;
// the explorer-loaded copy would never see it. Variables in a shared section are backed by the
// SAME physical pages in every process that maps this image, so the host's one write is visible
// to all loaded copies. 
#pragma section("umbrashr", read, write, shared)
__declspec(allocate("umbrashr")) volatile LONG g_sharedDuiRva   = 0;   // 0 = host has not set it
__declspec(allocate("umbrashr")) volatile LONG g_sharedDuiStamp = 0;   // dui70 TimeDateStamp
__declspec(allocate("umbrashr")) volatile LONG g_sharedDuiSize  = 0;   // dui70 SizeOfImage
// Authoritatively mark the section Read/Write/Shared — belt-and-suspenders over the pragma's
// `shared` attribute, so a non-shared fallback can't silently break the cross-process handoff.
#pragma comment(linker, "/SECTION:umbrashr,RWS")

namespace
{
    // DirectUI::Element::PaintBackground(Element* this, HDC, Value*, RECT const& x4). The four
    // RECTs pass by const-reference == LPRECT at the ABI. Element/Value are opaque here (void*).
    using fnElementPaintBg = void (DUI_CC*)(void*, HDC, void*, RECT*, RECT*, RECT*, RECT*);

    fnElementPaintBg g_realElementPaintBg = nullptr;
    bool             g_installed = false;
    bool             g_giveUp    = false;   // permanent failure (stamp mismatch, etc)

    // ---- recolour brush (umbra palette) ------------------------------------
    std::mutex g_brushMutex;
    HBRUSH     g_ctrlBrush = nullptr;
    HBRUSH     g_dlgBrush  = nullptr;

    HBRUSH DlgBackBrush()
    {
        std::lock_guard<std::mutex> lock(g_brushMutex);
        if (g_dlgBrush == nullptr)
            g_dlgBrush = ::CreateSolidBrush(umbra::getDlgBackgroundColor());
        return g_dlgBrush;
    }

    HBRUSH CtrlBackBrush()
    {
        std::lock_guard<std::mutex> lock(g_brushMutex);
        if (g_ctrlBrush == nullptr)
            g_ctrlBrush = ::CreateSolidBrush(umbra::getCtrlBackgroundColor());
        return g_ctrlBrush;
    }

    volatile LONG g_seq = 0;   // paint sequence counter (drives the log; kept even when it is off)

    // ---- diagnostics: per-paint log -> <logs>\dui-paint-<exe>.log ---------------------------
    // TEMP scaffolding (UMBRA_DIAG). When off, the Log* calls below are empty inlines — no
    // file, no per-paint WindowFromDC/format/write. When on: no dedup for paints (a state-
    // transition bug needed every repaint to diff), status lines dedup'd, a soft cap guards a
    // left-running host, and no per-line flush (the OS cache is coherent for a live reader).
#if UMBRA_DIAG
    HANDLE                           g_log = INVALID_HANDLE_VALUE;
    std::mutex                       g_logMutex;
    std::unordered_set<std::wstring> g_seenStatus;
    unsigned long                    g_logCount = 0;

    void WriteLogLocked(const wchar_t* line) noexcept   // call with g_logMutex held
    {
        if (g_log == INVALID_HANDLE_VALUE)
        {
            wchar_t path[MAX_PATH];
            umbraLogPath(L"dui-paint.log", path, ARRAYSIZE(path));
            g_log = ::CreateFileW(path, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE,
                                  nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
            if (g_log == INVALID_HANDLE_VALUE)
                return;
        }
        char u[512];
        const int n = ::WideCharToMultiByte(CP_UTF8, 0, line, -1, u, sizeof(u), nullptr, nullptr);
        if (n <= 1)
            return;
        DWORD w = 0;
        ::WriteFile(g_log, u, static_cast<DWORD>(n - 1), &w, nullptr);
    }

    void LogRaw(const wchar_t* line) noexcept
    {
        std::lock_guard<std::mutex> lock(g_logMutex);
        if (g_logCount >= 100000)   // safety cap; a forgotten host shouldn't fill the disk
            return;
        ++g_logCount;
        WriteLogLocked(line);
    }

    void LogStatus(const wchar_t* msg) noexcept
    {
        std::lock_guard<std::mutex> lock(g_logMutex);
        if (!g_seenStatus.insert(msg).second)
            return;
        wchar_t line[256];
        ::StringCchPrintfW(line, ARRAYSIZE(line), L"-- %s\r\n", msg);
        WriteLogLocked(line);
    }

    // One line per Element::PaintBackground call: sequence, decision (FILL, or why we passed),
    // classification, rect, and the hdc / element / value pointers — so a re-navigation render
    // can be told from the initial one (same hdc + element reused? new objects? does the body
    // even re-fire?). `wnd` is the target window class, "memDC" when double-buffered (the usual).
    void LogPaint(unsigned long seq, const wchar_t* decision, const void* el, HDC hdc,
                  const void* value, int code, int type, const RECT* rc) noexcept
    {
        wchar_t wnd[64] = L"memDC";
        const HWND h = ::WindowFromDC(hdc);
        if (h != nullptr)
            ::GetClassNameW(h, wnd, ARRAYSIZE(wnd));

        wchar_t line[400];
        ::StringCchPrintfW(line, ARRAYSIZE(line),
            L"%5lu %-7s code=%-2d type=%-3d @(%ld,%ld) %ldx%ld hdc=%p el=%p val=%p wnd=%s\r\n",
            seq, decision, code, type,
            rc ? rc->left : -1, rc ? rc->top : -1,
            rc ? (rc->right - rc->left) : -1, rc ? (rc->bottom - rc->top) : -1,
            hdc, el, value, wnd);
        LogRaw(line);
    }
#else
    inline void LogStatus(const wchar_t*) noexcept {}
    inline void LogPaint(unsigned long, const wchar_t*, const void*, HDC,
                         const void*, int, int, const RECT*) noexcept {}
#endif

    // ---- PE identity of an already-mapped module ---------------------------
    bool PeIdentityOfModule(HMODULE mod, DWORD& stamp, DWORD& sizeOfImage) noexcept
    {
        auto* const base = reinterpret_cast<const BYTE*>(mod);
        auto* const dos  = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
        if (dos->e_magic != IMAGE_DOS_SIGNATURE)
            return false;
        auto* const nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
        if (nt->Signature != IMAGE_NT_SIGNATURE)
            return false;
        stamp       = nt->FileHeader.TimeDateStamp;
        sizeOfImage = nt->OptionalHeader.SizeOfImage;
        return true;
    }

    // ---- the hook ----------------------------------------------------------
    // DirectUI::Element::PaintBackground
    void DUI_CC HookElementPaintBg(void* This, HDC hdc, void* value,
                                   RECT* pRect, RECT* pClip, RECT* pExclude, RECT* pTarget)
    {
        const unsigned long seq = static_cast<unsigned long>(::InterlockedIncrement(&g_seq));

        // Classify by the Value's TYPE tag (low 6 bits of dword0) — the real, stable discriminant.
        // A 15k-paint Control Panel log showed exactly three types, splitting cleanly:
        //   type 9  = content / nav backgrounds  (the centre block the mod's `type!=9` SKIPPED)
        //   type 11 = header / sidebar chrome
        //   type 1  = root
        // So fill the backgrounds (type != 1) and LEAVE type 1.
        // Filling is idempotent (an already-dark background just stays dark), so it is correct on
        // every repaint, breadcrumb-back included. The (raw+20)&7 "code" is a masked-away relabel
        // and partly a pointer-alignment artifact; kept for the log only, never for the decision.
        //
        //  Null=0  Int=1  Bool=2  Element=3  Ellist=4  String=5  Point=6  Size=7
        //  Rect=8  Color=9  Layout=10  Graphic=11  Sheet=12  Expr=13  ...
        int code = -1, type = -1;
        if (value != nullptr)
        {
            const DWORD v0 = *reinterpret_cast<const DWORD*>(value);
            type = static_cast<int>(v0 << 26) >> 26;
            const unsigned __int64 raw = *(reinterpret_cast<const unsigned __int64*>(value) + 1);
            code = static_cast<int>((raw + 20) & 7);
        }
                                                                                           //code == 2 is highlights
        if (value != nullptr && type != 1 && pRect != nullptr && code != 3 && code != 6 && !(code == 2 && type == 9))
        {
            ::FillRect(hdc, pRect, code == 4 ? CtrlBackBrush() : DlgBackBrush() );   // background -> dark; skip the original
            LogPaint(seq, L"FILL", This, hdc, value, code, type, pRect);
            return;
        }

        LogPaint(seq, (type == 1) ? L"t1" : L"skip", This, hdc, value, code, type, pRect);
        g_realElementPaintBg(This, hdc, value, pRect, pClip, pExclude, pTarget);
    }
}

// Exported: the host calls this ONCE (before installing the global hook) 
extern "C" __declspec(dllexport)
void UmbraSetDuiPaintBg(unsigned long rva, unsigned long stamp, unsigned long size) noexcept
{
    g_sharedDuiStamp = static_cast<LONG>(stamp);
    g_sharedDuiSize  = static_cast<LONG>(size);
    g_sharedDuiRva   = static_cast<LONG>(rva);
}

bool setProcessWideDuiPaintHook() noexcept
{
    if (g_installed)
        return true;
    if (g_giveUp)
        return false;

    const DWORD rva   = static_cast<DWORD>(g_sharedDuiRva);
    const DWORD stamp = static_cast<DWORD>(g_sharedDuiStamp);
    const DWORD size  = static_cast<DWORD>(g_sharedDuiSize);
    if (rva == 0)
    {
        // The host always sets this before installing the hook (and we run only after), so a
        // zero here means the host could not resolve the symbol — permanent for this session.
        LogStatus(L"host provided no dui70 RVA; DUI paint hook disabled");
        g_giveUp = true;
        return false;
    }

    const HMODULE dui = ::GetModuleHandleW(L"dui70.dll");
    if (dui == nullptr)
        return false;   // TRANSIENT: dui70 not loaded yet (regedit never loads it; explorer
                        // loads it lazily) — retry on a later window-creation CBT fire.

    DWORD liveStamp = 0, liveSize = 0;
    if (PeIdentityOfModule(dui, liveStamp, liveSize) && (liveStamp != stamp || liveSize != size))
    {
        wchar_t msg[256];
        ::StringCchPrintfW(msg, ARRAYSIZE(msg),
            L"dui70 mismatch host(stamp=%08lX size=%08lX) live(stamp=%08lX size=%08lX); disabled",
            stamp, size, liveStamp, liveSize);
        LogStatus(msg);
        g_giveUp = true;   // wrong RVA for this binary — do NOT attach (would crash)
        return false;
    }

    g_realElementPaintBg = reinterpret_cast<fnElementPaintBg>(reinterpret_cast<BYTE*>(dui) + rva);

    if (::DetourTransactionBegin() != NO_ERROR)
        return false;
    ::DetourUpdateThread(::GetCurrentThread());
    LONG err = ::DetourAttach(reinterpret_cast<PVOID*>(&g_realElementPaintBg),
                              reinterpret_cast<PVOID>(HookElementPaintBg));
    if (err != NO_ERROR)
    {
        ::DetourTransactionAbort();
        LogStatus(L"DetourAttach(Element::PaintBackground) failed; disabled");
        g_giveUp = true;
        return false;
    }
    err = ::DetourTransactionCommit();
    g_installed = (err == NO_ERROR);
    if (!g_installed)
    {
        LogStatus(L"DetourTransactionCommit failed; disabled");
        g_giveUp = true;
        return false;
    }

    wchar_t msg[160];
    ::StringCchPrintfW(msg, ARRAYSIZE(msg), L"DUI paint hook installed (rva=%08lX)", rva);
    LogStatus(msg);
    return true;
}

void unsetProcessWideDuiPaintHook() noexcept
{
    if (g_installed)
    {
        if (::DetourTransactionBegin() == NO_ERROR)
        {
            ::DetourUpdateThread(::GetCurrentThread());
            ::DetourDetach(reinterpret_cast<PVOID*>(&g_realElementPaintBg),
                           reinterpret_cast<PVOID>(HookElementPaintBg));
            ::DetourTransactionCommit();
        }
        g_installed = false;
    }
    std::lock_guard<std::mutex> lock(g_brushMutex);
    if (g_ctrlBrush != nullptr)
    {
        ::DeleteObject(g_ctrlBrush);
        g_ctrlBrush = nullptr;
    }
    if (g_dlgBrush != nullptr)
    {
        ::DeleteObject(g_dlgBrush);
        g_dlgBrush = nullptr;
    }
}
