// SPDX-License-Identifier: MIT
//
// The WM_NIGHT About dialog — a classic Win32 dialog (resource template IDD_ABOUT), dark-themed
// via umbra. Shows the 128px app icon, name/version, the MIT copyright, a link to the project, and
// the open-source components it builds on (Detours, umbra) as clickable SysLinks.

#include "AboutDialog.h"
#include "resource.h"
#include "version.h"

#include <windows.h>
#include <commctrl.h>   // NMLINK / SysLink
#include <shellapi.h>   // ShellExecuteW
#include <initializer_list>

#include <umbra.h>

namespace
{
    // Owned GDI objects, freed when the dialog is destroyed.
    struct AboutState { HICON icon; HFONT titleFont; };

    INT_PTR CALLBACK AboutProc(HWND dlg, UINT msg, WPARAM wParam, LPARAM lParam)
    {
        switch (msg)
        {
        case WM_INITDIALOG:
        {
            auto* st = new AboutState{};

            // 128px app icon in the left column.
            st->icon = static_cast<HICON>(::LoadImageW(::GetModuleHandleW(nullptr),
                MAKEINTRESOURCEW(IDI_APPICON), IMAGE_ICON, 128, 128, LR_DEFAULTCOLOR));
            if (st->icon != nullptr)
                ::SendDlgItemMessageW(dlg, IDC_ABOUT_ICON, STM_SETICON,
                                      reinterpret_cast<WPARAM>(st->icon), 0);

            // Larger title font.
            st->titleFont = ::CreateFontW(-26, 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE,
                DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
            if (st->titleFont != nullptr)
                ::SendDlgItemMessageW(dlg, IDC_ABOUT_TITLE, WM_SETFONT,
                                      reinterpret_cast<WPARAM>(st->titleFont), TRUE);

            ::SetWindowLongPtrW(dlg, DWLP_USER, reinterpret_cast<LONG_PTR>(st));

            ::SetDlgItemTextW(dlg, IDC_ABOUT_VERSION, L"Version " WMN_VERSION_STR);

            // Dark mode for the dialog canvas, title bar, and the SysLink controls.
            umbra::setDarkWndNotifySafe(dlg);
            umbra::setDarkTitleBar(dlg);
            for (int id : { IDC_ABOUT_LINK, IDC_ABOUT_DETOURS, IDC_ABOUT_UMBRA })
                umbra::enableSysLinkCtrlCtlColor(::GetDlgItem(dlg, id));
            return TRUE;
        }

        case WM_NOTIFY:
        {
            auto* hdr = reinterpret_cast<LPNMHDR>(lParam);
            if (hdr->code == NM_CLICK || hdr->code == NM_RETURN)
            {
                auto* link = reinterpret_cast<PNMLINK>(lParam);
                if (link->item.szUrl[0] != L'\0')
                    ::ShellExecuteW(dlg, L"open", link->item.szUrl, nullptr, nullptr, SW_SHOWNORMAL);
                return TRUE;
            }
            return FALSE;
        }

        case WM_COMMAND:
            if (LOWORD(wParam) == IDOK || LOWORD(wParam) == IDCANCEL)
            {
                ::EndDialog(dlg, 0);
                return TRUE;
            }
            return FALSE;

        case WM_DESTROY:
            if (auto* st = reinterpret_cast<AboutState*>(::GetWindowLongPtrW(dlg, DWLP_USER)))
            {
                if (st->icon != nullptr)      ::DestroyIcon(st->icon);
                if (st->titleFont != nullptr) ::DeleteObject(st->titleFont);
                delete st;
            }
            return FALSE;
        }
        return FALSE;
    }
}

void ShowAboutDialog(HWND owner)
{
    ::DialogBoxParamW(::GetModuleHandleW(nullptr), MAKEINTRESOURCEW(IDD_ABOUT),
                      owner, AboutProc, 0);
}
