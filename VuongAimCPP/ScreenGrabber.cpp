#include "ScreenGrabber.h"
#include <cstdint>

ScreenGrabber::ScreenGrabber() {}

ScreenGrabber::~ScreenGrabber() {
    CleanUp();
}

void ScreenGrabber::CleanUp() {
    if (m_StagingTexture) { m_StagingTexture->Release(); m_StagingTexture = nullptr; }
    if (m_DeskDupl) { m_DeskDupl->Release(); m_DeskDupl = nullptr; }
    if (m_DeviceContext) { m_DeviceContext->Release(); m_DeviceContext = nullptr; }
    if (m_Device) { m_Device->Release(); m_Device = nullptr; }
    m_ScreenWidth = 0;
    m_ScreenHeight = 0;
    m_StagingWidth = 0;
    m_StagingHeight = 0;
}

bool ScreenGrabber::Initialize() {
    CleanUp();
    IDXGIFactory1* factory = nullptr;
    if (FAILED(CreateDXGIFactory1(__uuidof(IDXGIFactory1), reinterpret_cast<void**>(&factory)))) return false;

    bool found = false;
    for (UINT i = 0; !found; ++i) {
        IDXGIAdapter1* adapter = nullptr;
        HRESULT ar = factory->EnumAdapters1(i, &adapter);
        if (ar == DXGI_ERROR_NOT_FOUND) break;
        if (FAILED(ar) || !adapter) break;

        for (UINT j = 0; !found; ++j) {
            IDXGIOutput* output = nullptr;
            HRESULT orr = adapter->EnumOutputs(j, &output);
            if (orr == DXGI_ERROR_NOT_FOUND) break;
            if (FAILED(orr) || !output) break;

            ID3D11Device* device = nullptr;
            ID3D11DeviceContext* context = nullptr;
            IDXGIOutput1* output1 = nullptr;
            IDXGIOutputDuplication* duplication = nullptr;
            D3D_FEATURE_LEVEL featureLevel{};

            HRESULT hr = D3D11CreateDevice(adapter, D3D_DRIVER_TYPE_UNKNOWN, nullptr, 0,
                nullptr, 0, D3D11_SDK_VERSION,
                &device, &featureLevel, &context);
            if (SUCCEEDED(hr)) {
                IDXGIDevice1* dxgiDevice = nullptr;
                if (SUCCEEDED(device->QueryInterface(__uuidof(IDXGIDevice1), reinterpret_cast<void**>(&dxgiDevice)))) {
                    dxgiDevice->SetMaximumFrameLatency(1);
                    dxgiDevice->Release();
                }
                if (SUCCEEDED(output->QueryInterface(__uuidof(IDXGIOutput1), reinterpret_cast<void**>(&output1)))) {
                    hr = output1->DuplicateOutput(device, &duplication);
                    if (SUCCEEDED(hr) && duplication) {
                        m_Device = device;
                        m_DeviceContext = context;
                        m_DeskDupl = duplication;
                        DXGI_OUTDUPL_DESC desc{};
                        m_DeskDupl->GetDesc(&desc);
                        m_ScreenWidth = static_cast<int>(desc.ModeDesc.Width);
                        m_ScreenHeight = static_cast<int>(desc.ModeDesc.Height);
                        found = true;
                    }
                }
            }

            if (output1) output1->Release();
            if (!found) {
                if (duplication) duplication->Release();
                if (context) context->Release();
                if (device) device->Release();
            }
            output->Release();
        }
        adapter->Release();
    }
    factory->Release();
    return found;
}

