#pragma once

struct ComposedFrame
{
    winrt::com_ptr<ID3D11Texture2D> Texture;
    winrt::Windows::Foundation::TimeSpan SystemRelativeTime = {};
};

class FrameCompositor
{
public:
    FrameCompositor(
        winrt::com_ptr<ID3D11Device> const& d3dDevice,
        winrt::com_ptr<ID3D11DeviceContext> const& d3dContext,
        winrt::Windows::Graphics::SizeInt32 frameSize);

    ComposedFrame ProcessFrame(winrt::Windows::Graphics::Capture::Direct3D11CaptureFrame const& frame);
    ComposedFrame RepeatFrame(winrt::Windows::Foundation::TimeSpan systemRelativeTime);

private:
    winrt::com_ptr<ID3D11DeviceContext> m_d3dContext;
    winrt::com_ptr<ID3D11Texture2D> m_outputTexture;
    winrt::com_ptr<ID3D11RenderTargetView> m_outputRTV;
};