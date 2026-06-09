// SPDX-License-Identifier: MIT
#include "whitelist.h"

#pragma comment(lib, "advapi32.lib")

namespace
{
    constexpr const wchar_t* kSubkey = L"Software\\WM_NIGHT\\Targets";

    // Built-in defaults — used when the subkey is absent (unconfigured). %SystemRoot% keeps
    // them correct on every machine.
    constexpr const wchar_t* kDefaults[] = {
        L"%SystemRoot%\\explorer.exe",
        L"%SystemRoot%\\regedit.exe",
    };

    // Real exe paths are well under MAX_PATH; anything longer than this is out of scope.
    constexpr size_t kBuf = 1024;

    bool PathEqualExpanded(const wchar_t* stored, const wchar_t* proc) noexcept
    {
        wchar_t expanded[kBuf];
        const DWORD n = ::ExpandEnvironmentStringsW(stored, expanded, kBuf);
        const wchar_t* s = (n != 0 && n <= kBuf) ? expanded : stored;
        return ::CompareStringOrdinal(s, -1, proc, -1, TRUE) == CSTR_EQUAL;
    }

    // Write the built-in defaults into the (created) subkey if the user hasn't configured one
    // yet, so a subsequent add/remove edits a real list instead of vanishing the defaults.
    void MaterializeDefaults()
    {
        if (whitelist::IsConfigured())
            return;
        HKEY key = nullptr;
        if (::RegCreateKeyExW(HKEY_CURRENT_USER, kSubkey, 0, nullptr, 0,
                              KEY_SET_VALUE, nullptr, &key, nullptr) != ERROR_SUCCESS)
            return;
        for (const wchar_t* d : kDefaults)
            ::RegSetValueExW(key, d, 0, REG_SZ,
                             reinterpret_cast<const BYTE*>(L""), sizeof(wchar_t));
        ::RegCloseKey(key);
    }
}

namespace whitelist
{
    bool IsWhitelisted(const wchar_t* procPath) noexcept
    {
        if (procPath == nullptr || procPath[0] == L'\0')
            return false;

        HKEY key = nullptr;
        if (::RegOpenKeyExW(HKEY_CURRENT_USER, kSubkey, 0, KEY_READ, &key) != ERROR_SUCCESS)
        {
            // Unconfigured: fall back to the built-in defaults.
            for (const wchar_t* d : kDefaults)
                if (PathEqualExpanded(d, procPath))
                    return true;
            return false;
        }

        // Configured: match the stored values verbatim (no defaults merged; empty => no-op).
        bool match = false;
        wchar_t name[kBuf];
        for (DWORD i = 0; !match; ++i)
        {
            DWORD len = ARRAYSIZE(name);
            const LONG e = ::RegEnumValueW(key, i, name, &len, nullptr, nullptr, nullptr, nullptr);
            if (e == ERROR_NO_MORE_ITEMS)
                break;
            if (e == ERROR_SUCCESS)
                match = PathEqualExpanded(name, procPath);
            else if (e == ERROR_MORE_DATA)
                continue;   // value name longer than kBuf — not a real exe path; skip
            else
                break;
        }
        ::RegCloseKey(key);
        return match;
    }

    bool IsConfigured() noexcept
    {
        HKEY key = nullptr;
        if (::RegOpenKeyExW(HKEY_CURRENT_USER, kSubkey, 0, KEY_READ, &key) != ERROR_SUCCESS)
            return false;
        ::RegCloseKey(key);
        return true;
    }

    std::vector<std::wstring> ReadEffectiveTargets(bool& outIsDefaults)
    {
        std::vector<std::wstring> out;
        HKEY key = nullptr;
        if (::RegOpenKeyExW(HKEY_CURRENT_USER, kSubkey, 0, KEY_READ, &key) != ERROR_SUCCESS)
        {
            outIsDefaults = true;
            for (const wchar_t* d : kDefaults)
                out.emplace_back(d);
            return out;
        }
        outIsDefaults = false;
        wchar_t name[kBuf];
        for (DWORD i = 0; ; ++i)
        {
            DWORD len = ARRAYSIZE(name);
            const LONG e = ::RegEnumValueW(key, i, name, &len, nullptr, nullptr, nullptr, nullptr);
            if (e == ERROR_NO_MORE_ITEMS)
                break;
            if (e == ERROR_SUCCESS)
                out.emplace_back(name);
            else if (e == ERROR_MORE_DATA)
                continue;
            else
                break;
        }
        ::RegCloseKey(key);
        return out;
    }

    bool AddTarget(const std::wstring& rawPath)
    {
        if (rawPath.empty())
            return false;
        MaterializeDefaults();
        HKEY key = nullptr;
        if (::RegCreateKeyExW(HKEY_CURRENT_USER, kSubkey, 0, nullptr, 0,
                              KEY_SET_VALUE, nullptr, &key, nullptr) != ERROR_SUCCESS)
            return false;
        const LONG r = ::RegSetValueExW(key, rawPath.c_str(), 0, REG_SZ,
                                        reinterpret_cast<const BYTE*>(L""), sizeof(wchar_t));
        ::RegCloseKey(key);
        return r == ERROR_SUCCESS;
    }

    bool RemoveTarget(const std::wstring& rawValueName)
    {
        MaterializeDefaults();
        HKEY key = nullptr;
        if (::RegOpenKeyExW(HKEY_CURRENT_USER, kSubkey, 0, KEY_SET_VALUE, &key) != ERROR_SUCCESS)
            return false;
        const LONG r = ::RegDeleteValueW(key, rawValueName.c_str());
        ::RegCloseKey(key);
        return r == ERROR_SUCCESS;
    }

    std::wstring Expand(const std::wstring& raw)
    {
        wchar_t buf[kBuf];
        const DWORD n = ::ExpandEnvironmentStringsW(raw.c_str(), buf, kBuf);
        return (n != 0 && n <= kBuf) ? std::wstring(buf) : raw;
    }
}
