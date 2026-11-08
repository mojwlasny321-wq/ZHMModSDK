#include "BetterInstinct.h"

#include <Windows.h>
#include <mmsystem.h>
#include <commdlg.h>
#include <objbase.h>
#include <algorithm>
#include <atomic>
#include <cctype>
#include <cfloat>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cwctype>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <thread>
#include <vector>

#include <imgui.h>

#include "Functions.h"
#include "Hooks.h"

#include "Glacier/ZActor.h"
#include "Glacier/ZCameraEntity.h"
#include "Glacier/ZCollision.h"
#include "Glacier/ZEntity.h"
#include "Glacier/ZGameLoopManager.h"
#include "Glacier/ZHitman5.h"
#include "Glacier/ZObject.h"
#include "Glacier/ZSpatialEntity.h"
#include "Glacier/ZGameTime.h"
#include "Glacier/ZInput.h"

#pragma comment(lib, "comdlg32.lib")
#pragma comment(lib, "ole32.lib")

static constexpr char kSection[] = "BetterInstinct";
static constexpr float kUnlockLevel = 0.25f; // after running dry, the meter must refill to this before Instinct works again
static constexpr float kMinStart = 0.10f;    // Instinct can't be started below this level
static constexpr float kLowLevel = 0.25f;    // below this the meter turns to the "low" color

namespace {

namespace fs = std::filesystem;

// ---------------------------------------------------------------------------
// String helpers
// ---------------------------------------------------------------------------

std::wstring Utf8ToWide(const std::string& p_Str) {
    if (p_Str.empty()) return L"";
    const int s_Len = MultiByteToWideChar(CP_UTF8, 0, p_Str.data(), static_cast<int>(p_Str.size()), nullptr, 0);
    std::wstring s_Out(static_cast<size_t>(s_Len), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, p_Str.data(), static_cast<int>(p_Str.size()), s_Out.data(), s_Len);
    return s_Out;
}

std::string WideToUtf8(const std::wstring& p_Str) {
    if (p_Str.empty()) return "";
    const int s_Len = WideCharToMultiByte(CP_UTF8, 0, p_Str.data(), static_cast<int>(p_Str.size()), nullptr, 0, nullptr, nullptr);
    std::string s_Out(static_cast<size_t>(s_Len), '\0');
    WideCharToMultiByte(CP_UTF8, 0, p_Str.data(), static_cast<int>(p_Str.size()), s_Out.data(), s_Len, nullptr, nullptr);
    return s_Out;
}

std::wstring FindModuleDir() {
    HMODULE s_Module = nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       reinterpret_cast<LPCWSTR>(&FindModuleDir), &s_Module);
    wchar_t s_Path[MAX_PATH] = {};
    GetModuleFileNameW(s_Module, s_Path, MAX_PATH);
    std::wstring s_Dir = s_Path;
    const size_t s_Slash = s_Dir.find_last_of(L"\\/");
    return s_Dir.substr(0, s_Slash == std::wstring::npos ? 0 : s_Slash + 1);
}

// ---------------------------------------------------------------------------
// Instinct key guard: a low-level keyboard/mouse hook that swallows the Instinct key press while
// Instinct is unusable, and tracks the physical key state (swallowed presses included).
// Best effort: it can only stop input that goes through normal Windows input.
// The game-level block (ZInputAction_Digital detour, further below) is the main blocker.
// ---------------------------------------------------------------------------

std::atomic<bool> g_Block{false};
std::atomic<int> g_Key{0x11};
std::atomic<bool> g_Phys[256];
std::atomic<bool> g_HookOk{false};
std::atomic<bool> g_HookReady{false};
std::atomic<DWORD> g_HookThreadId{0};
DWORD g_Pid = 0;
std::atomic<uint32_t> g_KbEvents{0};
std::atomic<uint32_t> g_MouseEvents{0};
std::atomic<int> g_LastVk{0};
constexpr ULONG_PTR kOurMarker = 0x464F4341; // tags the key events we inject ourselves, so the hook can ignore them

std::atomic<bool> g_HipAssist{false};     // Instinct is active: left clicks are held until they lasted long enough in GAME time
std::atomic<int> g_HipGameMs{200};        // how long a left click has to last, measured in game time (milliseconds)
std::atomic<float> g_HipTimeScale{1.0f};  // current game time scale (updated every frame)
std::atomic<float> g_HipRemain{0.0f};     // game-time milliseconds the pending click still has to be held
std::atomic<ULONGLONG> g_LDownTick{0};
std::atomic<UINT_PTR> g_HipTimer{0};      // pending "left button release" (safety timer id, 0 = none)
std::atomic<bool> g_PsSwallowLmb{false};  // point shooting: left click belongs to us, the game never sees it
std::atomic<bool> g_PsLmbSwallowed{false}; // the current left press was swallowed, so its release must be too
std::atomic<uint32_t> g_PsClicks{0};       // number of left presses swallowed (click-to-tag)
constexpr int kHipMaxRealMs = 1500;       // a held-over click never lasts longer than this in real time

void SendKey(int p_Vk, bool p_Up);

// Ends a held-over left click: stops the timer and (optionally) sends the release the game is still waiting for.
void CancelHipTimer(bool p_SendUp) {
    const UINT_PTR s_Id = g_HipTimer.exchange(0);
    if (s_Id != 0) {
        KillTimer(nullptr, s_Id);
        if (p_SendUp) SendKey(VK_LBUTTON, true);
    }
}

// Sends the pending release from the game thread (the safety timer cleans itself up when it fires).
void ReleaseHipClick() {
    if (g_HipTimer.exchange(0) != 0) {
        SendKey(VK_LBUTTON, true);
    }
}

VOID CALLBACK HipTimerProc(HWND, UINT, UINT_PTR p_Id, DWORD) {
    if (g_HipTimer.load() == p_Id) {
        CancelHipTimer(true);
    } else {
        KillTimer(nullptr, p_Id);
    }
}

bool MatchesKey(int p_Configured, int p_Vk) {
    if (p_Configured == p_Vk) return true;
    switch (p_Configured) {
        case VK_SHIFT: return p_Vk == VK_LSHIFT || p_Vk == VK_RSHIFT;
        case VK_CONTROL: return p_Vk == VK_LCONTROL || p_Vk == VK_RCONTROL;
        case VK_MENU: return p_Vk == VK_LMENU || p_Vk == VK_RMENU;
        default: return false;
    }
}

std::vector<int> KeyVariants(int p_Key) {
    switch (p_Key) {
        case VK_SHIFT: return {VK_LSHIFT, VK_RSHIFT};
        case VK_CONTROL: return {VK_LCONTROL, VK_RCONTROL};
        case VK_MENU: return {VK_LMENU, VK_RMENU};
        default: return {p_Key};
    }
}

bool GameIsForeground() {
    DWORD s_Pid = 0;
    GetWindowThreadProcessId(GetForegroundWindow(), &s_Pid);
    return s_Pid == g_Pid;
}

LRESULT CALLBACK KeyboardProc(int p_Code, WPARAM p_W, LPARAM p_L) {
    if (p_Code == HC_ACTION) {
        const auto* s_Kb = reinterpret_cast<const KBDLLHOOKSTRUCT*>(p_L);
        if (s_Kb->dwExtraInfo != kOurMarker) { // everything except what we injected ourselves (remapper/driver keys count)
            const bool s_Down = p_W == WM_KEYDOWN || p_W == WM_SYSKEYDOWN;
            const bool s_Up = p_W == WM_KEYUP || p_W == WM_SYSKEYUP;
            if (s_Down || s_Up) {
                ++g_KbEvents;
                g_LastVk = static_cast<int>(s_Kb->vkCode);
                g_Phys[s_Kb->vkCode & 0xFF] = s_Down;
                if (s_Down && g_Block.load() && MatchesKey(g_Key.load(), static_cast<int>(s_Kb->vkCode)) && GameIsForeground()) {
                    return 1; // swallow the press: the game never sees it
                }
            }
        }
    }
    return CallNextHookEx(nullptr, p_Code, p_W, p_L);
}

LRESULT CALLBACK MouseProc(int p_Code, WPARAM p_W, LPARAM p_L) {
    if (p_Code == HC_ACTION && p_W != WM_MOUSEMOVE) {
        const auto* s_Ms = reinterpret_cast<const MSLLHOOKSTRUCT*>(p_L);
        if (s_Ms->dwExtraInfo != kOurMarker) {
            int s_Vk = 0;
            bool s_Down = false;
            switch (p_W) {
                case WM_LBUTTONDOWN: s_Vk = VK_LBUTTON; s_Down = true; break;
                case WM_LBUTTONUP: s_Vk = VK_LBUTTON; break;
                case WM_RBUTTONDOWN: s_Vk = VK_RBUTTON; s_Down = true; break;
                case WM_RBUTTONUP: s_Vk = VK_RBUTTON; break;
                case WM_MBUTTONDOWN: s_Vk = VK_MBUTTON; s_Down = true; break;
                case WM_MBUTTONUP: s_Vk = VK_MBUTTON; break;
                case WM_XBUTTONDOWN:
                case WM_XBUTTONUP:
                    s_Vk = HIWORD(s_Ms->mouseData) == XBUTTON1 ? VK_XBUTTON1 : VK_XBUTTON2;
                    s_Down = p_W == WM_XBUTTONDOWN;
                    break;
                default: break;
            }

            if (s_Vk != 0) {
                ++g_MouseEvents;
                g_Phys[s_Vk] = s_Down;

                if (s_Vk == VK_LBUTTON) {
                    // Point shooting: left click only tags targets, it must not fire the weapon.
                    if (s_Down) {
                        if (g_PsSwallowLmb.load() && GameIsForeground()) {
                            g_PsLmbSwallowed = true;
                            ++g_PsClicks;
                            return 1;
                        }
                    } else if (g_PsLmbSwallowed.exchange(false)) {
                        return 1; // never leave a swallowed press half-finished
                    }

                    if (s_Down) {
                        CancelHipTimer(true); // new click while a held-over release is pending: finish the old one first
                        g_LDownTick = GetTickCount64();
                    } else if (g_HipAssist.load() && GameIsForeground() && !g_Phys[VK_RBUTTON].load()) {
                        // Hip fire during Instinct. Time runs slowed, but a click is measured in real time, so a normal
                        // click is over before the game has even raised the weapon. Keep the button held until it has
                        // lasted the configured length in GAME time (the frame update sends the release).
                        const float s_Scale = std::clamp(g_HipTimeScale.load(), 0.05f, 1.0f);
                        const float s_HeldGame = static_cast<float>(GetTickCount64() - g_LDownTick.load()) * s_Scale;
                        const float s_Need = static_cast<float>(std::max(0, g_HipGameMs.load()));
                        if (s_HeldGame < s_Need) {
                            const float s_RemainGame = s_Need - s_HeldGame;
                            const float s_RemainReal = std::min(s_RemainGame / s_Scale, static_cast<float>(kHipMaxRealMs));
                            // The timer is only a safety net (frames stop e.g. when a menu opens); the frame update releases on time.
                            const UINT s_Safety = static_cast<UINT>(s_RemainReal + 250.0f);
                            const UINT_PTR s_Id = SetTimer(nullptr, 0, s_Safety, HipTimerProc);
                            if (s_Id != 0) {
                                g_HipRemain = s_RemainGame;
                                g_HipTimer = s_Id;
                                return 1; // swallow the release; the frame update (or the safety timer) sends it later
                            }
                        }
                    }
                }

                if (s_Down && g_Block.load() && g_Key.load() == s_Vk && GameIsForeground()) {
                    return 1;
                }
            }
        }
    }
    return CallNextHookEx(nullptr, p_Code, p_W, p_L);
}

// Runs on its own thread: installs the hooks and pumps messages until it gets WM_QUIT.
// The thread holds its own reference to this DLL and ends with FreeLibraryAndExitThread, so unloading the mod never
// has to wait for it (waiting for a thread while the mod unloads is what could freeze the game).
DWORD WINAPI HookThreadProc(LPVOID p_Param) {
    HMODULE s_Module = static_cast<HMODULE>(p_Param);

    MSG s_Msg;
    PeekMessageW(&s_Msg, nullptr, WM_USER, WM_USER, PM_NOREMOVE); // make sure this thread has a message queue
    const DWORD s_MyId = GetCurrentThreadId();
    g_HookThreadId = s_MyId;

    HHOOK s_Kb = SetWindowsHookExW(WH_KEYBOARD_LL, KeyboardProc, s_Module, 0);
    HHOOK s_Ms = SetWindowsHookExW(WH_MOUSE_LL, MouseProc, s_Module, 0);
    g_HookOk = s_Kb != nullptr && s_Ms != nullptr;
    g_HookReady = true;

    while (GetMessageW(&s_Msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&s_Msg);
        DispatchMessageW(&s_Msg);
    }

    CancelHipTimer(true); // never leave the left button stuck down
    if (s_Kb) UnhookWindowsHookEx(s_Kb);
    if (s_Ms) UnhookWindowsHookEx(s_Ms);

    if (g_HookThreadId.load() == s_MyId) {
        g_HookOk = false;
        g_HookReady = false;
        g_HookThreadId = 0;
    }

    FreeLibraryAndExitThread(s_Module, 0); // drops our DLL reference and ends the thread without returning into DLL code
    return 0;
}

class InputGuard {
public:
    InputGuard() {
        g_Pid = GetCurrentProcessId();
        for (auto& s_Key : g_Phys) s_Key = false;

        // A hook thread from a previous load of the mod may still be finishing: give it a moment.
        for (int i = 0; i < 100 && g_HookThreadId.load() != 0; ++i) Sleep(5);
        g_HookReady = false;
        g_HookOk = false;

        HMODULE s_Module = nullptr; // (no UNCHANGED_REFCOUNT flag: this takes a reference for the thread)
        if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS, reinterpret_cast<LPCWSTR>(&HookThreadProc), &s_Module) ||
            s_Module == nullptr) {
            return;
        }

        HANDLE s_Thread = CreateThread(nullptr, 0, HookThreadProc, s_Module, 0, nullptr);
        if (s_Thread == nullptr) {
            FreeLibrary(s_Module);
            return;
        }
        CloseHandle(s_Thread);

        for (int i = 0; i < 200 && !g_HookReady.load(); ++i) Sleep(5); // wait (max ~1s) for the hooks to be installed
    }

    ~InputGuard() {
        // Only ask the thread to stop; it unhooks and exits on its own. Never wait for it here.
        g_Block = false;
        const DWORD s_Id = g_HookThreadId.load();
        if (g_HookReady.load() && s_Id != 0) {
            PostThreadMessageW(s_Id, WM_QUIT, 0, 0);
        }
    }
};

std::unique_ptr<InputGuard> g_Guard;

bool AsyncKeyDown(int p_Key) {
    for (int s_Vk : KeyVariants(p_Key)) {
        if ((GetAsyncKeyState(s_Vk) & 0x8000) != 0) return true;
    }
    return false;
}

bool HookKeyDown(int p_Key) {
    if (!g_HookOk.load()) return false;
    for (int s_Vk : KeyVariants(p_Key)) {
        if (g_Phys[s_Vk & 0xFF].load()) return true;
    }
    return false;
}

// Down if either Windows' own key state or the hook says so (the hook also sees keys we swallowed).
bool PhysKeyDown(int p_Key) {
    return AsyncKeyDown(p_Key) || HookKeyDown(p_Key);
}

void SendKey(int p_Vk, bool p_Up) {
    INPUT s_In = {};
    switch (p_Vk) {
        case VK_LBUTTON: case VK_RBUTTON: case VK_MBUTTON: case VK_XBUTTON1: case VK_XBUTTON2:
            s_In.type = INPUT_MOUSE;
            if (p_Vk == VK_LBUTTON) s_In.mi.dwFlags = p_Up ? MOUSEEVENTF_LEFTUP : MOUSEEVENTF_LEFTDOWN;
            else if (p_Vk == VK_RBUTTON) s_In.mi.dwFlags = p_Up ? MOUSEEVENTF_RIGHTUP : MOUSEEVENTF_RIGHTDOWN;
            else if (p_Vk == VK_MBUTTON) s_In.mi.dwFlags = p_Up ? MOUSEEVENTF_MIDDLEUP : MOUSEEVENTF_MIDDLEDOWN;
            else {
                s_In.mi.dwFlags = p_Up ? MOUSEEVENTF_XUP : MOUSEEVENTF_XDOWN;
                s_In.mi.mouseData = p_Vk == VK_XBUTTON1 ? XBUTTON1 : XBUTTON2;
            }
            break;
        default: {
            s_In.type = INPUT_KEYBOARD;
            s_In.ki.wVk = static_cast<WORD>(p_Vk);
            s_In.ki.wScan = static_cast<WORD>(MapVirtualKeyW(static_cast<UINT>(p_Vk), MAPVK_VK_TO_VSC));
            DWORD s_Flags = p_Up ? KEYEVENTF_KEYUP : 0;
            switch (p_Vk) { // extended keys
                case VK_RCONTROL: case VK_RMENU: case VK_LEFT: case VK_RIGHT: case VK_UP: case VK_DOWN:
                case VK_PRIOR: case VK_NEXT: case VK_END: case VK_HOME: case VK_INSERT: case VK_DELETE:
                    s_Flags |= KEYEVENTF_EXTENDEDKEY;
                    break;
                default: break;
            }
            s_In.ki.dwFlags = s_Flags;
            break;
        }
    }
    if (s_In.type == INPUT_MOUSE) s_In.mi.dwExtraInfo = kOurMarker;
    else s_In.ki.dwExtraInfo = kOurMarker;
    SendInput(1, &s_In, sizeof(INPUT));
}

// ---------------------------------------------------------------------------
// "Browse..." file dialog for the custom Instinct sound. It runs on its own thread, so the game keeps rendering
// while the dialog is open. Like the input-hook thread it holds its own reference to this DLL.
// ---------------------------------------------------------------------------

std::atomic<bool> g_BrowseBusy{false};   // a dialog is currently open
std::atomic<bool> g_BrowseReady{false};  // the player picked a file (g_BrowseResult)
std::mutex g_BrowseMutex;
std::wstring g_BrowseResult;
std::wstring g_BrowseStartDir;
HWND g_BrowseOwner = nullptr;

DWORD WINAPI BrowseThreadProc(LPVOID p_Param) {
    HMODULE s_Module = static_cast<HMODULE>(p_Param);
    const HRESULT s_Com = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);

    std::wstring s_Dir;
    {
        std::lock_guard<std::mutex> s_Lock(g_BrowseMutex);
        s_Dir = g_BrowseStartDir;
    }

    wchar_t s_File[MAX_PATH * 2] = {};
    OPENFILENAMEW s_Ofn = {};
    s_Ofn.lStructSize = sizeof(s_Ofn);
    s_Ofn.hwndOwner = g_BrowseOwner;
    s_Ofn.lpstrFilter = L"Sound files (*.wav;*.mp3)\0*.wav;*.mp3\0All files (*.*)\0*.*\0\0";
    s_Ofn.nFilterIndex = 1;
    s_Ofn.lpstrFile = s_File;
    s_Ofn.nMaxFile = static_cast<DWORD>(sizeof(s_File) / sizeof(s_File[0]));
    s_Ofn.lpstrInitialDir = s_Dir.empty() ? nullptr : s_Dir.c_str();
    s_Ofn.lpstrTitle = L"Choose an Instinct sound";
    s_Ofn.Flags = OFN_EXPLORER | OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_HIDEREADONLY | OFN_NOCHANGEDIR;

    if (GetOpenFileNameW(&s_Ofn)) {
        std::lock_guard<std::mutex> s_Lock(g_BrowseMutex);
        g_BrowseResult = s_File;
        g_BrowseReady = true;
    }

    if (SUCCEEDED(s_Com)) CoUninitialize();
    g_BrowseBusy = false;
    FreeLibraryAndExitThread(s_Module, 0);
    return 0;
}

void StartBrowse(const std::wstring& p_StartDir) {
    if (g_BrowseBusy.exchange(true)) return; // already open

    {
        std::lock_guard<std::mutex> s_Lock(g_BrowseMutex);
        g_BrowseStartDir = p_StartDir;
    }
    g_BrowseOwner = GetForegroundWindow(); // keeps the dialog on top of the game window

    HMODULE s_Module = nullptr; // (no UNCHANGED_REFCOUNT flag: this takes a reference for the thread)
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS, reinterpret_cast<LPCWSTR>(&BrowseThreadProc), &s_Module);
    HANDLE s_Thread = s_Module ? CreateThread(nullptr, 0, BrowseThreadProc, s_Module, 0, nullptr) : nullptr;
    if (s_Thread) {
        CloseHandle(s_Thread);
    } else {
        if (s_Module) FreeLibrary(s_Module);
        g_BrowseBusy = false;
    }
}

// ---------------------------------------------------------------------------
// Built-in sound presets (written to the sound folder on first run, if missing).
// ---------------------------------------------------------------------------

constexpr uint32_t kRate = 22050;
constexpr float kPi = 3.14159265358979f;
using Samples = std::vector<float>;

bool WriteWav(const fs::path& p_Path, const Samples& p_Samples, float p_Gain) {
    const uint32_t n = static_cast<uint32_t>(p_Samples.size());
    const uint32_t s_Bytes = n * 2;
    std::vector<uint8_t> s_Wav(44 + s_Bytes);
    auto* p = s_Wav.data();
    auto w32 = [&](size_t o, uint32_t v) { std::memcpy(p + o, &v, 4); };
    auto w16 = [&](size_t o, uint16_t v) { std::memcpy(p + o, &v, 2); };

    std::memcpy(p + 0, "RIFF", 4);
    w32(4, 36 + s_Bytes);
    std::memcpy(p + 8, "WAVEfmt ", 8);
    w32(16, 16);
    w16(20, 1);
    w16(22, 1);
    w32(24, kRate);
    w32(28, kRate * 2);
    w16(32, 2);
    w16(34, 16);
    std::memcpy(p + 36, "data", 4);
    w32(40, s_Bytes);

    for (uint32_t i = 0; i < n; ++i) {
        const int16_t s_Pcm = static_cast<int16_t>(std::clamp(p_Samples[i] * p_Gain, -1.0f, 1.0f) * 32767.0f);
        std::memcpy(p + 44 + i * 2, &s_Pcm, 2);
    }

    std::ofstream s_File(p_Path, std::ios::binary);
    if (!s_File) return false;
    s_File.write(reinterpret_cast<const char*>(s_Wav.data()), static_cast<std::streamsize>(s_Wav.size()));
    return true;
}

