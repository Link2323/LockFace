#include "native_logon.h"
#include "../common/secure_clear.h"
#include <ntsecapi.h>
#include <wincred.h>
#include <climits>

namespace facelogin::credential_provider {
HRESULT PackNativeLogon(const std::wstring& domain, const std::wstring& username,
    const std::wstring& password, CREDENTIAL_PROVIDER_USAGE_SCENARIO scenario,
    BYTE** output, DWORD* outputSize) {
    *output = nullptr;
    *outputSize = 0;
    const size_t maxChars = USHRT_MAX / sizeof(wchar_t);
    if (username.empty() || domain.size() > maxChars || username.size() > maxChars || password.size() > maxChars) return E_INVALIDARG;
    if (scenario != CPUS_LOGON && scenario != CPUS_UNLOCK_WORKSTATION) return E_INVALIDARG;
    std::wstring protectedPassword = password;
    facelogin::SecureWStringGuard protectedWipe(protectedPassword);
    if (!password.empty()) {
        CRED_PROTECTION_TYPE protection = CredUnprotected;
        if (!CredIsProtectedW(protectedPassword.data(), &protection)) return HRESULT_FROM_WIN32(GetLastError());
        if (protection == CredUnprotected) {
            DWORD chars = 0;
            CredProtectW(FALSE, protectedPassword.data(), static_cast<DWORD>(protectedPassword.size()+1), nullptr, &chars, nullptr);
            DWORD error = GetLastError();
            if (error != ERROR_INSUFFICIENT_BUFFER || chars == 0) return error ? HRESULT_FROM_WIN32(error) : E_FAIL;
            std::wstring encrypted(chars, L'\0');
            facelogin::SecureWStringGuard encryptedWipe(encrypted);
            if (!CredProtectW(FALSE, protectedPassword.data(), static_cast<DWORD>(protectedPassword.size()+1), encrypted.data(), &chars, nullptr)) return HRESULT_FROM_WIN32(GetLastError());
            encrypted.resize(wcslen(encrypted.c_str()));
            facelogin::SecureClearWString(protectedPassword);
            protectedPassword = encrypted;
        }
    }
    const std::wstring* values[] = {&domain, &username, &protectedPassword};
    for (const auto* value : values) {
        if (value->size() > USHRT_MAX / sizeof(wchar_t)) return E_INVALIDARG;
    }
    const size_t bytes = sizeof(KERB_INTERACTIVE_UNLOCK_LOGON) +
        (domain.size()+username.size()+protectedPassword.size())*sizeof(wchar_t);
    BYTE* buffer = static_cast<BYTE*>(CoTaskMemAlloc(bytes));
    if (!buffer) return E_OUTOFMEMORY;
    ZeroMemory(buffer, bytes);
    auto* logon = reinterpret_cast<KERB_INTERACTIVE_UNLOCK_LOGON*>(buffer);
    logon->Logon.MessageType = scenario == CPUS_UNLOCK_WORKSTATION ? KerbWorkstationUnlockLogon : KerbInteractiveLogon;
    size_t offset = sizeof(*logon);
    auto append = [&](const std::wstring& value, UNICODE_STRING& field) {
        field.Length = field.MaximumLength = static_cast<USHORT>(value.size()*sizeof(wchar_t));
        if (field.Length) CopyMemory(buffer+offset, value.data(), field.Length);
        field.Buffer = reinterpret_cast<PWSTR>(offset);
        offset += field.Length;
    };
    append(domain, logon->Logon.LogonDomainName);
    append(username, logon->Logon.UserName);
    append(protectedPassword, logon->Logon.Password);
    *output = buffer;
    *outputSize = static_cast<DWORD>(bytes);
    return S_OK;
}

}
