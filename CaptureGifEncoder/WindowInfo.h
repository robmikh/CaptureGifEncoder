#pragma once

struct WindowInfo
{
    WindowInfo(HWND windowHandle)
    {
        WindowHandle = windowHandle;
        auto titleLength = GetWindowTextLengthW(WindowHandle);
        if (titleLength > 0)
        {
            titleLength++;
        }
        std::wstring title(titleLength, 0);
        GetWindowTextW(WindowHandle, title.data(), titleLength);
        Title = title;
        auto classNameLength = 256;
        std::wstring className(classNameLength, 0);
        GetClassNameW(WindowHandle, className.data(), classNameLength);
        ClassName = className;
        RECT rect = {};
        winrt::check_bool(GetWindowRect(WindowHandle, &rect));
        Position = { (float)rect.left, (float)rect.top };
        Size = { (float)(rect.right - rect.left), (float)(rect.bottom - rect.top) };
    }

    HWND WindowHandle;
    std::wstring Title;
    std::wstring ClassName;
    winrt::Windows::Foundation::Numerics::float2 Position;
    winrt::Windows::Foundation::Numerics::float2 Size;

    bool operator==(const WindowInfo& info) { return WindowHandle == info.WindowHandle; }
    bool operator!=(const WindowInfo& info) { return !(*this == info); }
};

std::vector<WindowInfo> FindWindowsByTitle(std::wstring const& query)
{
    struct EnumState
    {
        std::wstring WindowQuery;
        std::vector<WindowInfo> Windows;
    };

    auto enumState = EnumState{ query, std::vector<WindowInfo>() };
    EnumWindows([](HWND hwnd, LPARAM lParam)
    {
        if (GetWindowTextLengthW(hwnd) > 0)
        {
            auto& enumState = *reinterpret_cast<EnumState*>(lParam);
            auto window = WindowInfo(hwnd);

            auto index = window.Title.find(enumState.WindowQuery.c_str());
            if (index == std::wstring::npos)
            {
                return TRUE;
            }

            enumState.Windows.push_back(window);
        }

        return TRUE;
    }, reinterpret_cast<LPARAM>(&enumState));

    return enumState.Windows;
}