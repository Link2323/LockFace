#pragma once

#include <string>
#include <vector>
#include <optional>
#include <cstdint>
#include <cmath>
#include "../common/recognizer_profile.h"
#include "../common/secure_clear.h"
#include <string_view>

namespace facelogin {

// Only SFace embeddings belong to this build's metric space.
// Trial range [0.70, 1.00], default 1.00; not a production FAR calibration.
inline float EmbeddingThresholdForDim(float baseThreshold, size_t dim) {
    if (dim != kRecognizerDimension) return 0.0f;
    return NormalizeMatchThreshold(baseThreshold);
}

// Stores and retrieves encrypted user credentials and face embeddings.
//
// V8 stores a minimal 20-byte header and one SYSTEM-user-scope DPAPI blob.
// Its protected payload repeats that header and contains all account/face data.
// See docs/contracts/users-dat.md for migration and persistence semantics.

// Maximum number of user accounts in the database. One Windows machine
// typically has ≤5 local accounts; this cap keeps the lock-screen tile list
// short and prevents abuse.
inline constexpr size_t kMaxUsers = 5;

// Maximum faces one account may enroll. Multi-angle enrollment uses up to 3
// (正面/左转/右转), so this is also the natural per-account limit. AddFace
// rejects when the account already has this many faces.
inline constexpr size_t kMaxFacesPerUser = 3;

// Sentinel for an in-memory face with no nominal angle. Any value
// outside the physical ±90° range means invalid; 1000 is preserved by
// serialization round-trips. Validity predicate:
// IsValidNominalAngle in face_align.h (physical angle domain).
inline constexpr float kNominalAngleInvalid = 1000.0f;

// One enrolled face for a user account. Each face carries a per-account
// id and a user-given label (defaults to L"脸N" where N = id). New ids reuse
// the smallest free slot (deleting #2 then re-adding gives #2 again), keeping
// the user-visible list compact.
//
// nominalYaw/nominalPitch: the enrollment POSE TARGET of this slot
// (kAngleTargets: 正面 0/0, 左转 +30/0, 右转 -30/0), not a measured average —
// a fixed target keeps the ±25° learning cones of the three slots disjoint
// even when the user under-turned (docs/work/in-progress/progressive-learning-v2.md §3).
// In-memory faces added without angles carry kNominalAngleInvalid; the learner's cone gate treats that as "no cone
// information" (pass) until re-enrollment.
struct FaceRecord {
    ~FaceRecord() { SecureClearVector(embedding); SecureClearWString(label); }
    FaceRecord() = default;
    FaceRecord(const FaceRecord&) = default;
    FaceRecord(FaceRecord&&) noexcept = default;
    FaceRecord& operator=(const FaceRecord& other) {
        if (this != &other) {
            SecureClearVector(embedding);
            SecureClearWString(label);
            id = other.id;
            label = other.label;
            embedding = other.embedding;
            nominalYaw = other.nominalYaw;
            nominalPitch = other.nominalPitch;
        }
        return *this;
    }
    FaceRecord& operator=(FaceRecord&& other) noexcept {
        if (this != &other) {
            SecureClearVector(embedding);
            SecureClearWString(label);
            id = other.id;
            label = std::move(other.label);
            embedding = std::move(other.embedding);
            nominalYaw = other.nominalYaw;
            nominalPitch = other.nominalPitch;
        }
        return *this;
    }
    uint32_t           id = 0;
    std::wstring       label;              // display name; "脸N" if user left blank
    std::vector<float> embedding;          // pinned SFace embedding is 128-D
    float              nominalYaw = kNominalAngleInvalid;
    float              nominalPitch = kNominalAngleInvalid;
};

struct UserRecord {
    ~UserRecord() { SecureClearWString(username); SecureClearWString(upn); SecureClearWString(sid); }
    UserRecord() = default;
    UserRecord(const UserRecord&) = default;
    UserRecord(UserRecord&&) noexcept = default;
    UserRecord& operator=(const UserRecord&) = default;
    UserRecord& operator=(UserRecord&&) noexcept = default;
    std::wstring username;
    std::wstring upn;      // UserPrincipalName (e.g. "john@outlook.com")
    std::wstring sid;      // Security Identifier (e.g. "S-1-5-21-...")
    std::vector<uint8_t> encryptedPassword;  // DPAPI encrypted
    std::vector<FaceRecord> faces;           // one or more enrolled faces
};

class CredentialStore {
public:
    CredentialStore() = default;
    ~CredentialStore() = default;
    CredentialStore(const CredentialStore&) = delete;
    CredentialStore& operator=(const CredentialStore&) = delete;
    // Service owns synchronization. Retain only encrypted disk snapshot at rest.
    void ReleasePlaintext();
    bool RestorePlaintext();

    // Set the data directory path. Default: PROGRAMDATA/FaceLogin/
    void SetDataDir(const std::wstring& dir) {
        ReleasePlaintext();
        m_recordsSealed = false;
        m_cachedUserCount = 0;
        m_dataDir = dir;
        m_hasDiskSnapshot = false;
        m_diskSnapshot.clear();
    }

