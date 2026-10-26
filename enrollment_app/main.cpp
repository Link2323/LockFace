#include <windows.h>
#include <shellapi.h>
#include <shlobj.h>
#include <string>
#include <fstream>
#include "WebviewHost.h"
#include "EnrollmentWizard.h"
#include "../common/logger.h"
#include "../common/registry_util.h"

// ============================================================================
// Check admin elevation; re-launch if needed
// ============================================================================
static bool EnsureAdmin() {
    BOOL isElevated = FALSE;
    HANDLE hToken = nullptr;
    if (OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &hToken)) {
        TOKEN_ELEVATION elevation;
        DWORD size = sizeof(TOKEN_ELEVATION);
        if (GetTokenInformation(hToken, TokenElevation, &elevation, size, &size))
            isElevated = elevation.TokenIsElevated;
        CloseHandle(hToken);
    }
    if (!isElevated) {
        wchar_t exePath[MAX_PATH];
        GetModuleFileNameW(nullptr, exePath, MAX_PATH);
        SHELLEXECUTEINFOW sei = {};
        sei.cbSize = sizeof(sei);
        sei.lpVerb = L"runas";
        sei.lpFile = exePath;
        sei.nShow = SW_SHOWNORMAL;
        if (ShellExecuteExW(&sei)) return true;
        MessageBoxW(nullptr,
            L"This application requires Administrator privileges to set up face login.",
            L"Administrator Required", MB_ICONERROR);
        return true;
    }
    return false; // already elevated, continue
}

// ============================================================================
// WinMain
// ============================================================================
int WINAPI wWinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance,
                     LPWSTR lpCmdLine, int nCmdShow) {
    UNREFERENCED_PARAMETER(hPrevInstance);
    UNREFERENCED_PARAMETER(lpCmdLine);
    UNREFERENCED_PARAMETER(nCmdShow);

    if (EnsureAdmin()) return 0;

    // Single instance: a second console would fight over the camera and show
    // its own device picker. The mutex dies with the process, so a crash can
    // never leave a stale lock behind. If mutex creation itself fails, run
    // unprotected rather than blocking the app.
    HANDLE instanceMutex = CreateMutexW(nullptr, TRUE,
                                        L"Local\\LockFaceConsoleInstance");
    if (instanceMutex && GetLastError() == ERROR_ALREADY_EXISTS) {
        MessageBoxW(nullptr, L"LockFace 控制台已在运行。",
                    L"LockFace Console", MB_OK | MB_ICONWARNING | MB_TOPMOST);
        CloseHandle(instanceMutex);
        return 0;
    }

    // Check models exist
    std::wstring modelsDir;
    {
        std::wstring regData = ReadRegString(REGVAL_DATA_PATH, L"");
        if (!regData.empty())
            modelsDir = regData + L"\\models";
        else {
            wchar_t programData[MAX_PATH];
            SHGetFolderPathW(nullptr, CSIDL_COMMON_APPDATA, nullptr, 0, programData);
            modelsDir = std::wstring(programData) + L"\\FaceLogin\\models";
        }
    }
    // Check models exist. The enrollment app uses the ONNX pipeline
    // exclusively (SCRFD detection + SFace recognition); no landmark
    // model is loaded at runtime anymore (the 5 keypoints come from SCRFD).
    std::wstring detPath = modelsDir + L"\\det_10g_gnkps.onnx";
    std::wstring recPath = modelsDir + L"\\face_recognition_sface_2021dec.onnx";

    if (GetFileAttributesW(detPath.c_str()) == INVALID_FILE_ATTRIBUTES ||
        GetFileAttributesW(recPath.c_str()) == INVALID_FILE_ATTRIBUTES) {
        std::wstring msg = L"Face recognition models are missing.\n\n"
            L"Expected files:\n  " + detPath + L"\n  " + recPath;
        MessageBoxW(nullptr, msg.c_str(), L"Models Not Found", MB_ICONERROR);
        return 1;
    }

    // Old faces cannot authenticate with SFace. Leave the old file untouched
    // until the user explicitly completes fresh enrollment.
    {
        const std::wstring database = modelsDir.substr(0, modelsDir.size() - 7) + L"\\data\\users.dat";
        std::ifstream file(database, std::ios::binary);
        uint32_t magic = 0, version = 0;
        file.read(reinterpret_cast<char*>(&magic), sizeof(magic));
        file.read(reinterpret_cast<char*>(&version), sizeof(version));
        if (file.good() && magic == 0x474F4C46 && (version == 4 || version == 5)) {
            MessageBoxW(nullptr,
                L"本版本已切换为 SFace。旧人脸数据已停用，请为每个账户重新录入正面、左转和右转人脸。完成录入前请使用 Windows 密码登录。",
                L"需要重新录入人脸", MB_OK | MB_ICONINFORMATION);
        }
    }

    // COM for WebView2 + WIC
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);

    // Preview pacing does not need a large inference pool: cap this process's
    // ONNX thread count. The face service keeps its tuned default (see
    // docs/performance-baseline.md perf2). Must run before the first model
    // initialization inside StartPreview.
    facelogin::SetOnnxThreadCap(4);

    facelogin::EnrollmentWizard wizard;

    // Startup repairs, best-effort and never fatal — both must run after the
    // wizard constructor (which resolves the session identity) and before the
    // UI loop so the first account state the page reads is correct.
    //
    // 1. SID repair (upstream #24): records stored with the machine SID a
    //    pre-fix build resolved on machines where computer name == user name.
    //    Runs FIRST — the UPN repair below only considers records already
    //    keyed by the token SID, so the SID repair must land for both to
    //    apply within a single launch.
    wizard.AutoRepairMachineSidOnStartup();

    // 2. UPN repair: backfill the MSA UPN into the current session user's
    //    record if it is still empty (enrolled while GetUserNameExW returned
    //    err 1332 on some machines).
    wizard.AutoRepairEmptyUpnOnStartup();

    WebviewHost host(hInstance, &wizard);

    int result = host.Run();

    CoUninitialize();
    return result;
}
