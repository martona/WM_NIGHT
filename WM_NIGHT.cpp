// SPDX-License-Identifier: MIT
//
// umbra-inject.exe — host / controller for the umbra dark-theming payload.
//
// Installs ONE global WH_CBT hook whose proc lives in umbra-payload.dll, then stays
// alive (pumping messages) so the hook stays live. WH_CBT maps the payload into each
// target process as early as its first window; the payload whitelists regedit.exe and
// themes from there. uiAccess (asInvoker + signed, see umbra-inject.vcxproj) lets this
// medium-integrity host reach an elevated regedit without elevating itself — granted
// only to a signed binary in a trusted path (we report whether we actually got it).
//
// Temporary in-tree sample; removed before this branch merges to umbra's main.

#include <cstdio>
#include <string>
#include <thread>
#include <vector>

#include <windows.h>
#include <dbghelp.h>    // symbol resolution for dui70's non-exported Element::PaintBackground
#include <tlhelp32.h>   // process snapshot, to bounce explorer.exe on shutdown

#include "hook.h"   // kUmbraLogDir (symbol-cache dir under the repo)

#pragma comment(lib, "dbghelp.lib")

namespace
{
    HANDLE g_stop = nullptr;

    std::wstring ModuleDir()
    {
        wchar_t path[MAX_PATH]{};
        const DWORD n = ::GetModuleFileNameW(nullptr, path, ARRAYSIZE(path));
        std::wstring p(path, (n != 0 && n < ARRAYSIZE(path)) ? n : 0);
        const size_t slash = p.find_last_of(L"\\/");
        return slash == std::wstring::npos ? L"." : p.substr(0, slash);
    }

    // Did we actually receive uiAccess? (Only granted to a signed binary in a trusted
    // path; otherwise the manifest request is silently dropped and we run medium.)
    bool HasUiAccess()
    {
        HANDLE token = nullptr;
        if (!::OpenProcessToken(::GetCurrentProcess(), TOKEN_QUERY, &token))
            return false;
        DWORD uiAccess = 0;
        DWORD returned = 0;
        const bool ok =
            ::GetTokenInformation(token, TokenUIAccess, &uiAccess, sizeof(uiAccess), &returned) != FALSE;
        ::CloseHandle(token);
        return ok && uiAccess != 0;
    }

    // --- dui70 Element::PaintBackground RVA resolution (host-side) -----------
    // The payload wants to hook DirectUI::Element::PaintBackground, a NON-exported member, in
    // injected shell processes. We resolve it once HERE (a normal medium-IL process can talk to
    // the Microsoft symbol server; a medium-IL DLL injected into an elevated target should not)
    // and hand the payload only an RVA + the dui70 PE identity, through the DLL's exported
    // UmbraSetDuiPaintBg() (a shared section forwards it to every injected copy). The payload adds
    // the RVA to the live dui70 base (validating the identity first). Assumes a matching arch:
    // build the host x64 to theme x64 shells (this box's case).

    // dui70's TimeDateStamp + SizeOfImage off disk — the payload confirms its loaded copy matches
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
            std::printf("[dui] cannot read %ls\n", dui);
            return false;
        }

        wchar_t cache[MAX_PATH];
        ::StringCchPrintfW(cache, ARRAYSIZE(cache), L"%s\\symcache", kUmbraLogDir);
        (void)::CreateDirectoryW(kUmbraLogDir, nullptr);
        (void)::CreateDirectoryW(cache, nullptr);

        wchar_t symPath[1024];
        ::StringCchPrintfW(symPath, ARRAYSIZE(symPath),
            L"srv*%s*https://msdl.microsoft.com/download/symbols", cache);

        const HANDLE proc = ::GetCurrentProcess();
        ::SymSetOptions(SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS | SYMOPT_FAIL_CRITICAL_ERRORS | SYMOPT_NO_PROMPTS);
        if (!::SymInitializeW(proc, symPath, FALSE))
        {
            std::printf("[dui] SymInitialize failed: %lu\n", ::GetLastError());
            return false;
        }

        bool wrote = false;
        const DWORD64 base = ::SymLoadModuleExW(proc, nullptr, dui, nullptr, 0, 0, nullptr, 0);
        if (base == 0)
        {
            std::printf("[dui] SymLoadModuleEx failed: %lu (symbols unreachable?)\n", ::GetLastError());
        }
        else
        {
            std::vector<SymHit> hits;
            // Exact scoped name first; widen if dbghelp's undecorated name is unscoped / overloaded.
            ::SymEnumSymbols(proc, base, "DirectUI::Element::PaintBackground", EnumSymProc, &hits);
            if (hits.empty())
                ::SymEnumSymbols(proc, base, "*Element::PaintBackground", EnumSymProc, &hits);
            if (hits.empty())
                ::SymEnumSymbols(proc, base, "*PaintBackground", EnumSymProc, &hits);

            DWORD64 chosen = 0;
            for (const SymHit& h : hits)
            {
                std::printf("[dui]   candidate %-48s rva=0x%08lX\n",
                            h.name.c_str(), static_cast<DWORD>(h.addr - base));
                if (chosen == 0)
                    chosen = h.addr;
                if (h.name == "DirectUI::Element::PaintBackground")
                    chosen = h.addr;   // prefer the exact scoped name over a wildcard hit
            }

            if (chosen != 0)
            {
                outRva   = static_cast<DWORD>(chosen - base);
                outStamp = diskStamp;
                outSize  = diskSize;
                wrote    = true;
                std::printf("[dui] resolved rva=0x%08lX (dui70 stamp=%08lX size=%08lX)\n",
                            outRva, diskStamp, diskSize);
            }
            else
            {
                std::printf("[dui] DirectUI::Element::PaintBackground not found in symbols\n");
            }
            ::SymUnloadModule64(proc, base);
        }

        ::SymCleanup(proc);
        return wrote;
    }

    // Terminate explorer.exe so the still-pinned payload is dropped from the shell and the next
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

        if (bounced > 0)
            std::printf("Bounced explorer.exe (%d); the shell auto-restarts clean.\n", bounced);
        else
            std::printf("explorer.exe not found; nothing to bounce.\n");
    }

    BOOL WINAPI ConsoleHandler(DWORD)
    {
        if (g_stop != nullptr)
            ::SetEvent(g_stop);
        return TRUE;
    }

    void PumpUntilStop()
    {
        MSG msg{};
        for (;;)
        {
            const DWORD wait = ::MsgWaitForMultipleObjects(1, &g_stop, FALSE, INFINITE, QS_ALLINPUT);
            if (wait == WAIT_OBJECT_0)
                return;
            if (wait != WAIT_OBJECT_0 + 1)
                return;
            while (::PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE))
            {
                ::TranslateMessage(&msg);
                ::DispatchMessageW(&msg);
            }
        }
    }
}

