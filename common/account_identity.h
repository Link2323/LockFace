#pragma once
#include <windows.h>
#include <string>
namespace facelogin::account_identity {
enum class Kind { Unknown, Local, Domain, MicrosoftAccount, AzureAd };
// UPN syntax alone cannot distinguish AD from Microsoft accounts.
inline Kind ClassifyAuthority(const std::wstring& authority, const std::wstring& computer) {
    if (authority.empty()) return Kind::Unknown;
    if (_wcsicmp(authority.c_str(), L"MicrosoftAccount") == 0) return Kind::MicrosoftAccount;
    if (_wcsicmp(authority.c_str(), L"AzureAD") == 0) return Kind::AzureAd;
    if (authority == L"." || (!computer.empty() && _wcsicmp(authority.c_str(), computer.c_str()) == 0)) return Kind::Local;
    return computer.empty() ? Kind::Unknown : Kind::Domain;
}
// User SIDs only; outputs are cleared on failure.
bool ResolveUserSid(const std::wstring& sidText, std::wstring& domain, std::wstring& username);
}
