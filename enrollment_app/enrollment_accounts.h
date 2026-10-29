#pragma once

#include "enrollment_templates.h"
#include "service_client.h"
#include "session_identity.h"
#include "../storage/credential_store.h"

namespace facelogin {

struct EnrollmentSaveOutcome {
    bool persisted = false;
};

// UI-thread account operations. The SYSTEM service owns database access;
// only account metadata is returned. No existing template or password is read.
class EnrollmentAccounts {
public:
    EnrollmentAccounts(const std::wstring& dataDir,
                       session_identity::SessionIdentity identity,
                       const EnrollmentServiceClient& serviceClient);

    bool SaveTemplates(const std::vector<EnrollmentTemplate>& templates,
                       const std::wstring& password, const std::wstring& label,
                       bool append, EnrollmentSaveOutcome& outcome);
    int GetFaceCount();
    std::string GetFacesJson();
    bool DeleteFace(int faceId);
    bool ClearAllFaces();
    bool WithdrawConsent();
    bool RenameFace(int faceId, const std::wstring& label);
    std::string GetSaveStatus() const { return m_saveStatus; }
    int GetAccountTypeChanged();
    std::string CheckAccountTypeChanged();
    bool RefreshAccountIdentity(const std::wstring& password);
    bool ClearStaleAccountUpn();
    bool AutoRepairEmptyUpnOnStartup();
    // Rewrites this session user's record when it carries a non-user SID (the
    // machine SID a pre-fix build stored where computer name == user name).
    bool AutoRepairMachineSidOnStartup();

private:
    struct AccountChange {
        int state = 0;
        size_t faces = 0;
        std::wstring currentUpn;
    };
    AccountChange ReadAccountChange();
    bool QueryAccount(enrollment::Snapshot& account) const;
    bool UpdateIdentity(const enrollment::Snapshot& account, const std::wstring& upn,
                        const std::vector<uint8_t>& password = {});
    std::string m_saveStatus = "failed";
    const session_identity::SessionIdentity m_identity;
    const EnrollmentServiceClient& m_serviceClient;
};

} // namespace facelogin
