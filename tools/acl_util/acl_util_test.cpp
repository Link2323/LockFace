#include "acl_util.h"
#include "acl_util_internal.h"
#include "logger.h"

#include <windows.h>
#include <aclapi.h>
#include <sddl.h>

#include <cstdio>
#include <string>
#include <vector>

namespace {
int failures = 0;
void Check(bool ok, const char* message) {
    if (!ok) { std::fprintf(stderr, "FAIL: %s\n", message); ++failures; }
}

void Policy(const wchar_t* sddl, bool readable, bool protectedAcl, bool expected, const char* message) {
    PSECURITY_DESCRIPTOR sd = nullptr;
    const bool parsed = ConvertStringSecurityDescriptorToSecurityDescriptorW(
        sddl, SDDL_REVISION_1, &sd, nullptr) != FALSE;
    Check(parsed, "parse test descriptor");
    if (parsed) Check(facelogin::acl_detail::MeetsDirectoryPolicy(sd, readable, protectedAcl) == expected, message);
    LocalFree(sd);
}

bool SetDescriptor(const std::wstring& path, const std::wstring& sddl, bool setOwner = true) {
    PSECURITY_DESCRIPTOR sd = nullptr;
    if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(sddl.c_str(), SDDL_REVISION_1, &sd, nullptr)) return false;
    PACL dacl = nullptr;
    PSID owner = nullptr;
    BOOL present = FALSE, defaulted = FALSE;
    GetSecurityDescriptorDacl(sd, &present, &dacl, &defaulted);
    GetSecurityDescriptorOwner(sd, &owner, &defaulted);
    const DWORD rc = SetNamedSecurityInfoW(const_cast<wchar_t*>(path.c_str()), SE_FILE_OBJECT,
        (setOwner ? OWNER_SECURITY_INFORMATION : 0) | DACL_SECURITY_INFORMATION | PROTECTED_DACL_SECURITY_INFORMATION,
        setOwner ? owner : nullptr, nullptr, dacl, nullptr);
    LocalFree(sd);
    return rc == ERROR_SUCCESS;
}

bool ActualPolicy(const std::wstring& path, bool readable) {
    PSECURITY_DESCRIPTOR sd = nullptr;
    const DWORD rc = GetNamedSecurityInfoW(path.c_str(), SE_FILE_OBJECT,
        OWNER_SECURITY_INFORMATION | DACL_SECURITY_INFORMATION,
        nullptr, nullptr, nullptr, nullptr, &sd);
    const bool ok = rc == ERROR_SUCCESS && facelogin::acl_detail::MeetsDirectoryPolicy(sd, readable, true);
    LocalFree(sd);
    return ok;
}

bool CreateFileAt(const std::wstring& path) {
    HANDLE file = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;
    CloseHandle(file);
    return true;
}
} // namespace

