#include "ScreenGrabber.h"
#include <cstdint>

ScreenGrabber::ScreenGrabber() {}

ScreenGrabber::~ScreenGrabber() {
    CleanUp();
}

void ScreenGrabber::CleanUp() {
    if (m_StagingTexture) m_StagingTexture->Release();
    if (m_DeskDupl) m_DeskDupl->Release();
    if (m_DeviceContext) m_DeviceContext->Release();
    if (m_Device) m_Device->Release();
}

bool ScreenGrabber::Initialize() {
    HRESULT hr;
    IDXGIFactory1* factory = nullptr;
    if (FAILED(CreateDXGIFactory1(__uuidof(IDXGIFactory1), (void**)&factory))) return false;

    IDXGIAdapter1* adapter = nullptr;
    IDXGIOutput* output = nullptr;
    bool found = false;

    for (UINT i = 0; factory->EnumAdapters1(i, &adapter) != DXGI_ERROR_NOT_FOUND; ++i) {
        for (UINT j = 0; adapter->EnumOutputs(j, &output) != DXGI_ERROR_NOT_FOUND; ++j) {
            ID3D11Device* testDevice = nullptr;
            ID3D11DeviceContext* testContext = nullptr;
            D3D_FEATURE_LEVEL fl;
            hr = D3D11CreateDevice(adapter, D3D_DRIVER_TYPE_UNKNOWN, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION, &testDevice, &fl, &testContext);
            if (SUCCEEDED(hr)) {
                IDXGIDevice1* dxgiDevice = nullptr;
                if (SUCCEEDED(testDevice->QueryInterface(__uuidof(IDXGIDevice1), (void**)&dxgiDevice))) {
                    dxgiDevice->SetMaximumFrameLatency(1);
                    dxgiDevice->Release();
                }

                IDXGIOutput1* output1 = nullptr;
                if (SUCCEEDED(output->QueryInterface(__uuidof(IDXGIOutput1), (void**)&output1))) {
                    IDXGIOutputDuplication* testDupl = nullptr;
                    hr = output1->DuplicateOutput(testDevice, &testDupl);
                    if (SUCCEEDED(hr)) {
                        m_Device = testDevice;
                        m_DeviceContext = testContext;
                        m_DeskDupl = testDupl;
                        DXGI_OUTDUPL_DESC duplDesc;
                        m_DeskDupl->GetDesc(&duplDesc);
                        m_ScreenWidth = duplDesc.ModeDesc.Width;
                        m_ScreenHeight = duplDesc.ModeDesc.Height;
                        found = true;
                        output1->Release();
                    }
                    else { testDevice->Release(); testContext->Release(); output1->Release(); }
                }
                else { testDevice->Release(); testContext->Release(); }
            }
            output->Release();
            if (found) break;
        }
        adapter->Release();
        if (found) break;
    }
    factory->Release();
    return found;
}

void ScreenGrabber::AcquireFrame(float* pDst, const cv::Rect& cropRegion) {
    if (!m_DeskDupl || !pDst) return;

    DXGI_OUTDUPL_FRAME_INFO frameInfo;
    IDXGIResource* desktopResource = nullptr;

    HRESULT hr = m_DeskDupl->AcquireNextFrame(0, &frameInfo, &desktopResource);
    if (hr == DXGI_ERROR_WAIT_TIMEOUT) return;
    if (FAILED(hr)) { m_DeskDupl->ReleaseFrame(); return; }

    ID3D11Texture2D* gpuTexture = nullptr;
    desktopResource->QueryInterface(__uuidof(ID3D11Texture2D), (void**)&gpuTexture);
    desktopResource->Release();

    if (!m_StagingTexture) {
        D3D11_TEXTURE2D_DESC desc;
        gpuTexture->GetDesc(&desc);
        desc.Width = cropRegion.width;
        desc.Height = cropRegion.height;
        desc.Usage = D3D11_USAGE_STAGING;
        desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        desc.BindFlags = 0;
        desc.MiscFlags = 0;
        desc.MipLevels = 1;
        desc.ArraySize = 1;
        desc.SampleDesc.Count = 1;
        m_Device->CreateTexture2D(&desc, nullptr, &m_StagingTexture);
    }

    D3D11_BOX sourceRegion;
    sourceRegion.left = cropRegion.x;
    sourceRegion.top = cropRegion.y;
    sourceRegion.front = 0;
    sourceRegion.right = cropRegion.x + cropRegion.width;
    sourceRegion.bottom = cropRegion.y + cropRegion.height;
    sourceRegion.back = 1;

    m_DeviceContext->CopySubresourceRegion(m_StagingTexture, 0, 0, 0, 0, gpuTexture, 0, &sourceRegion);

    D3D11_MAPPED_SUBRESOURCE map;
    hr = m_DeviceContext->Map(m_StagingTexture, 0, D3D11_MAP_READ, 0, &map);

    if (SUCCEEDED(hr)) {
        uint8_t* srcRow = (uint8_t*)map.pData;
        int width = cropRegion.width;
        int height = cropRegion.height;
        int area = width * height;

        float* pR = pDst;
        float* pG = pDst + area;
        float* pB = pDst + 2 * area;

        const float norm = 1.0f / 255.0f;
        const int rowPitch = map.RowPitch;

#pragma loop(hint_parallel(4))
#pragma loop(ivdep)
        for (int y = 0; y < height; ++y) {
            uint32_t* pPixel32 = (uint32_t*)(srcRow + y * rowPitch);
            for (int x = 0; x < width; ++x) {
                uint32_t pixel = pPixel32[x];
                *pB++ = (pixel & 0xFF) * norm;
                *pG++ = ((pixel >> 8) & 0xFF) * norm;
                *pR++ = ((pixel >> 16) & 0xFF) * norm;
            }
        }
        m_DeviceContext->Unmap(m_StagingTexture, 0);
    }

    gpuTexture->Release();
    m_DeskDupl->ReleaseFrame();
}