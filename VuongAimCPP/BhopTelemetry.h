#pragma once
#include "MemoryESP.h"
#include <windows.h>
#include <timeapi.h>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <ctime>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <string>
#include <array>
#include <algorithm>

extern std::atomic<bool> g_BhopEnabled;
extern std::atomic<bool> g_RecordMovementEnabled;
extern std::atomic<bool> g_IsRunning;
extern std::atomic<bool> g_ExitApp;

namespace BhopTelemetry {
    using Clock = std::chrono::steady_clock;
    constexpr auto kSampleInterval = std::chrono::milliseconds(8);
    constexpr auto kLoopInterval = std::chrono::milliseconds(1);
    constexpr auto kJumpHold = std::chrono::milliseconds(13);
    constexpr int kBhopHoldKey = VK_XBUTTON2;
    constexpr auto kGroundRetry = std::chrono::milliseconds(80);
    constexpr double kMinHorizontalSpeed = 120.0;
    constexpr double kMinContinueHorizontalSpeed = 40.0;
    constexpr uint32_t FL_ONGROUND = 1u;

    inline bool IsForegroundGame(DWORD pid) {
        HWND wnd = GetForegroundWindow();
        DWORD fgPid = 0;
        if (wnd) GetWindowThreadProcessId(wnd, &fgPid);
        return pid && pid == fgPid;
    }
    inline bool IsHeld(int vk) {
        return (GetAsyncKeyState(vk) & 0x8000) != 0;
    }
    inline bool SendSpace(bool down) {
        INPUT in{};
        in.type = INPUT_KEYBOARD;
        in.ki.wScan = static_cast<WORD>(MapVirtualKeyW(VK_SPACE, MAPVK_VK_TO_VSC));
        in.ki.dwFlags = KEYEVENTF_SCANCODE | (down ? 0 : KEYEVENTF_KEYUP);
        return SendInput(1, &in, sizeof(INPUT)) == 1;
    }
    inline double Speed2(float x, float y) {
        return std::hypot(static_cast<double>(x), static_cast<double>(y));
    }
    inline double NormalizeDeg(double angle) {
        return std::remainder(angle, 360.0);
    }
    inline double Deg(double radians) {
        return radians * 180.0 / 3.14159265358979323846;
    }
    inline std::string Timestamp() {
        SYSTEMTIME st{}; GetLocalTime(&st);
        std::ostringstream s;
        s << std::setfill('0') << std::setw(4) << st.wYear
            << std::setw(2) << st.wMonth << std::setw(2) << st.wDay << "_"
            << std::setw(2) << st.wHour << std::setw(2) << st.wMinute
            << std::setw(2) << st.wSecond << "_" << std::setw(3) << st.wMilliseconds;
        return s.str();
    }
    inline void LogOffsets(std::ostream& out) {
        out << "# FORMAT=BHopTelemetryV2; ALL_OFFSETS_FROM_DUMPS_GENERATED=2026-10-15; VERIFY_BUILD_VERSION\n";
        out << "# module-relative: dwLocalPlayerPawn=0x" << std::hex << Offsets::dwLocalPlayerPawn
            << "; dwViewAngles=0x" << Offsets::dwViewAngles
            << "; dwEntityList=0x" << Offsets::dwEntityList << std::dec << "\n";
        out << "# class-relative: C_BaseEntity::m_vecAbsVelocity=0x" << std::hex << Offsets::m_vecAbsVelocity
            << "; C_BaseEntity::m_fFlags=0x" << Offsets::m_fFlags
            << "; C_BasePlayerPawn::v_angle=0x" << Offsets::v_angle
            << "; C_BasePlayerPawn::m_pMovementServices=0x" << Offsets::m_pMovementServices
            << "; CCSPlayer_MovementServices::m_ModernJump=0x" << Offsets::m_ModernJump
            << std::dec << "\n";
        out << "# This log reads game memory; BHOP uses SendInput(Space); no view-angle writes, no auto-strafe input.\n";
        out << "# BHOP requires enabled mode + holding Mouse5 + movement key; 120 u/s to start, 40 u/s to continue.\n";
        out << "# GetAsyncKeyState is effective key state (may also reflect injected Space), not a physical-input trace.\n";
        out << "# acceleration = finite difference of measured velocity, not server physics engine acceleration.\n";
        out << "# dot_* fields show projection onto 8 keyboard directions relative to camera yaw; they are not optimality scores.\n";
    }
    struct JumpPulse {
        bool down = false;
        bool armed = true;
        bool started = false;
        bool lastSendOK = true;
        uint64_t presses = 0, sendFailures = 0, retries = 0;
        Clock::time_point lastChange = Clock::now() - std::chrono::seconds(1);
        Clock::time_point lastPress = Clock::now() - std::chrono::seconds(1);