int wmain() {
    Policy(L"O:BAD:P(A;OICI;FA;;;SY)(A;OICI;FA;;;BA)(A;OICI;FRFX;;;BU)", true, true, true, "installer root accepted");
    Policy(L"O:BAD:P(A;OICI;FA;;;SY)(A;OICI;FA;;;BA)(A;OICI;FRFX;;;BU)", false, true, false, "Users read rejected for data");
    Policy(L"O:SYD:P(A;OICI;FA;;;SY)(A;OICI;FA;;;BA)", false, true, true, "strict data accepted");
    Policy(L"O:BUD:P(A;OICI;FA;;;SY)(A;OICI;FA;;;BA)", true, true, false, "low privilege owner rejected despite strict DACL");
    Policy(L"O:BAD:P(A;OICI;FA;;;SY)(A;OICI;FA;;;BA)(A;OICI;FW;;;BU)", true, true, false, "Users write rejected");
    Policy(L"O:BAD:P(A;OICI;FA;;;SY)(A;OICI;FA;;;BA)(A;OICIIO;GW;;;BU)", true, true, false, "inherit-only generic write rejected");
    Policy(L"O:BAD:(A;OICI;FA;;;SY)(A;OICI;FA;;;BA)", false, true, false, "unprotected data requires repair");
    Policy(L"O:BAD:NO_ACCESS_CONTROL", true, false, false, "null DACL rejected");

    HANDLE token = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) return 1;
    DWORD needed = 0;
    GetTokenInformation(token, TokenUser, nullptr, 0, &needed);
    std::vector<BYTE> user(needed);
    if (!GetTokenInformation(token, TokenUser, user.data(), needed, &needed)) { CloseHandle(token); return 1; }
    wchar_t* sidText = nullptr;
    ConvertSidToStringSidW(reinterpret_cast<TOKEN_USER*>(user.data())->User.Sid, &sidText);
    const std::wstring sid = sidText;
    LocalFree(sidText);
    TOKEN_ELEVATION elevation = {};
    GetTokenInformation(token, TokenElevation, &elevation, sizeof(elevation), &needed);
    CloseHandle(token);

    wchar_t temp[MAX_PATH] = {};
    if (!GetTempPathW(MAX_PATH, temp)) return 1;
    const std::wstring root = std::wstring(temp) + L"LockFace-AclUtilTest-" +
        std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetTickCount64());
    if (!CreateDirectoryW(root.c_str(), nullptr)) return 1;
    const std::wstring hostile = root + L"\\hostile";
    Check(CreateDirectoryW(hostile.c_str(), nullptr) != FALSE, "create pre-owned directory");
    const std::wstring owned = L"O:" + sid + L"D:P(A;OICI;FA;;;SY)(A;OICI;FA;;;BA)(A;OICI;FA;;;" + sid + L")";
    Check(SetDescriptor(hostile, owned), "set caller-owned fixture");
    Check(!facelogin::EnsureSecureDirectory(hostile), "reject original pre-created owner attack");
    Check(!facelogin::IsDirectoryAdminWriteProtected(hostile), "low owner is not production");
    // Even after removing the owner's explicit ACE, ownership must not pass.
    Check(SetDescriptor(hostile, L"O:" + sid + L"D:P(A;OICI;FA;;;SY)(A;OICI;FA;;;BA)(A;OICI;FRFX;;;BU)"), "make deceptive strict DACL");
    Check(!facelogin::EnsureSecureDirectory(hostile), "reject owner bypass with no write ACE");
    Check(SetDescriptor(hostile, owned, false), "owner can restore its DACL (reason for rejection)");
    Check(RemoveDirectoryW(hostile.c_str()) != FALSE, "cleanup hostile fixture");

    const std::wstring occupied = root + L"\\occupied";
    Check(CreateFileAt(occupied), "create file-instead-of-directory fixture");
    Check(!facelogin::EnsureSecureDirectory(occupied), "reject wrong object type");
    Check(DeleteFileW(occupied.c_str()) != FALSE, "cleanup occupied fixture");
    const std::wstring missing = root + L"\\missing\\child";
    facelogin::Logger::Instance().ClearLogs();
    Check(!facelogin::EnsureSecureDirectory(missing), "missing ancestor fails");
    const auto logs = facelogin::Logger::Instance().GetRecentLogs();
    bool detailed = false;
    for (const auto& line : logs)
        detailed |= line.find(L"CreateDirectoryW") != std::wstring::npos &&
                    line.find(missing) != std::wstring::npos && line.find(L"error=") != std::wstring::npos;
    Check(detailed, "failure log contains API, exact path and error");

    const std::wstring secure = root + L"\\secure";
    if (elevation.TokenIsElevated) {
        Check(facelogin::EnsureSecureDirectory(secure), "create trusted root");
        Check(ActualPolicy(secure, true), "new root has trusted owner and protected DACL");
        Check(facelogin::IsDirectoryAdminWriteProtected(secure), "trusted directory is production on current drive");
        const std::wstring data = secure + L"\\data";
        Check(CreateDirectoryW(data.c_str(), nullptr) != FALSE, "create inherited-readable data");
        const std::wstring db = data + L"\\users.dat";
        const std::wstring backup = data + L"\\users.dat.bak";
        Check(CreateFileAt(db) && CreateFileAt(backup), "create credential fixtures");
        const wchar_t* readable = L"O:BAD:P(A;OICI;FA;;;SY)(A;OICI;FA;;;BA)(A;OICI;FRFX;;;BU)";
        Check(SetDescriptor(data, readable) && SetDescriptor(db, readable) && SetDescriptor(backup, readable), "set explicit protected Users-read fixture");
        Check(facelogin::EnsureSecureDirectory(data, false), "repair readable data subtree");
        Check(ActualPolicy(data, false) && ActualPolicy(db, false) && ActualPolicy(backup, false), "no Users read on directory, main or backup");
        Check(facelogin::EnsureSecureDirectory(data, false), "strict repair is idempotent");
        Check(DeleteFileW(db.c_str()) && DeleteFileW(backup.c_str()) && RemoveDirectoryW(data.c_str()), "cleanup credential fixtures");
        Check(RemoveDirectoryW(secure.c_str()) != FALSE, "cleanup secure root");
    } else {
        Check(!facelogin::EnsureSecureDirectory(secure), "unprivileged first creator cannot leave trusted directory");
        std::puts("SKIP: elevated filesystem creation/healing cases (rerun as administrator)");
    }
    Check(RemoveDirectoryW(root.c_str()) != FALSE, "cleanup temporary root");
    std::printf("AclUtilTest: %d failures\n", failures);
    return failures ? 1 : 0;
}
