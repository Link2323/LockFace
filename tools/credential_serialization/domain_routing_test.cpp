#include <windows.h>
#include <credentialprovider.h>
#include <ntsecapi.h>
#include <iostream>
#include "../../common/account_identity.h"
#include "../../common/secure_clear.h"
#include "../../credential_provider/LockFaceCredential.h"

namespace facelogin::account_identity {
// Deterministic directory fixture. Production ResolveUserSid is separately
// exercised by EnrollmentSupportTest; this test never contacts a domain.
bool ResolveUserSid(const std::wstring& sid, std::wstring& domain, std::wstring& user) {
    domain.clear(); user.clear();
    if (sid == L"S-1-5-21-11-22-33-1001") {
        domain = L"CORP"; user = L"ActualSam"; return true;
    }
    return false;
}
}
struct CredentialSerializationTestAccess {
    static HRESULT Pack(LockFaceCredential& c, CREDENTIAL_PROVIDER_USAGE_SCENARIO scenario,
                        bool unresolved, CREDENTIAL_PROVIDER_CREDENTIAL_SERIALIZATION& result) {
        c.Initialize(nullptr, scenario);
        const std::wstring sid = unresolved ? L"S-1-5-21-11-22-33-9999" : L"S-1-5-21-11-22-33-1001";
        wchar_t computer[MAX_COMPUTERNAME_LENGTH + 1]{};
        DWORD chars = ARRAYSIZE(computer);
        GetComputerNameW(computer, &chars);
        // Exercise transport parsing and SID retention too, rather than
        // assigning credential members directly.
        std::wstring message = facelogin::ipc::BuildAuthSuccessMessage(
            sid, L"user@corp.example", computer, L"OldStoredName", L"dummy-test-only");
        facelogin::SecureWStringGuard messageWipe(message);
        c.m_state = facelogin::credential_provider::AuthState::Authenticating;
        c.OnPipeResponse(true, message);
        return c.PackCredentials(&result);
    }
    static bool PasswordErased(const LockFaceCredential& c) { return c.m_password.empty(); }
    static bool CacheMatches(LockFaceCredential& c, const CREDENTIAL_PROVIDER_CREDENTIAL_SERIALIZATION& result) {
        CREDENTIAL_PROVIDER_GET_SERIALIZATION_RESPONSE response{};
        CREDENTIAL_PROVIDER_CREDENTIAL_SERIALIZATION repeated{};
        PWSTR text = nullptr;
        CREDENTIAL_PROVIDER_STATUS_ICON icon{};
        const HRESULT hr = c.GetSerialization(&response, &repeated, &text, &icon);
        const bool matches = SUCCEEDED(hr) && response == CPGSR_RETURN_CREDENTIAL_FINISHED &&
            repeated.cbSerialization == result.cbSerialization && repeated.ulAuthenticationPackage == result.ulAuthenticationPackage &&
            repeated.rgbSerialization && memcmp(repeated.rgbSerialization, result.rgbSerialization, result.cbSerialization) == 0;
        if (repeated.rgbSerialization) {
            SecureZeroMemory(repeated.rgbSerialization, repeated.cbSerialization);
            CoTaskMemFree(repeated.rgbSerialization);
        }
        CoTaskMemFree(text);
        return matches;
    }
};
static int failures = 0;
static void Check(bool ok, const char* label) {
    std::cout << (ok ? "PASS " : "FAIL ") << label << '\n';
    if (!ok) ++failures;
}
static std::wstring Read(const UNICODE_STRING& s, const CREDENTIAL_PROVIDER_CREDENTIAL_SERIALIZATION& c) {
    const auto offset = reinterpret_cast<ULONG_PTR>(s.Buffer);
    if (offset > c.cbSerialization || s.Length > c.cbSerialization - offset) return L"<invalid>";
    return {reinterpret_cast<const wchar_t*>(c.rgbSerialization + offset), s.Length / sizeof(wchar_t)};
}
int main() {
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    for (auto scenario : {CPUS_LOGON, CPUS_UNLOCK_WORKSTATION}) {
        LockFaceCredential credential;
        CREDENTIAL_PROVIDER_CREDENTIAL_SERIALIZATION result{};
        const HRESULT hr = CredentialSerializationTestAccess::Pack(credential, scenario, false, result);
        Check(SUCCEEDED(hr), "old AD enrollment packs with directory fixture");
        Check(CredentialSerializationTestAccess::PasswordErased(credential), "AD plaintext erased");
        if (SUCCEEDED(hr)) {
            Check(result.cbSerialization >= sizeof(KERB_INTERACTIVE_UNLOCK_LOGON), "native layout present");
            if (result.cbSerialization >= sizeof(KERB_INTERACTIVE_UNLOCK_LOGON)) {
                auto* logon = reinterpret_cast<KERB_INTERACTIVE_UNLOCK_LOGON*>(result.rgbSerialization);
                Check(Read(logon->Logon.LogonDomainName, result) == L"CORP", "SID overrides transport machine name despite UPN");
                Check(Read(logon->Logon.UserName, result) == L"ActualSam", "SID overrides stale stored SAM name");
                Check(logon->Logon.MessageType == (scenario == CPUS_LOGON ? KerbInteractiveLogon : KerbWorkstationUnlockLogon), "scenario reaches native packing");
            }
            Check(CredentialSerializationTestAccess::CacheMatches(credential, result), "repeat serialization cache preserves package and bytes");
            SecureZeroMemory(result.rgbSerialization, result.cbSerialization);
            CoTaskMemFree(result.rgbSerialization);
        }
    }
    LockFaceCredential credential;
    CREDENTIAL_PROVIDER_CREDENTIAL_SERIALIZATION result{};
    Check(FAILED(CredentialSerializationTestAccess::Pack(credential, CPUS_LOGON, true, result)), "unresolved AD SID rejected without MSA fallback");
    Check(!result.rgbSerialization && !result.cbSerialization, "lookup failure returns no credentials");
    Check(CredentialSerializationTestAccess::PasswordErased(credential), "lookup failure erases plaintext");
    CoUninitialize();
    return failures ? 1 : 0;
}
