#pragma once

#include <string>
#include <vector>
#include <cstdint>
#include "../storage/enrollment_protocol.h"

namespace facelogin {

// Synchronous administrative requests. Each method owns its connection,
// bounded wait and CONTROL_ACK; callers never manage a pipe handle.
class EnrollmentServiceClient {
public:
    EnrollmentServiceClient();
    explicit EnrollmentServiceClient(std::wstring pipeName);

    // Database reload retries busy/broken connections; configuration reload
    // allows worker startup. True requires the corresponding service OK.
    bool ReloadDatabase() const;
    bool AccountRequest(const enrollment::Request& request, enrollment::Snapshot& snapshot) const;
    bool DeleteEnrollment(const std::wstring& sid, uint32_t faceId = 0) const;
    bool WithdrawConsent(const std::wstring& sid) const;
    bool ReloadConfig() const;
    bool ProtectPassword(const std::wstring& password, std::vector<uint8_t>& encrypted) const;
    // Returns the service JSON snapshot or the existing JSON error response.
    std::string QueryLearningStatus() const;

private:
    bool DeleteEnrollmentRequest(const std::wstring& sid, uint32_t faceId, bool withdrawal) const;
    std::wstring m_pipeName;
};

} // namespace facelogin
