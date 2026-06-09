#pragma once
#include <windows.h>
#include <strsafe.h>

// Shared across the WM_NIGHT injection harness (host umbra-inject + payload umbra-payload).

// Master switch for ALL diagnostic file logging (dui-paint, themecolor, umbra-inject). Set to 0
// here (or define it from the build) to compile every diagnostic log out across host and payload.
// TEMP scaffolding — the logs never ship.
#ifndef UMBRA_DIAG
#  define UMBRA_DIAG 0
#endif

// --- Diagnostic logs: one fixed location, one file per target -------------
// Every host/payload diagnostic log goes to ONE hardcoded directory, so they never scatter across
// module dirs. (An injected target's <module dir> resolves to C:\Windows / system32, which a
// medium-integrity process can't even write — so logs would silently vanish.) A fixed repo path
// under the user profile is writable by both elevated and medium-integrity targets. The running
// .exe's name is appended to each log's base, so the global hook's several targets write SEPARATE
// files — themecolor-regedit.log, umbra-inject-explorer.log — instead of clobbering one shared
// file. Hardcoded by design: throwaway diagnostic plumbing that never ships. Delete for a clean run.
inline constexpr const wchar_t* kUmbraLogDir =
    L"C:\\Users\\Marton\\Desktop\\github\\WM_NIGHT\\logs";

// Writes the running module's base name without extension into `out` (e.g. "regedit" for
// C:\Windows\regedit.exe). Falls back to "unknown".
inline void umbraExeStem(wchar_t* out, size_t outCount) noexcept
{
    wchar_t path[MAX_PATH];
    const DWORD n = ::GetModuleFileNameW(nullptr, path, ARRAYSIZE(path));
    if (n == 0 || n >= ARRAYSIZE(path))
    {
        ::StringCchCopyW(out, outCount, L"unknown");
        return;
    }
    wchar_t* base = path;
    for (wchar_t* p = path; *p != L'\0'; ++p)
        if (*p == L'\\' || *p == L'/')
            base = p + 1;
    wchar_t* dot = nullptr;
    for (wchar_t* p = base; *p != L'\0'; ++p)
        if (*p == L'.')
            dot = p;
    if (dot != nullptr)
        *dot = L'\0';
    ::StringCchCopyW(out, outCount, base);
}

// Composes "<kUmbraLogDir>\<base>-<exe><ext>" into `out` (e.g. "...\themecolor-regedit.log"),
// creating the directory if needed. `out` is always left a valid (possibly truncated) string;
// returns false only if too small.
inline bool umbraLogPath(const wchar_t* fileName, wchar_t* out, size_t outCount) noexcept
{
    (void)::CreateDirectoryW(kUmbraLogDir, nullptr);   // succeeds, or already exists

    wchar_t exe[64];
    umbraExeStem(exe, ARRAYSIZE(exe));

    // Split fileName at its last '.', so the exe stem lands on the base, before the extension:
    // "themecolor.log" -> "themecolor-<exe>.log".
    wchar_t base[64];
    const wchar_t* ext = L"";
    const wchar_t* dot = nullptr;
    for (const wchar_t* p = fileName; *p != L'\0'; ++p)
        if (*p == L'.')
            dot = p;
    if (dot != nullptr)
    {
        ext = dot;
        ::StringCchCopyNW(base, ARRAYSIZE(base), fileName, static_cast<size_t>(dot - fileName));
    }
    else
    {
        ::StringCchCopyW(base, ARRAYSIZE(base), fileName);
    }

    return SUCCEEDED(::StringCchPrintfW(out, outCount, L"%s\\%s-%s%s",
                                        kUmbraLogDir, base, exe, ext));
}

// --- Process-wide dark hooks (compiled into the payload) -------------------
// The interception half of UMBRA's dark mode: Detours-based, process-wide hooks that drive the
// umbra library's per-window / per-colour theming decisions. The Detours dependency lives here in
// the payload, never in the umbra library.

// GetSysColor / GetSysColorBrush inline hook (classic + DirectUI colour residue).
bool setProcessWideColorHook() noexcept;
void unsetProcessWideColorHook() noexcept;

// uxtheme Open/Get/Draw hooks: HTHEME->class map + GetThemeColor override + DrawThemeBackground
// flat-fill where umbra::darkThemeBackground owns the decision.
bool setProcessWideThemeColorHook() noexcept;
void unsetProcessWideThemeColorHook() noexcept;

// dui70 DirectUI::Element::PaintBackground inline hook (Detours). DUI element backgrounds (the
// Control Panel content/chrome, ...) are filled HERE — before the offscreen memory-DC composite —
// so this reaches surfaces uxtheme hooks miss and window-level erase/backfill can't hold. The
// member is NON-exported: the host (umbra-inject.exe) resolves its RVA via symbols and passes it
// in through the DLL's exported UmbraSetDuiPaintBg() (a shared section carries it to every injected
// copy); the payload adds the live dui70 base and attaches. Returns true once attached; a cheap
// no-op if the host gave no RVA / it mismatched, and a transient false (retry on a later window-
// creation) while dui70 is not yet loaded in this process. TEMP.
bool setProcessWideDuiPaintHook() noexcept;
void unsetProcessWideDuiPaintHook() noexcept;
