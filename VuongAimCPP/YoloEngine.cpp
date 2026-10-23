#include "YoloEngine.h"
#include <fstream>
#include <iostream>
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>

void Logger::log(Severity severity, const char* msg) noexcept {
    if (severity <= Severity::kERROR && msg) std::cerr << "TensorRT: " << msg << '\n';
}

struct NMSBox {
    float x1, y1, x2, y2;
    float score;
    float area;
    int class_id;
};

YoloEngine::YoloEngine() = default;

YoloEngine::~YoloEngine() {
    if (m_CudaGraphInstance) cudaGraphExecDestroy(m_CudaGraphInstance);
    if (m_CudaGraph) cudaGraphDestroy(m_CudaGraph);
    if (m_Stream) cudaStreamDestroy(m_Stream);
    if (m_PinnedInput) cudaFreeHost(m_PinnedInput);
    if (m_PinnedOutput) cudaFreeHost(m_PinnedOutput);
    if (m_buffers[0]) cudaFree(m_buffers[0]);
    if (m_buffers[1]) cudaFree(m_buffers[1]);
    delete m_Context;
    delete m_Engine;
    delete m_Runtime;
}

bool YoloEngine::CheckCuda(cudaError_t status, const char* operation) const {
    if (status == cudaSuccess) return true;
    std::cerr << "CUDA " << operation << ": " << cudaGetErrorString(status) << '\n';
    return false;
}

bool YoloEngine::Initialize(const std::string& enginePath) {
    std::ifstream file(enginePath, std::ios::binary | std::ios::ate);
    if (!file) {
        std::cerr << "Cannot open engine: " << enginePath << '\n';
        return false;
    }

    const std::streamsize length = file.tellg();
    if (length <= 0) return false;
    file.seekg(0, std::ios::beg);
    std::vector<char> serialized(static_cast<size_t>(length));
    if (!file.read(serialized.data(), length)) return false;

    m_Runtime = createInferRuntime(m_Logger);
    if (!m_Runtime) return false;
    m_Engine = m_Runtime->deserializeCudaEngine(serialized.data(), serialized.size());
    if (!m_Engine) return false;
    m_Context = m_Engine->createExecutionContext();
    if (!m_Context) return false;
    if (m_Engine->getNbIOTensors() != 2) {
        std::cerr << "Engine must contain exactly one input and one output tensor\n";
        return false;
    }

    const char* inputName = nullptr;
    const char* outputName = nullptr;
    for (int i = 0; i < m_Engine->getNbIOTensors(); ++i) {
        const char* name = m_Engine->getIOTensorName(i);
        if (m_Engine->getTensorIOMode(name) == TensorIOMode::kINPUT) inputName = name;
        if (m_Engine->getTensorIOMode(name) == TensorIOMode::kOUTPUT) outputName = name;
    }
    if (!inputName || !outputName) return false;
    if (m_Engine->getTensorDataType(inputName) != DataType::kFLOAT ||
        m_Engine->getTensorDataType(outputName) != DataType::kFLOAT) {
        std::cerr << "Engine I/O must be FP32; internal FP16 layers are supported\n";
        return false;
    }

    const Dims inDims = m_Engine->getTensorShape(inputName);
    const Dims outDims = m_Engine->getTensorShape(outputName);
    auto volume = [](const Dims& dims) -> int64_t {
        int64_t product = 1;
        if (dims.nbDims <= 0) return -1;
        for (int i = 0; i < dims.nbDims; ++i) {
            if (dims.d[i] <= 0) return -1;
            product *= dims.d[i];
        }
        return product;
        };

    if (volume(inDims) != static_cast<int64_t>(3 * INPUT_W * INPUT_H) ||
        volume(outDims) != static_cast<int64_t>((4 + NUM_CLASSES) * NUM_ANCHORS)) {
        std::cerr << "Tensor shape incompatible with 128x128 YOLO model\n";
        return false;
    }

    const size_t inputBytes = static_cast<size_t>(3 * INPUT_W * INPUT_H) * sizeof(float);
    const size_t outputBytes = static_cast<size_t>((4 + NUM_CLASSES) * NUM_ANCHORS) * sizeof(float);
    if (!CheckCuda(cudaStreamCreateWithFlags(&m_Stream, cudaStreamNonBlocking), "stream")) return false;
    if (!CheckCuda(cudaMalloc(&m_buffers[0], inputBytes), "input alloc")) return false;
    if (!CheckCuda(cudaMalloc(&m_buffers[1], outputBytes), "output alloc")) return false;
    if (!CheckCuda(cudaMallocHost(reinterpret_cast<void**>(&m_PinnedInput), inputBytes), "pinned input")) return false;
    if (!CheckCuda(cudaMallocHost(reinterpret_cast<void**>(&m_PinnedOutput), outputBytes), "pinned output")) return false;

    if (!m_Context->setTensorAddress(inputName, m_buffers[0])) return false;
    if (!m_Context->setTensorAddress(outputName, m_buffers[1])) return false;
    return true;
}

bool YoloEngine::ExecuteDirect() {
    const size_t inBytes = static_cast<size_t>(3 * INPUT_W * INPUT_H) * sizeof(float);
    const size_t outBytes = static_cast<size_t>((4 + NUM_CLASSES) * NUM_ANCHORS) * sizeof(float);
    if (!CheckCuda(cudaMemcpyAsync(m_buffers[0], m_PinnedInput, inBytes, cudaMemcpyHostToDevice, m_Stream), "copy input")) return false;
    if (!m_Context->enqueueV3(m_Stream)) {
        std::cerr << "TensorRT enqueueV3 failed\n";
        return false;
    }
    if (!CheckCuda(cudaMemcpyAsync(m_PinnedOutput, m_buffers[1], outBytes, cudaMemcpyDeviceToHost, m_Stream), "copy output")) return false;
    return CheckCuda(cudaStreamSynchronize(m_Stream), "stream synchronize");
}

