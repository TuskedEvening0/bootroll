#pragma once
// D3D11 renderer for ImGui with WARP (software) fallback for WinPE / headless GPU.
#include <d3d11.h>
#include <wrl/client.h>

struct ImDrawData; // forward decl (imgui.h included by the .cpp)

namespace bootroll {

class RenderDX11 {
public:
    ~RenderDX11();

    bool init(HWND hwnd);
    // Shut down the ImGui renderer backend and release D3D resources. Must be
    // called before ImGui::DestroyContext(); safe to call twice.
    void shutdown();
    // Recreate the render target view on resize (no-op while minimized).
    void resize(int width, int height);
    void newFrame();
    void renderDrawData(ImDrawData* drawData);
    void present();
    // Rebuild the GPU font texture after the font atlas was rebuilt (DPI change).
    void recreateFontTexture();

    ID3D11Device* device() const { return m_device.Get(); }

private:
    void createRenderTarget();

    HWND m_hwnd = nullptr;
    Microsoft::WRL::ComPtr<ID3D11Device> m_device;
    Microsoft::WRL::ComPtr<ID3D11DeviceContext> m_context;
    Microsoft::WRL::ComPtr<IDXGISwapChain> m_swapChain;
    Microsoft::WRL::ComPtr<ID3D11RenderTargetView> m_mainRenderTargetView;
    bool m_usingWarp = false;
};

} // namespace bootroll
