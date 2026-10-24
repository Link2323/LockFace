#include "account_identity.h"
#include <sddl.h>

namespace facelogin::account_identity {
bool ResolveUserSid(const std::wstring& sidText, std::wstring& domain, std::wstring& username) {
    domain.clear();
    username.clear();
    if (sidText.empty()) return false;
    PSID sid = nullptr;
    if (!ConvertStringSidToSidW(sidText.c_str(), &sid)) return false;
    DWORD nameChars = 0, domainChars = 0;
    SID_NAME_USE kind{};
    LookupAccountSidW(nullptr, sid, nullptr, &nameChars, nullptr, &domainChars, &kind);
    bool resolved = false;
    if (GetLastError() == ERROR_INSUFFICIENT_BUFFER && nameChars && domainChars) {
        std::wstring name(nameChars, L'\0'), authority(domainChars, L'\0');
        if (LookupAccountSidW(nullptr, sid, name.data(), &nameChars, authority.data(), &domainChars, &kind) && kind == SidTypeUser && name[0] && authority[0]) {
            name.resize(wcslen(name.c_str()));
            authority.resize(wcslen(authority.c_str()));
            domain = authority;
            username = name;
            resolved = true;
        }
    }
    LocalFree(sid);
    return resolved;
}
}
