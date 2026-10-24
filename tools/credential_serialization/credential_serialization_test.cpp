#include <windows.h>
#include "../../common/account_identity.h"
#include "../../credential_provider/native_logon.h"
#include <credentialprovider.h>
#include <ntsecapi.h>
#include <wincred.h>
#include <sddl.h>
#include <string>
#include <memory>
#include <vector>
#include <iostream>

#include "../../credential_provider/pipe_client.h"

#include "../../credential_provider/LockFaceCredential.h"

struct CredentialSerializationTestAccess {
    static HRESULT Pack(LockFaceCredential& credential, const wchar_t* domain, const wchar_t* upn,
        CREDENTIAL_PROVIDER_USAGE_SCENARIO scenario, CREDENTIAL_PROVIDER_CREDENTIAL_SERIALIZATION* result) {
        credential.Initialize(nullptr, scenario);
        credential.m_domain = domain;
        credential.m_username = L"DummyUser";
        credential.m_upn = upn;
        credential.m_password = L"dummy-test-only";
        return credential.PackCredentials(result);
    }
    static bool PasswordErased(const LockFaceCredential& credential) { return credential.m_password.empty(); }
    static void SetSid(LockFaceCredential& credential, const std::wstring& sid) { credential.m_sid = sid; }
};

static int failures = 0;
static void Check(bool condition, const char* name) {
    std::cout << (condition ? "PASS " : "FAIL ") << name << '\n';
    if (!condition) ++failures;
}
static ULONG AuthenticationPackage(const char* package) {
    HANDLE lsa = nullptr;
    if (LsaConnectUntrusted(&lsa) != 0) return ULONG_MAX;
    LSA_STRING name{static_cast<USHORT>(strlen(package)), static_cast<USHORT>(strlen(package) + 1), const_cast<char*>(package)};
    ULONG id = ULONG_MAX;
    LsaLookupAuthenticationPackage(lsa, &name, &id);
    LsaDeregisterLogonProcess(lsa);
    return id;
}
static std::wstring ReadPackedString(const UNICODE_STRING& value, const BYTE* buffer, DWORD size) {
    const auto offset = reinterpret_cast<ULONG_PTR>(value.Buffer);
    if (offset > size || value.Length > size-offset) return L"<invalid-offset>";
    return std::wstring(reinterpret_cast<const wchar_t*>(buffer+offset), value.Length/sizeof(wchar_t));
}
static void TestNativeAccount(const wchar_t* domain, const wchar_t* upn, CREDENTIAL_PROVIDER_USAGE_SCENARIO scenario, const char* label) {
    LockFaceCredential credential;
    CREDENTIAL_PROVIDER_CREDENTIAL_SERIALIZATION result{};
    HRESULT hr = CredentialSerializationTestAccess::Pack(credential, domain, upn, scenario, &result);
    Check(SUCCEEDED(hr), (std::string(label)+": pack succeeds").c_str());
    if (FAILED(hr)) return;
    Check(result.ulAuthenticationPackage == AuthenticationPackage("Negotiate"), (std::string(label)+": Negotiate package").c_str());
    Check(result.cbSerialization >= sizeof(KERB_INTERACTIVE_UNLOCK_LOGON), (std::string(label)+": unlock-capable layout").c_str());
    if (result.cbSerialization >= sizeof(KERB_INTERACTIVE_UNLOCK_LOGON)) {
        auto* packed = reinterpret_cast<KERB_INTERACTIVE_UNLOCK_LOGON*>(result.rgbSerialization);
        Check(packed->Logon.MessageType == (scenario == CPUS_UNLOCK_WORKSTATION ? KerbWorkstationUnlockLogon : KerbInteractiveLogon), (std::string(label)+": correct scenario").c_str());
        Check(ReadPackedString(packed->Logon.LogonDomainName, result.rgbSerialization, result.cbSerialization) == domain, (std::string(label)+": domain preserved despite UPN").c_str());
        Check(ReadPackedString(packed->Logon.UserName, result.rgbSerialization, result.cbSerialization) == L"DummyUser", (std::string(label)+": SAM username preserved").c_str());
        std::wstring protectedPassword = ReadPackedString(packed->Logon.Password,result.rgbSerialization,result.cbSerialization);
        CRED_PROTECTION_TYPE protection = CredUnprotected;
        Check(CredIsProtectedW(protectedPassword.data(),&protection) && protection != CredUnprotected, (std::string(label)+": serialized password protected").c_str());
        if (!protectedPassword.empty()) SecureZeroMemory(protectedPassword.data(),protectedPassword.size()*sizeof(wchar_t));
    }
    Check(CredentialSerializationTestAccess::PasswordErased(credential), (std::string(label)+": transient password erased").c_str());
    SecureZeroMemory(result.rgbSerialization, result.cbSerialization);
    CoTaskMemFree(result.rgbSerialization);
}
static void TestIdentityProvider(const wchar_t* domain, const char* label) {
    LockFaceCredential credential;
    CREDENTIAL_PROVIDER_CREDENTIAL_SERIALIZATION result{};
    HRESULT hr = CredentialSerializationTestAccess::Pack(credential, domain, L"dummy@example.test", CPUS_LOGON, &result);
    Check(SUCCEEDED(hr), (std::string(label)+": identity-provider packing succeeds").c_str());
    if (SUCCEEDED(hr)) {
        Check(result.ulAuthenticationPackage == AuthenticationPackage("MICROSOFT_AUTHENTICATION_PACKAGE_V1_0"), "online path retains MSV1_0");
        Check(CredentialSerializationTestAccess::PasswordErased(credential), (std::string(label)+": transient password erased").c_str());
        SecureZeroMemory(result.rgbSerialization, result.cbSerialization);
        CoTaskMemFree(result.rgbSerialization);
    }
}
static void TestExistingDomainEnrollment() {
    HANDLE token = nullptr;
    Check(OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token) != FALSE, "domain SID: token query");
    if (!token) return;
    DWORD bytes = 0;
    GetTokenInformation(token, TokenUser, nullptr, 0, &bytes);
    std::vector<BYTE> buffer(bytes);
    if (!GetTokenInformation(token, TokenUser, buffer.data(), bytes, &bytes)) { CloseHandle(token); Check(false,"domain SID: read"); return; }
    CloseHandle(token);
    PSID sid = reinterpret_cast<TOKEN_USER*>(buffer.data())->User.Sid;
    wchar_t name[256]{}, domain[256]{}, computer[256]{};
    DWORD names = ARRAYSIZE(name), domains = ARRAYSIZE(domain), computers = ARRAYSIZE(computer);
    SID_NAME_USE kind{};
    if (!LookupAccountSidW(nullptr, sid, name, &names, domain, &domains, &kind) || !GetComputerNameW(computer, &computers) || _wcsicmp(domain,computer)==0) {
        std::cout << "SKIP domain SID: test requires a domain account\n";
        return;
    }
    PWSTR sidText = nullptr;
    if (!ConvertSidToStringSidW(sid,&sidText)) { Check(false,"domain SID: format"); return; }
    LockFaceCredential credential;
    CredentialSerializationTestAccess::SetSid(credential,sidText);
    LocalFree(sidText);
    CREDENTIAL_PROVIDER_CREDENTIAL_SERIALIZATION result{};
    HRESULT hr = CredentialSerializationTestAccess::Pack(credential,L"MicrosoftAccount",L"incorrect-msa@example.test",CPUS_LOGON,&result);
    Check(SUCCEEDED(hr),"existing enrollment: pack succeeds");
    if (SUCCEEDED(hr)) {
        auto* logon = reinterpret_cast<KERB_INTERACTIVE_UNLOCK_LOGON*>(result.rgbSerialization);
        Check(ReadPackedString(logon->Logon.LogonDomainName,result.rgbSerialization,result.cbSerialization)==domain,"existing enrollment: SID corrects mislabelled domain");
        Check(ReadPackedString(logon->Logon.UserName,result.rgbSerialization,result.cbSerialization)==name,"existing enrollment: SID resolves SAM username");
        SecureZeroMemory(result.rgbSerialization,result.cbSerialization);
        CoTaskMemFree(result.rgbSerialization);
    }
}
int main() {
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    TestNativeAccount(L"CORP", L"DummyUser@corp.example", CPUS_LOGON, "domain login");
    TestNativeAccount(L"CORP", L"DummyUser@corp.example", CPUS_UNLOCK_WORKSTATION, "domain unlock");
    TestNativeAccount(L"TEST-PC", L"", CPUS_LOGON, "local login");
    TestNativeAccount(L"TEST-PC", L"", CPUS_UNLOCK_WORKSTATION, "local unlock");
    TestIdentityProvider(L"MicrosoftAccount","Microsoft account");
    TestIdentityProvider(L"AzureAD","Entra account");
    TestExistingDomainEnrollment();
    using namespace facelogin::account_identity;
    Check(ClassifyAuthority(L"CORP", L"TEST-PC") == Kind::Domain, "AD authority classified as domain");
    Check(ClassifyAuthority(L"test-pc", L"TEST-PC") == Kind::Local, "computer comparison ignores case");
    Check(ClassifyAuthority(L"microsoftaccount", L"TEST-PC") == Kind::MicrosoftAccount, "MSA authority preserved");
    Check(ClassifyAuthority(L"AzureAD", L"TEST-PC") == Kind::AzureAd, "Entra authority excluded from AD");
    Check(ClassifyAuthority(L"CORP", L"") == Kind::Unknown, "missing computer name remains unknown");
    BYTE* invalidBuffer = nullptr;
    DWORD invalidBytes = 0;
    Check(FAILED(facelogin::credential_provider::PackNativeLogon(L"CORP", L"User", L"dummy", CPUS_CREDUI, &invalidBuffer, &invalidBytes)) && !invalidBuffer && !invalidBytes, "unsupported scenario rejected");
    Check(FAILED(facelogin::credential_provider::PackNativeLogon(L"CORP", std::wstring(32768, L'x'), L"dummy", CPUS_LOGON, &invalidBuffer, &invalidBytes)), "oversized UNICODE_STRING rejected");
    CoUninitialize();
    return failures ? 1 : 0;
}
