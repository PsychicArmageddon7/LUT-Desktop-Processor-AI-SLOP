#include <windows.h>
#include <d3d11.h>
#include <dxgi1_2.h>
#include <d3dcompiler.h>
#include <wrl/client.h>
#include <commdlg.h>
#include <shellapi.h>
#include <timeapi.h> // Required for high-precision multimedia timer
#include <iostream>
#include <fstream>
#include <sstream>
#include <vector>
#include <string>
#include <chrono>
#include <thread>
#include <algorithm>

// Ensure Visual Studio treats this as a Windows App to avoid the LNK2019 "main" error
#pragma comment(linker, "/SUBSYSTEM:windows /ENTRY:mainCRTStartup")

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "d3dcompiler.lib")
#pragma comment(lib, "comdlg32.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "winmm.lib") // Required for timeBeginPeriod / timeEndPeriod

using Microsoft::WRL::ComPtr;

// Custom Window Messages and Menu IDs for the System Tray
#define WM_TRAYICON (WM_USER + 1)
#define ID_TRAY_CHANGE_LUT   1001
#define ID_TRAY_CLEAR_LUT    1002
#define ID_TRAY_PAUSE_RESUME 1003
#define ID_TRAY_EXIT         1004

// Custom Menu IDs for Framerate Options 
#define ID_TRAY_FPS_60       2001
#define ID_TRAY_FPS_90       2002
#define ID_TRAY_FPS_120      2003
#define ID_TRAY_FPS_165      2004
#define ID_TRAY_FPS_240      2005
#define ID_TRAY_FPS_UNCAPPED 2006

// Custom Menu IDs for Resolution & Scaling Options
#define ID_TRAY_RES_NATIVE   3001
#define ID_TRAY_RES_1080P    3002
#define ID_TRAY_RES_1440P    3003
#define ID_TRAY_RES_4K       3004
#define ID_TRAY_RES_720P     3005
#define ID_TRAY_RES_CUSTOM   3006
#define ID_TRAY_SCALE_TOGGLE 3007

enum FrameRateCap {
    CAP_60 = 60,
    CAP_90 = 90,
    CAP_120 = 120,
    CAP_165 = 165,
    CAP_240 = 240,
    CAP_UNCAPPED = 0
};

// Structure to pass state between the Window UI processing and our Graphics Loop
struct AppState {
    bool requiresLutUpdate = false;
    std::string newLutPath = "";
    bool isPaused = false;
    FrameRateCap fpsCap = CAP_120; // Default framerate limit

    // Resolution & Scaling State Variables
    int targetWidth = 0;  // 0 represents Native Desktop resolution
    int targetHeight = 0; // 0 represents Native Desktop resolution
    bool scalingEnabled = true;
    bool requiresResUpdate = false;
};

// D3D11 Constant Buffer structure to handle texture mapping and offsetting
struct VS_CONSTANT_BUFFER {
    float scaleX, scaleY;
    float offsetX, offsetY;
};

const char* shaderCode = R"(
    cbuffer VS_CONSTANT_BUFFER : register(b0) {
        float scaleX;
        float scaleY;
        float offsetX;
        float offsetY;
    };

    Texture2D screenTex : register(t0);
    Texture3D lutTex : register(t1); // Changed to Texture3D to support 3D LUT sampling
    SamplerState samp : register(s0);

    struct VS_OUTPUT {
        float4 Pos : SV_POSITION;
        float2 Tex : TEXCOORD0;
    };

    VS_OUTPUT VS(uint id : SV_VertexID) {
        VS_OUTPUT output;
        float2 baseTex = float2((id << 1) & 2, id & 2);
        
        // Dynamically scale/offset texture coordinates to prevent squishing when scaling is disabled
        output.Tex = baseTex * float2(scaleX, scaleY) + float2(offsetX, offsetY);
        output.Pos = float4(baseTex * float2(2, -2) + float2(-1, 1), 0, 1);
        return output;
    }

    float4 PS(VS_OUTPUT input) : SV_Target {
        float4 color = screenTex.Sample(samp, input.Tex);
        
        // Sampling coordinates mapped directly to 3D color lookup coordinates
        float3 gradedColor = lutTex.Sample(samp, color.rgb).rgb;
        
        return float4(gradedColor, color.a);
    }
)";

struct float4_color { float r, g, b, a; };

// Helper to strip leading/trailing whitespace to avoid parser failure on indented comments
std::string Trim(const std::string& str) {
    size_t first = str.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) return "";
    size_t last = str.find_last_not_of(" \t\r\n");
    return str.substr(first, (last - first + 1));
}

// Structure to coordinate custom resolution parameters inside the dialog message loop
struct CustomResParams {
    int width = 1920;
    int height = 1080;
    bool submitted = false;
};