int main()
{
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);

    std::setvbuf(stdout, nullptr, _IONBF, 0);
    std::printf("umbra-inject - dark-theming host (stub: regedit title bar)\n");
    std::printf("uiAccess: %s\n\n",
                HasUiAccess() ? "granted" : "NO (run signed from a trusted path, or elevate)");

    const std::wstring dllPath = ModuleDir() + L"\\umbra-payload.dll";
    HMODULE hookModule = ::LoadLibraryW(dllPath.c_str());
    if (hookModule == nullptr)
    {
        std::fwprintf(stderr, L"LoadLibrary(%s) failed: %lu\n", dllPath.c_str(), ::GetLastError());
        return 1;
    }

    const auto cbtProc = reinterpret_cast<HOOKPROC>(::GetProcAddress(hookModule, "UmbraCbtHook"));
    if (cbtProc == nullptr)
    {
        std::fwprintf(stderr, L"GetProcAddress(UmbraCbtHook) failed: %lu\n", ::GetLastError());
        ::FreeLibrary(hookModule);
        return 1;
    }

    // Resolve dui70's Element::PaintBackground (symbols, once) and hand it to the payload via its
    // exported setter — a shared section forwards it to every injected copy. Best-effort: on
    // failure the payload just runs without the DUI paint hook. Do this BEFORE SetWindowsHookEx,
    // so the shared section is populated before any target maps the payload and reads it.
    if (const auto setDui = reinterpret_cast<void (*)(unsigned long, unsigned long, unsigned long)>(
            ::GetProcAddress(hookModule, "UmbraSetDuiPaintBg")))
    {
        std::printf("[dui] resolving DirectUI::Element::PaintBackground"
                    " (first run downloads dui70.pdb to logs\\symcache)...\n");
        DWORD rva = 0, stamp = 0, size = 0;
        if (ResolveDuiPaintBgRva(rva, stamp, size))
            setDui(rva, stamp, size);
        else
            std::printf("[dui] unresolved; payload runs WITHOUT the DUI paint hook.\n");
        std::printf("\n");
    }

    g_stop = ::CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (g_stop == nullptr)
    {
        ::FreeLibrary(hookModule);
        return 1;
    }
    ::SetConsoleCtrlHandler(ConsoleHandler, TRUE);

    const HHOOK cbtHook = ::SetWindowsHookExW(WH_CBT, cbtProc, hookModule, 0);
    if (cbtHook == nullptr)
    {
        std::fwprintf(stderr, L"SetWindowsHookEx(WH_CBT) failed: %lu\n", ::GetLastError());
        ::SetConsoleCtrlHandler(ConsoleHandler, FALSE);
        ::CloseHandle(g_stop);
        ::FreeLibrary(hookModule);
        return 1;
    }

    std::printf("WH_CBT installed. Open regedit; its title bar should go dark.\n");
    std::printf("Press Enter to unhook and exit.\n");

    std::thread inputThread([]()
    {
        (void)std::getchar();
        if (g_stop != nullptr)
            ::SetEvent(g_stop);
    });

    PumpUntilStop();

    if (inputThread.joinable())
        inputThread.detach();

    ::UnhookWindowsHookEx(cbtHook);
    ::SetConsoleCtrlHandler(ConsoleHandler, FALSE);
    ::CloseHandle(g_stop);
    g_stop = nullptr;

    // Passive cleanup: the payload stays pinned in any process it themed (its subclasses
    // self-remove on WM_NCDESTROY). We just release our own reference.
    ::FreeLibrary(hookModule);
    std::printf("Unhooked. Already-themed windows keep their theme until they close.\n");

    // The CBT hook is down and our payload reference is released; now drop the copy still pinned
    // in the shell by restarting it, so the next launch injects a fresh payload into a clean tree.
    BounceExplorer();
    return 0;
}