bool ScreenGrabber::AcquireFrame(float* pDst, const cv::Rect& cropRegion) {
    if (!m_DeskDupl || !m_Device || !m_DeviceContext || !pDst) return false;
    if (cropRegion.x < 0 || cropRegion.y < 0 || cropRegion.width <= 0 || cropRegion.height <= 0) return false;
    if (cropRegion.x + cropRegion.width > m_ScreenWidth ||
        cropRegion.y + cropRegion.height > m_ScreenHeight) return false;

    DXGI_OUTDUPL_FRAME_INFO frameInfo{};
    IDXGIResource* resource = nullptr;
    HRESULT hr = m_DeskDupl->AcquireNextFrame(0, &frameInfo, &resource);
    if (hr == DXGI_ERROR_WAIT_TIMEOUT) return false;
    if (hr == DXGI_ERROR_ACCESS_LOST) {
        Initialize();
        return false;
    }
    if (FAILED(hr)) return false;

    bool ok = false;
    ID3D11Texture2D* texture = nullptr;
    if (resource) {
        hr = resource->QueryInterface(__uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&texture));
        resource->Release();
    }
    else {
        hr = E_POINTER;
    }

    if (SUCCEEDED(hr) && texture) {
        D3D11_TEXTURE2D_DESC srcDesc{};
        texture->GetDesc(&srcDesc);

        if (srcDesc.Format == DXGI_FORMAT_B8G8R8A8_UNORM ||
            srcDesc.Format == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB) {
            if (!m_StagingTexture || m_StagingWidth != cropRegion.width || m_StagingHeight != cropRegion.height) {
                if (m_StagingTexture) { m_StagingTexture->Release(); m_StagingTexture = nullptr; }
                D3D11_TEXTURE2D_DESC stageDesc = srcDesc;
                stageDesc.Width = static_cast<UINT>(cropRegion.width);
                stageDesc.Height = static_cast<UINT>(cropRegion.height);
                stageDesc.MipLevels = 1;
                stageDesc.ArraySize = 1;
                stageDesc.SampleDesc.Count = 1;
                stageDesc.SampleDesc.Quality = 0;
                stageDesc.Usage = D3D11_USAGE_STAGING;
                stageDesc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
                stageDesc.BindFlags = 0;
                stageDesc.MiscFlags = 0;
                if (SUCCEEDED(m_Device->CreateTexture2D(&stageDesc, nullptr, &m_StagingTexture))) {
                    m_StagingWidth = cropRegion.width;
                    m_StagingHeight = cropRegion.height;
                }
                else {
                    m_StagingWidth = m_StagingHeight = 0;
                }
            }

            if (m_StagingTexture) {
                D3D11_BOX box{};
                box.left = static_cast<UINT>(cropRegion.x);
                box.top = static_cast<UINT>(cropRegion.y);
                box.front = 0;
                box.right = static_cast<UINT>(cropRegion.x + cropRegion.width);
                box.bottom = static_cast<UINT>(cropRegion.y + cropRegion.height);
                box.back = 1;

                m_DeviceContext->CopySubresourceRegion(m_StagingTexture, 0, 0, 0, 0, texture, 0, &box);

                D3D11_MAPPED_SUBRESOURCE mapped{};
                hr = m_DeviceContext->Map(m_StagingTexture, 0, D3D11_MAP_READ, 0, &mapped);
                if (SUCCEEDED(hr)) {
                    const int width = cropRegion.width;
                    const int height = cropRegion.height;
                    const int area = width * height;
                    float* red = pDst;
                    float* green = pDst + area;
                    float* blue = pDst + 2 * area;
                    constexpr float scale = 1.0f / 255.0f;

                    for (int y = 0; y < height; ++y) {
                        const uint32_t* row = reinterpret_cast<const uint32_t*>(
                            static_cast<const uint8_t*>(mapped.pData) + y * mapped.RowPitch);
                        for (int x = 0; x < width; ++x) {
                            const uint32_t px = row[x];
                            *blue++ = static_cast<float>(px & 0xFF) * scale;
                            *green++ = static_cast<float>((px >> 8) & 0xFF) * scale;
                            *red++ = static_cast<float>((px >> 16) & 0xFF) * scale;
                        }
                    }
                    m_DeviceContext->Unmap(m_StagingTexture, 0);
                    ok = true;
                }
            }
        }
        texture->Release();
    }

    m_DeskDupl->ReleaseFrame();
    return ok;
}
