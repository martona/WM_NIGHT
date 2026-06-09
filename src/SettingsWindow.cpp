// SPDX-License-Identifier: MIT
//
// The WM_NIGHT Settings window: a classic Win32 frame (dark title bar via umbra) whose entire
// client area hosts a Windows.UI.Xaml island (system XAML Islands, C++/WinRT — Windows SDK only,
// no NuGet / Windows App SDK runtime). The Win32 side owns the frame and the message pump; XAML
// owns the content and follows the system light/dark app theme.
//
// Content: a "Settings" header; a scrollable list of target blocks (exe icon | full path |
// trashcan remove button); a "your mileage may vary" message shown only on the built-in defaults
// or an empty list; and an "Add..." button that opens the shell file picker. All target state is
// the shared registry whitelist (see whitelist.h).

#include "SettingsWindow.h"
#include "resource.h"
#include "whitelist.h"

// winuser.h defines GetCurrentTime as a macro (-> GetTickCount), which collides with a XAML method
// of the same name in the cppwinrt animation headers (warning C4002). Drop it before they load.
#undef GetCurrentTime

#include <shellapi.h>   // SHGetFileInfoW (exe icons)
#include <shobjidl.h>   // IFileOpenDialog (Add... picker)
#include <vector>

#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.System.h>
#include <winrt/Windows.UI.h>
#include <winrt/Windows.UI.Text.h>
#include <winrt/Windows.Graphics.Imaging.h>
#include <winrt/Windows.Storage.Streams.h>
#include <winrt/Windows.UI.Xaml.h>
#include <winrt/Windows.UI.Xaml.Controls.h>
#include <winrt/Windows.UI.Xaml.Controls.Primitives.h>   // ButtonBase::Click (defines Button.Click)
#include <winrt/Windows.UI.Xaml.Media.h>
#include <winrt/Windows.UI.Xaml.Media.Imaging.h>
#include <winrt/Windows.UI.Xaml.Hosting.h>
#include <windows.ui.xaml.hosting.desktopwindowxamlsource.h>   // IDesktopWindowXamlSourceNative[2]
#include <DispatcherQueue.h>                                   // CreateDispatcherQueueController

#include <umbra.h>

#pragma comment(lib, "windowsapp.lib")
#pragma comment(lib, "advapi32.lib")   // registry: remembered window placement

using namespace winrt;
using namespace winrt::Windows::UI::Xaml;
using namespace winrt::Windows::UI::Xaml::Controls;
using namespace winrt::Windows::UI::Xaml::Hosting;
using namespace winrt::Windows::UI::Xaml::Media;

namespace wgi = winrt::Windows::Graphics::Imaging;
namespace wss = winrt::Windows::Storage::Streams;
namespace wxi = winrt::Windows::UI::Xaml::Media::Imaging;

namespace
{
    constexpr wchar_t kSettingsClass[] = L"WM_NIGHT_Settings";

    // Shown only on the built-in defaults, or when the list has been pruned empty.
    constexpr wchar_t kYmmvMessage[] =
        L"WM_NIGHT can dark-theme standard win32 apps. Your mileage may vary; the apps might look "
        L"partially transformed or might even crash. Click the below button to dark-theme "
        L"additional executables.";

    HWND                                       g_hwnd    = nullptr;   // the Win32 frame
    HWND                                       g_island  = nullptr;   // the XAML island child HWND
    DesktopWindowXamlSource                    g_source{ nullptr };
    WindowsXamlManager                         g_manager{ nullptr };
    Windows::System::DispatcherQueueController g_dq{ nullptr };
    com_ptr<IDesktopWindowXamlSourceNative2>   g_sourceNative2;

    StackPanel g_listPanel{ nullptr };   // the per-target blocks live here
    TextBlock  g_message{ nullptr };     // the YMMV message (collapsed when not applicable)
    Grid       g_root{ nullptr };        // content root (RequestedTheme flips on a live theme change)

    // Forward declarations (definition order below is bottom-up).
    void RefreshList();
    void ScheduleRefresh();
    UIElement BuildTargetBlock(std::wstring const& raw);
    void OnAddClicked();

