#pragma warning(push)
#pragma warning(disable: 4267 4244 6001 26495)
#include "ScreenGrabber.h"
#include "YoloEngine.h"
#include "MouseController.h"
#pragma warning(pop)

#include "MemoryESP.h"
#include "BhopTelemetry.h"

#pragma comment(lib, "user32.lib")
#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "winmm.lib")
#pragma comment(lib, "nvinfer_10.lib")

#include <cmath>
#include <chrono>
#include <thread>
#include <vector>
#include <windows.h>
#include <timeapi.h>
#include <immintrin.h>
#include <string>
#include <atomic>
#include <psapi.h>
#include <algorithm>
#include <iostream>
#include <limits>

std::atomic<int> g_TargetCycleUs{ 1000000 / 175 };
std::atomic<int> g_TargetClass{ -1 };
std::atomic<int> g_WeaponMode{ 0 };
std::atomic<bool> g_IsRunning{ false };
std::atomic<bool> g_TriggerEnabled{ false };
std::atomic<bool> g_EspEnabled{ false };
std::atomic<bool> g_AntiFlashEnabled{ false };
std::atomic<bool> g_ExitApp{ false };
std::atomic<bool> g_BhopEnabled{ false };
std::atomic<bool> g_RecordMovementEnabled{ false };
std::atomic<float> g_AimSmooth{ 2.0f };

HWND g_hMenu = nullptr;
HWND g_hEditFps = nullptr;
HWND g_hEditSmooth = nullptr;

constexpr int ID_BTN_CT = 101;
constexpr int ID_BTN_T = 102;
constexpr int ID_BTN_DM = 103;
constexpr int ID_BTN_TRIGGER = 104;
constexpr int ID_BTN_START = 105;
constexpr int ID_LBL_STAT = 106;
constexpr int ID_BTN_SET_FPS = 107;
constexpr int ID_BTN_ESP = 108;
constexpr int ID_BTN_ANTIFLASH = 109;
constexpr int ID_LBL_WEAPON = 110;
constexpr int ID_BTN_BHOP = 111;
constexpr int ID_BTN_RECORD = 112;
constexpr int ID_BTN_SET_SMOOTH = 113;

constexpr int TRIGGER_HOLD_KEY = VK_LCONTROL;

static void SetSuperPriority(HANDLE threadHandle, DWORD coreIndex) noexcept {
    (void)coreIndex;
    SetThreadPriority(threadHandle, THREAD_PRIORITY_ABOVE_NORMAL);
}

static inline uintptr_t ResolveEntityByIndex(const Memory& mem, uintptr_t entityList, int index) noexcept {
    if (!entityList) return 0;
    uintptr_t entityIndex = static_cast<uintptr_t>(index) & Offsets::EntityHandleMask;
    uintptr_t chunk = mem.Read<uintptr_t>(entityList + Offsets::EntityListFirstChunkOffset + (Offsets::EntityChunkPointerStride * (entityIndex >> Offsets::EntityHandleChunkShift)));
    if (!chunk) return 0;
    return mem.Read<uintptr_t>(chunk + (Offsets::EntityIdentityStride * (entityIndex & Offsets::EntityHandleEntryMask)));
}