bool YoloEngine::CaptureGraph() {
    const size_t inBytes = static_cast<size_t>(3 * INPUT_W * INPUT_H) * sizeof(float);
    const size_t outBytes = static_cast<size_t>((4 + NUM_CLASSES) * NUM_ANCHORS) * sizeof(float);
    if (cudaStreamBeginCapture(m_Stream, cudaStreamCaptureModeGlobal) != cudaSuccess) return false;

    bool succeeded = true;
    succeeded = succeeded && cudaMemcpyAsync(m_buffers[0], m_PinnedInput, inBytes, cudaMemcpyHostToDevice, m_Stream) == cudaSuccess;
    if (succeeded) succeeded = m_Context->enqueueV3(m_Stream);
    if (succeeded) succeeded = cudaMemcpyAsync(m_PinnedOutput, m_buffers[1], outBytes, cudaMemcpyDeviceToHost, m_Stream) == cudaSuccess;

    cudaGraph_t graph = nullptr;
    const cudaError_t status = cudaStreamEndCapture(m_Stream, &graph);
    if (!succeeded || status != cudaSuccess || !graph) {
        if (graph) cudaGraphDestroy(graph);
        return false;
    }

    cudaGraphExec_t executable = nullptr;
    if (cudaGraphInstantiate(&executable, graph, nullptr, nullptr, 0) != cudaSuccess || !executable) {
        cudaGraphDestroy(graph);
        return false;
    }
    m_CudaGraph = graph;
    m_CudaGraphInstance = executable;
    m_IsGraphCaptured = true;
    return true;
}

std::vector<Detection> YoloEngine::Detect(float conf_thresh) {
    std::vector<Detection> detections;
    if (!m_Context || !m_Stream || !m_PinnedInput || !m_PinnedOutput) return detections;
    if (!m_GraphAttempted) {
        m_GraphAttempted = true;
        if (!ExecuteDirect()) return detections;
        if (!CaptureGraph()) std::cerr << "CUDA Graph unavailable; direct inference enabled\n";
    }

    bool inferenceOk = false;
    if (m_IsGraphCaptured) {
        inferenceOk = CheckCuda(cudaGraphLaunch(m_CudaGraphInstance, m_Stream), "graph launch") &&
            CheckCuda(cudaStreamSynchronize(m_Stream), "graph synchronize");
    }
    else {
        inferenceOk = ExecuteDirect();
    }
    if (!inferenceOk) return detections;

    const float* out = m_PinnedOutput;
    const int n = NUM_ANCHORS;
    std::vector<NMSBox> candidates;
    candidates.reserve(n);

    for (int i = 0; i < n; ++i) {
        int label = -1;
        float score = 0.0f;
        for (int c = 0; c < NUM_CLASSES; ++c) {
            const float value = out[(4 + c) * n + i];
            if (std::isfinite(value) && value > score) {
                score = value;
                label = c;
            }
        }
        if (label < 0 || score < conf_thresh) continue;

        const float cx = out[i];
        const float cy = out[n + i];
        const float w = out[2 * n + i];
        const float h = out[3 * n + i];
        if (!std::isfinite(cx) || !std::isfinite(cy) || !std::isfinite(w) || !std::isfinite(h)) continue;
        if (w < 1.0f || h < 1.0f) continue;

        const float x1 = std::clamp(cx - w * 0.5f, 0.0f, static_cast<float>(INPUT_W));
        const float y1 = std::clamp(cy - h * 0.5f, 0.0f, static_cast<float>(INPUT_H));
        const float x2 = std::clamp(cx + w * 0.5f, 0.0f, static_cast<float>(INPUT_W));
        const float y2 = std::clamp(cy + h * 0.5f, 0.0f, static_cast<float>(INPUT_H));
        if (x2 <= x1 || y2 <= y1) continue;
        candidates.push_back({ x1, y1, x2, y2, score, (x2 - x1) * (y2 - y1), label });
    }

    std::sort(candidates.begin(), candidates.end(), [](const NMSBox& a, const NMSBox& b) {
        return a.score > b.score;
        });
    std::vector<uint8_t> suppressed(candidates.size(), 0);
    detections.reserve(candidates.size());

    for (size_t i = 0; i < candidates.size(); ++i) {
        if (suppressed[i]) continue;
        const NMSBox& a = candidates[i];
        const int left = std::clamp(static_cast<int>(std::floor(a.x1)), 0, INPUT_W - 1);
        const int top = std::clamp(static_cast<int>(std::floor(a.y1)), 0, INPUT_H - 1);
        const int right = std::clamp(static_cast<int>(std::ceil(a.x2)), 0, INPUT_W);
        const int bottom = std::clamp(static_cast<int>(std::ceil(a.y2)), 0, INPUT_H);
        if (right > left && bottom > top)
            detections.push_back({ a.class_id, a.score, cv::Rect(left, top, right - left, bottom - top) });

        for (size_t j = i + 1; j < candidates.size(); ++j) {
            if (suppressed[j]) continue;
            const NMSBox& b = candidates[j];
            if (a.class_id != b.class_id) continue;
            const float intersectionW = std::max(0.0f, std::min(a.x2, b.x2) - std::max(a.x1, b.x1));
            const float intersectionH = std::max(0.0f, std::min(a.y2, b.y2) - std::max(a.y1, b.y1));
            const float intersection = intersectionW * intersectionH;
            const float denominator = a.area + b.area - intersection;
            if (denominator > 0.0f && intersection / denominator > 0.45f) suppressed[j] = 1;
        }
    }
    return detections;
}
