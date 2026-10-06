#pragma once
// GLFW window wrapper hosting ImGui (imgui_impl_glfw backend).
// Linux counterpart of platform/win/Win32Window.
#include <GLFW/glfw3.h>

namespace bootroll {

class GlfwWindow {
public:
    GlfwWindow() = default;
    ~GlfwWindow();

    // Non-copyable: owns a GLFWwindow.
    GlfwWindow(const GlfwWindow&) = delete;
    GlfwWindow& operator=(const GlfwWindow&) = delete;

    bool create(const char* title, int width, int height);
    // Pump events. Returns false once the window should close.
    bool pumpMessages();
    bool minimized() const;
    GLFWwindow* handle() const { return m_window; }
    int clientWidth() const { return m_width; }
    int clientHeight() const { return m_height; }
    // Live content scale (Wayland compositor scale / X11 Xft.dpi), clamped to
    // [1, 3] like Win32Window::dpiScale(). Queried per call so the main loop
    // can follow monitor/compositor changes, mirroring the Windows path.
    float dpiScale() const;
    // True when GLFW picked its Wayland backend (glfw >= 3.4; false otherwise).
    bool isWayland() const;

private:
    static void framebufferSizeCallback(GLFWwindow* window, int w, int h);

    GLFWwindow* m_window = nullptr;
    int m_width = 0;
    int m_height = 0;
};

} // namespace bootroll
