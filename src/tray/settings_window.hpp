#pragma once

// Locating the settings window that is already open.
//
// The tray's Settings action must open the settings window or bring the existing one
// forward. Launching unconditionally would stack duplicate windows, so the tray first
// looks for a visible top-level window owned by a process in its own session whose image
// name matches the settings executable. Nothing here knows the settings UI's internals:
// it matches the executable name, not a window class or title, so a replacement client
// keeps working as long as it uses the same file name.
//
// Fusion's own tray icon is not the Windows input-language indicator; this helper only
// deals with the settings window.

#include <windows.h>

namespace fusion::tray {

// Case-insensitive match on the image file name (not the full path). Returns the first
// visible, titled top-level window owned by such a process in `session_id`, or nullptr.
// `exclude_pid` may be 0; when non-zero that process is skipped (the caller's own).
HWND find_window_of_process_image(const wchar_t *image_name, DWORD session_id, DWORD exclude_pid);

// Restores a minimized window and tries to bring it to the foreground. Returns true when
// the window ends up as the foreground window.
bool activate_window(HWND window);

} // namespace fusion::tray
