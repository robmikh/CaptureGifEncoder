#include "pch.h"
#include "GifEncoder.h"

namespace winrt
{
    using namespace Windows::Foundation;
    using namespace Windows::Graphics;
    using namespace Windows::Graphics::Capture;
    using namespace Windows::Graphics::Imaging;
    using namespace Windows::Storage::Streams;
}

namespace util
{
    using namespace robmikh::common::uwp;
}

GifEncoder::GifEncoder(
    winrt::com_ptr<ID3D11Device> const& d3dDevice, 
    winrt::com_ptr<ID3D11DeviceContext> const& d3dContext,
    winrt::IRandomAccessStream const& stream,
    winrt::SizeInt32 gifSize)
{
    m_d3dContext = d3dContext;
    m_gifSize = gifSize;

    m_encoder = CreateWgiEncoderAsync(stream).get();

    // Create our staging texture
    D3D11_TEXTURE2D_DESC description = {};
    description.Width = gifSize.Width;
    description.Height = gifSize.Height;
    description.MipLevels = 1;
    description.ArraySize = 1;
    description.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    description.SampleDesc.Count = 1;
    description.SampleDesc.Quality = 0;
    description.Usage = D3D11_USAGE_STAGING;
    description.BindFlags = 0;
    description.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    winrt::check_hresult(d3dDevice->CreateTexture2D(&description, nullptr, m_stagingTexture.put()));

    // Setup our frame compositor and texture differ
    m_frameCompositor = std::make_unique<FrameCompositor>(d3dDevice, d3dContext, gifSize);
    m_textureDiffer = std::make_unique<TextureDiffer>(d3dDevice, d3dContext, gifSize);
}

bool GifEncoder::ProcessFrame(winrt::Direct3D11CaptureFrame const& frame)
{
    auto timeStamp = frame.SystemRelativeTime();

    auto firstFrame = false;

    // Compute frame delta
    if (m_lastTimeStamp.count() == 0)
    {
        m_lastTimeStamp = timeStamp;
        firstFrame = true;
    }
    auto timeStampDelta = timeStamp - m_lastTimeStamp;

    // Throttle frame processing to 30fps
    if (!firstFrame && timeStampDelta < std::chrono::milliseconds(33))
    {
        return false;
    }
    m_lastCandidateTimeStamp = timeStamp;

    auto composedFrame = m_frameCompositor->ProcessFrame(frame);

    return ProcessFrameAsync(composedFrame, false).get();
}

winrt::IAsyncAction GifEncoder::StopEncodingAsync()
{
    // Repeat the last frame
    auto composedFrame = m_frameCompositor->RepeatFrame(m_lastCandidateTimeStamp);
    co_await ProcessFrameAsync(composedFrame, true);

    co_await m_encoder.FlushAsync();
}

winrt::IAsyncOperation<winrt::BitmapEncoder> GifEncoder::CreateWgiEncoderAsync(
    winrt::IRandomAccessStream stream)
{
    // Setup our encoder
    auto encoder = co_await winrt::BitmapEncoder::CreateAsync(winrt::BitmapEncoder::GifEncoderId(), stream);
    auto containerProperties = encoder.BitmapContainerProperties();
    // Write the application block
    // http://www.vurdalakov.net/misc/gif/netscape-looping-application-extension
    std::string text("NETSCAPE2.0");
    std::vector<uint8_t> chars(text.begin(), text.end());
    WINRT_VERIFY(chars.size() == 11);
    co_await containerProperties.SetPropertiesAsync(
        {
            { L"/appext/application", winrt::BitmapTypedValue(winrt::PropertyValue::CreateUInt8Array(chars), winrt::PropertyType::UInt8Array) },
            // The first value is the size of the block, which is the fixed value 3.
            // The second value is the looping extension, which is the fixed value 1.
            // The third and fourth values comprise an unsigned 2-byte integer (little endian).
            //     The value of 0 means to loop infinitely.
            // The final value is the block terminator, which is the fixed value 0.
            { L"/appext/data", winrt::BitmapTypedValue(winrt::PropertyValue::CreateUInt8Array({ 3, 1, 0, 0, 0 }), winrt::PropertyType::UInt8Array) },
        });
    co_return encoder;
}

