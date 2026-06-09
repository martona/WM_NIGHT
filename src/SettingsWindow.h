// SPDX-License-Identifier: MIT
#pragma once
#include <windows.h>

// One-time, at startup on the UI (main) thread: initialize the COM apartment (STA) that the
// XAML-Islands Settings window needs.
void SettingsInit();

// Open the Settings window, or focus it if it is already open.
void ShowSettingsWindow(HINSTANCE hInst);

// Called from the main message loop: lets the XAML island pre-handle keyboard / focus messages.
// Returns true if the island consumed the message (so the caller should skip Translate/Dispatch).
bool SettingsPreTranslateMessage(MSG* msg);
