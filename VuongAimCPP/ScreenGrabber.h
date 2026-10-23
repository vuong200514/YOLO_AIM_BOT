#pragma once
#include <d3d11.h>
#include <dxgi1_2.h>
#include <opencv2/opencv.hpp>

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")

class ScreenGrabber {
public:
    ScreenGrabber();
    ~ScreenGrabber();
    bool Initialize();
    bool AcquireFrame(float* pDst, const cv::Rect& cropRegion);
    void CleanUp();
    int GetScreenWidth() const { return m_ScreenWidth; }
    int GetScreenHeight() const { return m_ScreenHeight; }

private:
    ID3D11Device* m_Device = nullptr;
    ID3D11DeviceContext* m_DeviceContext = nullptr;
    IDXGIOutputDuplication* m_DeskDupl = nullptr;
    ID3D11Texture2D* m_StagingTexture = nullptr;
    int m_ScreenWidth = 0;
    int m_ScreenHeight = 0;
    int m_StagingWidth = 0;
    int m_StagingHeight = 0;
};
