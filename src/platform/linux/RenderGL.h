#pragma once
// OpenGL3 renderer for ImGui (imgui_impl_opengl3 backend). Mesa llvmpipe
// provides the software fallback, mirroring the DX11 WARP fallback on Windows.
#include <GLFW/glfw3.h>

struct ImDrawData; // forward decl (imgui.h included by the .cpp)

namespace bootroll {

class RenderGL {
public:
    ~RenderGL();

    // The GLFW context must be current on the calling thread.
    bool init(GLFWwindow* window);
    // Shut down the ImGui renderer backend. Must be called before
    // ImGui::DestroyContext(); safe to call twice.
    void shutdown();
    // Nothing to reallocate for OpenGL (viewport is set every frame).
    void resize(int width, int height);
    void newFrame();
    void renderDrawData(ImDrawData* drawData);
    void present();

private:
    GLFWwindow* m_window = nullptr;
    bool m_initialized = false;
};

} // namespace bootroll