// Message handler for the Custom Resolution modal pop-up
LRESULT CALLBACK CustomResDlgProc(HWND hwnd, UINT uMsg, WPARAM wParam, LPARAM lParam) {
    CustomResParams* params = (CustomResParams*)GetWindowLongPtr(hwnd, GWLP_USERDATA);

    switch (uMsg) {
    case WM_CREATE: {
        CREATESTRUCT* pCreate = (CREATESTRUCT*)lParam;
        SetWindowLongPtr(hwnd, GWLP_USERDATA, (LONG_PTR)pCreate->lpCreateParams);
        params = (CustomResParams*)pCreate->lpCreateParams;

        wchar_t wStr[16], hStr[16];
        swprintf_s(wStr, L"%d", params ? params->width : 1920);
        swprintf_s(hStr, L"%d", params ? params->height : 1080);

        // Standard Win32 modal dialog design
        CreateWindowW(L"STATIC", L"Width:", WS_VISIBLE | WS_CHILD | SS_LEFT, 20, 20, 50, 20, hwnd, NULL, NULL, NULL);
        HWND hWidthEdit = CreateWindowW(L"EDIT", wStr, WS_VISIBLE | WS_CHILD | WS_BORDER | ES_NUMBER, 80, 18, 100, 20, hwnd, (HMENU)101, NULL, NULL);

        CreateWindowW(L"STATIC", L"Height:", WS_VISIBLE | WS_CHILD | SS_LEFT, 20, 50, 50, 20, hwnd, NULL, NULL, NULL);
        HWND hHeightEdit = CreateWindowW(L"EDIT", hStr, WS_VISIBLE | WS_CHILD | WS_BORDER | ES_NUMBER, 80, 48, 100, 20, hwnd, (HMENU)102, NULL, NULL);

        CreateWindowW(L"BUTTON", L"OK", WS_VISIBLE | WS_CHILD | BS_DEFPUSHBUTTON, 40, 90, 60, 25, hwnd, (HMENU)IDOK, NULL, NULL);
        CreateWindowW(L"BUTTON", L"Cancel", WS_VISIBLE | WS_CHILD | BS_PUSHBUTTON, 110, 90, 60, 25, hwnd, (HMENU)IDCANCEL, NULL, NULL);

        SetFocus(hWidthEdit);
        return 0;
    }
    case WM_COMMAND: {
        int wmId = LOWORD(wParam);
        if (wmId == IDOK) {
            wchar_t wBuf[16], hBuf[16];
            GetDlgItemTextW(hwnd, 101, wBuf, 16);
            GetDlgItemTextW(hwnd, 102, hBuf, 16);
            if (params) {
                int w = _wtoi(wBuf);
                int h = _wtoi(hBuf);

                // Keep the custom values clamped to actual desktop display boundaries
                int scrW = GetSystemMetrics(SM_CXSCREEN);
                int scrH = GetSystemMetrics(SM_CYSCREEN);
                params->width = (w > scrW) ? scrW : ((w < 1) ? 1 : w);
                params->height = (h > scrH) ? scrH : ((h < 1) ? 1 : h);
                params->submitted = true;
            }
            DestroyWindow(hwnd);
        }
        else if (wmId == IDCANCEL) {
            DestroyWindow(hwnd);
        }
        return 0;
    }
    case WM_CLOSE: {
        DestroyWindow(hwnd);
        return 0;
    }
    }
    return DefWindowProc(hwnd, uMsg, wParam, lParam);
}

// Spawns modal window to retrieve width and height parameters from user
bool PromptCustomResolution(HWND parent, int& width, int& height) {
    char hInst; // Placeholder
    HINSTANCE hInstModule = GetModuleHandle(NULL);
    const wchar_t CLASS_NAME[] = L"CustomResDlgClass";

    WNDCLASSW wc = {};
    wc.lpfnWndProc = CustomResDlgProc;
    wc.hInstance = hInstModule;
    wc.lpszClassName = CLASS_NAME;
    wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    RegisterClassW(&wc);

    CustomResParams params;
    params.width = width;
    params.height = height;

    int scrW = GetSystemMetrics(SM_CXSCREEN);
    int scrH = GetSystemMetrics(SM_CYSCREEN);
    int dlgW = 210;
    int dlgH = 160;
    int dlgX = (scrW - dlgW) / 2;
    int dlgY = (scrH - dlgH) / 2;

    HWND hwndDlg = CreateWindowExW(
        WS_EX_DLGMODALFRAME | WS_EX_TOPMOST,
        CLASS_NAME, L"Custom Resolution",
        WS_VISIBLE | WS_POPUP | WS_CAPTION | WS_SYSMENU,
        dlgX, dlgY, dlgW, dlgH,
        parent, NULL, hInstModule, &params
    );

    if (parent) EnableWindow(parent, FALSE);

    MSG msg = {};
    while (IsWindow(hwndDlg)) {
        if (GetMessage(&msg, NULL, 0, 0)) {
            TranslateMessage(&msg);
            DispatchMessage(&msg);
        }
    }

    if (parent) {
        EnableWindow(parent, TRUE);
        SetForegroundWindow(parent);
    }

    UnregisterClassW(CLASS_NAME, hInstModule);

    if (params.submitted && params.width > 0 && params.height > 0) {
        width = params.width;
        height = params.height;
        return true;
    }
    return false;
}

