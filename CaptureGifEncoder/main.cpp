#include "pch.h"
#include "WindowInfo.h"

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

float CLEARCOLOR[] = { 0.0f, 0.0f, 0.0f, 1.0f }; // RGBA

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
    auto d3dDevice = util::CreateD3DDevice();
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

    // Create a texture that will hold the frame we'll be encoding
    winrt::com_ptr<ID3D11Texture2D> gifTexture;
    winrt::com_ptr<ID3D11RenderTargetView> renderTargetView;
    {
        D3D11_TEXTURE2D_DESC description = {};
        description.Width = itemSize.Width;
        description.Height = itemSize.Height;
        description.MipLevels = 1;
        description.ArraySize = 1;
        description.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        description.SampleDesc.Count = 1;
        description.SampleDesc.Quality = 0;
        description.Usage = D3D11_USAGE_DEFAULT;
        description.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
        description.CPUAccessFlags = 0;
        winrt::check_hresult(d3dDevice->CreateTexture2D(&description, nullptr, gifTexture.put()));
        winrt::check_hresult(d3dDevice->CreateRenderTargetView(gifTexture.get(), nullptr, renderTargetView.put()));
    }

    // Encode frames as they arrive. Because we created our frame pool using 
    // Direct3D11CaptureFramePool::CreateFreeThreaded, this lambda will fire on a different thread
    // than our current one. If you'd like the callback to fire on your thread, create the frame pool
    // using Direct3D11CaptureFramePool::Create and make sure your thread has a DispatcherQueue and you
    // are pumping messages.
    auto lastTimeStamp = winrt::TimeSpan{ 0 };
    framePool.FrameArrived([itemSize, d3dContext, d2dContext, gifTexture, encoder, imageEncoder, renderTargetView, &lastTimeStamp](auto& framePool, auto&)
    {
        auto frame = framePool.TryGetNextFrame();
        auto contentSize = frame.ContentSize();
        auto timeStamp = frame.SystemRelativeTime();
        auto frameTexture = GetDXGIInterfaceFromObject<ID3D11Texture2D>(frame.Surface());

        // Compute the frame delay
        if (lastTimeStamp.count() == 0)
        {
            lastTimeStamp = timeStamp;
        }
        auto timeStampDelta = timeStamp - lastTimeStamp;
        lastTimeStamp = timeStamp;
        auto millisconds = std::chrono::duration_cast<std::chrono::milliseconds>(timeStampDelta);
        // Use 10ms units
        auto frameDelay = millisconds.count() / 10;

        // In order to support window resizing, we need to only copy out the part of
        // the buffer that contains the window. If the window is smaller than the buffer,
        // then it's a straight forward copy using the ContentSize. If the window is larger,
        // we need to clamp to the size of the buffer. For simplicity, we always clamp.
        auto width = std::clamp(contentSize.Width, 0, itemSize.Width);
        auto height = std::clamp(contentSize.Height, 0, itemSize.Height);

        D3D11_BOX region = {};
        region.left = 0;
        region.right = width;
        region.top = 0;
        region.bottom = height;
        region.back = 1;

        // Clear the texture to black
        d3dContext->ClearRenderTargetView(renderTargetView.get(), CLEARCOLOR); 
        // Copy our window into the gif texture
        d3dContext->CopySubresourceRegion(
            gifTexture.get(),
            0,
            0, 0, 0,
            frameTexture.get(),
            0,
            &region);

        // Make a D2D bitmap so that we can hand this off to WIC
        winrt::com_ptr<ID2D1Bitmap1> d2dBitmap;
        winrt::check_hresult(d2dContext->CreateBitmapFromDxgiSurface(gifTexture.as<IDXGISurface>().get(), nullptr, d2dBitmap.put()));

        // Setup our WIC frame (note the matching pixel format)
        winrt::com_ptr<IWICBitmapFrameEncode> wicFrame;
        winrt::check_hresult(encoder->CreateNewFrame(wicFrame.put(), nullptr));
        winrt::check_hresult(wicFrame->Initialize(nullptr));
        auto wicPixelFormat = GUID_WICPixelFormat32bppBGRA;
        winrt::check_hresult(wicFrame->SetPixelFormat(&wicPixelFormat));

        // Write our frame delay
        winrt::com_ptr<IWICMetadataQueryWriter> metadata;
        winrt::check_hresult(wicFrame->GetMetadataQueryWriter(metadata.put()));
        {
            PROPVARIANT delayValue = {};
            delayValue.vt = VT_UI2;
            delayValue.uiVal = static_cast<unsigned short>(frameDelay);
            winrt::check_hresult(metadata->SetMetadataByName(L"/grctlext/Delay", &delayValue));
        }

        // Write the frame to our image (this must come after you write the metadata)
        winrt::check_hresult(imageEncoder->WriteFrame(d2dBitmap.get(), wicFrame.get(), nullptr));
        winrt::check_hresult(wicFrame->Commit());
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
