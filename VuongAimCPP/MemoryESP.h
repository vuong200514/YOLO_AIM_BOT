#pragma once
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <TlHelp32.h>
#include <cstdint>
#include <cstddef>
#include <string>
#include <atomic>
#include <vector>
#include <chrono>
#include <thread>
#include <iostream>
#include <fstream>
#include <sstream>
#include <regex>
#include <filesystem>
#include <iterator>
#include <limits>

static_assert(sizeof(void*) == 8, "Build this project for x64 (not Win32).");
extern std::atomic<bool> g_FpsBoostEnabled;
struct Vector2 { float x, y; };
struct Vector3 { float x, y, z; };
struct Matrix4x4 { float m[16]; };

namespace Offsets {
    // Fallback numbers are copied from the dump dated 2026-10-15; files must load successfully.
    inline std::ptrdiff_t dwEntityList = 0x271A908;
    inline std::ptrdiff_t dwLocalPlayerPawn = 0x2565718;
    inline std::ptrdiff_t dwViewMatrix = 0x256B020;
    inline std::ptrdiff_t dwViewAngles = 0x257BCC8;
    inline std::ptrdiff_t dwGlobalVars = 0x2230090;
    inline std::ptrdiff_t dwGameEntitySystem_highestEntityIndex = 0x2120;
    inline std::ptrdiff_t dwGameRules = 0x25651D0;

    inline std::ptrdiff_t m_iHealth = 0x34C;
    inline std::ptrdiff_t m_iTeamNum = 0x3E7;
    inline std::ptrdiff_t m_hPlayerPawn = 0x92C;
    inline std::ptrdiff_t m_iIDEntIndex = 0x36DC;
    inline std::ptrdiff_t m_entitySpottedState = 0x1E80;
    inline std::ptrdiff_t m_bSpotted = 0x8;
    inline std::ptrdiff_t m_bSpottedByMask = 0xC;
    inline std::ptrdiff_t m_flFlashDuration = 0x1510;
    inline std::ptrdiff_t m_flFlashMaxAlpha = 0x150C;
    inline std::ptrdiff_t m_pEntity = 0x10;
    inline std::ptrdiff_t m_designerName = 0x20;
    inline std::ptrdiff_t m_clrRender = 0xCA0;
    inline std::ptrdiff_t m_Glow = 0xDE8;
    inline std::ptrdiff_t m_bGlowing = 0x51;
    inline std::ptrdiff_t m_glowColorOverride = 0x40;
    inline std::ptrdiff_t m_iGlowType = 0x30;

    // Telemetry: offsets are class-relative, NOT module-relative.
    inline std::ptrdiff_t m_vecAbsVelocity = 0x3F8;    // C_BaseEntity::Vector
    inline std::ptrdiff_t m_fFlags = 0x3F4;           // C_BaseEntity::uint32
    inline std::ptrdiff_t m_hGroundEntity = 0x530;    // C_BaseEntity::CHandle
    inline std::ptrdiff_t m_MoveType = 0x525;         // C_BaseEntity::MoveType_t
    inline std::ptrdiff_t m_pGameSceneNode = 0x330;   // C_BaseEntity::CGameSceneNode*
    inline std::ptrdiff_t m_vecAbsOrigin = 0xC8;      // CGameSceneNode::VectorWS
    inline std::ptrdiff_t m_pMovementServices = 0x1330; // C_BasePlayerPawn::*
    inline std::ptrdiff_t v_angle = 0x13A8;            // C_BasePlayerPawn::QAngle
    inline std::ptrdiff_t m_bOnGroundLastTick = 0x1D88; // C_CSPlayerPawn::bool
    inline std::ptrdiff_t m_flCmdForwardMove = 0x1A0; // CPlayer_MovementServices::float
    inline std::ptrdiff_t m_flCmdLeftMove = 0x1A4;
    inline std::ptrdiff_t m_flForwardMove = 0x1C0;
    inline std::ptrdiff_t m_flLeftMove = 0x1C4;
    inline std::ptrdiff_t m_flMaxspeed = 0x1AC;
    inline std::ptrdiff_t m_flStamina = 0x694;         // CCSPlayer_MovementServices::float
    inline std::ptrdiff_t m_vecWalkWishVel = 0x7A4;  // CCSPlayer_MovementServices::Vector2D
    inline std::ptrdiff_t m_ModernJump = 0x6C8;       // Embedded CCSPlayerModernJump
    inline std::ptrdiff_t m_nLastLandedTick = 0x20;   // relative to embedded ModernJump
    inline std::ptrdiff_t m_flLastLandedFrac = 0x24;
    inline std::ptrdiff_t m_flLastLandedVelocityX = 0x28;
    inline std::ptrdiff_t m_flLastLandedVelocityY = 0x2C;
    inline std::ptrdiff_t m_flLastLandedVelocityZ = 0x30;
    inline std::ptrdiff_t m_nLastJumpTick = 0x700;     // CCSPlayer_MovementServices::GameTick_t
    inline std::ptrdiff_t m_flLastJumpFrac = 0x704;
    inline std::ptrdiff_t m_flLastJumpVelocityZ = 0x708;

