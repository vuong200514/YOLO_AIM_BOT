#pragma once

#include <vector>
#include <string>
#include <opencv2/opencv.hpp>
#include <NvInfer.h>
#include <cuda_runtime.h>

using namespace nvinfer1;

struct Detection {
    int class_id;
    float confidence;
    cv::Rect box;
};

class Logger : public ILogger {
    void log(Severity severity, const char* msg) noexcept override {}
};

class YoloEngine {
public:
    YoloEngine();
    ~YoloEngine();

    bool Initialize(const std::string& enginePath);
    std::vector<Detection> Detect(float conf_thresh = 0.5f);
    float* GetInputBuffer() const { return m_PinnedInput; }

private:
    IRuntime* m_Runtime = nullptr;
    ICudaEngine* m_Engine = nullptr;
    IExecutionContext* m_Context = nullptr;

    void* m_buffers[2];
    float* m_PinnedInput = nullptr;
    float* m_PinnedOutput = nullptr;

    cudaStream_t m_Stream = nullptr;
    Logger m_Logger;

    cudaGraph_t m_CudaGraph = nullptr;
    cudaGraphExec_t m_CudaGraphInstance = nullptr;
    bool m_IsGraphCaptured = false;

    const int INPUT_W = 128;
    const int INPUT_H = 128;
    const int NUM_CLASSES = 2;
    const int NUM_ANCHORS = 336;
};