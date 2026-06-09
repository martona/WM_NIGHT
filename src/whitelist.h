// SPDX-License-Identifier: MIT
//
// Shared whitelist config for the WM_NIGHT harness. Compiled into BOTH the payload
// (WM_NIGHThook.dll — calls IsWhitelisted on the process it injected into) and the host /
// settings UI (WM_NIGHT.exe — reads and edits the list). Pure Win32: no umbra, no XAML, so the
// payload stays lean.
//
// Storage: HKCU\Software\WM_NIGHT\Targets, one REG_SZ value per target. The value NAME is the
// target's full path (may contain %SystemRoot% etc.); the value data is reserved for future
// per-entry flags. Semantics:
//   * subkey ABSENT  -> unconfigured: fall back to the built-in defaults (explorer + regedit).
//   * subkey PRESENT -> use exactly the stored values (ZERO values => empty whitelist => no-op).

#pragma once
#include <windows.h>
#include <string>
#include <vector>

namespace whitelist
{
    // Is `fullProcessPath` (a real, expanded path, e.g. from GetModuleFileNameW) on the
    // whitelist? Reads the registry on each call via a stack-only path (no allocations). Used by
    // the payload once per process, on first injection.
    [[nodiscard]] bool IsWhitelisted(const wchar_t* fullProcessPath) noexcept;

    // True if the user has ever configured the list (i.e. the subkey exists).
    [[nodiscard]] bool IsConfigured() noexcept;

    // The list to show in the UI: raw value names (defaults still carry %SystemRoot%). Sets
    // outIsDefaults true when unconfigured (the returned list is then the built-in defaults).
    [[nodiscard]] std::vector<std::wstring> ReadEffectiveTargets(bool& outIsDefaults);

    // Add / remove a target. Both first MATERIALIZE the defaults into the registry if the list
    // was still unconfigured, so the defaults are never silently lost by the first edit.
    bool AddTarget(const std::wstring& rawPath);
    bool RemoveTarget(const std::wstring& rawValueName);

    // ExpandEnvironmentStrings convenience (for display + icon extraction in the UI).
    [[nodiscard]] std::wstring Expand(const std::wstring& raw);
}
