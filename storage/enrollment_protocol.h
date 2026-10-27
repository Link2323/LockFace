#pragma once

#include "credential_store.h"

namespace facelogin::enrollment {

inline constexpr wchar_t kRequestPrefix[] = L"ENROLLMENT_V1:";
inline constexpr wchar_t kResponsePrefix[] = L"ENROLLMENT_OK:";
inline constexpr wchar_t kError[] = L"ENROLLMENT_ERROR";
inline constexpr size_t kMaxMessageBytes = 65536;

enum class Operation : uint32_t { Query = 1, SaveTemplates, Rename, UpdateIdentity };

struct Request {
    Operation operation = Operation::Query;
    std::wstring username, upn, sid;
    // Expected record SID for identity repair; the target must be the caller.
    std::wstring previousSid;
    std::vector<uint8_t> encryptedPassword;
    std::vector<FaceRecord> faces;
    bool replace = false;
    uint32_t faceId = 0;
    std::wstring label;
};

// Metadata only: no stored embedding or password blob may leave the service.
struct Snapshot {
    bool found = false;
    std::wstring username, upn, sid;
    std::vector<FaceRecord> faces;
};

bool EncodeRequest(const Request& request, std::wstring& message);
bool DecodeRequest(const std::wstring& message, Request& request);
bool EncodeSnapshot(const Snapshot& snapshot, std::wstring& message);
bool DecodeSnapshot(const std::wstring& message, Snapshot& snapshot);

// Caller privilege and SYSTEM identity are checked by the pipe dispatch.
// This operation additionally binds all queries and writes to the token SID.
// A write is committed atomically; false must never be presented as completed.
bool Execute(CredentialStore& store, const std::wstring& callerSid,
             const Request& request, Snapshot& snapshot);

} // namespace facelogin::enrollment
