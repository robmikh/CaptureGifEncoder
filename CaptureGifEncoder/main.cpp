#include "pch.h"
#include "WindowInfo.h"
#include "FrameCompositor.h"
#include "GifEncoder.h"

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
    auto wicFactory = util::CreateWICFactory();

    // TODO: Use args to determine file name/path
    auto currentPath = std::filesystem::current_path();
    auto folder = co_await winrt::StorageFolder::GetFolderFromPathAsync(currentPath.wstring());
    auto file = co_await folder.CreateFileAsync(L"test.gif", winrt::CreationCollisionOption::ReplaceExisting);
    auto stream = co_await file.OpenAsync(winrt::FileAccessMode::ReadWrite);
    
    // Identify our capture target
    auto item = util::CreateCaptureItemForWindow(window.WindowHandle);
    RECT windowRect = {};
    winrt::check_hresult(DwmGetWindowAttribute(window.WindowHandle, DWMWA_EXTENDED_FRAME_BOUNDS, reinterpret_cast<void*>(&windowRect), sizeof(windowRect)));
    winrt::SizeInt32 captureSize = { windowRect.right - windowRect.left, windowRect.bottom - windowRect.top };

    // Setup our gif encoder
    auto encoder = std::make_shared<GifEncoder>(d3dDevice, d3dContext, d2dDevice, wicFactory, stream, captureSize);

    // Setup Windows.Graphics.Capture
    auto framePool = winrt::Direct3D11CaptureFramePool::CreateFreeThreaded(
        device,
        winrt::DirectXPixelFormat::B8G8R8A8UIntNormalized,
        2,
        captureSize);
    auto session = framePool.CreateCaptureSession(item);

    // Encode frames as they arrive. Because we created our frame pool using 
    // Direct3D11CaptureFramePool::CreateFreeThreaded, this lambda will fire on a different thread
    // than our current one. If you'd like the callback to fire on your thread, create the frame pool
    // using Direct3D11CaptureFramePool::Create and make sure your thread has a DispatcherQueue and you
    // are pumping messages.
    framePool.FrameArrived([captureSize, d3dContext, encoder](auto& framePool, auto&)
    {
        auto frame = framePool.TryGetNextFrame();
        encoder->ProcessFrame(frame);
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
    encoder->StopEncoding();
    co_await winrt::Launcher::LaunchFileAsync(file);
}

int wmain(int argc, wchar_t* argv[])
{
    winrt::init_apartment();
    
    std::vector<std::wstring> args(argv + 1, argv + argc);

    MainAsync(args).get();
}
