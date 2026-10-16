#pragma warning(push)
#pragma warning(disable: 4267 4244 6001 26495)
#include "ScreenGrabber.h"
#include "YoloEngine.h"
#include "MouseController.h"
#pragma warning(pop)

#include "MemoryESP.h" 

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

std::atomic<int> g_TargetCycleUs{ 1000000 / 144 };
std::atomic<int> g_TargetClass{ -1 };
std::atomic<bool> g_IsRunning{ false };
std::atomic<bool> g_TriggerEnabled{ false };
std::atomic<bool> g_EspEnabled{ false };
std::atomic<bool> g_AntiFlashEnabled{ false };
std::atomic<bool> g_FpsBoostEnabled{ false };
std::atomic<bool> g_ExitApp{ false };

HWND g_hMenu = nullptr;
HWND g_hEditFps = nullptr;

constexpr int ID_BTN_CT = 101;
constexpr int ID_BTN_T = 102;
constexpr int ID_BTN_DM = 103;
constexpr int ID_BTN_TRIGGER = 104;
constexpr int ID_BTN_START = 105;
constexpr int ID_LBL_STAT = 106;
constexpr int ID_BTN_SET_FPS = 107;
constexpr int ID_BTN_ESP = 108;
constexpr int ID_BTN_ANTIFLASH = 109;
constexpr int ID_BTN_FPSBOOST = 111;

constexpr int TRIGGER_HOLD_KEY = VK_LCONTROL;

struct CachedEnvEntity {
    uintptr_t address;
    int offset;
};

static void SetSuperPriority(HANDLE threadHandle, DWORD coreIndex) noexcept {
    SetThreadPriority(threadHandle, THREAD_PRIORITY_TIME_CRITICAL);
    DWORD_PTR mask = (static_cast<DWORD_PTR>(1) << coreIndex);
    SetThreadAffinityMask(threadHandle, mask);
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
    ScreenGrabber grabber; if (!grabber.Initialize()) return;
    YoloEngine ai; if (!ai.Initialize("aimbot_128.engine")) return;
    MouseController mouse;

    int cx = grabber.GetScreenWidth() / 2, cy = grabber.GetScreenHeight() / 2;
    cv::Rect region(cx - 64, cy - 64, 128, 128);
    float* aiInputBuffer = ai.GetInputBuffer();

    bool isShooting = false;

    while (!g_ExitApp.load(std::memory_order_relaxed)) {
        if (!g_IsRunning.load(std::memory_order_relaxed)) {
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
            continue;
        }

        if (!mem.GetClientBase()) {
            mem.Attach(L"cs2.exe");
            if (!mem.GetClientBase()) {
                std::this_thread::sleep_for(std::chrono::milliseconds(500));
                continue;
            }
        }

        auto frameStart = std::chrono::high_resolution_clock::now();
        grabber.AcquireFrame(aiInputBuffer, region);
        std::vector<Detection> targets = ai.Detect(0.3f);

        if (!targets.empty()) {
            const Detection* bestTarget = nullptr;
            float minDistSq = 999999.0f;
            int targetClass = g_TargetClass.load(std::memory_order_relaxed);

            for (const auto& t : targets) {
                if (targetClass != 2 && t.class_id != targetClass) continue;
                float tx = t.box.x + t.box.width * 0.5f, ty = t.box.y + t.box.height * 0.5f;
                float dx = tx - 64.0f, dy = ty - 64.0f, dSq = dx * dx + dy * dy;
                if (dSq < minDistSq) { minDistSq = dSq; bestTarget = &t; }
            }

            if (bestTarget) {
                float dx = (bestTarget->box.x + bestTarget->box.width * 0.5f) - 64.0f;
                float dy = (bestTarget->box.y + bestTarget->box.height * 0.5f) - 64.0f;

                mouse.Move(dx, dy, 0.44f);

                if (g_TriggerEnabled.load(std::memory_order_relaxed)) {
                    if (GetAsyncKeyState(TRIGGER_HOLD_KEY) & 0x8000) {
                        float thresholdX = bestTarget->box.width * 0.3f;
                        float thresholdY = bestTarget->box.height * 0.3f;

                        if ((std::abs(dx) <= thresholdX) && (std::abs(dy) <= thresholdY)) {
                            bool isValidEnemy = false;
                            uintptr_t client = mem.GetClientBase();

                            if (client) {
                                uintptr_t localPawn = mem.Read<uintptr_t>(client + Offsets::dwLocalPlayerPawn);
                                if (localPawn) {
                                    int crosshairId = mem.Read<int>(localPawn + Offsets::m_iIDEntIndex);
                                    if (crosshairId > 0) {
                                        uintptr_t entList = mem.Read<uintptr_t>(client + Offsets::dwEntityList);
                                        uintptr_t targetPawn = ResolveEntityByIndex(mem, entList, crosshairId);

                                        if (targetPawn) {
                                            int localTeam = mem.Read<int>(localPawn + Offsets::m_iTeamNum) & 0xFF;
                                            int targetTeam = mem.Read<int>(targetPawn + Offsets::m_iTeamNum) & 0xFF;
                                            int targetHealth = mem.Read<int>(targetPawn + Offsets::m_iHealth);

                                            if ((targetTeam != localTeam) && (targetHealth > 0)) {
                                                isValidEnemy = true;
                                            }
                                        }
                                    }
                                }
                            }

                            if (isValidEnemy) {
                                if (!isShooting) {
                                    mouse_event(MOUSEEVENTF_LEFTDOWN, 0, 0, 0, 0);
                                    isShooting = true;
                                }
                            }
                            else if (isShooting) {
                                mouse_event(MOUSEEVENTF_LEFTUP, 0, 0, 0, 0);
                                isShooting = false;
                            }
                        }
                        else if (isShooting) {
                            mouse_event(MOUSEEVENTF_LEFTUP, 0, 0, 0, 0);
                            isShooting = false;
                        }
                    }
                    else if (isShooting) {
                        mouse_event(MOUSEEVENTF_LEFTUP, 0, 0, 0, 0);
                        isShooting = false;
                    }
                }
            }
            else if (isShooting) {
                mouse_event(MOUSEEVENTF_LEFTUP, 0, 0, 0, 0);
                isShooting = false;
            }
        }
        else if (isShooting) {
            mouse_event(MOUSEEVENTF_LEFTUP, 0, 0, 0, 0);
            isShooting = false;
        }

        auto frameEnd = std::chrono::high_resolution_clock::now();
        long long elapsedUs = std::chrono::duration_cast<std::chrono::microseconds>(frameEnd - frameStart).count();
        int currentCycle = g_TargetCycleUs.load(std::memory_order_relaxed);

        if (elapsedUs < currentCycle) {
            long long sleepVal = currentCycle - elapsedUs;
            if (sleepVal > 1500) std::this_thread::sleep_for(std::chrono::microseconds(sleepVal - 1000));
            while (std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::high_resolution_clock::now() - frameStart).count() < currentCycle) _mm_pause();
        }
    }

    if (isShooting) {
        mouse_event(MOUSEEVENTF_LEFTUP, 0, 0, 0, 0);
    }
}

