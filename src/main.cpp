// Entry point: wires the platform layer, renderer and portable App.
// Windows: Win32 window + DX11 (wWinMain). Linux: GLFW window + OpenGL3 (main).
#include "app/App.h"
#include "app/CrashHandler.h"
#include "app/Settings.h"
#include "imgui.h"
#include "Version.h"

#include <cmath>

#ifdef _WIN32

#include "platform/win/PlatformWin.h"
#include "platform/win/RenderDX11.h"
#include "platform/win/Win32Window.h"

#include "backends/imgui_impl_win32.h"

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int nCmdShow)
{
    (void)nCmdShow;

    bootroll::installCrashHandler();

    auto platform = bootroll::createPlatform();

    // ImGui context must exist before App::init (theme/font setup touches GetIO).
    ImGui::CreateContext();

    bootroll::App app;
    if (!app.init(platform.get(), BOOTROLL_APP_VERSION)) {
        return 1;
    }

    bootroll::Win32Window window;
    const auto& settings = bootroll::Settings::instance();
    // Scale the default size once on first run; later runs replay saved pixels.
    const float dpi = platform->dpiScale();
    int winW = settings.windowWidth;
    int winH = settings.windowHeight;
    if (!settings.windowSizeFromUser) {
        winW = lroundf(winW * dpi);
        winH = lroundf(winH * dpi);
    }
    if (!window.create(instance, (L"Bootroll " + std::wstring(BOOTROLL_APP_VERSION_WIDE)).c_str(),
                       winW, winH)) {
        return 1;
    }

    bootroll::RenderDX11 renderer;
    if (!renderer.init(window.hwnd())) {
        return 1;
    }
    ImGui_ImplWin32_Init(window.hwnd());

    bool running = true;
    while (running) {
        running = window.pumpMessages();

        if (!window.minimized() &&
            (window.clientWidth() != ImGui::GetIO().DisplaySize.x ||
             window.clientHeight() != ImGui::GetIO().DisplaySize.y)) {
            renderer.resize(window.clientWidth(), window.clientHeight());
        }
        app.onResize(window.clientWidth(), window.clientHeight());

        // Follow per-monitor DPI changes: rebuild style + font atlas (CPU) and
        // the font texture (GPU) before the next frame.
        if (window.dpiScale() != app.dpiScale()) {
            app.setDpiScale(window.dpiScale());
            renderer.recreateFontTexture();
        }

        ImGui_ImplWin32_NewFrame();
        renderer.newFrame();
        ImGui::NewFrame();

        app.drawFrame();

        ImGui::Render();
        renderer.renderDrawData(ImGui::GetDrawData());
        renderer.present();

        // Screens request shutdown via app.requestExit() (e.g. after spawning
        // an elevated replacement instance).
        if (app.exitRequested()) {
            running = false;
        }
    }

    // Backends must shut down before ImGui::DestroyContext(). The DX11
    // renderer's destructor runs too late (after context teardown), which
    // crashed on every exit - hence the explicit shutdown() here.
    renderer.shutdown();
    ImGui_ImplWin32_Shutdown();
    app.shutdown();
    ImGui::DestroyContext();
    return 0;
}

#else // Linux: GLFW + OpenGL3 (M8_PLAN §2)

#include "platform/linux/GlfwWindow.h"
#include "platform/linux/PlatformLinux.h"
#include "platform/linux/RenderGL.h"

#include "backends/imgui_impl_glfw.h"

int main(int argc, char** argv)
{
    (void)argc;
    (void)argv;

    bootroll::installCrashHandler();

    auto platform = bootroll::createPlatform();

    // ImGui context must exist before App::init (theme/font setup touches GetIO).
    ImGui::CreateContext();

    bootroll::App app;
    if (!app.init(platform.get(), BOOTROLL_APP_VERSION)) {
        return 1;
    }

    bootroll::GlfwWindow window;
    const auto& settings = bootroll::Settings::instance();
    // Default size: Wayland sizes are in points (the compositor applies the
    // physical scale) and X11 initial sizing follows the monitor via
    // GLFW_SCALE_TO_MONITOR, so unlike the Windows branch there is no manual
    // DPI pre-scaling here. Later runs replay saved pixels on both platforms.
    if (!window.create((std::string("Bootroll ") + BOOTROLL_APP_VERSION).c_str(),
                       settings.windowWidth, settings.windowHeight)) {
        return 1;
    }

    bootroll::RenderGL renderer;
    if (!renderer.init(window.handle())) {
        return 1;
    }
    ImGui_ImplGlfw_InitForOpenGL(window.handle(), true); // installs callbacks

    app.log(std::string("window platform: ") +
            (window.isWayland() ? "wayland" : "x11"));

    // First-frame DPI sync: App::init applied the pre-window default scale.
    if (window.dpiScale() != app.dpiScale()) {
        app.setDpiScale(window.dpiScale());
        renderer.recreateFontTexture();
    }

    bool running = true;
    while (running) {
        running = window.pumpMessages();

        if (!window.minimized() &&
            (window.clientWidth() != ImGui::GetIO().DisplaySize.x ||
             window.clientHeight() != ImGui::GetIO().DisplaySize.y)) {
            renderer.resize(window.clientWidth(), window.clientHeight());
        }
        app.onResize(window.clientWidth(), window.clientHeight());

        // Follow content-scale changes (monitor switch / compositor scale):
        // rebuild style + font atlas (CPU) and the font texture (GPU), the
        // same rhythm as the Win32 per-monitor-DPI block above.
        if (window.dpiScale() != app.dpiScale()) {
            app.setDpiScale(window.dpiScale());
            renderer.recreateFontTexture();
        }

        ImGui_ImplGlfw_NewFrame();
        renderer.newFrame();
        ImGui::NewFrame();

        // Every frame, same rhythm as Windows: disk enumeration results are
        // harvested here (App::drawFrame -> pumpDiskEnum).
        app.drawFrame();

        ImGui::Render();
        renderer.renderDrawData(ImGui::GetDrawData());
        renderer.present();

        // Screens request shutdown via app.requestExit() (e.g. after spawning
        // an elevated replacement instance).
        if (app.exitRequested()) {
            running = false;
        }
    }

    // Exit-order red line (LESSONS.md #2): renderer backend first, then the
    // platform backend, then the App, then the ImGui context.
    renderer.shutdown();
    ImGui_ImplGlfw_Shutdown();
    app.shutdown();
    ImGui::DestroyContext();
    return 0;
}

#endif
