#include "ipc_protocol.h"
#include <algorithm>
#include <sddl.h>

namespace facelogin {
namespace ipc {

static bool IsValidSid(const std::wstring& value) {
    if (value.empty()) return false;
    PSID sid = nullptr;
    const BOOL valid = ConvertStringSidToSidW(value.c_str(), &sid);
    if (sid) LocalFree(sid);
    return valid == TRUE;
}

AuthResult ParseAuthMessage(const std::wstring& message) {
    AuthResult result;

    if (message.empty()) {
        result.status = AuthResult::Status::Error;
        result.errorMessage = L"Empty message received";
        return result;
    }

    // Success format: "AUTH_SUCCESS:SID:UPN:USERNAME:PASSWORD".
    if (message.starts_with(MSG_AUTH_SUCCESS_PREFIX)) {
        // Parse in place on the caller-owned message. The password is copied
        // exactly once, straight into result.password (whose receiver guards
        // and zeroes it); no intermediate payload/passwordPart strings may
        // ever hold the password, because a plain std::wstring destructor
        // frees without scrubbing and leaves plaintext in the heap (security
        // audit 1.9.0, finding 1).
        const size_t prefixLen = wcslen(MSG_AUTH_SUCCESS_PREFIX);

        // The first three colons delimit SID, UPN, and username; the password
        // is the remaining suffix and may itself contain colons.
        size_t colonCount = 0;
        for (size_t i = prefixLen; i < message.size(); ++i) {
            if (message[i] == L':') colonCount++;
        }

        if (colonCount >= 3) {
            size_t pos1 = message.find(L':', prefixLen);
            if (pos1 == std::wstring::npos) { result.status = AuthResult::Status::Error; result.errorMessage = L"Malformed AUTH_SUCCESS"; return result; }

            size_t pos2 = message.find(L':', pos1 + 1);
            if (pos2 == std::wstring::npos) { result.status = AuthResult::Status::Error; result.errorMessage = L"Malformed AUTH_SUCCESS"; return result; }

            size_t pos3 = message.find(L':', pos2 + 1);
            if (pos3 == std::wstring::npos) { result.status = AuthResult::Status::Error; result.errorMessage = L"Malformed AUTH_SUCCESS"; return result; }

            result.sid.assign(message, prefixLen, pos1 - prefixLen);
            result.upn.assign(message, pos1 + 1, pos2 - pos1 - 1);

            // The backslash must sit inside the DOMAIN\USER segment between
            // pos2 and pos3 — not first, not last, and never beyond pos3 in
            // the password suffix (npos + 1 wraps, so test npos first).
            const size_t userBegin = pos2 + 1;
            const size_t userEnd = pos3;
            const size_t slashPos = message.find(L'\\', userBegin);
            const bool slashValid = slashPos != std::wstring::npos &&
                                    slashPos != userBegin &&
                                    slashPos + 1 < userEnd;
            if (!IsValidSid(result.sid) || !slashValid ||
                pos3 + 1 >= message.size()) {
                result.status = AuthResult::Status::Error;
                result.errorMessage = L"Malformed AUTH_SUCCESS: invalid required field";
                return result;
            }

            result.domain.assign(message, userBegin, slashPos - userBegin);
            result.username.assign(message, slashPos + 1, userEnd - slashPos - 1);

            result.password.assign(message, pos3 + 1, std::wstring::npos);
            result.status = AuthResult::Status::Success;
            return result;
        }
        result.status = AuthResult::Status::Error;
        result.errorMessage = L"Malformed AUTH_SUCCESS: expected current format";
        return result;
    }

    if (message == MSG_AUTH_TIMEOUT) {
        result.status = AuthResult::Status::Timeout;
        return result;
    }

    if (message.starts_with(MSG_AUTH_ERROR_PREFIX)) {
        result.status = AuthResult::Status::Error;
        result.errorMessage = message.substr(wcslen(MSG_AUTH_ERROR_PREFIX));
        return result;
    }

    // Unknown message
    result.status = AuthResult::Status::Error;
    result.errorMessage = L"Unknown message: " + message;
    return result;
}

std::wstring BuildAuthSuccessMessage(const std::wstring& sid,
                                      const std::wstring& upn,
                                      const std::wstring& domain,
                                      const std::wstring& username,
                                      const std::wstring& password) {
    // Format: SID:UPN:DOMAIN\USERNAME:PASSWORD
    // Append each field directly into one reserved buffer. Chained operator+
    // expressions create extra temporary strings containing the plaintext
    // password, which cannot be explicitly scrubbed by the caller.
    std::wstring msg;
    msg.reserve(wcslen(MSG_AUTH_SUCCESS_PREFIX) + sid.size() + upn.size() +
                domain.size() + username.size() + password.size() + 4);
    msg.append(MSG_AUTH_SUCCESS_PREFIX)
       .append(sid).push_back(L':');
    msg.append(upn).push_back(L':');
    msg.append(domain).push_back(L'\\');
    msg.append(username).push_back(L':');
    msg.append(password);
    return msg;
}

std::wstring BuildAuthErrorMessage(const std::wstring& error) {
    std::wstring msg(MSG_AUTH_ERROR_PREFIX);
    msg += error;
    return msg;
}

} // namespace ipc
} // namespace facelogin