Samples PresetLowHum() { // 2.0 s, loops seamlessly (whole number of cycles)
    const uint32_t n = kRate * 2;
    Samples s(n);
    for (uint32_t i = 0; i < n; ++i) {
        const float t = static_cast<float>(i) / kRate;
        const float v = 0.5f * std::sin(2 * kPi * 55.0f * t) + 0.3f * std::sin(2 * kPi * 82.5f * t) + 0.2f * std::sin(2 * kPi * 110.0f * t);
        s[i] = v * (0.75f + 0.25f * std::sin(2 * kPi * 0.5f * t));
    }
    return s;
}

Samples PresetHeartbeat() { // 1.0 s loop
    const uint32_t n = kRate;
    Samples s(n, 0.0f);
    auto thump = [&](float p_Start, float p_Amp) {
        for (uint32_t i = 0; i < n; ++i) {
            const float t = static_cast<float>(i) / kRate - p_Start;
            if (t < 0.0f) continue;
            const float env = std::exp(-t * 16.0f) * std::min(1.0f, t / 0.004f);
            s[i] += p_Amp * env * (std::sin(2 * kPi * 55.0f * t) + 0.5f * std::sin(2 * kPi * 110.0f * t));
        }
    };
    thump(0.0f, 1.0f);
    thump(0.22f, 0.75f);
    return s;
}

Samples PresetPulseSweep() { // 1.5 s loop, fades to silence at both ends
    const uint32_t n = static_cast<uint32_t>(kRate * 1.5f);
    Samples s(n);
    double s_Phase = 0.0;
    for (uint32_t i = 0; i < n; ++i) {
        const float t = static_cast<float>(i) / n;
        s_Phase += 2.0 * kPi * (220.0f + 380.0f * t) / kRate;
        const float env = std::sin(kPi * t);
        s[i] = static_cast<float>(std::sin(s_Phase)) * env * env * 0.8f;
    }
    return s;
}

// Case-insensitive "does p_Hay contain p_NeedleLower" (p_NeedleLower must already be lowercase). No allocations.
bool ContainsNoCase(const char* p_Hay, const std::string& p_NeedleLower) {
    if (p_NeedleLower.empty()) return false;
    const size_t s_HayLen = std::strlen(p_Hay);
    const size_t s_NeedleLen = p_NeedleLower.size();
    if (s_NeedleLen > s_HayLen) return false;
    for (size_t i = 0; i + s_NeedleLen <= s_HayLen; ++i) {
        size_t j = 0;
        while (j < s_NeedleLen && std::tolower(static_cast<unsigned char>(p_Hay[i + j])) == static_cast<unsigned char>(p_NeedleLower[j])) ++j;
        if (j == s_NeedleLen) return true;
    }
    return false;
}

// Stable, always-writable place for our own settings file (independent of where the DLL is loaded from).
std::wstring ConfigDir() {
    wchar_t s_Buf[MAX_PATH] = {};
    const DWORD s_Len = GetEnvironmentVariableW(L"APPDATA", s_Buf, MAX_PATH);
    if (s_Len == 0 || s_Len >= MAX_PATH) return L"";
    return std::wstring(s_Buf) + L"\\BetterInstinct\\";
}

std::string OneLine(std::string p_Str) { // values are stored one per line
    p_Str.erase(std::remove(p_Str.begin(), p_Str.end(), '\r'), p_Str.end());
    p_Str.erase(std::remove(p_Str.begin(), p_Str.end(), '\n'), p_Str.end());
    return p_Str;
}

} // namespace

// ---------------------------------------------------------------------------
// Lifecycle
// ---------------------------------------------------------------------------

BetterInstinct::~BetterInstinct() {
    if (m_ConfigReady) {
        SaveConfigFile();
    }

    m_InputBlock = false;
    m_CaptureNames = false;
    g_HipAssist = false;
    g_PsSwallowLmb = false;
    ReleaseHipClick();

    const ZMemberDelegate<BetterInstinct, void(const SGameUpdateEvent&)> s_Delegate(this, &BetterInstinct::OnFrameUpdate);
    Globals::GameLoopManager->UnregisterFrameUpdate(s_Delegate, 1, EUpdateMode::eUpdatePlayMode);

    RestoreTint();
    CloseSound();
    g_Guard.reset(); // removes the input hooks

    if (m_Touched && Globals::GameTimeManager) {
        Globals::GameTimeManager->m_fGameTimeMultiplier = 1.0f;
    }
}

void BetterInstinct::Init() {
    // Lets us answer "not pressed" to the game when it asks about the Instinct action.
    Hooks::ZInputAction_Digital->AddDetour(this, &BetterInstinct::ZInputAction_Digital);
    Hooks::ZHM5ItemWeapon_IsFiring->AddDetour(this, &BetterInstinct::ZHM5ItemWeapon_IsFiring);
}

void BetterInstinct::OnEngineInitialized() {
    LoadSettings();
    RebuildPatterns(); // default patterns (LoadSoundText below may replace them)

    // Register the game-loop update first, so nothing below can stop the core feature from running.
    const ZMemberDelegate<BetterInstinct, void(const SGameUpdateEvent&)> s_Delegate(this, &BetterInstinct::OnFrameUpdate);
    Globals::GameLoopManager->RegisterFrameUpdate(s_Delegate, 1, EUpdateMode::eUpdatePlayMode);

    try {
        m_ModDir = FindModuleDir();
        m_SoundDir = m_ModDir + L"InstinctSounds\\";
        std::error_code s_Ec;
        fs::create_directories(fs::path(m_SoundDir), s_Ec);
        EnsurePresets();
        LoadSoundText();
        ScanSounds();
    } catch (...) {
        m_SoundStatus = "Couldn't prepare the sound folder.";
    }

    // Our own settings file wins over everything loaded above (first run: it gets created from the current values).
    bool s_ConfigFound = false;
    try {
        s_ConfigFound = LoadConfigFile();
        if (s_ConfigFound) ScanSounds(); // re-select the saved sound
    } catch (...) {
    }
    SaveConfigFile(); // also rewrites an older config file with every current option
    m_ConfigReady = true;

    try {
        g_Key = m_InstinctKey;
        g_Guard = std::make_unique<InputGuard>();
    } catch (...) {
        g_HookOk = false;
    }
}

// ---------------------------------------------------------------------------
// Game-level input blocking
// ---------------------------------------------------------------------------