winrt::IAsyncOperation<bool> GifEncoder::ProcessFrameAsync(ComposedFrame const& composedFrame, bool force)
{
    bool updated = false;

    auto diff = m_textureDiffer->ProcessFrame(composedFrame.Texture);
    if (force && !diff.has_value())
    {
        // Since there's no change, pick a small random part of the frame.
        diff = std::optional(DiffRect{ 0, 0, 5, 5 });
    }

    if (auto diffRect = diff)
    {
        auto timeStampDelta = composedFrame.SystemRelativeTime - m_lastTimeStamp;
        m_lastTimeStamp = composedFrame.SystemRelativeTime;

        // Inflate our rect to eliminate artifacts
        auto inflateAmount = 1;
        auto left = static_cast<uint32_t>(std::max(static_cast<int32_t>(diffRect->Left) - inflateAmount, 0));
        auto top = static_cast<uint32_t>(std::max(static_cast<int32_t>(diffRect->Top) - inflateAmount, 0));
        auto right = static_cast<uint32_t>(std::min(static_cast<int32_t>(diffRect->Right) + inflateAmount, m_gifSize.Width));
        auto bottom = static_cast<uint32_t>(std::min(static_cast<int32_t>(diffRect->Bottom) + inflateAmount, m_gifSize.Height));

        auto diffWidth = right - left;
        auto diffHeight = bottom - top;

        // Copy the relevant portion into our staging texture
        D3D11_BOX region = {};
        region.left = left;
        region.right = right;
        region.top = top;
        region.bottom = bottom;
        region.back = 1;
        m_d3dContext->CopySubresourceRegion(m_stagingTexture.get(), 0, left, top, 0, composedFrame.Texture.get(), 0, &region);

        // Copy the bytes from the staging texture
        D3D11_TEXTURE2D_DESC desc = {};
        m_stagingTexture->GetDesc(&desc);
        size_t bytesPerPixel = 4; // Assuming BGRA8
        D3D11_MAPPED_SUBRESOURCE mapped = {};
        winrt::check_hresult(m_d3dContext->Map(m_stagingTexture.get(), 0, D3D11_MAP_READ, 0, &mapped));
        // Textures can occupy more space in video memory than you might expect given
        // their size and pixel format. The RowPitch field in the D3D11_MAPPED_SUBRESOURCE
        // tells you how many bytes there are per "row".
        auto destStride = static_cast<size_t>(diffWidth) * bytesPerPixel;
        std::vector<byte> bytes(destStride * static_cast<size_t>(diffHeight), 0);
        auto source = reinterpret_cast<byte*>(mapped.pData);
        auto dest = bytes.data();
        source += (mapped.RowPitch * static_cast<size_t>(top)) + (static_cast<size_t>(left) * bytesPerPixel);
        for (auto i = 0; i < (int)diffHeight; i++)
        {
            memcpy(dest, source, destStride);

            source += mapped.RowPitch;
            dest += destStride;
        }
        m_d3dContext->Unmap(m_stagingTexture.get(), 0);

        auto frame = std::make_shared<GifFrameImage>(std::move(bytes), DiffRect{ left, top, right, bottom }, composedFrame.SystemRelativeTime);
        //co_await EncodeFrameAsync(frame, composedFrame.SystemRelativeTime + timeStampDelta);
        m_previousFrame.swap(frame);
        if (frame != nullptr)
        {
            auto currentTime = composedFrame.SystemRelativeTime;
            if (force)
            {
                currentTime += timeStampDelta;
            }
            co_await EncodeFrameAsync(frame, currentTime);
        }

        updated = true;
    }

    co_return updated;
}

winrt::IAsyncAction GifEncoder::EncodeFrameAsync(std::shared_ptr<GifFrameImage> frame, winrt::TimeSpan currentTime)
{
    auto frameWidth = frame->Rect.Right - frame->Rect.Left;
    auto frameHeight = frame->Rect.Bottom - frame->Rect.Top;

    auto frameDuration = currentTime - frame->TimeStamp;
    // Compute the frame delay
    auto millisconds = std::chrono::duration_cast<std::chrono::milliseconds>(frameDuration);
    // Use 10ms units
    auto frameDelay = millisconds.count() / 10;

    // Advance to the next frame (we don't do this at the end to avoid empty frames)
    if (frameCount > 0)
    {
        co_await m_encoder.GoToNextFrameAsync();
    }

    // Write our frame delay
    co_await m_encoder.BitmapProperties().SetPropertiesAsync(
        {
            { L"/grctlext/Delay", winrt::BitmapTypedValue(winrt::PropertyValue::CreateUInt16(static_cast<uint16_t>(frameDelay)), winrt::PropertyType::UInt16) },
            { L"/imgdesc/Left", winrt::BitmapTypedValue(winrt::PropertyValue::CreateUInt16(static_cast<uint16_t>(frame->Rect.Left)), winrt::PropertyType::UInt16) },
            { L"/imgdesc/Top", winrt::BitmapTypedValue(winrt::PropertyValue::CreateUInt16(static_cast<uint16_t>(frame->Rect.Top)), winrt::PropertyType::UInt16) },
        });

    // Write the frame to our image
    m_encoder.SetPixelData(
        winrt::BitmapPixelFormat::Bgra8,
        winrt::BitmapAlphaMode::Premultiplied,
        frameWidth,
        frameHeight,
        1.0,
        1.0,
        frame->Bytes);

    frameCount++;
}