    // --- Remembered window placement (HKCU\Software\WM_NIGHT\SettingsPlacement) -------------
    constexpr wchar_t kAppKey[]     = L"Software\\WM_NIGHT";
    constexpr wchar_t kPlaceValue[] = L"SettingsPlacement";

    struct Placement { LONG x, y, w, h; };

    bool LoadPlacement(Placement& p)
    {
        DWORD cb = sizeof(p);
        return ::RegGetValueW(HKEY_CURRENT_USER, kAppKey, kPlaceValue, RRF_RT_REG_BINARY,
                              nullptr, &p, &cb) == ERROR_SUCCESS && cb == sizeof(p);
    }

    void SavePlacement(const Placement& p)
    {
        ::RegSetKeyValueW(HKEY_CURRENT_USER, kAppKey, kPlaceValue, REG_BINARY, &p, sizeof(p));
    }

    // "Fully on the screen(s)": every corner must land on some monitor — catches a window saved on
    // a now-disconnected display or dragged off-screen.
    bool AllCornersOnScreen(const Placement& p)
    {
        const POINT corners[] = {
            { p.x,           p.y           }, { p.x + p.w - 1, p.y           },
            { p.x,           p.y + p.h - 1 }, { p.x + p.w - 1, p.y + p.h - 1 },
        };
        for (const POINT& pt : corners)
            if (::MonitorFromPoint(pt, MONITOR_DEFAULTTONULL) == nullptr)
                return false;
        return true;
    }

    // Default: centered on the primary display, ~1.5x wider and 2x taller than the old 760x560.
    Placement DefaultPlacement()
    {
        RECT wa{};
        ::SystemParametersInfoW(SPI_GETWORKAREA, 0, &wa, 0);
        const LONG caW = wa.right - wa.left, caH = wa.bottom - wa.top;
        Placement p{};
        p.w = caW < 1140 ? caW : 1140;
        p.h = caH < 1120 ? caH : 1120;
        p.x = wa.left + (caW - p.w) / 2;
        p.y = wa.top  + (caH - p.h) / 2;
        return p;
    }

    Placement ResolvePlacement()
    {
        Placement p{};
        if (LoadPlacement(p) && p.w >= 320 && p.h >= 240 && AllCornersOnScreen(p))
            return p;
        return DefaultPlacement();
    }

    // XAML Islands needs a DispatcherQueue on this thread and the XAML framework initialized for
    // it. Done once (the manager + dispatcher controller are kept alive for the process lifetime).
    void EnsureXamlBootstrapped()
    {
        if (g_manager)
            return;
        DispatcherQueueOptions options{ sizeof(DispatcherQueueOptions),
                                        DQTYPE_THREAD_CURRENT, DQTAT_COM_NONE };
        check_hresult(CreateDispatcherQueueController(options,
            reinterpret_cast<ABI::Windows::System::IDispatcherQueueController**>(put_abi(g_dq))));
        g_manager = WindowsXamlManager::InitializeForCurrentThread();
    }

    Brush ThemedBackground()
    {
        // Prefer the framework's themed page-background brush — a ThemeResource, so it tracks
        // light/dark live. (Available when an Application instance exists.)
        if (auto app = Application::Current())
        {
            auto res = app.Resources();
            auto key = box_value(L"ApplicationPageBackgroundThemeBrush");
            if (res.HasKey(key))
                return res.Lookup(key).try_as<Brush>();
        }
        // Fallback: a solid brush matching the current system theme (static; good enough here).
        const auto c = umbra::isDarkModeReg()
            ? Windows::UI::ColorHelper::FromArgb(255, 32, 32, 32)
            : Windows::UI::ColorHelper::FromArgb(255, 243, 243, 243);
        return SolidColorBrush(c);
    }

    // Push the current system light/dark theme onto the XAML content. Called at build time and on a
    // live WM_SETTINGCHANGE — the island doesn't auto-follow the OS theme without an Application
    // driving it, so we set RequestedTheme (re-resolves every ThemeResource in the subtree) and
    // re-apply the background explicitly (covers the solid fallback brush).
    void ApplyXamlTheme()
    {
        if (!g_root)
            return;
        g_root.RequestedTheme(umbra::isDarkModeReg() ? ElementTheme::Dark : ElementTheme::Light);
        if (auto bg = ThemedBackground())
            g_root.Background(bg);
    }