    constexpr uintptr_t EntityListFirstChunkOffset = 0x10;
    constexpr uintptr_t EntityChunkPointerStride = 0x8;
    constexpr uintptr_t EntityIdentityStride = 0x70;
    constexpr uintptr_t EntityHandleMask = 0x7FFF;
    constexpr uintptr_t EntityHandleEntryMask = 0x1FF;
    constexpr uintptr_t EntityHandleChunkShift = 9;

    inline std::filesystem::path ExecutableDirectory() {
        wchar_t buf[32768] = {};
        DWORD n = GetModuleFileNameW(nullptr, buf, 32768);
        if (!n || n >= 32768) return {};
        return std::filesystem::path(buf).parent_path();
    }
    inline bool ReadFileContent(const std::wstring& filename, std::string& out) {
        std::ifstream f(ExecutableDirectory() / filename, std::ios::binary);
        if (!f) return false;
        out.assign(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
        return true;
    }
    // C++ brace-matching prevents accidental matches against identically named fields in other classes.
    inline bool NamespaceBody(const std::string& file, const std::string& ns, std::string& body) {
        const std::regex pattern("\\bnamespace\\s+" + ns + "\\s*\\{");
        std::smatch match;
        if (!std::regex_search(file, match, pattern)) return false;
        auto open = static_cast<size_t>(match.position() + match.length() - 1);
        int depth = 0;
        for (size_t pos = open; pos < file.size(); ++pos) {
            if (file[pos] == '{') ++depth;
            else if (file[pos] == '}' && --depth == 0) {
                body = file.substr(open + 1, pos - open - 1);
                return true;
            }
        }
        return false;
    }
    inline bool GetField(const std::string& namespaceBody, const std::string& field, std::ptrdiff_t& dest) {
        std::regex pattern("\\b" + field + R"(\s*=\s*(0x[0-9a-fA-F]+))");
        std::smatch m;
        if (!std::regex_search(namespaceBody, m, pattern)) return false;
        try { dest = static_cast<std::ptrdiff_t>(std::stoull(m[1].str(), nullptr, 16)); return true; }
        catch (...) { return false; }
    }
    inline bool GetScopedField(const std::string& contents, const std::string& cls,
        const std::string& field, std::ptrdiff_t& dest) {
        std::string body;
        return NamespaceBody(contents, cls, body) && GetField(body, field, dest);
    }
    inline bool LoadOffsets(std::string* error = nullptr) {
        std::string offsetsContent, clientContent;
        if (!(ReadFileContent(L"offsets.hpp", offsetsContent) || ReadFileContent(L"offset.hpp", offsetsContent))) {
            if (error) *error = "Missing offsets.hpp beside the executable";
            return false;
        }
        if (!(ReadFileContent(L"client_dll.hpp", clientContent) || ReadFileContent(L"client.dll.hpp", clientContent))) {
            if (error) *error = "Missing client_dll.hpp beside the executable";
            return false;
        }
        std::string module;
        if (!NamespaceBody(offsetsContent, "client_dll", module)) {
            if (error) *error = "Missing offsets::client_dll namespace";
            return false;
        }
        std::vector<std::string> missing;
#define LOAD_MOD(F) do { if (!GetField(module, #F, F)) missing.emplace_back(std::string("offsets::client_dll::") + #F); } while (false)
        LOAD_MOD(dwEntityList); LOAD_MOD(dwLocalPlayerPawn); LOAD_MOD(dwViewMatrix);
        LOAD_MOD(dwViewAngles); LOAD_MOD(dwGlobalVars);
        LOAD_MOD(dwGameEntitySystem_highestEntityIndex); LOAD_MOD(dwGameRules);
#undef LOAD_MOD
#define LOAD_CLASS(CLASS, FIELD) do { if (!GetScopedField(clientContent, #CLASS, #FIELD, FIELD)) missing.emplace_back(std::string(#CLASS "::" #FIELD)); } while (false)
        LOAD_CLASS(C_BaseEntity, m_iHealth);
        LOAD_CLASS(C_BaseEntity, m_iTeamNum);
        LOAD_CLASS(CCSPlayerController, m_hPlayerPawn);
        LOAD_CLASS(C_CSPlayerPawn, m_iIDEntIndex);
        LOAD_CLASS(C_CSPlayerPawn, m_entitySpottedState);
        LOAD_CLASS(EntitySpottedState_t, m_bSpotted);
        LOAD_CLASS(EntitySpottedState_t, m_bSpottedByMask);
        LOAD_CLASS(C_CSPlayerPawnBase, m_flFlashDuration);
        LOAD_CLASS(C_CSPlayerPawnBase, m_flFlashMaxAlpha);
        LOAD_CLASS(CEntityInstance, m_pEntity);
        LOAD_CLASS(CEntityIdentity, m_designerName);
        LOAD_CLASS(C_BaseModelEntity, m_clrRender);
        LOAD_CLASS(C_BaseModelEntity, m_Glow);
        LOAD_CLASS(CGlowProperty, m_bGlowing);
        LOAD_CLASS(CGlowProperty, m_glowColorOverride);
        LOAD_CLASS(CGlowProperty, m_iGlowType);
        LOAD_CLASS(C_BaseEntity, m_vecAbsVelocity);
        LOAD_CLASS(C_BaseEntity, m_fFlags);
        LOAD_CLASS(C_BaseEntity, m_hGroundEntity);
        LOAD_CLASS(C_BaseEntity, m_MoveType);
        LOAD_CLASS(C_BaseEntity, m_pGameSceneNode);
        LOAD_CLASS(CGameSceneNode, m_vecAbsOrigin);
        LOAD_CLASS(C_BasePlayerPawn, m_pMovementServices);
        LOAD_CLASS(C_BasePlayerPawn, v_angle);
        LOAD_CLASS(C_CSPlayerPawn, m_bOnGroundLastTick);
        LOAD_CLASS(CPlayer_MovementServices, m_flCmdForwardMove);
        LOAD_CLASS(CPlayer_MovementServices, m_flCmdLeftMove);
        LOAD_CLASS(CPlayer_MovementServices, m_flForwardMove);
        LOAD_CLASS(CPlayer_MovementServices, m_flLeftMove);
        LOAD_CLASS(CPlayer_MovementServices, m_flMaxspeed);
        LOAD_CLASS(CCSPlayer_MovementServices, m_flStamina);
        LOAD_CLASS(CCSPlayer_MovementServices, m_vecWalkWishVel);
        LOAD_CLASS(CCSPlayer_MovementServices, m_ModernJump);
        LOAD_CLASS(CCSPlayerModernJump, m_nLastLandedTick);
        LOAD_CLASS(CCSPlayerModernJump, m_flLastLandedFrac);
        LOAD_CLASS(CCSPlayerModernJump, m_flLastLandedVelocityX);
        LOAD_CLASS(CCSPlayerModernJump, m_flLastLandedVelocityY);
        LOAD_CLASS(CCSPlayerModernJump, m_flLastLandedVelocityZ);
        LOAD_CLASS(CCSPlayer_MovementServices, m_nLastJumpTick);
        LOAD_CLASS(CCSPlayer_MovementServices, m_flLastJumpFrac);
        LOAD_CLASS(CCSPlayer_MovementServices, m_flLastJumpVelocityZ);
#undef LOAD_CLASS
        if (!missing.empty() && error) {
            *error = "Missing or changed fields:";
            for (const auto& f : missing) *error += "\n  " + f;
        }
        return missing.empty();
    }
    inline void UpdateAllOffsets() { (void)LoadOffsets(); }
}

class Memory {
    HANDLE hProcess = nullptr;
    uintptr_t clientBase = 0;
    DWORD processId = 0;
public:
    Memory() noexcept = default;
    Memory(const Memory&) = delete;
    Memory& operator=(const Memory&) = delete;
    ~Memory() { Detach(); }
    void Detach() noexcept {
        if (hProcess) CloseHandle(hProcess);
        hProcess = nullptr; clientBase = 0; processId = 0;
    }
    DWORD GetProcessID() const noexcept { return processId; }
    HANDLE GetHandle() const noexcept { return hProcess; }
    uintptr_t GetClientBase() const noexcept { return clientBase; }
    bool IsAlive() const noexcept {
        DWORD code = 0;
        return hProcess && GetExitCodeProcess(hProcess, &code) && code == STILL_ACTIVE;
    }
    bool Attach(const wchar_t* processName) noexcept {
        Detach();
        PROCESSENTRY32W entry{}; entry.dwSize = sizeof(entry);
        HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
        if (snapshot == INVALID_HANDLE_VALUE) return false;
        if (Process32FirstW(snapshot, &entry)) {
            do {
                if (!_wcsicmp(entry.szExeFile, processName)) {
                    processId = entry.th32ProcessID;
                    hProcess = OpenProcess(PROCESS_VM_READ | PROCESS_VM_WRITE |
                        PROCESS_VM_OPERATION | PROCESS_QUERY_LIMITED_INFORMATION, FALSE, processId);
                    if (!hProcess) { // Read-only fallback for telemetry; Write() may then fail.
                        hProcess = OpenProcess(PROCESS_VM_READ | PROCESS_QUERY_LIMITED_INFORMATION, FALSE, processId);
                    }
                    break;
                }
            } while (Process32NextW(snapshot, &entry));
        }
        CloseHandle(snapshot);
        if (!hProcess) { Detach(); return false; }
        MODULEENTRY32W mod{}; mod.dwSize = sizeof(mod);
        snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, processId);
        if (snapshot == INVALID_HANDLE_VALUE) { Detach(); return false; }
        if (Module32FirstW(snapshot, &mod)) {
            do {
                if (!_wcsicmp(mod.szModule, L"client.dll")) {
                    clientBase = reinterpret_cast<uintptr_t>(mod.modBaseAddr);
                    break;
                }
            } while (Module32NextW(snapshot, &mod));
        }
        CloseHandle(snapshot);
        if (!clientBase) { Detach(); return false; }
        return true;
    }
    template <typename T> bool TryRead(uintptr_t address, T& out) const noexcept {
        out = {};
        if (!hProcess || !address) return false;
        SIZE_T n = 0;
        return ReadProcessMemory(hProcess, reinterpret_cast<LPCVOID>(address), &out,
            sizeof(T), &n) && n == sizeof(T);
    }
    template <typename T> T Read(uintptr_t address) const noexcept {
        T out{}; (void)TryRead(address, out); return out;
    }
    template <typename T> bool Write(uintptr_t address, const T& value) const noexcept {
        if (!hProcess || !address) return false;
        SIZE_T written = 0;
        return WriteProcessMemory(hProcess, reinterpret_cast<LPVOID>(address),
            &value, sizeof(T), &written) && written == sizeof(T);
    }
    std::string ReadString(uintptr_t address, size_t size = 128) const noexcept {
        char buf[256]{};
        if (!hProcess || !address) return {};
        SIZE_T n = 0;
        SIZE_T length = static_cast<SIZE_T>(size < 255 ? size : 255);
        if (!ReadProcessMemory(hProcess, reinterpret_cast<LPCVOID>(address), buf, length, &n)) return {};
        buf[(n < 256) ? n : 255] = 0;
        return buf;
    }
};
