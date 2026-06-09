// SPDX-License-Identifier: MIT
#pragma once

// Application icon — the lowest icon id becomes the EXE icon; also the tray source.
#define IDI_APPICON     101

// Tray menu commands.
#define IDM_SETTINGS    40001
#define IDM_ABOUT       40002
#define IDM_EXIT        40003

// About dialog.
#define IDD_ABOUT           200
#define IDC_ABOUT_ICON      201
#define IDC_ABOUT_TITLE     202
#define IDC_ABOUT_VERSION   203
#define IDC_ABOUT_COPYRIGHT 204
#define IDC_ABOUT_LINK      205
#define IDC_ABOUT_DETOURS   206
#define IDC_ABOUT_UMBRA     207

#ifndef IDC_STATIC
#define IDC_STATIC          (-1)
#endif
