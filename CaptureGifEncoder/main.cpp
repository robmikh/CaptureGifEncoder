#include "pch.h"
#include "WindowInfo.h"
#include "FrameCompositor.h"
#include "TextureDiffer.h"

namespace winrt
{
    using namespace Windows::Foundation;
    using namespace Windows::Storage;
    using namespace Windows::System;
    using namespace Windows::Graphics;
    using namespace Windows::Graphics::Capture;
    using namespace Windows::Graphics::DirectX;
    using namespace Windows::Graphics::DirectX::Direct3D11;
}

namespace util
{
    using namespace robmikh::common::desktop;
    using namespace robmikh::common::uwp;
}

inline auto CreateD3DDevice(UINT flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT)
{
    flags |= D3D11_CREATE_DEVICE_DEBUG;

    winrt::com_ptr<ID3D11Device> device;
    HRESULT hr = util::CreateD3DDevice(D3D_DRIVER_TYPE_HARDWARE, flags, device);
    if (DXGI_ERROR_UNSUPPORTED == hr)
    {
        hr = util::CreateD3DDevice(D3D_DRIVER_TYPE_WARP, flags, device);
    }

    winrt::check_hresult(hr);
    return device;
}

winrt::IAsyncAction MainAsync(std::vector<std::wstring> const& args)
{
    // Arg validation
    if (args.size() <= 0)
    {
        wprintf(L"Invalid input!\n");
        co_return;
    }
    auto windowQuery = args[0];

    // Find the window we want to record
    auto matchedWindows = FindWindowsByTitle(windowQuery);
    if (matchedWindows.size() <= 0)
    {
        wprintf(L"Couldn't find a window that contains '%s'!\n", windowQuery.c_str());
        co_return;
    }
    auto window = matchedWindows[0];
    wprintf(L"Using '%s'\n", window.Title.c_str());

    // Init D3D, D2D, and WIC
    auto d3dDevice = CreateD3DDevice();
    winrt::com_ptr<ID3D11DeviceContext> d3dContext;
    d3dDevice->GetImmediateContext(d3dContext.put());
    auto device = CreateDirect3DDevice(d3dDevice.as<IDXGIDevice>().get());
    auto d2dFactory = util::CreateD2DFactory();
    auto d2dDevice = util::CreateD2DDevice(d2dFactory, d3dDevice);
    winrt::com_ptr<ID2D1DeviceContext> d2dContext;
    winrt::check_hresult(d2dDevice->CreateDeviceContext(D2D1_DEVICE_CONTEXT_OPTIONS_NONE, d2dContext.put()));
    auto wicFactory = util::CreateWICFactory();

    // TODO: Use args to determine file name/path
    auto currentPath = std::filesystem::current_path();
    auto folder = co_await winrt::StorageFolder::GetFolderFromPathAsync(currentPath.wstring());
    auto file = co_await folder.CreateFileAsync(L"test.gif", winrt::CreationCollisionOption::ReplaceExisting);

    auto stream = co_await file.OpenAsync(winrt::FileAccessMode::ReadWrite);
    auto abiStream = util::CreateStreamFromRandomAccessStream(stream);

    // Setup WIC to encode a gif
    winrt::com_ptr<IWICBitmapEncoder> encoder;
    winrt::check_hresult(wicFactory->CreateEncoder(GUID_ContainerFormatGif, nullptr, encoder.put()));
    winrt::check_hresult(encoder->Initialize(abiStream.get(), WICBitmapEncoderNoCache));

    winrt::com_ptr<IWICImageEncoder> imageEncoder;
    winrt::check_hresult(wicFactory->CreateImageEncoder(d2dDevice.get(), imageEncoder.put()));

    // Write the application block
    // http://www.vurdalakov.net/misc/gif/netscape-looping-application-extension
    winrt::com_ptr<IWICMetadataQueryWriter> metadata;
    winrt::check_hresult(encoder->GetMetadataQueryWriter(metadata.put()));
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

    // Setup Windows.Graphics.Capture
    auto item = util::CreateCaptureItemForWindow(window.WindowHandle);
    auto itemSize = item.Size();
    auto framePool = winrt::Direct3D11CaptureFramePool::CreateFreeThreaded(
        device,
        winrt::DirectXPixelFormat::B8G8R8A8UIntNormalized,
        1,
        itemSize);
    auto session = framePool.CreateCaptureSession(item);

    // Setup our frame compositor and differ
    auto frameCompositor = std::make_shared<FrameCompositor>(d3dDevice, d3dContext, itemSize);
    auto textureDiffer = std::make_shared<TextureDiffer>(d3dDevice, d3dContext, itemSize);

    // Create a texture that will hold the frame we'll be encoding
    winrt::com_ptr<ID3D11Texture2D> gifTexture;
    {
        D3D11_TEXTURE2D_DESC description = {};
        description.Width = itemSize.Width;
        description.Height = itemSize.Height;
        description.MipLevels = 1;
        description.ArraySize = 1;
        description.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        description.SampleDesc.Count = 1;
        description.SampleDesc.Quality = 0;
        description.Usage = D3D11_USAGE_STAGING;
        description.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        winrt::check_hresult(d3dDevice->CreateTexture2D(&description, nullptr, gifTexture.put()));
    }

    // Encode frames as they arrive. Because we created our frame pool using 
    // Direct3D11CaptureFramePool::CreateFreeThreaded, this lambda will fire on a different thread
    // than our current one. If you'd like the callback to fire on your thread, create the frame pool
    // using Direct3D11CaptureFramePool::Create and make sure your thread has a DispatcherQueue and you
    // are pumping messages.
    auto lastTimeStamp = winrt::TimeSpan{ 0 };
    framePool.FrameArrived([itemSize, d3dContext, d2dContext, gifTexture, encoder, imageEncoder, frameCompositor, textureDiffer, &lastTimeStamp](auto& framePool, auto&)
    {
        auto frame = framePool.TryGetNextFrame();
        auto timeStamp = frame.SystemRelativeTime();

        auto firstFrame = false;

        // Compute frame delta
        if (lastTimeStamp.count() == 0)
        {
            lastTimeStamp = timeStamp;
            firstFrame = true;
        }
        auto timeStampDelta = timeStamp - lastTimeStamp;

        // Throttle frame processing to 30fps
        if (!firstFrame && timeStampDelta < std::chrono::milliseconds(33))
        {
            return;
        }

        auto composedFrame = frameCompositor->ProcessFrame(frame);
        auto diff = textureDiffer->ProcessFrame(composedFrame.Texture);

        if (auto diffRect = diff)
        {
            lastTimeStamp = timeStamp;

            // Inflate our rect to eliminate artifacts
            auto inflateAmount = 1;
            auto left = static_cast<uint32_t>(std::max(static_cast<int32_t>(diffRect->Left) - inflateAmount, 0));
            auto top = static_cast<uint32_t>(std::max(static_cast<int32_t>(diffRect->Top) - inflateAmount, 0));
            auto right = static_cast<uint32_t>(std::min(static_cast<int32_t>(diffRect->Right) + inflateAmount, itemSize.Width));
            auto bottom = static_cast<uint32_t>(std::min(static_cast<int32_t>(diffRect->Bottom) + inflateAmount, itemSize.Height));

            auto diffWidth = right - left;
            auto diffHeight = bottom - top;

            // Copy the relevant portion into our gif texture
            D3D11_BOX region = {};
            region.left = left;
            region.right = right;
            region.top = top;
            region.bottom = bottom;
            region.back = 1;
            d3dContext->CopySubresourceRegion(gifTexture.get(), 0, left, top, 0, composedFrame.Texture.get(), 0, &region);

            // Copy the bytes from the staging texture
            D3D11_TEXTURE2D_DESC desc = {};
            gifTexture->GetDesc(&desc);
            size_t bytesPerPixel = 4; // Assuming BGRA8
            D3D11_MAPPED_SUBRESOURCE mapped = {};
            winrt::check_hresult(d3dContext->Map(gifTexture.get(), 0, D3D11_MAP_READ, 0, &mapped));
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
            d3dContext->Unmap(gifTexture.get(), 0);

            // Compute the frame delay
            auto millisconds = std::chrono::duration_cast<std::chrono::milliseconds>(timeStampDelta);
            // Use 10ms units
            auto frameDelay = millisconds.count() / 10;

            // Create a D2D bitmap from the bytes
            // TODO: How to hand bytes directly to WIC?
            D2D1_BITMAP_PROPERTIES1 bitmapProperties = {};
            bitmapProperties.pixelFormat.format = DXGI_FORMAT_B8G8R8A8_UNORM;
            bitmapProperties.pixelFormat.alphaMode = D2D1_ALPHA_MODE_PREMULTIPLIED;
            bitmapProperties.dpiX = 96.0f;
            bitmapProperties.dpiY = 96.0f;
            winrt::com_ptr<ID2D1Bitmap1> d2dBitmap;
            winrt::check_hresult(d2dContext->CreateBitmap(D2D_SIZE_U{ diffWidth, diffHeight }, reinterpret_cast<void*>(bytes.data()), static_cast<uint32_t>(destStride), &bitmapProperties, d2dBitmap.put()));
            
            // Setup our WIC frame (note the matching pixel format)
            winrt::com_ptr<IWICBitmapFrameEncode> wicFrame;
            winrt::check_hresult(encoder->CreateNewFrame(wicFrame.put(), nullptr));
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
                metadataValue.uiVal = static_cast<unsigned short>(left);
                winrt::check_hresult(metadata->SetMetadataByName(L"/imgdesc/Left", &metadataValue));
            }
            // Top
            {
                PROPVARIANT metadataValue = {};
                metadataValue.vt = VT_UI2;
                metadataValue.uiVal = static_cast<unsigned short>(top);
                winrt::check_hresult(metadata->SetMetadataByName(L"/imgdesc/Top", &metadataValue));
            }

            // Write the frame to our image (this must come after you write the metadata)
            winrt::check_hresult(imageEncoder->WriteFrame(d2dBitmap.get(), wicFrame.get(), nullptr));
            winrt::check_hresult(wicFrame->Commit());
        }
    });

    session.StartCapture();
    // TODO: enable timed recording through a flag
    //co_await std::chrono::seconds(5);
    wprintf(L"Press ENTER to stop recording... ");
    // Wait for user input
    std::wstring tempString;
    std::getline(std::wcin, tempString);

    // Stop the capture (and give it a little bit of time)
    session.Close();
    framePool.Close();
    co_await std::chrono::milliseconds(100);

    // Finish our recording and display the file
    winrt::check_hresult(encoder->Commit());
    co_await winrt::Launcher::LaunchFileAsync(file);
}

int wmain(int argc, wchar_t* argv[])
{
    winrt::init_apartment();
    
    std::vector<std::wstring> args(argv + 1, argv + argc);

    MainAsync(args).get();
}