    // Pull the shell icon for `path` into a BGRA8 premultiplied SoftwareBitmap (UI thread; GDI).
    wgi::SoftwareBitmap MakeIconBitmap(std::wstring const& path)
    {
        SHFILEINFOW sfi{};
        if (::SHGetFileInfoW(path.c_str(), 0, &sfi, sizeof(sfi), SHGFI_ICON | SHGFI_LARGEICON) == 0
            || sfi.hIcon == nullptr)
            return nullptr;

        wgi::SoftwareBitmap result{ nullptr };
        ICONINFO ii{};
        if (::GetIconInfo(sfi.hIcon, &ii))
        {
            BITMAP bm{};
            if (::GetObjectW(ii.hbmColor, sizeof(bm), &bm) != 0 && bm.bmWidth > 0 && bm.bmHeight > 0)
            {
                const int w = bm.bmWidth, h = bm.bmHeight;
                BITMAPINFO bi{};
                bi.bmiHeader.biSize        = sizeof(BITMAPINFOHEADER);
                bi.bmiHeader.biWidth       = w;
                bi.bmiHeader.biHeight      = -h;   // top-down
                bi.bmiHeader.biPlanes      = 1;
                bi.bmiHeader.biBitCount    = 32;
                bi.bmiHeader.biCompression = BI_RGB;

                std::vector<uint8_t> px(static_cast<size_t>(w) * h * 4);
                const HDC dc = ::GetDC(nullptr);
                const int got = ::GetDIBits(dc, ii.hbmColor, 0, h, px.data(), &bi, DIB_RGB_COLORS);
                ::ReleaseDC(nullptr, dc);

                if (got == h)
                {
                    // Legacy icons with no alpha channel come back fully transparent — treat as
                    // opaque. Then premultiply (SoftwareBitmapSource wants premultiplied BGRA8).
                    bool anyAlpha = false;
                    for (size_t i = 3; i < px.size(); i += 4)
                        if (px[i] != 0) { anyAlpha = true; break; }
                    for (size_t i = 0; i < px.size(); i += 4)
                    {
                        uint8_t a = anyAlpha ? px[i + 3] : 255;
                        px[i + 3] = a;
                        px[i + 0] = static_cast<uint8_t>(px[i + 0] * a / 255);
                        px[i + 1] = static_cast<uint8_t>(px[i + 1] * a / 255);
                        px[i + 2] = static_cast<uint8_t>(px[i + 2] * a / 255);
                    }

                    wgi::SoftwareBitmap sb(wgi::BitmapPixelFormat::Bgra8, w, h,
                                           wgi::BitmapAlphaMode::Premultiplied);
                    wss::Buffer buf(static_cast<uint32_t>(px.size()));
                    buf.Length(static_cast<uint32_t>(px.size()));
                    memcpy(buf.data(), px.data(), px.size());
                    sb.CopyFromBuffer(buf);
                    result = sb;
                }
            }
            if (ii.hbmColor) ::DeleteObject(ii.hbmColor);
            if (ii.hbmMask)  ::DeleteObject(ii.hbmMask);
        }
        ::DestroyIcon(sfi.hIcon);
        return result;
    }

    // Extract the icon (sync, GDI) then push it into the Image asynchronously (SetBitmapAsync).
    fire_and_forget SetExeIcon(Image image, std::wstring path)
    {
        auto sb = MakeIconBitmap(path);
        if (!sb)
            co_return;
        wxi::SoftwareBitmapSource src;
        co_await src.SetBitmapAsync(sb);
        image.Source(src);
    }

    std::wstring PickExe(HWND owner)
    {
        std::wstring result;
        com_ptr<IFileOpenDialog> dlg;
        if (FAILED(::CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER,
                                      IID_PPV_ARGS(dlg.put()))))
            return result;