    // Load users.dat from disk. Returns true on success.
    bool LoadDatabase();

    // SYSTEM-service startup only: seal V6/V7 main and backup databases;
    // re-protect V6 passwords and retire inactive V4/V5 secrets.
    bool UpgradeLegacyPasswords();

    // Save current in-memory records to users.dat. Returns true on success.
    bool SaveDatabase(bool refreshBackup = false);

    // Service only, under its store lock. Zero removes the account; otherwise
    // removes one face. Idempotent, purges the old backup before committing.
    bool DeletePersistedFace(const std::wstring& sid, uint32_t faceId = 0);

    // Get all loaded user records
    const std::vector<UserRecord>& GetUsers() const { return m_users; }

    // Find the index of the record matching the given identity.
    // Match priority: SID > UPN > username (only non-empty candidates are
    // tried). Returns m_users.size() (i.e. "not found") when nothing matches.
    size_t FindUserIndex(const std::wstring& sid,
                         const std::wstring& upn = L"",
                         const std::wstring& username = L"") const;

    // Add a face to a user account (create-or-append):
    //   - Account not found: creates it with the given encrypted password and
    //     the first face (id = 1). Rejects when the database already has
    //     kMaxUsers accounts.
    //   - Account found: appends a new face (id = smallest free slot) WITHOUT
    //     touching existing faces or the stored password. The passed
    //     encryptedPassword is ignored in this case.
    // Rejects (returns false) when the account already holds
    // kMaxFacesPerUser faces.
    // nominalYaw/nominalPitch: the slot's pose target for the learning
    // cone gate; defaults to kNominalAngleInvalid (legacy/no-angle callers).
    // Call SaveDatabase() to persist.
    bool AddFace(const std::wstring& username,
                 const std::wstring& upn,
                 const std::wstring& sid,
                 const std::vector<uint8_t>& encryptedPassword,
                 const std::vector<float>& embedding,
                 const std::wstring& label = L"",
                 uint32_t* outFaceId = nullptr,
                 float nominalYaw = kNominalAngleInvalid,
                 float nominalPitch = kNominalAngleInvalid);

    // Update the identity + stored password of an existing account IN PLACE,
    // preserving all enrolled faces (their ids/labels/embeddings are untouched).
    // Used when the account switches from a Microsoft (MSA) to a local account:
    // clears the stale MSA UPN (pass an empty upn) and refreshes username/SID.
    // Returns false when idx is out of range.
    // Call SaveDatabase() to persist.
    bool UpdateAccountIdentity(size_t idx,
                               const std::wstring& username,
                               const std::wstring& upn,
                               const std::wstring& sid,
                               const std::vector<uint8_t>& encryptedPassword);

    // Delete one face of an account. If the account ends up with no faces,
    // the whole account record is removed (an account with zero faces must
    // never be persisted — the login tile reads the record count and would
    // show a tile that can never match).
    // Call SaveDatabase() to persist.
    bool DeleteFace(const std::wstring& sid, uint32_t faceId);

    // Replace one face's embedding in place (progressive-learning EMA
    // update; ids/labels/password/nominal angles untouched). Returns true and
    // ALWAYS commits the swap (except unknown sid/face or dimension mismatch).
    // The cross-angle pair distance is observation-only (red line 3,
    // 2026-09-04): every update logs the account's minimum template-pair
    // distance, and a pair closer than kCrossAngleSentinelDist additionally
    // logs a WARN — intra-account convergence is coverage loss (acceptance
    // region shrinks, fail-safe direction), NOT a security hole, and a
    // rejecting sentinel false-positives legacy 0.60-pair accounts into a
    // permanently wedged learning channel. The real brakes on poisoning are
    // the update distance gate (evaluated against the pose-selected target
    // slot), the pose cone, the legacy-path landing-clarity gate and PAD —
    // none of which depend on intra-account spacing.
    // Call SaveDatabase() to persist.
    bool UpdateTemplateFace(const std::wstring& sid, uint32_t faceId,
                            const std::vector<float>& newEmbedding);

    // Observation threshold for template-pair spacing (min measured
    // cross-angle same-person distance 0.681, docs/threshold-calibration.md
    // appendix). Only used for logging since the 2026-09-04 revision.
    static constexpr float kCrossAngleSentinelDist = 0.68f;

    // Remove an account entirely (equivalent to deleting all of its faces).
    // Call SaveDatabase() to persist.
    bool ClearAllFaces(const std::wstring& sid);

    // Remove all faces from an account but keep the account identity
    // (username/UPN/SID/password). Used when re-enrolling multi-angle: the old
    // angle records are replaced, not accumulated.
    // Returns false if the account is not found.
    // Call SaveDatabase() to persist.
    bool ClearFacesForAccount(const std::wstring& sid);

    // Remove an account by SID. Call SaveDatabase() to persist.
    bool DeleteUserBySid(const std::wstring& sid);

