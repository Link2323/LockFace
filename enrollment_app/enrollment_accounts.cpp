#include "enrollment_accounts.h"
#include "../common/logger.h"
#include "../common/json_util.h"

#include <cmath>
#include <sstream>
#include <utility>

namespace facelogin {

EnrollmentAccounts::EnrollmentAccounts(const std::wstring& dataDir,
    session_identity::SessionIdentity identity, const EnrollmentServiceClient& serviceClient)
    : m_identity(std::move(identity)), m_serviceClient(serviceClient) {
    m_store.SetDataDir(dataDir);
}

bool EnrollmentAccounts::SaveAndReload(bool* persisted) {
    m_saveStatus = "failed";
    if (persisted) *persisted = false;
    if (!m_store.SaveDatabase()) {
        FACELOGIN_ERROR(L"Enrollment account update: database save failed");
        return false;
    }
    if (persisted) *persisted = true;
    const bool confirmed = m_serviceClient.ReloadDatabase();
    m_saveStatus = confirmed ? "saved" : "reload_failed";
    return confirmed;
}

bool EnrollmentAccounts::SaveTemplates(const std::vector<EnrollmentTemplate>& templates,
    const std::wstring& password, const std::wstring& label, bool append,
    EnrollmentSaveOutcome& outcome) {
    outcome = {};
    if (templates.empty()) {
        FACELOGIN_ERROR(L"Enrollment save refused: no prepared templates");
        return false;
    }
    if (append) {
        // The appended face belongs to the logged-on session owner — the session
        // token SID must match the enrolled account. No password is required for
        // an append because the stored password is retained.
        std::wstring tokenSid = session_identity::GetCurrentProcessUserSid();
        if (tokenSid.empty() || tokenSid != m_identity.sid) {
            FACELOGIN_ERROR(L"Face append refused: token SID %s != enrolled SID %s",
                            tokenSid.c_str(), m_identity.sid.c_str());
            return false;
        }
    }
    const bool multiAngle = templates.size() > 1;

    if (!m_store.LoadDatabase()) return false;

    // --- Slot pre-check (bug4 fix) ---
    // Two-level cap: kMaxUsers accounts total, kMaxFacesPerUser faces per account.
    // Multi-angle re-enrollment REPLACES old faces (not appends) so it never
    // overflows per-account; single-angle append and new-account creation are
    // checked below.
    size_t existingIdx = m_store.FindUserIndex(m_identity.sid, m_identity.upn, m_identity.username);
    const bool accountExists = existingIdx < m_store.GetUsers().size();
    if (accountExists) {
        if (multiAngle) {
            // Multi-angle re-enrollment: clear old faces, keep account identity.
            if (!m_store.ClearFacesForAccount(m_identity.sid)) {
                FACELOGIN_ERROR(L"Failed to clear existing faces for multi-angle re-enrollment");
                return false;
            }
            // After clearing, the account still exists in m_users (identity +
            // password intact, faces empty). The loop below will find it via
            // FindUserIndex → all angles use the append branch, which preserves
            // the stored password. End result: old faces replaced, account
            // identity untouched.
        } else {
            // Single-angle append: check per-account face slots.
            size_t currentFaces = m_store.GetUsers()[existingIdx].faces.size();
            if (currentFaces + templates.size() > facelogin::kMaxFacesPerUser) {
                FACELOGIN_ERROR(L"Not enough face slots: %s has %zu face(s), "
                               L"need %zu more (max %zu per account). "
                               L"Delete unused faces or re-enroll with multi-angle to replace all.",
                               m_identity.username.c_str(), currentFaces, templates.size(),
                               facelogin::kMaxFacesPerUser);
                return false;
            }
        }
    } else {
        // New account: check global user cap.
        if (m_store.GetUsers().size() >= facelogin::kMaxUsers) {
            FACELOGIN_ERROR(L"Cannot create new account: database has %zu users (max %zu). "
                           L"Delete an unused account first.",
                           m_store.GetUsers().size(), facelogin::kMaxUsers);
            return false;
        }
    }

    for (const auto& prepared : templates) {
        const auto& ef = prepared.embedding;
        // create-or-append (1.3.0): the same account may hold several faces.
        // First-time enrollment stores the (protected) password and face #1;
        // subsequent enrollments APPEND a face and leave the stored password
        // untouched (the user is the logged-on session owner, already trusted).
        // In multi-angle mode each angle is one AddFace: the create branch
        // runs for angle 0, the append branch for angles 1-2.
        // Label: multi-angle always uses the angle name. Single-angle keeps a
        // user-provided name, falling back to the angle name (正面/左转/右转)
        // so an appended face is not labeled with the generic 脸N.
        std::wstring groupLabel = label;
        if (multiAngle || groupLabel.empty()) groupLabel = prepared.label;
        size_t idx = m_store.FindUserIndex(m_identity.sid, m_identity.upn, m_identity.username);
        uint32_t newFaceId = 0;
        if (idx >= m_store.GetUsers().size()) {
            // First face for this account — protect the password now.
            if (password.empty()) {
                FACELOGIN_ERROR(L"Enrollment refused: a Windows account password is required");
                return false;
            }
            std::vector<uint8_t> protectedPassword;
            if (!m_serviceClient.ProtectPassword(password, protectedPassword)) {
                FACELOGIN_ERROR(L"Service password protection failed");
                return false;
            }
            if (!m_store.AddFace(m_identity.username, m_identity.upn, m_identity.sid, protectedPassword, ef,
                                 groupLabel, &newFaceId,
                                 static_cast<float>(prepared.targetYaw), 0.0f)) {
                FACELOGIN_ERROR(L"Failed to create enrollment for %s", m_identity.username.c_str());
                return false;
            }
        } else {
            // Append a face to an existing account. AddFace ignores the password
            // argument here, so the stored password is preserved.
            if (m_store.GetUsers()[idx].faces.size() >= facelogin::kMaxFacesPerUser) {
                FACELOGIN_ERROR(L"Cannot append: %s already has %zu faces (max %zu)",
                                m_identity.username.c_str(), m_store.GetUsers()[idx].faces.size(),
                                facelogin::kMaxFacesPerUser);
                return false;
            }
            if (!m_store.AddFace(m_identity.username, m_identity.upn, m_identity.sid, {}, ef, groupLabel,
                                 &newFaceId,
                                 static_cast<float>(prepared.targetYaw), 0.0f)) {
                FACELOGIN_ERROR(L"Failed to append face for %s", m_identity.username.c_str());
                return false;
            }
        }
        FACELOGIN_INFO(L"Enrollment saved for: %s (face #%u, emb=%zu-D, angle=%ls, "
                       L"nominalYaw=%d)",
                       m_identity.username.c_str(), newFaceId, ef.size(), prepared.label.c_str(),
                       prepared.targetYaw);
    }

    return SaveAndReload(&outcome.persisted);
}

int EnrollmentAccounts::GetFaceCount() {
    if (!m_store.LoadDatabase()) return 0;
    return static_cast<int>(m_store.GetFaceCount(m_identity.sid));
}

std::string EnrollmentAccounts::GetFacesJson() {
    if (!m_store.LoadDatabase()) return "[]";
    size_t idx = m_store.FindUserIndex(m_identity.sid, m_identity.upn, m_identity.username);
    if (idx >= m_store.GetUsers().size()) return "[]";

    std::ostringstream js;
    js << "[";
    const auto& faces = m_store.GetUsers()[idx].faces;
    for (size_t i = 0; i < faces.size(); i++) {
        if (i > 0) js << ",";
        const auto& f = faces[i];
        js << "{\"id\":" << f.id
           << ",\"label\":\"" << JsonEscapeUtf8(f.label) << "\"}";
    }
    js << "]";
    return js.str();
}

bool EnrollmentAccounts::DeleteFace(int faceId) {
    m_saveStatus = "failed";
    if (faceId <= 0) return false;
    const bool confirmed = m_serviceClient.DeleteEnrollment(m_identity.sid, static_cast<uint32_t>(faceId));
    m_saveStatus = confirmed ? "saved" : "failed";
    return confirmed;
}

bool EnrollmentAccounts::ClearAllFaces() {
    m_saveStatus = "failed";
    const bool confirmed = m_serviceClient.DeleteEnrollment(m_identity.sid);
    m_saveStatus = confirmed ? "saved" : "failed";
    return confirmed;
}

bool EnrollmentAccounts::RenameFace(int faceId, const std::wstring& label) {
    m_saveStatus = "failed";
    if (faceId <= 0) return false;
    if (!m_store.LoadDatabase()) return false;
    if (!m_store.RenameFace(m_identity.sid, static_cast<uint32_t>(faceId), label)) return false;
    return SaveAndReload();
}

// Detect a stale account-type record (symmetric MSA ↔ local). We never trust
// m_identity.upn/m_identity.accountType here — they are derived from the session at construction
// and could be empty for local accounts; instead we re-query the CURRENT
// session identity with session_identity::ReadSessionIdentity() (docs/todo.md bug1):
//   SID authority distinguishes domain accounts before MSA/local comparison.
// Then compare against the stored record (matched by SID, which Windows keeps
// across MSA↔local conversions):
//   local + record UPN is an MSA email  → state 1 (MSA→local, clear UPN)
//   MSA   + record UPN empty/different  → state 2 (local→MSA, write current UPN)
//   everything else                     → state 0 (no refresh needed)
EnrollmentAccounts::AccountChange EnrollmentAccounts::ReadAccountChange() {
    AccountChange change;
    const auto current = session_identity::ReadSessionIdentity();
    // Domain UPNs are not MSA bindings.
    if (current.accountType != "local" && current.accountType != "msa") return change;
    change.currentUpn = current.upn;
    const bool sessionIsMsa = current.accountType == "msa";
    if (!m_store.LoadDatabase()) return change;
    const auto tokenSid = session_identity::GetCurrentProcessUserSid();
    const auto idx = m_store.FindUserIndex(tokenSid, m_identity.upn, m_identity.username);
    if (idx >= m_store.GetUsers().size()) return change;
    const auto& record = m_store.GetUsers()[idx];
    change.faces = record.faces.size();
    if (!sessionIsMsa && session_identity::HasUpn(record.upn)) change.state = 1;
    else if (sessionIsMsa && record.upn != change.currentUpn) change.state = 2;
    return change;
}

int EnrollmentAccounts::GetAccountTypeChanged() {
    return ReadAccountChange().state;
}

std::string EnrollmentAccounts::CheckAccountTypeChanged() {
    const auto change = ReadAccountChange();
    if (change.state == 0) return "{\"state\":0}";
    std::string json = "{\"state\":" + std::to_string(change.state) +
        ",\"faces\":" + std::to_string(change.faces);
    if (change.state == 2) json += ",\"upn\":\"" + JsonEscapeUtf8(change.currentUpn) + "\"";
    return json + "}";
}

bool EnrollmentAccounts::RefreshAccountIdentity(const std::wstring& password) {
    int state = GetAccountTypeChanged();
    if (state == 0) {
        FACELOGIN_WARN(L"RefreshAccountIdentity: no stale record to refresh");
        return false;
    }
    if (password.empty()) {
        FACELOGIN_WARN(L"RefreshAccountIdentity: empty password");
        return false;
    }

    // Validate the CURRENT password before touching the database.
    // ValidatePassword routes by live SID authority: MSA goes
    // through CloudAP online/cached validation, local uses LogonUserW(".").
    if (!session_identity::ValidatePassword(m_identity, password)) {
        FACELOGIN_WARN(L"RefreshAccountIdentity: password validation failed");
        return false;
    }

    // Only the SYSTEM service may create stored password ciphertext.
    std::vector<uint8_t> protectedPassword;
    if (!m_serviceClient.ProtectPassword(password, protectedPassword)) {
        FACELOGIN_ERROR(L"RefreshAccountIdentity: service password protection failed");
        return false;
    }

    std::wstring tokenSid = session_identity::GetCurrentProcessUserSid();
    if (!m_store.LoadDatabase()) return false;
    size_t idx = m_store.FindUserIndex(tokenSid, m_identity.upn, m_identity.username);
    if (idx >= m_store.GetUsers().size()) {
        FACELOGIN_WARN(L"RefreshAccountIdentity: record vanished before update");
        return false;
    }

    // Write the identity matching the CURRENT account type:
    //   state 1 (MSA→local): clear the old MSA UPN.
    //   state 2 (local→MSA): write the current MSA email (session UPN).
    std::wstring newUpn;
    if (state == 2) {
        newUpn = session_identity::ResolveSessionUpn();
    }
    // state 1 → newUpn stays empty (local account).

    if (!m_store.UpdateAccountIdentity(idx, m_identity.username, newUpn, tokenSid, protectedPassword)) {
        FACELOGIN_ERROR(L"RefreshAccountIdentity: identity update failed");
        return false;
    }
    bool persisted = false;
    const bool reloadConfirmed = SaveAndReload(&persisted);
    if (!persisted) return false;
    FACELOGIN_INFO(L"RefreshAccountIdentity: refreshed identity of %s (UPN=%s%s, faces preserved)",
                   m_identity.username.c_str(),
                   newUpn.empty() ? L"<cleared>" : newUpn.c_str(),
                   state == 2 ? L", MSA" : L", local");
    return reloadConfirmed;
}

// Dismiss path for the stale-account prompt. Unlike RefreshAccountIdentity
// this needs NO user input: it only drops the '@'-carrying UPN that a buggy
// build wrote into a LOCAL account's record (docs/todo.md bug1), keeping
// username, SID, faces and the DPAPI-encrypted password byte-for-byte intact.
//   - state 1 (current session local + record carries an MSA email): the
//     email is either a misattribution (another account / converted MSA) —
//     clearing it makes the lock-screen pack domain\username, fixing the
//     silent login failure — or a genuine MSA→local conversion where the user
//     simply doesn't want to re-enter the password; identity is corrected and
//     if the password changed they should still run the full refresh.
//   - state 2 (local→MSA) is deliberately NOT touched: the record needs the
//     CURRENT email + re-encrypted password, which only the refresh provides.
bool EnrollmentAccounts::ClearStaleAccountUpn() {
    int state = GetAccountTypeChanged();
    if (state != 1) {
        FACELOGIN_INFO(L"ClearStaleAccountUpn: not a stale MSA→local record (state=%d), no-op", state);
        return false;
    }

    std::wstring tokenSid = session_identity::GetCurrentProcessUserSid();
    if (!m_store.LoadDatabase()) return false;
    size_t idx = m_store.FindUserIndex(tokenSid, m_identity.upn, m_identity.username);
    if (idx >= m_store.GetUsers().size()) {
        FACELOGIN_WARN(L"ClearStaleAccountUpn: record vanished before update");
        return false;
    }
    const auto& rec = m_store.GetUsers()[idx];
    if (rec.upn.find(L'@') == std::wstring::npos) {
        FACELOGIN_INFO(L"ClearStaleAccountUpn: record already has no MSA email, no-op");
        return false;
    }

    // Pass the record's own encryptedPassword back unchanged so the stored
    // credential is not re-encrypted (only the identity email is dropped).
    if (!m_store.UpdateAccountIdentity(idx, rec.username, L"", tokenSid, rec.encryptedPassword)) {
        FACELOGIN_ERROR(L"ClearStaleAccountUpn: identity update failed");
        return false;
    }
    bool persisted = false;
    const bool reloadConfirmed = SaveAndReload(&persisted);
    if (!persisted) return false;
    FACELOGIN_INFO(L"ClearStaleAccountUpn: cleared stale MSA UPN for %s (faces=%zu, password untouched)",
                   rec.username.c_str(), rec.faces.size());
    return reloadConfirmed;
}

bool EnrollmentAccounts::AutoRepairEmptyUpnOnStartup() {
    // Re-derive the current session's MSA identity (GetUserNameExW + token
    // fallback). Only proceed when we actually have an MSA email to write.
    const auto current = session_identity::ReadSessionIdentity();
    std::wstring curUpn = current.upn;
    if (current.accountType != "msa") {
        // Local session (or detection inconclusive) — nothing to repair.
        return false;
    }

    std::wstring tokenSid = session_identity::GetCurrentProcessUserSid();
    if (tokenSid.empty()) {
        FACELOGIN_WARN(L"AutoRepairEmptyUpnOnStartup: could not resolve current session SID, skipping");
        return false;
    }

    if (!m_store.LoadDatabase()) return false;
    bool modified = false;
    for (size_t idx = 0; idx < m_store.GetUsers().size(); ++idx) {
        const auto& rec = m_store.GetUsers()[idx];
        // Only THIS session user's own record, and only when its UPN is empty.
        // A non-empty but different UPN may be a genuine re-binding and is left
        // to RefreshAccountIdentity (which requires a password check).
        if (rec.sid != tokenSid) continue;
        if (!rec.upn.empty()) continue;

        // Pass rec.sid (not tokenSid) and the existing encryptedPassword back
        // unchanged so the record stays self-consistent and the stored
        // credential is not re-encrypted; faces are preserved by UpdateAccountIdentity.
        if (!m_store.UpdateAccountIdentity(idx, rec.username, curUpn, rec.sid, rec.encryptedPassword)) {
            FACELOGIN_ERROR(L"AutoRepairEmptyUpnOnStartup: identity update failed for %s", rec.username.c_str());
            continue;
        }
        FACELOGIN_INFO(L"AutoRepairEmptyUpnOnStartup: repaired empty UPN for %s (sid=%s, new upn=%s, faces=%zu, password untouched)",
                       rec.username.c_str(), rec.sid.c_str(), curUpn.c_str(), rec.faces.size());
        modified = true;
    }

    if (!modified) return false;

    return SaveAndReload();
}

// Repair records written by a pre-fix build on machines where the computer
// name equals the user name (upstream #24): ReadSessionIdentity then resolved
// the MACHINE domain SID via LookupAccountNameW and stored it. Unlock kept
// working (authentication is face-matched, the SID plays no role), but the
// token-SID guard refuses every append, multi-angle re-enrollment and identity
// refresh forever. The fix rewrites THIS session user's own record with the
// authoritative token SID; a stored SID that still resolves to a genuine user
// account (possibly another user's record) is never touched.
bool EnrollmentAccounts::AutoRepairMachineSidOnStartup() {
    std::wstring tokenSid = session_identity::GetCurrentProcessUserSid();
    if (tokenSid.empty()) {
        FACELOGIN_WARN(L"AutoRepairMachineSidOnStartup: could not resolve current session SID, skipping");
        return false;
    }
    if (!session_identity::SidIsUserAccount(tokenSid)) {
        FACELOGIN_WARN(L"AutoRepairMachineSidOnStartup: session SID does not resolve to a user account, skipping");
        return false;
    }

    if (!m_store.LoadDatabase()) return false;
    bool modified = false;
    for (size_t idx = 0; idx < m_store.GetUsers().size(); ++idx) {
        const auto& rec = m_store.GetUsers()[idx];
        if (rec.sid == tokenSid) continue;
        if (rec.sid.empty() || session_identity::SidIsUserAccount(rec.sid)) continue;
        // Only THIS session user's own record: same SAM name (case-insensitive,
        // as SAM itself is) or same MSA UPN.
        const bool sameUser =
            (!rec.upn.empty() && !m_identity.upn.empty() && rec.upn == m_identity.upn) ||
            (!rec.username.empty() && !m_identity.username.empty() &&
             lstrcmpiW(rec.username.c_str(), m_identity.username.c_str()) == 0);
        if (!sameUser) continue;

        // Pass the record's own username/upn and encryptedPassword back
        // unchanged so the stored credential is not re-encrypted; faces are
        // preserved by UpdateAccountIdentity.
        if (!m_store.UpdateAccountIdentity(idx, rec.username, rec.upn, tokenSid, rec.encryptedPassword)) {
            FACELOGIN_ERROR(L"AutoRepairMachineSidOnStartup: identity update failed for %s", rec.username.c_str());
            continue;
        }
        FACELOGIN_INFO(L"AutoRepairMachineSidOnStartup: replaced non-user SID with the session token SID "
                       L"for %s (faces=%zu, password untouched)",
                       rec.username.c_str(), rec.faces.size());
        modified = true;
    }

    if (!modified) return false;

    return SaveAndReload();
}

} // namespace facelogin