void BetterInstinct::RebuildPatterns() {
    std::vector<std::string> s_New;
    std::string s_Cur;

    auto s_Flush = [&]() {
        const size_t s_A = s_Cur.find_first_not_of(" \t");
        if (s_A != std::string::npos) {
            const size_t s_B = s_Cur.find_last_not_of(" \t");
            std::string s_Item = s_Cur.substr(s_A, s_B - s_A + 1);
            std::transform(s_Item.begin(), s_Item.end(), s_Item.begin(),
                           [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
            s_New.push_back(std::move(s_Item));
        }
        s_Cur.clear();
    };

    for (const char* p = m_ActionPatterns; *p; ++p) {
        if (*p == ',') s_Flush();
        else s_Cur += *p;
    }
    s_Flush();

    std::lock_guard<std::mutex> s_Lock(m_NameMutex);
    m_Patterns = std::move(s_New);
}

bool BetterInstinct::ActionMatches(const char* p_Name) {
    std::lock_guard<std::mutex> s_Lock(m_NameMutex);
    for (const auto& s_Pattern : m_Patterns) {
        if (ContainsNoCase(p_Name, s_Pattern)) return true;
    }
    return false;
}

// The game polls this for every input action, many times per frame, possibly from several threads.
DEFINE_PLUGIN_DETOUR(BetterInstinct, bool, ZInputAction_Digital, ZInputAction* th, int32_t controllerId) {
    const bool s_Block = m_InputBlock.load();
    const bool s_Capture = m_CaptureNames.load();

    if (!th || (!s_Block && !s_Capture)) {
        return HookResult<bool>(HookAction::Continue());
    }

    const char* s_Name = th->m_szName;
    if (!s_Name) {
        return HookResult<bool>(HookAction::Continue());
    }

    if (s_Capture) {
        std::lock_guard<std::mutex> s_Lock(m_NameMutex);
        if (m_SeenNames.size() < 1000) {
            m_SeenNames.insert(s_Name);
        }
    }

    if (s_Block && ActionMatches(s_Name)) {
        ++m_BlockedPolls;
        return HookResult(HookAction::Return(), false);
    }

    return HookResult<bool>(HookAction::Continue());
}

// ---------------------------------------------------------------------------
// Settings
// ---------------------------------------------------------------------------

// The game asks every weapon "are you firing?" all the time. Whenever it asks about Agent 47's own weapon we note
// which weapon that is and when, so the mod knows what he is holding.
DEFINE_PLUGIN_DETOUR(BetterInstinct, bool, ZHM5ItemWeapon_IsFiring, IFirearm* th) {
    if (!m_PsEnabled || !th) {
        return HookResult<bool>(HookAction::Continue());
    }

    const auto s_LocalHitman = SDK()->GetLocalPlayer();
    if (!s_LocalHitman) {
        return HookResult<bool>(HookAction::Continue());
    }

    ZHM5ItemWeapon* s_Weapon = static_cast<ZHM5ItemWeapon*>(th);
    if (s_Weapon->m_pOwner != s_LocalHitman.m_entityRef) {
        return HookResult<bool>(HookAction::Continue());
    }

    m_HeldWeapon.store(s_Weapon);
    m_HeldWeaponTick.store(std::chrono::steady_clock::now().time_since_epoch().count());
    ++m_WeaponPolls;

    return HookResult<bool>(HookAction::Continue());
}

void BetterInstinct::LoadSettings() {
    m_InstinctKey = static_cast<int>(GetSettingInt(kSection, "instinct_key", m_InstinctKey));
    m_ToggleMode = GetSettingBool(kSection, "toggle_mode", m_ToggleMode);
    m_BlockEnabled = GetSettingBool(kSection, "block_enabled", m_BlockEnabled);
    m_FocusScale = std::clamp(static_cast<float>(GetSettingDouble(kSection, "focus_scale", m_FocusScale)), 0.05f, 0.95f);
    m_BlendSpeed = std::clamp(static_cast<float>(GetSettingDouble(kSection, "blend_speed", m_BlendSpeed)), 1.0f, 30.0f);
    m_DrainSeconds = std::clamp(static_cast<float>(GetSettingDouble(kSection, "drain_seconds", m_DrainSeconds)), 2.0f, 120.0f);
    m_RechargeSeconds = std::clamp(static_cast<float>(GetSettingDouble(kSection, "recharge_seconds", m_RechargeSeconds)), 2.0f, 300.0f);
    m_RechargeDelay = std::clamp(static_cast<float>(GetSettingDouble(kSection, "recharge_delay", m_RechargeDelay)), 0.0f, 10.0f);
    m_HipAssist = GetSettingBool(kSection, "hip_assist", m_HipAssist);
    m_HipGameMs = std::clamp(static_cast<int>(GetSettingInt(kSection, "hip_game_ms", m_HipGameMs)), 0, 600);

    m_HudEnabled = GetSettingBool(kSection, "hud_enabled", m_HudEnabled);
    m_HudAlways = GetSettingBool(kSection, "hud_always", m_HudAlways);
    m_HudWidth = std::clamp(static_cast<float>(GetSettingDouble(kSection, "hud_width", m_HudWidth)), 0.05f, 0.5f);
    m_HudYOffset = std::clamp(static_cast<float>(GetSettingDouble(kSection, "hud_y", m_HudYOffset)), 0.0f, 600.0f);
    m_HudScale = std::clamp(static_cast<float>(GetSettingDouble(kSection, "hud_scale", m_HudScale)), 0.5f, 3.0f);
    m_HudOpacity = std::clamp(static_cast<float>(GetSettingDouble(kSection, "hud_opacity", m_HudOpacity)), 0.1f, 1.0f);
    m_FillColor[0] = std::clamp(static_cast<float>(GetSettingDouble(kSection, "fill_r", m_FillColor[0])), 0.0f, 1.0f);
    m_FillColor[1] = std::clamp(static_cast<float>(GetSettingDouble(kSection, "fill_g", m_FillColor[1])), 0.0f, 1.0f);
    m_FillColor[2] = std::clamp(static_cast<float>(GetSettingDouble(kSection, "fill_b", m_FillColor[2])), 0.0f, 1.0f);
    m_LowColor[0] = std::clamp(static_cast<float>(GetSettingDouble(kSection, "low_r", m_LowColor[0])), 0.0f, 1.0f);
    m_LowColor[1] = std::clamp(static_cast<float>(GetSettingDouble(kSection, "low_g", m_LowColor[1])), 0.0f, 1.0f);
    m_LowColor[2] = std::clamp(static_cast<float>(GetSettingDouble(kSection, "low_b", m_LowColor[2])), 0.0f, 1.0f);

    m_TintEnabled = GetSettingBool(kSection, "tint_enabled", m_TintEnabled);
    m_InstinctTint[0] = std::clamp(static_cast<float>(GetSettingDouble(kSection, "tint_r", m_InstinctTint[0])), 0.0f, 1.0f);
    m_InstinctTint[1] = std::clamp(static_cast<float>(GetSettingDouble(kSection, "tint_g", m_InstinctTint[1])), 0.0f, 1.0f);
    m_InstinctTint[2] = std::clamp(static_cast<float>(GetSettingDouble(kSection, "tint_b", m_InstinctTint[2])), 0.0f, 1.0f);
    m_TintStrength = std::clamp(static_cast<float>(GetSettingDouble(kSection, "tint_strength", m_TintStrength)), 0.0f, 1.0f);

    // Older versions saved a plain white meter colour: switch to the new gold look once.
    if (GetSettingInt(kSection, "hud_style", 0) < 2) {
        m_FillColor[0] = 1.00f;
        m_FillColor[1] = 0.78f;
        m_FillColor[2] = 0.28f;
    }

    m_SoundEnabled = GetSettingBool(kSection, "sound_enabled", m_SoundEnabled);
    m_SoundSource = std::clamp(static_cast<int>(GetSettingInt(kSection, "sound_source", m_SoundSource)), 0, 1);
    m_SoundVolume = std::clamp(static_cast<float>(GetSettingDouble(kSection, "sound_volume", m_SoundVolume)), 0.0f, 1.0f);
    m_FadeOutOn = GetSettingBool(kSection, "fade_out_on", m_FadeOutOn);
    m_FadeOutMs = std::clamp(static_cast<float>(GetSettingDouble(kSection, "fade_out_ms", m_FadeOutMs)), 0.0f, 3000.0f);
    m_FadeInOn = GetSettingBool(kSection, "fade_in_on", m_FadeInOn);
    m_FadeInMs = std::clamp(static_cast<float>(GetSettingDouble(kSection, "fade_in_ms", m_FadeInMs)), 0.0f, 3000.0f);
}

void BetterInstinct::SaveSettings() {
    SetSettingInt(kSection, "instinct_key", m_InstinctKey);
    SetSettingInt(kSection, "hud_style", 2);
    SetSettingBool(kSection, "toggle_mode", m_ToggleMode);
    SetSettingBool(kSection, "block_enabled", m_BlockEnabled);
    SetSettingDouble(kSection, "focus_scale", m_FocusScale);
    SetSettingDouble(kSection, "blend_speed", m_BlendSpeed);
    SetSettingDouble(kSection, "drain_seconds", m_DrainSeconds);
    SetSettingDouble(kSection, "recharge_seconds", m_RechargeSeconds);
    SetSettingDouble(kSection, "recharge_delay", m_RechargeDelay);
    SetSettingBool(kSection, "hip_assist", m_HipAssist);
    SetSettingInt(kSection, "hip_game_ms", m_HipGameMs);

    SetSettingBool(kSection, "hud_enabled", m_HudEnabled);
    SetSettingBool(kSection, "hud_always", m_HudAlways);
    SetSettingDouble(kSection, "hud_width", m_HudWidth);
    SetSettingDouble(kSection, "hud_y", m_HudYOffset);
    SetSettingDouble(kSection, "hud_scale", m_HudScale);
    SetSettingDouble(kSection, "hud_opacity", m_HudOpacity);
    SetSettingDouble(kSection, "fill_r", m_FillColor[0]);
    SetSettingDouble(kSection, "fill_g", m_FillColor[1]);
    SetSettingDouble(kSection, "fill_b", m_FillColor[2]);
    SetSettingDouble(kSection, "low_r", m_LowColor[0]);
    SetSettingDouble(kSection, "low_g", m_LowColor[1]);
    SetSettingDouble(kSection, "low_b", m_LowColor[2]);

    SetSettingBool(kSection, "tint_enabled", m_TintEnabled);
    SetSettingDouble(kSection, "tint_r", m_InstinctTint[0]);
    SetSettingDouble(kSection, "tint_g", m_InstinctTint[1]);
    SetSettingDouble(kSection, "tint_b", m_InstinctTint[2]);
    SetSettingDouble(kSection, "tint_strength", m_TintStrength);

    SetSettingBool(kSection, "sound_enabled", m_SoundEnabled);
    SetSettingInt(kSection, "sound_source", m_SoundSource);
    SetSettingDouble(kSection, "sound_volume", m_SoundVolume);
    SetSettingBool(kSection, "fade_out_on", m_FadeOutOn);
    SetSettingDouble(kSection, "fade_out_ms", m_FadeOutMs);
    SetSettingBool(kSection, "fade_in_on", m_FadeInOn);
    SetSettingDouble(kSection, "fade_in_ms", m_FadeInMs);

    SaveSoundText();
    SaveConfigFile();
}

// The text values (selected file name, custom path, Instinct action name(s)) live in a small text file next to the DLL.
void BetterInstinct::LoadSoundText() {
    std::ifstream s_File(fs::path(m_ModDir + L"BetterInstinct.sound.txt"));
    std::string s_Line;
    if (std::getline(s_File, s_Line)) {
        while (!s_Line.empty() && (s_Line.back() == '\r' || s_Line.back() == '\n')) s_Line.pop_back();
        m_PresetName = s_Line;
    }
    if (std::getline(s_File, s_Line)) {
        while (!s_Line.empty() && (s_Line.back() == '\r' || s_Line.back() == '\n')) s_Line.pop_back();
        std::snprintf(m_CustomPath, sizeof(m_CustomPath), "%s", s_Line.c_str());
    }
    if (std::getline(s_File, s_Line)) {
        while (!s_Line.empty() && (s_Line.back() == '\r' || s_Line.back() == '\n')) s_Line.pop_back();
        if (!s_Line.empty()) {
            std::snprintf(m_ActionPatterns, sizeof(m_ActionPatterns), "%s", s_Line.c_str());
        }
    }
    RebuildPatterns();
}

void BetterInstinct::SaveSoundText() {
    std::ofstream s_File(fs::path(m_ModDir + L"BetterInstinct.sound.txt"), std::ios::trunc);
    if (s_File) {
        s_File << m_PresetName << "\n" << m_CustomPath << "\n" << m_ActionPatterns << "\n";
    }
}

bool BetterInstinct::SaveConfigFile() {
    std::wstring s_Dir = ConfigDir();
    if (s_Dir.empty()) s_Dir = m_ModDir;
    if (s_Dir.empty()) return false;

    std::error_code s_Ec;
    fs::create_directories(fs::path(s_Dir), s_Ec);

    const fs::path s_Final = fs::path(s_Dir) / L"BetterInstinct.cfg";
    const fs::path s_Tmp = fs::path(s_Dir) / L"BetterInstinct.cfg.tmp";

    {
        std::ofstream s_File(s_Tmp, std::ios::trunc);
        if (!s_File) return false;

        auto s_Int = [&](const char* k, long long v) { s_File << k << "=" << v << "\n"; };
        auto s_Flt = [&](const char* k, float v) { s_File << k << "=" << std::to_string(v) << "\n"; };
        auto s_Bool = [&](const char* k, bool v) { s_File << k << "=" << (v ? 1 : 0) << "\n"; };
        auto s_Str = [&](const char* k, const std::string& v) { s_File << k << "=" << OneLine(v) << "\n"; };

        s_Int("instinct_key", m_InstinctKey);
        s_Bool("toggle_mode", m_ToggleMode);
        s_Bool("block_enabled", m_BlockEnabled);
        s_Flt("focus_scale", m_FocusScale);
        s_Flt("blend_speed", m_BlendSpeed);
        s_Flt("drain_seconds", m_DrainSeconds);
        s_Flt("recharge_seconds", m_RechargeSeconds);
        s_Flt("recharge_delay", m_RechargeDelay);

        s_Bool("hip_assist", m_HipAssist);
        s_Int("hip_game_ms", m_HipGameMs);

        s_Bool("ps_enabled", m_PsEnabled);
        s_Int("ps_key", m_PsKey);
        s_Int("ps_tag_mode", m_PsTagMode);
        s_Flt("ps_hover_dwell", m_PsHoverDwell);
        s_Flt("ps_scale", m_PsScale);
        s_Flt("ps_exec_scale", m_PsExecScale);
        s_Flt("ps_tag_cost", m_PsTagCost);
        s_Int("ps_max_tags", m_PsMaxTags);
        s_Flt("ps_drain_mul", m_PsDrainMul);
        s_Flt("ps_snap_radius", m_PsSnapRadius);
        s_Flt("ps_max_dist", m_PsMaxDist);
        s_Int("ps_aim_part", m_PsAimPart);
        s_Flt("ps_snap_time", m_PsSnapTime);
        s_Flt("ps_shot_delay", m_PsShotDelay);
        s_Int("ps_order", m_PsOrder);
        s_Bool("ps_block_fire", m_PsBlockFire);
        s_Bool("ps_sound", m_PsSoundOn);
        s_Flt("ps_head_height", m_PsHeadHeight);
        s_Flt("ps_torso_height", m_PsTorsoHeight);
        s_Bool("ps_y_up", m_PsYUp);
        s_Bool("ps_require_weapon", m_PsRequireWeapon);
        s_Int("ps_shot_mode", m_PsShotMode);
        s_Bool("ps_precise", m_PsPrecise);
        s_Bool("ps_miss_finish", m_PsMissFinish);
        s_Int("ps_muzzle_axis", m_PsMuzzleAxis);
        s_Bool("ps_prime", m_PsPrime);
        s_Flt("ps_weapon_window", m_PsWeaponWindow);
        s_Int("ps_origin_mode", m_PsOriginMode);
        s_Bool("ps_show_muzzle", m_PsShowMuzzle);
        s_Bool("ps_prompt", m_PsPrompt);

        s_Bool("hud_enabled", m_HudEnabled);
        s_Bool("hud_always", m_HudAlways);
        s_Flt("hud_width", m_HudWidth);
        s_Flt("hud_y", m_HudYOffset);
        s_Flt("hud_scale", m_HudScale);
        s_Flt("hud_opacity", m_HudOpacity);
        s_Flt("fill_r", m_FillColor[0]);
        s_Flt("fill_g", m_FillColor[1]);
        s_Flt("fill_b", m_FillColor[2]);
        s_Flt("low_r", m_LowColor[0]);
        s_Flt("low_g", m_LowColor[1]);
        s_Flt("low_b", m_LowColor[2]);

        s_Bool("tint_enabled", m_TintEnabled);
        s_Flt("tint_r", m_InstinctTint[0]);
        s_Flt("tint_g", m_InstinctTint[1]);
        s_Flt("tint_b", m_InstinctTint[2]);
        s_Flt("tint_strength", m_TintStrength);

        s_Bool("sound_enabled", m_SoundEnabled);
        s_Int("sound_source", m_SoundSource);
        s_Flt("sound_volume", m_SoundVolume);
        s_Bool("fade_out_on", m_FadeOutOn);
        s_Flt("fade_out_ms", m_FadeOutMs);
        s_Bool("fade_in_on", m_FadeInOn);
        s_Flt("fade_in_ms", m_FadeInMs);

        s_Str("preset_name", m_PresetName);
        s_Str("custom_path", m_CustomPath);
        s_Str("action_patterns", m_ActionPatterns);

        s_File.flush();
        if (!s_File.good()) return false;
    }

    fs::rename(s_Tmp, s_Final, s_Ec);
    if (s_Ec) {
        s_Ec.clear();
        fs::copy_file(s_Tmp, s_Final, fs::copy_options::overwrite_existing, s_Ec);
        std::error_code s_Ec2;
        fs::remove(s_Tmp, s_Ec2);
    }
    return !s_Ec;
}

bool BetterInstinct::LoadConfigFile() {
    std::wstring s_Dir = ConfigDir();
    if (s_Dir.empty()) s_Dir = m_ModDir;
    if (s_Dir.empty()) return false;

    std::ifstream s_File(fs::path(s_Dir) / L"BetterInstinct.cfg");
    if (!s_File) return false;

    std::map<std::string, std::string> s_Map;
    std::string s_Line;
    while (std::getline(s_File, s_Line)) {
        while (!s_Line.empty() && (s_Line.back() == '\r' || s_Line.back() == '\n')) s_Line.pop_back();
        const size_t s_Eq = s_Line.find('=');
        if (s_Eq == std::string::npos || s_Eq == 0) continue;
        s_Map[s_Line.substr(0, s_Eq)] = s_Line.substr(s_Eq + 1);
    }
    if (s_Map.empty()) return false;

    auto s_Find = [&](const char* k) -> const std::string* {
        const auto s_It = s_Map.find(k);
        return s_It == s_Map.end() ? nullptr : &s_It->second;
    };
    auto s_Flt = [&](const char* k, float& v, float lo, float hi) {
        if (const std::string* p = s_Find(k)) {
            char* s_End = nullptr;
            const double s_D = std::strtod(p->c_str(), &s_End);
            if (s_End != p->c_str()) v = std::clamp(static_cast<float>(s_D), lo, hi);
        }
    };
    auto s_Int = [&](const char* k, int& v, int lo, int hi) {
        if (const std::string* p = s_Find(k)) {
            char* s_End = nullptr;
            const long s_L = std::strtol(p->c_str(), &s_End, 10);
            if (s_End != p->c_str()) v = std::clamp(static_cast<int>(s_L), lo, hi);
        }
    };
    auto s_Bool = [&](const char* k, bool& v) {
        if (const std::string* p = s_Find(k)) v = (*p == "1");
    };
    auto s_Str = [&](const char* k, char* dst, size_t cap) {
        if (const std::string* p = s_Find(k)) std::snprintf(dst, cap, "%s", p->c_str());
    };

    s_Int("instinct_key", m_InstinctKey, 1, 254);
    s_Bool("toggle_mode", m_ToggleMode);
    s_Bool("block_enabled", m_BlockEnabled);
    s_Flt("focus_scale", m_FocusScale, 0.05f, 0.95f);
    s_Flt("blend_speed", m_BlendSpeed, 1.0f, 30.0f);
    s_Flt("drain_seconds", m_DrainSeconds, 2.0f, 120.0f);
    s_Flt("recharge_seconds", m_RechargeSeconds, 2.0f, 300.0f);
    s_Flt("recharge_delay", m_RechargeDelay, 0.0f, 10.0f);

    s_Bool("hip_assist", m_HipAssist);
    s_Int("hip_game_ms", m_HipGameMs, 0, 600);

    s_Bool("ps_enabled", m_PsEnabled);
    s_Int("ps_key", m_PsKey, 1, 254);
    s_Int("ps_tag_mode", m_PsTagMode, 0, 1);
    s_Flt("ps_hover_dwell", m_PsHoverDwell, 0.05f, 2.0f);
    s_Flt("ps_scale", m_PsScale, 0.02f, 0.95f);
    s_Flt("ps_exec_scale", m_PsExecScale, 0.02f, 1.0f);
    s_Flt("ps_tag_cost", m_PsTagCost, 0.0f, 0.5f);
    s_Int("ps_max_tags", m_PsMaxTags, 1, 12);
    s_Flt("ps_drain_mul", m_PsDrainMul, 0.0f, 2.0f);
    s_Flt("ps_snap_radius", m_PsSnapRadius, 0.02f, 0.30f);
    s_Flt("ps_max_dist", m_PsMaxDist, 10.0f, 200.0f);
    s_Int("ps_aim_part", m_PsAimPart, 0, 1);
    s_Flt("ps_snap_time", m_PsSnapTime, 0.02f, 0.6f);
    s_Flt("ps_shot_delay", m_PsShotDelay, 0.05f, 1.0f);
    s_Int("ps_order", m_PsOrder, 0, 1);
    s_Bool("ps_block_fire", m_PsBlockFire);
    s_Bool("ps_sound", m_PsSoundOn);
    s_Flt("ps_head_height", m_PsHeadHeight, 0.3f, 2.5f);
    s_Flt("ps_torso_height", m_PsTorsoHeight, 0.3f, 2.5f);
    s_Bool("ps_y_up", m_PsYUp);
    s_Bool("ps_require_weapon", m_PsRequireWeapon);
    s_Int("ps_shot_mode", m_PsShotMode, 0, 1);
    s_Bool("ps_precise", m_PsPrecise);
    s_Bool("ps_miss_finish", m_PsMissFinish);
    s_Int("ps_muzzle_axis", m_PsMuzzleAxis, 0, 5);
    s_Bool("ps_prime", m_PsPrime);
    s_Flt("ps_weapon_window", m_PsWeaponWindow, 0.2f, 5.0f);
    s_Int("ps_origin_mode", m_PsOriginMode, 0, 2);
    s_Bool("ps_show_muzzle", m_PsShowMuzzle);
    s_Bool("ps_prompt", m_PsPrompt);

    s_Bool("hud_enabled", m_HudEnabled);
    s_Bool("hud_always", m_HudAlways);
    s_Flt("hud_width", m_HudWidth, 0.05f, 0.5f);
    s_Flt("hud_y", m_HudYOffset, 0.0f, 600.0f);
    s_Flt("hud_scale", m_HudScale, 0.5f, 3.0f);
    s_Flt("hud_opacity", m_HudOpacity, 0.1f, 1.0f);
    s_Flt("fill_r", m_FillColor[0], 0.0f, 1.0f);
    s_Flt("fill_g", m_FillColor[1], 0.0f, 1.0f);
    s_Flt("fill_b", m_FillColor[2], 0.0f, 1.0f);
    s_Flt("low_r", m_LowColor[0], 0.0f, 1.0f);
    s_Flt("low_g", m_LowColor[1], 0.0f, 1.0f);
    s_Flt("low_b", m_LowColor[2], 0.0f, 1.0f);

    s_Bool("tint_enabled", m_TintEnabled);
    s_Flt("tint_r", m_InstinctTint[0], 0.0f, 1.0f);
    s_Flt("tint_g", m_InstinctTint[1], 0.0f, 1.0f);
    s_Flt("tint_b", m_InstinctTint[2], 0.0f, 1.0f);
    s_Flt("tint_strength", m_TintStrength, 0.0f, 1.0f);

    s_Bool("sound_enabled", m_SoundEnabled);
    s_Int("sound_source", m_SoundSource, 0, 1);
    s_Flt("sound_volume", m_SoundVolume, 0.0f, 1.0f);
    s_Bool("fade_out_on", m_FadeOutOn);
    s_Flt("fade_out_ms", m_FadeOutMs, 0.0f, 3000.0f);
    s_Bool("fade_in_on", m_FadeInOn);
    s_Flt("fade_in_ms", m_FadeInMs, 0.0f, 3000.0f);

    if (const std::string* p = s_Find("preset_name")) m_PresetName = *p;
    s_Str("custom_path", m_CustomPath, sizeof(m_CustomPath));
    s_Str("action_patterns", m_ActionPatterns, sizeof(m_ActionPatterns));
    if (m_ActionPatterns[0] == '\0') {
        std::snprintf(m_ActionPatterns, sizeof(m_ActionPatterns), "%s", "instinct,focus");
    }
    RebuildPatterns();

    g_Key = m_InstinctKey;
    return true;
}

// ---------------------------------------------------------------------------
// Sound (Windows MCI: wav and mp3, looping, volume fades)
// ---------------------------------------------------------------------------

void BetterInstinct::EnsurePresets() {
    struct Preset { const wchar_t* Name; Samples (*Make)(); };
    const Preset s_Presets[] = {
        {L"Preset - Low Hum.wav", PresetLowHum},
        {L"Preset - Heartbeat.wav", PresetHeartbeat},
        {L"Preset - Pulse Sweep.wav", PresetPulseSweep},
    };

    for (const auto& s_Preset : s_Presets) {
        const fs::path s_Path = fs::path(m_SoundDir) / s_Preset.Name;
        std::error_code s_Ec;
        if (!fs::exists(s_Path, s_Ec)) {
            WriteWav(s_Path, s_Preset.Make(), 0.6f);
        }
    }
}

void BetterInstinct::ScanSounds() {
    m_PresetFiles.clear();
    std::error_code s_Ec;
    for (const auto& s_Entry : fs::directory_iterator(fs::path(m_SoundDir), s_Ec)) {
        std::error_code s_Ec2;
        if (!s_Entry.is_regular_file(s_Ec2)) continue;
        std::wstring s_Ext = s_Entry.path().extension().wstring();
        std::transform(s_Ext.begin(), s_Ext.end(), s_Ext.begin(), [](wchar_t c) { return static_cast<wchar_t>(std::towlower(c)); });
        if (s_Ext == L".wav" || s_Ext == L".mp3") {
            m_PresetFiles.push_back(WideToUtf8(s_Entry.path().filename().wstring()));
        }
    }
    std::sort(m_PresetFiles.begin(), m_PresetFiles.end());

    m_PresetIndex = -1;
    for (size_t i = 0; i < m_PresetFiles.size(); ++i) {
        if (m_PresetFiles[i] == m_PresetName) m_PresetIndex = static_cast<int>(i);
    }
    if (m_PresetIndex < 0 && !m_PresetFiles.empty()) {
        m_PresetIndex = 0;
        m_PresetName = m_PresetFiles[0];
    }
}

std::wstring BetterInstinct::CurrentSoundPath() const {
    if (m_SoundSource == 1) {
        return Utf8ToWide(m_CustomPath);
    }
    if (m_PresetIndex >= 0 && m_PresetIndex < static_cast<int>(m_PresetFiles.size())) {
        return m_SoundDir + Utf8ToWide(m_PresetFiles[m_PresetIndex]);
    }
    return L"";
}

bool BetterInstinct::OpenSound() {
    const std::wstring s_Path = CurrentSoundPath();
    std::error_code s_Ec;
    if (s_Path.empty() || !fs::exists(fs::path(s_Path), s_Ec)) {
        m_SoundStatus = "Sound file not found.";
        return false;
    }

    mciSendStringW(L"close instinctsnd", nullptr, 0, nullptr);
    const std::wstring s_Cmd = L"open \"" + s_Path + L"\" type mpegvideo alias instinctsnd";
    if (mciSendStringW(s_Cmd.c_str(), nullptr, 0, nullptr) != 0) {
        m_SoundStatus = "Couldn't open that file (use .wav or .mp3).";
        return false;
    }

    m_SoundStatus.clear();
    m_SndOpen = true;
    return true;
}

void BetterInstinct::CloseSound() {
    if (m_SndOpen) {
        mciSendStringW(L"stop instinctsnd", nullptr, 0, nullptr);
        mciSendStringW(L"close instinctsnd", nullptr, 0, nullptr);
    }
    m_SndOpen = false;
    m_SndPlaying = false;
    m_SndDir = 0;
    m_SndGain = 0.0f;
}

void BetterInstinct::ApplySoundVolume() {
    if (!m_SndOpen) return;
    const int s_Vol = std::clamp(static_cast<int>(m_SndGain * m_SoundVolume * 1000.0f), 0, 1000);
    const std::wstring s_Cmd = L"setaudio instinctsnd volume to " + std::to_wstring(s_Vol);
    mciSendStringW(s_Cmd.c_str(), nullptr, 0, nullptr);
}

void BetterInstinct::StartSound() {
    if (!m_SoundEnabled) return;

    const bool s_FadeIn = m_FadeInOn && m_FadeInMs > 0.0f;

    if (!m_SndPlaying) {
        if (!m_SndOpen && !OpenSound()) return;
        m_SndGain = s_FadeIn ? 0.0f : 1.0f;
        ApplySoundVolume();
        mciSendStringW(L"play instinctsnd repeat", nullptr, 0, nullptr);
        m_SndPlaying = true;
        m_SndDir = s_FadeIn ? 1 : 0;
    } else if (s_FadeIn) {
        m_SndDir = 1; // was fading out: fade back in from where it is
    } else {
        m_SndGain = 1.0f;
        m_SndDir = 0;
        ApplySoundVolume();
    }
}

void BetterInstinct::StopSound() {
    if (!m_SndPlaying) return;

    if (m_FadeOutOn && m_FadeOutMs > 0.0f) {
        m_SndDir = -1;
    } else {
        CloseSound();
    }
}

// Runs every frame from the UI callback, so sound also stops cleanly when you leave the mission.
void BetterInstinct::UpdateSound(float p_Dt, bool p_InPlay) {
    const bool s_Wanted = m_SoundEnabled && (((m_Active.load() || m_PsState.load() != PS_IDLE) && p_InPlay) || m_Testing);

    if (s_Wanted && !m_SoundWasWanted) {
        StartSound();
    } else if (!s_Wanted && m_SoundWasWanted) {
        StopSound();
    }
    m_SoundWasWanted = s_Wanted;

    if (m_SndPlaying && m_SndDir != 0) {
        if (m_SndDir > 0) {
            m_SndGain += p_Dt / std::max(m_FadeInMs * 0.001f, 0.001f);
            if (m_SndGain >= 1.0f) { m_SndGain = 1.0f; m_SndDir = 0; }
            ApplySoundVolume();
        } else {
            m_SndGain -= p_Dt / std::max(m_FadeOutMs * 0.001f, 0.001f);
            if (m_SndGain <= 0.0f) {
                CloseSound();
            } else {
                ApplySoundVolume();
            }
        }
    }
}

// ---------------------------------------------------------------------------
// Input helpers
// ---------------------------------------------------------------------------

bool BetterInstinct::IsKeyDown(int p_Vk) {
    return p_Vk != 0 && (GetAsyncKeyState(p_Vk) & 0x8000) != 0;
}

bool BetterInstinct::InstinctKeyDown() const {
    return PhysKeyDown(m_InstinctKey);
}

// Makes the game think the Instinct key was released (or, in toggle mode, taps it to switch Instinct off).
void BetterInstinct::ForceReleaseInstinct() {
    const auto s_Variants = KeyVariants(m_InstinctKey);
    if (m_ToggleMode) {
        SendKey(s_Variants[0], false);
        SendKey(s_Variants[0], true);
        return;
    }

    for (int s_Vk : s_Variants) {
        if (PhysKeyDown(s_Vk)) SendKey(s_Vk, true);
    }
}

std::string BetterInstinct::KeyName(int p_Vk) {
    switch (p_Vk) {
        case VK_LBUTTON: return "Left Mouse";
        case VK_RBUTTON: return "Right Mouse";
        case VK_MBUTTON: return "Middle Mouse";
        case VK_XBUTTON1: return "Mouse 4";
        case VK_XBUTTON2: return "Mouse 5";
        case VK_SHIFT: return "Shift (either)";
        case VK_CONTROL: return "Ctrl (either)";
        case VK_MENU: return "Alt (either)";
        default: break;
    }

    UINT s_Scan = MapVirtualKeyW(static_cast<UINT>(p_Vk), MAPVK_VK_TO_VSC);
    switch (p_Vk) { // keys that need the "extended" bit to get the right name
        case VK_LEFT: case VK_RIGHT: case VK_UP: case VK_DOWN:
        case VK_PRIOR: case VK_NEXT: case VK_END: case VK_HOME:
        case VK_INSERT: case VK_DELETE: case VK_DIVIDE: case VK_NUMLOCK:
        case VK_RCONTROL: case VK_RMENU:
            s_Scan |= 0x100;
            break;
        default: break;
    }

    wchar_t s_Name[64] = {};
    if (s_Scan != 0 && GetKeyNameTextW(static_cast<LONG>(s_Scan << 16), s_Name, 64) > 0) {
        char s_Utf8[128] = {};
        WideCharToMultiByte(CP_UTF8, 0, s_Name, -1, s_Utf8, sizeof(s_Utf8), nullptr, nullptr);
        return s_Utf8;
    }

    char s_Fallback[16];
    std::snprintf(s_Fallback, sizeof(s_Fallback), "Key 0x%02X", p_Vk);
    return s_Fallback;
}

// ---------------------------------------------------------------------------
// Point shooting (Absolution / Dead Eye style)
// ---------------------------------------------------------------------------

namespace {

struct PsCandidate {
    TEntityRef<ZActor> m_Ref;
    SVector3 m_Aim;
    float m_ScreenDist = 0.0f;
    float m_WorldDist = 0.0f;
};

// World position of the game camera.
bool PsGetCameraPos(float* p_Out) {
    if (!Functions::GetCurrentCamera) {
        return false;
    }
    const auto s_Camera = Functions::GetCurrentCamera->Call();
    if (!s_Camera) {
        return false;
    }
    const SMatrix s_Matrix = s_Camera->GetObjectToWorldMatrix();
    p_Out[0] = s_Matrix.Trans.x;
    p_Out[1] = s_Matrix.Trans.y;
    p_Out[2] = s_Matrix.Trans.z;
    return true;
}

// World position of Agent 47 (used to look through his own body and to sort targets by distance).
bool PsGetPlayerPos(float* p_Out) {
    if (auto s_LocalHitman = SDK()->GetLocalPlayer()) {
        if (auto* s_Spatial = s_LocalHitman.m_entityRef.QueryInterface<ZSpatialEntity>()) {
            const SMatrix s_Matrix = s_Spatial->GetObjectToWorldMatrix();
            p_Out[0] = s_Matrix.Trans.x;
            p_Out[1] = s_Matrix.Trans.y;
            p_Out[2] = s_Matrix.Trans.z;
            return true;
        }
    }
    return false;
}

// Direction the camera looks in (cameras look along -Z).
bool PsGetCameraDir(float* p_Out) {
    if (!Functions::GetCurrentCamera) {
        return false;
    }
    const auto s_Camera = Functions::GetCurrentCamera->Call();
    if (!s_Camera) {
        return false;
    }
    const SMatrix s_Matrix = s_Camera->GetObjectToWorldMatrix();
    float s_X = -s_Matrix.ZAxis.x;
    float s_Y = -s_Matrix.ZAxis.y;
    float s_Z = -s_Matrix.ZAxis.z;
    const float s_Len = std::sqrt(s_X * s_X + s_Y * s_Y + s_Z * s_Z);
    if (s_Len < 0.0001f) {
        return false;
    }
    p_Out[0] = s_X / s_Len;
    p_Out[1] = s_Y / s_Len;
    p_Out[2] = s_Z / s_Len;
    return true;
}

// Which body part a point on an enemy belongs to, from its position in his own axes (x right, z up).
int PsClassifyPart(const float* p_Local) {
    const float s_Z = p_Local[2];
    if (s_Z >= 1.50f) {
        return 0; // head
    }
    if (s_Z >= 0.95f) {
        return std::fabs(p_Local[0]) >= 0.22f ? 2 : 1; // arm : torso
    }
    return 3; // leg
}

const char* PsPartName(int p_Part) {
    switch (p_Part) {
        case 0: return "HEAD";
        case 2: return "ARM";
        case 3: return "LEG";
        default: return "BODY";
    }
}

// World position of a point stored in an enemy's own axes (so it follows him as he moves and turns).
bool PsAimFromLocal(const TEntityRef<ZActor>& p_Actor, const float* p_Local, SVector3& p_Out) {
    const ZEntityRef s_Entity = p_Actor.m_entityRef;
    auto* s_Spatial = s_Entity.QueryInterface<ZSpatialEntity>();
    if (!s_Spatial) {
        return false;
    }
    const SMatrix s_M = s_Spatial->GetObjectToWorldMatrix();
    p_Out = SVector3(s_M.Trans.x + s_M.XAxis.x * p_Local[0] + s_M.YAxis.x * p_Local[1] + s_M.ZAxis.x * p_Local[2],
                     s_M.Trans.y + s_M.XAxis.y * p_Local[0] + s_M.YAxis.y * p_Local[1] + s_M.ZAxis.y * p_Local[2],
                     s_M.Trans.z + s_M.XAxis.z * p_Local[0] + s_M.YAxis.z * p_Local[1] + s_M.ZAxis.z * p_Local[2]);
    return true;
}

// A transform at p_Trans whose chosen local axis points along (fx, fy, fz). Engine convention: X = right,
// Y = backward (forward is -Y), Z = up. p_Axis picks which local axis should point out of the barrel:
// 0 = -Y (engine forward), 1 = +Y, 2 = -Z, 3 = +Z, 4 = +X, 5 = -X.
SMatrix PsBuildAimMatrix(const float4& p_Trans, float p_Fx, float p_Fy, float p_Fz, int p_Axis) {
    float s_Ux = 0.0f, s_Uy = 0.0f, s_Uz = 1.0f;
    if (std::fabs(p_Fz) > 0.98f) {
        s_Uy = 1.0f;
        s_Uz = 0.0f;
    }

    // right = normalize(cross(up, forward))
    float s_Rx = s_Uy * p_Fz - s_Uz * p_Fy;
    float s_Ry = s_Uz * p_Fx - s_Ux * p_Fz;
    float s_Rz = s_Ux * p_Fy - s_Uy * p_Fx;
    float s_Rl = std::sqrt(s_Rx * s_Rx + s_Ry * s_Ry + s_Rz * s_Rz);
    if (s_Rl < 0.0001f) {
        s_Rx = 1.0f;
        s_Ry = 0.0f;
        s_Rz = 0.0f;
        s_Rl = 1.0f;
    }
    s_Rx /= s_Rl;
    s_Ry /= s_Rl;
    s_Rz /= s_Rl;

    // up = cross(forward, right)
    const float s_Vx = p_Fy * s_Rz - p_Fz * s_Ry;
    const float s_Vy = p_Fz * s_Rx - p_Fx * s_Rz;
    const float s_Vz = p_Fx * s_Ry - p_Fy * s_Rx;

    const float4 s_R(s_Rx, s_Ry, s_Rz, 0.0f);
    const float4 s_NegR(-s_Rx, -s_Ry, -s_Rz, 0.0f);
    const float4 s_F(p_Fx, p_Fy, p_Fz, 0.0f);
    const float4 s_NegF(-p_Fx, -p_Fy, -p_Fz, 0.0f);
    const float4 s_U(s_Vx, s_Vy, s_Vz, 0.0f);
    const float4 s_NegU(-s_Vx, -s_Vy, -s_Vz, 0.0f);

    switch (p_Axis) {
        case 1: return SMatrix(s_NegR, s_F, s_U, p_Trans);     // +Y
        case 2: return SMatrix(s_R, s_NegU, s_NegF, p_Trans);  // -Z
        case 3: return SMatrix(s_R, s_U, s_F, p_Trans);        // +Z
        case 4: return SMatrix(s_F, s_R, s_U, p_Trans);        // +X
        case 5: return SMatrix(s_NegF, s_R, s_NegU, p_Trans);  // -X
        default: return SMatrix(s_R, s_NegF, s_U, p_Trans);    // -Y
    }
}

} // namespace

// Aim point of an actor: his origin (the feet) plus a head or torso height.
bool BetterInstinct::PsAimPoint(const TEntityRef<ZActor>& p_Actor, SVector3& p_Out) {
    const ZEntityRef s_Entity = p_Actor.m_entityRef;
    auto* s_Spatial = s_Entity.QueryInterface<ZSpatialEntity>();
    if (!s_Spatial) {
        return false;
    }

    const SMatrix s_Matrix = s_Spatial->GetObjectToWorldMatrix();
    const float s_Height = m_PsAimPart == 0 ? m_PsHeadHeight : m_PsTorsoHeight;
    p_Out = SVector3(s_Matrix.Trans.x, s_Matrix.Trans.y, s_Matrix.Trans.z);
    if (m_PsYUp) {
        p_Out.y += s_Height;
    } else {
        p_Out.z += s_Height;
    }
    return true;
}

// Can the camera see the aim point? Walls and other people in between make it invalid.
bool BetterInstinct::PsLineOfSight(const float* p_Cam, const SVector3& p_To, ZActor* p_Actor) {
    if (!Globals::CollisionManager || !*Globals::CollisionManager) {
        return true; // can't check: don't block tagging
    }

    float s_PlayerPos[3] = {};
    const bool s_HavePlayer = PsGetPlayerPos(s_PlayerPos);

    float s_From[3] = {p_Cam[0], p_Cam[1], p_Cam[2]};

    for (int s_Try = 0; s_Try < 3; ++s_Try) {
        const float s_Dx = p_To.x - s_From[0];
        const float s_Dy = p_To.y - s_From[1];
        const float s_Dz = p_To.z - s_From[2];
        const float s_Len = std::sqrt(s_Dx * s_Dx + s_Dy * s_Dy + s_Dz * s_Dz);
        if (s_Len < 0.5f) {
            return true;
        }
        const float s_Inv = 1.0f / s_Len;

        ZRayQueryInput s_Input {
            .m_vFrom = float4(s_From[0], s_From[1], s_From[2], 1.0f),
            .m_vTo = float4(p_To.x, p_To.y, p_To.z, 1.0f),
        };
        ZRayQueryOutput s_Output {};

        if (!(*Globals::CollisionManager)->RayCastClosestHit(s_Input, &s_Output)) {
            return true; // nothing in the way
        }

        if (s_Output.m_pBlockingSpatialEntity.m_pInterfaceRef) {
            ZEntityRef s_Parent = s_Output.m_pBlockingSpatialEntity.m_entityRef.GetLogicalParent();
            if (ZActor* s_HitActor = s_Parent.QueryInterface<ZActor>()) {
                return s_HitActor == p_Actor; // the target itself is fine, somebody else in front is not
            }
        }

        // Close enough to the aim point: the ray reached the target (or the ground right next to it).
        const float s_Hx = s_Output.m_vPosition.x - p_To.x;
        const float s_Hy = s_Output.m_vPosition.y - p_To.y;
        const float s_Hz = s_Output.m_vPosition.z - p_To.z;
        if (s_Hx * s_Hx + s_Hy * s_Hy + s_Hz * s_Hz < 0.6f * 0.6f) {
            return true;
        }

        // Hit Agent 47's own body (third person camera): look through it and try again.
        if (s_HavePlayer) {
            const float s_Px = s_Output.m_vPosition.x - s_PlayerPos[0];
            const float s_Py = s_Output.m_vPosition.y - s_PlayerPos[1];
            const float s_Pz = s_Output.m_vPosition.z - s_PlayerPos[2];
            if (s_Px * s_Px + s_Py * s_Py + s_Pz * s_Pz < 4.0f) {
                s_From[0] = s_Output.m_vPosition.x + s_Dx * s_Inv * 0.3f;
                s_From[1] = s_Output.m_vPosition.y + s_Dy * s_Inv * 0.3f;
                s_From[2] = s_Output.m_vPosition.z + s_Dz * s_Inv * 0.3f;
                continue;
            }
        }

        return false;
    }

    return false;
}

// The weapon Agent 47 holds: the one the game polled for him very recently.
ZHM5ItemWeapon* BetterInstinct::GetHeldWeapon() const {
    ZHM5ItemWeapon* s_Weapon = m_HeldWeapon.load();
    if (!s_Weapon) {
        return nullptr;
    }

    const int64_t s_Now = std::chrono::steady_clock::now().time_since_epoch().count();
    const auto s_AgeMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                             std::chrono::steady_clock::duration(s_Now - m_HeldWeaponTick.load())).count();
    if (s_AgeMs > static_cast<long long>(m_PsWeaponWindow * 1000.0f)) {
        return nullptr;
    }

    const auto s_LocalHitman = SDK()->GetLocalPlayer();
    if (!s_LocalHitman || s_Weapon->m_pOwner != s_LocalHitman.m_entityRef) {
        return nullptr;
    }
    return s_Weapon;
}

bool BetterInstinct::HasWeaponInHand() const {
    return GetHeldWeapon() != nullptr;
}

// Finds the exact point on an enemy that the crosshair is resting on (so a leg stays a leg and a head stays a head).
bool BetterInstinct::PsRefineAim(const float* p_Cam, const float* p_Dir, const TEntityRef<ZActor>& p_Ref, float* p_OutLocal,
                                 SVector3& p_OutAim) {
    if (!Globals::CollisionManager || !*Globals::CollisionManager) {
        return false;
    }

    ZActor* s_Actor = p_Ref.m_pInterfaceRef;
    const ZEntityRef s_Entity = p_Ref.m_entityRef;
    auto* s_Spatial = s_Entity.QueryInterface<ZSpatialEntity>();
    if (!s_Actor || !s_Spatial) {
        return false;
    }

    float s_PlayerPos[3] = {};
    const bool s_HavePlayer = PsGetPlayerPos(s_PlayerPos);

    float s_From[3] = {p_Cam[0], p_Cam[1], p_Cam[2]};
    const float s_Range = m_PsMaxDist + 5.0f;

    for (int s_Try = 0; s_Try < 3; ++s_Try) {
        ZRayQueryInput s_Input {
            .m_vFrom = float4(s_From[0], s_From[1], s_From[2], 1.0f),
            .m_vTo = float4(s_From[0] + p_Dir[0] * s_Range, s_From[1] + p_Dir[1] * s_Range, s_From[2] + p_Dir[2] * s_Range, 1.0f),
        };
        ZRayQueryOutput s_Output {};

        if (!(*Globals::CollisionManager)->RayCastClosestHit(s_Input, &s_Output)) {
            return false;
        }

        if (s_Output.m_pBlockingSpatialEntity.m_pInterfaceRef) {
            ZEntityRef s_Parent = s_Output.m_pBlockingSpatialEntity.m_entityRef.GetLogicalParent();
            if (s_Parent.QueryInterface<ZActor>() == s_Actor) {
                const SMatrix s_M = s_Spatial->GetObjectToWorldMatrix();
                const float s_Hx = s_Output.m_vPosition.x - s_M.Trans.x;
                const float s_Hy = s_Output.m_vPosition.y - s_M.Trans.y;
                const float s_Hz = s_Output.m_vPosition.z - s_M.Trans.z;

                p_OutLocal[0] = s_Hx * s_M.XAxis.x + s_Hy * s_M.XAxis.y + s_Hz * s_M.XAxis.z;
                p_OutLocal[1] = s_Hx * s_M.YAxis.x + s_Hy * s_M.YAxis.y + s_Hz * s_M.YAxis.z;
                p_OutLocal[2] = s_Hx * s_M.ZAxis.x + s_Hy * s_M.ZAxis.y + s_Hz * s_M.ZAxis.z;

                // Sanity: the point has to be somewhere on a person.
                if (p_OutLocal[2] < -0.3f || p_OutLocal[2] > 2.4f || std::fabs(p_OutLocal[0]) > 1.5f ||
                    std::fabs(p_OutLocal[1]) > 1.5f) {
                    return false;
                }

                p_OutAim = SVector3(s_Output.m_vPosition.x, s_Output.m_vPosition.y, s_Output.m_vPosition.z);
                return true;
            }
        }

        // Hit Agent 47's own body (third person camera): look through it and try again.
        if (s_HavePlayer) {
            const float s_Px = s_Output.m_vPosition.x - s_PlayerPos[0];
            const float s_Py = s_Output.m_vPosition.y - s_PlayerPos[1];
            const float s_Pz = s_Output.m_vPosition.z - s_PlayerPos[2];
            if (s_Px * s_Px + s_Py * s_Py + s_Pz * s_Pz < 4.0f) {
                s_From[0] = s_Output.m_vPosition.x + p_Dir[0] * 0.3f;
                s_From[1] = s_Output.m_vPosition.y + p_Dir[1] * 0.3f;
                s_From[2] = s_Output.m_vPosition.z + p_Dir[2] * 0.3f;
                continue;
            }
        }

        return false;
    }

    return false;
}

// Fires one real bullet from Agent 47's weapon at p_Aim. The muzzle is moved and turned toward the target for the
// instant of the shot and put back right after, so normal shooting is never affected.
//  - Origin: normally the weapon's own muzzle. If something solid is between the muzzle and the target (cover, a wall
//    the gun is pressed against) the bullet would die at the geometry, so in mode 1 it starts on the camera side instead.
//  - Everything here is plain ballistics + ammo: no shooting animation or recoil is played, so nothing can lock up if
//    those are unavailable.
int BetterInstinct::PsFireRealShot(const SVector3& p_Aim, ZActor* p_Actor) {
    ZHM5ItemWeapon* s_Weapon = GetHeldWeapon();
    if (!s_Weapon || !Hooks::ZHM5ItemWeapon_FireProjectiles) {
        return 0;
    }

    IFirearm* s_Firearm = s_Weapon;
    const int s_MagBefore = static_cast<int>(s_Firearm->GetBulletsInMagazine());
    if (s_MagBefore <= 0) {
        return 2;
    }

    ZSpatialEntity* s_Muzzle = s_Weapon->m_MuzzleExit.QueryInterface<ZSpatialEntity>();
    if (!s_Muzzle) {
        return 0;
    }

    const SMatrix s_Original = s_Muzzle->GetObjectToWorldMatrix();
    float s_Origin[3] = {s_Original.Trans.x, s_Original.Trans.y, s_Original.Trans.z};
    int s_OriginUsed = 0;

    if (m_PsOriginMode != 0 && p_Actor) {
        float s_Cam[3] = {};
        if (PsGetCameraPos(s_Cam)) {
            bool s_UseCamera = m_PsOriginMode == 2;
            if (m_PsOriginMode == 1 && !PsLineOfSight(s_Origin, p_Aim, p_Actor) && PsLineOfSight(s_Cam, p_Aim, p_Actor)) {
                s_UseCamera = true; // the muzzle is blocked but the camera can see the target
            }
            if (s_UseCamera) {
                const float s_Cx = p_Aim.x - s_Cam[0];
                const float s_Cy = p_Aim.y - s_Cam[1];
                const float s_Cz = p_Aim.z - s_Cam[2];
                const float s_CLen = std::sqrt(s_Cx * s_Cx + s_Cy * s_Cy + s_Cz * s_Cz);
                if (s_CLen > 1.5f) {
                    s_Origin[0] = s_Cam[0] + s_Cx / s_CLen * 0.6f;
                    s_Origin[1] = s_Cam[1] + s_Cy / s_CLen * 0.6f;
                    s_Origin[2] = s_Cam[2] + s_Cz / s_CLen * 0.6f;
                    s_OriginUsed = 1;
                }
            }
        }
    }

    float s_Dx = p_Aim.x - s_Origin[0];
    float s_Dy = p_Aim.y - s_Origin[1];
    float s_Dz = p_Aim.z - s_Origin[2];
    const float s_Len = std::sqrt(s_Dx * s_Dx + s_Dy * s_Dy + s_Dz * s_Dz);
    if (s_Len < 0.3f) {
        return 0;
    }
    s_Dx /= s_Len;
    s_Dy /= s_Len;
    s_Dz /= s_Len;

    const float4 s_Pos(s_Origin[0], s_Origin[1], s_Origin[2], s_Original.Trans.w);
    s_Muzzle->SetObjectToWorldMatrixFromEditor(PsBuildAimMatrix(s_Pos, s_Dx, s_Dy, s_Dz, m_PsMuzzleAxis));

    const auto s_OldToFire = s_Weapon->m_nBulletsToFire;
    const auto s_OldFired = s_Weapon->m_nBulletsFired;
    if (m_PsPrime) {
        s_Weapon->m_nBulletsToFire = 1;
        s_Weapon->m_nBulletsFired = 0;
    }
    s_Weapon->m_fPrecisionFactor = 0.0f;

    const bool s_Ok = Hooks::ZHM5ItemWeapon_FireProjectiles->CallOriginal(s_Weapon, true);

    s_Weapon->m_fPrecisionFactor = 0.0f;
    if (m_PsPrime) {
        s_Weapon->m_nBulletsToFire = s_OldToFire;
        s_Weapon->m_nBulletsFired = s_OldFired;
    }
    s_Muzzle->SetObjectToWorldMatrixFromEditor(s_Original);

    // Ammo: if the game didn't use a bullet itself, take one out of the magazine.
    int s_MagAfter = static_cast<int>(s_Firearm->GetBulletsInMagazine());
    if (s_MagAfter >= s_MagBefore) {
        s_Firearm->SetBulletsInMagazine(s_MagBefore - 1);
        s_MagAfter = s_MagBefore - 1;
    }

    m_PsDbgMagBefore = s_MagBefore;
    m_PsDbgMagAfter = s_MagAfter;
    m_PsDbgOrigin = s_OriginUsed;
    m_PsDbgDist = s_Len;
    m_PsLastFireOk = s_Ok;
    ++m_PsShotsFired;
    return 1;
}

void BetterInstinct::PsPlaySound(const wchar_t* p_File) {
    if (!m_PsSoundOn || m_SoundDir.empty()) {
        return;
    }
    std::error_code s_Ec;
    const fs::path s_Path = fs::path(m_SoundDir) / p_File;
    if (!fs::exists(s_Path, s_Ec)) {
        return;
    }
    PlaySoundW(s_Path.c_str(), nullptr, SND_FILENAME | SND_ASYNC | SND_NODEFAULT);
}

void BetterInstinct::ResetPointShooting() {
    {
        std::lock_guard<std::mutex> s_Lock(m_PsMutex);
        m_PsTargets.clear();
        m_PsFlashes.clear();
        m_PsHoverActor = nullptr;
        m_PsHoverValid = false;
        m_PsHoverCan = false;
        m_PsHoverFrac = 0.0f;
        m_PsLastAimValid = false;
        m_PsPrevDown = false;
    }
    m_PsTagCount = 0;
    m_PsState = PS_IDLE;
    g_PsSwallowLmb = false;
}

// Runs every game update. Hold the PS key: aim and tag. Release: execute all tags one after another.
void BetterInstinct::UpdatePointShooting(float p_Dt, bool p_GameFocused) {
    const bool s_KeyDown = m_PsEnabled && p_GameFocused && m_PsKey != 0 && PhysKeyDown(m_PsKey);

    const uint32_t s_Clicks = g_PsClicks.load();
    const bool s_Click = s_Clicks != m_PsLastClicks;
    m_PsLastClicks = s_Clicks;

    if (!m_PsEnabled && m_PsState.load() != PS_IDLE) {
        ResetPointShooting(); // switched off in the settings while running
    }

    const int s_State = m_PsState.load();

    // Everybody who is alive right now: tagged targets that die or despawn are dropped from the queue.
    std::vector<ZActor*> s_Alive;
    const bool s_HaveList = Globals::ActorManager != nullptr;
    if ((s_State != PS_IDLE || !m_PsPending.empty()) && s_HaveList) {
        const auto& s_List = Globals::ActorManager->m_aliveActors;
        for (size_t i = 0; i < s_List.size(); ++i) {
            if (ZActor* s_Actor = s_List[i].m_pInterfaceRef) {
                s_Alive.push_back(s_Actor);
            }
        }
    }
    auto s_IsAlive = [&](ZActor* p_Actor) {
        return std::find(s_Alive.begin(), s_Alive.end(), p_Actor) != s_Alive.end();
    };

    float s_Cam[3] = {};
    const bool s_HaveCam = PsGetCameraPos(s_Cam);
    float s_CamDir[3] = {};
    const bool s_HaveCamDir = PsGetCameraDir(s_CamDir);

    std::lock_guard<std::mutex> s_Lock(m_PsMutex);

    // Age the visual effects.
    for (auto& s_Flash : m_PsFlashes) {
        s_Flash.m_Age += p_Dt;
    }
    m_PsFlashes.erase(std::remove_if(m_PsFlashes.begin(), m_PsFlashes.end(), [](const PsFlash& f) { return f.m_Age > 1.2f; }),
                      m_PsFlashes.end());

    // Keep the queue valid and the markers glued to the enemies.
    if (s_State != PS_IDLE) {
        for (size_t i = m_PsTargets.size(); i-- > 0;) {
            PsTarget& s_Target = m_PsTargets[i];
            ZActor* s_Actor = s_Target.m_Ref.m_pInterfaceRef;
            if (!s_Actor || !s_IsAlive(s_Actor)) {
                m_PsTargets.erase(m_PsTargets.begin() + static_cast<std::ptrdiff_t>(i));
                continue;
            }
            SVector3 s_Aim;
            if (s_Target.m_HasLocal) {
                if (PsAimFromLocal(s_Target.m_Ref, s_Target.m_Local, s_Aim)) {
                    s_Target.m_Aim = s_Aim;
                }
            } else if (PsAimPoint(s_Target.m_Ref, s_Aim)) {
                s_Target.m_Aim = s_Aim;
            }
            s_Target.m_Age += p_Dt;
        }
        m_PsTagCount = static_cast<int>(m_PsTargets.size());
    }

    // Muzzle calibration aid: where does the chosen barrel axis point? (cached for the overlay)
    m_PsMuzzleValid = false;
    if (m_PsShowMuzzle) {
        if (ZHM5ItemWeapon* s_DbgWeapon = GetHeldWeapon()) {
            if (auto* s_DbgMuzzle = s_DbgWeapon->m_MuzzleExit.QueryInterface<ZSpatialEntity>()) {
                const SMatrix s_M = s_DbgMuzzle->GetObjectToWorldMatrix();
                const float s_Sign = (m_PsMuzzleAxis == 0 || m_PsMuzzleAxis == 2 || m_PsMuzzleAxis == 5) ? -1.0f : 1.0f;
                float s_D[3];
                if (m_PsMuzzleAxis == 0 || m_PsMuzzleAxis == 1) {
                    s_D[0] = s_M.YAxis.x; s_D[1] = s_M.YAxis.y; s_D[2] = s_M.YAxis.z;
                } else if (m_PsMuzzleAxis == 2 || m_PsMuzzleAxis == 3) {
                    s_D[0] = s_M.ZAxis.x; s_D[1] = s_M.ZAxis.y; s_D[2] = s_M.ZAxis.z;
                } else {
                    s_D[0] = s_M.XAxis.x; s_D[1] = s_M.XAxis.y; s_D[2] = s_M.XAxis.z;
                }
                m_PsMuzzlePos[0] = s_M.Trans.x;
                m_PsMuzzlePos[1] = s_M.Trans.y;
                m_PsMuzzlePos[2] = s_M.Trans.z;
                m_PsMuzzleDir[0] = s_D[0] * s_Sign;
                m_PsMuzzleDir[1] = s_D[1] * s_Sign;
                m_PsMuzzleDir[2] = s_D[2] * s_Sign;
                m_PsMuzzleValid = true;
            }
        }
    }

    // Check real shots a moment after they were fired: did the bullet actually hurt the target?
    for (size_t i = m_PsPending.size(); i-- > 0;) {
        PsPending& s_Pending = m_PsPending[i];
        s_Pending.m_Timer -= p_Dt;
        if (s_Pending.m_Timer > 0.0f) {
            continue;
        }

        ZActor* s_Actor = s_Pending.m_Ref.m_pInterfaceRef;
        bool s_Hit = true; // gone from the alive list = dead = hit
        if (s_Actor && s_IsAlive(s_Actor)) {
            s_Hit = s_Actor->m_fCurrentHitPoints < s_Pending.m_HpBefore - 0.001f;
            if (!s_Hit && m_PsMissFinish) {
                TEntityRef<IItem> s_Item;
                TEntityRef<ZSetpieceEntity> s_SetPieceEntity;
                Functions::ZActor_KillActor->Call(s_Actor, s_Item, s_SetPieceEntity, EDamageEvent::eDE_Shoot,
                                                  EDeathBehavior::eDB_IMPACT_ANIM);
            }
        }

        if (s_Hit) {
            ++m_PsShotsHit;
        } else {
            ++m_PsShotsMissed;
        }
        m_PsPending.erase(m_PsPending.begin() + static_cast<std::ptrdiff_t>(i));
    }

    switch (s_State) {
        case PS_IDLE: {
            m_PsHoverValid = false;
            if (s_KeyDown && !m_PsPrevDown && !m_Blocked) {
                // Point shooting needs a weapon in Agent 47's hands.
                if (m_PsRequireWeapon && !HasWeaponInHand()) {
                    m_PsHint = 2.5f;
                    m_PsHintKind = 1;
                    break;
                }
                ReleaseHipClick(); // a held-over hip click would otherwise stay down
                m_PsTargets.clear();
                m_PsTagCount = 0;
                m_PsHoverActor = nullptr;
                m_PsHoverFrac = 0.0f;
                m_PsState = PS_AIMING;
            }
            break;
        }

        case PS_AIMING: {
            // Key released, or the meter ran dry: shoot everything that was tagged.
            if (!s_KeyDown || m_Meter <= 0.0f) {
                if (m_Meter <= 0.0f) {
                    m_Locked = true; // ran dry
                }
                m_PsHoverValid = false;
                m_PsHoverActor = nullptr;
                m_PsHoverFrac = 0.0f;

                if (m_PsTargets.empty()) {
                    m_PsState = PS_IDLE;
                } else {
                    if (m_PsOrder == 1) {
                        float s_Player[3] = {};
                        const bool s_HavePlayer = PsGetPlayerPos(s_Player);
                        auto s_DistSq = [&](const PsTarget& t) {
                            const float dx = t.m_Aim.x - (s_HavePlayer ? s_Player[0] : s_Cam[0]);
                            const float dy = t.m_Aim.y - (s_HavePlayer ? s_Player[1] : s_Cam[1]);
                            const float dz = t.m_Aim.z - (s_HavePlayer ? s_Player[2] : s_Cam[2]);
                            return dx * dx + dy * dy + dz * dz;
                        };
                        std::stable_sort(m_PsTargets.begin(), m_PsTargets.end(),
                                         [&](const PsTarget& a, const PsTarget& b) { return s_DistSq(a) < s_DistSq(b); });
                    }
                    m_PsExecPhase = 0;
                    m_PsExecTimer = m_PsSnapTime;
                    m_PsLastAimValid = false;
                    m_PsState = PS_EXECUTING;
                }
                break;
            }

            // Which enemy is under (or near) the crosshair, and can the camera actually see him?
            bool s_HasBest = false;
            PsCandidate s_Best;
            if (s_HaveCam && s_HaveList) {
                const float s_ScreenW = m_ScreenW.load();
                const float s_ScreenH = m_ScreenH.load();
                const float s_CenterX = s_ScreenW * 0.5f;
                const float s_CenterY = s_ScreenH * 0.5f;
                const float s_Radius = m_PsSnapRadius * s_ScreenH;

                std::vector<PsCandidate> s_Cands;
                const auto& s_List = Globals::ActorManager->m_aliveActors;
                for (size_t i = 0; i < s_List.size(); ++i) {
                    const TEntityRef<ZActor>& s_Ref = s_List[i];
                    ZActor* s_Actor = s_Ref.m_pInterfaceRef;
                    if (!s_Actor || s_Actor->IsPacified()) {
                        continue;
                    }

                    bool s_Tagged = false;
                    for (const PsTarget& t : m_PsTargets) {
                        if (t.m_Ref.m_pInterfaceRef == s_Actor) { s_Tagged = true; break; }
                    }
                    if (s_Tagged) {
                        continue;
                    }

                    SVector3 s_Aim;
                    if (!PsAimPoint(s_Ref, s_Aim)) {
                        continue;
                    }

                    const float s_Dx = s_Aim.x - s_Cam[0];
                    const float s_Dy = s_Aim.y - s_Cam[1];
                    const float s_Dz = s_Aim.z - s_Cam[2];
                    const float s_WorldDist = std::sqrt(s_Dx * s_Dx + s_Dy * s_Dy + s_Dz * s_Dz);
                    if (s_WorldDist > m_PsMaxDist || s_WorldDist < 0.8f) {
                        continue;
                    }

                    SVector2 s_Screen;
                    if (!SDK()->WorldToScreen(s_Aim, s_Screen)) {
                        continue;
                    }
                    const float s_Sx = s_Screen.x - s_CenterX;
                    const float s_Sy = s_Screen.y - s_CenterY;
                    const float s_ScreenDist = std::sqrt(s_Sx * s_Sx + s_Sy * s_Sy);
                    if (s_ScreenDist > s_Radius) {
                        continue;
                    }

                    PsCandidate s_Cand;
                    s_Cand.m_Ref = s_Ref;
                    s_Cand.m_Aim = s_Aim;
                    s_Cand.m_ScreenDist = s_ScreenDist;
                    s_Cand.m_WorldDist = s_WorldDist;
                    s_Cands.push_back(s_Cand);
                }

                std::sort(s_Cands.begin(), s_Cands.end(),
                          [](const PsCandidate& a, const PsCandidate& b) { return a.m_ScreenDist < b.m_ScreenDist; });

                const size_t s_Checks = std::min<size_t>(s_Cands.size(), 4);
                for (size_t i = 0; i < s_Checks; ++i) {
                    if (PsLineOfSight(s_Cam, s_Cands[i].m_Aim, s_Cands[i].m_Ref.m_pInterfaceRef)) {
                        s_Best = s_Cands[i];
                        s_HasBest = true;
                        break;
                    }
                }
            }

            const bool s_CanTag = static_cast<int>(m_PsTargets.size()) < m_PsMaxTags && m_Meter >= m_PsTagCost &&
                                  m_Meter > 0.0f;

            auto s_Tag = [&](const PsCandidate& p_Cand) {
                PsTarget s_Target;
                s_Target.m_Ref = p_Cand.m_Ref;
                s_Target.m_Aim = p_Cand.m_Aim;
                s_Target.m_Dist = p_Cand.m_WorldDist;
                s_Target.m_Part = m_PsAimPart == 0 ? 0 : 1;

                // Tag the exact point under the crosshair, so a shot at a leg stays a shot at a leg.
                if (m_PsPrecise && s_HaveCamDir) {
                    float s_Local[3] = {};
                    SVector3 s_Hit;
                    if (PsRefineAim(s_Cam, s_CamDir, p_Cand.m_Ref, s_Local, s_Hit)) {
                        s_Target.m_Aim = s_Hit;
                        s_Target.m_Local[0] = s_Local[0];
                        s_Target.m_Local[1] = s_Local[1];
                        s_Target.m_Local[2] = s_Local[2];
                        s_Target.m_HasLocal = true;
                        s_Target.m_Part = PsClassifyPart(s_Local);
                    }
                }
                m_PsTargets.push_back(s_Target);
                m_PsTagCount = static_cast<int>(m_PsTargets.size());
                m_Meter = std::max(0.0f, m_Meter - m_PsTagCost);

                PsFlash s_Ping;
                s_Ping.m_Pos = p_Cand.m_Aim;
                s_Ping.m_Kill = false;
                m_PsFlashes.push_back(s_Ping);
                PsPlaySound(L"PointTag.wav");
            };

            m_PsHoverValid = s_HasBest;
            m_PsHoverCan = s_HasBest && s_CanTag;

            if (s_HasBest) {
                m_PsHoverAim = s_Best.m_Aim;
                ZActor* s_BestActor = s_Best.m_Ref.m_pInterfaceRef;

                if (m_PsTagMode == 1) {
                    // Hover: the crosshair has to rest on the same enemy for a moment.
                    if (s_BestActor == m_PsHoverActor) {
                        m_PsHoverFrac += p_Dt / std::max(0.05f, m_PsHoverDwell);
                    } else {
                        m_PsHoverActor = s_BestActor;
                        m_PsHoverFrac = 0.0f;
                    }
                    if (!s_CanTag) {
                        m_PsHoverFrac = 0.0f;
                    } else if (m_PsHoverFrac >= 1.0f) {
                        s_Tag(s_Best);
                        m_PsHoverActor = nullptr;
                        m_PsHoverFrac = 0.0f;
                        m_PsHoverValid = false;
                    }
                } else {
                    // Click: left mouse button tags whoever is under the crosshair.
                    m_PsHoverActor = s_BestActor;
                    m_PsHoverFrac = 0.0f;
                    if (s_Click && s_CanTag) {
                        s_Tag(s_Best);
                        m_PsHoverValid = false;
                    }
                }
            } else {
                m_PsHoverActor = nullptr;
                m_PsHoverFrac = 0.0f;
            }
            break;
        }

        case PS_EXECUTING: {
            m_PsHoverValid = false;
            m_PsExecTimer -= p_Dt;
            if (m_PsExecTimer > 0.0f) {
                break;
            }

            if (m_PsExecPhase == 0) {
                // The aim has snapped onto the next target: fire.
                if (m_PsTargets.empty()) {
                    m_PsState = PS_IDLE;
                    break;
                }

                const PsTarget s_Target = m_PsTargets.front();
                m_PsTargets.erase(m_PsTargets.begin());
                m_PsTagCount = static_cast<int>(m_PsTargets.size());

                if (ZActor* s_Actor = s_Target.m_Ref.m_pInterfaceRef) {
                    bool s_Shot = false;

                    if (m_PsShotMode == 0) {
                        // Real shot: a bullet leaves Agent 47's weapon and hits whatever body part was tagged.
                        const float s_HpBefore = s_Actor->m_fCurrentHitPoints;
                        const int s_Result = PsFireRealShot(s_Target.m_Aim, s_Actor);
                        m_PsLastShotResult = s_Result;

                        if (s_Result == 1) {
                            s_Shot = true;
                            PsPending s_Pending;
                            s_Pending.m_Ref = s_Target.m_Ref;
                            s_Pending.m_HpBefore = s_HpBefore;
                            s_Pending.m_Timer = 0.8f;
                            m_PsPending.push_back(s_Pending);
                        } else if (s_Result == 0 && !m_PsRequireWeapon) {
                            // "Require a weapon" is off and the weapon could not be fired (not detected): don't stall the
                            // sequence, finish the target directly instead.
                            TEntityRef<IItem> s_FbItem;
                            TEntityRef<ZSetpieceEntity> s_FbSetPiece;
                            Functions::ZActor_KillActor->Call(s_Actor, s_FbItem, s_FbSetPiece, EDamageEvent::eDE_Shoot,
                                                              EDeathBehavior::eDB_IMPACT_ANIM);
                            ++m_PsFallbackKills;
                            s_Shot = true;
                        } else {
                            // No weapon in hand, or the magazine is empty: the sequence stops here.
                            m_PsHint = 2.5f;
                            m_PsHintKind = s_Result == 2 ? 2 : 1;
                            m_PsTargets.clear();
                            m_PsTagCount = 0;
                            m_PsState = PS_IDLE;
                            break;
                        }
                    } else {
                        // Direct kill (no bullets).
                        TEntityRef<IItem> s_Item;
                        TEntityRef<ZSetpieceEntity> s_SetPieceEntity;
                        Functions::ZActor_KillActor->Call(s_Actor, s_Item, s_SetPieceEntity, EDamageEvent::eDE_Shoot,
                                                          EDeathBehavior::eDB_IMPACT_ANIM);
                        s_Shot = true;
                    }

                    if (s_Shot) {
                        PsFlash s_Kill;
                        s_Kill.m_Pos = s_Target.m_Aim;
                        s_Kill.m_Kill = true;
                        m_PsFlashes.push_back(s_Kill);
                        m_PsLastAim = s_Target.m_Aim;
                        m_PsLastAimValid = true;
                        m_PsPunch = 1.0f;
                        PsPlaySound(L"PointShot.wav");
                    }
                }

                m_PsExecPhase = 1;
                m_PsExecTimer = m_PsShotDelay;
            } else {
                // Pause after the shot is over: next snap, or back to normal gameplay.
                if (m_PsTargets.empty()) {
                    m_PsState = PS_IDLE;
                } else {
                    m_PsExecPhase = 0;
                    m_PsExecTimer = m_PsSnapTime;
                }
            }
            break;
        }

        default:
            break;
    }

    m_PsPrevDown = s_KeyDown;

    // Left click belongs to point shooting while it runs (always in click mode, optionally in hover mode).
    g_PsSwallowLmb = m_PsEnabled && m_PsState.load() != PS_IDLE && (m_PsTagMode == 0 || m_PsBlockFire);
}

// Draws everything you see of point shooting on top of the game: hover brackets, tagged markers, the snapping
// reticle, muzzle flashes and kill confirmations. Runs on the UI thread and only uses data cached by the game thread.
void BetterInstinct::DrawPsOverlay() {
    const ImVec2 s_Screen = ImGui::GetIO().DisplaySize;
    m_ScreenW = s_Screen.x;
    m_ScreenH = s_Screen.y;

    const float s_Dt = std::clamp(m_LastHudDt, 0.0f, 0.1f);
    const int s_State = m_PsState.load();

    std::vector<PsTarget> s_Targets;
    std::vector<PsFlash> s_Flashes;
    bool s_HoverValid = false;
    bool s_HoverCan = false;
    SVector3 s_HoverAim;
    float s_HoverFrac = 0.0f;
    SVector3 s_LastAim;
    bool s_LastValid = false;
    int s_Phase = 0;
    bool s_MuzzleValid = false;
    float s_MuzzlePos[3] = {};
    float s_MuzzleDir[3] = {};
    {
        std::lock_guard<std::mutex> s_Lock(m_PsMutex);
        s_Targets = m_PsTargets;
        s_Flashes = m_PsFlashes;
        s_HoverValid = m_PsHoverValid;
        s_HoverCan = m_PsHoverCan;
        s_HoverAim = m_PsHoverAim;
        s_HoverFrac = std::clamp(m_PsHoverFrac, 0.0f, 1.0f);
        s_LastAim = m_PsLastAim;
        s_LastValid = m_PsLastAimValid;
        s_Phase = m_PsExecPhase;
        s_MuzzleValid = m_PsMuzzleValid;
        for (int i = 0; i < 3; ++i) {
            s_MuzzlePos[i] = m_PsMuzzlePos[i];
            s_MuzzleDir[i] = m_PsMuzzleDir[i];
        }
    }
    const bool s_ShowMuzzle = m_PsShowMuzzle && m_InPlay && s_MuzzleValid;

    const bool s_Show = m_InPlay && s_State != PS_IDLE;
    m_PsFade += ((s_Show ? 1.0f : 0.0f) - m_PsFade) * std::clamp(10.0f * s_Dt, 0.0f, 1.0f);

    const float s_Punch = m_PsPunch.load();
    m_PsPunch = std::max(0.0f, s_Punch - s_Dt * 4.0f);

    if (s_State != PS_EXECUTING) {
        m_PsReticleSet = false;
    }
    if (m_PsFade < 0.01f && s_Flashes.empty() && s_Punch <= 0.0f && !s_ShowMuzzle) {
        return;
    }

    ImDrawList* s_Dl = ImGui::GetBackgroundDrawList();
    const float s_Ui = s_Screen.y / 1080.0f;
    const float s_Th = std::max(1.0f, std::round(s_Ui * 1.5f));
    const float s_Fade = m_PsFade;

    auto s_Col = [](float r, float g, float b, float a) {
        return ImGui::ColorConvertFloat4ToU32(ImVec4(r, g, b, std::clamp(a, 0.0f, 1.0f)));
    };
    auto s_Proj = [&](const SVector3& p_World, ImVec2& p_Out) {
        SVector2 s_P;
        if (!SDK()->WorldToScreen(p_World, s_P)) {
            return false;
        }
        p_Out = ImVec2(s_P.x, s_P.y);
        return true;
    };

    // Warm vignette while the world is frozen.
    if (s_Fade > 0.01f) {
        const float s_Edge = s_Screen.y * 0.20f;
        const ImU32 s_In = s_Col(0.95f, 0.62f, 0.10f, 0.16f * s_Fade);
        const ImU32 s_Out = s_Col(0.95f, 0.62f, 0.10f, 0.0f);
        s_Dl->AddRectFilledMultiColor(ImVec2(0, 0), ImVec2(s_Screen.x, s_Edge), s_In, s_In, s_Out, s_Out);
        s_Dl->AddRectFilledMultiColor(ImVec2(0, s_Screen.y - s_Edge), ImVec2(s_Screen.x, s_Screen.y), s_Out, s_Out, s_In, s_In);
        s_Dl->AddRectFilledMultiColor(ImVec2(0, 0), ImVec2(s_Edge, s_Screen.y), s_In, s_Out, s_Out, s_In);
        s_Dl->AddRectFilledMultiColor(ImVec2(s_Screen.x - s_Edge, 0), ImVec2(s_Screen.x, s_Screen.y), s_Out, s_In, s_In, s_Out);
    }

    const float s_Time = std::chrono::duration<float>(std::chrono::steady_clock::now().time_since_epoch()).count();

    // Crosshair pickup zone + hover brackets (aiming only).
    if (s_State == PS_AIMING) {
        s_Dl->AddCircle(ImVec2(s_Screen.x * 0.5f, s_Screen.y * 0.5f), m_PsSnapRadius * s_Screen.y,
                        s_Col(1.0f, 0.9f, 0.65f, 0.10f * s_Fade), 64, s_Th);

        ImVec2 s_P;
        if (s_HoverValid && s_Proj(s_HoverAim, s_P)) {
            const float s_R = 22.0f * s_Ui;
            const float s_C = s_R * 0.45f;
            const ImU32 s_Color = s_HoverCan ? s_Col(1.0f, 0.93f, 0.72f, 0.95f * s_Fade) : s_Col(0.90f, 0.25f, 0.20f, 0.95f * s_Fade);
            for (int sx = -1; sx <= 1; sx += 2) {
                for (int sy = -1; sy <= 1; sy += 2) {
                    const ImVec2 s_Corner(s_P.x + sx * s_R, s_P.y + sy * s_R);
                    s_Dl->AddLine(s_Corner, ImVec2(s_P.x + sx * (s_R - s_C), s_P.y + sy * s_R), s_Color, s_Th);
                    s_Dl->AddLine(s_Corner, ImVec2(s_P.x + sx * s_R, s_P.y + sy * (s_R - s_C)), s_Color, s_Th);
                }
            }
            if (!s_HoverCan) {
                const float s_X = s_R * 0.4f;
                s_Dl->AddLine(ImVec2(s_P.x - s_X, s_P.y - s_X), ImVec2(s_P.x + s_X, s_P.y + s_X), s_Color, s_Th);
                s_Dl->AddLine(ImVec2(s_P.x - s_X, s_P.y + s_X), ImVec2(s_P.x + s_X, s_P.y - s_X), s_Color, s_Th);
            } else if (m_PsTagMode == 1 && s_HoverFrac > 0.0f) {
                s_Dl->PathArcTo(s_P, s_R * 1.35f, -3.14159265f * 0.5f, -3.14159265f * 0.5f + 3.14159265f * 2.0f * s_HoverFrac, 32);
                s_Dl->PathStroke(s_Col(1.0f, 0.78f, 0.28f, 0.95f * s_Fade), 0, s_Th * 1.6f);
            }
        }
    }

    // Tagged targets: numbered markers linked in shooting order.
    ImVec2 s_Prev;
    bool s_HavePrev = false;
    ImFont* s_Font = ImGui::GetFont();
    const float s_FontSize = std::max(10.0f, ImGui::GetFontSize() * 0.9f * s_Ui);
    for (size_t i = 0; i < s_Targets.size(); ++i) {
        ImVec2 s_P;
        if (!s_Proj(s_Targets[i].m_Aim, s_P)) {
            continue;
        }

        const bool s_Current = s_State == PS_EXECUTING && i == 0;
        const float s_Pop = 1.0f + std::max(0.0f, 1.0f - s_Targets[i].m_Age / 0.25f) * 1.5f;
        const float s_Pulse = 0.85f + 0.15f * std::sin(s_Time * 9.0f + static_cast<float>(i));
        const float s_R = 13.0f * s_Ui * s_Pop;
        const float s_Cr = s_Current ? 1.0f : 1.0f;
        const float s_Cg = s_Current ? 0.30f : 0.80f;
        const float s_Cb = s_Current ? 0.22f : 0.30f;
        const float s_A = s_Fade * s_Pulse;

        if (s_HavePrev) {
            s_Dl->AddLine(s_Prev, s_P, s_Col(s_Cr, s_Cg, s_Cb, 0.30f * s_Fade), s_Th);
        }
        s_Prev = s_P;
        s_HavePrev = true;

        s_Dl->AddCircleFilled(s_P, s_R * 1.9f, s_Col(s_Cr, s_Cg, s_Cb, 0.14f * s_A), 24);
        s_Dl->AddCircle(s_P, s_R * 1.35f, s_Col(s_Cr, s_Cg, s_Cb, 0.85f * s_A), 28, s_Th);
        s_Dl->AddQuad(ImVec2(s_P.x, s_P.y - s_R), ImVec2(s_P.x + s_R, s_P.y), ImVec2(s_P.x, s_P.y + s_R),
                      ImVec2(s_P.x - s_R, s_P.y), s_Col(1.0f, 0.95f, 0.80f, 0.95f * s_A), s_Th * 1.2f);
        s_Dl->AddCircleFilled(s_P, s_R * 0.22f, s_Col(1.0f, 0.97f, 0.88f, 0.95f * s_A), 10);

        const float s_T0 = s_R * 1.7f;
        const float s_T1 = s_R * 2.4f;
        const ImU32 s_Tick = s_Col(s_Cr, s_Cg, s_Cb, 0.8f * s_A);
        s_Dl->AddLine(ImVec2(s_P.x - s_T1, s_P.y), ImVec2(s_P.x - s_T0, s_P.y), s_Tick, s_Th);
        s_Dl->AddLine(ImVec2(s_P.x + s_T0, s_P.y), ImVec2(s_P.x + s_T1, s_P.y), s_Tick, s_Th);
        s_Dl->AddLine(ImVec2(s_P.x, s_P.y - s_T1), ImVec2(s_P.x, s_P.y - s_T0), s_Tick, s_Th);
        s_Dl->AddLine(ImVec2(s_P.x, s_P.y + s_T0), ImVec2(s_P.x, s_P.y + s_T1), s_Tick, s_Th);

        char s_Num[8];
        std::snprintf(s_Num, sizeof(s_Num), "%d", static_cast<int>(i) + 1);
        s_Dl->AddText(s_Font, s_FontSize, ImVec2(s_P.x + s_R * 1.6f, s_P.y - s_R * 2.1f), s_Col(1.0f, 0.95f, 0.8f, 0.95f * s_A), s_Num);
        if (s_Targets[i].m_HasLocal) {
            s_Dl->AddText(s_Font, s_FontSize * 0.75f, ImVec2(s_P.x + s_R * 1.6f, s_P.y + s_R * 0.6f), s_Col(1.0f, 0.82f, 0.45f, 0.85f * s_A),
                          PsPartName(s_Targets[i].m_Part));
        }
    }

    // Execution: the aim snaps from target to target.
    if (s_State == PS_EXECUTING) {
        ImVec2 s_Target;
        bool s_HaveTarget = false;
        if (!s_Targets.empty()) {
            s_HaveTarget = s_Proj(s_Targets[0].m_Aim, s_Target);
        } else if (s_LastValid) {
            s_HaveTarget = s_Proj(s_LastAim, s_Target);
        }

        if (s_HaveTarget) {
            if (!m_PsReticleSet) {
                m_PsReticleX = s_Screen.x * 0.5f;
                m_PsReticleY = s_Screen.y * 0.5f;
                m_PsReticleSet = true;
            }
            const float s_Speed = s_Phase == 0 ? 30.0f : 80.0f;
            const float s_K = 1.0f - std::exp(-s_Dt * s_Speed);
            m_PsReticleX += (s_Target.x - m_PsReticleX) * s_K;
            m_PsReticleY += (s_Target.y - m_PsReticleY) * s_K;

            const ImVec2 s_P(m_PsReticleX, m_PsReticleY);
            const float s_R = 30.0f * s_Ui;
            const ImU32 s_Color = s_Col(1.0f, 0.35f, 0.25f, 0.95f);
            s_Dl->AddCircle(s_P, s_R, s_Color, 40, s_Th * 1.4f);
            s_Dl->AddLine(ImVec2(s_P.x - s_R * 1.5f, s_P.y), ImVec2(s_P.x - s_R * 0.45f, s_P.y), s_Color, s_Th);
            s_Dl->AddLine(ImVec2(s_P.x + s_R * 0.45f, s_P.y), ImVec2(s_P.x + s_R * 1.5f, s_P.y), s_Color, s_Th);
            s_Dl->AddLine(ImVec2(s_P.x, s_P.y - s_R * 1.5f), ImVec2(s_P.x, s_P.y - s_R * 0.45f), s_Color, s_Th);
            s_Dl->AddLine(ImVec2(s_P.x, s_P.y + s_R * 0.45f), ImVec2(s_P.x, s_P.y + s_R * 1.5f), s_Color, s_Th);
            s_Dl->AddCircleFilled(s_P, s_R * 0.10f, s_Color, 8);
        }
    }

    // Muzzle calibration: a cyan line along the barrel direction you picked. It should run straight out of the gun
    // toward where the weapon points; if it points sideways or backwards, pick another muzzle direction.
    if (s_ShowMuzzle) {
        const SVector3 s_Start(s_MuzzlePos[0], s_MuzzlePos[1], s_MuzzlePos[2]);
        const SVector3 s_End(s_MuzzlePos[0] + s_MuzzleDir[0] * 12.0f, s_MuzzlePos[1] + s_MuzzleDir[1] * 12.0f,
                             s_MuzzlePos[2] + s_MuzzleDir[2] * 12.0f);
        ImVec2 s_A;
        ImVec2 s_B;
        const bool s_HaveA = s_Proj(s_Start, s_A);
        const bool s_HaveB = s_Proj(s_End, s_B);
        if (s_HaveA && s_HaveB) {
            s_Dl->AddLine(s_A, s_B, s_Col(0.2f, 0.9f, 1.0f, 0.9f), s_Th * 2.0f);
            s_Dl->AddCircleFilled(s_A, 5.0f * s_Ui, s_Col(0.2f, 0.9f, 1.0f, 0.95f), 12);
            s_Dl->AddCircle(s_B, 7.0f * s_Ui, s_Col(0.2f, 0.9f, 1.0f, 0.95f), 16, s_Th);
            s_Dl->AddText(ImGui::GetFont(), std::max(10.0f, ImGui::GetFontSize() * 0.85f * s_Ui), ImVec2(s_A.x + 8.0f * s_Ui, s_A.y - 18.0f * s_Ui),
                          s_Col(0.2f, 0.9f, 1.0f, 0.95f), "MUZZLE");
        }
    }

    // Shots and tag pings.
    for (const PsFlash& s_Flash : s_Flashes) {
        ImVec2 s_P;
        if (!s_Proj(s_Flash.m_Pos, s_P)) {
            continue;
        }
        const float s_T = s_Flash.m_Age;

        if (!s_Flash.m_Kill) {
            const float s_K = std::clamp(s_T / 0.4f, 0.0f, 1.0f);
            s_Dl->AddCircle(s_P, (8.0f + 30.0f * s_K) * s_Ui, s_Col(1.0f, 0.82f, 0.35f, 1.0f - s_K), 28, s_Th);
            continue;
        }

        // Muzzle flash: a tracer from the bottom of the screen to the target for a split second.
        if (s_T < 0.09f) {
            const float s_F = 1.0f - s_T / 0.09f;
            const ImVec2 s_Muzzle(s_Screen.x * 0.5f, s_Screen.y * 0.84f);
            s_Dl->AddLine(s_Muzzle, s_P, s_Col(1.0f, 0.95f, 0.75f, 0.9f * s_F), s_Th * 3.0f);
            s_Dl->AddLine(s_Muzzle, s_P, s_Col(1.0f, 1.0f, 1.0f, 0.9f * s_F), s_Th);
            const float s_Len = 38.0f * s_Ui * s_F;
            s_Dl->AddLine(ImVec2(s_Muzzle.x - s_Len, s_Muzzle.y), ImVec2(s_Muzzle.x + s_Len, s_Muzzle.y), s_Col(1.0f, 0.9f, 0.6f, 0.9f * s_F), s_Th * 2.0f);
            s_Dl->AddLine(ImVec2(s_Muzzle.x, s_Muzzle.y - s_Len), ImVec2(s_Muzzle.x, s_Muzzle.y + s_Len), s_Col(1.0f, 0.9f, 0.6f, 0.9f * s_F), s_Th * 2.0f);
            s_Dl->AddCircleFilled(s_Muzzle, 9.0f * s_Ui * s_F, s_Col(1.0f, 0.97f, 0.85f, 0.95f * s_F), 14);
        }

        // Impact ring.
        const float s_K = std::clamp(s_T / 0.6f, 0.0f, 1.0f);
        s_Dl->AddCircle(s_P, (10.0f + 55.0f * s_K) * s_Ui, s_Col(1.0f, 0.35f, 0.25f, 1.0f - s_K), 32, s_Th * 1.5f);
        if (s_T < 0.12f) {
            s_Dl->AddCircleFilled(s_P, 16.0f * s_Ui * (1.0f - s_T / 0.12f), s_Col(1.0f, 1.0f, 0.95f, 0.9f), 14);
        }

        // Kill confirmation: a red X that holds, then fades.
        if (s_T < 1.0f) {
            const float s_Xa = 1.0f - std::clamp((s_T - 0.35f) / 0.65f, 0.0f, 1.0f);
            const float s_X = 12.0f * s_Ui * (0.6f + 0.4f * std::min(s_T / 0.1f, 1.0f));
            const ImU32 s_Red = s_Col(0.95f, 0.18f, 0.15f, 0.95f * s_Xa);
            s_Dl->AddLine(ImVec2(s_P.x - s_X, s_P.y - s_X), ImVec2(s_P.x + s_X, s_P.y + s_X), s_Red, s_Th * 2.2f);
            s_Dl->AddLine(ImVec2(s_P.x - s_X, s_P.y + s_X), ImVec2(s_P.x + s_X, s_P.y - s_X), s_Red, s_Th * 2.2f);
        }
    }

    // Screen punch when a shot goes off.
    if (s_Punch > 0.0f) {
        s_Dl->AddRectFilled(ImVec2(0, 0), s_Screen, s_Col(1.0f, 0.95f, 0.85f, 0.16f * s_Punch));
    }
}

// The small "[ MOUSE 4 ]  POINT SHOOT" widget above the Instinct meter. It always shows the key that is bound right now.
void BetterInstinct::DrawPsPrompt() {
    const ImVec2 s_Screen = ImGui::GetIO().DisplaySize;
    const float s_Dt = std::clamp(m_LastHudDt, 0.0f, 0.1f);
    const int s_State = m_PsState.load();

    const float s_Hint = m_PsHint.load();
    if (s_Hint > 0.0f) {
        m_PsHint = std::max(0.0f, s_Hint - s_Dt);
    }

    const bool s_WeaponOk = !m_PsRequireWeapon || HasWeaponInHand();
    const bool s_Ready = m_PsPrompt && m_PsEnabled && m_InPlay && s_WeaponOk && (m_Active.load() || s_State == PS_AIMING);
    m_PsPromptFade += ((s_Ready ? 1.0f : 0.0f) - m_PsPromptFade) * std::clamp(10.0f * s_Dt, 0.0f, 1.0f);

    const bool s_ShowHint = s_Hint > 0.0f && m_InPlay;
    if (m_PsPromptFade < 0.01f && !s_ShowHint) {
        return;
    }

    ImDrawList* s_Dl = ImGui::GetBackgroundDrawList();
    ImFont* s_Font = ImGui::GetFont();
    const float s_Ui = (s_Screen.y / 1080.0f) * m_HudScale;
    const float s_Th = std::max(1.0f, std::round(s_Ui));

    // Same geometry as the Instinct meter, so the widget sits just above its label.
    const float s_Bottom = s_Screen.y - m_HudYOffset * (s_Screen.y / 1080.0f);
    const float s_Top = s_Bottom - std::max(4.0f, 7.0f * s_Ui);
    const float s_LabelH = std::max(10.0f, ImGui::GetFontSize() * 0.8f * s_Ui) + 5.0f * s_Ui;
    const float s_CenterX = s_Screen.x * 0.5f;
    const float s_CenterY = s_Top - s_LabelH - 26.0f * s_Ui;

    auto s_Col = [](float r, float g, float b, float a) {
        return ImGui::ColorConvertFloat4ToU32(ImVec4(r, g, b, std::clamp(a, 0.0f, 1.0f)));
    };
    const float s_Time = std::chrono::duration<float>(std::chrono::steady_clock::now().time_since_epoch()).count();
    const float s_A = m_PsPromptFade * m_HudOpacity;

    if (m_PsPromptFade >= 0.01f) {
        std::string s_KeyText = KeyName(m_PsKey);
        for (char& s_Ch : s_KeyText) {
            s_Ch = static_cast<char>(std::toupper(static_cast<unsigned char>(s_Ch)));
        }
        const char* s_Action = s_State == PS_AIMING ? (m_PsTagCount.load() > 0 ? "RELEASE TO FIRE" : "TAG TARGETS") : "POINT SHOOT";

        const float s_FontSize = std::max(10.0f, ImGui::GetFontSize() * 0.85f * s_Ui);
        const ImVec2 s_KeySize = s_Font->CalcTextSizeA(s_FontSize, FLT_MAX, 0.0f, s_KeyText.c_str());
        const ImVec2 s_ActSize = s_Font->CalcTextSizeA(s_FontSize, FLT_MAX, 0.0f, s_Action);

        const float s_Pad = 8.0f * s_Ui;
        const float s_CapH = std::max(22.0f * s_Ui, s_KeySize.y + 8.0f * s_Ui);
        const float s_CapW = std::max(26.0f * s_Ui, s_KeySize.x + 2.0f * s_Pad);
        const float s_Gap = 10.0f * s_Ui;
        const float s_Total = s_CapW + s_Gap + s_ActSize.x;
        const float s_X0 = s_CenterX - s_Total * 0.5f;
        const ImVec2 s_CapMin(s_X0, s_CenterY - s_CapH * 0.5f);
        const ImVec2 s_CapMax(s_X0 + s_CapW, s_CenterY + s_CapH * 0.5f);

        const float s_Pulse = s_State == PS_AIMING ? 1.0f : 0.78f + 0.22f * std::sin(s_Time * 4.0f);
        s_Dl->AddRectFilled(s_CapMin, s_CapMax, s_Col(0.0f, 0.0f, 0.0f, 0.55f * s_A), 3.0f * s_Ui);
        s_Dl->AddRect(s_CapMin, s_CapMax, s_Col(m_FillColor[0], m_FillColor[1], m_FillColor[2], 0.95f * s_Pulse * s_A), 3.0f * s_Ui, 0, s_Th);

        const ImVec2 s_KeyPos(s_CapMin.x + (s_CapW - s_KeySize.x) * 0.5f, s_CenterY - s_KeySize.y * 0.5f);
        s_Dl->AddText(s_Font, s_FontSize, ImVec2(s_KeyPos.x + 1.0f, s_KeyPos.y + 1.0f), s_Col(0.0f, 0.0f, 0.0f, 0.7f * s_A), s_KeyText.c_str());
        s_Dl->AddText(s_Font, s_FontSize, s_KeyPos, s_Col(1.0f, 0.97f, 0.88f, s_A), s_KeyText.c_str());

        const ImVec2 s_ActPos(s_CapMax.x + s_Gap, s_CenterY - s_ActSize.y * 0.5f);
        s_Dl->AddText(s_Font, s_FontSize, ImVec2(s_ActPos.x + 1.0f, s_ActPos.y + 1.0f), s_Col(0.0f, 0.0f, 0.0f, 0.7f * s_A), s_Action);
        s_Dl->AddText(s_Font, s_FontSize, s_ActPos, s_Col(0.95f, 0.95f, 0.95f, 0.9f * s_A), s_Action);
    }

    if (s_ShowHint) {
        const char* s_Msg = m_PsHintKind.load() == 2 ? "OUT OF AMMO" : "DRAW A WEAPON TO POINT SHOOT";
        const float s_FontSize = std::max(10.0f, ImGui::GetFontSize() * 0.9f * s_Ui);
        const ImVec2 s_Size = s_Font->CalcTextSizeA(s_FontSize, FLT_MAX, 0.0f, s_Msg);
        const float s_HintA = std::clamp(s_Hint / 0.5f, 0.0f, 1.0f) * m_HudOpacity;
        const float s_Y = s_CenterY - (m_PsPromptFade >= 0.01f ? 30.0f * s_Ui : 0.0f) - s_Size.y * 0.5f;
        const ImVec2 s_Pos(s_CenterX - s_Size.x * 0.5f, s_Y);
        s_Dl->AddText(s_Font, s_FontSize, ImVec2(s_Pos.x + 1.0f, s_Pos.y + 1.0f), s_Col(0.0f, 0.0f, 0.0f, 0.8f * s_HintA), s_Msg);
        s_Dl->AddText(s_Font, s_FontSize, s_Pos, s_Col(m_LowColor[0], m_LowColor[1], m_LowColor[2], s_HintA), s_Msg);
    }
}

// A "click to change" key button (same behaviour as the Instinct key button). Returns true when the key changed.
bool BetterInstinct::DrawKeyBinder(const char* p_Id, int& p_Key, bool& p_Binding, bool& p_Armed) {
    bool s_Changed = false;
    ImGui::PushID(p_Id);

    if (p_Binding) {
        ImGui::Button("Press a key or mouse button... (Esc cancels)", ImVec2(-1, 0));

        bool s_AnyDown = false;
        for (int vk = 1; vk < 255; ++vk) {
            if (vk >= VK_SHIFT && vk <= VK_MENU) continue;
            if (IsKeyDown(vk)) { s_AnyDown = true; break; }
        }

        if (!p_Armed) {
            p_Armed = !s_AnyDown; // wait for the click that started binding to be released
        } else {
            for (int vk = 1; vk < 255; ++vk) {
                if (vk >= VK_SHIFT && vk <= VK_MENU) continue;
                if (!IsKeyDown(vk)) continue;

                if (vk != VK_ESCAPE) {
                    p_Key = vk;
                    s_Changed = true;
                }
                p_Binding = false;
                break;
            }
        }
    } else {
        const std::string s_Text = KeyName(p_Key) + "  (click to change)";
        if (ImGui::Button(s_Text.c_str(), ImVec2(-1, 0))) {
            p_Binding = true;
            p_Armed = false;
        }
    }

    ImGui::PopID();
    return s_Changed;
}

// ---------------------------------------------------------------------------
// Game loop
// ---------------------------------------------------------------------------

void BetterInstinct::OnFrameUpdate(const SGameUpdateEvent& p_UpdateEvent) {
    if (!Globals::GameTimeManager) {
        return;
    }

    // Real (unscaled) frame delta, so neither the meter nor the blend depend on the slowdown itself.
    using clock = std::chrono::steady_clock;
    const auto s_Now = clock::now();
    const float s_Dt = std::min(std::chrono::duration<float>(s_Now - m_LastFrame).count(), 0.1f);
    m_LastFrame = s_Now;
    m_LastUpdateTicks.store(s_Now.time_since_epoch().count());
    ++m_FrameCount;

    // Only run the Instinct logic while Agent 47 actually exists, so nothing happens in the main menu / hub screens.
    bool s_HasPlayer = false;
    if (auto s_LocalHitman = SDK()->GetLocalPlayer()) {
        s_HasPlayer = true;
    }
    m_HasPlayer = s_HasPlayer;

    if (!s_HasPlayer) {
        m_NoPlayerTime += s_Dt;
        m_Active.store(false);
        m_InputBlock = false;
        g_Block = false;
        m_PrevDown = false;
        g_HipAssist = false;
        ReleaseHipClick(); // never leave the left button held when leaving the mission
        ResetPointShooting();
        UpdateTint(s_Dt, false); // fade out and restore the tint when there is no player

        if (m_NoPlayerTime > 1.0f) { // really out of the mission (not just a short gap): start the next one fresh
            m_Meter = 1.0f;
            m_Locked = false;
            m_Blocked = false;
            m_NeedRelease = false;
            m_ToggleState = false;
            m_RechargeWait = 0.0f;
            m_HoldVisible = 0.0f;

            if (m_Touched) {
                Globals::GameTimeManager->m_fGameTimeMultiplier = 1.0f;
                m_CurrentScale = 1.0f;
                m_Touched = false;
            }
        }
        return;
    }
    m_NoPlayerTime = 0.0f;

    // Only react to keys when the game window is the active window.
    DWORD s_ForegroundPid = 0;
    GetWindowThreadProcessId(GetForegroundWindow(), &s_ForegroundPid);
    const bool s_GameFocused = s_ForegroundPid == GetCurrentProcessId() && !m_Binding && !m_BindingPs;
    const bool s_KeyDown = s_GameFocused && InstinctKeyDown();
    const bool s_PressEdge = s_KeyDown && !m_PrevDown;

    g_Key = m_InstinctKey;

    // Is Instinct usable right now? Not if the meter ran dry (until it recovers) or is below the start minimum.
    const bool s_WasActive = m_Active.load() || m_PsState.load() != PS_IDLE;
    m_Blocked = m_Locked || (!s_WasActive && m_Meter < kMinStart);

    // Block the key at the input level while unusable, so the game can't switch its own Instinct on either.
    g_Block = m_BlockEnabled && m_Blocked;
    // Same at the game level: the game is told the Instinct action is not pressed.
    m_InputBlock = (m_BlockEnabled && m_Blocked) || m_ForceBlockTest;

    // Safety net that doesn't depend on knowing the game's action name: while Instinct is unusable and the
    // player presses/holds the key, undo it the same way as when the meter runs dry (inject a key release).
    //  - Hold mode: release every frame while the key is held (also beats key auto-repeat).
    //  - Toggle mode: tap once on the press, which flips the game's Instinct straight back off.
    if (m_BlockEnabled && m_Blocked && s_KeyDown) {
        if (m_ToggleMode) {
            if (s_PressEdge) ForceReleaseInstinct();
        } else {
            ForceReleaseInstinct();
        }
    }

    // A key press that happened while blocked (or that we force-released) must be released before Instinct can start again.
    if (!s_KeyDown) {
        m_NeedRelease = false;
    } else if (m_Blocked) {
        m_NeedRelease = true;
    }

    // Point shooting (state machine, tagging, execution).
    UpdatePointShooting(s_Dt, s_GameFocused);

    // Does the player want Instinct right now?
    bool s_Want;
    if (m_ToggleMode) {
        if (s_KeyDown && !m_PrevDown && !m_Blocked) {
            m_ToggleState = !m_ToggleState;
        }
        if (m_Blocked) {
            m_ToggleState = false;
        }
        s_Want = m_ToggleState;
    } else {
        m_ToggleState = false;
        s_Want = s_KeyDown && !m_NeedRelease && !m_Blocked;
    }
    m_PrevDown = s_KeyDown;

    // Point shooting takes over completely while it is running.
    if (m_PsState.load() != PS_IDLE) {
        s_Want = false;
        m_ToggleState = false;
    }

    // Ran dry while in use: lock Instinct and make the game drop it too.
    if (s_Want && m_Meter <= 0.0f) {
        m_Locked = true;
        m_Blocked = true;
        m_NeedRelease = true;
        m_ToggleState = false;
        s_Want = false;
        if (m_BlockEnabled) {
            g_Block = true;
            m_InputBlock = true; // game-level block takes effect immediately
            ForceReleaseInstinct();
        }
    }

    m_Active.store(s_Want);
    UpdateTint(s_Dt, s_Want);
    g_HipGameMs = m_HipGameMs;
    g_HipAssist = m_HipAssist && m_PsState.load() == PS_IDLE && (s_Want || m_CurrentScale < 0.9f); // also covers the short blend back to normal speed

    // Meter: drains while active, recharges (after a short delay) otherwise.
    const int s_PsNow = m_PsState.load();
    if (s_Want) {
        m_Meter -= s_Dt / m_DrainSeconds;
        m_RechargeWait = m_RechargeDelay;
    } else if (s_PsNow == PS_AIMING) {
        m_Meter -= s_Dt / m_DrainSeconds * m_PsDrainMul;
        m_RechargeWait = m_RechargeDelay;
    } else if (s_PsNow == PS_EXECUTING) {
        m_RechargeWait = m_RechargeDelay;
    } else if (m_RechargeWait > 0.0f) {
        m_RechargeWait -= s_Dt;
    } else {
        m_Meter += s_Dt / m_RechargeSeconds;
    }
    m_Meter = std::clamp(m_Meter, 0.0f, 1.0f);

    if (m_Locked && m_Meter >= kUnlockLevel) {
        m_Locked = false;
    }

    if (s_Want || m_Meter < 0.999f) {
        m_HoldVisible = 1.5f;
    } else {
        m_HoldVisible = std::max(0.0f, m_HoldVisible - s_Dt);
    }

    // Time scale.
    const bool s_PsSlow = m_PsState.load() != PS_IDLE;
    const float s_Target = s_PsSlow ? (m_PsState.load() == PS_EXECUTING ? m_PsExecScale : m_PsScale)
                                    : (s_Want ? m_FocusScale : 1.0f);
    const float s_T = std::clamp(m_BlendSpeed * s_Dt, 0.0f, 1.0f);
    m_CurrentScale += (s_Target - m_CurrentScale) * s_T;

    // Hip-fire assist bookkeeping: a held-over left click is released once it has lasted long enough in GAME time.
    g_HipTimeScale = m_CurrentScale;
    if (g_HipTimer.load() != 0) {
        const float s_Remain = g_HipRemain.load() - s_Dt * 1000.0f * m_CurrentScale;
        g_HipRemain = s_Remain;
        if (s_Remain <= 0.0f) {
            ReleaseHipClick();
        }
    }

    if (s_Want || s_PsSlow || std::abs(m_CurrentScale - 1.0f) > 0.002f) {
        Globals::GameTimeManager->m_fGameTimeMultiplier = m_CurrentScale;
        m_Touched = true;
    } else if (m_Touched) {
        // Back to normal: hand control of the multiplier back to the game.
        Globals::GameTimeManager->m_fGameTimeMultiplier = 1.0f;
        m_CurrentScale = 1.0f;
        m_Touched = false;
    }
}

// ---------------------------------------------------------------------------
// Instinct color tint
// ---------------------------------------------------------------------------

// Runs every frame (also outside missions, with p_Active = false, so the tint always fades out and is restored).
void BetterInstinct::UpdateTint(float p_Dt, bool p_Active) {
    const float s_Target = (m_TintEnabled && p_Active) ? 1.0f : 0.0f;

    float s_Blend = m_TintBlend.load();
    s_Blend += (s_Target - s_Blend) * std::clamp(m_BlendSpeed * p_Dt, 0.0f, 1.0f);
    if (std::abs(s_Target - s_Blend) < 0.002f) s_Blend = s_Target;
    m_TintBlend.store(s_Blend);

    if (s_Blend > 0.0f) {
        ApplyTint(s_Blend);
    } else if (m_TintApplied) {
        RestoreTint();
    }
}

void BetterInstinct::ApplyTint(float p_Blend) {
    // A multiplier on the world's colors: 1 = unchanged, so blend 0 or strength 0 is exactly the vanilla look.
    const float s_Amount = std::clamp(p_Blend * m_TintStrength, 0.0f, 1.0f);
    for (int i = 0; i < 3; ++i) {
        m_TintMultiplier[i] = 1.0f + (std::clamp(m_InstinctTint[i], 0.0f, 1.0f) - 1.0f) * s_Amount;
    }
    m_TintApplied = true;

    // ENGINE LAYER (not connected yet):
    // Write m_TintMultiplier into the color-filter / multiplier parameter of the game's post-filter that carries
    // the Instinct look. That post-filter is part of the 3D scene render, so the UI, menus and map stay untouched.
    //  - Save the original value the first time through, and put exactly that value back in RestoreTint().
    //  - Set m_TintHookReady = true only once the write really reaches the engine (the settings window shows it).
    // To find the right entity type and property name, look through the SDK's Glacier headers / ZHMTools type
    // definitions for "Postfilter" and "ColorGrading", and inspect the live entities with the SDK's editor
    // tools while Instinct is on.
}

void BetterInstinct::RestoreTint() {
    m_TintMultiplier[0] = m_TintMultiplier[1] = m_TintMultiplier[2] = 1.0f;
    m_TintApplied = false;

    // ENGINE LAYER (not connected yet): put back the original post-filter value saved in ApplyTint().
}

// ---------------------------------------------------------------------------
// HUD meter
// ---------------------------------------------------------------------------

float BetterInstinct::Rand01() {
    m_Rng = m_Rng * 1664525u + 1013904223u;
    return static_cast<float>(m_Rng >> 8) * (1.0f / 16777216.0f);
}

void BetterInstinct::DrawHud() {
    using clock = std::chrono::steady_clock;
    const auto s_Now = clock::now();
    const float s_Dt = std::min(std::chrono::duration<float>(s_Now - m_HudLast).count(), 0.1f);
    m_HudLast = s_Now;

    // In play = the game loop ran recently AND Agent 47 exists. Menus and loading screens hide the meter.
    const int64_t s_SinceUpdate = s_Now.time_since_epoch().count() - m_LastUpdateTicks.load();
    const bool s_InPlay = m_HasPlayer.load() &&
        std::chrono::duration_cast<std::chrono::milliseconds>(clock::duration(s_SinceUpdate)).count() < 250;
    m_InPlay = s_InPlay;
    m_LastHudDt = s_Dt;
    const bool s_Preview = m_ShowUI; // while the settings window is open, always show the meter so it can be positioned

    const bool s_Active = m_Active.load() || m_PsState.load() != PS_IDLE;

    float s_TargetAlpha = 0.0f;
    if (m_HudEnabled && (s_InPlay || s_Preview)) {
        if (s_Preview || m_HudAlways || s_Active || m_Blocked || m_HoldVisible > 0.0f) {
            s_TargetAlpha = 1.0f;
        }
    }
    m_HudAlpha += (s_TargetAlpha - m_HudAlpha) * std::clamp(8.0f * s_Dt, 0.0f, 1.0f);
    if (m_HudAlpha < 0.01f) {
        m_Particles.clear();
        return;
    }

    const ImVec2 s_Screen = ImGui::GetIO().DisplaySize;
    const float s_Ui = (s_Screen.y / 1080.0f) * m_HudScale;
    const float s_Thick = std::max(1.0f, std::round(s_Ui));
    const float s_W = s_Screen.x * m_HudWidth * m_HudScale;
    const float s_H = std::max(5.0f, 9.0f * s_Ui);
    const float s_Cx = s_Screen.x * 0.5f;
    const float s_Bottom = s_Screen.y - m_HudYOffset * (s_Screen.y / 1080.0f);
    const float s_Top = s_Bottom - s_H;
    const float s_Left = s_Cx - s_W * 0.5f;
    const float s_Right = s_Cx + s_W * 0.5f;

    const float s_Time = std::chrono::duration<float>(s_Now.time_since_epoch()).count();
    const float s_A = m_HudAlpha * m_HudOpacity;
    ImDrawList* s_Dl = ImGui::GetBackgroundDrawList();

    auto s_Col = [](float r, float g, float b, float a) {
        return ImGui::ColorConvertFloat4ToU32(ImVec4(r, g, b, std::clamp(a, 0.0f, 1.0f)));
    };

    // Fill color: gold normally, blends to the "low" color when nearly empty, pulses while recharging.
    const float s_Low = 1.0f - std::clamp(m_Meter / kLowLevel, 0.0f, 1.0f);
    float s_R = m_FillColor[0] + (m_LowColor[0] - m_FillColor[0]) * s_Low;
    float s_G = m_FillColor[1] + (m_LowColor[1] - m_FillColor[1]) * s_Low;
    float s_B = m_FillColor[2] + (m_LowColor[2] - m_FillColor[2]) * s_Low;
    float s_FillA = s_A;
    if (m_Blocked) {
        s_R = m_LowColor[0];
        s_G = m_LowColor[1];
        s_B = m_LowColor[2];
        s_FillA = s_A * (0.55f + 0.35f * std::sin(s_Time * 8.0f));
    }

    // Label above the bar.
    const char* s_Label = m_PsState.load() != PS_IDLE ? "POINT SHOOTING" : (m_Blocked ? "RECHARGING" : "INSTINCT");
    ImFont* s_Font = ImGui::GetFont();
    const float s_FontSize = std::max(10.0f, ImGui::GetFontSize() * 0.8f * s_Ui);
    const ImVec2 s_TextSize = s_Font->CalcTextSizeA(s_FontSize, FLT_MAX, 0.0f, s_Label);
    const ImVec2 s_TextPos(s_Cx - s_TextSize.x * 0.5f, s_Top - s_TextSize.y - 6.0f * s_Ui);
    s_Dl->AddText(s_Font, s_FontSize, ImVec2(s_TextPos.x + 1.0f, s_TextPos.y + 1.0f), IM_COL32(0, 0, 0, static_cast<int>(160 * s_A)), s_Label);
    s_Dl->AddText(s_Font, s_FontSize, s_TextPos, s_Col(1.0f, 0.93f, 0.75f, 0.85f * s_A), s_Label);

    // Dark backing, then the grey "empty" track.
    const float s_Inset = s_Thick * 2.0f;
    const float s_InnerL = s_Left + s_Inset;
    const float s_InnerR = s_Right - s_Inset;
    const float s_InnerT = s_Top + s_Inset;
    const float s_InnerB = s_Bottom - s_Inset;
    const float s_MidY = (s_InnerT + s_InnerB) * 0.5f;

    s_Dl->AddRectFilled(ImVec2(s_Left, s_Top), ImVec2(s_Right, s_Bottom), IM_COL32(10, 10, 12, static_cast<int>(170 * s_A)));
    s_Dl->AddRectFilled(ImVec2(s_InnerL, s_InnerT), ImVec2(s_InnerR, s_InnerB), IM_COL32(88, 88, 92, static_cast<int>(120 * s_A)));

    // Gold fill: darker at the start, brighter toward the tip.
    const float s_HeadX = s_InnerL + (s_InnerR - s_InnerL) * m_Meter;
    if (s_HeadX - s_InnerL > 0.5f) {
        const ImU32 s_Dim = s_Col(s_R * 0.50f, s_G * 0.50f, s_B * 0.50f, s_FillA);
        const ImU32 s_Bright = s_Col(s_R + (1.0f - s_R) * 0.35f, s_G + (1.0f - s_G) * 0.35f, s_B + (1.0f - s_B) * 0.35f, s_FillA);
        s_Dl->AddRectFilledMultiColor(ImVec2(s_InnerL, s_InnerT), ImVec2(s_HeadX, s_InnerB), s_Dim, s_Bright, s_Bright, s_Dim);

        // Thin highlight along the top of the fill.
        s_Dl->AddRectFilled(ImVec2(s_InnerL, s_InnerT), ImVec2(s_HeadX, s_InnerT + s_Thick),
                            IM_COL32(255, 255, 255, static_cast<int>(70 * s_A)));
    }

    // Sparkle particles: spawn at the tip while Instinct is in use, drift up and fade out.
    if (s_Active && m_Meter > 0.004f) {
        m_EmitAcc += s_Dt * 60.0f;
        while (m_EmitAcc >= 1.0f) {
            m_EmitAcc -= 1.0f;
            if (m_Particles.size() >= 160) break;

            Particle s_P;
            s_P.x = s_HeadX + (Rand01() - 0.5f) * 2.0f * s_Ui;
            s_P.y = s_InnerT + Rand01() * (s_InnerB - s_InnerT);
            s_P.vx = (Rand01() - 0.5f) * 70.0f * s_Ui;
            s_P.vy = -(12.0f + 55.0f * Rand01()) * s_Ui;
            s_P.maxLife = 0.35f + 0.65f * Rand01();
            s_P.life = s_P.maxLife;
            s_P.size = (0.8f + 1.8f * Rand01()) * std::max(1.0f, s_Ui);
            m_Particles.push_back(s_P);
        }
    } else {
        m_EmitAcc = 0.0f;
    }

    for (auto& s_P : m_Particles) {
        s_P.life -= s_Dt;
        s_P.x += s_P.vx * s_Dt;
        s_P.y += s_P.vy * s_Dt;
        s_P.vy += 40.0f * s_Ui * s_Dt; // slight pull down so the sparks arc
    }
    m_Particles.erase(std::remove_if(m_Particles.begin(), m_Particles.end(), [](const Particle& p) { return p.life <= 0.0f; }),
                      m_Particles.end());

    for (const auto& s_P : m_Particles) {
        const float s_K = s_P.life / s_P.maxLife;
        s_Dl->AddCircleFilled(ImVec2(s_P.x, s_P.y), s_P.size * (0.5f + 0.5f * s_K),
                              s_Col(s_R + (1.0f - s_R) * 0.55f, s_G + (1.0f - s_G) * 0.55f, s_B + (1.0f - s_B) * 0.55f, s_K * s_A), 6);
    }

    // Bright spark at the tip of the fill (stronger while Instinct is in use).
    if (m_Meter > 0.004f) {
        const float s_Pulse = s_Active ? 0.85f + 0.15f * std::sin(s_Time * 14.0f) : 0.5f;
        const float s_Rad = s_H * (s_Active ? 0.95f : 0.7f);

        for (int i = 0; i < 4; ++i) { // soft glow
            const float s_Rr = s_Rad * (2.6f - 0.6f * static_cast<float>(i));
            s_Dl->AddCircleFilled(ImVec2(s_HeadX, s_MidY), s_Rr, s_Col(s_R, s_G, s_B, (0.06f + 0.05f * static_cast<float>(i)) * s_Pulse * s_A), 20);
        }
        s_Dl->AddCircleFilled(ImVec2(s_HeadX, s_MidY), s_Rad * 0.5f, s_Col(1.0f, 0.97f, 0.88f, 0.95f * s_Pulse * s_A), 12);

        const float s_Len = s_Rad * (2.3f + (s_Active ? 0.6f * std::sin(s_Time * 10.0f) : 0.0f));
        const ImU32 s_Star = s_Col(1.0f, 0.97f, 0.88f, 0.8f * s_Pulse * s_A);
        s_Dl->AddLine(ImVec2(s_HeadX - s_Len, s_MidY), ImVec2(s_HeadX + s_Len, s_MidY), s_Star, s_Thick);
        s_Dl->AddLine(ImVec2(s_HeadX, s_MidY - s_Len * 0.8f), ImVec2(s_HeadX, s_MidY + s_Len * 0.8f), s_Star, s_Thick);
    }

    // Point shooting: one pip per possible target below the bar, filled for every tagged one.
    if (m_PsState.load() != PS_IDLE) {
        const int s_MaxTags = std::max(1, m_PsMaxTags);
        const int s_Tagged = m_PsTagCount.load();
        const float s_PipR = std::max(2.0f, 3.5f * s_Ui);
        const float s_Gap = s_PipR * 3.0f;
        const float s_TotalW = s_Gap * static_cast<float>(s_MaxTags - 1);
        const float s_PipY = s_Bottom + s_PipR * 2.6f;

        for (int i = 0; i < s_MaxTags; ++i) {
            const ImVec2 s_C(s_Cx - s_TotalW * 0.5f + s_Gap * static_cast<float>(i), s_PipY);
            if (i < s_Tagged) {
                s_Dl->AddCircleFilled(s_C, s_PipR * 1.9f, s_Col(1.0f, 0.78f, 0.28f, 0.18f * s_A), 14);
                s_Dl->AddCircleFilled(s_C, s_PipR, s_Col(1.0f, 0.86f, 0.45f, 0.95f * s_A), 12);
            } else {
                s_Dl->AddCircle(s_C, s_PipR, s_Col(1.0f, 0.9f, 0.65f, 0.35f * s_A), 12, s_Thick);
            }
        }
    }

    // Thin outline, a little brighter while Instinct is active.
    const float s_Border = s_Active ? 0.55f + 0.2f * std::sin(s_Time * 6.0f) : 0.35f;
    s_Dl->AddRect(ImVec2(s_Left, s_Top), ImVec2(s_Right, s_Bottom), s_Col(1.0f, 0.9f, 0.65f, s_Border * s_A), 0.0f, 0, s_Thick);
}

// ---------------------------------------------------------------------------
// UI
// ---------------------------------------------------------------------------

void BetterInstinct::OnDrawMenu() {
    if (ImGui::Button("BETTER INSTINCT")) {
        m_ShowUI = !m_ShowUI;
    }
}

void BetterInstinct::OnDrawUI(bool p_HasFocus) {
    DrawHud();
    DrawPsOverlay();
    DrawPsPrompt();
    UpdateSound(m_LastHudDt, m_InPlay);

    // A file picked in the "Browse..." dialog (it runs on its own thread): use it as the custom sound and save.
    if (g_BrowseReady.exchange(false)) {
        std::wstring s_Picked;
        {
            std::lock_guard<std::mutex> s_Lock(g_BrowseMutex);
            s_Picked = g_BrowseResult;
        }
        const std::string s_Utf8 = WideToUtf8(s_Picked);
        if (!s_Utf8.empty() && s_Utf8.size() < sizeof(m_CustomPath)) {
            std::snprintf(m_CustomPath, sizeof(m_CustomPath), "%s", s_Utf8.c_str());
            m_SoundSource = 1;
            CloseSound();
            m_SoundStatus.clear();
            SaveSettings();
        } else if (!s_Utf8.empty()) {
            m_SoundStatus = "That file path is too long.";
        }
    }

    // Safety net: save everything once more whenever the settings window is closed.
    static bool s_WasOpen = false;
    if (m_ShowUI) {
        s_WasOpen = true;
    } else if (s_WasOpen) {
        s_WasOpen = false;
        SaveSettings();
    }

    // Outside a mission the game loop doesn't run, so never leave the game-level block switched on.
    if (!m_InPlay) {
        m_InputBlock = false;
    }

    // Action names are only recorded while the Debug section is open (set again below if it is).
    m_CaptureNames = false;

    if (!m_ShowUI) {
        m_Binding = false;
        m_BindingPs = false;
        m_ForceBlockTest = false;
        if (m_Testing) {
            m_Testing = false;
        }
        return;
    }

    bool s_Changed = false;

    ImGui::SetNextWindowSize(ImVec2(420, 0), ImGuiCond_FirstUseEver);
    if (ImGui::Begin("Better Instinct", &m_ShowUI)) {
        // --- Key binding ---
        ImGui::TextUnformatted("Instinct key (set this to your in-game Instinct bind)");
        if (m_Binding) {
            ImGui::Button("Press a key or mouse button... (Esc cancels)", ImVec2(-1, 0));

            bool s_AnyDown = false;
            for (int vk = 1; vk < 255; ++vk) {
                if (vk >= VK_SHIFT && vk <= VK_MENU) continue; // generic modifiers; L/R variants are captured instead
                if (IsKeyDown(vk)) { s_AnyDown = true; break; }
            }

            if (!m_BindArmed) {
                m_BindArmed = !s_AnyDown; // wait for the click that started binding to be released
            } else {
                for (int vk = 1; vk < 255; ++vk) {
                    if (vk >= VK_SHIFT && vk <= VK_MENU) continue;
                    if (!IsKeyDown(vk)) continue;

                    if (vk != VK_ESCAPE) {
                        m_InstinctKey = vk;
                        s_Changed = true;
                    }
                    m_Binding = false;
                    break;
                }
            }
        } else {
            const std::string s_Text = KeyName(m_InstinctKey) + "  (click to change)";
            if (ImGui::Button(s_Text.c_str(), ImVec2(-1, 0))) {
                m_Binding = true;
                m_BindArmed = false;
            }
        }

        int s_Mode = m_ToggleMode ? 1 : 0;
        if (ImGui::RadioButton("Hold", &s_Mode, 0)) { m_ToggleMode = false; s_Changed = true; }
        ImGui::SameLine();
        if (ImGui::RadioButton("Toggle", &s_Mode, 1)) { m_ToggleMode = true; s_Changed = true; }
        ImGui::SameLine();
        ImGui::TextDisabled("(match your in-game Instinct setting)");

        if (ImGui::Checkbox("Block Instinct when the meter is too low", &m_BlockEnabled)) s_Changed = true;
        if (ImGui::Checkbox("Hip-fire assist", &m_HipAssist)) s_Changed = true;
        ImGui::TextDisabled("During Instinct, left clicks are held until they lasted this long in GAME time, so un-aimed shots register.");
        ImGui::BeginDisabled(!m_HipAssist);
        if (ImGui::SliderInt("Click length (game time)", &m_HipGameMs, 0, 600, "%d ms")) s_Changed = true;
        ImGui::TextDisabled("At 0.25x time scale a 200 ms click is held for about 800 ms of real time. Raise it if shots still get lost.");
        ImGui::EndDisabled();
        ImGui::TextDisabled(g_HookOk.load() ? "Input guard: active" : "Input guard: NOT active (blocking unavailable)");

        if (ImGui::InputText("Instinct action name(s)", m_ActionPatterns, sizeof(m_ActionPatterns))) {
            RebuildPatterns();
            s_Changed = true;
        }
        ImGui::TextDisabled("Comma-separated, matches part of the game's action name. Find it in Debug below.");

        if (ImGui::CollapsingHeader("Debug")) {
            m_CaptureNames = true;

            ImGui::Text("Weapon in hand: %s   (the game polled it %u times)", HasWeaponInHand() ? "YES" : "no", m_WeaponPolls.load());
            ImGui::Text("Real shots: fired=%u  hit=%u  missed=%u  last result=%d  FireProjectiles returned=%s", m_PsShotsFired.load(),
                        m_PsShotsHit.load(), m_PsShotsMissed.load(), m_PsLastShotResult.load(), m_PsLastFireOk.load() ? "true" : "false");

            {
                const int64_t s_DbgNow = std::chrono::steady_clock::now().time_since_epoch().count();
                const long long s_DbgAge = m_WeaponPolls.load() == 0 ? -1 : std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::steady_clock::duration(s_DbgNow - m_HeldWeaponTick.load())).count();
                if (s_DbgAge < 0) {
                    ImGui::TextDisabled("Weapon polls: none yet (draw a weapon in a mission and watch the count)");
                } else {
                    ImGui::Text("Last weapon poll: %lld ms ago   (counts as in hand for %.0f ms)", s_DbgAge,
                                static_cast<double>(m_PsWeaponWindow) * 1000.0);
                }
                ImGui::Text("Last real shot: magazine %d -> %d   bullet origin=%s   distance=%.1f m   direct-kill fallbacks=%u",
                            m_PsDbgMagBefore.load(), m_PsDbgMagAfter.load(), m_PsDbgOrigin.load() == 1 ? "camera side" : "muzzle",
                            m_PsDbgDist.load(), m_PsFallbackKills.load());
                ImGui::TextDisabled("Last result: 0 = could not fire, 1 = fired, 2 = magazine empty. No recoil/animation is played.");
            }

            ImGui::Text("Game updates seen: %u", m_FrameCount.load());
            ImGui::Text("Instinct key down:  Windows=%s  hook=%s", AsyncKeyDown(m_InstinctKey) ? "YES" : "no", HookKeyDown(m_InstinctKey) ? "YES" : "no");
            ImGui::Text("Hook events: keyboard=%u  mouse=%u  last key=0x%02X", g_KbEvents.load(), g_MouseEvents.load(), g_LastVk.load());
            ImGui::Text("Active=%s  Blocked=%s  Locked=%s  NeedRelease=%s", m_Active.load() ? "yes" : "no", m_Blocked ? "yes" : "no",
                        m_Locked ? "yes" : "no", m_NeedRelease ? "yes" : "no");
            ImGui::Text("Meter=%.2f  time scale=%.2f", m_Meter, m_CurrentScale);
            ImGui::Text("Tint blend=%.2f  multiplier=(%.2f, %.2f, %.2f)  applied=%s  engine hook=%s", m_TintBlend.load(),
                        m_TintMultiplier[0], m_TintMultiplier[1], m_TintMultiplier[2], m_TintApplied ? "yes" : "no",
                        m_TintHookReady ? "connected" : "not connected");
            ImGui::Text("Game-level block: %s   blocked polls=%u", m_InputBlock.load() ? "ON" : "off", m_BlockedPolls.load());
            ImGui::TextDisabled("Updates stay at 0 outside a mission. Hold your Instinct key in a mission and watch these.");

            ImGui::Checkbox("Test: block matching actions now", &m_ForceBlockTest);
            ImGui::TextDisabled("With this on, Instinct should stop working in game. If it doesn't, pick another action name.");

            ImGui::InputText("Filter action names", m_NameFilter, sizeof(m_NameFilter));
            if (ImGui::Button("Clear list")) {
                std::lock_guard<std::mutex> s_Lock(m_NameMutex);
                m_SeenNames.clear();
            }
            ImGui::SameLine();
            ImGui::TextDisabled("Click a name to use it as the Instinct action");

            std::string s_Picked;
            if (ImGui::BeginChild("SeenActions", ImVec2(0, 150), true)) {
                std::lock_guard<std::mutex> s_Lock(m_NameMutex);
                for (const auto& s_SeenName : m_SeenNames) {
                    if (m_NameFilter[0] && !ContainsNoCase(s_SeenName.c_str(), [&]() {
                            std::string s_F = m_NameFilter;
                            std::transform(s_F.begin(), s_F.end(), s_F.begin(),
                                           [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
                            return s_F;
                        }())) {
                        continue;
                    }
                    if (ImGui::Selectable(s_SeenName.c_str())) {
                        s_Picked = s_SeenName;
                    }
                }
            }
            ImGui::EndChild();

            if (!s_Picked.empty()) {
                std::snprintf(m_ActionPatterns, sizeof(m_ActionPatterns), "%s", s_Picked.c_str());
                RebuildPatterns();
                s_Changed = true;
            }
        } else {
            m_ForceBlockTest = false;
        }

        ImGui::Separator();

        // --- Point shooting ---
        if (ImGui::Checkbox("Point shooting (Absolution / Dead Eye style)", &m_PsEnabled)) s_Changed = true;
        ImGui::BeginDisabled(!m_PsEnabled);
        ImGui::TextUnformatted("Point shooting key");
        if (DrawKeyBinder("psbind", m_PsKey, m_BindingPs, m_BindArmedPs)) s_Changed = true;
        ImGui::TextDisabled("Hold it: time almost stops and you can tag enemies. Release it: all tagged enemies get shot.");

        int s_TagMode = m_PsTagMode;
        ImGui::TextUnformatted("Tagging");
        if (ImGui::RadioButton("Click to tag (left mouse)", &s_TagMode, 0)) { m_PsTagMode = 0; s_Changed = true; }
        ImGui::SameLine();
        if (ImGui::RadioButton("Hover to tag", &s_TagMode, 1)) { m_PsTagMode = 1; s_Changed = true; }
        if (m_PsTagMode == 1) {
            if (ImGui::SliderFloat("Hover time", &m_PsHoverDwell, 0.05f, 1.5f, "%.2f s")) s_Changed = true;
            if (ImGui::Checkbox("Left click doesn't fire while point shooting", &m_PsBlockFire)) s_Changed = true;
        }
        if (ImGui::SliderFloat("Pickup radius", &m_PsSnapRadius, 0.02f, 0.30f, "%.2f of screen height")) s_Changed = true;
        if (ImGui::SliderFloat("Max distance", &m_PsMaxDist, 10.0f, 200.0f, "%.0f m")) s_Changed = true;

        int s_Part = m_PsAimPart;
        ImGui::TextUnformatted("Aim at");
        if (ImGui::RadioButton("Head", &s_Part, 0)) { m_PsAimPart = 0; s_Changed = true; }
        ImGui::SameLine();
        if (ImGui::RadioButton("Torso", &s_Part, 1)) { m_PsAimPart = 1; s_Changed = true; }

        if (ImGui::Checkbox("Tag the exact body part under the crosshair", &m_PsPrecise)) s_Changed = true;
        ImGui::TextDisabled("On: aim at a leg, arm, head... and the shot goes exactly there. Off: always head or torso.");

        if (ImGui::Checkbox("Require a weapon in hand", &m_PsRequireWeapon)) s_Changed = true;
        static const char* const s_ShotModes[] = {"Real shots from my weapon (uses ammo)", "Direct kill (no bullets)"};
        if (ImGui::Combo("Shot type", &m_PsShotMode, s_ShotModes, 2)) s_Changed = true;
        if (m_PsShotMode == 0) {
            if (ImGui::Checkbox("If a bullet misses, finish the target", &m_PsMissFinish)) s_Changed = true;
        }
        if (ImGui::Checkbox("Show the point shooting key prompt", &m_PsPrompt)) s_Changed = true;

        if (ImGui::SliderFloat("Time scale while aiming", &m_PsScale, 0.02f, 0.95f, "%.2fx")) s_Changed = true;
        if (ImGui::SliderFloat("Time scale while shooting", &m_PsExecScale, 0.02f, 1.0f, "%.2fx")) s_Changed = true;
        if (ImGui::SliderFloat("Meter per tagged target", &m_PsTagCost, 0.0f, 0.5f, "%.2f")) s_Changed = true;
        if (ImGui::SliderInt("Max targets", &m_PsMaxTags, 1, 12)) s_Changed = true;
        if (ImGui::SliderFloat("Meter drain while aiming", &m_PsDrainMul, 0.0f, 2.0f, "%.2fx")) s_Changed = true;

        static const char* const s_OrderNames[] = {"In the order they were tagged", "Nearest first"};
        if (ImGui::Combo("Shooting order", &m_PsOrder, s_OrderNames, 2)) s_Changed = true;
        if (ImGui::SliderFloat("Aim snap time", &m_PsSnapTime, 0.02f, 0.6f, "%.2f s")) s_Changed = true;
        if (ImGui::SliderFloat("Pause after each shot", &m_PsShotDelay, 0.05f, 1.0f, "%.2f s")) s_Changed = true;

        if (ImGui::Checkbox("Tag and shot sounds", &m_PsSoundOn)) s_Changed = true;
        ImGui::TextDisabled("Optional: put PointTag.wav and PointShot.wav in the InstinctSounds folder.");

        if (ImGui::TreeNode("Point shooting calibration")) {
            ImGui::TextDisabled("If the markers sit at the wrong height, adjust these (meters above the enemy's feet).");
            if (ImGui::SliderFloat("Head height", &m_PsHeadHeight, 0.3f, 2.5f, "%.2f m")) s_Changed = true;
            if (ImGui::SliderFloat("Torso height", &m_PsTorsoHeight, 0.3f, 2.5f, "%.2f m")) s_Changed = true;
            if (ImGui::Checkbox("World up axis is Y (try this if markers float sideways)", &m_PsYUp)) s_Changed = true;

            ImGui::Separator();
            ImGui::TextDisabled("Real shots: if bullets fly the wrong way, try another muzzle direction.");
            static const char* const s_Axes[] = {"-Y (engine forward, default)", "+Y (inverted)", "-Z", "+Z", "+X", "-X"};
            if (ImGui::Combo("Muzzle direction", &m_PsMuzzleAxis, s_Axes, 6)) s_Changed = true;
            if (ImGui::Checkbox("Show the barrel direction on screen", &m_PsShowMuzzle)) s_Changed = true;
            ImGui::TextDisabled("Hold your weapon and look for the cyan MUZZLE line: it must run out of the barrel. If not, change the direction above.");
            static const char* const s_Origins[] = {"Weapon muzzle", "Camera side, only if the muzzle is blocked", "Always camera side"};
            if (ImGui::Combo("Bullet origin", &m_PsOriginMode, s_Origins, 3)) s_Changed = true;
            ImGui::TextDisabled("Use the camera side if bullets die against cover right in front of the gun. Every tagged point is shot, one bullet each.");
            if (ImGui::Checkbox("Prepare the weapon for one bullet before firing", &m_PsPrime)) s_Changed = true;
            if (ImGui::SliderFloat("Weapon detection window", &m_PsWeaponWindow, 0.2f, 5.0f, "%.1f s")) s_Changed = true;
            ImGui::TreePop();
        }
        ImGui::EndDisabled();

        ImGui::Separator();

        // --- Slowdown & meter ---
        if (ImGui::SliderFloat("Time scale", &m_FocusScale, 0.05f, 0.95f, "%.2fx")) s_Changed = true;
        if (ImGui::SliderFloat("Transition speed", &m_BlendSpeed, 1.0f, 30.0f, "%.1f")) s_Changed = true;
        if (ImGui::SliderFloat("Drain time (full to empty)", &m_DrainSeconds, 2.0f, 60.0f, "%.0f s")) s_Changed = true;
        if (ImGui::SliderFloat("Recharge time (empty to full)", &m_RechargeSeconds, 2.0f, 120.0f, "%.0f s")) s_Changed = true;
        if (ImGui::SliderFloat("Recharge delay", &m_RechargeDelay, 0.0f, 5.0f, "%.1f s")) s_Changed = true;

        if (ImGui::Button("Refill meter")) {
            m_Meter = 1.0f;
            m_Locked = false;
        }

        ImGui::Separator();

        // --- Instinct color tint ---
        if (ImGui::Checkbox("Tint the world while Instinct is active", &m_TintEnabled)) s_Changed = true;
        ImGui::BeginDisabled(!m_TintEnabled);
        if (ImGui::ColorEdit3("Instinct tint", m_InstinctTint)) s_Changed = true;
        if (ImGui::SliderFloat("Tint strength", &m_TintStrength, 0.0f, 1.0f, "%.2f")) s_Changed = true;
        if (ImGui::Button("Reset tint")) {
            m_InstinctTint[0] = 0.30f;
            m_InstinctTint[1] = 0.70f;
            m_InstinctTint[2] = 1.00f;
            m_TintStrength = 0.70f;
            s_Changed = true;
        }
        ImGui::EndDisabled();
        ImGui::TextDisabled(m_TintHookReady ? "World tint: connected to the game's color grading"
                                            : "World tint: engine hook not connected yet (see ApplyTint in the source)");

        ImGui::Separator();

        // --- HUD ---
        if (ImGui::Checkbox("Show Instinct meter", &m_HudEnabled)) s_Changed = true;
        ImGui::BeginDisabled(!m_HudEnabled);
        if (ImGui::Checkbox("Always visible (even when full)", &m_HudAlways)) s_Changed = true;
        if (ImGui::SliderFloat("Width", &m_HudWidth, 0.05f, 0.5f, "%.2f")) s_Changed = true;
        if (ImGui::SliderFloat("Height from bottom", &m_HudYOffset, 0.0f, 400.0f, "%.0f px")) s_Changed = true;
        if (ImGui::SliderFloat("Size", &m_HudScale, 0.5f, 3.0f, "%.2f")) s_Changed = true;
        if (ImGui::SliderFloat("Opacity", &m_HudOpacity, 0.1f, 1.0f, "%.2f")) s_Changed = true;
        if (ImGui::ColorEdit3("Meter color", m_FillColor)) s_Changed = true;
        if (ImGui::ColorEdit3("Low-meter color", m_LowColor)) s_Changed = true;
        ImGui::EndDisabled();

        ImGui::Separator();

        // --- Sound ---
        if (ImGui::Checkbox("Instinct sound", &m_SoundEnabled)) {
            s_Changed = true;
            if (!m_SoundEnabled) m_Testing = false;
        }

        ImGui::BeginDisabled(!m_SoundEnabled);
        static const char* const s_SourceNames[] = {"Preset / folder", "Custom file"};
        if (ImGui::Combo("Source", &m_SoundSource, s_SourceNames, 2)) {
            s_Changed = true;
            CloseSound();
        }

        ImGui::BeginDisabled(g_BrowseBusy.load());
        if (ImGui::Button(g_BrowseBusy.load() ? "Choose a file in the dialog..." : "Browse for your own sound...")) {
            std::wstring s_StartDir = m_SoundDir;
            if (m_CustomPath[0]) {
                std::error_code s_BrowseEc;
                const fs::path s_Parent = fs::path(Utf8ToWide(m_CustomPath)).parent_path();
                if (!s_Parent.empty() && fs::is_directory(s_Parent, s_BrowseEc)) s_StartDir = s_Parent.wstring();
            }
            StartBrowse(s_StartDir);
        }
        ImGui::EndDisabled();
        ImGui::SameLine();
        ImGui::TextDisabled(".wav / .mp3");

        if (m_SoundSource == 0) {
            const char* s_Current = (m_PresetIndex >= 0 && m_PresetIndex < static_cast<int>(m_PresetFiles.size()))
                                        ? m_PresetFiles[m_PresetIndex].c_str() : "(no sounds found)";
            if (ImGui::BeginCombo("Sound", s_Current)) {
                for (int i = 0; i < static_cast<int>(m_PresetFiles.size()); ++i) {
                    if (ImGui::Selectable(m_PresetFiles[i].c_str(), i == m_PresetIndex)) {
                        m_PresetIndex = i;
                        m_PresetName = m_PresetFiles[i];
                        CloseSound();
                        s_Changed = true;
                    }
                }
                ImGui::EndCombo();
            }
            if (ImGui::Button("Rescan folder")) {
                ScanSounds();
                CloseSound();
            }
            ImGui::SameLine();
            ImGui::TextDisabled("mods\\InstinctSounds\\ (.wav / .mp3)");
        } else {
            if (ImGui::InputText("File path", m_CustomPath, sizeof(m_CustomPath))) {
                CloseSound();
                s_Changed = true;
            }
            ImGui::TextDisabled("Chosen with Browse, or paste the full path to a .wav or .mp3 file");
        }

        if (ImGui::SliderFloat("Volume", &m_SoundVolume, 0.0f, 1.0f, "%.2f")) {
            ApplySoundVolume();
            s_Changed = true;
        }

        ImGui::TextUnformatted("When instinct is released:");
        int s_Release = m_FadeOutOn ? 1 : 0;
        if (ImGui::RadioButton("Stop instantly", &s_Release, 0)) { m_FadeOutOn = false; s_Changed = true; }
        ImGui::SameLine();
        if (ImGui::RadioButton("Fade out", &s_Release, 1)) { m_FadeOutOn = true; s_Changed = true; }
        if (m_FadeOutOn) {
            if (ImGui::SliderFloat("Fade-out (ms)", &m_FadeOutMs, 50.0f, 3000.0f, "%.0f")) s_Changed = true;
        }

        if (ImGui::Checkbox("Fade in on activate", &m_FadeInOn)) s_Changed = true;
        if (m_FadeInOn) {
            if (ImGui::SliderFloat("Fade-in (ms)", &m_FadeInMs, 50.0f, 3000.0f, "%.0f")) s_Changed = true;
        }

        if (!m_Testing) {
            if (ImGui::Button("Test sound")) m_Testing = true;
        } else {
            if (ImGui::Button("Stop test")) m_Testing = false;
        }
        if (!m_SoundStatus.empty()) {
            ImGui::TextColored(ImVec4(1.0f, 0.6f, 0.2f, 1.0f), "%s", m_SoundStatus.c_str());
        }
        ImGui::EndDisabled();
    }
    ImGui::End();

    if (s_Changed) {
        SaveSettings();
    }
}

DEFINE_ZHM_PLUGIN(BetterInstinct)