    // Rename one face of an account (e.g. via the face management UI).
    // Returns false if the account or face id is unknown.
    // Call SaveDatabase() to persist.
    bool RenameFace(const std::wstring& sid, uint32_t faceId,
                    const std::wstring& label);

    // Number of faces enrolled for an account (0 = not enrolled).
    size_t GetFaceCount(const std::wstring& sid) const;

    // Find the best matching identity for a probe embedding.
    // Matching is account-level: each account's closest face is its
    // representative distance, then accounts are compared against each other
    // (so two faces of the same account never compete and inflate the
    // best/second-best ratio). This operation never decrypts a password.
    // It is used by the authentication worker boundary so the long-lived
    // parent only materializes a credential after all liveness/identity gates
    // have passed.
    //
    // Returns the identity if:
    //  1. distance < threshold, AND
    //  2. best distance / second-best distance < 0.75 (single account case: always passes)
    // Returns std::nullopt if no match found.
    struct IdentityMatch {
        std::wstring username;
        std::wstring upn;
        std::wstring sid;
        float        distance = 0.0f;
        // Which stored face produced the winning distance. The learner's
        // legacy no-angle path EMA-updates this face; the pose-selected path
        // re-targets by nominal yaw (see GetAccountFaceDistances).
        uint32_t     faceId = 0;
        std::wstring faceLabel;   // display label, logging only
        // Distance of the WINNING account's second-nearest comparable face.
        // Legacy no-angle learning path only (faces without nominal angles,
        // where the update target is still the distance-nearest face): an
        // equidistant frame cannot say which slot it belongs to, so it must
        // not update either. The pose-selected path does not use this —
        // slot ownership is decided by nominal yaw, not embedding distance.
        // Negative = the account has only one comparable face (gate passes
        // trivially).
        float        secondFaceDistance = -1.0f;
    };

    struct MatchResult {
        std::wstring username;
        std::wstring upn;
        std::wstring sid;
        std::wstring password;  // Decrypted — zero after use!
        float distance;
    };
    // probeDim is the number of floats in probeEmbedding (128 for dlib,
    // 128 for OpenCV SFace). Only stored embeddings of the same
    // dimensionality are compared; others are skipped as non-comparable.
    // outBestDistance (optional) receives the closest account-level distance
    // even when the match is rejected (threshold or ratio gate) — callers use
    // it to log how far off a failed probe was. Untouched when no stored
    // embedding is comparable.
    std::optional<IdentityMatch> FindBestIdentity(const float probeEmbedding[],
                                                   size_t probeDim,
                                                   float threshold = 0.30f,
                                                   float* outBestDistance = nullptr);

    // One face of an account as seen by a probe: its id/label/nominal pose
    // angles plus the probe's Euclidean distance to that face's embedding.
    // nominalYaw/Pitch carry kNominalAngleInvalid for faces without a
    // stored angle.
    struct AccountFaceDistance {
        uint32_t    faceId = 0;
        std::wstring label;
        float       nominalYaw = kNominalAngleInvalid;
        float       nominalPitch = kNominalAngleInvalid;
        float       distance = 0.0f;
    };

    // Distance from a probe to EVERY comparable face of one account (≤3),
    // with each face's nominal pose angles. Feeds the learner's pose-based
    // update-target selection: with overlapping enrolled slots the
    // distance-nearest face is not necessarily the pose slot the probe
    // actually landed in. Faces with a non-comparable embedding dimensionality
    // are skipped. Same lock requirement as FindBestIdentity (store lock).
    std::vector<AccountFaceDistance> GetAccountFaceDistances(
        const std::wstring& sid, const float probeEmbedding[],
        size_t probeDim) const;

    // Locate an already-authorized identity and decrypt its password.  This
    // must be called only after the caller has completed all authentication
    // gates; it intentionally performs no face matching itself.
    std::optional<MatchResult> LoadCredentialForSid(const std::wstring& sid,
                                                     float distance = 0.0f);

    // Get the number of registered users
    size_t GetUserCount() const { return m_recordsSealed ? m_cachedUserCount : m_users.size(); }

    // Get the full path to the database file
    std::wstring GetDataDir() const;

private:
    bool LoadDatabaseFile(const std::wstring& path, bool allowLegacy);
    bool LoadDatabaseBytes(std::string_view bytes, bool allowLegacy);
    bool m_recordsSealed = false;
    size_t m_cachedUserCount = 0;
    bool SaveDatabaseFile(const std::wstring& path);
    uint32_t m_loadedVersion = kCredentialDatabaseVersion;
    std::wstring m_dataDir;  // If empty, uses default
    std::vector<UserRecord> m_users;
    bool m_hasDiskSnapshot = false;
    bool m_diskExists = false;
    std::string m_diskSnapshot;
    bool m_purgeBackupOnSave = false;

    // Slot-coverage shrink warning de-dup: log only when the pair or the
    // distance actually changes (m_lastShrinkDist = -1 re-arms).
    uint32_t m_lastShrinkPairA = 0;
    uint32_t m_lastShrinkPairB = 0;
    float m_lastShrinkDist = -1.0f;
};

} // namespace facelogin