static void VisualsThread() {
    Memory mem;
    int loopCounter = 0;
    uintptr_t cachedPawns[65]{};
    std::vector<CachedEnvEntity> cachedEnvEntities;
    bool mapScanned = false;

    while (!g_ExitApp.load()) {
        if (!mem.GetClientBase()) {
            mem.Attach(L"cs2.exe");
            memset(cachedPawns, 0, sizeof(cachedPawns));
            cachedEnvEntities.clear();
            mapScanned = false;
            std::this_thread::sleep_for(std::chrono::seconds(1));
            continue;
        }

        if (g_EspEnabled.load() || g_AntiFlashEnabled.load() || g_FpsBoostEnabled.load()) {
            uintptr_t client = mem.GetClientBase();
            if (client) {
                uintptr_t localPawn = mem.Read<uintptr_t>(client + Offsets::dwLocalPlayerPawn);

                if (localPawn) {
                    if (g_AntiFlashEnabled.load()) {
                        mem.Write<float>(localPawn + Offsets::m_flFlashMaxAlpha, 0.0f);
                        mem.Write<float>(localPawn + Offsets::m_flFlashDuration, 0.0f);
                    }

                    if (g_EspEnabled.load()) {
                        uintptr_t entList = mem.Read<uintptr_t>(client + Offsets::dwEntityList);
                        int lTeam = mem.Read<int>(localPawn + Offsets::m_iTeamNum) & 0xFF;

                        if (loopCounter % 50 == 0) {
                            for (int i = 1; i <= 64; i++) {
                                uintptr_t ctrl = ResolveEntityByIndex(mem, entList, i);
                                if (!ctrl) { cachedPawns[i] = 0; continue; }

                                uint32_t pawnHandle = mem.Read<uint32_t>(ctrl + Offsets::m_hPlayerPawn);
                                if (!pawnHandle || pawnHandle == 0xFFFFFFFF) { cachedPawns[i] = 0; continue; }

                                cachedPawns[i] = ResolveEntityByIndex(mem, entList, static_cast<int>(pawnHandle & Offsets::EntityHandleMask));
                            }
                        }

                        for (int i = 1; i <= 64; i++) {
                            uintptr_t pawn = cachedPawns[i];
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

                if (g_FpsBoostEnabled.load()) {
                    if (!mapScanned) {
                        uintptr_t entList = mem.Read<uintptr_t>(client + Offsets::dwEntityList);
                        int highestIndex = mem.Read<int>(client + Offsets::dwGameEntitySystem_highestEntityIndex);

                        for (int i = 65; i <= highestIndex; i++) {
                            uintptr_t ctrl = ResolveEntityByIndex(mem, entList, i);
                            if (!ctrl) continue;

                            uintptr_t pEntityIdentity = mem.Read<uintptr_t>(ctrl + Offsets::m_pEntity);
                            if (!pEntityIdentity) continue;

                            uintptr_t pDesignerName = mem.Read<uintptr_t>(pEntityIdentity + Offsets::m_designerName);
                            if (!pDesignerName) continue;

                            std::string designerName = mem.ReadString(pDesignerName, 32);

                            if (designerName.find("env_volumetric_fog") != std::string::npos) {
                                cachedEnvEntities.push_back({ ctrl, static_cast<int>(Offsets::m_bActiveVolumetricFog) });
                            }
                            else if (designerName.find("post_processing_volume") != std::string::npos) {
                                cachedEnvEntities.push_back({ ctrl, static_cast<int>(Offsets::m_bPostProcessMaster) });
                            }
                        }
                        mapScanned = true;
                    }

                    if (loopCounter % 500 == 0) {
                        for (const auto& envEnt : cachedEnvEntities) {
                            mem.Write<bool>(envEnt.address + envEnt.offset, false);
                        }
                    }
                }
            }
        }

        loopCounter++;
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
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
        case ID_BTN_FPSBOOST: g_FpsBoostEnabled = !g_FpsBoostEnabled; SetDlgItemTextA(hwnd, ID_BTN_FPSBOOST, g_FpsBoostEnabled ? "FPS BOOST: [ON]" : "FPS BOOST: [OFF]"); break;
        case ID_BTN_SET_FPS: { char buf[16]{}; GetWindowTextA(g_hEditFps, buf, 15); try { int val = std::stoi(buf); if (val > 0) g_TargetCycleUs = 1000000 / val; } catch (...) {} break; }
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
    g_hMenu = CreateWindowExA(WS_EX_TOPMOST, CLASS_NAME, "Vuong_CPP_Sync", WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU, 100, 100, 300, 600, nullptr, nullptr, hInstance, nullptr);
    auto CM = [](int id) { return reinterpret_cast<HMENU>(static_cast<UINT_PTR>(id)); };
    CreateWindowA("BUTTON", "SELECT CT", WS_VISIBLE | WS_CHILD | BS_DEFPUSHBUTTON, 50, 20, 200, 35, g_hMenu, CM(ID_BTN_CT), hInstance, nullptr);
    CreateWindowA("BUTTON", "SELECT T", WS_VISIBLE | WS_CHILD | BS_DEFPUSHBUTTON, 50, 65, 200, 35, g_hMenu, CM(ID_BTN_T), hInstance, nullptr);
    CreateWindowA("BUTTON", "DEATHMATCH", WS_VISIBLE | WS_CHILD | BS_DEFPUSHBUTTON, 50, 110, 200, 35, g_hMenu, CM(ID_BTN_DM), hInstance, nullptr);
    CreateWindowA("BUTTON", "AI TRIGGER: [OFF]", WS_VISIBLE | WS_CHILD | BS_DEFPUSHBUTTON, 50, 155, 200, 35, g_hMenu, CM(ID_BTN_TRIGGER), hInstance, nullptr);
    CreateWindowA("BUTTON", "RADAR: [OFF]", WS_VISIBLE | WS_CHILD | BS_DEFPUSHBUTTON, 50, 200, 200, 35, g_hMenu, CM(ID_BTN_ESP), hInstance, nullptr);
    CreateWindowA("BUTTON", "ANTI-FLASH: [OFF]", WS_VISIBLE | WS_CHILD | BS_DEFPUSHBUTTON, 50, 245, 200, 35, g_hMenu, CM(ID_BTN_ANTIFLASH), hInstance, nullptr);
    CreateWindowA("BUTTON", "FPS BOOST: [OFF]", WS_VISIBLE | WS_CHILD | BS_DEFPUSHBUTTON, 50, 290, 200, 35, g_hMenu, CM(ID_BTN_FPSBOOST), hInstance, nullptr);
    g_hEditFps = CreateWindowExA(WS_EX_CLIENTEDGE, "EDIT", "144", WS_VISIBLE | WS_CHILD | ES_NUMBER, 135, 335, 60, 25, g_hMenu, nullptr, hInstance, nullptr);
    CreateWindowA("BUTTON", "OK", WS_VISIBLE | WS_CHILD, 200, 335, 50, 25, g_hMenu, CM(ID_BTN_SET_FPS), hInstance, nullptr);
    CreateWindowA("BUTTON", "START", WS_VISIBLE | WS_CHILD | BS_DEFPUSHBUTTON, 50, 415, 200, 40, g_hMenu, CM(ID_BTN_START), hInstance, nullptr);
    CreateWindowA("STATIC", "Ready...", WS_VISIBLE | WS_CHILD | SS_CENTER, 10, 385, 280, 20, g_hMenu, CM(ID_LBL_STAT), hInstance, nullptr);
    ShowWindow(g_hMenu, SW_SHOW);
}

int main() {
    SetPriorityClass(GetCurrentProcess(), HIGH_PRIORITY_CLASS);
    timeBeginPeriod(1);
    CreateMenuWindow(GetModuleHandle(nullptr));

    std::thread t1(AimbotThread);
    std::thread t2(VisualsThread);

    MSG msg{};
    while (!g_ExitApp.load()) {
        while (PeekMessageA(&msg, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessageA(&msg);
        }
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

    timeEndPeriod(1);
    return 0;
}