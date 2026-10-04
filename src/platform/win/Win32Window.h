#pragma once
// Minimal Win32 window wrapper hosting ImGui (imgui_impl_win32 backend).
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

namespace bootroll {

class Win32Window {
public:
    Win32Window();
    ~Win32Window();

    bool create(HINSTANCE instance, const wchar_t* title, int width, int height);
    // Pump messages. Returns false once WM_QUIT was received.
    bool pumpMessages();
    // Per-monitor DPI of the display this window is on (96 dpi = 1.0x).
    float dpiScale() const;
    bool minimized() const { return m_minimized; }
    HWND hwnd() const { return m_hwnd; }
    int clientWidth() const { return m_width; }
    int clientHeight() const { return m_height; }

private:
    static LRESULT CALLBACK wndProcStatic(HWND, UINT, WPARAM, LPARAM);
    LRESULT wndProc(HWND, UINT, WPARAM, LPARAM);

    HWND m_hwnd = nullptr;
    int m_width = 0;
    int m_height = 0;
    bool m_minimized = false;
};

} // namespace bootroll
