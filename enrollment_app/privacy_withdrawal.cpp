#include "privacy_withdrawal.h"
#include "../common/logger.h"

namespace facelogin {
namespace {
constexpr wchar_t kPending[] = L"ConsentWithdrawalPending";

bool ReportRegistryError(const wchar_t* operation, LSTATUS status) {
    FACELOGIN_ERROR(L"Privacy withdrawal registry %s failed (%ld)", operation, status);
    return false;
}
} // namespace

PrivacyWithdrawalState PrivacyWithdrawal::Read() const {
    DWORD value = 0, size = sizeof(value);
    const auto status = RegGetValueW(m_root, m_path.c_str(), kPending,
        RRF_RT_REG_DWORD, nullptr, &value, &size);
    if (status == ERROR_FILE_NOT_FOUND || status == ERROR_PATH_NOT_FOUND)
        return PrivacyWithdrawalState::None;
    if (status != ERROR_SUCCESS || value != 1) {
        ReportRegistryError(L"read", status == ERROR_SUCCESS ? ERROR_INVALID_DATA : status);
        return PrivacyWithdrawalState::Unavailable;
    }
    return PrivacyWithdrawalState::Pending;
}

bool PrivacyWithdrawal::Begin() const {
    HKEY key = nullptr;
    auto status = RegCreateKeyExW(m_root, m_path.c_str(), 0, nullptr, 0,
                                 KEY_SET_VALUE | KEY_QUERY_VALUE, nullptr, &key, nullptr);
    if (status != ERROR_SUCCESS) return ReportRegistryError(L"open", status);
    const DWORD pending = 1;
    status = RegSetValueExW(key, kPending, 0, REG_DWORD,
                           reinterpret_cast<const BYTE*>(&pending), sizeof(pending));
    if (status == ERROR_SUCCESS) status = RegFlushKey(key);
    RegCloseKey(key);
    if (status != ERROR_SUCCESS) return ReportRegistryError(L"begin", status);
    return true;
}

bool PrivacyWithdrawal::Complete() const {
    HKEY key = nullptr;
    auto status = RegOpenKeyExW(m_root, m_path.c_str(), 0, KEY_SET_VALUE | KEY_QUERY_VALUE, &key);
    if (status != ERROR_SUCCESS) return ReportRegistryError(L"open completion", status);
    for (const auto name : {L"ConsentTime", L"ConsentVersion"}) {
        status = RegDeleteValueW(key, name);
        if (status != ERROR_SUCCESS && status != ERROR_FILE_NOT_FOUND) {
            RegCloseKey(key);
            return ReportRegistryError(L"complete", status);
        }
    }
    // Make removal of the old acceptance durable before dropping the marker.
    // A crash between these flushes must reopen as pending or unaccepted.
    status = RegFlushKey(key);
    if (status == ERROR_SUCCESS) status = RegDeleteValueW(key, kPending);
    if (status != ERROR_SUCCESS) {
        RegCloseKey(key);
        return ReportRegistryError(L"persist consent cleanup", status);
    }
    status = RegFlushKey(key);
    if (status != ERROR_SUCCESS) {
        // Keep the retry marker visible if flushing completion fails.
        const DWORD pending = 1;
        RegSetValueExW(key, kPending, 0, REG_DWORD,
                      reinterpret_cast<const BYTE*>(&pending), sizeof(pending));
        RegFlushKey(key);
    }
    RegCloseKey(key);
    if (status != ERROR_SUCCESS) return ReportRegistryError(L"flush completion", status);
    return true;
}

bool PrivacyWithdrawal::Run(const std::function<bool()>& removeAccount) const {
    return Begin() && removeAccount() && Complete();
}

} // namespace facelogin
