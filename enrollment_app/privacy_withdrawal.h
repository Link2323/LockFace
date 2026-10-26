#pragma once

#include <windows.h>
#include <functional>
#include <string>

namespace facelogin {

enum class PrivacyWithdrawalState { None, Pending, Unavailable };

// The marker is independent of ConsentVersion/ConsentTime, so interrupted
// registry cleanup cannot turn a pending withdrawal into remembered consent.
class PrivacyWithdrawal {
public:
    explicit PrivacyWithdrawal(HKEY root = HKEY_CURRENT_USER,
                               const wchar_t* path = L"SOFTWARE\\LockFace")
        : m_root(root), m_path(path) {}
    PrivacyWithdrawalState Read() const;
    // Persist and flush intent BEFORE invoking the service. Clear the marker
    // last, only after confirmed deletion and consent-record cleanup.
    bool Run(const std::function<bool()>& removeAccount) const;

private:
    bool Begin() const;
    bool Complete() const;
    HKEY m_root;
    std::wstring m_path;
};

} // namespace facelogin
