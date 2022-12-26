#pragma once
#include "FrameCompositor.h"
#include "TextureDiffer.h"

class GifEncoder
{
public:
    GifEncoder(
        winrt::com_ptr<ID3D11Device> const& d3dDevice,
        winrt::com_ptr<ID3D11DeviceContext> const& d3dContext,
        winrt::com_ptr<ID2D1Device> const& d2dDevice,
        winrt::com_ptr<IWICImagingFactory2> const& wicFactory,
        winrt::Windows::Storage::Streams::IRandomAccessStream const& stream,
        winrt::Windows::Graphics::SizeInt32 gifSize);
    
    bool ProcessFrame(winrt::Windows::Graphics::Capture::Direct3D11CaptureFrame const& frame);

    void StopEncoding();

private:
    struct GifFrameImage
    {
        winrt::com_ptr<ID3D11Texture2D> Texture;
        DiffRect Rect = {};
        winrt::Windows::Foundation::TimeSpan TimeStamp = {};

        GifFrameImage(winrt::com_ptr<ID3D11Texture2D> const& texture, DiffRect const& rect, winrt::Windows::Foundation::TimeSpan const& timeStamp)
        {
            Texture = texture;
            Rect = rect;
            TimeStamp = timeStamp;
        }
    };

    bool ProcessFrame(ComposedFrame const& composedFrame, bool force);
    void EncodeFrame(std::shared_ptr<GifFrameImage> const& frame, winrt::Windows::Foundation::TimeSpan const& currentTime);

private:
    winrt::com_ptr<ID3D11Device> m_d3dDevice;
    winrt::com_ptr<ID2D1DeviceContext> m_d2dContext;
    winrt::com_ptr<IWICBitmapEncoder> m_encoder;
    winrt::com_ptr<IWICImageEncoder> m_imageEncoder;
    std::unique_ptr<FrameCompositor> m_frameCompositor;
    std::unique_ptr<TextureDiffer> m_textureDiffer;
    winrt::Windows::Graphics::SizeInt32 m_gifSize = {};
    winrt::Windows::Foundation::TimeSpan m_lastTimeStamp = {};
    winrt::Windows::Foundation::TimeSpan m_lastCandidateTimeStamp = {};
    uint64_t frameCount = 0;
    std::shared_ptr<GifFrameImage> m_previousFrame;
};