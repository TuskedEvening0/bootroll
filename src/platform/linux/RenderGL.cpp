#include "platform/linux/RenderGL.h"
#include "imgui.h"
#include "backends/imgui_impl_opengl3.h"

namespace bootroll {

RenderGL::~RenderGL()
{
    shutdown();
}

void RenderGL::shutdown()
{
    // Must happen before ImGui::DestroyContext(), otherwise the backend
    // touches a freed context (crash on exit - see LESSONS.md #2).
    if (m_initialized) {
        ImGui_ImplOpenGL3_Shutdown();
        m_initialized = false;
    }
    m_window = nullptr;
}

bool RenderGL::init(GLFWwindow* window)
{
    m_window = window;
    if (!ImGui_ImplOpenGL3_Init(nullptr)) { // default "#version 130"
        return false;
    }
    m_initialized = true;
    return true;
}

void RenderGL::resize(int width, int height)
{
    // The GL viewport is refreshed in newFrame() from the current framebuffer
    // size; nothing to recreate (unlike the DX11 render target view).
    (void)width;
    (void)height;
}

void RenderGL::recreateFontTexture()
{
    // Called after App::setDpiScale() rebuilt the style and font atlas.
    if (m_initialized) {
        ImGui_ImplOpenGL3_CreateDeviceObjects();
    }
}

void RenderGL::newFrame()
{
    int fbWidth = 0, fbHeight = 0;
    glfwGetFramebufferSize(m_window, &fbWidth, &fbHeight);
    glViewport(0, 0, fbWidth, fbHeight);

    ImGui_ImplOpenGL3_NewFrame();
    const float bg[4] = { 0.08f, 0.09f, 0.10f, 1.0f };
    glClearColor(bg[0], bg[1], bg[2], bg[3]);
    glClear(GL_COLOR_BUFFER_BIT);
}

void RenderGL::renderDrawData(ImDrawData* drawData)
{
    ImGui_ImplOpenGL3_RenderDrawData(drawData);
}

void RenderGL::present()
{
    glfwSwapBuffers(m_window);
}

} // namespace bootroll
