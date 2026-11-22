// RmlUi 6.3 Win32 platform helpers (MIT, see LICENSE.txt next to this file), trimmed for Fluvi.
#pragma once
#include <RmlUi/Core/Input.h>

namespace RmlWin32Fluvi {
// Current modifier keys; call on the thread that receives the window messages.
int GetKeyModifierState();
// Win32 virtual-key code -> RmlUi key identifier.
Rml::Input::KeyIdentifier ConvertKey(int win32_key_code);
} // namespace RmlWin32Fluvi
