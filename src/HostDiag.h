// SPDX-License-Identifier: MIT
#pragma once
#include <string>

// Startup diagnostics surfaced (small-print) in the Settings window. Each returns one line; the
// state is a snapshot taken during host startup. Implemented in WM_NIGHT.cpp (where the state lives).
std::wstring DiagDllLine();        // payload DLL load state + version
std::wstring DiagUiAccessLine();   // was uiAccess actually granted to this process?
std::wstring DiagDuiLine();        // dui70 Element::PaintBackground RVA resolution result
