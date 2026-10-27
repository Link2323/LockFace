#include "credential_store.h"

#include "dpapi_util.h"
#include "config_util.h"

#include <windows.h>

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>
#include <cstring>
#include <algorithm>
#include <chrono>

namespace {

constexpr uint32_t kFileMagic = 0x474F4C46;
int g_failures = 0;

void Check(bool condition, const char* description) {
    if (!condition) {
        std::fprintf(stderr, "FAIL: %s\n", description);
        ++g_failures;
    }
}

std::vector<float> Basis(size_t index, size_t dimension = 128) {
    std::vector<float> value(dimension, 0.0f);
    value[index] = 1.0f;
    return value;
}

std::vector<float> Midpoint(size_t first, size_t second) {
    std::vector<float> value(128, 0.0f);
    const float component = 1.0f / std::sqrt(2.0f);
    value[first] = component;
    value[second] = component;
    return value;
}

std::wstring MakeTempRoot() {
    wchar_t temp[MAX_PATH] = {};
    if (!GetTempPathW(ARRAYSIZE(temp), temp)) return {};
    const std::wstring root = std::wstring(temp) + L"FaceLogin-CredentialStoreTest-" +
        std::to_wstring(GetCurrentProcessId()) + L"-" +
        std::to_wstring(GetTickCount64());
    return CreateDirectoryW(root.c_str(), nullptr) ? root : std::wstring{};
}

void CleanupTempRoot(const std::wstring& root) {
    if (root.empty()) return;
    const std::wstring data = root + L"\\data";
    DeleteFileW((data + L"\\users.dat").c_str());
    DeleteFileW((data + L"\\users.dat.bak").c_str());
    RemoveDirectoryW(data.c_str());
    RemoveDirectoryW(root.c_str());
}

template <typename T>
void Write(std::ofstream& file, const T& value) {
    file.write(reinterpret_cast<const char*>(&value), sizeof(value));
}

void WriteText(std::ofstream& file, const std::wstring& value) {
    const uint32_t length = static_cast<uint32_t>(value.size());
    Write(file, length);
    if (length != 0) {
        file.write(reinterpret_cast<const char*>(value.data()),
                   static_cast<std::streamsize>(length * sizeof(wchar_t)));
    }
}

void WriteUnsupportedDatabase(const std::wstring& root, uint32_t version) {
    CreateDirectoryW((root + L"\\data").c_str(), nullptr);
    std::ofstream file(root + L"\\data\\users.dat", std::ios::binary | std::ios::trunc);
    const uint32_t count = 0;
    Write(file, kFileMagic);
    Write(file, version);
    Write(file, count);
    Write(file, facelogin::kRecognizerModelTag);
}

void WriteLegacyDatabase(const std::wstring& root, uint32_t version) {
    std::ofstream file(root + L"\\data\\users.dat", std::ios::binary | std::ios::trunc);
    Write(file, kFileMagic);
    Write(file, version);
    Write(file, uint32_t{1});
    WriteText(file, L"legacy");
    WriteText(file, L"");
    WriteText(file, L"SID-LEGACY");
    const auto password = facelogin::DpapiUtil::Protect(L"legacy-test-password");
    Write(file, static_cast<uint32_t>(password.size()));
    file.write(reinterpret_cast<const char*>(password.data()), password.size());
    Write(file, uint32_t{1}); // face count
    Write(file, uint32_t{1}); // face ID
    WriteText(file, L"front");
    Write(file, uint32_t{512});
    const auto embedding = Basis(0, 512);
    file.write(reinterpret_cast<const char*>(embedding.data()), embedding.size() * sizeof(float));
    if (version == 5) {
        Write(file, 0.0f);
        Write(file, 0.0f);
    }
}

std::string ReadDatabaseBytes(const std::wstring& root) {
    std::ifstream file(root + L"\\data\\users.dat", std::ios::binary);
    return {std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
}

// These conversions touch synthetic fixtures only, never a product database.
void WrapV8Fixture(const std::wstring& root) {
    auto bytes = ReadDatabaseBytes(root);
    bytes.insert(16, 4, '\0');
    auto cipher = facelogin::DpapiUtil::Protect(reinterpret_cast<const uint8_t*>(bytes.data()), bytes.size());
    const auto length = static_cast<uint32_t>(cipher.size());
    memcpy(bytes.data() + 16, &length, 4);
    cipher = facelogin::DpapiUtil::Protect(reinterpret_cast<const uint8_t*>(bytes.data()), bytes.size());
    std::ofstream file(root + L"\\data\\users.dat", std::ios::binary | std::ios::trunc);
    file.write(bytes.data(), 20);
    file.write(reinterpret_cast<const char*>(cipher.data()), cipher.size());
    if (!bytes.empty()) SecureZeroMemory(bytes.data(), bytes.size());
}
void MakePlainLegacyFixture(const std::wstring& root, uint32_t version) {
    const auto bytes = ReadDatabaseBytes(root);
    auto plain = facelogin::DpapiUtil::Unprotect(reinterpret_cast<const uint8_t*>(bytes.data() + 20), bytes.size() - 20);
    Check(plain.size() >= 16, "decrypt synthetic legacy fixture");
    if (plain.size() < 20) return;
    plain.erase(plain.begin() + 16, plain.begin() + 20);
    memcpy(plain.data() + 4, &version, sizeof(version));
    std::ofstream file(root + L"\\data\\users.dat", std::ios::binary | std::ios::trunc);
    file.write(reinterpret_cast<const char*>(plain.data()), plain.size());
    SecureZeroMemory(plain.data(), plain.size());
}

void WriteValidV7Record(std::ofstream& file, const std::wstring& username,
                        const std::wstring& sid) {
    WriteText(file, username);
    WriteText(file, L"");
    WriteText(file, sid);
    const uint32_t passwordLength = 2;
    Write(file, passwordLength);
    const uint8_t encryptedPassword[] = {0x01, 0x02};
    file.write(reinterpret_cast<const char*>(encryptedPassword), sizeof(encryptedPassword));
    const uint32_t faceCount = 1;
    const uint32_t faceId = 1;
    const uint32_t embeddingLength = 128;
    Write(file, faceCount);
    Write(file, faceId);
    WriteText(file, L"face");
    Write(file, embeddingLength);
    const auto embedding = Basis(0);
    file.write(reinterpret_cast<const char*>(embedding.data()),
               static_cast<std::streamsize>(embedding.size() * sizeof(float)));
    const float yaw = 0.0f, pitch = 0.0f;
    Write(file, yaw);
    Write(file, pitch);
}

void TestIdentityMatching() {
    facelogin::CredentialStore store;
    const auto password = facelogin::DpapiUtil::Protect(L"test-password");
    Check(!password.empty(), "DPAPI creates a identity-bound test credential");
    Check(store.AddFace(L"alice", L"alice@example.test", L"SID-A", password,
                        Basis(0)), "first Alice face is added");
    Check(store.AddFace(L"alice", L"alice@example.test", L"SID-A", password,
                        Basis(1)), "second Alice face is added");
    Check(store.AddFace(L"bob", L"bob@example.test", L"SID-B", password,
                        Basis(2)), "Bob face is added");

    const auto aliceProbe = Basis(1);
    auto identity = store.FindBestIdentity(aliceProbe.data(), aliceProbe.size(), 0.80f);
    Check(identity && identity->sid == L"SID-A",
          "identity-only match chooses the closest account without a credential field");
    Check(identity && identity->faceId == 2 && identity->faceLabel == L"脸2",
          "identity match reports which stored face won (progressive learning)");

    const auto ambiguous = Midpoint(1, 2);
    float ratioMissDistance = -1.0f;
    Check(!store.FindBestIdentity(ambiguous.data(), ambiguous.size(), 1.00f,
                                  &ratioMissDistance),
          "best/second account ratio rejects an ambiguous probe");
    Check(ratioMissDistance > 0.70f && ratioMissDistance < 0.80f,
          "outBestDistance reports the closest distance on ratio rejection");
    const auto legacyProbe = Basis(0, 512);
    float legacyMissDistance = -1.0f;
    Check(!store.FindBestIdentity(legacyProbe.data(), legacyProbe.size(), 0.80f,
                                  &legacyMissDistance),
          "different embedding dimensions are never compared");
    Check(legacyMissDistance < 0.0f,
          "outBestDistance is untouched when no embedding is comparable");
    const auto strangerProbe = Basis(3);
    float strangerMissDistance = -1.0f;
    Check(!store.FindBestIdentity(strangerProbe.data(), strangerProbe.size(), 0.80f,
                                  &strangerMissDistance),
          "probe far from every enrollment is rejected");
    Check(std::fabs(strangerMissDistance - std::sqrt(2.0f)) < 1e-4f,
          "outBestDistance reports how far off the failed probe was");

    auto credential = store.LoadCredentialForSid(L"SID-A", identity ? identity->distance : 0.0f);
    Check(credential && credential->password == L"test-password" &&
          credential->sid == L"SID-A",
          "credential is decrypted only by final SID lookup");
    Check(!store.LoadCredentialForSid(L""), "empty authorized SID fails closed");
    Check(!store.LoadCredentialForSid(L"SID-MISSING"),
          "unknown authorized SID fails closed");

    Check(store.DeleteUserBySid(L"SID-A"), "authorized record can be removed");
    Check(!store.LoadCredentialForSid(L"SID-A"),
          "record disappearance between match and release fails closed");
}

void TestThresholds() {
    Check(facelogin::EmbeddingThresholdForDim(0.80f, 128) == 0.80f,
          "explicit SFace threshold is preserved");
    Check(facelogin::EmbeddingThresholdForDim(0.20f, 128) == 1.00f,
          "out-of-range SFace threshold resets to trial default");
    Check(facelogin::EmbeddingThresholdForDim(1.20f, 128) == 1.00f,
          "loose SFace threshold stays below trial ceiling");
    Check(facelogin::EmbeddingThresholdForDim(1.00f, 512) == 0.0f,
          "R50 dimensions cannot authenticate with this build");
    auto old = facelogin::ConfigFromJson(R"({"match_threshold":0.8,"ema_learning":true,"camera_rotation":90})");
    Check(old.match_threshold == 1.0f && !old.ema_learning && old.camera_rotation == 90,
          "old config resets recognizer settings while retaining camera settings");
    auto fresh = facelogin::ConfigFromJson(R"({"recognizer_model":"opencv-sface-2021dec","match_threshold":0.95,"ema_learning":true})");
    Check(fresh.match_threshold == 0.95f && !fresh.ema_learning && !fresh.failure_learning,
          "SFace trial preserves its own threshold and cannot enable unvalidated learning");
    Check(facelogin::ConfigFromJson(facelogin::ConfigToJson(fresh)).match_threshold == 0.95f,
          "SFace config model marker survives serialization");
}

void TestPasswordBlobValidation() {
    facelogin::CredentialStore store;
    const auto embedding = Basis(0);
    Check(!store.AddFace(L"invalid", L"", L"SID-I", {}, embedding),
          "empty password blob is rejected");
    Check(!store.AddFace(L"invalid", L"", L"SID-I", {0x00}, embedding),
          "single-byte legacy sentinel is rejected");
}

void TestDeleteFace() {
    facelogin::CredentialStore store;
    const auto password = facelogin::DpapiUtil::Protect(L"test-password");
    Check(store.AddFace(L"alice", L"alice@example.test", L"SID-A", password,
                        Basis(0)), "first Alice face is added");
    Check(store.AddFace(L"alice", L"alice@example.test", L"SID-A", password,
                        Basis(1)), "second Alice face is added");
    // Zoe is deliberately the LAST record: deleting her only face used to
    // log rec.username after the vector erase — UB exactly in this position.
    Check(store.AddFace(L"zoe", L"zoe@example.test", L"SID-Z", password,
                        Basis(2)), "Zoe face is added as the last account");

    Check(store.DeleteFace(L"SID-A", 1), "one of several faces is deleted");
    Check(store.GetFaceCount(L"SID-A") == 1,
          "account survives while at least one face remains");
    Check(!store.DeleteFace(L"SID-A", 9), "unknown face id is rejected");
    Check(!store.DeleteFace(L"SID-MISSING", 1), "unknown account is rejected");

    Check(store.DeleteFace(L"SID-Z", 1), "last account's only face is deleted");
    Check(store.GetFaceCount(L"SID-Z") == 0,
          "account is dropped together with its last face");
    Check(!store.LoadCredentialForSid(L"SID-Z"), "dropped account fails closed");

    Check(store.DeleteFace(L"SID-A", 2), "remaining Alice face is deleted");
    Check(store.GetUserCount() == 0,
          "database empties after the last account loses its last face");
}

// Unit vector tilted `perturb` away from a basis axis (still ~orthogonal to
// every other axis — distance to Basis(index) ≈ atan(perturb)).
std::vector<float> NearBasis(size_t index, float perturb) {
    std::vector<float> value(128, 0.0f);
    value[index] = 1.0f;
    value[(index + 7) % 128] = perturb;
    float norm = 0.0f;
    for (float v : value) norm += v * v;
    norm = std::sqrt(norm);
    for (float& v : value) v /= norm;
    return value;
}

float VectorDistance(const std::vector<float>& a, const std::vector<float>& b) {
    float sum = 0.0f;
    for (size_t i = 0; i < a.size(); i++) {
        const float diff = a[i] - b[i];
        sum += diff * diff;
    }
    return std::sqrt(sum);
}

void TestUpdateTemplateFace() {
    const std::wstring root = MakeTempRoot();
    Check(!root.empty(), "temp root created for template update tests");
    if (root.empty()) return;

    facelogin::CredentialStore store;
    store.SetDataDir(root);
    const auto password = facelogin::DpapiUtil::Protect(L"test-password");
    Check(store.AddFace(L"alice", L"alice@example.test", L"SID-A", password,
                        Basis(0), L"正面"),
          "front face is added for update tests");
    Check(store.AddFace(L"alice", L"alice@example.test", L"SID-A", {},
                        Basis(1), L"左转"),
          "left face is added for update tests");

    const auto inCone = NearBasis(0, 0.10f);   // ≈0.10 from Basis(0), ≈1.40 from Basis(1)
    Check(store.UpdateTemplateFace(L"SID-A", 1, inCone),
          "same-pose EMA update is accepted");

    // Pair-spacing observation (2026-09-04): replacing face #1 with a vector
    // only ~0.10 from face #2 collapses the pair below
    // kCrossAngleSentinelDist — the update COMMITS anyway (a rejecting
    // sentinel wedged the whole learning channel on legacy near-pair
    // accounts; convergence is fail-safe coverage loss, docs red line 3).
    const auto nearFace2 = NearBasis(1, 0.10f);
    Check(store.UpdateTemplateFace(L"SID-A", 1, nearFace2),
          "converging update commits (pair distance is observation-only)");
    {
        const auto& users = store.GetUsers();
        Check(users.size() == 1 && users[0].faces.size() == 2 &&
                  VectorDistance(users[0].faces[0].embedding, nearFace2) < 1e-5f,
              "observation-only update actually replaces the template");
    }

    Check(!store.UpdateTemplateFace(L"SID-MISSING", 1, inCone),
          "unknown SID update is rejected");
    Check(!store.UpdateTemplateFace(L"SID-A", 9, inCone),
          "unknown face id update is rejected");
    Check(!store.UpdateTemplateFace(L"SID-A", 1, std::vector<float>(256, 0.5f)),
          "dimension-mismatched update is rejected");

    // Persisted: a fresh store reloads the updated template with ids/labels
    // and the password intact (learning must never corrupt the credential).
    Check(store.SaveDatabase(), "updated database saves");
    {
        facelogin::CredentialStore reloaded;
        reloaded.SetDataDir(root);
        Check(reloaded.LoadDatabase(), "updated database reloads");
        const auto& users = reloaded.GetUsers();
        Check(users.size() == 1 && users[0].faces.size() == 2 &&
                  users[0].faces[0].label == L"正面" &&
                  users[0].faces[1].label == L"左转" &&
                  VectorDistance(users[0].faces[0].embedding, nearFace2) < 1e-5f,
              "updated template round-trips with labels and slot order");
        auto credential = reloaded.LoadCredentialForSid(L"SID-A", 0.1f);
        Check(credential && credential->password == L"test-password",
              "password survives template updates");
    }
    CleanupTempRoot(root);
}

void TestV7RoundTripAndReload() {
    const std::wstring root = MakeTempRoot();
    Check(!root.empty(), "temporary credential-store directory is created");
    if (root.empty()) return;

    facelogin::CredentialStore writer;
    writer.SetDataDir(root);
    const auto encrypted = facelogin::DpapiUtil::Protect(L"round-trip-password");
    Check(writer.AddFace(L"roundtrip", L"roundtrip@example.test", L"SID-R",
                         encrypted, Basis(0), L"front"),
          "round-trip record is added");
    Check(writer.SaveDatabase(), "V7 database saves");

    facelogin::CredentialStore reader;
    reader.SetDataDir(root);
    Check(reader.LoadDatabase() && reader.GetUserCount() == 1 &&
          reader.GetFaceCount(L"SID-R") == 1,
          "V7 database reload preserves account and face");
    const auto credential = reader.LoadCredentialForSid(L"SID-R");
    Check(credential && credential->password == L"round-trip-password",
          "V7 reload preserves decryptable credential");

    // Replace the file with a valid empty database. A previously authorized
    // SID must disappear immediately after the reload.
    {
        std::ofstream file(root + L"\\data\\users.dat",
                           std::ios::binary | std::ios::trunc);
        const uint32_t version = facelogin::kCredentialDatabaseVersion;
        const uint32_t count = 0;
        Write(file, kFileMagic);
        Write(file, version);
        Write(file, count);
        Write(file, facelogin::kRecognizerModelTag);
    }
    WrapV8Fixture(root);
    Check(reader.LoadDatabase() && reader.GetUserCount() == 0,
          "valid empty database atomically clears old records");
    Check(!reader.LoadCredentialForSid(L"SID-R"),
          "database reload disappearance fails closed");
    CleanupTempRoot(root);
}

void TestV7NominalAnglesAndLegacyRead() {
    const std::wstring root = MakeTempRoot();
    Check(!root.empty(), "temp root created for V7 nominal-angle tests");
    if (root.empty()) return;

    // V7 round trip: nominal angles persist per face.
    {
        facelogin::CredentialStore writer;
        writer.SetDataDir(root);
        const auto encrypted = facelogin::DpapiUtil::Protect(L"v5-password");
        Check(writer.AddFace(L"v5", L"", L"SID-V5", encrypted, Basis(0),
                             L"正面", nullptr, 0.0f, 0.0f),
              "front face with nominal angles is added");
        Check(writer.AddFace(L"v5", L"", L"SID-V5", {}, Basis(1),
                             L"左转", nullptr, 30.0f, 0.0f),
              "left face with nominal yaw +30 is added");
        Check(writer.SaveDatabase(), "V7 database saves");
        facelogin::CredentialStore reader;
        reader.SetDataDir(root);
        Check(reader.LoadDatabase(), "V7 database reloads");
        const auto& users = reader.GetUsers();
        Check(users.size() == 1 && users[0].faces.size() == 2 &&
                  users[0].faces[0].nominalYaw == 0.0f &&
                  users[0].faces[1].nominalYaw == 30.0f &&
                  users[0].faces[0].nominalPitch == 0.0f &&
                  users[0].faces[1].nominalPitch == 0.0f,
              "V7 nominal angles round-trip per face");
        auto credential = reader.LoadCredentialForSid(L"SID-V5");
        Check(credential && credential->password == L"v5-password",
              "V7 reload preserves decryptable credential");

        // Landing-clarity input: the winning account's second-nearest face
        // is exposed on IdentityMatch; a probe near face #1 reports face #2's
        // distance as the runner-up.
        const auto probe = NearBasis(0, 0.05f);
        auto identity = reader.FindBestIdentity(probe.data(), probe.size(), 0.80f);
        Check(identity && identity->faceId == 1 &&
                  identity->secondFaceDistance > 1.30f,
              "identity match reports the winning account's runner-up face");
        // Single-face account: negative runner-up = clarity gate passes.
        facelogin::CredentialStore single;
        single.SetDataDir(root);
        const auto encrypted2 = facelogin::DpapiUtil::Protect(L"v5b");
        Check(single.AddFace(L"solo", L"", L"SID-SOLO", encrypted2, Basis(4)),
              "single-face account is added");
        auto solo = single.FindBestIdentity(Basis(4).data(), 128, 0.80f);
        Check(solo && solo->secondFaceDistance < 0.0f,
              "single-face account reports no runner-up face");
    }

    // Model changes force re-enrollment; even an old header with accounts
    // must expose no identities and leave its bytes untouched.
    for (uint32_t version : {4u, 5u}) {
        WriteLegacyDatabase(root, version);
        const auto originalBytes = ReadDatabaseBytes(root);
        facelogin::CredentialStore reader;
        reader.SetDataDir(root);
        Check(reader.LoadDatabase() && reader.GetUserCount() == 0,
              "old R50 databases remain inactive without conversion");
        Check(!reader.LoadCredentialForSid(L"SID-LEGACY"),
              "old credentials cannot authenticate through the new store");
        Check(ReadDatabaseBytes(root) == originalBytes,
              "loading an old database preserves its original bytes");
    }
    {
        std::ofstream file(root + L"\\data\\users.dat", std::ios::binary | std::ios::trunc);
        Write(file, kFileMagic);
        const uint32_t version = facelogin::kCredentialDatabaseVersion, count = 0, wrongTag = 0;
        Write(file, version); Write(file, count); Write(file, wrongTag);
    }
    facelogin::CredentialStore wrong;
    wrong.SetDataDir(root);
    Check(!wrong.LoadDatabase(), "a V7 file with a different recognizer is rejected");
    CleanupTempRoot(root);
}

void TestUnsupportedVersionsAreRejected() {
    const std::wstring root = MakeTempRoot();
    Check(!root.empty(), "unsupported-version fixture directory is created");
    if (root.empty()) return;

    for (uint32_t version : {1u, 2u, 3u, 9u}) {
        WriteUnsupportedDatabase(root, version);
        facelogin::CredentialStore store;
        store.SetDataDir(root);
        Check(!store.LoadDatabase() && store.GetUserCount() == 0,
              "unsupported database version is rejected");
    }
    CleanupTempRoot(root);
}

void TestMalformedReloadIsTransactional() {
    const std::wstring root = MakeTempRoot();
    Check(!root.empty(), "malformed fixture directory is created");
    if (root.empty()) return;
    CreateDirectoryW((root + L"\\data").c_str(), nullptr);

    // Two records are declared. The first is complete; the second is
    // truncated after its username length. No prefix may become visible.
    {
        std::ofstream file(root + L"\\data\\users.dat",
                           std::ios::binary | std::ios::trunc);
        const uint32_t version = facelogin::kCredentialDatabaseVersion;
        const uint32_t count = 2;
        Write(file, kFileMagic);
        Write(file, version);
        Write(file, count);
        Write(file, facelogin::kRecognizerModelTag);
        WriteValidV7Record(file, L"first", L"SID-FIRST");
        const uint32_t secondNameLength = 8;
        Write(file, secondNameLength);
    }
    WrapV8Fixture(root);
    facelogin::CredentialStore store;
    store.SetDataDir(root);
    Check(!store.LoadDatabase(), "truncated database reload is rejected");
    Check(store.GetUserCount() == 0,
          "rejected reload exposes no partially parsed identities");

    // The header count itself is bounded before any record allocation.
    {
        std::ofstream file(root + L"\\data\\users.dat",
                           std::ios::binary | std::ios::trunc);
        const uint32_t version = facelogin::kCredentialDatabaseVersion;
        const uint32_t count = static_cast<uint32_t>(facelogin::kMaxUsers + 1);
        Write(file, kFileMagic);
        Write(file, version);
        Write(file, count);
        Write(file, facelogin::kRecognizerModelTag);
    }
    WrapV8Fixture(root);
    Check(!store.LoadDatabase() && store.GetUserCount() == 0,
          "oversized database user count fails closed");
    CleanupTempRoot(root);
}

void TestIdentityScopedProtection() {
    const auto encrypted = facelogin::DpapiUtil::Protect(L"identity-scope-test");
    Check(!encrypted.empty(), "identity-scope fixture is encrypted");
    auto plain = facelogin::DpapiUtil::Unprotect(encrypted);
    Check(!plain.empty(), "encrypting identity can decrypt its credential");
    if (!plain.empty()) SecureZeroMemory(plain.data(), plain.size());
    const bool impersonated = ImpersonateAnonymousToken(GetCurrentThread()) != FALSE;
    Check(impersonated, "test enters a different security identity");
    if (impersonated) {
        const bool system = facelogin::DpapiUtil::IsSystemContext();
        auto forbidden = facelogin::DpapiUtil::Unprotect(encrypted);
        const bool reverted = RevertToSelf() != FALSE;
        if (!reverted) TerminateProcess(GetCurrentProcess(), ERROR_CANNOT_IMPERSONATE);
        Check(!system && forbidden.empty(), "another identity cannot decrypt copied user-scope ciphertext");
        if (!forbidden.empty()) SecureZeroMemory(forbidden.data(), forbidden.size());
    }
}

void TestAtomicSaveFailure() {
    const auto root = MakeTempRoot();
    Check(!root.empty(), "atomic-save fixture directory created");
    if (root.empty()) return;
    facelogin::CredentialStore store;
    store.SetDataDir(root);
    const auto password = facelogin::DpapiUtil::Protect(L"atomic-save-test");
    Check(store.AddFace(L"atomic", L"", L"SID-ATOMIC", password, Basis(0)) && store.SaveDatabase(),
          "atomic-save initial database persisted");
    const auto original = ReadDatabaseBytes(root);
    const auto main = root + L"\\data\\users.dat";
    HANDLE locked = CreateFileW(main.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                                OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    Check(locked != INVALID_HANDLE_VALUE, "hold original database against replacement");
    if (locked != INVALID_HANDLE_VALUE) {
        Check(store.RenameFace(L"SID-ATOMIC", 1, L"changed"), "modify in-memory face label");
        Check(!store.SaveDatabase() && ReadDatabaseBytes(root) == original,
              "failed atomic replacement preserves original database bytes");
        CloseHandle(locked);
        Check(store.SaveDatabase(), "save succeeds after replacement lock released");
    }
    CleanupTempRoot(root);
}

void TestEncryptedTemplateBoundary() {
    const auto root = MakeTempRoot();
    if (root.empty()) { Check(false, "encrypted boundary fixture created"); return; }
    facelogin::CredentialStore store;
    store.SetDataDir(root);
    const auto password = facelogin::DpapiUtil::Protect(L"synthetic-secret");
    const std::wstring name = L"unmistakable-private-account";
    const auto embedding = Midpoint(7, 29);
    Check(store.AddFace(name, L"private@example.invalid", L"SID-PRIVATE", password,
                        embedding, L"private-template", nullptr, 30, 0) && store.SaveDatabase(),
          "encrypted template database saves");
    const auto original = ReadDatabaseBytes(root);
    Check(original.find(std::string(reinterpret_cast<const char*>(name.data()), name.size() * 2)) == std::string::npos &&
          original.find(std::string(reinterpret_cast<const char*>(embedding.data()), embedding.size() * 4)) == std::string::npos,
          "account and exact template bytes are absent from disk");
    store.ReleasePlaintext();
    Check(store.GetUsers().empty() && store.GetUserCount() == 1 && !store.SaveDatabase(),
          "sealed store exposes only count and cannot accidentally save an empty database");
    Check(store.RestorePlaintext() && store.GetUsers().size() == 1 && store.GetUsers()[0].faces[0].embedding == embedding,
          "a round restores the encrypted cached snapshot");
    store.ReleasePlaintext();
    const bool impersonated = ImpersonateAnonymousToken(GetCurrentThread()) != FALSE;
    Check(impersonated, "enter another identity for template decryption test");
    if (impersonated) {
        const bool rejected = !store.RestorePlaintext() && store.GetUsers().empty();
        if (!RevertToSelf()) TerminateProcess(GetCurrentProcess(), ERROR_CANNOT_IMPERSONATE);
        Check(rejected, "copied templates cannot be unsealed by another identity");
        Check(store.RestorePlaintext(), "failed unseal never makes subsequent restore a false success");
    }
    auto write = [&](const std::string& data) {
        std::ofstream file(root + L"\\data\\users.dat", std::ios::binary | std::ios::trunc);
        file.write(data.data(), data.size());
    };
    for (int corruption = 0; corruption < 4; ++corruption) {
        auto damaged = original;
        if (corruption == 0) damaged[8] = 0; // syntactically valid but unauthenticated count
        if (corruption == 1) damaged[damaged.size() / 2] ^= 0x41;
        if (corruption == 2) damaged.pop_back();
        if (corruption == 3) damaged.push_back('x');
        write(damaged);
        Check(!store.LoadDatabase() && store.GetUsers().empty(), "tampered header/ciphertext, truncation and trailing bytes fail closed");
    }
    write(original);
    MakePlainLegacyFixture(root, 7);
    const auto old = ReadDatabaseBytes(root);
    Check(!store.LoadDatabase(), "ordinary reads refuse unencrypted V7 templates");
    if (facelogin::DpapiUtil::IsSystemContext()) {
        Check(store.UpgradeLegacyPasswords() && store.LoadDatabase() && store.GetUsers().size() == 1 &&
              store.GetUsers()[0].faces[0].embedding == embedding && store.GetUsers()[0].encryptedPassword == password,
              "SYSTEM migrates V7 without re-enrollment or changing password protection");
    } else {
        Check(!store.UpgradeLegacyPasswords() && ReadDatabaseBytes(root) == old,
              "non-SYSTEM cannot migrate templates or modify the legacy file");
    }
    CleanupTempRoot(root);
}

void TestLegacyPasswordMigration() {
    const auto root = MakeTempRoot();
    Check(!root.empty(), "migration fixture directory created");
    if (root.empty()) return;
    std::wstring password = L"migration-test-password";
    DATA_BLOB input{static_cast<DWORD>(password.size() * sizeof(wchar_t)),
                    reinterpret_cast<BYTE*>(password.data())}, output{};
    Check(CryptProtectData(&input, nullptr, nullptr, nullptr, nullptr,
                          CRYPTPROTECT_LOCAL_MACHINE | CRYPTPROTECT_UI_FORBIDDEN, &output),
          "legacy machine-scope fixture encrypted");
    if (!output.pbData) { CleanupTempRoot(root); return; }
    const std::vector<uint8_t> legacy(output.pbData, output.pbData + output.cbData);
    LocalFree(output.pbData);
    facelogin::CredentialStore writer;
    writer.SetDataDir(root);
    Check(writer.AddFace(L"migration", L"", L"SID-MIGRATION", legacy, Basis(0),
                         L"left", nullptr, 30.0f, 0.0f) && writer.SaveDatabase(),
          "migration fixture records saved");
    const auto main = root + L"\\data\\users.dat";
    MakePlainLegacyFixture(root, 6);
    Check(CopyFileW(main.c_str(), (main + L".bak").c_str(), FALSE), "legacy backup created");
    const auto original = ReadDatabaseBytes(root);
    facelogin::CredentialStore reader;
    reader.SetDataDir(root);
    Check(!reader.LoadDatabase() && reader.GetUserCount() == 0,
          "ordinary loads cannot activate legacy machine-scope credentials");
    if (!facelogin::DpapiUtil::IsSystemContext()) {
        Check(!reader.UpgradeLegacyPasswords() && ReadDatabaseBytes(root) == original,
              "non-SYSTEM migration fails without modifying legacy records");
        std::puts("SYSTEM migration integration: not run (requires SYSTEM test process)");
    } else {
        Check(reader.UpgradeLegacyPasswords() && reader.LoadDatabase(), "SYSTEM migrates V6 main and backup");
        auto credential = reader.LoadCredentialForSid(L"SID-MIGRATION");
        Check(credential && credential->password == password, "migration preserves the password");
        if (credential) SecureZeroMemory(credential->password.data(), credential->password.size() * sizeof(wchar_t));
        const auto& users = reader.GetUsers();
        Check(users.size() == 1 && users[0].faces[0].nominalYaw == 30.0f &&
              users[0].encryptedPassword != legacy, "migration preserves faces and replaces old ciphertext");
        Check(CopyFileW((main + L".bak").c_str(), main.c_str(), FALSE) && reader.LoadDatabase(),
              "backup is independently loadable as V7");
        const auto upgraded = ReadDatabaseBytes(root);
        Check(reader.UpgradeLegacyPasswords() && ReadDatabaseBytes(root) == upgraded,
              "repeated migration leaves V7 ciphertext unchanged");
        // An undecryptable legacy file must never be replaced by an empty/new database.
        {
            std::ofstream file(main, std::ios::binary | std::ios::trunc);
            Write(file, kFileMagic); Write(file, uint32_t{6}); Write(file, uint32_t{1});
            Write(file, facelogin::kRecognizerModelTag);
            WriteValidV7Record(file, L"broken", L"SID-BROKEN");
        }
        const auto broken = ReadDatabaseBytes(root);
        Check(!reader.UpgradeLegacyPasswords() && ReadDatabaseBytes(root) == broken,
              "undecryptable legacy credential leaves original file intact");
    }
    SecureZeroMemory(password.data(), password.size() * sizeof(wchar_t));
    CleanupTempRoot(root);
}

void TestPrivacyDeletion() {
    const auto root = MakeTempRoot();
    Check(!root.empty(), "create privacy fixture");
    if (root.empty()) return;
    const auto path = root + L"\\data\\users.dat";
    facelogin::CredentialStore service;
    service.SetDataDir(root);
    Check(service.AddFace(L"alice", L"", L"SID-A", {1, 2}, Basis(0)) &&
          service.AddFace(L"alice", L"", L"SID-A", {}, Basis(1)) &&
          service.AddFace(L"bob", L"", L"SID-B", {3, 4}, Basis(2)) && service.SaveDatabase(),
          "seed two independent accounts");
    facelogin::CredentialStore staleUi, staleLearner;
    staleUi.SetDataDir(root);
    staleLearner.SetDataDir(root);
    Check(staleUi.LoadDatabase() && staleLearner.LoadDatabase(), "retain old UI and learner snapshots");
    Check(CopyFileW(path.c_str(), (path + L".bak").c_str(), FALSE), "seed old backup");
    // Locked backups must fail explicitly and leave the primary unchanged.
    const auto before = ReadDatabaseBytes(root);
    HANDLE held = CreateFileW((path + L".bak").c_str(), GENERIC_READ, FILE_SHARE_READ,
                             nullptr, OPEN_EXISTING, 0, nullptr);
    Check(held != INVALID_HANDLE_VALUE, "hold backup to simulate deletion failure");
    Check(!service.DeletePersistedFace(L"SID-A") && ReadDatabaseBytes(root) == before,
          "backup failure cannot acknowledge a completed deletion");
    if (held != INVALID_HANDLE_VALUE) CloseHandle(held);
    for (const auto& suffix : {L".tmp.crash", L".bak.tmp.crash"}) {
        Check(CopyFileW(path.c_str(), (path + suffix).c_str(), FALSE), "seed interrupted-save residue");
    }
    Check(service.DeletePersistedFace(L"SID-A", 1), "service deletes a single face");
    Check(service.GetFaceCount(L"SID-A") == 1 && service.GetFaceCount(L"SID-B") == 1,
          "single-face deletion preserves other faces and other accounts");
    Check(GetFileAttributesW((path + L".bak").c_str()) == INVALID_FILE_ATTRIBUTES &&
          GetFileAttributesW((path + L".tmp.crash").c_str()) == INVALID_FILE_ATTRIBUTES &&
          GetFileAttributesW((path + L".bak.tmp.crash").c_str()) == INVALID_FILE_ATTRIBUTES,
          "deletion removes backups and interrupted-save residues");
    Check(!staleUi.SaveDatabase() && !staleLearner.SaveDatabase(true),
          "stale UI and learner cannot restore a deleted face or recreate an old backup");
    Check(service.DeletePersistedFace(L"SID-A", 1), "lost single-delete ACK can be retried");
    Check(service.DeletePersistedFace(L"SID-A") && service.DeletePersistedFace(L"SID-A"),
          "withdrawal is idempotent even when the account is already absent");
    Check(service.LoadDatabase() && service.GetUserCount() == 1 &&
          service.GetUsers()[0].sid == L"SID-B" && service.GetUsers()[0].encryptedPassword == std::vector<uint8_t>({3, 4}),
          "withdrawal removes the entire target credential and preserves other credentials");
    Check(service.SaveDatabase(true), "learning can resume after deletion");
    facelogin::CredentialStore copy;
    const auto backupRoot = MakeTempRoot();
    copy.SetDataDir(backupRoot);
    CreateDirectoryW((backupRoot + L"\\data").c_str(), nullptr);
    Check(CopyFileW((path + L".bak").c_str(), (backupRoot + L"\\data\\users.dat").c_str(), FALSE) &&
          copy.LoadDatabase() && copy.GetFaceCount(L"SID-A") == 0 && copy.GetFaceCount(L"SID-B") == 1,
          "later daily backup cannot retain the withdrawn account");
    Check(CopyFileW(path.c_str(), (path + L".bak").c_str(), FALSE) &&
          service.ClearFacesForAccount(L"SID-B") &&
          service.AddFace(L"bob", L"", L"SID-B", {}, Basis(3)) && service.SaveDatabase() &&
          GetFileAttributesW((path + L".bak").c_str()) == INVALID_FILE_ATTRIBUTES,
          "multi-angle replacement also purges the superseded template backup");
    // A failed primary replacement never reports success; retry completes later.
    held = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    Check(held != INVALID_HANDLE_VALUE, "hold primary to simulate commit failure");
    Check(!service.DeletePersistedFace(L"SID-B"), "failed primary commit is reported");
    service.ReleasePlaintext();
    Check(!service.RestorePlaintext() && service.GetUsers().empty(),
          "failed deletion cannot resurrect its old encrypted cache during the next authentication");
    if (held != INVALID_HANDLE_VALUE) CloseHandle(held);
    Check(service.DeletePersistedFace(L"SID-B") && service.LoadDatabase() && service.GetUserCount() == 0,
          "retry after commit failure persists an empty database");
    CleanupTempRoot(backupRoot);
    CleanupTempRoot(root);
}

} // namespace

int BenchmarkTemplateUnseal() {
    const auto root = MakeTempRoot();
    if (root.empty()) return 1;
    facelogin::CredentialStore store;
    store.SetDataDir(root);
    const auto password = facelogin::DpapiUtil::Protect(L"synthetic-benchmark-password");
    bool ok = true;
    for (int user = 0; user < 5; ++user) {
        for (int face = 0; face < 3; ++face) {
            ok = ok && store.AddFace(L"synthetic-" + std::to_wstring(user), L"", L"SID-" + std::to_wstring(user),
                password, Basis(face), L"angle", nullptr, float(face * 30), 0);
        }
    }
    ok = ok && store.SaveDatabase() && store.LoadDatabase();
    std::vector<double> times;
    times.reserve(1000);
    for (int i = 0; ok && i < 1000; ++i) {
        store.ReleasePlaintext();
        const auto start = std::chrono::steady_clock::now();
        ok = store.RestorePlaintext() && store.GetUsers().size() == 5;
        store.ReleasePlaintext();
        times.push_back(std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count());
    }
    std::sort(times.begin(), times.end());
    if (ok) std::printf("V8 actual RestorePlaintext + ReleasePlaintext (5 accounts/15 templates, n=1000, current-user identity): median=%.4f ms p95=%.4f ms max=%.4f ms\n",
        times[499], times[949], times.back());
    CleanupTempRoot(root);
    return ok ? 0 : 1;
}

int wmain(int argc, wchar_t** argv) {
    // Synthetic cross-identity probe, never uses the installed database.
    // Write under SYSTEM, copy to a normal user's readable directory, then
    // run --reject-scope-probe as that user to verify the production identity boundary.
    if (argc == 3 && std::wstring(argv[1]) == L"--write-scope-probe") {
        if (!facelogin::DpapiUtil::IsSystemContext()) return 2;
        auto encrypted = facelogin::DpapiUtil::Protect(L"synthetic-system-scope-probe");
        if (encrypted.empty()) return 1;
        auto plain = facelogin::DpapiUtil::Unprotect(encrypted);
        const std::wstring expected = L"synthetic-system-scope-probe";
        const bool valid = plain.size() == expected.size() * sizeof(wchar_t) &&
            memcmp(plain.data(), expected.data(), plain.size()) == 0;
        if (!plain.empty()) SecureZeroMemory(plain.data(), plain.size());
        if (!valid) return 1;
        std::ofstream file(argv[2], std::ios::binary | std::ios::trunc);
        file.write(reinterpret_cast<const char*>(encrypted.data()), encrypted.size());
        file.close();
        return file ? 0 : 1;
    }
    if (argc == 3 && std::wstring(argv[1]) == L"--reject-scope-probe") {
        if (facelogin::DpapiUtil::IsSystemContext()) return 2;
        std::ifstream file(argv[2], std::ios::binary);
        std::vector<uint8_t> encrypted((std::istreambuf_iterator<char>(file)), {});
        if (encrypted.empty()) return 1;
        auto plain = facelogin::DpapiUtil::Unprotect(encrypted);
        const bool rejected = plain.empty();
        if (!plain.empty()) SecureZeroMemory(plain.data(), plain.size());
        std::puts(rejected ? "Cross-identity probe: decryption rejected" : "Cross-identity probe: FAILED");
        return rejected ? 0 : 1;
    }
    if (argc == 2 && std::wstring(argv[1]) == L"--benchmark-unseal") return BenchmarkTemplateUnseal();
    if (argc != 1) return 2;
    TestEncryptedTemplateBoundary();
    TestPrivacyDeletion();
    TestIdentityScopedProtection();
    TestAtomicSaveFailure();
    TestLegacyPasswordMigration();
    TestIdentityMatching();
    TestThresholds();
    TestPasswordBlobValidation();
    TestDeleteFace();
    TestV7RoundTripAndReload();
    TestUpdateTemplateFace();
    TestV7NominalAnglesAndLegacyRead();
    TestUnsupportedVersionsAreRejected();
    TestMalformedReloadIsTransactional();
    if (g_failures != 0) {
        std::fprintf(stderr, "CredentialStoreTest: %d failure(s)\n", g_failures);
        return 1;
    }
    std::puts("CredentialStoreTest: all checks passed");
    return 0;
}
