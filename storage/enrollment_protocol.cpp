#include "enrollment_protocol.h"
#include "../common/account_identity.h"
#include <cstring>
#include <algorithm>

namespace facelogin::enrollment {
namespace {
// Hex transport keeps the existing UTF-16 message framing. Reserve before
// encoding templates so growth never leaves copies in freed allocations.
class Writer {
public:
    explicit Writer(std::wstring& out, const wchar_t* prefix) : out(out) {
        SecureClearWString(out);
        out.reserve(kMaxMessageBytes / sizeof(wchar_t));
        out.append(prefix);
    }
    void bytes(const void* data, size_t size) {
        if (!ok || size > (kMaxMessageBytes / 2 - 2 - out.size()) / 2) { ok = false; return; }
        constexpr wchar_t hex[] = L"0123456789ABCDEF";
        const auto p = static_cast<const uint8_t*>(data);
        for (size_t i = 0; i < size; ++i) { out.push_back(hex[p[i] >> 4]); out.push_back(hex[p[i] & 15]); }
    }
    template<class T> void value(T v) { bytes(&v, sizeof(v)); }
    void text(const std::wstring& text, size_t limit) {
        if (text.size() > limit || text.find(L'\0') != std::wstring::npos) { ok = false; return; }
        value<uint32_t>(static_cast<uint32_t>(text.size())); bytes(text.data(), text.size() * 2);
    }
    bool ok = true;
private:
    std::wstring& out;
};
class Reader {
public:
    Reader(const std::wstring& in, const wchar_t* prefix) : in(in), offset(wcslen(prefix)) {
        ok = in.size() >= offset && in.compare(0, offset, prefix) == 0 &&
            (in.size() + 1) * sizeof(wchar_t) <= kMaxMessageBytes;
    }
    bool bytes(void* out, size_t size) {
        if (!ok || offset > in.size() || size > (in.size() - offset) / 2) { ok = false; return false; }
        auto p = static_cast<uint8_t*>(out);
        auto digit = [](wchar_t c) { return c >= L'0' && c <= L'9' ? c - L'0' : c >= L'A' && c <= L'F' ? c - L'A' + 10 : -1; };
        for (size_t i = 0; i < size; ++i) {
            int hi = digit(in[offset++]), lo = digit(in[offset++]);
            if (hi < 0 || lo < 0) { ok = false; return false; }
            p[i] = static_cast<uint8_t>((hi << 4) | lo);
        }
        return true;
    }
    template<class T> T value() { T v{}; bytes(&v, sizeof(v)); return v; }
    std::wstring text(size_t limit) {
        const auto n = value<uint32_t>();
        if (!ok || n > limit) { ok = false; return {}; }
        std::wstring out(n, L'\0'); bytes(out.data(), n * 2);
        if (out.find(L'\0') != std::wstring::npos) ok = false;
        return out;
    }
    bool done() const { return ok && offset == in.size(); }
    bool ok = true;
private:
    const std::wstring& in;
    size_t offset;
};

bool ValidFace(const FaceRecord& face) {
    if (face.embedding.size() != kRecognizerDimension || face.label.size() > 64 ||
        !std::isfinite(face.nominalYaw) || !std::isfinite(face.nominalPitch) ||
        std::abs(face.nominalYaw) > 90 || std::abs(face.nominalPitch) > 90) return false;
    double norm = 0;
    for (float v : face.embedding) { if (!std::isfinite(v)) return false; norm += double(v) * v; }
    return norm >= 0.81 && norm <= 1.21;
}

size_t OwnRecord(const CredentialStore& store, const std::wstring& caller) {
    auto index = store.FindUserIndex(caller);
    if (index < store.GetUsers().size()) return index;
    std::wstring domain, username;
    if (!account_identity::ResolveUserSid(caller, domain, username)) return store.GetUsers().size();
    // Repair historical machine-domain SIDs only when the SAM name matches
    // the authenticated user and the old SID does not resolve to a user.
    const auto& users = store.GetUsers();
    for (size_t i = 0; i < users.size(); ++i) {
        std::wstring oldDomain, oldUser;
        if (!users[i].username.empty() && _wcsicmp(users[i].username.c_str(), username.c_str()) == 0 &&
            !account_identity::ResolveUserSid(users[i].sid, oldDomain, oldUser)) return i;
    }
    return users.size();
}
}

bool EncodeRequest(const Request& q, std::wstring& message) {
    Writer w(message, kRequestPrefix);
    w.value<uint32_t>(static_cast<uint32_t>(q.operation));
    w.text(q.username, 256); w.text(q.upn, 256); w.text(q.sid, 512); w.text(q.previousSid, 512);
    if (q.encryptedPassword.size() > 4096 || q.faces.size() > kMaxFacesPerUser) return false;
    w.value<uint32_t>(static_cast<uint32_t>(q.encryptedPassword.size()));
    w.bytes(q.encryptedPassword.data(), q.encryptedPassword.size());
    w.value<uint32_t>(q.replace ? 1 : 0); w.value(q.faceId); w.text(q.label, 64);
    w.value<uint32_t>(static_cast<uint32_t>(q.faces.size()));
    for (const auto& f : q.faces) {
        if (!ValidFace(f)) return false;
        w.text(f.label, 64); w.value(f.nominalYaw); w.value(f.nominalPitch);
        w.bytes(f.embedding.data(), kRecognizerDimension * sizeof(float));
    }
    return w.ok;
}

bool DecodeRequest(const std::wstring& message, Request& request) {
    Request q;
    Reader r(message, kRequestPrefix);
    const auto op = r.value<uint32_t>();
    if (op < 1 || op > 4) return false;
    q.operation = static_cast<Operation>(op);
    q.username = r.text(256); q.upn = r.text(256); q.sid = r.text(512); q.previousSid = r.text(512);
    const auto n = r.value<uint32_t>();
    if (!r.ok || n > 4096 || n == 1) return false;
    q.encryptedPassword.resize(n); r.bytes(q.encryptedPassword.data(), n);
    const auto replace = r.value<uint32_t>();
    if (replace > 1) return false;
    q.replace = replace != 0; q.faceId = r.value<uint32_t>(); q.label = r.text(64);
    const auto faces = r.value<uint32_t>();
    if (!r.ok || faces > kMaxFacesPerUser) return false;
    for (uint32_t i = 0; i < faces; ++i) {
        FaceRecord f;
        f.label = r.text(64); f.nominalYaw = r.value<float>(); f.nominalPitch = r.value<float>();
        f.embedding.resize(kRecognizerDimension); r.bytes(f.embedding.data(), kRecognizerDimension * sizeof(float));
        if (!r.ok || !ValidFace(f)) return false;
        q.faces.push_back(std::move(f));
    }
    if (!r.done()) return false;
    request = std::move(q);
    return true;
}

bool EncodeSnapshot(const Snapshot& s, std::wstring& message) {
    Writer w(message, kResponsePrefix);
    w.value<uint32_t>(s.found ? 1 : 0); w.text(s.username, 256); w.text(s.upn, 256); w.text(s.sid, 512);
    if (s.faces.size() > 16) return false;
    w.value<uint32_t>(static_cast<uint32_t>(s.faces.size()));
    for (const auto& f : s.faces) { w.value(f.id); w.text(f.label, 64); }
    return w.ok;
}

bool DecodeSnapshot(const std::wstring& message, Snapshot& snapshot) {
    Reader r(message, kResponsePrefix);
    Snapshot s;
    const auto found = r.value<uint32_t>();
    if (found > 1) return false;
    s.found = found != 0; s.username = r.text(256); s.upn = r.text(256); s.sid = r.text(512);
    const auto faces = r.value<uint32_t>();
    if (!r.ok || faces > 16 || (!s.found && faces != 0)) return false;
    for (uint32_t i = 0; i < faces; ++i) {
        FaceRecord f; f.id = r.value<uint32_t>(); f.label = r.text(64);
        if (!f.id) return false;
        s.faces.push_back(std::move(f));
    }
    if (!r.done()) return false;
    snapshot = std::move(s);
    return true;
}

bool Execute(CredentialStore& store, const std::wstring& callerSid,
             const Request& q, Snapshot& snapshot) {
    snapshot = {};
    if (callerSid.empty() || (!q.sid.empty() && q.sid != callerSid)) return false;
    if (!store.LoadDatabase()) return false;
    auto index = OwnRecord(store, callerSid);
    const bool exists = index < store.GetUsers().size();
    if (q.operation != Operation::Query) {
        std::wstring domain, username;
        if (!account_identity::ResolveUserSid(callerSid, domain, username)) return false;
        if (q.operation == Operation::SaveTemplates) {
            if (q.username.empty() || q.faces.empty() || q.faces.size() > kMaxFacesPerUser ||
                _wcsicmp(q.username.c_str(), username.c_str()) != 0) return false;
            for (const auto& f : q.faces) if (!ValidFace(f)) return false;
            if (exists && store.GetUsers()[index].sid != callerSid) return false; // repair first
            if (!exists && (q.encryptedPassword.empty() ||
                store.FindUserIndex(callerSid, q.upn, q.username) < store.GetUsers().size())) return false;
            // A lost response must not append the exact same captured template
            // again. Fresh captures have different vectors; replays are no-ops.
            const bool replay = exists && !q.replace && std::all_of(q.faces.begin(), q.faces.end(), [&](const FaceRecord& f) {
                const auto& stored = store.GetUsers()[index].faces;
                return std::any_of(stored.begin(), stored.end(), [&](const FaceRecord& old) {
                    return old.embedding == f.embedding && old.label == f.label &&
                        old.nominalYaw == f.nominalYaw && old.nominalPitch == f.nominalPitch;
                });
            });
            if (!replay) {
                if (exists && !q.replace && store.GetUsers()[index].faces.size() + q.faces.size() > kMaxFacesPerUser) return false;
                if (exists && q.replace && !store.ClearFacesForAccount(callerSid)) return false;
                for (const auto& f : q.faces) {
                    if (!store.AddFace(q.username, q.upn, callerSid, q.encryptedPassword,
                                       f.embedding, f.label, nullptr, f.nominalYaw, f.nominalPitch)) return false;
                }
            }
        } else if (q.operation == Operation::Rename) {
            if (!exists || !store.RenameFace(callerSid, q.faceId, q.label)) return false;
        } else if (q.operation == Operation::UpdateIdentity) {
            if (!exists || q.previousSid != store.GetUsers()[index].sid || q.username.empty() ||
                _wcsicmp(q.username.c_str(), username.c_str()) != 0) return false;
            const auto password = q.encryptedPassword.empty() ? store.GetUsers()[index].encryptedPassword : q.encryptedPassword;
            if (!store.UpdateAccountIdentity(index, q.username, q.upn, callerSid, password)) return false;
        } else return false;
        if (!store.SaveDatabase()) return false;
        index = OwnRecord(store, callerSid);
    }
    if (index < store.GetUsers().size()) {
        const auto& user = store.GetUsers()[index];
        snapshot.found = true; snapshot.username = user.username; snapshot.upn = user.upn; snapshot.sid = user.sid;
        for (const auto& face : user.faces) {
            FaceRecord metadata; metadata.id = face.id; metadata.label = face.label;
            snapshot.faces.push_back(std::move(metadata));
        }
    }
    return true;
}
} // namespace facelogin::enrollment
