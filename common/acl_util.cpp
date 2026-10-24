#include "acl_util.h"
#include "acl_util_internal.h"
#include "logger.h"

#include <aclapi.h>
#include <sddl.h>

namespace facelogin {
namespace {

constexpr ACCESS_MASK kTamperMask =
    FILE_WRITE_DATA | FILE_APPEND_DATA | FILE_WRITE_EA | FILE_WRITE_ATTRIBUTES |
    FILE_DELETE_CHILD | DELETE | WRITE_DAC | WRITE_OWNER |
    GENERIC_WRITE | GENERIC_ALL | MAXIMUM_ALLOWED;

bool IsAdminClassTrustee(PSID sid) {
    if (!sid || !IsValidSid(sid)) return false;
    if (IsWellKnownSid(sid, WinLocalSystemSid) ||
        IsWellKnownSid(sid, WinBuiltinAdministratorsSid)) return true;
    PSID trustedInstaller = nullptr;
    if (!ConvertStringSidToSidW(
            L"S-1-5-80-956008885-3418522649-1831038044-1853292631-2271478464",
            &trustedInstaller)) return false;
    const bool trusted = EqualSid(sid, trustedInstaller) != FALSE;
    LocalFree(trustedInstaller);
    return trusted;
}

bool Fail(const wchar_t* operation, const std::wstring& path, DWORD error) {
    FACELOGIN_ERROR(L"%s failed for '%s' (error=%lu)", operation, path.c_str(), error);
    return false;
}

struct ObjectHandle {
    HANDLE value = INVALID_HANDLE_VALUE;
    ~ObjectHandle() { if (value != INVALID_HANDLE_VALUE) CloseHandle(value); }
};

struct SecurityDescriptor {
    PSECURITY_DESCRIPTOR value = nullptr;
    ~SecurityDescriptor() { if (value) LocalFree(value); }
};

bool ReadSecurity(HANDLE handle, const std::wstring& path, SecurityDescriptor& sd) {
    const DWORD rc = GetSecurityInfo(handle, SE_FILE_OBJECT,
        OWNER_SECURITY_INFORMATION | DACL_SECURITY_INFORMATION,
        nullptr, nullptr, nullptr, nullptr, &sd.value);
    return rc == ERROR_SUCCESS || Fail(L"GetSecurityInfo", path, rc);
}

bool HasTrustedOwner(PSECURITY_DESCRIPTOR sd) {
    PSID owner = nullptr;
    BOOL defaulted = FALSE;
    return GetSecurityDescriptorOwner(sd, &owner, &defaulted) && IsAdminClassTrustee(owner);
}

// Pin the object while checking/changing it. Never follow a final junction or
// symlink, and refuse file/directory substitution before touching security.
bool OpenObject(const std::wstring& path, bool directory, DWORD access, ObjectHandle& handle) {
    handle.value = CreateFileW(path.c_str(), access | FILE_READ_ATTRIBUTES,
        FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
        FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    if (handle.value == INVALID_HANDLE_VALUE) return Fail(L"CreateFileW", path, GetLastError());
    BY_HANDLE_FILE_INFORMATION info = {};
    if (!GetFileInformationByHandle(handle.value, &info))
        return Fail(L"GetFileInformationByHandle", path, GetLastError());
    if ((info.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) ||
        ((info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0) != directory)
        return Fail(L"Validate object type (reparse points are unsupported)", path, ERROR_INVALID_DATA);
    return true;
}

bool SecureObject(const std::wstring& path, bool directory, bool allowUserRead, PACL desiredDacl) {
    ObjectHandle handle;
    if (!OpenObject(path, directory, READ_CONTROL, handle)) return false;
    SecurityDescriptor before;
    if (!ReadSecurity(handle.value, path, before)) return false;
    // A low-privilege owner can recover WRITE_DAC even after a DACL rewrite.
    // Reject rather than take ownership of attacker-created content.
    if (!HasTrustedOwner(before.value))
        return Fail(L"Validate administrator-class owner", path, ERROR_INVALID_OWNER);
    if (!acl_detail::MeetsDirectoryPolicy(before.value, allowUserRead, true)) {
        ObjectHandle writable;
        if (!OpenObject(path, directory, READ_CONTROL | WRITE_DAC, writable)) return false;
        const DWORD rc = SetSecurityInfo(writable.value, SE_FILE_OBJECT,
            DACL_SECURITY_INFORMATION | PROTECTED_DACL_SECURITY_INFORMATION,
            nullptr, nullptr, desiredDacl, nullptr);
        if (rc != ERROR_SUCCESS) return Fail(L"SetSecurityInfo", path, rc);
        SecurityDescriptor after;
        if (!ReadSecurity(handle.value, path, after)) return false;
        if (!acl_detail::MeetsDirectoryPolicy(after.value, allowUserRead, true))
            return Fail(L"Verify secured ACL", path, ERROR_ACCESS_DENIED);
    }

    // Explicit/protected credential file ACEs survive parent ACL changes.
    // Enforce the strict policy on backups and temporary files as well.
    if (directory && !allowUserRead) {
        WIN32_FIND_DATAW entry = {};
        HANDLE search = FindFirstFileW((path + L"\\*").c_str(), &entry);
        if (search == INVALID_HANDLE_VALUE) {
            const DWORD error = GetLastError();
            return error == ERROR_FILE_NOT_FOUND || Fail(L"FindFirstFileW", path, error);
        }
        bool ok = true;
        do {
            if (wcscmp(entry.cFileName, L".") == 0 || wcscmp(entry.cFileName, L"..") == 0) continue;
            if (!SecureObject(path + L"\\" + entry.cFileName,
                              (entry.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0,
                              false, desiredDacl)) {
                ok = false;
                break;
            }
        } while (FindNextFileW(search, &entry));
        if (ok && GetLastError() != ERROR_NO_MORE_FILES)
            ok = Fail(L"FindNextFileW", path, GetLastError());
        FindClose(search);
        return ok;
    }
    return true;
}

} // namespace

namespace acl_detail {

bool MeetsDirectoryPolicy(PSECURITY_DESCRIPTOR sd, bool allowUserRead, bool requireProtected) {
    if (!sd || !IsValidSecurityDescriptor(sd) || !HasTrustedOwner(sd)) return false;
    SECURITY_DESCRIPTOR_CONTROL control = 0;
    DWORD revision = 0;
    if (!GetSecurityDescriptorControl(sd, &control, &revision) ||
        (requireProtected && !(control & SE_DACL_PROTECTED))) return false;
    BOOL present = FALSE, defaulted = FALSE;
    PACL dacl = nullptr;
    if (!GetSecurityDescriptorDacl(sd, &present, &dacl, &defaulted) ||
        !present || !dacl || !IsValidAcl(dacl)) return false;
    for (DWORD i = 0; i < dacl->AceCount; ++i) {
        void* ace = nullptr;
        if (!GetAce(dacl, i, &ace)) return false;
        const auto* header = static_cast<const ACE_HEADER*>(ace);
        if (header->AceType == ACCESS_DENIED_ACE_TYPE) continue;
        // Unknown/conditional/object ACE forms are not evidence of safety.
        if (header->AceType != ACCESS_ALLOWED_ACE_TYPE) return false;
        const auto* allowed = static_cast<const ACCESS_ALLOWED_ACE*>(ace);
        PSID sid = reinterpret_cast<PSID>(const_cast<DWORD*>(&allowed->SidStart));
        // Future children must also be protected: inspect inherit-only ACEs.
        if (!IsAdminClassTrustee(sid) &&
            (allowUserRead ? (allowed->Mask & kTamperMask) != 0 : allowed->Mask != 0)) return false;
    }
    return true;
}

} // namespace acl_detail

bool IsDirectoryAdminWriteProtected(const std::wstring& dir) {
    ObjectHandle handle;
    if (!OpenObject(dir, true, READ_CONTROL, handle)) return false;
    SecurityDescriptor sd;
    return ReadSecurity(handle.value, dir, sd) &&
        acl_detail::MeetsDirectoryPolicy(sd.value, true, false);
}

bool EnsureSecureDirectory(const std::wstring& dir, bool allowUserRead) {
    if (dir.empty()) return Fail(L"Validate directory path", dir, ERROR_INVALID_NAME);
    SecurityDescriptor desired;
    // Do not inherit a caller's per-user default owner. Only SYSTEM and
    // elevated administrators can create this Administrators-owned object.
    const wchar_t* sddl = allowUserRead
        ? L"O:BAD:P(A;OICI;FA;;;SY)(A;OICI;FA;;;BA)(A;OICI;GRGX;;;BU)"
        : L"O:BAD:P(A;OICI;FA;;;SY)(A;OICI;FA;;;BA)";
    if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(sddl, SDDL_REVISION_1, &desired.value, nullptr))
        return Fail(L"ConvertStringSecurityDescriptorToSecurityDescriptorW", dir, GetLastError());
    SECURITY_ATTRIBUTES sa = {sizeof(sa), desired.value, FALSE};
    if (!CreateDirectoryW(dir.c_str(), &sa)) {
        const DWORD error = GetLastError();
        if (error != ERROR_ALREADY_EXISTS) return Fail(L"CreateDirectoryW", dir, error);
    }
    BOOL present = FALSE, defaulted = FALSE;
    PACL dacl = nullptr;
    if (!GetSecurityDescriptorDacl(desired.value, &present, &dacl, &defaulted))
        return Fail(L"GetSecurityDescriptorDacl", dir, GetLastError());
    return SecureObject(dir, true, allowUserRead, dacl);
}

} // namespace facelogin