        void Stop() {
            if (!down) return;
            if (!SendSpace(false)) ++sendFailures;
            down = false;
            lastChange = Clock::now();
        }

        void Update(bool active, bool grounded, bool movementReady) {
            const auto now = Clock::now();
            if (!active) {
                Stop();
                armed = true;
                started = false;
                return;
            }
            if (!grounded) {
                if (down) Stop();
                armed = true;
                return;
            }
            if (down && now - lastChange >= kJumpHold) Stop();
            if (!movementReady) {
                if (down) Stop();
                return;
            }
            if (!armed && !down && now - lastPress >= kGroundRetry) {
                armed = true;
                ++retries;
            }
            if (!armed || down || IsHeld(VK_SPACE)) return;
            lastSendOK = SendSpace(true);
            lastChange = now;
            if (lastSendOK) {
                lastPress = now;
                down = true;
                armed = false;
                started = true;
                ++presses;
            }
            else {
                ++sendFailures;
            }
        }
    };

    inline void Run() {
        Memory mem;
        JumpPulse jump;
        std::ofstream logfile;
        std::string logName;
        auto lastFileAttempt = Clock::now() - std::chrono::seconds(5);
        auto began = Clock::now();
        auto lastSample = began - kSampleInterval;
        auto lastFlush = began;
        auto lastReconnectAttempt = began - std::chrono::seconds(5);
        bool havePrev = false, prevOnGround = false;
        Vector3 prevV{};
        auto prevT = began;
        uint64_t rowCount = 0, invalidCount = 0, landings = 0, takeoffs = 0, groundFrames = 0, airFrames = 0;
        double maxSpeed = 0.0, maxAirSpeed = 0.0, maxLandSpeed = 0.0;
        auto CloseLog = [&]() {
            if (!logfile.is_open()) return;
            logfile << "# SUMMARY: samples=" << rowCount << "; invalid_samples=" << invalidCount
                << "; landings=" << landings << "; takeoffs=" << takeoffs
                << "; ground_samples=" << groundFrames << "; air_samples=" << airFrames
                << "; jump_presses=" << jump.presses << "; jump_retries=" << jump.retries << "; jump_send_failures=" << jump.sendFailures
                << "; max_speed=" << maxSpeed << "; max_air_speed=" << maxAirSpeed
                << "; max_landing_speed=" << maxLandSpeed << "\n";
            logfile.flush(); logfile.close();
            };
        while (!g_ExitApp.load(std::memory_order_relaxed)) {
            auto now = Clock::now();
            if (g_RecordMovementEnabled.load(std::memory_order_relaxed) && !logfile.is_open() &&
                now - lastFileAttempt >= std::chrono::seconds(2)) {
                lastFileAttempt = now;
                auto fileName = "bhop_telemetry_" + Timestamp() + ".txt";
                auto fullPath = Offsets::ExecutableDirectory() / fileName;
                logfile.open(fullPath, std::ios::out | std::ios::trunc);
                if (!logfile) {
                    logfile.clear();
                    try {
                        fullPath = std::filesystem::temp_directory_path() / fileName;
                        logfile.open(fullPath, std::ios::out | std::ios::trunc);
                    }
                    catch (...) { logfile.clear(); }
                }
                if (logfile) {
                    logName = fullPath.string();
                    std::cout << "Movement telemetry: " << logName << std::endl;
                    began = now; lastSample = now - kSampleInterval;
                    havePrev = false; rowCount = invalidCount = landings = takeoffs = groundFrames = airFrames = 0;
                    maxSpeed = maxAirSpeed = maxLandSpeed = 0.0;
                    jump.presses = jump.sendFailures = jump.retries = 0;
                    LogOffsets(logfile);
                    logfile << "elapsed_ms,wall_epoch_ms,attached,game_foreground,app_running,bhop_enabled,valid,read_error,"
                        << "health,flags,on_ground,on_ground_last_tick,ground_entity,move_type,"
                        << "velocity_x,velocity_y,velocity_z,speed_xy,velocity_yaw_deg,"
                        << "client_view_pitch,client_view_yaw,pawn_view_pitch,pawn_view_yaw,"
                        << "yaw_diff,delta_velocity_x,delta_velocity_y,delta_speed_xy,sample_dt_ms,"
                        << "origin_x,origin_y,origin_z,movement_services_ok,scene_node_ok,"
                        << "w,a,s,d,space,ctrl,shift,"
                        << "jump_injected_down,total_jump_presses,last_jump_send_ok,"
                        << "input_forward_cmd,input_left_cmd,input_forward,input_left,"
                        << "maxspeed,stamina,walkwish_x,walkwish_y,"
                        << "last_jump_tick,last_jump_frac,last_jump_velocity_z,"
                        << "last_landed_tick,last_landed_frac,last_landed_velocity_x,last_landed_velocity_y,last_landed_velocity_z,"
                        << "dot_W,dot_WD,dot_D,dot_SD,dot_S,dot_SA,dot_A,dot_WA,"
                        << "hold_key_down,movement_ready,bhop_active,jump_armed,chain_started,total_jump_retries,event\n";
                    logfile << std::fixed << std::setprecision(4);
                }
                else {
                    std::cerr << "Movement telemetry: could not open TXT log file" << std::endl;
                }
            }
            if (!g_RecordMovementEnabled.load(std::memory_order_relaxed) && logfile.is_open()) CloseLog();
            const bool detailedSampleDue = logfile.is_open() && now - lastSample >= kSampleInterval;

            bool appEnabled = g_IsRunning.load(std::memory_order_relaxed);
            bool bhopEnabled = g_BhopEnabled.load(std::memory_order_relaxed);
            if (!bhopEnabled && !logfile.is_open()) {
                jump.Update(false, false, false);
                std::this_thread::sleep_for(std::chrono::milliseconds(20));
                continue;
            }
            if (!mem.IsAlive() || !mem.GetClientBase()) {
                jump.Update(false, false, false);
                if (now - lastReconnectAttempt >= std::chrono::seconds(2)) {
                    mem.Attach(L"cs2.exe"); lastReconnectAttempt = now;
                    havePrev = false;
                }
            }
            const bool attached = mem.IsAlive() && mem.GetClientBase();
            const bool foreground = attached && IsForegroundGame(mem.GetProcessID());
            uintptr_t pawn = 0;
            Vector3 vel{}, clientView{}, pawnView{}, origin{};
            bool readOK = false;
            bool coreOK = false;
            bool groundLastTick = false;
            uint32_t flags = 0, groundHandle = 0;
            uint8_t moveType = 0;
            int32_t health = 0;
            uintptr_t movement = 0;
            bool movementOK = false, nodeOK = false;
            float fc = 0, lc = 0, f = 0, l = 0, maxspeed = 0, stamina = 0, lastJumpFrac = 0, lastJumpVz = 0;
            int32_t lastJumpTick = 0, lastLandedTick = 0;
            float lastLandedFrac = 0, landingX = 0, landingY = 0, landingZ = 0;
            Vector2 wish{};
            std::string error = "ok";
            if (attached) {
                auto base = mem.GetClientBase();
                if (!mem.TryRead(base + Offsets::dwLocalPlayerPawn, pawn) || !pawn) {
                    error = "pawn_null_or_unreadable";
                }
                else {
                    bool a = mem.TryRead(pawn + Offsets::m_vecAbsVelocity, vel);
                    bool b = mem.TryRead(pawn + Offsets::m_fFlags, flags);
                    bool c = mem.TryRead(pawn + Offsets::m_iHealth, health);
                    bool d = mem.TryRead(pawn + Offsets::v_angle, pawnView);
                    bool e = mem.TryRead(base + Offsets::dwViewAngles, clientView);
                    coreOK = a && b && c &&
                        std::isfinite(vel.x) && std::isfinite(vel.y) && std::isfinite(vel.z);
                    readOK = coreOK && d && e &&
                        std::isfinite(pawnView.y) && std::isfinite(clientView.y);
                    if (!coreOK) error = "velocity_flags_or_health_unreadable";
                    else if (!readOK) error = "yaw_unreadable_or_nonfinite";
                    if (detailedSampleDue) {
                        mem.TryRead(pawn + Offsets::m_bOnGroundLastTick, groundLastTick);
                        mem.TryRead(pawn + Offsets::m_hGroundEntity, groundHandle);
                        mem.TryRead(pawn + Offsets::m_MoveType, moveType);
                        uintptr_t node = 0;
                        if (mem.TryRead(pawn + Offsets::m_pGameSceneNode, node) && node)
                            nodeOK = mem.TryRead(node + Offsets::m_vecAbsOrigin, origin);
                        if (mem.TryRead(pawn + Offsets::m_pMovementServices, movement) && movement) {
                            movementOK = mem.TryRead(movement + Offsets::m_flCmdForwardMove, fc);
                            mem.TryRead(movement + Offsets::m_flCmdLeftMove, lc);
                            mem.TryRead(movement + Offsets::m_flForwardMove, f);
                            mem.TryRead(movement + Offsets::m_flLeftMove, l);
                            mem.TryRead(movement + Offsets::m_flMaxspeed, maxspeed);
                            mem.TryRead(movement + Offsets::m_flStamina, stamina);
                            mem.TryRead(movement + Offsets::m_vecWalkWishVel, wish);
                            mem.TryRead(movement + Offsets::m_nLastJumpTick, lastJumpTick);
                            mem.TryRead(movement + Offsets::m_flLastJumpFrac, lastJumpFrac);
                            mem.TryRead(movement + Offsets::m_flLastJumpVelocityZ, lastJumpVz);
                            auto modern = movement + Offsets::m_ModernJump; // embedded, not a pointer
                            mem.TryRead(modern + Offsets::m_nLastLandedTick, lastLandedTick);
                            mem.TryRead(modern + Offsets::m_flLastLandedFrac, lastLandedFrac);
                            mem.TryRead(modern + Offsets::m_flLastLandedVelocityX, landingX);
                            mem.TryRead(modern + Offsets::m_flLastLandedVelocityY, landingY);
                            mem.TryRead(modern + Offsets::m_flLastLandedVelocityZ, landingZ);
                        }
                    } // detailedSampleDue
                }
            }
            else error = "not_attached";
            bool grounded = coreOK && (flags & FL_ONGROUND) != 0;
            const bool holdDown = IsHeld(kBhopHoldKey);
            const bool movingKeys = IsHeld('W') || IsHeld('A') || IsHeld('S') || IsHeld('D');
            const double horizontalSpeed = coreOK ? Speed2(vel.x, vel.y) : 0.0;
            const double minSpeed = jump.started ? kMinContinueHorizontalSpeed : kMinHorizontalSpeed;
            const bool movementReady = movingKeys && horizontalSpeed >= minSpeed;
            const bool bhopActive = bhopEnabled && appEnabled && foreground && coreOK && health > 0 && holdDown;
            jump.Update(bhopActive, grounded, movementReady);
            if (detailedSampleDue) {
                double speed = readOK ? Speed2(vel.x, vel.y) : 0;
                double yawVel = (readOK && speed > 0.01) ? Deg(std::atan2(vel.y, vel.x)) : 0;
                double diff = readOK ? NormalizeDeg(yawVel - pawnView.y) : 0;
                double dt = havePrev && readOK ? std::chrono::duration<double>(now - prevT).count() : 0;
                double ax = dt > 0 ? (vel.x - prevV.x) / dt : 0;
                double ay = dt > 0 ? (vel.y - prevV.y) / dt : 0;
                double prevSpeed = Speed2(prevV.x, prevV.y);
                double ds = dt > 0 ? (speed - prevSpeed) / dt : 0;
                std::string event = "none";
                if (!readOK) { event = "read_failed"; ++invalidCount; havePrev = false; }
                else {
                    if (havePrev && !prevOnGround && grounded) {
                        event = "LANDED"; ++landings; maxLandSpeed = std::max(maxLandSpeed, speed);
                    }
                    else if (havePrev && prevOnGround && !grounded) {
                        event = "TAKEOFF"; ++takeoffs;
                    }
                    if (grounded) ++groundFrames; else ++airFrames;
                    maxSpeed = std::max(maxSpeed, speed);
                    if (!grounded) maxAirSpeed = std::max(maxAirSpeed, speed);
                }
                auto epoch = std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::system_clock::now().time_since_epoch()).count();
                auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - began).count();
                const int keyW = IsHeld('W'), keyA = IsHeld('A'), keyS = IsHeld('S'), keyD = IsHeld('D');
                const int space = IsHeld(VK_SPACE), ctrl = IsHeld(VK_CONTROL), shift = IsHeld(VK_SHIFT);
                logfile << elapsed << ',' << epoch << ',' << int(attached) << ',' << int(foreground)
                    << ',' << int(appEnabled) << ',' << int(bhopEnabled) << ',' << int(readOK)
                    << ',' << error << ',' << health << ',' << flags << ',' << int(grounded)
                    << ',' << int(groundLastTick) << ',' << groundHandle << ',' << int(moveType)
                    << ',' << vel.x << ',' << vel.y << ',' << vel.z << ',' << speed
                    << ',' << yawVel << ',' << clientView.x << ',' << clientView.y
                    << ',' << pawnView.x << ',' << pawnView.y << ',' << diff
                    << ',' << ax << ',' << ay << ',' << ds << ',' << (dt * 1000.0)
                    << ',' << origin.x << ',' << origin.y << ',' << origin.z << ',' << int(movementOK) << ',' << int(nodeOK)
                    << ',' << keyW << ',' << keyA << ',' << keyS << ',' << keyD
                    << ',' << space << ',' << ctrl << ',' << shift
                    << ',' << int(jump.down) << ',' << jump.presses << ',' << int(jump.lastSendOK)
                    << ',' << fc << ',' << lc << ',' << f << ',' << l << ',' << maxspeed
                    << ',' << stamina << ',' << wish.x << ',' << wish.y
                    << ',' << lastJumpTick << ',' << lastJumpFrac << ',' << lastJumpVz
                    << ',' << lastLandedTick << ',' << lastLandedFrac
                    << ',' << landingX << ',' << landingY << ',' << landingZ;
                constexpr double PI = 3.14159265358979323846;
                // Forward (W), forward-right, right, backward-right, backward, backward-left, left, forward-left.
                for (int i = 0; i < 8; ++i) {
                    double theta = (pawnView.y - i * 45.0) * PI / 180.0;
                    logfile << ',' << (vel.x * std::cos(theta) + vel.y * std::sin(theta));
                }
                logfile << ',' << int(holdDown) << ',' << int(movementReady)
                    << ',' << int(bhopActive) << ',' << int(jump.armed)
                    << ',' << int(jump.started) << ',' << jump.retries
                    << ',' << event << '\n';
                ++rowCount;
                lastSample = now;
                if (readOK) { prevV = vel; prevOnGround = grounded; prevT = now; havePrev = true; }
                if (now - lastFlush >= std::chrono::seconds(1)) { logfile.flush(); lastFlush = now; }
            }
            std::this_thread::sleep_for(kLoopInterval);
        }
        jump.Stop();
        CloseLog();
    }
}
