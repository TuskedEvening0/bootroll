#include "platform/linux/GlfwWindow.h"

#include <algorithm>
#include <cstdio>

namespace bootroll {

GlfwWindow::~GlfwWindow()
{
    if (m_window) {
        glfwDestroyWindow(m_window);
        m_window = nullptr;
    }
    // glfwTerminate is safe even when glfwInit failed / was never called.
    glfwTerminate();
}

void GlfwWindow::framebufferSizeCallback(GLFWwindow*, int w, int h)
{
    // Track the framebuffer (GL viewport size). ImGui_ImplOpenGL3 derives the
    // viewport from the actual GL state, while the ImGui display size comes
    // from glfwGetWindowSize(), so nothing else is needed here.
    (void)w;
    (void)h;
}

bool GlfwWindow::create(const char* title, int width, int height)
{
    glfwSetErrorCallback([](int error, const char* description) {
        std::fprintf(stderr, "GLFW error %d: %s\n", error, description);
    });

    if (!glfwInit()) {
        std::fprintf(stderr, "glfwInit failed\n");
        return false;
    }

    // OpenGL 3.0+ core-ish context; imgui_impl_opengl3 targets GLSL 130 by
    // default and Mesa/llvmpipe provides a software fallback for it.
    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 0);
    glfwWindowHint(GLFW_CLIENT_API, GLFW_OPENGL_API);
    glfwWindowHint(GLFW_VISIBLE, GLFW_TRUE);
    // Scale the initial window size by the monitor content scale (X11; on
    // Wayland this hint is a no-op because the compositor owns sizing, and
    // surface sizes are already in points).
    glfwWindowHint(GLFW_SCALE_TO_MONITOR, GLFW_TRUE);

    m_window = glfwCreateWindow(width, height, title, nullptr, nullptr);
    if (!m_window) {
        std::fprintf(stderr, "glfwCreateWindow failed\n");
        glfwTerminate();
        return false;
    }
    glfwMakeContextCurrent(m_window);
    glfwSwapInterval(1); // vsync, mirrors the DX11 present interval

    // Track window size in points, matching ImGui's DisplaySize semantics
    // (imgui_impl_glfw feeds glfwGetWindowSize into DisplaySize) and thus the
    // Win32Window client-area contract used by main.cpp.
    glfwGetWindowSize(m_window, &m_width, &m_height);
    glfwSetWindowUserPointer(m_window, this);
    glfwSetFramebufferSizeCallback(m_window, framebufferSizeCallback);
    return true;
}

bool GlfwWindow::pumpMessages()
{
    if (!m_window) {
        return false;
    }
    glfwPollEvents();
    glfwGetWindowSize(m_window, &m_width, &m_height);
    return !glfwWindowShouldClose(m_window);
}

bool GlfwWindow::minimized() const
{
    return !m_window || glfwGetWindowAttrib(m_window, GLFW_ICONIFIED) != 0;
}

float GlfwWindow::dpiScale() const
{
    if (!m_window) {
        return 1.0f;
    }
    float xs = 1.0f;
    float ys = 1.0f;
    glfwGetWindowContentScale(m_window, &xs, &ys);
    if (xs <= 0.0f) {
        xs = 1.0f; // headless / undetermined scale
    }
    return std::clamp(xs, 1.0f, 3.0f); // same clamp as Win32Window::dpiScale()
}

bool GlfwWindow::isWayland() const
{
#if defined(GLFW_VERSION_MAJOR) && (GLFW_VERSION_MAJOR > 3 || \
    (GLFW_VERSION_MAJOR == 3 && GLFW_VERSION_MINOR >= 4))
    return glfwGetPlatform() == GLFW_PLATFORM_WAYLAND;
#else
    return false; // glfw < 3.4 has no platform query (X11 assumed)
#endif
}

} // namespace bootroll
