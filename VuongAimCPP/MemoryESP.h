#pragma once
#include <windows.h>
#include <TlHelp32.h>
#include <cstdint>
#include <string>
#include <atomic>
#include <vector>
#include <chrono>
#include <thread>
#include <iostream>
#include <fstream>
#include <sstream>
#include <regex>

static_assert(sizeof(void*) == 8, "VUI LONG CHUYEN CAU HINH VISUAL STUDIO SANG x64.");

extern std::atomic<bool> g_FpsBoostEnabled;

struct Vector2 { float x, y; };
struct Vector3 { float x, y, z; };
struct Matrix4x4 { float m[16]; };

namespace Offsets {
    inline std::ptrdiff_t dwEntityList = 0x27151A8;
    inline std::ptrdiff_t dwLocalPlayerPawn = 0x25606D8;
    inline std::ptrdiff_t dwViewMatrix = 0x2565A20;
    inline std::ptrdiff_t dwGameEntitySystem_highestEntityIndex = 0x2120;
    inline std::ptrdiff_t dwGameRules = 0x255C8D8;

    inline std::ptrdiff_t m_iHealth = 0x34C;
    inline std::ptrdiff_t m_iTeamNum = 0x3E7;
    inline std::ptrdiff_t m_hPlayerPawn = 0x92C;
    inline std::ptrdiff_t m_iIDEntIndex = 0x36CC;
    inline std::ptrdiff_t m_entitySpottedState = 0x1E88;
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

    constexpr uintptr_t EntityListFirstChunkOffset = 0x10;
    constexpr uintptr_t EntityChunkPointerStride = 0x8;
    constexpr uintptr_t EntityIdentityStride = 0x70;
    constexpr uintptr_t EntityHandleMask = 0x7FFF;
    constexpr uintptr_t EntityHandleEntryMask = 0x1FF;
    constexpr uintptr_t EntityHandleChunkShift = 9;

    inline bool ReadFileContent(const std::wstring& filename, std::string& outContent) {
        wchar_t exePath[MAX_PATH] = { 0 };
        GetModuleFileNameW(NULL, exePath, MAX_PATH);
        std::wstring pathStr(exePath);
        size_t lastSlash = pathStr.find_last_of(L"\\/");
        std::wstring dir = (lastSlash != std::wstring::npos) ? pathStr.substr(0, lastSlash + 1) : L"";

        std::ifstream file(dir + filename);
        if (!file.is_open()) return false;
        outContent.assign((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
        file.close();
        return true;
    }

    inline void ParseOffset(const std::string& content, const std::string& varName, std::ptrdiff_t& target) {
        std::regex pattern(varName + R"(\s*=\s*(0x[0-9a-fA-F]+))");
        std::smatch match;
        if (std::regex_search(content, match, pattern)) {
            target = std::stoull(match[1].str(), nullptr, 16);
        }
    }

    inline bool LoadOffsets() {
        std::string offsetsContent;
        if (ReadFileContent(L"offsets.hpp", offsetsContent) || ReadFileContent(L"offset.hpp", offsetsContent)) {
            ParseOffset(offsetsContent, "dwEntityList", dwEntityList);
            ParseOffset(offsetsContent, "dwLocalPlayerPawn", dwLocalPlayerPawn);
            ParseOffset(offsetsContent, "dwViewMatrix", dwViewMatrix);
            ParseOffset(offsetsContent, "dwGameEntitySystem_highestEntityIndex", dwGameEntitySystem_highestEntityIndex);
            ParseOffset(offsetsContent, "dwGameRules", dwGameRules);
        }

        std::string clientContent;
        if (ReadFileContent(L"client_dll.hpp", clientContent) || ReadFileContent(L"client.dll.hpp", clientContent)) {
            ParseOffset(clientContent, "m_iHealth", m_iHealth);
            ParseOffset(clientContent, "m_iTeamNum", m_iTeamNum);
            ParseOffset(clientContent, "m_hPlayerPawn", m_hPlayerPawn);
            ParseOffset(clientContent, "m_iIDEntIndex", m_iIDEntIndex);
            ParseOffset(clientContent, "m_entitySpottedState", m_entitySpottedState);
            ParseOffset(clientContent, "m_bSpotted", m_bSpotted);
            ParseOffset(clientContent, "m_bSpottedByMask", m_bSpottedByMask);
            ParseOffset(clientContent, "m_flFlashDuration", m_flFlashDuration);
            ParseOffset(clientContent, "m_flFlashMaxAlpha", m_flFlashMaxAlpha);
            ParseOffset(clientContent, "m_clrRender", m_clrRender);
            ParseOffset(clientContent, "m_Glow", m_Glow);
            ParseOffset(clientContent, "m_bGlowing", m_bGlowing);
            ParseOffset(clientContent, "m_glowColorOverride", m_glowColorOverride);
            ParseOffset(clientContent, "m_iGlowType", m_iGlowType);
        }

        return true;
    }

    inline void UpdateAllOffsets() {
        LoadOffsets();
    }
}

class Memory {
private:
    HANDLE hProcess = nullptr;
    uintptr_t clientBase = 0;
    DWORD processId = 0;

public:
    Memory() noexcept = default;

    ~Memory() {
        if (hProcess) CloseHandle(hProcess);
    }

    DWORD GetProcessID() const noexcept { return processId; }
    HANDLE GetHandle() const noexcept { return hProcess; }
    uintptr_t GetClientBase() const noexcept { return clientBase; }

    bool Attach(const wchar_t* processName) noexcept {
        PROCESSENTRY32W entry{};
        entry.dwSize = sizeof(PROCESSENTRY32W);

        HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
        if (Process32FirstW(snapshot, &entry)) {
            do {
                if (!_wcsicmp(entry.szExeFile, processName)) {
                    processId = entry.th32ProcessID;
                    hProcess = OpenProcess(PROCESS_ALL_ACCESS, FALSE, processId);
                    break;
                }
            } while (Process32NextW(snapshot, &entry));
        }
        CloseHandle(snapshot);

        if (!hProcess) return false;

        MODULEENTRY32W modEntry{};
        modEntry.dwSize = sizeof(MODULEENTRY32W);
        snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, processId);

        if (Module32FirstW(snapshot, &modEntry)) {
            do {
                if (!_wcsicmp(modEntry.szModule, L"client.dll")) {
                    clientBase = reinterpret_cast<uintptr_t>(modEntry.modBaseAddr);
                    break;
                }
            } while (Module32NextW(snapshot, &modEntry));
        }
        CloseHandle(snapshot);

        return clientBase != 0;
    }

    template <typename T>
    T Read(uintptr_t address) const noexcept {
        T buffer{};
        ReadProcessMemory(hProcess, reinterpret_cast<LPCVOID>(address), &buffer, sizeof(T), nullptr);
        return buffer;
    }

    template <typename T>
    bool Write(uintptr_t address, const T& value) const noexcept {
        return WriteProcessMemory(hProcess, reinterpret_cast<LPVOID>(address), &value, sizeof(T), nullptr) != 0;
    }

    std::string ReadString(uintptr_t address, size_t size = 128) const noexcept {
        char buffer[256] = { 0 };
        size_t readSize = size < 256 ? size : 255;
        if (ReadProcessMemory(hProcess, reinterpret_cast<LPCVOID>(address), buffer, readSize, nullptr)) {
            return std::string(buffer);
        }
        return "";
    }
};