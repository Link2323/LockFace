#include "privacy_withdrawal.h"
#include <cstdio>
#include <sddl.h>
#include <vector>

int RunPrivacyWithdrawalTests() {
    using facelogin::PrivacyWithdrawal;
    using facelogin::PrivacyWithdrawalState;
    int failures = 0;
    const auto check = [&failures](bool ok, const char* label) {
        if (!ok) { std::fprintf(stderr, "FAIL: %s\n", label); ++failures; }
    };
    const auto path = L"SOFTWARE\\LockFaceWithdrawalTest-" +
        std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetTickCount64());
    HKEY key = nullptr;
    DWORD disposition = 0;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, path.c_str(), 0, nullptr, 0, KEY_ALL_ACCESS,
                       nullptr, &key, &disposition) != ERROR_SUCCESS) return 1;
    // Never touch an existing registry key, including on a fixture-name collision.
    if (disposition != REG_CREATED_NEW_KEY) { RegCloseKey(key); return 1; }
    PrivacyWithdrawal state(HKEY_CURRENT_USER, path.c_str());
    check(state.Read() == PrivacyWithdrawalState::None, "missing withdrawal marker allows enrollment");
    const wchar_t version[] = L"1.1", time[] = L"2026-10-26T10:00:00+08:00";
    check(RegSetValueExW(key, L"ConsentVersion", 0, REG_SZ,
        reinterpret_cast<const BYTE*>(version), sizeof(version)) == ERROR_SUCCESS &&
        RegSetValueExW(key, L"ConsentTime", 0, REG_SZ,
        reinterpret_cast<const BYTE*>(time), sizeof(time)) == ERROR_SUCCESS,
        "seed prior consent for withdrawal");
    const DWORD theme = 7;
    check(RegSetValueExW(key, L"ThemeMode", 0, REG_DWORD,
        reinterpret_cast<const BYTE*>(&theme), sizeof(theme)) == ERROR_SUCCESS, "seed unrelated preference");
    unsigned calls = 0;
    check(!state.Run([&]() {
        ++calls;
        PrivacyWithdrawal reopened(HKEY_CURRENT_USER, path.c_str());
        check(reopened.Read() == PrivacyWithdrawalState::Pending,
              "durable intent is readable before the deletion callback starts");
        return false;
    }) && calls == 1, "service failure preserves pending withdrawal");
    RegCloseKey(key);
    key = nullptr;
    PrivacyWithdrawal afterRestart(HKEY_CURRENT_USER, path.c_str());
    check(afterRestart.Read() == PrivacyWithdrawalState::Pending,
          "new state object with no cached handles still reads pending intent");
    check(RegOpenKeyExW(HKEY_CURRENT_USER, path.c_str(), 0, KEY_ALL_ACCESS, &key) == ERROR_SUCCESS,
          "reopen the isolated registry fixture");
    if (key) {
        RegDeleteValueW(key, L"ConsentVersion");
        RegDeleteValueW(key, L"ConsentTime");
    }
    check(afterRestart.Read() == PrivacyWithdrawalState::Pending,
          "partial consent cleanup does not clear the withdrawal marker");
    check(afterRestart.Run([&]() { ++calls; return true; }) && calls == 2 &&
          afterRestart.Read() == PrivacyWithdrawalState::None,
          "confirmed retry clears the independent marker last");
    DWORD value = 0, size = sizeof(value);
    check(RegGetValueW(HKEY_CURRENT_USER, path.c_str(), L"ThemeMode", RRF_RT_REG_DWORD,
                      nullptr, &value, &size) == ERROR_SUCCESS && value == theme,
          "completion preserves unrelated user preferences");
    for (const auto name : {L"ConsentVersion", L"ConsentTime", L"ConsentWithdrawalPending"}) {
        size = sizeof(value);
        check(RegGetValueW(HKEY_CURRENT_USER, path.c_str(), name, RRF_RT_ANY, nullptr,
                          &value, &size) == ERROR_FILE_NOT_FOUND,
              "completed withdrawal removes both consent fields and pending marker");
    }
    if (key) {
        // Model service success followed by an actual registry cleanup denial.
        // The original open handle retains WRITE_DAC so the test can restore
        // permissions and remove its unique fixture even after denying writes.
        DWORD securitySize = 0;
        const auto queried = RegGetKeySecurity(key, DACL_SECURITY_INFORMATION, nullptr, &securitySize);
        std::vector<BYTE> originalSecurity(securitySize);
        PSECURITY_DESCRIPTOR denied = nullptr;
        const bool prepared = queried == ERROR_INSUFFICIENT_BUFFER &&
            RegGetKeySecurity(key, DACL_SECURITY_INFORMATION,
                reinterpret_cast<PSECURITY_DESCRIPTOR>(originalSecurity.data()), &securitySize) == ERROR_SUCCESS &&
            ConvertStringSecurityDescriptorToSecurityDescriptorW(
                L"D:(D;;0x0002;;;WD)(A;;KA;;;WD)", SDDL_REVISION_1, &denied, nullptr);
        check(prepared, "prepare isolated registry cleanup failure");
        if (prepared) {
            check(!afterRestart.Run([&]() {
                check(RegSetKeySecurity(key, DACL_SECURITY_INFORMATION, denied) == ERROR_SUCCESS,
                      "deny cleanup after confirmed service deletion");
                return true;
            }), "registry cleanup failure cannot report completed withdrawal");
            check(afterRestart.Read() == PrivacyWithdrawalState::Pending,
                  "cleanup failure preserves a separately readable pending marker");
            check(RegSetKeySecurity(key, DACL_SECURITY_INFORMATION,
                reinterpret_cast<PSECURITY_DESCRIPTOR>(originalSecurity.data())) == ERROR_SUCCESS,
                "restore fixture permissions for retry and cleanup");
            check(afterRestart.Run([]() { return true; }), "retry completes after registry access is restored");
        }
        if (denied) LocalFree(denied);
        const wchar_t invalid[] = L"invalid";
        RegSetValueExW(key, L"ConsentWithdrawalPending", 0, REG_SZ,
                      reinterpret_cast<const BYTE*>(invalid), sizeof(invalid));
        check(afterRestart.Read() == PrivacyWithdrawalState::Unavailable,
              "malformed marker blocks enrollment instead of trusting old consent");
        value = 0;
        RegSetValueExW(key, L"ConsentWithdrawalPending", 0, REG_DWORD,
                      reinterpret_cast<const BYTE*>(&value), sizeof(value));
        check(afterRestart.Read() == PrivacyWithdrawalState::Unavailable,
              "unexpected marker value cannot bypass the withdrawal gate");
        check(afterRestart.Run([]() { return true; }), "retry repairs an invalid marker and completes cleanup");
        RegCloseKey(key);
    }
    PrivacyWithdrawal unavailable(nullptr, L"");
    check(unavailable.Read() == PrivacyWithdrawalState::Unavailable, "registry read failure blocks enrollment");
    check(!unavailable.Run([&]() { ++calls; return true; }) && calls == 2,
          "failure to persist withdrawal never invokes the service callback");
    // This exact, unique key was created by this test and contains no subkeys.
    check(RegDeleteKeyW(HKEY_CURRENT_USER, path.c_str()) == ERROR_SUCCESS,
          "remove isolated registry fixture");
    return failures;
}
