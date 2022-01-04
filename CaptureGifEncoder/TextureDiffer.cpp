#include "pch.h"
#include "TextureDiffer.h"
#include "TextureDiffShader.h"

namespace winrt
{
    using namespace Windows::Graphics;
}

template <typename T>
T ReadFromBuffer(
    winrt::com_ptr<ID3D11DeviceContext> const& d3dContext,
    winrt::com_ptr<ID3D11Buffer> const& stagingBuffer)
{
    D3D11_BUFFER_DESC desc = {};
    stagingBuffer->GetDesc(&desc);

    assert(sizeof(T) <= desc.ByteWidth);

    D3D11_MAPPED_SUBRESOURCE mapped = {};
    d3dContext->Map(stagingBuffer.get(), 0, D3D11_MAP_READ, 0, &mapped);

    T result = {};
    result = *reinterpret_cast<T*>(mapped.pData);

    d3dContext->Unmap(stagingBuffer.get(), 0);

    return result;
}

TextureDiffer::TextureDiffer(
    winrt::com_ptr<ID3D11Device> const& d3dDevice, 
    winrt::com_ptr<ID3D11DeviceContext> const& d3dContext, 
    winrt::SizeInt32 textureSize)
{
    m_d3dDevice = d3dDevice;
    m_d3dContext = d3dContext;
    m_textureSize = textureSize;

    D3D11_TEXTURE2D_DESC previousTextureDesc = {};
    previousTextureDesc.Width = static_cast<uint32_t>(textureSize.Width);
    previousTextureDesc.Height = static_cast<uint32_t>(textureSize.Height);
    previousTextureDesc.MipLevels = 1;
    previousTextureDesc.ArraySize = 1;
    previousTextureDesc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    previousTextureDesc.SampleDesc.Count = 1;
    previousTextureDesc.Usage = D3D11_USAGE_DEFAULT;
    previousTextureDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
    winrt::check_hresult(d3dDevice->CreateTexture2D(&previousTextureDesc, nullptr, m_previousTexture.put()));
    winrt::check_hresult(d3dDevice->CreateShaderResourceView(m_previousTexture.get(), nullptr, m_previousTextureSRV.put()));

    auto diffBufferSize = static_cast<uint32_t>(sizeof(DiffRect));

    D3D11_BUFFER_DESC diffBufferDesc = {};
    diffBufferDesc.ByteWidth = diffBufferSize;
    diffBufferDesc.Usage = D3D11_USAGE_DEFAULT;
    diffBufferDesc.BindFlags = D3D11_BIND_UNORDERED_ACCESS;
    diffBufferDesc.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;
    diffBufferDesc.StructureByteStride = diffBufferSize;
    winrt::check_hresult(d3dDevice->CreateBuffer(&diffBufferDesc, nullptr, m_diffBuffer.put()));

    D3D11_BUFFER_DESC diffDefaultBufferDesc = {};
    diffDefaultBufferDesc.ByteWidth = diffBufferSize;
    diffDefaultBufferDesc.Usage = D3D11_USAGE_DEFAULT;
    diffDefaultBufferDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    DiffRect initialRect = {};
    initialRect.Left = static_cast<uint32_t>(textureSize.Width);
    initialRect.Top = static_cast<uint32_t>(textureSize.Height);
    initialRect.Right = 0;
    initialRect.Bottom = 0;
    D3D11_SUBRESOURCE_DATA initData = {};
    initData.pSysMem = reinterpret_cast<void*>(&initialRect);
    winrt::check_hresult(d3dDevice->CreateBuffer(&diffDefaultBufferDesc, &initData, m_diffDefaultBuffer.put()));

    D3D11_BUFFER_DESC diffStagingBufferDesc = {};
    diffStagingBufferDesc.ByteWidth = diffBufferSize;
    diffStagingBufferDesc.Usage = D3D11_USAGE_STAGING;
    diffStagingBufferDesc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    winrt::check_hresult(d3dDevice->CreateBuffer(&diffStagingBufferDesc, nullptr, m_diffStagingBuffer.put()));

    D3D11_UNORDERED_ACCESS_VIEW_DESC uavDiff = {};
    uavDiff.Format = DXGI_FORMAT_UNKNOWN;
    uavDiff.ViewDimension = D3D11_UAV_DIMENSION_BUFFER;
    uavDiff.Buffer.NumElements = 1;
    winrt::check_hresult(d3dDevice->CreateUnorderedAccessView(m_diffBuffer.get(), &uavDiff, m_diffBufferUAV.put()));

    winrt::check_hresult(d3dDevice->CreateComputeShader(g_main, ARRAYSIZE(g_main), nullptr, m_diffShader.put()));

    std::array<ID3D11UnorderedAccessView*, 1> uavs = { m_diffBufferUAV.get() };
    d3dContext->CSSetShader(m_diffShader.get(), nullptr, 0);
    d3dContext->CSSetUnorderedAccessViews(0, 1, uavs.data(), nullptr);
}

std::optional<DiffRect> TextureDiffer::ProcessFrame(winrt::com_ptr<ID3D11Texture2D> const& frameTexture)
{
    if (m_firstFrame)
    {
        m_firstFrame = false;
        m_d3dContext->CopyResource(m_previousTexture.get(), frameTexture.get());
        return std::optional<DiffRect>(DiffRect{ 0, 0, static_cast<uint32_t>(m_textureSize.Width), static_cast<uint32_t>(m_textureSize.Height) });
    }
    
    winrt::com_ptr<ID3D11ShaderResourceView> frameTextureSRV;
    winrt::check_hresult(m_d3dDevice->CreateShaderResourceView(frameTexture.get(), nullptr, frameTextureSRV.put()));

    m_d3dContext->CopyResource(m_diffBuffer.get(), m_diffDefaultBuffer.get());
    std::array<ID3D11ShaderResourceView*, 2> srvs = { frameTextureSRV.get(), m_previousTextureSRV.get() };
    m_d3dContext->CSSetShaderResources(0, 2, srvs.data());
    m_d3dContext->Dispatch(static_cast<uint32_t>(m_textureSize.Width) / 2, static_cast<uint32_t>(m_textureSize.Height) / 2, 1);

    m_d3dContext->CopyResource(m_diffStagingBuffer.get(), m_diffBuffer.get());
    m_d3dContext->CopyResource(m_previousTexture.get(), frameTexture.get());

    auto diffRect = ReadFromBuffer<DiffRect>(m_d3dContext, m_diffStagingBuffer);

    if (diffRect.IsValid())
    {
        return std::optional(diffRect);
    }
    else
    {
        return std::nullopt;
    }
}
