#pragma once

#include <string>

namespace facelogin::session_identity {

// Snapshot used for display and locating enrollment records. Live account
// changes and password checks re-query the process session, not users.dat.
struct SessionIdentity {
    std::wstring username;
    std::wstring upn;
    std::wstring sid;
    std::wstring domain;
    std::string accountType;
};

SessionIdentity ReadSessionIdentity();
std::wstring ResolveSessionUpn();
std::wstring GetCurrentProcessUserSid();
// True only when the SID string resolves to a user account on this machine —
// machine/well-known/unresolvable SIDs are not account identity.
bool SidIsUserAccount(const std::wstring& sid);
bool HasUpn(const std::wstring& upn);
// Borrows the password for this call; never stores or logs it.
bool ValidatePassword(const SessionIdentity& identity, const std::wstring& password);

} // namespace facelogin::session_identity
