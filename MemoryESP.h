#pragma once
#include <windows.h>
#include <TlHelp32.h>
#include <cstdint>
#include <string>
#include <atomic>

static_assert(sizeof(void*) == 8, "VUI LONG CHUYEN CAU HINH VISUAL STUDIO SANG x64.");

extern std::atomic<bool> g_FpsBoostEnabled;
struct Vector2 { float x, y; };
struct Vector3 { float x, y, z; };
struct Matrix4x4 { float m[16]; };

namespace Offsets {
    constexpr std::ptrdiff_t dwEntityList = 0x24AF268;
    constexpr std::ptrdiff_t dwLocalPlayerPawn = 0x2069B50;
    constexpr std::ptrdiff_t dwGameEntitySystem_highestEntityIndex = 0x20A0;

    constexpr std::ptrdiff_t m_iHealth = 0x354;
    constexpr std::ptrdiff_t m_iTeamNum = 0x3F3;
    constexpr std::ptrdiff_t m_hPlayerPawn = 0x90C;
    constexpr std::ptrdiff_t m_iIDEntIndex = 0x3EAC;

    // RADAR OFFSETS
    constexpr std::ptrdiff_t m_entitySpottedState = 0x26E0;
    constexpr std::ptrdiff_t m_bSpotted = 0x8;
    constexpr std::ptrdiff_t m_bSpottedByMask = 0xC; // Bắt buộc phải có cho CS2

    // FLASH & ENV OFFSETS
    constexpr std::ptrdiff_t m_flFlashDuration = 0x15F8;
    constexpr std::ptrdiff_t m_flFlashMaxAlpha = 0x15F4; // Hack Flash an toàn nhất
    constexpr std::ptrdiff_t m_bActiveVolumetricFog = 0x654;
    constexpr std::ptrdiff_t m_bPostProcessMaster = 0xF94;
    constexpr std::ptrdiff_t m_pEntity = 0x10;
    constexpr std::ptrdiff_t m_designerName = 0x20;

    constexpr uintptr_t EntityListFirstChunkOffset = 0x10;
    constexpr uintptr_t EntityChunkPointerStride = 0x8;
    constexpr uintptr_t EntityIdentityStride = 0x70;
    constexpr uintptr_t EntityHandleMask = 0x7FFF;
    constexpr uintptr_t EntityHandleEntryMask = 0x1FF;
    constexpr uintptr_t EntityHandleChunkShift = 9;
}

class Memory {
private:
    HANDLE hProcess = nullptr;
    uintptr_t clientBase = 0;
    DWORD processId = 0;
public:
    Memory() noexcept = default;
    ~Memory() { if (hProcess) CloseHandle(hProcess); }

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

    template <typename T> T Read(uintptr_t address) const noexcept {
        T buffer{};
        ReadProcessMemory(hProcess, reinterpret_cast<LPCVOID>(address), &buffer, sizeof(T), nullptr);
        return buffer;
    }

    template <typename T> bool Write(uintptr_t address, const T& value) const noexcept {
        // Đã xóa VirtualProtectEx để tránh làm treo phân vùng bộ nhớ của game
        return WriteProcessMemory(hProcess, reinterpret_cast<LPVOID>(address), &value, sizeof(T), nullptr) != 0;
    }

    std::string ReadString(uintptr_t address, size_t size = 128) const noexcept {
        char buffer[256] = { 0 };
        size_t readSize = size < 256 ? size : 255;
        if (ReadProcessMemory(hProcess, reinterpret_cast<LPCVOID>(address), buffer, readSize, nullptr)) {
            buffer[readSize] = '\0';
            return std::string(buffer);
        }
        return "";
    }
};