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
// Fill policy (scalpel):
//   1. Win32 HWND ancestry must be a shell browser (CabinetWClass / ExploreWClass), not desktop
//      hosts. The DUI root HWND itself is always "DirectUIHWND" — useless alone; we walk parents.
//   2. DUI root must be HWNDView registered by SHELL32 (classic CP tree from the paint log).
//   3. Interactive element classes (ControlPanelLink, Button, Search*, …) always pass through
//      so hover / selection bands stay intact.
//   4. Named CP chrome classes get a solid dark FillRect.
//   5. Bare dui70 `Element` with Value type Graphic (11) under the same gates — the CP top
//      navbar / left rail paint as anonymous Elements (log: 2208x120 and 400x1266). No global
//      type/code heuristic: type is consulted only for this unnamed-chrome case.
//
// DIAG: optional per-paint log (UMBRA_DIAG) with identity + decision reason.

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
    HBRUSH     g_dlgBrush  = nullptr;

    HBRUSH DlgBackBrush()
    {
        std::lock_guard<std::mutex> lock(g_brushMutex);
        if (g_dlgBrush == nullptr)
            g_dlgBrush = ::CreateSolidBrush(umbra::getDlgBackgroundColor());
        return g_dlgBrush;
    }

    volatile LONG g_seq = 0;   // paint sequence counter (drives the log; kept even when it is off)

    // ---- dui70 identity helpers (resolved once at attach) ---------------------------------
    // All x64 member calls are thiscall-in-rcx; a free-function pointer with an explicit first
    // `void* this` matches. Virtual slots verified against the live Element / ClassInfoBase /
    // HWNDElement vtables on this box (dui70 10.0.26100.x): GetClassInfoW = [35], IClassInfo
    // GetName = [8] / GetModule = [12] / IsSubclassOf = [10], HWNDElement::GetHWND = [45].
    constexpr int kVtGetClassInfoW   = 35;
    constexpr int kVtIClassGetName   = 8;
    constexpr int kVtIClassIsSubOf   = 10;
    constexpr int kVtIClassGetModule = 12;
    constexpr int kVtHwndGetHWND     = 45;

    using fnElGetRoot      = void* (DUI_CC*)(void* el);
    using fnElGetTopLevel  = void* (DUI_CC*)(void* el);
    using fnCiGetName      = const wchar_t* (DUI_CC*)(void* ci);   // ClassInfoBase::GetName
    using fnCiGetModule    = HINSTANCE (DUI_CC*)(void* ci);        // ClassInfoBase::GetModule
    using fnHwndElClassPtr = void* (__stdcall*)();                 // HWNDElement::GetClassInfoPtr

    fnElGetRoot      g_fnGetRoot      = nullptr;
    fnElGetTopLevel  g_fnGetTopLevel  = nullptr;
    fnCiGetName      g_fnCiGetName    = nullptr;   // direct export fallback
    fnCiGetModule    g_fnCiGetModule  = nullptr;
    fnHwndElClassPtr g_fnHwndClassPtr = nullptr;

    void* VCall(void* obj, int slot) noexcept
    {
        if (obj == nullptr)
            return nullptr;
        void** vt = *reinterpret_cast<void***>(obj);
        if (vt == nullptr || vt[slot] == nullptr)
            return nullptr;
        using fn = void* (DUI_CC*)(void*);
        return reinterpret_cast<fn>(vt[slot])(obj);
    }

    void* VCall2(void* obj, int slot, void* a1) noexcept
    {
        if (obj == nullptr)
            return nullptr;
        void** vt = *reinterpret_cast<void***>(obj);
        if (vt == nullptr || vt[slot] == nullptr)
            return nullptr;
        using fn = void* (DUI_CC*)(void*, void*);
        return reinterpret_cast<fn>(vt[slot])(obj, a1);
    }

    // Basename of a module path into `out` (no extension strip — keep .dll). Empty on failure.
    void ModuleBaseName(HINSTANCE mod, wchar_t* out, size_t outCount) noexcept
    {
        out[0] = L'\0';
        if (mod == nullptr || outCount == 0)
            return;
        wchar_t path[MAX_PATH];
        const DWORD n = ::GetModuleFileNameW(mod, path, ARRAYSIZE(path));
        if (n == 0 || n >= ARRAYSIZE(path))
            return;
        const wchar_t* base = path;
        for (const wchar_t* p = path; *p != L'\0'; ++p)
            if (*p == L'\\' || *p == L'/')
                base = p + 1;
        ::StringCchCopyW(out, outCount, base);
    }

    bool ModuleBaseIsShell32(const wchar_t* base) noexcept
    {
        return base != nullptr && base[0] != L'\0' && ::_wcsicmp(base, L"SHELL32.dll") == 0;
    }

    // Resolve GetClassInfoW via vtable; return IClassInfo* or null. SEH-guarded.
    const wchar_t* SafeClassName(void* el, wchar_t* fallbackBuf, size_t fallbackCount) noexcept
    {
        if (fallbackCount)
            fallbackBuf[0] = L'\0';
        const wchar_t* name = nullptr;
        __try
        {
            void* ci = VCall(el, kVtGetClassInfoW);
            if (ci != nullptr)
            {
                // Prefer virtual GetName (works for any IClassInfo); fall back to ClassInfoBase export.
                name = reinterpret_cast<const wchar_t*>(VCall(ci, kVtIClassGetName));
                if ((name == nullptr || name[0] == L'\0') && g_fnCiGetName != nullptr)
                    name = g_fnCiGetName(ci);
            }
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            if (fallbackCount)
                ::StringCchCopyW(fallbackBuf, fallbackCount, L"!exc");
            return fallbackBuf;
        }
        if (name == nullptr || name[0] == L'\0')
        {
            if (fallbackCount)
                ::StringCchCopyW(fallbackBuf, fallbackCount, L"?");
            return fallbackBuf;
        }
        return name;
    }

    void SafeClassModule(void* el, wchar_t* out, size_t outCount) noexcept
    {
        out[0] = L'\0';
        __try
        {
            void* ci = VCall(el, kVtGetClassInfoW);
            if (ci == nullptr)
                return;
            HINSTANCE mod = reinterpret_cast<HINSTANCE>(VCall(ci, kVtIClassGetModule));
            if (mod == nullptr && g_fnCiGetModule != nullptr)
                mod = g_fnCiGetModule(ci);
            ModuleBaseName(mod, out, outCount);
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            ::StringCchCopyW(out, outCount, L"!exc");
        }
    }

    void* SafeGetRoot(void* el) noexcept
    {
        void* root = nullptr;
        __try
        {
            if (g_fnGetRoot != nullptr)
                root = g_fnGetRoot(el);
            if (root == nullptr && g_fnGetTopLevel != nullptr)
                root = g_fnGetTopLevel(el);
            if (root == nullptr)
                root = el;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            root = el;
        }
        return root;
    }

    // HWND of the root element when it is an HWNDElement (or subclass). Null otherwise.
    HWND SafeRootHwnd(void* rootEl) noexcept
    {
        HWND hwnd = nullptr;
        __try
        {
            void* rootCi = VCall(rootEl, kVtGetClassInfoW);
            if (rootCi == nullptr)
                return nullptr;

            bool isHwndEl = false;
            if (g_fnHwndClassPtr != nullptr)
            {
                void* hwndCi = g_fnHwndClassPtr();
                if (hwndCi != nullptr)
                {
                    // IsSubclassOf returns bool (al, zero-extended into rax). Treat non-zero as true;
                    // also accept an exact HWNDElement class-info pointer match.
                    const auto r = reinterpret_cast<uintptr_t>(VCall2(rootCi, kVtIClassIsSubOf, hwndCi));
                    isHwndEl = (r != 0) || (rootCi == hwndCi);
                }
            }
            if (!isHwndEl)
            {
                // Name fallback when IsSubclassOf isn't available / isn't reflexive.
                const wchar_t* rn = reinterpret_cast<const wchar_t*>(VCall(rootCi, kVtIClassGetName));
                if (rn != nullptr &&
                    (::_wcsicmp(rn, L"HWNDElement") == 0 ||
                     ::_wcsicmp(rn, L"DialogElement") == 0 ||
                     ::_wcsicmp(rn, L"TouchHWNDElement") == 0 ||
                     ::_wcsicmp(rn, L"HWNDHost") == 0))
                    isHwndEl = true;
            }

            if (!isHwndEl)
                return nullptr;

            void** vt = *reinterpret_cast<void***>(rootEl);
            if (vt == nullptr || vt[kVtHwndGetHWND] == nullptr)
                return nullptr;
            using fnGetHwnd = HWND (DUI_CC*)(void*);
            hwnd = reinterpret_cast<fnGetHwnd>(vt[kVtHwndGetHWND])(rootEl);
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            hwnd = nullptr;
        }
        return hwnd;
    }

    // ---- Win32 HWND ancestry: shell browser vs desktop ------------------------------------
    // DirectUIHWND is the DUI host on every surface — walk parents for the real shell frame.
    // Allow: CabinetWClass / ExploreWClass (folder windows + classic Control Panel).
    // Deny hard: desktop / taskbar hosts if seen first.
    bool ClassIsShellBrowser(const wchar_t* cls) noexcept
    {
        return cls != nullptr &&
               (::_wcsicmp(cls, L"CabinetWClass") == 0 ||
                ::_wcsicmp(cls, L"ExploreWClass") == 0);
    }

    bool ClassIsDesktopOrTray(const wchar_t* cls) noexcept
    {
        if (cls == nullptr || cls[0] == L'\0')
            return false;
        static const wchar_t* const kDeny[] = {
            L"Progman",
            L"WorkerW",
            L"Shell_TrayWnd",
            L"Shell_SecondaryTrayWnd",
            L"DV2ControlHost",          // Vista+ desktop side helpers
            L"DesktopWindowContentBridge",
        };
        for (const wchar_t* d : kDeny)
            if (::_wcsicmp(cls, d) == 0)
                return true;
        return false;
    }

    // Walk parents of `hwnd`. On success writes the browser class into `outBrowserCls` and
    // returns true. Deny-list hits or no browser → false (out set to the deny class, or "-").
    bool IsUnderShellBrowser(HWND hwnd, wchar_t* outBrowserCls, size_t outCount) noexcept
    {
        if (outCount)
            ::StringCchCopyW(outBrowserCls, outCount, L"-");
        if (hwnd == nullptr || !::IsWindow(hwnd))
            return false;

        HWND cur = hwnd;
        for (int i = 0; i < 24 && cur != nullptr; ++i)
        {
            wchar_t cls[128];
            if (::GetClassNameW(cur, cls, ARRAYSIZE(cls)) == 0)
                break;
            if (ClassIsDesktopOrTray(cls))
            {
                if (outCount)
                    ::StringCchCopyW(outBrowserCls, outCount, cls);
                return false;
            }
            if (ClassIsShellBrowser(cls))
            {
                if (outCount)
                    ::StringCchCopyW(outBrowserCls, outCount, cls);
                return true;
            }
            HWND parent = ::GetParent(cur);
            if (parent == nullptr)
                parent = ::GetWindow(cur, GW_OWNER);
            cur = parent;
        }
        return false;
    }

    // ---- DUI element class policy ---------------------------------------------------------
    // Interactive: never replace PaintBackground (hover / press / selection bands).
    bool IsInteractiveDuiClass(const wchar_t* name) noexcept
    {
        if (name == nullptr || name[0] == L'\0')
            return false;
        static const wchar_t* const kExact[] = {
            L"ControlPanelLink",
            L"Button",
            L"Button3d",            // command glyph; keep system hover
            L"SearchBox",
            L"SearchEditBox",
            L"TouchButton",
            L"TouchHyperLink",
            L"PushButton",
            L"RepeatButton",
            L"CheckBoxGlyph",
            L"RadioButtonGlyph",
            L"ScrollBar",
            L"Thumb",
        };
        for (const wchar_t* e : kExact)
            if (::_wcsicmp(name, e) == 0)
                return true;
        return false;
    }

    // Named CP chrome we own.
    bool IsCpChromeDuiClass(const wchar_t* name) noexcept
    {
        if (name == nullptr || name[0] == L'\0')
            return false;
        static const wchar_t* const kExact[] = {
            L"HWNDView",
            L"ControlPanelCategoryModule",
            L"ControLPanelCategoryModuleInner", // Microsoft's capital-L typo — real class name
            L"ControlPanelNavPane",
            L"ControlPanelNavPaneInner",
            // Folder-in-CP / applet chrome seen in the same HWNDView tree (pass-class before):
            L"StatusBarModule",
            L"StatusBarModuleInner",
            L"ProperTreeModule",
            L"ProperTreeModuleInner",
            L"ProperTreeHost",
            L"ViewHost",
        };
        for (const wchar_t* e : kExact)
            if (::_wcsicmp(name, e) == 0)
                return true;
        return false;
    }

    // DirectUI Value type tag (low 6 bits of dword0). Graphic=11 is the CP header/nav slab.
    constexpr int kDuiValueGraphic = 11;

    // First few interesting caller modules (skip our hook + dui70). Pipe-separated into `out`.
    void CaptureCallerModules(wchar_t* out, size_t outCount) noexcept
    {
        out[0] = L'\0';
        void* stack[12]{};
        const USHORT n = ::CaptureStackBackTrace(2, 12, stack, nullptr);
        if (n == 0)
            return;

        HMODULE self = nullptr;
        ::GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                             GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                             reinterpret_cast<LPCWSTR>(&CaptureCallerModules), &self);
        HMODULE dui = ::GetModuleHandleW(L"dui70.dll");

        unsigned kept = 0;
        for (USHORT i = 0; i < n && kept < 4; ++i)
        {
            HMODULE mod = nullptr;
            if (!::GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                                      GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                      reinterpret_cast<LPCWSTR>(stack[i]), &mod) || !mod)
                continue;
            if (mod == self || mod == dui)
                continue;

            wchar_t base[64];
            ModuleBaseName(mod, base, ARRAYSIZE(base));
            if (base[0] == L'\0')
                continue;
            if (kept > 0 && ::wcsstr(out, base) != nullptr)
                continue;

            if (kept == 0)
                ::StringCchCopyW(out, outCount, base);
            else
            {
                ::StringCchCatW(out, outCount, L"|");
                ::StringCchCatW(out, outCount, base);
            }
            ++kept;
        }
        if (kept == 0)
            ::StringCchCopyW(out, outCount, L"-");
    }

    bool ResolveDuiApis(HMODULE dui) noexcept
    {
        g_fnGetRoot = reinterpret_cast<fnElGetRoot>(
            ::GetProcAddress(dui, "?GetRoot@Element@DirectUI@@QEAAPEAV12@XZ"));
        g_fnGetTopLevel = reinterpret_cast<fnElGetTopLevel>(
            ::GetProcAddress(dui, "?GetTopLevel@Element@DirectUI@@QEAAPEAV12@XZ"));
        g_fnCiGetName = reinterpret_cast<fnCiGetName>(
            ::GetProcAddress(dui, "?GetName@ClassInfoBase@DirectUI@@UEBAPEBGXZ"));
        g_fnCiGetModule = reinterpret_cast<fnCiGetModule>(
            ::GetProcAddress(dui, "?GetModule@ClassInfoBase@DirectUI@@UEBAPEAUHINSTANCE__@@XZ"));
        g_fnHwndClassPtr = reinterpret_cast<fnHwndElClassPtr>(
            ::GetProcAddress(dui, "?GetClassInfoPtr@HWNDElement@DirectUI@@SAPEAUIClassInfo@2@XZ"));
        return true;
    }

    // Value type/code — LOG ONLY (kept so we can still correlate; never used for fill decisions).
    void ReadValueTags(void* value, int& type, int& code) noexcept
    {
        type = -1;
        code = -1;
        if (value == nullptr)
            return;
        __try
        {
            const DWORD v0 = *reinterpret_cast<const DWORD*>(value);
            type = static_cast<int>(v0 << 26) >> 26;
            const unsigned __int64 raw = *(reinterpret_cast<const unsigned __int64*>(value) + 1);
            code = static_cast<int>((raw + 20) & 7);
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            type = -1;
            code = -1;
        }
    }

    // ---- diagnostics: per-paint log -> <logs>\dui-paint-<exe>.log ---------------------------
