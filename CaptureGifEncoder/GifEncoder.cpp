#include "pch.h"
#include "GifEncoder.h"

namespace winrt
{
    using namespace Windows::Graphics;
    using namespace Windows::Graphics::Capture;
    using namespace Windows::Storage::Streams;
}

namespace util
{
    using namespace robmikh::common::uwp;
}

GifEncoder::GifEncoder(
    winrt::com_ptr<ID3D11Device> const& d3dDevice, 
    winrt::com_ptr<ID3D11DeviceContext> const& d3dContext,
    winrt::com_ptr<ID2D1Device> const& d2dDevice,
    winrt::com_ptr<IWICImagingFactory2> const& wicFactory,
    winrt::IRandomAccessStream const& stream,
    winrt::SizeInt32 gifSize)
{
    m_gifSize = gifSize;
    m_d3dDevice = d3dDevice;
    winrt::check_hresult(d2dDevice->CreateDeviceContext(D2D1_DEVICE_CONTEXT_OPTIONS_NONE, m_d2dContext.put()));
    auto abiStream = util::CreateStreamFromRandomAccessStream(stream);

    // Setup WIC to encode a gif
    winrt::check_hresult(wicFactory->CreateEncoder(GUID_ContainerFormatGif, nullptr, m_encoder.put()));
    winrt::check_hresult(m_encoder->Initialize(abiStream.get(), WICBitmapEncoderNoCache));
    winrt::check_hresult(wicFactory->CreateImageEncoder(d2dDevice.get(), m_imageEncoder.put()));

    // Write the application block
    // http://www.vurdalakov.net/misc/gif/netscape-looping-application-extension
    winrt::com_ptr<IWICMetadataQueryWriter> metadata;
    winrt::check_hresult(m_encoder->GetMetadataQueryWriter(metadata.put()));
    {
        PROPVARIANT value = {};
        value.vt = VT_UI1 | VT_VECTOR;
        value.caub.cElems = 11;
        std::string text("NETSCAPE2.0");
        std::vector<uint8_t> chars(text.begin(), text.end());
        WINRT_VERIFY(chars.size() == 11);
        value.caub.pElems = chars.data();
        winrt::check_hresult(metadata->SetMetadataByName(L"/appext/application", &value));
    }
    {
        PROPVARIANT value = {};
        value.vt = VT_UI1 | VT_VECTOR;
        value.caub.cElems = 5;
        // The first value is the size of the block, which is the fixed value 3.
        // The second value is the looping extension, which is the fixed value 1.
        // The third and fourth values comprise an unsigned 2-byte integer (little endian).
        //     The value of 0 means to loop infinitely.
        // The final value is the block terminator, which is the fixed value 0.
        std::vector<uint8_t> data({ 3, 1, 0, 0, 0 });
        value.caub.pElems = data.data();
        winrt::check_hresult(metadata->SetMetadataByName(L"/appext/data", &value));
    }

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

    return ProcessFrame(composedFrame, false);
}

void GifEncoder::StopEncoding()
{
    // Repeat the last frame
    auto composedFrame = m_frameCompositor->RepeatFrame(m_lastCandidateTimeStamp);
    ProcessFrame(composedFrame, true);

    winrt::check_hresult(m_encoder->Commit());
}

bool GifEncoder::ProcessFrame(ComposedFrame const& composedFrame, bool force)
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

        // Create the frame
        auto textureCopy = util::CopyD3DTexture(m_d3dDevice, composedFrame.Texture, false);
        auto frame = std::make_shared<GifFrameImage>(textureCopy, DiffRect { left, top, right, bottom }, composedFrame.SystemRelativeTime);

        // Encode the frame
        m_previousFrame.swap(frame);
        if (frame != nullptr)
        {
            auto currentTime = composedFrame.SystemRelativeTime;
            if (force)
            {
                currentTime += timeStampDelta;
            }
            EncodeFrame(frame, currentTime);
        }

        updated = true;
    }

    return updated;
}

void GifEncoder::EncodeFrame(std::shared_ptr<GifFrameImage> const& frame, winrt::Windows::Foundation::TimeSpan const& currentTime)
{
    auto diffWidth = frame->Rect.Right - frame->Rect.Left;
    auto diffHeight = frame->Rect.Bottom - frame->Rect.Top;

    auto frameDuration = currentTime - frame->TimeStamp;
    // Compute the frame delay
    auto millisconds = std::chrono::duration_cast<std::chrono::milliseconds>(frameDuration);
    // Use 10ms units
    auto frameDelay = millisconds.count() / 10;

    // Create a D2D bitmap
    winrt::com_ptr<ID2D1Bitmap1> d2dBitmap;
    winrt::check_hresult(m_d2dContext->CreateBitmapFromDxgiSurface(frame->Texture.as<IDXGISurface>().get(), nullptr, d2dBitmap.put()));

    // Setup our WIC frame (note the matching pixel format)
    winrt::com_ptr<IWICBitmapFrameEncode> wicFrame;
    winrt::check_hresult(m_encoder->CreateNewFrame(wicFrame.put(), nullptr));
    winrt::check_hresult(wicFrame->Initialize(nullptr));
    auto wicPixelFormat = GUID_WICPixelFormat32bppBGRA;
    winrt::check_hresult(wicFrame->SetPixelFormat(&wicPixelFormat));

    // Write frame metadata
    winrt::com_ptr<IWICMetadataQueryWriter> metadata;
    winrt::check_hresult(wicFrame->GetMetadataQueryWriter(metadata.put()));
    // Delay
    {
        PROPVARIANT delayValue = {};
        delayValue.vt = VT_UI2;
        delayValue.uiVal = static_cast<unsigned short>(frameDelay);
        winrt::check_hresult(metadata->SetMetadataByName(L"/grctlext/Delay", &delayValue));
    }
    // Left
    {
        PROPVARIANT metadataValue = {};
        metadataValue.vt = VT_UI2;
        metadataValue.uiVal = static_cast<unsigned short>(frame->Rect.Left);
        winrt::check_hresult(metadata->SetMetadataByName(L"/imgdesc/Left", &metadataValue));
    }
    // Top
    {
        PROPVARIANT metadataValue = {};
        metadataValue.vt = VT_UI2;
        metadataValue.uiVal = static_cast<unsigned short>(frame->Rect.Top);
        winrt::check_hresult(metadata->SetMetadataByName(L"/imgdesc/Top", &metadataValue));
    }

    // Write the frame to our image (this must come after you write the metadata)
    WICImageParameters frameParams = {};
    frameParams.PixelFormat.format = DXGI_FORMAT_B8G8R8A8_UNORM;
    frameParams.PixelFormat.alphaMode = D2D1_ALPHA_MODE_PREMULTIPLIED;
    frameParams.DpiX = 96.0f;
    frameParams.DpiY = 96.0f;
    frameParams.Left = static_cast<float>(frame->Rect.Left);
    frameParams.Top = static_cast<float>(frame->Rect.Top);
    frameParams.PixelWidth = diffWidth;
    frameParams.PixelHeight = diffHeight;
    winrt::check_hresult(m_imageEncoder->WriteFrame(d2dBitmap.get(), wicFrame.get(), &frameParams));
    winrt::check_hresult(wicFrame->Commit());
}
