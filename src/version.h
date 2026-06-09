// SPDX-License-Identifier: MIT
//
// WM_NIGHT release version — the single control point for the product version.
// Bump the four numbers below for a release; keep WMN_VERSION_STR in sync (the
// resource compiler can't stringize macros, so the dotted string is spelled out).

#pragma once

#define WMN_VERSION_MAJOR      0
#define WMN_VERSION_MINOR      1
#define WMN_VERSION_PATCH      0
#define WMN_VERSION_REVISION   0

// Comma form for the VERSIONINFO FILEVERSION / PRODUCTVERSION fields.
#define WMN_VERSION_DIGITAL    WMN_VERSION_MAJOR, WMN_VERSION_MINOR, WMN_VERSION_PATCH, WMN_VERSION_REVISION
// Dotted string form for the StringFileInfo block and the About box. Keep in sync ^.
#define WMN_VERSION_STR        L"0.1.0.0"

#define WMN_PRODUCT_NAME       L"WM_NIGHT"
#define WMN_FILE_DESCRIPTION   L"WM_NIGHT - native dark mode for the Windows desktop"
#define WMN_COMPANY_NAME       L"Marton Anka"
#define WMN_COPYRIGHT          L"Copyright (c) 2026 Marton Anka"
#define WMN_INTERNAL_NAME      L"WM_NIGHT"
#define WMN_ORIGINAL_FILENAME  L"WM_NIGHT.exe"