#if UMBRA_DIAG
    HANDLE                           g_log = INVALID_HANDLE_VALUE;
    std::mutex                       g_logMutex;
    std::unordered_set<std::wstring> g_seenStatus;
    std::unordered_set<std::wstring> g_seenIdentity;
    unsigned long                    g_logCount = 0;

    void WriteLogLocked(const wchar_t* line) noexcept
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
        char u[1024];
        const int n = ::WideCharToMultiByte(CP_UTF8, 0, line, -1, u, sizeof(u), nullptr, nullptr);
        if (n <= 1)
            return;
        DWORD w = 0;
        ::WriteFile(g_log, u, static_cast<DWORD>(n - 1), &w, nullptr);
    }

    void LogRaw(const wchar_t* line) noexcept
    {
        std::lock_guard<std::mutex> lock(g_logMutex);
        if (g_logCount >= 100000)
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

    void LogPaint(unsigned long seq, const wchar_t* decision,
                  const wchar_t* elClass, const wchar_t* elMod,
                  const wchar_t* rootClass, const wchar_t* rootMod,
                  const wchar_t* rootWnd, const wchar_t* shell,
                  const wchar_t* callers,
                  int code, int type, const RECT* rc,
                  HDC hdc, const void* el, const void* value, const wchar_t* hdcWnd) noexcept
    {
        wchar_t line[800];
        ::StringCchPrintfW(line, ARRAYSIZE(line),
            L"%5lu %-10s code=%-2d type=%-3d el=%s elMod=%s root=%s rootMod=%s "
            L"rootWnd=%s shell=%s call=%s @(%ld,%ld) %ldx%ld hdc=%p elp=%p val=%p wnd=%s\r\n",
            seq, decision, code, type,
            elClass, elMod, rootClass, rootMod, rootWnd, shell, callers,
            rc ? rc->left : -1, rc ? rc->top : -1,
            rc ? (rc->right - rc->left) : -1, rc ? (rc->bottom - rc->top) : -1,
            hdc, el, value, hdcWnd);
        LogRaw(line);

        wchar_t key[400];
        ::StringCchPrintfW(key, ARRAYSIZE(key),
            L"%s|%s|%s|%s|%s|%s|t%d|c%d",
            decision, elClass, elMod, rootClass, rootMod, shell, type, code);
        {
            std::lock_guard<std::mutex> lock(g_logMutex);
            if (g_seenIdentity.insert(key).second)
            {
                wchar_t sum[512];
                ::StringCchPrintfW(sum, ARRAYSIZE(sum),
                    L"NEW %-10s el=%s elMod=%s root=%s rootMod=%s rootWnd=%s shell=%s type=%d code=%d call=%s\r\n",
                    decision, elClass, elMod, rootClass, rootMod, rootWnd, shell, type, code, callers);
                WriteLogLocked(sum);
            }
        }
    }
#else
    inline void LogStatus(const wchar_t*) noexcept {}
    inline void LogPaint(unsigned long, const wchar_t*,
                         const wchar_t*, const wchar_t*,
                         const wchar_t*, const wchar_t*,
                         const wchar_t*, const wchar_t*,
                         const wchar_t*,
                         int, int, const RECT*,
                         HDC, const void*, const void*, const wchar_t*) noexcept {}
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
    void DUI_CC HookElementPaintBg(void* This, HDC hdc, void* value,
                                   RECT* pRect, RECT* pClip, RECT* pExclude, RECT* pTarget)
    {
        const unsigned long seq = static_cast<unsigned long>(::InterlockedIncrement(&g_seq));

        int type = -1, code = -1;
        ReadValueTags(value, type, code);

        // --- identity (once) ------------------------------------------------
        wchar_t elClassBuf[64]{};
        wchar_t rootClassBuf[64]{};
        wchar_t elMod[64]{};
        wchar_t rootMod[64]{};
        wchar_t rootWndCls[64] = L"-";
        wchar_t shellCls[64]   = L"-";
        wchar_t callers[160]{};
        wchar_t hdcWnd[64] = L"memDC";

        const wchar_t* elClass = SafeClassName(This, elClassBuf, ARRAYSIZE(elClassBuf));
        SafeClassModule(This, elMod, ARRAYSIZE(elMod));
        if (elMod[0] == L'\0')
            ::StringCchCopyW(elMod, ARRAYSIZE(elMod), L"-");

        void* root = SafeGetRoot(This);
        const wchar_t* rootClass = SafeClassName(root, rootClassBuf, ARRAYSIZE(rootClassBuf));
        SafeClassModule(root, rootMod, ARRAYSIZE(rootMod));
        if (rootMod[0] == L'\0')
            ::StringCchCopyW(rootMod, ARRAYSIZE(rootMod), L"-");

        HWND rootHwnd = SafeRootHwnd(root);
        if (rootHwnd != nullptr)
        {
            if (::GetClassNameW(rootHwnd, rootWndCls, ARRAYSIZE(rootWndCls)) == 0)
                ::StringCchCopyW(rootWndCls, ARRAYSIZE(rootWndCls), L"?");
        }

        // Fallback HWND for ancestry: WindowFromDC when the root isn't an HWNDElement host
        // (rare) or GetHWND failed — still prefer rootHwnd when present.
        HWND ancHwnd = rootHwnd;
        const HWND hdcHwnd = ::WindowFromDC(hdc);
        if (hdcHwnd != nullptr)
        {
            if (::GetClassNameW(hdcHwnd, hdcWnd, ARRAYSIZE(hdcWnd)) == 0)
                ::StringCchCopyW(hdcWnd, ARRAYSIZE(hdcWnd), L"?");
            if (ancHwnd == nullptr)
                ancHwnd = hdcHwnd;
        }

        const bool underBrowser = IsUnderShellBrowser(ancHwnd, shellCls, ARRAYSIZE(shellCls));

#if UMBRA_DIAG
        CaptureCallerModules(callers, ARRAYSIZE(callers));
#endif

        // --- policy ---------------------------------------------------------
        // Decision tags (log):
        //   FILL / FILL-el11 | pass-hwnd | pass-root | pass-inter | pass-class
        const wchar_t* decision = L"pass-class";
        bool doFill = false;

        if (!underBrowser)
        {
            decision = L"pass-hwnd";
        }
        else if (::_wcsicmp(rootClass, L"HWNDView") != 0 || !ModuleBaseIsShell32(rootMod))
        {
            // Classic CP tree only (from the paint log). SearchBox/explorerframe stays out.
            decision = L"pass-root";
        }
        else if (IsInteractiveDuiClass(elClass))
        {
            decision = L"pass-inter";
        }
        else if (IsCpChromeDuiClass(elClass) && pRect != nullptr)
        {
            decision = L"FILL";
            doFill = true;
        }
        else if (elClass != nullptr && ::_wcsicmp(elClass, L"Element") == 0 &&
                 type == kDuiValueGraphic && pRect != nullptr)
        {
            // Unnamed chrome under CP only: top navbar + left rail (type=11 slabs in the log).
            // Not a global type heuristic — gated by shell browser + HWNDView/SHELL32 above,
            // and limited to bare Element so Color(9) hover residues on Element stay original.
            decision = L"FILL-el11";
            doFill = true;
        }
        else
        {
            decision = L"pass-class";
        }

        LogPaint(seq, decision,
                 elClass, elMod, rootClass, rootMod, rootWndCls, shellCls, callers,
                 code, type, pRect, hdc, This, value, hdcWnd);

        if (doFill)
        {
            ::FillRect(hdc, pRect, DlgBackBrush());
            return;   // skip original — solid chrome
        }

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
        LogStatus(L"host provided no dui70 RVA; DUI paint hook disabled");
        g_giveUp = true;
        return false;
    }

    const HMODULE dui = ::GetModuleHandleW(L"dui70.dll");
    if (dui == nullptr)
        return false;   // TRANSIENT: dui70 not loaded yet — retry on a later CBT fire.

    DWORD liveStamp = 0, liveSize = 0;
    if (PeIdentityOfModule(dui, liveStamp, liveSize) && (liveStamp != stamp || liveSize != size))
    {
        wchar_t msg[256];
        ::StringCchPrintfW(msg, ARRAYSIZE(msg),
            L"dui70 mismatch host(stamp=%08lX size=%08lX) live(stamp=%08lX size=%08lX); disabled",
            stamp, size, liveStamp, liveSize);
        LogStatus(msg);
        g_giveUp = true;
        return false;
    }

    ResolveDuiApis(dui);

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

    wchar_t msg[200];
    ::StringCchPrintfW(msg, ARRAYSIZE(msg),
        L"DUI paint hook installed (rva=%08lX getRoot=%d getName=%d hwndCI=%d policy=class+hwnd)",
        rva,
        g_fnGetRoot != nullptr ? 1 : 0,
        g_fnCiGetName != nullptr ? 1 : 0,
        g_fnHwndClassPtr != nullptr ? 1 : 0);
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
    if (g_dlgBrush != nullptr)
    {
        ::DeleteObject(g_dlgBrush);
        g_dlgBrush = nullptr;
    }
}
