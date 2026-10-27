#include "enrollment_accounts.h"
#include "../common/logger.h"
#include "../common/json_util.h"
#include <sstream>
#include <utility>

namespace facelogin {
EnrollmentAccounts::EnrollmentAccounts(const std::wstring&,
    session_identity::SessionIdentity identity, const EnrollmentServiceClient& serviceClient)
    : m_identity(std::move(identity)), m_serviceClient(serviceClient) {}

bool EnrollmentAccounts::QueryAccount(enrollment::Snapshot& account) const {
    enrollment::Request request;
    request.sid = session_identity::GetCurrentProcessUserSid();
    return !request.sid.empty() && m_serviceClient.AccountRequest(request, account);
}

bool EnrollmentAccounts::SaveTemplates(const std::vector<EnrollmentTemplate>& templates,
    const std::wstring& password, const std::wstring& label, bool append,
    EnrollmentSaveOutcome& outcome) {
    outcome = {};
    m_saveStatus = "failed";
    if (templates.empty() || templates.size() > kMaxFacesPerUser) return false;
    const auto sid = session_identity::GetCurrentProcessUserSid();
    if (sid.empty() || (append && sid != m_identity.sid)) return false;
    enrollment::Snapshot current;
    if (!QueryAccount(current)) return false;
    enrollment::Request request;
    request.operation = enrollment::Operation::SaveTemplates;
    request.sid = sid;
    request.username = m_identity.username;
    request.upn = m_identity.upn;
    request.replace = templates.size() > 1;
    if (!current.found && (password.empty() ||
        !m_serviceClient.ProtectPassword(password, request.encryptedPassword))) return false;
    request.faces.reserve(templates.size());
    for (const auto& prepared : templates) {
        FaceRecord face;
        face.embedding = prepared.embedding;
        face.label = templates.size() > 1 || label.empty() ? prepared.label : label;
        face.nominalYaw = static_cast<float>(prepared.targetYaw);
        face.nominalPitch = 0;
        request.faces.push_back(std::move(face));
    }
    enrollment::Snapshot saved;
    const bool confirmed = m_serviceClient.AccountRequest(request, saved);
    outcome.persisted = confirmed;
    m_saveStatus = confirmed ? "saved" : "failed";
    return confirmed;
}

int EnrollmentAccounts::GetFaceCount() {
    enrollment::Snapshot account;
    return QueryAccount(account) ? static_cast<int>(account.faces.size()) : 0;
}

std::string EnrollmentAccounts::GetFacesJson() {
    enrollment::Snapshot account;
    if (!QueryAccount(account)) return "[]";
    std::ostringstream js;
    js << "[";
    for (size_t i = 0; i < account.faces.size(); ++i) {
        if (i) js << ",";
        js << "{\"id\":" << account.faces[i].id << ",\"label\":\""
           << JsonEscapeUtf8(account.faces[i].label) << "\"}";
    }
    js << "]";
    return js.str();
}

bool EnrollmentAccounts::DeleteFace(int faceId) {
    m_saveStatus = "failed";
    if (faceId <= 0) return false;
    const bool confirmed = m_serviceClient.DeleteEnrollment(session_identity::GetCurrentProcessUserSid(), static_cast<uint32_t>(faceId));
    m_saveStatus = confirmed ? "saved" : "failed";
    return confirmed;
}

bool EnrollmentAccounts::ClearAllFaces() {
    const bool confirmed = m_serviceClient.DeleteEnrollment(session_identity::GetCurrentProcessUserSid());
    m_saveStatus = confirmed ? "saved" : "failed";
    return confirmed;
}

bool EnrollmentAccounts::RenameFace(int faceId, const std::wstring& label) {
    m_saveStatus = "failed";
    if (faceId <= 0) return false;
    enrollment::Request request;
    request.operation = enrollment::Operation::Rename;
    request.sid = session_identity::GetCurrentProcessUserSid();
    request.faceId = static_cast<uint32_t>(faceId);
    request.label = label;
    enrollment::Snapshot saved;
    const bool confirmed = !request.sid.empty() && m_serviceClient.AccountRequest(request, saved);
    m_saveStatus = confirmed ? "saved" : "failed";
    return confirmed;
}

EnrollmentAccounts::AccountChange EnrollmentAccounts::ReadAccountChange() {
    AccountChange change;
    const auto current = session_identity::ReadSessionIdentity();
    if (current.accountType != "local" && current.accountType != "msa") return change;
    enrollment::Snapshot account;
    if (!QueryAccount(account) || !account.found) return change;
    change.currentUpn = current.upn;
    change.faces = account.faces.size();
    if (current.accountType == "local" && session_identity::HasUpn(account.upn)) change.state = 1;
    else if (current.accountType == "msa" && account.upn != current.upn) change.state = 2;
    return change;
}

int EnrollmentAccounts::GetAccountTypeChanged() { return ReadAccountChange().state; }

std::string EnrollmentAccounts::CheckAccountTypeChanged() {
    const auto change = ReadAccountChange();
    if (change.state == 0) return "{\"state\":0}";
    std::string json = "{\"state\":" + std::to_string(change.state) +
        ",\"faces\":" + std::to_string(change.faces);
    if (change.state == 2) json += ",\"upn\":\"" + JsonEscapeUtf8(change.currentUpn) + "\"";
    return json + "}";
}

bool EnrollmentAccounts::UpdateIdentity(const enrollment::Snapshot& account,
    const std::wstring& upn, const std::vector<uint8_t>& password) {
    m_saveStatus = "failed";
    if (!account.found) return false;
    enrollment::Request request;
    request.operation = enrollment::Operation::UpdateIdentity;
    request.previousSid = account.sid;
    request.sid = session_identity::GetCurrentProcessUserSid();
    request.username = m_identity.username;
    request.upn = upn;
    request.encryptedPassword = password;
    enrollment::Snapshot saved;
    const bool confirmed = !request.sid.empty() && m_serviceClient.AccountRequest(request, saved);
    m_saveStatus = confirmed ? "saved" : "failed";
    return confirmed;
}

bool EnrollmentAccounts::RefreshAccountIdentity(const std::wstring& password) {
    const int state = GetAccountTypeChanged();
    if (!state || password.empty() || !session_identity::ValidatePassword(m_identity, password)) return false;
    std::vector<uint8_t> encrypted;
    if (!m_serviceClient.ProtectPassword(password, encrypted)) return false;
    enrollment::Snapshot account;
    if (!QueryAccount(account)) return false;
    return UpdateIdentity(account, state == 2 ? session_identity::ResolveSessionUpn() : L"", encrypted);
}

bool EnrollmentAccounts::ClearStaleAccountUpn() {
    if (GetAccountTypeChanged() != 1) return false;
    enrollment::Snapshot account;
    if (!QueryAccount(account) || account.upn.find(L'@') == std::wstring::npos) return false;
    return UpdateIdentity(account, L"");
}

bool EnrollmentAccounts::AutoRepairEmptyUpnOnStartup() {
    const auto current = session_identity::ReadSessionIdentity();
    if (current.accountType != "msa" || current.upn.empty()) return false;
    enrollment::Snapshot account;
    if (!QueryAccount(account) || !account.found || !account.upn.empty() ||
        account.sid != session_identity::GetCurrentProcessUserSid()) return false;
    return UpdateIdentity(account, current.upn);
}

bool EnrollmentAccounts::AutoRepairMachineSidOnStartup() {
    const auto sid = session_identity::GetCurrentProcessUserSid();
    if (sid.empty() || !session_identity::SidIsUserAccount(sid)) return false;
    enrollment::Snapshot account;
    if (!QueryAccount(account) || !account.found || account.sid == sid ||
        session_identity::SidIsUserAccount(account.sid)) return false;
    return UpdateIdentity(account, account.upn);
}
} // namespace facelogin