        const COMDLG_FILTERSPEC filters[] = {
            { L"Executables (*.exe)", L"*.exe" },
            { L"All files (*.*)",     L"*.*"   },
        };
        dlg->SetFileTypes(ARRAYSIZE(filters), filters);
        dlg->SetTitle(L"Choose an executable to dark-theme");
        FILEOPENDIALOGOPTIONS opts = 0;
        dlg->GetOptions(&opts);
        dlg->SetOptions(opts | FOS_FORCEFILESYSTEM | FOS_FILEMUSTEXIST | FOS_PATHMUSTEXIST);

        if (SUCCEEDED(dlg->Show(owner)))
        {
            com_ptr<IShellItem> item;
            if (SUCCEEDED(dlg->GetResult(item.put())))
            {
                PWSTR psz = nullptr;
                if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &psz)) && psz != nullptr)
                {
                    result.assign(psz);
                    ::CoTaskMemFree(psz);
                }
            }
        }
        return result;
    }

    bool AlreadyPresent(std::wstring const& expandedPath)
    {
        bool isDefaults = false;
        for (auto const& raw : whitelist::ReadEffectiveTargets(isDefaults))
            if (::CompareStringOrdinal(whitelist::Expand(raw).c_str(), -1,
                                       expandedPath.c_str(), -1, TRUE) == CSTR_EQUAL)
                return true;
        return false;
    }

    void OnAddClicked()
    {
        const std::wstring picked = PickExe(g_hwnd);
        if (picked.empty())
            return;
        if (!AlreadyPresent(picked))   // picked is an absolute path; no env vars to expand
        {
            whitelist::AddTarget(picked);
            ScheduleRefresh();
        }
    }

    UIElement BuildTargetBlock(std::wstring const& raw)
    {
        const std::wstring expanded = whitelist::Expand(raw);

        Grid block;
        for (auto type : { GridUnitType::Auto, GridUnitType::Star, GridUnitType::Auto })
        {
            ColumnDefinition col;
            col.Width(GridLengthHelper::FromValueAndType(type == GridUnitType::Star ? 1.0 : 0.0, type));
            block.ColumnDefinitions().Append(col);
        }

        Image icon;
        icon.Width(24);
        icon.Height(24);
        icon.VerticalAlignment(VerticalAlignment::Center);
        icon.Margin(ThicknessHelper::FromLengths(0, 0, 12, 0));
        Grid::SetColumn(icon, 0);
        SetExeIcon(icon, expanded);   // fire-and-forget async fill
        block.Children().Append(icon);

        TextBlock pathText;
        pathText.Text(hstring(expanded));
        pathText.VerticalAlignment(VerticalAlignment::Center);
        pathText.TextTrimming(TextTrimming::CharacterEllipsis);
        pathText.TextWrapping(TextWrapping::NoWrap);
        Grid::SetColumn(pathText, 1);
        block.Children().Append(pathText);

        Button del;
        FontIcon trash;
        trash.FontFamily(FontFamily(L"Segoe MDL2 Assets"));
        trash.Glyph(L"");   // Delete (trash can)
        del.Content(trash);
        del.VerticalAlignment(VerticalAlignment::Center);
        del.Margin(ThicknessHelper::FromLengths(12, 0, 0, 0));
        {
            const hstring name{ raw };
            del.Click([name](Windows::Foundation::IInspectable const&, RoutedEventArgs const&)
            {
                whitelist::RemoveTarget(std::wstring{ name.c_str() });
                ScheduleRefresh();
            });
        }
        Grid::SetColumn(del, 2);
        block.Children().Append(del);

        Border border;
        border.BorderBrush(SolidColorBrush(Windows::UI::ColorHelper::FromArgb(40, 128, 128, 128)));
        border.BorderThickness(ThicknessHelper::FromUniformLength(1));
        border.CornerRadius(CornerRadiusHelper::FromUniformRadius(6));
        border.Padding(ThicknessHelper::FromLengths(12, 8, 8, 8));
        border.Margin(ThicknessHelper::FromLengths(0, 0, 0, 8));
        border.Child(block);
        return border;
    }

    void RefreshList()
    {
        if (!g_listPanel)
            return;
        g_listPanel.Children().Clear();
        bool isDefaults = false;
        auto items = whitelist::ReadEffectiveTargets(isDefaults);
        for (auto const& raw : items)
            g_listPanel.Children().Append(BuildTargetBlock(raw));
        if (g_message)
            g_message.Visibility((isDefaults || items.empty()) ? Visibility::Visible
                                                               : Visibility::Collapsed);
    }

    // Re-read + rebuild on the UI thread, AFTER the current event handler unwinds (a remove handler
    // is destroying the very button it fired from).
    void ScheduleRefresh()
    {
        if (g_dq)
            g_dq.DispatcherQueue().TryEnqueue([]() { RefreshList(); });
    }

    UIElement BuildContent()
    {
        Grid root;
        g_root = root;

        for (auto type : { GridUnitType::Auto, GridUnitType::Star, GridUnitType::Auto, GridUnitType::Auto })
        {
            RowDefinition row;
            row.Height(GridLengthHelper::FromValueAndType(type == GridUnitType::Star ? 1.0 : 0.0, type));
            root.RowDefinitions().Append(row);
        }

        TextBlock title;
        title.Text(L"WM_NIGHT");
        title.FontSize(28);
        title.FontWeight(Windows::UI::Text::FontWeights::SemiBold());
        title.Margin(ThicknessHelper::FromLengths(24, 24, 24, 12));
        Grid::SetRow(title, 0);
        root.Children().Append(title);

        ScrollViewer scroller;
        scroller.VerticalScrollBarVisibility(ScrollBarVisibility::Auto);
        scroller.HorizontalScrollBarVisibility(ScrollBarVisibility::Disabled);
        scroller.Padding(ThicknessHelper::FromLengths(24, 0, 24, 0));
        StackPanel list;
        g_listPanel = list;
        scroller.Content(list);
        Grid::SetRow(scroller, 1);
        root.Children().Append(scroller);

        TextBlock message;
        message.Text(kYmmvMessage);
        message.TextWrapping(TextWrapping::Wrap);
        message.FontSize(13);
        message.Opacity(0.85);
        message.Margin(ThicknessHelper::FromLengths(24, 12, 24, 4));
        g_message = message;
        Grid::SetRow(message, 2);
        root.Children().Append(message);

        Button add;
        add.Content(box_value(hstring(L"Add…")));
        add.HorizontalAlignment(HorizontalAlignment::Left);
        add.Margin(ThicknessHelper::FromLengths(24, 8, 24, 24));
        add.Click([](Windows::Foundation::IInspectable const&, RoutedEventArgs const&) { OnAddClicked(); });
        Grid::SetRow(add, 3);
        root.Children().Append(add);

        ApplyXamlTheme();   // initial light/dark to match the system
        RefreshList();
        return root;
    }

    void SizeIslandToClient()
    {
        if (g_island == nullptr)
            return;
        RECT rc{};
        ::GetClientRect(g_hwnd, &rc);
        ::SetWindowPos(g_island, nullptr, 0, 0, rc.right - rc.left, rc.bottom - rc.top,
                       SWP_SHOWWINDOW);
    }

    LRESULT CALLBACK SettingsWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
    {
        switch (msg)
        {
        case WM_ERASEBKGND:
        {
            // Fill the client with the themed colour BEFORE the XAML island first paints, so a
            // dark-mode launch doesn't flash white.
            const HDC hdc = reinterpret_cast<HDC>(wParam);
            RECT rc{};
            ::GetClientRect(hwnd, &rc);
            const HBRUSH br = ::CreateSolidBrush(umbra::isDarkModeReg() ? RGB(32, 32, 32)
                                                                        : RGB(243, 243, 243));
            ::FillRect(hdc, &rc, br);
            ::DeleteObject(br);
            return 1;
        }

        case WM_SIZE:
            SizeIslandToClient();
            return 0;

        case WM_SETFOCUS:
            if (g_island != nullptr)
                ::SetFocus(g_island);   // hand keyboard focus to the island
            return 0;

        case WM_SETTINGCHANGE:
            // System light/dark flip: re-apply the dark title bar AND push the new theme onto the
            // XAML content (it won't follow on its own).
            if (umbra::handleSettingChange(lParam))
            {
                umbra::setDarkTitleBar(hwnd);
                ApplyXamlTheme();
            }
            return 0;

        case WM_GETMINMAXINFO:
        {
            auto* mmi = reinterpret_cast<MINMAXINFO*>(lParam);
            mmi->ptMinTrackSize.x = 480;
            mmi->ptMinTrackSize.y = 360;
            return 0;
        }

        case WM_DESTROY:
            // Remember size + position, but only from a normal (not min/max) state so we store a
            // sane restore rect.
            if (!::IsIconic(hwnd) && !::IsZoomed(hwnd))
            {
                RECT r{};
                if (::GetWindowRect(hwnd, &r))
                    SavePlacement(Placement{ r.left, r.top, r.right - r.left, r.bottom - r.top });
            }
            if (g_source != nullptr)
            {
                g_source.Close();
                g_source = nullptr;
            }
            g_sourceNative2 = nullptr;
            g_root = nullptr;
            g_listPanel = nullptr;
            g_message = nullptr;
            g_island = nullptr;
            g_hwnd   = nullptr;
            return 0;
        }
        return ::DefWindowProcW(hwnd, msg, wParam, lParam);
    }

    void CreateSettingsWindow(HINSTANCE hInst)
    {
        static bool s_registered = false;
        if (!s_registered)
        {
            WNDCLASSEXW wc{};
            wc.cbSize        = sizeof(wc);
            wc.lpfnWndProc   = SettingsWndProc;
            wc.hInstance     = hInst;
            wc.hCursor       = ::LoadCursorW(nullptr, IDC_ARROW);
            wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);  // brief; Phase 3 dark-fills
            wc.hIcon         = ::LoadIconW(hInst, MAKEINTRESOURCEW(IDI_APPICON));
            wc.lpszClassName = kSettingsClass;
            ::RegisterClassExW(&wc);
            s_registered = true;
        }

        const Placement pl = ResolvePlacement();
        g_hwnd = ::CreateWindowExW(0, kSettingsClass, L"WM_NIGHT - Settings",
                                   WS_OVERLAPPEDWINDOW, pl.x, pl.y, pl.w, pl.h,
                                   nullptr, nullptr, hInst, nullptr);
        if (g_hwnd == nullptr)
            return;

        umbra::setDarkTitleBar(g_hwnd);

        EnsureXamlBootstrapped();

        g_source = DesktopWindowXamlSource{};
        auto native = g_source.as<IDesktopWindowXamlSourceNative>();
        check_hresult(native->AttachToWindow(g_hwnd));
        check_hresult(native->get_WindowHandle(&g_island));
        g_sourceNative2 = g_source.as<IDesktopWindowXamlSourceNative2>();

        g_source.Content(BuildContent());
        SizeIslandToClient();

        ::ShowWindow(g_hwnd, SW_SHOW);
        ::SetForegroundWindow(g_hwnd);
    }
}

void SettingsInit()
{
    init_apartment(apartment_type::single_threaded);
}

void ShowSettingsWindow(HINSTANCE hInst)
{
    if (g_hwnd != nullptr)   // already open — focus it
    {
        if (::IsIconic(g_hwnd))
            ::ShowWindow(g_hwnd, SW_RESTORE);
        ::SetForegroundWindow(g_hwnd);
        return;
    }
    try
    {
        CreateSettingsWindow(hInst);
    }
    catch (...)
    {
        // XAML init can throw; don't take the tray process down with it — clean up and report.
        if (g_hwnd != nullptr)
        {
            ::DestroyWindow(g_hwnd);
            g_hwnd = nullptr;
        }
        ::MessageBoxW(nullptr,
            L"The Settings window could not be created (XAML Islands failed to initialize).",
            L"WM_NIGHT", MB_OK | MB_ICONERROR);
    }
}

bool SettingsPreTranslateMessage(MSG* msg)
{
    if (!g_sourceNative2)
        return false;
    BOOL handled = FALSE;
    return SUCCEEDED(g_sourceNative2->PreTranslateMessage(msg, &handled)) && handled != FALSE;
}
