#include "platform/win/Win32Window.h"
#include "backends/imgui_impl_win32.h"

#include <algorithm>

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

namespace bootroll {

namespace {

Win32Window* g_self = nullptr; // single-window app

} // namespace

Win32Window::Win32Window() = default;

Win32Window::~Win32Window()
{
    if (m_hwnd) {
        DestroyWindow(m_hwnd);
    }
    g_self = nullptr;
}

bool Win32Window::create(HINSTANCE instance, const wchar_t* title, int width, int height)
{
    // Per-monitor DPI awareness is enabled in createPlatform() (must precede
    // any DPI query and window creation).

    WNDCLASSEXW wc = {};
    wc.cbSize = sizeof(wc);
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = &Win32Window::wndProcStatic;
    wc.hInstance = instance;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hIcon = LoadIconW(instance, MAKEINTRESOURCEW(1)); // optional icon resource
    wc.lpszClassName = L"BootrollWndClass";
    RegisterClassExW(&wc);

    RECT rc = { 0, 0, width, height };
    AdjustWindowRect(&rc, WS_OVERLAPPEDWINDOW, FALSE);

    m_hwnd = CreateWindowW(wc.lpszClassName, title, WS_OVERLAPPEDWINDOW,
                           CW_USEDEFAULT, CW_USEDEFAULT,
                           rc.right - rc.left, rc.bottom - rc.top,
                           nullptr, nullptr, instance, nullptr);
    if (!m_hwnd) {
        return false;
    }
    g_self = this;

    // Rely on WM_DPICHANGED to size correctly; start from client size.
    RECT cr = {};
    GetClientRect(m_hwnd, &cr);
    m_width = cr.right - cr.left;
    m_height = cr.bottom - cr.top;

    ShowWindow(m_hwnd, SW_SHOWDEFAULT);
    UpdateWindow(m_hwnd);
    return true;
}

bool Win32Window::pumpMessages()
{
    MSG msg;
    while (PeekMessageW(&msg, nullptr, 0U, 0U, PM_REMOVE)) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
        if (msg.message == WM_QUIT) {
            return false;
        }
    }
    return true;
}

float Win32Window::dpiScale() const
{
    using Fn = UINT(WINAPI*)(HWND);
    UINT dpi = 96;
    if (HMODULE user32 = GetModuleHandleW(L"user32.dll")) {
        if (auto fn = reinterpret_cast<Fn>(GetProcAddress(user32, "GetDpiForWindow"))) {
            dpi = fn(m_hwnd);
        }
    }
    return std::clamp(float(dpi) / 96.0f, 1.0f, 3.0f);
}

LRESULT Win32Window::wndProcStatic(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    if (g_self) {
        return g_self->wndProc(hwnd, msg, wParam, lParam);
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

LRESULT Win32Window::wndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    if (ImGui_ImplWin32_WndProcHandler(hwnd, msg, wParam, lParam)) {
        return 1;
    }

    switch (msg) {
    case WM_SIZE: {
        m_width = LOWORD(lParam);
        m_height = HIWORD(lParam);
        m_minimized = (wParam == SIZE_MINIMIZED);
        return 0;
    }
    case WM_DPICHANGED: {
        // Move/resize to the suggested rect so the client area keeps its
        // logical size on the new display.
        const RECT* suggested = reinterpret_cast<const RECT*>(lParam);
        SetWindowPos(hwnd, nullptr, suggested->left, suggested->top,
                     suggested->right - suggested->left, suggested->bottom - suggested->top,
                     SWP_NOZORDER | SWP_NOACTIVATE);
        return 0;
    }
    case WM_SYSCOMMAND:
        if ((wParam & 0xFFF0) == SC_KEYMENU) { // disable ALT menu beep
            return 0;
        }
        break;
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    default:
        break;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

} // namespace bootroll
