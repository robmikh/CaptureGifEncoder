#pragma once
#include "FrameCompositor.h"
#include "TextureDiffer.h"

class GifEncoder
{
public:
    GifEncoder(
        winrt::com_ptr<ID3D11Device> const& d3dDevice,
        winrt::com_ptr<ID3D11DeviceContext> const& d3dContext,
        winrt::Windows::Storage::Streams::IRandomAccessStream const& stream,
        winrt::Windows::Graphics::SizeInt32 gifSize);

    bool ProcessFrame(winrt::Windows::Graphics::Capture::Direct3D11CaptureFrame const& frame);

    winrt::Windows::Foundation::IAsyncAction StopEncodingAsync();

private:
    struct GifFrameImage
    {
        std::vector<byte> Bytes;
        DiffRect Rect = {};
        winrt::Windows::Foundation::TimeSpan TimeStamp = {};

        GifFrameImage(std::vector<byte>&& bytes, DiffRect const& rect, winrt::Windows::Foundation::TimeSpan const& timeStamp)
        {
            Bytes = std::move(bytes);
            Rect = rect;
            TimeStamp = timeStamp;
        }
    };

    static winrt::Windows::Foundation::IAsyncOperation<winrt::Windows::Graphics::Imaging::BitmapEncoder> CreateWgiEncoderAsync(winrt::Windows::Storage::Streams::IRandomAccessStream stream);

    winrt::Windows::Foundation::IAsyncOperation<bool> ProcessFrameAsync(ComposedFrame const& composedFrame, bool force);
    winrt::Windows::Foundation::IAsyncAction EncodeFrameAsync(std::shared_ptr<GifFrameImage> frame, winrt::Windows::Foundation::TimeSpan currentTime);

private:
    winrt::com_ptr<ID3D11DeviceContext> m_d3dContext;
    winrt::Windows::Graphics::Imaging::BitmapEncoder m_encoder{ nullptr };
    winrt::com_ptr<ID3D11Texture2D> m_stagingTexture;
    std::unique_ptr<FrameCompositor> m_frameCompositor;
    std::unique_ptr<TextureDiffer> m_textureDiffer;
    winrt::Windows::Graphics::SizeInt32 m_gifSize = {};
    winrt::Windows::Foundation::TimeSpan m_lastTimeStamp = {};
    winrt::Windows::Foundation::TimeSpan m_lastCandidateTimeStamp = {};
    uint64_t frameCount = 0;
    RECT m_rect = {};
    std::shared_ptr<GifFrameImage> m_previousFrame;
    bool m_firstSubmittedFrame = true;
};