// Hybrid-spin high precision sleep function for smooth high-framerate pacing
void PreciseSleep(double seconds) {
    static bool timerInitialized = false;
    if (!timerInitialized) {
        timeBeginPeriod(1); // Set Windows scheduler resolution to 1ms
        timerInitialized = true;
    }

    auto start = std::chrono::high_resolution_clock::now();
    double target_ms = seconds * 1000.0;

    while (true) {
        auto now = std::chrono::high_resolution_clock::now();
        double elapsed_ms = std::chrono::duration<double, std::milli>(now - start).count();
        double remaining_ms = target_ms - elapsed_ms;

        if (remaining_ms <= 0.0) {
            break;
        }

        if (remaining_ms > 1.5) {
            // Sleep coarse segment to save CPU cycles
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        else {
            // Spin-yield sub-millisecond segment for microsecond-level accuracy
            std::this_thread::yield();
        }
    }
}

// Unified parser that handles both 1D and 3D LUT formats
std::vector<float4_color> LoadUnifiedLUT(const std::string& filepath, int& outSize) {
    std::vector<float4_color> parsedLines;
    bool is3D = true;
    int parsedSize = 0;

    std::ifstream file(filepath);
    if (file.is_open()) {
        std::string rawLine;
        while (std::getline(file, rawLine)) {
            std::string line = Trim(rawLine);
            if (line.empty() || line[0] == '#') continue;

            std::stringstream ss(line);
            std::string token;
            ss >> token;

            if (token == "LUT_3D_SIZE") {
                ss >> parsedSize;
                is3D = true;
                continue;
            }
            if (token == "LUT_1D_SIZE") {
                ss >> parsedSize;
                is3D = false;
                continue;
            }
            if (isalpha(token[0])) continue;

            try {
                float r = std::stof(token);
                float g, b;
                if (ss >> g >> b) {
                    parsedLines.push_back({ r, g, b, 1.0f });
                }
            }
            catch (...) {}
        }
    }

    std::vector<float4_color> finalLut;

    // Fallback/Identity LUT creation (Size 33x33x33)
    if (parsedLines.empty() || parsedSize <= 0) {
        outSize = 33;
        finalLut.resize(outSize * outSize * outSize);
        int idx = 0;
        float div = static_cast<float>(outSize - 1);
        for (int b = 0; b < outSize; ++b) {
            for (int g = 0; g < outSize; ++g) {
                for (int r = 0; r < outSize; ++r) {
                    finalLut[idx++] = { r / div, g / div, b / div, 1.0f };
                }
            }
        }
        return finalLut;
    }

    if (is3D) {
        // Standard 3D layout: safety check to prevent reading past bounds in D3D11 if lines are missing
        int expectedSize = parsedSize * parsedSize * parsedSize;
        if (static_cast<int>(parsedLines.size()) < expectedSize) {
            parsedLines.resize(expectedSize, { 1.0f, 1.0f, 1.0f, 1.0f });
        }
        outSize = parsedSize;
        finalLut = std::move(parsedLines);
    }
    else {
        // Convert (Bake) 1D Curves into a 3D texture mapping at runtime
        outSize = 33;
        finalLut.resize(outSize * outSize * outSize);

        int idx = 0;
        float div = static_cast<float>(outSize - 1);

        // Safety: Use the actual number of parsed elements to calculate limits rather than the file header size
        int max1DIndex = static_cast<int>(parsedLines.size()) - 1;

        for (int b = 0; b < outSize; ++b) {
            for (int g = 0; g < outSize; ++g) {
                for (int r = 0; r < outSize; ++r) {
                    float u = r / div;
                    float v = g / div;
                    float w = b / div;

                    auto sample1D = [&](float val, int channel) -> float {
                        float exactIdx = val * max1DIndex;
                        int idx0 = static_cast<int>(exactIdx);
                        int idx1 = (idx0 < max1DIndex) ? idx0 + 1 : idx0;
                        float t = exactIdx - idx0;

                        float val0, val1;
                        if (channel == 0) { val0 = parsedLines[idx0].r; val1 = parsedLines[idx1].r; }
                        else if (channel == 1) { val0 = parsedLines[idx0].g; val1 = parsedLines[idx1].g; }
                        else { val0 = parsedLines[idx0].b; val1 = parsedLines[idx1].b; }

                        return val0 * (1.0f - t) + val1 * t;
                        };

                    float finalR = sample1D(u, 0);
                    float finalG = sample1D(v, 1);
                    float finalB = sample1D(w, 2);

                    finalLut[idx++] = { finalR, finalG, finalB, 1.0f };
                }
            }
        }
    }

    return finalLut;
}

std::string SelectLUTFile(HWND parent = NULL) {
    OPENFILENAMEA ofn;
    CHAR szFile[MAX_PATH] = { 0 };

    ZeroMemory(&ofn, sizeof(ofn));
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = parent;
    ofn.lpstrFile = szFile;
    ofn.nMaxFile = sizeof(szFile);
    ofn.lpstrFilter = "Cube LUT Files (*.cube)\0*.cube\0All Files (*.*)\0*.*\0";
    ofn.nFilterIndex = 1;
    ofn.Flags = OFN_PATHMUSTEXIST | OFN_FILEMUSTEXIST | OFN_NOCHANGEDIR;

    if (GetOpenFileNameA(&ofn) == TRUE) {
        return std::string(ofn.lpstrFile);
    }
    return "";
}

LRESULT CALLBACK WindowProc(HWND hwnd, UINT uMsg, WPARAM wParam, LPARAM lParam) {
    AppState* state = (AppState*)GetWindowLongPtr(hwnd, GWLP_USERDATA);

    switch (uMsg) {
    case WM_CREATE: {
        CREATESTRUCT* pCreate = (CREATESTRUCT*)lParam;
        SetWindowLongPtr(hwnd, GWLP_USERDATA, (LONG_PTR)pCreate->lpCreateParams);

        NOTIFYICONDATA nid = {};
        nid.cbSize = sizeof(nid);
        nid.hWnd = hwnd;
        nid.uID = 1;
        nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
        nid.uCallbackMessage = WM_TRAYICON;
        // Modified to load the custom embedded icon from resource ID 101
        nid.hIcon = LoadIcon(GetModuleHandle(NULL), MAKEINTRESOURCE(101));
        lstrcpy(nid.szTip, L"LUT Desktop Processor");
        Shell_NotifyIcon(NIM_ADD, &nid);
        return 0;
    }
    case WM_TRAYICON: {
        if (lParam == WM_RBUTTONUP) {
            POINT pt;
            GetCursorPos(&pt);
            HMENU hMenu = CreatePopupMenu();

            // LUT Settings
            InsertMenu(hMenu, -1, MF_BYPOSITION | MF_STRING, ID_TRAY_CHANGE_LUT, L"Change LUT...");
            InsertMenu(hMenu, -1, MF_BYPOSITION | MF_STRING, ID_TRAY_CLEAR_LUT, L"Clear LUT");
            InsertMenu(hMenu, -1, MF_SEPARATOR, 0, NULL);

            // Resolution Settings
            UINT flagsNative = (state && state->targetWidth == 0) ? (MF_STRING | MF_CHECKED) : (MF_STRING | MF_UNCHECKED);
            UINT flags1080 = (state && state->targetWidth == 1920 && state->targetHeight == 1080) ? (MF_STRING | MF_CHECKED) : (MF_STRING | MF_UNCHECKED);
            UINT flags1440 = (state && state->targetWidth == 2560 && state->targetHeight == 1440) ? (MF_STRING | MF_CHECKED) : (MF_STRING | MF_UNCHECKED);
            UINT flags4K = (state && state->targetWidth == 3840 && state->targetHeight == 2160) ? (MF_STRING | MF_CHECKED) : (MF_STRING | MF_UNCHECKED);
            UINT flags720 = (state && state->targetWidth == 1280 && state->targetHeight == 720) ? (MF_STRING | MF_CHECKED) : (MF_STRING | MF_UNCHECKED);

            // Determine if active resolution matches custom selection
            bool isCustom = false;
            if (state && state->targetWidth != 0) {
                int w = state->targetWidth;
                int h = state->targetHeight;
                if (!((w == 1920 && h == 1080) || (w == 2560 && h == 1440) || (w == 3840 && h == 2160) || (w == 1280 && h == 720))) {
                    isCustom = true;
                }
            }
            UINT flagsCustom = isCustom ? (MF_STRING | MF_CHECKED) : (MF_STRING | MF_UNCHECKED);

            InsertMenu(hMenu, -1, MF_BYPOSITION | flagsNative, ID_TRAY_RES_NATIVE, L"Resolution: Native Desktop");
            InsertMenu(hMenu, -1, MF_BYPOSITION | flags4K, ID_TRAY_RES_4K, L"Resolution: 3840 x 2160 (4K)");
            InsertMenu(hMenu, -1, MF_BYPOSITION | flags1440, ID_TRAY_RES_1440P, L"Resolution: 2560 x 1440 (1440p)");
            InsertMenu(hMenu, -1, MF_BYPOSITION | flags1080, ID_TRAY_RES_1080P, L"Resolution: 1920 x 1080 (1080p)");
            InsertMenu(hMenu, -1, MF_BYPOSITION | flags720, ID_TRAY_RES_720P, L"Resolution: 1280 x 720 (720p)");

            wchar_t customMenuText[64] = L"Resolution: Custom...";
            if (isCustom && state) {
                swprintf_s(customMenuText, L"Resolution: Custom (%d x %d)...", state->targetWidth, state->targetHeight);
            }
            InsertMenu(hMenu, -1, MF_BYPOSITION | flagsCustom, ID_TRAY_RES_CUSTOM, customMenuText);
            InsertMenu(hMenu, -1, MF_SEPARATOR, 0, NULL);

            // Scaling Toggle
            UINT flagsScale = (state && state->scalingEnabled) ? (MF_STRING | MF_CHECKED) : (MF_STRING | MF_UNCHECKED);
            InsertMenu(hMenu, -1, flagsScale, ID_TRAY_SCALE_TOGGLE, L"Enable Scaling / Stretching");
            InsertMenu(hMenu, -1, MF_SEPARATOR, 0, NULL);

            // Framerate Settings (Implemented directly alongside other settings with proper checkmarks)
            UINT flags60 = (state && state->fpsCap == CAP_60) ? (MF_STRING | MF_CHECKED) : (MF_STRING | MF_UNCHECKED);
            UINT flags90 = (state && state->fpsCap == CAP_90) ? (MF_STRING | MF_CHECKED) : (MF_STRING | MF_UNCHECKED);
            UINT flags120 = (state && state->fpsCap == CAP_120) ? (MF_STRING | MF_CHECKED) : (MF_STRING | MF_UNCHECKED);
            UINT flags165 = (state && state->fpsCap == CAP_165) ? (MF_STRING | MF_CHECKED) : (MF_STRING | MF_UNCHECKED);
            UINT flags240 = (state && state->fpsCap == CAP_240) ? (MF_STRING | MF_CHECKED) : (MF_STRING | MF_UNCHECKED);
            UINT flagsUncapped = (state && state->fpsCap == CAP_UNCAPPED) ? (MF_STRING | MF_CHECKED) : (MF_STRING | MF_UNCHECKED);

            InsertMenu(hMenu, -1, MF_BYPOSITION | flagsUncapped, ID_TRAY_FPS_UNCAPPED, L"Framerate: Uncapped");
            InsertMenu(hMenu, -1, MF_BYPOSITION | flags240, ID_TRAY_FPS_240, L"Framerate: 240 FPS");
            InsertMenu(hMenu, -1, MF_BYPOSITION | flags165, ID_TRAY_FPS_165, L"Framerate: 165 FPS");
            InsertMenu(hMenu, -1, MF_BYPOSITION | flags120, ID_TRAY_FPS_120, L"Framerate: 120 FPS");
            InsertMenu(hMenu, -1, MF_BYPOSITION | flags90, ID_TRAY_FPS_90, L"Framerate: 90 FPS");
            InsertMenu(hMenu, -1, MF_BYPOSITION | flags60, ID_TRAY_FPS_60, L"Framerate: 60 FPS");
            InsertMenu(hMenu, -1, MF_SEPARATOR, 0, NULL);

            // Playback Settings
            if (state && state->isPaused) {
                InsertMenu(hMenu, -1, MF_BYPOSITION | MF_STRING, ID_TRAY_PAUSE_RESUME, L"Resume Duplication");
            }
            else {
                InsertMenu(hMenu, -1, MF_BYPOSITION | MF_STRING, ID_TRAY_PAUSE_RESUME, L"Pause Duplication");
            }

            InsertMenu(hMenu, -1, MF_SEPARATOR, 0, NULL);
            InsertMenu(hMenu, -1, MF_BYPOSITION | MF_STRING, ID_TRAY_EXIT, L"Exit Application");

            SetForegroundWindow(hwnd);
            TrackPopupMenu(hMenu, TPM_BOTTOMALIGN | TPM_LEFTALIGN, pt.x, pt.y, 0, hwnd, NULL);
            DestroyMenu(hMenu);
        }
        return 0;
    }
    case WM_COMMAND: {
        int wmId = LOWORD(wParam);

        if (wmId == ID_TRAY_EXIT) {
            PostQuitMessage(0);
        }
        else if (wmId == ID_TRAY_CHANGE_LUT) {
            std::string path = SelectLUTFile(hwnd);
            if (!path.empty() && state != nullptr) {
                state->newLutPath = path;
                state->requiresLutUpdate = true;
            }
        }
        else if (wmId == ID_TRAY_CLEAR_LUT) {
            if (state != nullptr) {
                state->newLutPath = "";
                state->requiresLutUpdate = true;
            }
        }
        else if (wmId == ID_TRAY_PAUSE_RESUME) {
            if (state != nullptr) {
                state->isPaused = !state->isPaused;
                ShowWindow(hwnd, state->isPaused ? SW_HIDE : SW_SHOW);
            }
        }
        else if (wmId >= ID_TRAY_FPS_60 && wmId <= ID_TRAY_FPS_UNCAPPED) {
            if (state != nullptr) {
                switch (wmId) {
                case ID_TRAY_FPS_60:       state->fpsCap = CAP_60; break;
                case ID_TRAY_FPS_90:       state->fpsCap = CAP_90; break;
                case ID_TRAY_FPS_120:      state->fpsCap = CAP_120; break;
                case ID_TRAY_FPS_165:      state->fpsCap = CAP_165; break;
                case ID_TRAY_FPS_240:      state->fpsCap = CAP_240; break;
                case ID_TRAY_FPS_UNCAPPED: state->fpsCap = CAP_UNCAPPED; break;
                }
            }
        }
        else if (wmId >= ID_TRAY_RES_NATIVE && wmId <= ID_TRAY_RES_CUSTOM) {
            if (state != nullptr) {
                int oldW = state->targetWidth;
                int oldH = state->targetHeight;
                switch (wmId) {
                case ID_TRAY_RES_NATIVE:
                    state->targetWidth = 0;
                    state->targetHeight = 0;
                    break;
                case ID_TRAY_RES_1080P:
                    state->targetWidth = 1920;
                    state->targetHeight = 1080;
                    break;
                case ID_TRAY_RES_1440P:
                    state->targetWidth = 2560;
                    state->targetHeight = 1440;
                    break;
                case ID_TRAY_RES_4K:
                    state->targetWidth = 3840;
                    state->targetHeight = 2160;
                    break;
                case ID_TRAY_RES_720P:
                    state->targetWidth = 1280;
                    state->targetHeight = 720;
                    break;
                case ID_TRAY_RES_CUSTOM: {
                    int customW = 1920;
                    int customH = 1080;
                    if (state->targetWidth > 0) {
                        customW = state->targetWidth;
                        customH = state->targetHeight;
                    }
                    if (PromptCustomResolution(hwnd, customW, customH)) {
                        state->targetWidth = customW;
                        state->targetHeight = customH;
                    }
                    break;
                }
                }
                if (state->targetWidth != oldW || state->targetHeight != oldH) {
                    state->requiresResUpdate = true;
                }
            }
        }
        else if (wmId == ID_TRAY_SCALE_TOGGLE) {
            if (state != nullptr) {
                state->scalingEnabled = !state->scalingEnabled;
                state->requiresResUpdate = true;
            }
        }
        return 0;
    }
    case WM_DESTROY: {
        NOTIFYICONDATA nid = {};
        nid.cbSize = sizeof(nid);
        nid.hWnd = hwnd;
        nid.uID = 1;
        Shell_NotifyIcon(NIM_DELETE, &nid);
        PostQuitMessage(0);
        return 0;
    }
    }
    return DefWindowProc(hwnd, uMsg, wParam, lParam);
}

int main() {
    HINSTANCE hInstance = GetModuleHandle(NULL);
    int nCmdShow = SW_SHOW;

    std::string initialLutPath = SelectLUTFile();

    int width = GetSystemMetrics(SM_CXSCREEN);
    int height = GetSystemMetrics(SM_CYSCREEN);

    const wchar_t CLASS_NAME[] = L"LUTOverlayClass";
    WNDCLASS wc = { };
    wc.lpfnWndProc = WindowProc;
    wc.hInstance = hInstance;
    // Modified to load the custom embedded icon for the window class
    wc.hIcon = LoadIcon(hInstance, MAKEINTRESOURCE(101));
    wc.lpszClassName = CLASS_NAME;
    RegisterClass(&wc);

    AppState appState;

    HWND hwnd = CreateWindowEx(
        WS_EX_TOPMOST | WS_EX_LAYERED | WS_EX_TRANSPARENT,
        CLASS_NAME, L"LUT Desktop Processor",
        WS_POPUP,
        0, 0, width, height,
        NULL, NULL, hInstance, &appState
    );

    SetLayeredWindowAttributes(hwnd, 0, 255, LWA_ALPHA);
    SetWindowDisplayAffinity(hwnd, WDA_EXCLUDEFROMCAPTURE);
    ShowWindow(hwnd, nCmdShow);

    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;
    ComPtr<IDXGISwapChain1> swapChain;

    DXGI_SWAP_CHAIN_DESC1 sd = {};
    sd.Width = width;
    sd.Height = height;
    sd.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    sd.SampleDesc.Count = 1;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.BufferCount = 2;
    sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;

    ComPtr<IDXGIFactory2> dxgiFactory;
    CreateDXGIFactory1(IID_PPV_ARGS(&dxgiFactory));

    D3D_FEATURE_LEVEL featureLevel;
    D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT,
        nullptr, 0, D3D11_SDK_VERSION, &device, &featureLevel, &context);

    dxgiFactory->CreateSwapChainForHwnd(device.Get(), hwnd, &sd, nullptr, nullptr, &swapChain);

    ComPtr<ID3D11Texture2D> backBuffer;
    swapChain->GetBuffer(0, IID_PPV_ARGS(&backBuffer));
    ComPtr<ID3D11RenderTargetView> renderTargetView;
    device->CreateRenderTargetView(backBuffer.Get(), nullptr, &renderTargetView);

    ComPtr<IDXGIAdapter> adapter;
    dxgiFactory->EnumAdapters(0, &adapter);
    ComPtr<IDXGIOutput> output;
    adapter->EnumOutputs(0, &output);
    ComPtr<IDXGIOutput1> output1;
    output.As(&output1);

    ComPtr<IDXGIOutputDuplication> deskDupl;
    HRESULT hr = output1->DuplicateOutput(device.Get(), &deskDupl);
    if (FAILED(hr)) {
        MessageBox(NULL, L"Desktop Duplication failed. Make sure your games are set to Windowed/Borderless.", L"Error", MB_OK);
        return -1;
    }

    ComPtr<ID3DBlob> vsBlob, psBlob, errorBlob;
    D3DCompile(shaderCode, strlen(shaderCode), nullptr, nullptr, nullptr, "VS", "vs_5_0", 0, 0, &vsBlob, &errorBlob);
    D3DCompile(shaderCode, strlen(shaderCode), nullptr, nullptr, nullptr, "PS", "ps_5_0", 0, 0, &psBlob, &errorBlob);

    ComPtr<ID3D11VertexShader> vertexShader;
    ComPtr<ID3D11PixelShader> pixelShader;
    device->CreateVertexShader(vsBlob->GetBufferPointer(), vsBlob->GetBufferSize(), nullptr, &vertexShader);
    device->CreatePixelShader(psBlob->GetBufferPointer(), psBlob->GetBufferSize(), nullptr, &pixelShader);

    ComPtr<ID3D11SamplerState> samplerState;
    D3D11_SAMPLER_DESC sampDesc = {};
    sampDesc.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    sampDesc.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP;
    sampDesc.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP;
    sampDesc.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    device->CreateSamplerState(&sampDesc, &samplerState);

    // Dynamic constant buffer allocation to handle on-the-fly texture offsetting
    ComPtr<ID3D11Buffer> constantBuffer;
    D3D11_BUFFER_DESC cbDesc = {};
    cbDesc.ByteWidth = sizeof(VS_CONSTANT_BUFFER);
    cbDesc.Usage = D3D11_USAGE_DYNAMIC;
    cbDesc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    cbDesc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    device->CreateBuffer(&cbDesc, nullptr, &constantBuffer);
    context->VSSetConstantBuffers(0, 1, constantBuffer.GetAddressOf());

    ComPtr<ID3D11Texture3D> lutTexture; // Now 3D texture mapping
    ComPtr<ID3D11ShaderResourceView> lutSRV;

    auto UpdateLutTexture = [&](const std::string& path) {
        int lutSize = 0;
        std::vector<float4_color> lutData = LoadUnifiedLUT(path, lutSize);

        lutTexture.Reset();
        lutSRV.Reset();

        D3D11_TEXTURE3D_DESC lutDesc = {};
        lutDesc.Width = lutSize;
        lutDesc.Height = lutSize;
        lutDesc.Depth = lutSize;
        lutDesc.MipLevels = 1;
        lutDesc.Format = DXGI_FORMAT_R32G32B32A32_FLOAT;
        lutDesc.Usage = D3D11_USAGE_IMMUTABLE;
        lutDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE;

        D3D11_SUBRESOURCE_DATA lutSubData = {};
        lutSubData.pSysMem = lutData.data();
        lutSubData.SysMemPitch = lutSize * sizeof(float4_color);
        lutSubData.SysMemSlicePitch = lutSize * lutSize * sizeof(float4_color);

        device->CreateTexture3D(&lutDesc, &lutSubData, &lutTexture);
        device->CreateShaderResourceView(lutTexture.Get(), nullptr, &lutSRV);

        context->PSSetShaderResources(1, 1, lutSRV.GetAddressOf());
        };

    // Load initial user choice
    UpdateLutTexture(initialLutPath);

    context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    context->VSSetShader(vertexShader.Get(), nullptr, 0);
    context->PSSetShader(pixelShader.Get(), nullptr, 0);
    context->PSSetSamplers(0, 1, samplerState.GetAddressOf());

    D3D11_VIEWPORT vp = { 0.0f, 0.0f, (float)width, (float)height, 0.0f, 1.0f };
    context->RSSetViewports(1, &vp);

    auto lastFrameTime = std::chrono::high_resolution_clock::now();

    MSG msg = { };
    while (msg.message != WM_QUIT) {
        if (PeekMessage(&msg, NULL, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessage(&msg);
        }
        else {
            if (appState.requiresLutUpdate) {
                UpdateLutTexture(appState.newLutPath);
                appState.requiresLutUpdate = false;
            }

            // Resolution / Scaling Update logic
            if (appState.requiresResUpdate) {
                context->OMSetRenderTargets(0, nullptr, nullptr);
                renderTargetView.Reset();
                backBuffer.Reset();

                int activeWidth = (appState.targetWidth == 0) ? width : appState.targetWidth;
                int activeHeight = (appState.targetHeight == 0) ? height : appState.targetHeight;

                int swapChainW = width;
                int swapChainH = height;

                // Stretch Mode: Resize swapchain to render at target resolution
                // Center/Pixel-Perfect Mode: Swapchain stays at native to draw transparent borders
                if (appState.scalingEnabled) {
                    swapChainW = activeWidth;
                    swapChainH = activeHeight;
                }

                hr = swapChain->ResizeBuffers(2, swapChainW, swapChainH, DXGI_FORMAT_B8G8R8A8_UNORM, 0);
                if (SUCCEEDED(hr)) {
                    swapChain->GetBuffer(0, IID_PPV_ARGS(&backBuffer));
                    device->CreateRenderTargetView(backBuffer.Get(), nullptr, &renderTargetView);
                }
                appState.requiresResUpdate = false;
            }

            if (appState.isPaused) {
                Sleep(16); // Coarse 60fps sleep to save resources while paused
                continue;
            }

            // High Precision Framerate Control
            auto now = std::chrono::high_resolution_clock::now();
            double elapsedSeconds = std::chrono::duration<double>(now - lastFrameTime).count();

            double targetInterval = 0.0;
            if (appState.fpsCap != CAP_UNCAPPED) {
                targetInterval = 1.0 / static_cast<double>(appState.fpsCap);
            }

            if (elapsedSeconds < targetInterval) {
                double waitTime = targetInterval - elapsedSeconds;
                PreciseSleep(waitTime);
                continue; // Re-evaluate loop timing
            }

            lastFrameTime = std::chrono::high_resolution_clock::now();

            ComPtr<IDXGIResource> desktopResource;
            DXGI_OUTDUPL_FRAME_INFO frameInfo;

            // Set short timeout (1ms) to keep main thread responsive even under no screen changes
            hr = deskDupl->AcquireNextFrame(1, &frameInfo, &desktopResource);

            if (SUCCEEDED(hr)) {
                ComPtr<ID3D11Texture2D> desktopTexture;
                desktopResource.As(&desktopTexture);

                ComPtr<ID3D11ShaderResourceView> desktopSRV;
                device->CreateShaderResourceView(desktopTexture.Get(), nullptr, &desktopSRV);

                int activeWidth = (appState.targetWidth == 0) ? width : appState.targetWidth;
                int activeHeight = (appState.targetHeight == 0) ? height : appState.targetHeight;

                D3D11_VIEWPORT currentVp = {};
                VS_CONSTANT_BUFFER cbData = {};

                if (appState.scalingEnabled) {
                    currentVp.Width = (float)activeWidth;
                    currentVp.Height = (float)activeHeight;
                    currentVp.TopLeftX = 0.0f;
                    currentVp.TopLeftY = 0.0f;
                    currentVp.MinDepth = 0.0f;
                    currentVp.MaxDepth = 1.0f;

                    cbData.scaleX = 1.0f;
                    cbData.scaleY = 1.0f;
                    cbData.offsetX = 0.0f;
                    cbData.offsetY = 0.0f;
                }
                else {
                    currentVp.Width = (float)activeWidth;
                    currentVp.Height = (float)activeHeight;
                    currentVp.TopLeftX = (float)(width - activeWidth) / 2.0f;
                    currentVp.TopLeftY = (float)(height - activeHeight) / 2.0f;
                    currentVp.MinDepth = 0.0f;
                    currentVp.MaxDepth = 1.0f;

                    cbData.scaleX = (float)activeWidth / (float)width;
                    cbData.scaleY = (float)activeHeight / (float)height;
                    cbData.offsetX = currentVp.TopLeftX / (float)width;
                    cbData.offsetY = currentVp.TopLeftY / (float)height;
                }

                // Push dynamic coordinate scaling limits to Vertex Shader
                D3D11_MAPPED_SUBRESOURCE mappedResource;
                hr = context->Map(constantBuffer.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mappedResource);
                if (SUCCEEDED(hr)) {
                    memcpy(mappedResource.pData, &cbData, sizeof(VS_CONSTANT_BUFFER));
                    context->Unmap(constantBuffer.Get(), 0);
                }

                context->RSSetViewports(1, &currentVp);

                // Clear entire frame to alpha-transparent black to prevent trailing artifacts outside the viewport
                float clearColor[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
                context->ClearRenderTargetView(renderTargetView.Get(), clearColor);

                context->OMSetRenderTargets(1, renderTargetView.GetAddressOf(), nullptr);
                context->PSSetShaderResources(0, 1, desktopSRV.GetAddressOf());

                context->Draw(3, 0);

                ID3D11ShaderResourceView* nullSRV[1] = { nullptr };
                context->PSSetShaderResources(0, 1, nullSRV);

                // Present with no V-sync (0 interval) for high refresh support
                swapChain->Present(0, 0);
                deskDupl->ReleaseFrame();
            }
            else if (hr == DXGI_ERROR_WAIT_TIMEOUT) {
                // No update occurred; no frame was drawn, which saves active GPU utilization
            }
            else if (hr == DXGI_ERROR_ACCESS_LOST) {
                break;
            }
        }
    }

    // Clean up multimedia scheduler resolution settings upon exit
    timeEndPeriod(1);

    return 0;
}