static void AimbotThread() {
    SetSuperPriority(GetCurrentThread(), 1);

    Memory mem;
    ScreenGrabber grabber;
    if (!grabber.Initialize()) {
        std::cerr << "Screen capture initialization failed\n";
        return;
    }
    YoloEngine ai;
    if (!ai.Initialize("aimbot_128.engine")) {
        std::cerr << "TensorRT engine initialization failed\n";
        return;
    }
    MouseController mouse;

    const int centerX = grabber.GetScreenWidth() / 2;
    const int centerY = grabber.GetScreenHeight() / 2;
    const cv::Rect region(centerX - 64, centerY - 64, 128, 128);
    if (region.x < 0 || region.y < 0) return;

    float* buffer = ai.GetInputBuffer();
    bool isShooting = false;
    bool locked = false;
    cv::Point2f previousTarget(64.0f, 64.0f);
    using Clock = std::chrono::steady_clock;
    auto lastFreshFrame = Clock::now();
    auto statisticsStart = Clock::now();
    double captureMs = 0.0;
    double inferenceMs = 0.0;
    double controlMs = 0.0;
    uint64_t freshFrames = 0;

    auto releaseShot = [&]() {
        if (isShooting) {
            mouse.Click(false);
            isShooting = false;
        }
        };
    auto milliseconds = [](Clock::time_point first, Clock::time_point second) {
        return std::chrono::duration<double, std::milli>(second - first).count();
        };

    while (!g_ExitApp.load(std::memory_order_relaxed)) {
        if (!g_IsRunning.load(std::memory_order_relaxed)) {
            releaseShot();
            mouse.Reset();
            locked = false;
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
            continue;
        }

        if (!mem.GetClientBase() || !mem.IsAlive()) {
            releaseShot();
            mouse.Reset();
            locked = false;
            if (!mem.Attach(L"cs2.exe")) {
                std::this_thread::sleep_for(std::chrono::milliseconds(500));
                continue;
            }
        }

        DWORD activePid = 0;
        HWND activeWindow = GetForegroundWindow();
        if (activeWindow) GetWindowThreadProcessId(activeWindow, &activePid);
        if (activePid != mem.GetProcessID()) {
            releaseShot();
            mouse.Reset();
            locked = false;
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
            continue;
        }

        const auto frameStart = Clock::now();
        if (!grabber.AcquireFrame(buffer, region)) {
            if (Clock::now() - lastFreshFrame > std::chrono::milliseconds(150)) {
                releaseShot();
                mouse.Reset();
                locked = false;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
            continue;
        }
        lastFreshFrame = Clock::now();
        const auto captureDone = Clock::now();
        const std::vector<Detection> targets = ai.Detect(0.3f);
        const auto inferenceDone = Clock::now();

        const Detection* bestTarget = nullptr;
        const int selectedClass = g_TargetClass.load(std::memory_order_relaxed);
        float closestToCrosshair = std::numeric_limits<float>::max();
        float closestToPrevious = std::numeric_limits<float>::max();
        const Detection* continuityTarget = nullptr;

        for (const auto& candidate : targets) {
            if (selectedClass != 2 && candidate.class_id != selectedClass) continue;
            if (candidate.box.width <= 0 || candidate.box.height <= 0) continue;
            const float x = candidate.box.x + 0.5f * candidate.box.width;
            const float y = candidate.box.y + 0.5f * candidate.box.height;
            const float dx = x - 64.0f;
            const float dy = y - 64.0f;
            const float crosshairDist2 = dx * dx + dy * dy;
            if (crosshairDist2 < closestToCrosshair) {
                closestToCrosshair = crosshairDist2;
                bestTarget = &candidate;
            }
            if (locked) {
                const float sx = x - previousTarget.x;
                const float sy = y - previousTarget.y;
                const float previousDist2 = sx * sx + sy * sy;
                if (previousDist2 < closestToPrevious) {
                    closestToPrevious = previousDist2;
                    continuityTarget = &candidate;
                }
            }
        }

        if (locked && continuityTarget && closestToPrevious < 40.0f * 40.0f) {
            bestTarget = continuityTarget;
        }

        if (bestTarget) {
            previousTarget = cv::Point2f(
                bestTarget->box.x + 0.5f * bestTarget->box.width,
                bestTarget->box.y + 0.5f * bestTarget->box.height);
            locked = true;
        }
        else {
            locked = false;
            mouse.Reset();
        }

        bool shouldShoot = false;
        if (g_TriggerEnabled.load(std::memory_order_relaxed) &&
            (GetAsyncKeyState(TRIGGER_HOLD_KEY) & 0x8000)) {
            bool checkCrosshair = g_WeaponMode.load(std::memory_order_relaxed) == 1;
            if (!checkCrosshair && bestTarget) {
                const float dx = previousTarget.x - 64.0f;
                const float dy = previousTarget.y - 64.0f;
                checkCrosshair = std::abs(dx) <= bestTarget->box.width * 0.4f &&
                    std::abs(dy) <= bestTarget->box.height * 0.4f;
            }
            if (checkCrosshair) {
                const uintptr_t client = mem.GetClientBase();
                const uintptr_t localPawn = mem.Read<uintptr_t>(client + Offsets::dwLocalPlayerPawn);
                if (localPawn) {
                    const int crosshairId = mem.Read<int>(localPawn + Offsets::m_iIDEntIndex);
                    if (crosshairId > 0) {
                        const uintptr_t entityList = mem.Read<uintptr_t>(client + Offsets::dwEntityList);
                        const uintptr_t targetPawn = ResolveEntityByIndex(mem, entityList, crosshairId);
                        if (targetPawn) {
                            const int localTeam = mem.Read<int>(localPawn + Offsets::m_iTeamNum) & 0xFF;
                            const int targetTeam = mem.Read<int>(targetPawn + Offsets::m_iTeamNum) & 0xFF;
                            const int targetHealth = mem.Read<int>(targetPawn + Offsets::m_iHealth);
                            shouldShoot = targetTeam != localTeam && targetHealth > 0;
                        }
                    }
                }
            }
        }

        if (shouldShoot && !isShooting) {
            mouse.Click(true);
            isShooting = true;
        }
        if (!shouldShoot) releaseShot();

        if (bestTarget) {
            const float dx = previousTarget.x - 64.0f;
            const float dy = previousTarget.y - 64.0f;
            mouse.Move(dx, dy, g_AimSmooth.load(std::memory_order_relaxed));
        }

        const auto controlDone = Clock::now();
        captureMs += milliseconds(frameStart, captureDone);
        inferenceMs += milliseconds(captureDone, inferenceDone);
        controlMs += milliseconds(inferenceDone, controlDone);
        ++freshFrames;

        const double reportSeconds = std::chrono::duration<double>(controlDone - statisticsStart).count();
        if (reportSeconds >= 2.0 && freshFrames != 0) {
            std::cout << "FreshFPS=" << freshFrames / reportSeconds
                << " CaptureMs=" << captureMs / freshFrames
                << " InferenceMs=" << inferenceMs / freshFrames
                << " ControlMs=" << controlMs / freshFrames
                << " Smooth=" << g_AimSmooth.load(std::memory_order_relaxed)
                << std::endl;
            captureMs = inferenceMs = controlMs = 0.0;
            freshFrames = 0;
            statisticsStart = controlDone;
        }

        const int targetCycleUs = std::max(1000, g_TargetCycleUs.load(std::memory_order_relaxed));
        const auto targetDuration = std::chrono::microseconds(targetCycleUs);
        const auto targetTime = frameStart + targetDuration;
        const auto now = Clock::now();
        if (now < targetTime) std::this_thread::sleep_until(targetTime);
    }
    releaseShot();
}

static void VisualsThread() {
    Memory mem;

    while (!g_ExitApp.load(std::memory_order_relaxed)) {
        if (!mem.GetClientBase()) {
            mem.Attach(L"cs2.exe");
            std::this_thread::sleep_for(std::chrono::seconds(1));
            continue;
        }

        bool espOn = g_EspEnabled.load(std::memory_order_relaxed);
        bool antiFlashOn = g_AntiFlashEnabled.load(std::memory_order_relaxed);

        if (espOn || antiFlashOn) {
            uintptr_t client = mem.GetClientBase();
            if (client) {
                uintptr_t localPawn = mem.Read<uintptr_t>(client + Offsets::dwLocalPlayerPawn);

                if (localPawn) {
                    if (antiFlashOn) {
                        float currentDuration = mem.Read<float>(localPawn + Offsets::m_flFlashDuration);
                        if (currentDuration > 0.0f) {
                            mem.Write<float>(localPawn + Offsets::m_flFlashDuration, 0.0f);
                        }
                    }

                    if (espOn) {
                        uintptr_t entList = mem.Read<uintptr_t>(client + Offsets::dwEntityList);
                        int lTeam = mem.Read<int>(localPawn + Offsets::m_iTeamNum) & 0xFF;

                        for (int i = 1; i <= 64; i++) {
                            uintptr_t ctrl = ResolveEntityByIndex(mem, entList, i);
                            if (!ctrl) continue;

                            uint32_t pawnHandle = mem.Read<uint32_t>(ctrl + Offsets::m_hPlayerPawn);
                            if (!pawnHandle || pawnHandle == 0xFFFFFFFF) continue;

                            uintptr_t pawn = ResolveEntityByIndex(mem, entList, static_cast<int>(pawnHandle & Offsets::EntityHandleMask));
                            if (!pawn || pawn == localPawn) continue;

                            int pHp = mem.Read<int>(pawn + Offsets::m_iHealth);
                            if (pHp > 0 && pHp <= 100) {
                                int pTeam = mem.Read<int>(pawn + Offsets::m_iTeamNum) & 0xFF;
                                if (pTeam != lTeam) {
                                    mem.Write<bool>(pawn + Offsets::m_entitySpottedState + Offsets::m_bSpotted, true);
                                    mem.Write<uint32_t>(pawn + Offsets::m_entitySpottedState + Offsets::m_bSpottedByMask, 0xFFFFFFFF);
                                    mem.Write<uint32_t>(pawn + Offsets::m_entitySpottedState + Offsets::m_bSpottedByMask + 4, 0xFFFFFFFF);
                                }
                            }
                        }
                    }
                }
            }
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(200));
    }
}

static LRESULT CALLBACK WindowProc(HWND hwnd, UINT uMsg, WPARAM wParam, LPARAM lParam) {
    switch (uMsg) {
    case WM_COMMAND:
        switch (LOWORD(wParam)) {
        case ID_BTN_CT: g_TargetClass = 0; SetDlgItemTextA(hwnd, ID_LBL_STAT, "SELECTED: CT"); break;
        case ID_BTN_T: g_TargetClass = 1; SetDlgItemTextA(hwnd, ID_LBL_STAT, "SELECTED: T"); break;
        case ID_BTN_DM: g_TargetClass = 2; SetDlgItemTextA(hwnd, ID_LBL_STAT, "SELECTED: DM"); break;
        case ID_BTN_TRIGGER: g_TriggerEnabled = !g_TriggerEnabled; SetDlgItemTextA(hwnd, ID_BTN_TRIGGER, g_TriggerEnabled ? "AI TRIGGER: [ON]" : "AI TRIGGER: [OFF]"); break;
        case ID_BTN_ESP: g_EspEnabled = !g_EspEnabled; SetDlgItemTextA(hwnd, ID_BTN_ESP, g_EspEnabled ? "RADAR: [ON]" : "RADAR: [OFF]"); break;
        case ID_BTN_ANTIFLASH: g_AntiFlashEnabled = !g_AntiFlashEnabled; SetDlgItemTextA(hwnd, ID_BTN_ANTIFLASH, g_AntiFlashEnabled ? "ANTI-FLASH: [ON]" : "ANTI-FLASH: [OFF]"); break;
        case ID_BTN_BHOP: g_BhopEnabled = !g_BhopEnabled; SetDlgItemTextA(hwnd, ID_BTN_BHOP, g_BhopEnabled ? "BHOP MOUSE5: [ARMED]" : "BHOP MOUSE5: [OFF]"); break;
        case ID_BTN_RECORD: g_RecordMovementEnabled = !g_RecordMovementEnabled; SetDlgItemTextA(hwnd, ID_BTN_RECORD, g_RecordMovementEnabled ? "MOVEMENT LOG: [ON]" : "MOVEMENT LOG: [OFF]"); break;
        case ID_BTN_SET_FPS: { char buf[16]{}; GetWindowTextA(g_hEditFps, buf, 15); try { int val = std::stoi(buf); if (val > 0) g_TargetCycleUs = 1000000 / val; } catch (...) {} break; }
        case ID_BTN_SET_SMOOTH: { char buf[32]{}; GetWindowTextA(g_hEditSmooth, buf, 31); try { float val = std::stof(buf); if (std::isfinite(val)) g_AimSmooth = std::clamp(val, 0.1f, 64.0f); } catch (...) {} break; }
        case ID_BTN_START: if (g_TargetClass != -1) { g_IsRunning = true; ShowWindow(hwnd, SW_HIDE); } break;
        } break;
    case WM_CLOSE: g_ExitApp = true; PostQuitMessage(0); return 0;
    } return DefWindowProcA(hwnd, uMsg, wParam, lParam);
}

static void CreateMenuWindow(HINSTANCE hInstance) {
    const char* CLASS_NAME = "SimpleMenu";
    WNDCLASSA wc = { }; wc.lpfnWndProc = WindowProc; wc.hInstance = hInstance; wc.lpszClassName = CLASS_NAME;
    wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1); wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    RegisterClassA(&wc);

    g_hMenu = CreateWindowExA(WS_EX_TOPMOST, CLASS_NAME, "Vuong_CPP_Sync", WS_POPUP | WS_BORDER, 100, 100, 300, 710, nullptr, nullptr, hInstance, nullptr);
    auto CM = [](int id) { return reinterpret_cast<HMENU>(static_cast<UINT_PTR>(id)); };

    CreateWindowA("BUTTON", "SELECT CT", WS_VISIBLE | WS_CHILD | BS_DEFPUSHBUTTON, 50, 20, 200, 35, g_hMenu, CM(ID_BTN_CT), hInstance, nullptr);
    CreateWindowA("BUTTON", "SELECT T", WS_VISIBLE | WS_CHILD | BS_DEFPUSHBUTTON, 50, 65, 200, 35, g_hMenu, CM(ID_BTN_T), hInstance, nullptr);
    CreateWindowA("BUTTON", "DEATHMATCH", WS_VISIBLE | WS_CHILD | BS_DEFPUSHBUTTON, 50, 110, 200, 35, g_hMenu, CM(ID_BTN_DM), hInstance, nullptr);
    CreateWindowA("BUTTON", "AI TRIGGER: [OFF]", WS_VISIBLE | WS_CHILD | BS_DEFPUSHBUTTON, 50, 155, 200, 35, g_hMenu, CM(ID_BTN_TRIGGER), hInstance, nullptr);
    CreateWindowA("BUTTON", "RADAR: [OFF]", WS_VISIBLE | WS_CHILD | BS_DEFPUSHBUTTON, 50, 200, 200, 35, g_hMenu, CM(ID_BTN_ESP), hInstance, nullptr);
    CreateWindowA("BUTTON", "ANTI-FLASH: [OFF]", WS_VISIBLE | WS_CHILD | BS_DEFPUSHBUTTON, 50, 245, 200, 35, g_hMenu, CM(ID_BTN_ANTIFLASH), hInstance, nullptr);

    CreateWindowA("BUTTON", "BHOP MOUSE5: [OFF]", WS_VISIBLE | WS_CHILD, 50, 290, 200, 35, g_hMenu, CM(ID_BTN_BHOP), hInstance, nullptr);
    CreateWindowA("BUTTON", "MOVEMENT LOG: [OFF]", WS_VISIBLE | WS_CHILD, 50, 335, 200, 35, g_hMenu, CM(ID_BTN_RECORD), hInstance, nullptr);
    CreateWindowA("STATIC", "WEAPON: SSG08", WS_VISIBLE | WS_CHILD | SS_CENTER | SS_CENTERIMAGE | WS_BORDER, 50, 380, 200, 35, g_hMenu, CM(ID_LBL_WEAPON), hInstance, nullptr);

    g_hEditFps = CreateWindowExA(WS_EX_CLIENTEDGE, "EDIT", "175", WS_VISIBLE | WS_CHILD | ES_NUMBER, 135, 430, 60, 25, g_hMenu, nullptr, hInstance, nullptr);
    CreateWindowA("BUTTON", "OK", WS_VISIBLE | WS_CHILD, 200, 430, 50, 25, g_hMenu, CM(ID_BTN_SET_FPS), hInstance, nullptr);
    CreateWindowA("STATIC", "Aim smooth", WS_VISIBLE | WS_CHILD, 40, 472, 90, 25, g_hMenu, nullptr, hInstance, nullptr);
    g_hEditSmooth = CreateWindowExA(WS_EX_CLIENTEDGE, "EDIT", "2.0", WS_VISIBLE | WS_CHILD, 135, 470, 60, 25, g_hMenu, nullptr, hInstance, nullptr);
    CreateWindowA("BUTTON", "OK", WS_VISIBLE | WS_CHILD, 200, 470, 50, 25, g_hMenu, CM(ID_BTN_SET_SMOOTH), hInstance, nullptr);
    CreateWindowA("BUTTON", "START", WS_VISIBLE | WS_CHILD | BS_DEFPUSHBUTTON, 50, 560, 200, 40, g_hMenu, CM(ID_BTN_START), hInstance, nullptr);
    CreateWindowA("STATIC", "Ready...", WS_VISIBLE | WS_CHILD | SS_CENTER, 10, 520, 280, 20, g_hMenu, CM(ID_LBL_STAT), hInstance, nullptr);
    ShowWindow(g_hMenu, SW_SHOW);
}

