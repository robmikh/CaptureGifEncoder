#pragma once

struct DiffRect
{
    uint32_t Left;
    uint32_t Top;
    uint32_t Right;
    uint32_t Bottom;

    uint32_t Width()
    {
        return Right - Left;
    }

    uint32_t Height()
    {
        return Bottom - Top;
    }

    bool IsValid()
    {
        return Right >= Left && Bottom >= Top;
    }
};

class TextureDiffer
{
public:
    TextureDiffer(
        winrt::com_ptr<ID3D11Device> const& d3dDevice,
        winrt::com_ptr<ID3D11DeviceContext> const& d3dContext,
        winrt::Windows::Graphics::SizeInt32 textureSize);

    std::optional<DiffRect> ProcessFrame(winrt::com_ptr<ID3D11Texture2D> const& frameTexture);

private:
    winrt::com_ptr<ID3D11Device> m_d3dDevice;
    winrt::com_ptr<ID3D11DeviceContext> m_d3dContext;
    winrt::com_ptr<ID3D11ComputeShader> m_diffShader;
    winrt::com_ptr<ID3D11Buffer> m_diffBuffer;
    winrt::com_ptr<ID3D11UnorderedAccessView> m_diffBufferUAV;
    winrt::com_ptr<ID3D11Buffer> m_diffDefaultBuffer;
    winrt::com_ptr<ID3D11Buffer> m_diffStagingBuffer;
    winrt::com_ptr<ID3D11Texture2D> m_previousTexture;
    winrt::com_ptr<ID3D11ShaderResourceView> m_previousTextureSRV;
    bool m_firstFrame = true;
    winrt::Windows::Graphics::SizeInt32 m_textureSize = {};
};