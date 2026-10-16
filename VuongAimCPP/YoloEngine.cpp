#include "YoloEngine.h"
#include <fstream>
#include <algorithm>

#define checkCuda(status) { status; }

struct NMSBox {
    float x1, y1, x2, y2;
    float score;
    int class_id;
    float area;
};

YoloEngine::YoloEngine() {
    m_buffers[0] = nullptr;
    m_buffers[1] = nullptr;
}

YoloEngine::~YoloEngine() {
    if (m_CudaGraphInstance) cudaGraphExecDestroy(m_CudaGraphInstance);
    if (m_CudaGraph) cudaGraphDestroy(m_CudaGraph);
    if (m_Stream) cudaStreamDestroy(m_Stream);
    if (m_PinnedInput) cudaFreeHost(m_PinnedInput);
    if (m_PinnedOutput) cudaFreeHost(m_PinnedOutput);
    if (m_buffers[0]) cudaFree(m_buffers[0]);
    if (m_buffers[1]) cudaFree(m_buffers[1]);
    if (m_Context) delete m_Context;
    if (m_Engine) delete m_Engine;
    if (m_Runtime) delete m_Runtime;
}

bool YoloEngine::Initialize(const std::string& enginePath) {
    std::ifstream file(enginePath, std::ios::binary);
    if (!file.good()) return false;
    file.seekg(0, file.end);
    size_t size = file.tellg();
    file.seekg(0, file.beg);
    char* trtModelStream = new char[size];
    file.read(trtModelStream, size);
    file.close();

    m_Runtime = createInferRuntime(m_Logger);
    m_Engine = m_Runtime->deserializeCudaEngine(trtModelStream, size);
    delete[] trtModelStream;
    if (!m_Engine) return false;

    m_Context = m_Engine->createExecutionContext();

    checkCuda(cudaStreamCreate(&m_Stream));

    size_t inputBytes = 3 * INPUT_W * INPUT_H * sizeof(float);
    size_t outputBytes = (4 + NUM_CLASSES) * NUM_ANCHORS * sizeof(float);

    checkCuda(cudaMalloc(&m_buffers[0], inputBytes));
    checkCuda(cudaMalloc(&m_buffers[1], outputBytes));

    checkCuda(cudaMallocHost((void**)&m_PinnedInput, inputBytes));
    checkCuda(cudaMallocHost((void**)&m_PinnedOutput, outputBytes));

    m_Context->setTensorAddress(m_Engine->getIOTensorName(0), m_buffers[0]);
    m_Context->setTensorAddress(m_Engine->getIOTensorName(1), m_buffers[1]);

    return true;
}

std::vector<Detection> YoloEngine::Detect(float conf_thresh) {
    std::vector<Detection> finalDets;
    if (!m_Context) return finalDets;

    size_t inputBytes = 3 * INPUT_W * INPUT_H * sizeof(float);
    size_t outputBytes = (4 + NUM_CLASSES) * NUM_ANCHORS * sizeof(float);

    if (!m_IsGraphCaptured) {
        checkCuda(cudaStreamBeginCapture(m_Stream, cudaStreamCaptureModeGlobal));
        checkCuda(cudaMemcpyAsync(m_buffers[0], m_PinnedInput, inputBytes, cudaMemcpyHostToDevice, m_Stream));
        m_Context->enqueueV3(m_Stream);
        checkCuda(cudaMemcpyAsync(m_PinnedOutput, m_buffers[1], outputBytes, cudaMemcpyDeviceToHost, m_Stream));
        checkCuda(cudaStreamEndCapture(m_Stream, &m_CudaGraph));
        checkCuda(cudaGraphInstantiate(&m_CudaGraphInstance, m_CudaGraph, nullptr, nullptr, 0));
        m_IsGraphCaptured = true;
    }

    checkCuda(cudaGraphLaunch(m_CudaGraphInstance, m_Stream));
    checkCuda(cudaStreamSynchronize(m_Stream));

    float* pOut = m_PinnedOutput;
    int cols = NUM_ANCHORS;

    static std::vector<NMSBox> candidates;
    candidates.clear();

    for (int i = 0; i < cols; ++i) {
        float maxScore = 0.0f;
        int maxClassId = -1;

        for (int c = 0; c < NUM_CLASSES; ++c) {
            float score = pOut[(4 + c) * cols + i];
            if (score > maxScore) {
                maxScore = score;
                maxClassId = c;
            }
        }

        if (maxScore > conf_thresh) {
            float cx = pOut[i];
            float cy = pOut[cols + i];
            float w = pOut[2 * cols + i];
            float h = pOut[3 * cols + i];

            float x1 = cx - w * 0.5f;
            float y1 = cy - h * 0.5f;
            float x2 = x1 + w;
            float y2 = y1 + h;

            candidates.push_back({ x1, y1, x2, y2, maxScore, maxClassId, w * h });
        }
    }

    if (candidates.empty()) return finalDets;

    std::sort(candidates.begin(), candidates.end(), [](const NMSBox& a, const NMSBox& b) {
        return a.score > b.score;
        });

    const float nms_thresh = 0.45f;
    size_t count = candidates.size();

    static std::vector<bool> suppressed;
    suppressed.assign(count, false);

    for (size_t i = 0; i < count; ++i) {
        if (suppressed[i]) continue;

        NMSBox& b1 = candidates[i];
        finalDets.push_back({ b1.class_id, b1.score, cv::Rect((int)b1.x1, (int)b1.y1, (int)(b1.x2 - b1.x1), (int)(b1.y2 - b1.y1)) });

        for (size_t j = i + 1; j < count; ++j) {
            if (suppressed[j]) continue;

            NMSBox& b2 = candidates[j];

            float xx1 = std::max(b1.x1, b2.x1);
            float yy1 = std::max(b1.y1, b2.y1);
            float xx2 = std::min(b1.x2, b2.x2);
            float yy2 = std::min(b1.y2, b2.y2);

            float w = std::max(0.0f, xx2 - xx1);
            float h = std::max(0.0f, yy2 - yy1);
            float inter = w * h;

            if (inter > 0.0f) {
                float unionArea = b1.area + b2.area - inter;
                float iou = inter / unionArea;
                if (iou > nms_thresh) suppressed[j] = true;
            }
        }
    }

    return finalDets;
}