int main() {
    std::string offsetError;
    if (!Offsets::LoadOffsets(&offsetError)) {
        MessageBoxA(nullptr, offsetError.c_str(), "Offset loading failed", MB_ICONERROR | MB_OK);
        return 1;
    }
    SetPriorityClass(GetCurrentProcess(), HIGH_PRIORITY_CLASS);
    timeBeginPeriod(1);
    CreateMenuWindow(GetModuleHandle(nullptr));

    std::thread t1(AimbotThread);
    std::thread t2(VisualsThread);
    std::thread t3(BhopTelemetry::Run);

    MSG msg{};
    bool xLastState = false;

    while (!g_ExitApp.load()) {
        while (PeekMessageA(&msg, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessageA(&msg);
        }

        bool xState = (GetAsyncKeyState('X') & 0x8000) != 0;
        if (xState && !xLastState) {
            if (g_WeaponMode == 0) {
                g_WeaponMode = 1;
                SetDlgItemTextA(g_hMenu, ID_LBL_WEAPON, "WEAPON: OP (AWP)");
            }
            else {
                g_WeaponMode = 0;
                SetDlgItemTextA(g_hMenu, ID_LBL_WEAPON, "WEAPON: SSG08");
            }
        }
        xLastState = xState;

        if (GetAsyncKeyState(VK_HOME) & 1) {
            bool r = !g_IsRunning.load();
            g_IsRunning.store(r);
            ShowWindow(g_hMenu, r ? SW_HIDE : SW_SHOW);
        }
        if (GetAsyncKeyState(VK_END)) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    g_ExitApp.store(true);

    if (t1.joinable()) t1.join();
    if (t2.joinable()) t2.join();
    if (t3.joinable()) t3.join();

    timeEndPeriod(1);
    return 0;
}