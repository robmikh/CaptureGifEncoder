#include "pch.h"
#include "FrameCompositor.h"

namespace winrt
{
    using namespace Windows::Graphics;
    using namespace Windows::Graphics::Capture;
}

float CLEARCOLOR[] = { 0.0f, 0.0f, 0.0f, 1.0f }; // RGBA

FrameCompositor::FrameCompositor(
    winrt::com_ptr<ID3D11Device> const& d3dDevice, 
    winrt::com_ptr<ID3D11DeviceContext> const& d3dContext, 
    winrt::SizeInt32 frameSize)
{
    winrt::com_ptr<ID3D11Texture2D> outputTexture;
    D3D11_TEXTURE2D_DESC textureDesc = {};
    textureDesc.Width = static_cast<uint32_t>(frameSize.Width);
    textureDesc.Height = static_cast<uint32_t>(frameSize.Height);
    textureDesc.MipLevels = 1;
    textureDesc.ArraySize = 1;
    textureDesc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    textureDesc.SampleDesc.Count = 1;
    textureDesc.Usage = D3D11_USAGE_DEFAULT;
    textureDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
    winrt::check_hresult(d3dDevice->CreateTexture2D(&textureDesc, nullptr, outputTexture.put()));

    winrt::com_ptr<ID3D11RenderTargetView> outputRTV;
    winrt::check_hresult(d3dDevice->CreateRenderTargetView(outputTexture.get(), nullptr, outputRTV.put()));

    m_d3dContext = d3dContext;
    m_outputTexture = outputTexture;
    m_outputRTV = outputRTV;
}

ComposedFrame FrameCompositor::ProcessFrame(winrt::Direct3D11CaptureFrame const& frame)
{
    auto frameTexture = GetDXGIInterfaceFromObject<ID3D11Texture2D>(frame.Surface());
    auto systemRelativeTime = frame.SystemRelativeTime();
    auto contentSize = frame.ContentSize();

    D3D11_TEXTURE2D_DESC desc = {};
    frameTexture->GetDesc(&desc);

    m_d3dContext->ClearRenderTargetView(m_outputRTV.get(), CLEARCOLOR);

    // In order to support window resizing, we need to only copy out the part of
    // the buffer that contains the window. If the window is smaller than the buffer,
    // then it's a straight forward copy using the ContentSize. If the window is larger,
    // we need to clamp to the size of the buffer. For simplicity, we always clamp.
    auto width = std::clamp(static_cast<uint32_t>(contentSize.Width), (uint32_t)0, desc.Width);
    auto height = std::clamp(static_cast<uint32_t>(contentSize.Height), (uint32_t)0, desc.Height);

    D3D11_BOX region = {};
    region.left = 0;
    region.right = width;
    region.top = 0;
    region.bottom = height;
    region.back = 1;


    m_d3dContext->CopySubresourceRegion(m_outputTexture.get(), 0, 0, 0, 0, frameTexture.get(), 0, &region);

    ComposedFrame composedFrame = {};
    composedFrame.Texture = m_outputTexture;
    composedFrame.SystemRelativeTime = systemRelativeTime;
    return composedFrame;
}

ComposedFrame FrameCompositor::RepeatFrame(winrt::Windows::Foundation::TimeSpan systemRelativeTime)
{
    ComposedFrame composedFrame = {};
    composedFrame.Texture = m_outputTexture;
    composedFrame.SystemRelativeTime = systemRelativeTime;
    return composedFrame;
}
