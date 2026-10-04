#include "platform/win/RenderDX11.h"
#include "imgui.h"
#include "backends/imgui_impl_dx11.h"

using Microsoft::WRL::ComPtr;

namespace bootroll {

RenderDX11::~RenderDX11()
{
    shutdown();
}

void RenderDX11::shutdown()
{
    if (m_device) {
        // Must happen before ImGui::DestroyContext(), otherwise the backend
        // touches a freed context (access violation on exit).
        ImGui_ImplDX11_Shutdown();
        m_mainRenderTargetView.Reset();
        m_context.Reset();
        m_swapChain.Reset();
        m_device.Reset();
    }
}

bool RenderDX11::init(HWND hwnd)
{
    m_hwnd = hwnd;

    DXGI_SWAP_CHAIN_DESC sd = {};
    sd.BufferDesc.Width = 0;
    sd.BufferDesc.Height = 0;
    sd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    sd.SampleDesc.Count = 1;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.BufferCount = 2;
    sd.OutputWindow = hwnd;
    sd.Windowed = TRUE;
    sd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;
    sd.Flags = 0;

    UINT createFlags = 0;
#ifdef _DEBUG
    createFlags |= D3D11_CREATE_DEVICE_DEBUG;
#endif

    // Hardware first, WARP fallback (WinPE often has no GPU driver).
    D3D_FEATURE_LEVEL fl = D3D_FEATURE_LEVEL_11_0;
    HRESULT hr = D3D11CreateDeviceAndSwapChain(
        nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, createFlags,
        nullptr, 0, D3D11_SDK_VERSION, &sd, m_swapChain.GetAddressOf(),
        m_device.GetAddressOf(), &fl, m_context.GetAddressOf());

    if (FAILED(hr)) {
        m_usingWarp = true;
        hr = D3D11CreateDeviceAndSwapChain(
            nullptr, D3D_DRIVER_TYPE_WARP, nullptr, createFlags,
            nullptr, 0, D3D11_SDK_VERSION, &sd, m_swapChain.GetAddressOf(),
            m_device.GetAddressOf(), &fl, m_context.GetAddressOf());
    }
    if (FAILED(hr)) {
        return false;
    }

    createRenderTarget();
    ImGui_ImplDX11_Init(m_device.Get(), m_context.Get());
    return true;
}

void RenderDX11::createRenderTarget()
{
    ComPtr<ID3D11Texture2D> backBuffer;
    if (SUCCEEDED(m_swapChain->GetBuffer(0, IID_PPV_ARGS(backBuffer.GetAddressOf())))) {
        m_device->CreateRenderTargetView(backBuffer.Get(), nullptr,
                                         m_mainRenderTargetView.GetAddressOf());
    }
}

void RenderDX11::resize(int width, int height)
{
    if (m_swapChain && width > 0 && height > 0) {
        m_mainRenderTargetView.Reset();
        m_swapChain->ResizeBuffers(0, width, height, DXGI_FORMAT_UNKNOWN, 0);
        createRenderTarget();
    }
}

void RenderDX11::newFrame()
{
    ImGui_ImplDX11_NewFrame();
    const float bg[4] = { 0.08f, 0.09f, 0.10f, 1.0f };
    m_context->OMSetRenderTargets(1, m_mainRenderTargetView.GetAddressOf(), nullptr);
    m_context->ClearRenderTargetView(m_mainRenderTargetView.Get(), bg);
}

void RenderDX11::renderDrawData(ImDrawData* drawData)
{
    ImGui_ImplDX11_RenderDrawData(drawData);
}

void RenderDX11::present()
{
    m_swapChain->Present(1, 0); // vsync
}

void RenderDX11::recreateFontTexture()
{
    // The font atlas was rebuilt on the CPU side (DPI change); refresh the GPU copy.
    ImGui_ImplDX11_InvalidateDeviceObjects();
    ImGui_ImplDX11_CreateDeviceObjects();
}

} // namespace bootroll
