#include "log_viewer.h"
#include "service_client.h"
#include "../../face_service/pipe_server.h"
#include "session_identity.h"
#include "../../common/account_identity.h"
#include "enrollment_accounts.h"
#include "enrollment_capture_health.h"
#include "ipc_protocol.h"

#include <windows.h>
#include <sddl.h>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>
#include <vector>

int RunPreviewFrameEncoderTests();
int RunPrivacyWithdrawalTests();

namespace {

using facelogin::EnrollmentLogSource;
using facelogin::ReadEnrollmentLog;
using facelogin::ClearEnrollmentLog;
namespace ipc = facelogin::ipc;
namespace fs = std::filesystem;

int failures = 0;

void Check(bool condition, const char* message) {
    if (!condition) {
        std::fprintf(stderr, "FAIL: %s\n", message);
        ++failures;
    }
}

void WriteBytes(const fs::path& path, const std::string& bytes) {
    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    file.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    Check(file.good(), "write test log fixture");
}

void TestLogs(const fs::path& dataDir) {
    const auto logDir = dataDir / L"log";
    std::error_code error;
    Check(fs::create_directory(logDir, error), "create fixture log directory");
    if (error) return;
    const std::wstring dir = dataDir.wstring();

    Check(ReadEnrollmentLog(dir, EnrollmentLogSource::Service) ==
              "[\"service.log not available\"]", "missing log retains placeholder");
    Check(ClearEnrollmentLog(dir, EnrollmentLogSource::Service),
          "clearing a missing log succeeds");

    const std::string utf8 = "你好 \"face\" C:\\models\r\nsecond\n\n";
    WriteBytes(logDir / L"console.log", utf8);
    Check(ReadEnrollmentLog(dir, EnrollmentLogSource::Console) ==
              "[\"你好 \\\"face\\\" C:\\\\models\",\"second\"]",
          "UTF-8 lines preserve Unicode and escape quotes and backslashes");

    const std::wstring legacy = L"\xfeff旧日志\r\nsecond\r\n";
    WriteBytes(logDir / L"auth_worker.log",
               std::string(reinterpret_cast<const char*>(legacy.data()),
                           legacy.size() * sizeof(wchar_t)));
    Check(ReadEnrollmentLog(dir, EnrollmentLogSource::AuthWorker) ==
              "[\"旧日志\",\"second\"]", "legacy UTF-16LE BOM log decodes");
    const std::wstring noBom = L"worker legacy\r\n";
    WriteBytes(logDir / L"auth_worker.log",
               std::string(reinterpret_cast<const char*>(noBom.data()),
                           noBom.size() * sizeof(wchar_t)));
    Check(ReadEnrollmentLog(dir, EnrollmentLogSource::AuthWorker) ==
              "[\"worker legacy\"]", "legacy UTF-16LE without BOM decodes");

    WriteBytes(logDir / L"service.log", std::string(300 * 1024, 'x') + "\r\nlatest\r\n");
    Check(ReadEnrollmentLog(dir, EnrollmentLogSource::Service) == "[\"latest\"]",
          "tail cap discards partial first line and retains latest complete line");
    WriteBytes(logDir / L"service.log", std::string(300 * 1024, 'x'));
    Check(ReadEnrollmentLog(dir, EnrollmentLogSource::Service) == "[]",
          "oversized line without newline is not displayed as a fragment");

    WriteBytes(logDir / L"credential_provider.log", "provider\r\n");
    Check(ReadEnrollmentLog(dir, EnrollmentLogSource::CredentialProvider) ==
              "[\"provider\"]", "provider source reads the provider file");
    WriteBytes(logDir / L"console.2000-01-01.log", "archived\r\n");
    HANDLE writer = CreateFileW((logDir / L"console.log").c_str(), FILE_APPEND_DATA,
                                FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                                OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    Check(writer != INVALID_HANDLE_VALUE, "open append-mode live log writer");
    Check(ClearEnrollmentLog(dir, EnrollmentLogSource::Console), "clear live console log");
    Check(ReadEnrollmentLog(dir, EnrollmentLogSource::Console) == "[]", "cleared log is empty");
    if (writer != INVALID_HANDLE_VALUE) {
        const std::string next = "after clear\r\n";
        DWORD written = 0;
        Check(WriteFile(writer, next.data(), static_cast<DWORD>(next.size()), &written, nullptr) &&
                  written == next.size(), "live writer appends after clear");
        CloseHandle(writer);
        Check(ReadEnrollmentLog(dir, EnrollmentLogSource::Console) == "[\"after clear\"]",
              "live append resumes at the new end without holes");
    }
    Check(fs::file_size(logDir / L"console.2000-01-01.log", error) == 10 && !error,
          "clearing active log preserves rotated archive");
    Check(ReadEnrollmentLog(dir, EnrollmentLogSource::CredentialProvider) == "[\"provider\"]",
          "clearing console leaves other sources untouched");
    Check(ClearEnrollmentLog(dir, EnrollmentLogSource::AuthWorker), "clear worker source");
    Check(ClearEnrollmentLog(dir, EnrollmentLogSource::CredentialProvider), "clear provider source");
    Check(ReadEnrollmentLog(dir, EnrollmentLogSource::AuthWorker) == "[]" &&
              ReadEnrollmentLog(dir, EnrollmentLogSource::CredentialProvider) == "[]",
          "source-specific clears target their own files");
    WriteBytes(logDir / L"auth_worker.log", "keep on failure\r\n");
    HANDLE locked = CreateFileW((logDir / L"auth_worker.log").c_str(), GENERIC_READ,
                                FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    Check(locked != INVALID_HANDLE_VALUE, "hold log without write sharing");
    if (locked != INVALID_HANDLE_VALUE) {
        Check(!ClearEnrollmentLog(dir, EnrollmentLogSource::AuthWorker),
              "locked log reports clear failure");
        CloseHandle(locked);
        Check(ReadEnrollmentLog(dir, EnrollmentLogSource::AuthWorker) == "[\"keep on failure\"]",
              "failed log clear preserves file contents");
    }
}

// The server uses its own uniquely named pipe, never the installed service.
// Every test-side I/O has a deadline so a broken handshake cannot hang the suite.
bool CompleteIo(HANDLE pipe, OVERLAPPED& operation, BOOL immediate, DWORD& bytes) {
    if (!immediate && GetLastError() != ERROR_IO_PENDING) return false;
    if (!immediate && WaitForSingleObject(operation.hEvent, 5000) != WAIT_OBJECT_0) {
        CancelIoEx(pipe, &operation);
        GetOverlappedResult(pipe, &operation, &bytes, TRUE);
        return false;
    }
    return GetOverlappedResult(pipe, &operation, &bytes, FALSE) != FALSE;
}

bool ReadMessage(HANDLE pipe, std::wstring& message) {
    wchar_t buffer[128] = {};
    OVERLAPPED operation{};
    operation.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!operation.hEvent) return false;
    DWORD bytes = 0;
    BOOL immediate = ReadFile(pipe, buffer, sizeof(buffer), &bytes, &operation);
    bool ok = CompleteIo(pipe, operation, immediate, bytes);
    CloseHandle(operation.hEvent);
    if (ok) message.assign(buffer, bytes / sizeof(wchar_t));
    return ok;
}

bool WriteMessage(HANDLE pipe, const std::wstring& message) {
    OVERLAPPED operation{};
    operation.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!operation.hEvent) return false;
    DWORD bytes = 0;
    const DWORD size = static_cast<DWORD>((message.size() + 1) * sizeof(wchar_t));
    BOOL immediate = WriteFile(pipe, message.c_str(), size, &bytes, &operation);
    bool ok = CompleteIo(pipe, operation, immediate, bytes) && bytes == size;
    CloseHandle(operation.hEvent);
    return ok;
}

enum class Request { Database, Config, Learning };

void TestServiceRequest(Request kind, const std::wstring& reply, bool accepted,
                        bool disconnect = false) {
    static unsigned int sequence = 0;
    const std::wstring name = L"\\\\.\\pipe\\FaceLogin.EnrollmentSupportTest." +
        std::to_wstring(GetCurrentProcessId()) + L"." + std::to_wstring(++sequence);
    HANDLE pipe = CreateNamedPipeW(name.c_str(), PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED,
                                   PIPE_TYPE_MESSAGE | PIPE_READMODE_MESSAGE | PIPE_WAIT,
                                   1, 65536, 65536, 0, nullptr);
    Check(pipe != INVALID_HANDLE_VALUE, "create isolated test pipe");
    if (pipe == INVALID_HANDLE_VALUE) return;

    std::wstring request;
    std::wstring acknowledgement;
    bool serverOk = false;
    DWORD serverError = ERROR_SUCCESS;
    std::thread server([&] {
        OVERLAPPED operation{};
        operation.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        if (!operation.hEvent) return;
        DWORD bytes = 0;
        BOOL immediate = ConnectNamedPipe(pipe, &operation);
        bool connected = !immediate && GetLastError() == ERROR_PIPE_CONNECTED;
        if (!connected) connected = CompleteIo(pipe, operation, immediate, bytes);
        CloseHandle(operation.hEvent);
        if (connected && ReadMessage(pipe, request)) {
            if (disconnect) serverOk = true;
            else serverOk = WriteMessage(pipe, reply) && ReadMessage(pipe, acknowledgement);
        }
        if (!serverOk) serverError = GetLastError();
        DisconnectNamedPipe(pipe);
    });

    facelogin::EnrollmentServiceClient client(name);
    bool result = false;
    std::string learning;
    switch (kind) {
    case Request::Database: result = client.ReloadDatabase(); break;
    case Request::Config: result = client.ReloadConfig(); break;
    case Request::Learning: learning = client.QueryLearningStatus(); break;
    }
    const DWORD clientError = GetLastError();
    server.join();
    CloseHandle(pipe);

    if (!serverOk) {
        std::fprintf(stderr, "Pipe case %u (request=%d, disconnect=%d): server error=%lu, client error=%lu, request units=%zu, ack units=%zu\n",
                     sequence, static_cast<int>(kind), disconnect, serverError, clientError,
                     request.size(), acknowledgement.size());
    }

    const wchar_t* expected = kind == Request::Database ? ipc::MSG_RELOAD_DB :
        kind == Request::Config ? ipc::MSG_CONFIG_RELOAD : ipc::MSG_LEARNING_STATUS;
    std::wstring expectedMessage(expected);
    expectedMessage.push_back(L'\0');
    Check(serverOk && request == expectedMessage, "service request uses the existing wire message and terminator");
    if (!disconnect) {
        std::wstring ack(ipc::MSG_CONTROL_ACK);
        ack.push_back(L'\0');
        Check(acknowledgement == ack, "every received response is acknowledged, including rejection");
    }
    if (kind == Request::Learning) {
        Check(learning == (disconnect ? "{\"error\":\"Service did not respond\"}" :
                                       "{\"status\":\"就绪\"}"),
              "learning query preserves JSON/UTF-8 response and disconnect error");
    } else {
        Check(result == accepted, "reload succeeds only for its matching OK response");
    }
}

void TestServiceClient() {
    TestServiceRequest(Request::Database, ipc::MSG_RELOAD_OK, true);
    TestServiceRequest(Request::Database, L"RELOAD_FAILED", false);
    TestServiceRequest(Request::Config, ipc::MSG_CONFIG_RELOAD_OK, true);
    TestServiceRequest(Request::Config, ipc::MSG_RELOAD_OK, false);
    TestServiceRequest(Request::Config, L"", false, true);
    TestServiceRequest(Request::Learning, L"{\"status\":\"就绪\"}", true);
    TestServiceRequest(Request::Learning, L"", false, true);

    facelogin::EnrollmentServiceClient absent(L"\\\\.\\pipe\\FaceLogin.AbsentEnrollmentTest." +
                                               std::to_wstring(GetCurrentProcessId()));
    Check(!absent.ReloadDatabase() && !absent.ReloadConfig(), "unavailable service cannot confirm a reload");
    Check(absent.QueryLearningStatus() == "{\"error\":\"Service not running\"}",
          "unavailable service retains learning error response");
}

void TestPasswordEndpointVerification() {
    const std::wstring name = L"\\\\.\\pipe\\LockFace.FakePasswordService." +
        std::to_wstring(GetCurrentProcessId());
    HANDLE pipe = CreateNamedPipeW(name.c_str(), PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED,
        PIPE_TYPE_MESSAGE | PIPE_READMODE_MESSAGE | PIPE_WAIT, 1, 4096, 4096, 0, nullptr);
    Check(pipe != INVALID_HANDLE_VALUE, "create fake password service");
    if (pipe == INVALID_HANDLE_VALUE) return;
    std::wstring received;
    std::thread server([&] {
        OVERLAPPED op{};
        op.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        DWORD bytes = 0;
        BOOL immediate = ConnectNamedPipe(pipe, &op);
        bool connected = !immediate && GetLastError() == ERROR_PIPE_CONNECTED;
        if (!connected) connected = CompleteIo(pipe, op, immediate, bytes);
        CloseHandle(op.hEvent);
        if (connected) ReadMessage(pipe, received);
        DisconnectNamedPipe(pipe);
    });
    facelogin::EnrollmentServiceClient client(name);
    std::vector<uint8_t> encrypted{1, 2};
    Check(!client.ProtectPassword(L"must-not-reach-fake-service", encrypted) && encrypted.empty(),
          "password protection rejects a server outside the SCM SYSTEM service");
    server.join();
    CloseHandle(pipe);
    Check(received.empty(), "fake service receives no password bytes");
    Check(!client.ProtectPassword(L"", encrypted), "empty password rejected");
    Check(!client.ProtectPassword(std::wstring(257, L'x'), encrypted), "oversized password rejected");
    Check(!client.ProtectPassword(std::wstring(L"a\0b", 3), encrypted), "embedded NUL password rejected");
}

void TestDeletionEndpointVerification() {
    const std::wstring name = L"\\\\.\\pipe\\LockFace.FakeDeletionService." +
        std::to_wstring(GetCurrentProcessId());
    HANDLE pipe = CreateNamedPipeW(name.c_str(), PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED,
        PIPE_TYPE_MESSAGE | PIPE_READMODE_MESSAGE | PIPE_WAIT, 1, 4096, 4096, 0, nullptr);
    Check(pipe != INVALID_HANDLE_VALUE, "create fake password service");
    if (pipe == INVALID_HANDLE_VALUE) return;
    std::wstring received;
    std::thread server([&] {
        OVERLAPPED op{};
        op.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        DWORD bytes = 0;
        BOOL immediate = ConnectNamedPipe(pipe, &op);
        bool connected = !immediate && GetLastError() == ERROR_PIPE_CONNECTED;
        if (!connected) connected = CompleteIo(pipe, op, immediate, bytes);
        CloseHandle(op.hEvent);
        if (connected) ReadMessage(pipe, received);
        DisconnectNamedPipe(pipe);
    });
    facelogin::EnrollmentServiceClient client(name);
    Check(!client.DeleteEnrollment(L"S-1-5-21-111-222-333-1001"),
          "deletion rejects a server outside the SCM SYSTEM service");
    server.join();
    CloseHandle(pipe);
    Check(received.empty(), "fake service receives no deletion target");
    Check(!client.DeleteEnrollment(L"bad-sid"), "malformed deletion target rejected before connection");
}

void TestPipeClientAuthorization() {
    BYTE adminSid[SECURITY_MAX_SID_SIZE]{};
    DWORD sidSize = sizeof(adminSid);
    BOOL elevated = FALSE;
    Check(CreateWellKnownSid(WinBuiltinAdministratorsSid, nullptr, adminSid, &sidSize) &&
          CheckTokenMembership(nullptr, adminSid, &elevated), "read enabled administrator membership");
    for (DWORD level : {SECURITY_IDENTIFICATION, SECURITY_IMPERSONATION, SECURITY_ANONYMOUS}) {
        const std::wstring name = L"\\\\.\\pipe\\LockFace.AuthorizationTest." +
            std::to_wstring(GetCurrentProcessId()) + L"." + std::to_wstring(level);
        facelogin::PipeServer pipeServer;
        bool exchange = false, privileged = false, restored = false;
        std::thread server([&] {
            std::wstring request;
            if (!pipeServer.WaitForClient(5000, name.c_str()) ||
                !pipeServer.ReadMessage(request, 5000)) return;
            exchange = request == L"AUTHORIZATION_TEST";
            privileged = pipeServer.IsClientPrivileged();
            HANDLE token = nullptr;
            const BOOL opened = OpenThreadToken(GetCurrentThread(), TOKEN_QUERY, TRUE, &token);
            restored = !opened && GetLastError() == ERROR_NO_TOKEN;
            if (opened) { CloseHandle(token); RevertToSelf(); }
            pipeServer.Disconnect();
        });
        HANDLE client = INVALID_HANDLE_VALUE;
        const auto deadline = GetTickCount64() + 5000;
        do {
            client = CreateFileW(name.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr,
                OPEN_EXISTING, SECURITY_SQOS_PRESENT | level, nullptr);
            if (client != INVALID_HANDLE_VALUE) break;
            Sleep(5);
        } while (GetTickCount64() < deadline);
        if (client != INVALID_HANDLE_VALUE) {
            const wchar_t request[] = L"AUTHORIZATION_TEST";
            DWORD written = 0;
            WriteFile(client, request, sizeof(request), &written, nullptr);
        }
        server.join();
        if (client != INVALID_HANDLE_VALUE) CloseHandle(client);
        const bool expected = elevated && level != SECURITY_ANONYMOUS;
        Check(exchange && restored, "authorization checks the connected client and restores thread identity");
        if (privileged != expected) {
            std::fprintf(stderr, "Authorization level=%lu, elevated=%d: expected=%d actual=%d\n",
                         level, elevated, expected, privileged);
        }
        Check(privileged == expected,
              "Identification/Impersonation accept enabled admins; anonymous and unelevated clients are denied");
    }
}

// Upstream #24: the session SID must come from the process token, never from
// a name lookup that silently returns the machine SID when computer name ==
// user name. These invariants hold on every machine; the collision itself is
// exercised structurally by TestMachineSidRepair below.
void TestSessionIdentity() {
    namespace si = facelogin::session_identity;
    const auto identity = si::ReadSessionIdentity();
    const auto tokenSid = si::GetCurrentProcessUserSid();
    Check(!tokenSid.empty(), "process token user SID resolves");
    Check(identity.sid == tokenSid, "ReadSessionIdentity adopts the authoritative token SID");
    Check(si::SidIsUserAccount(tokenSid), "session token SID resolves to a user account");

    Check(!si::SidIsUserAccount(L""), "empty SID is not a user account");
    Check(!si::SidIsUserAccount(L"S-1-5-18"), "well-known SYSTEM SID is not a user account");
    Check(!si::SidIsUserAccount(L"S-1-5-21-1-2-3-4"), "unresolvable SID is not a user account");

    // The pollution source itself: the computer name must resolve to a
    // NON-user SID — that is exactly what a pre-fix build stored when the
    // two names collided.
    wchar_t computer[256] = {};
    DWORD computerLen = ARRAYSIZE(computer);
    if (GetComputerNameW(computer, &computerLen)) {
        DWORD sidSize = 0, domainSize = 0;
        SID_NAME_USE sidType = SidTypeUnknown;
        LookupAccountNameW(nullptr, computer, nullptr, &sidSize, nullptr, &domainSize, &sidType);
        if (sidSize > 0) {
            std::vector<BYTE> sidBuf(sidSize);
            std::vector<wchar_t> domainBuf(domainSize > 0 ? domainSize : 1);
            if (LookupAccountNameW(nullptr, computer, sidBuf.data(), &sidSize,
                                   domainBuf.data(), &domainSize, &sidType)) {
                LPWSTR sidStr = nullptr;
                if (ConvertSidToStringSidW(reinterpret_cast<PSID>(sidBuf.data()), &sidStr)) {
                    const std::wstring computerSid = sidStr;
                    LocalFree(sidStr);
                    Check(!facelogin::session_identity::SidIsUserAccount(computerSid),
                          "computer-name SID is not a user account (pollution is detectable)");
                }
            }
        }
    }
}

// Resolves the machine SID (S-1-5-21-X-Y-Z) the way the pre-fix build did on
// machines where computer name == user name: LookupAccountNameW of the
// computer name.
std::wstring ResolveComputerSidForTest() {
    wchar_t computer[256] = {};
    DWORD computerLen = ARRAYSIZE(computer);
    if (!GetComputerNameW(computer, &computerLen)) return L"";
    DWORD sidSize = 0, domainSize = 0;
    SID_NAME_USE sidType = SidTypeUnknown;
    LookupAccountNameW(nullptr, computer, nullptr, &sidSize, nullptr, &domainSize, &sidType);
    if (sidSize == 0) return L"";
    std::vector<BYTE> sidBuf(sidSize);
    std::vector<wchar_t> domainBuf(domainSize > 0 ? domainSize : 1);
    if (!LookupAccountNameW(nullptr, computer, sidBuf.data(), &sidSize,
                            domainBuf.data(), &domainSize, &sidType)) return L"";
    LPWSTR sidStr = nullptr;
    std::wstring sid;
    if (ConvertSidToStringSidW(reinterpret_cast<PSID>(sidBuf.data()), &sidStr)) {
        sid = sidStr;
        LocalFree(sidStr);
    }
    return sid;
}

// End-to-end repair: a record stored with the machine SID (upstream #24
// pollution) is rewritten to the session token SID on startup, keeping
// username/UPN/password bytes/faces; a record keyed by a genuine user SID
// (local Administrator, machineSid + "-500") is left untouched.
void TestMachineSidRepair(const fs::path& dataDir) {
    namespace si = facelogin::session_identity;
    const std::wstring machineSid = ResolveComputerSidForTest();
    Check(!machineSid.empty(), "resolve machine SID for fixture");
    const std::wstring tokenSid = si::GetCurrentProcessUserSid();
    Check(!tokenSid.empty(), "resolve session token SID for fixture");
    if (machineSid.empty() || tokenSid.empty()) return;

    const std::wstring adminSid = machineSid + L"-500";
    Check(si::SidIsUserAccount(adminSid),
          "local Administrator SID is a genuine user account (fixture)");

    std::error_code error;
    Check(fs::create_directory(dataDir, error) && !error, "create sid-repair fixture directory");

    // What ReadSessionIdentity computed on the collision machine.
    wchar_t username[256] = {};
    DWORD usernameLen = ARRAYSIZE(username);
    si::SessionIdentity identity;
    if (GetUserNameW(username, &usernameLen) && usernameLen > 0) {
        identity.username = username;
    }
    identity.sid = machineSid;
    identity.accountType = "local";

    facelogin::CredentialStore store;
    store.SetDataDir(dataDir.wstring());
    const std::vector<uint8_t> password = {0x41, 0x42, 0x43};
    const std::vector<float> embedding(facelogin::kRecognizerDimension, 0.25f);
    uint32_t faceId = 0;
    Check(store.AddFace(identity.username, L"", machineSid, password, embedding,
                        L"正面", &faceId, 0.0f, 0.0f),
          "create polluted record keyed by machine SID");
    Check(store.AddFace(L"admin-untouched", L"", adminSid, password, embedding,
                        L"正面", &faceId, 0.0f, 0.0f),
          "create untouched record keyed by a genuine user SID");
    Check(store.SaveDatabase(), "persist polluted fixture");

    // One-shot service stub answering the single RELOAD_DB the repair issues.
    const std::wstring pipeName = L"\\\\.\\pipe\\FaceLogin.EnrollmentSupportTest.SidRepair." +
        std::to_wstring(GetCurrentProcessId());
    HANDLE pipe = CreateNamedPipeW(pipeName.c_str(), PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED,
                                   PIPE_TYPE_MESSAGE | PIPE_READMODE_MESSAGE | PIPE_WAIT,
                                   1, 65536, 65536, 0, nullptr);
    Check(pipe != INVALID_HANDLE_VALUE, "create sid-repair test pipe");
    if (pipe == INVALID_HANDLE_VALUE) return;
    std::thread server([&] {
        OVERLAPPED operation{};
        operation.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        if (!operation.hEvent) return;
        DWORD bytes = 0;
        BOOL immediate = ConnectNamedPipe(pipe, &operation);
        bool connected = !immediate && GetLastError() == ERROR_PIPE_CONNECTED;
        if (!connected) connected = CompleteIo(pipe, operation, immediate, bytes);
        CloseHandle(operation.hEvent);
        std::wstring request, acknowledgement;
        if (connected && ReadMessage(pipe, request)) {
            WriteMessage(pipe, ipc::MSG_RELOAD_OK);
            ReadMessage(pipe, acknowledgement);
        }
        DisconnectNamedPipe(pipe);
    });

    facelogin::EnrollmentServiceClient client(pipeName);
    facelogin::EnrollmentAccounts accounts(dataDir.wstring(), identity, client);
    const bool repaired = accounts.AutoRepairMachineSidOnStartup();
    server.join();
    CloseHandle(pipe);
    Check(repaired, "machine-SID record repaired and reload confirmed");

    facelogin::CredentialStore verify;
    verify.SetDataDir(dataDir.wstring());
    Check(verify.LoadDatabase(), "reload repaired database");
    const auto& users = verify.GetUsers();
    Check(users.size() == 2, "repair rewrites in place, never creates accounts");
    const size_t repairedIdx = verify.FindUserIndex(tokenSid, L"", L"");
    Check(repairedIdx < users.size(), "repaired record is keyed by the token SID");
    if (repairedIdx < users.size()) {
        Check(lstrcmpiW(users[repairedIdx].username.c_str(), username) == 0,
              "repaired record keeps the username");
        Check(users[repairedIdx].encryptedPassword == password,
              "repaired record keeps the stored credential bytes");
        Check(users[repairedIdx].faces.size() == 1, "repaired record keeps its faces");
    }
    const size_t untouchedIdx = verify.FindUserIndex(adminSid, L"", L"");
    Check(untouchedIdx < users.size() && users[untouchedIdx].sid == adminSid &&
              users[untouchedIdx].faces.size() == 1,
          "genuine user-SID record is untouched");
}

void TestCaptureFrameWatchdog() {
    using facelogin::CaptureFrameWatchdog;
    using namespace std::chrono_literals;
    const CaptureFrameWatchdog::Clock::time_point start{};
    CaptureFrameWatchdog stream(start);
    Check(!stream.TimedOut(start + 4999ms), "new capture allows camera startup before deadline");
    Check(stream.TimedOut(start + 5s), "no initial camera frame ends at deadline");
    Check(stream.AcceptFrame(1, start + 1s), "first fresh frame resets camera deadline");
    Check(!stream.AcceptFrame(1, start + 5s), "repeated cache frame cannot reset camera deadline");
    Check(stream.TimedOut(start + 6s), "camera stall times out even after partial sample progress");
    Check(stream.AcceptFrame(2, start + 6s), "new camera sequence permits a recovered frame");
    Check(!stream.TimedOut(start + 10s), "fresh frame resets camera deadline");
    // Continuous video is healthy while the user takes time to turn their head.
    for (std::uint64_t i = 3; i < 100; ++i) {
        const auto now = start + std::chrono::seconds(i + 4);
        Check(stream.AcceptFrame(i, now) && !stream.TimedOut(now),
              "fresh video does not time out while waiting for a pose");
    }
}

void TestAccountMutationOutcomes(const fs::path& dataDir) {
    std::error_code error;
    if (!fs::create_directory(dataDir, error) || error) {
        Check(false, "create account mutation fixture");
        return;
    }
    const auto identity = facelogin::session_identity::ReadSessionIdentity();
    facelogin::CredentialStore store;
    store.SetDataDir(dataDir.wstring());
    const std::vector<uint8_t> password = {0x41, 0x42, 0x43};
    const std::vector<float> embedding(facelogin::kRecognizerDimension, 0.25f);
    uint32_t faceId = 0;
    Check(store.AddFace(identity.username, identity.upn, identity.sid, password, embedding,
                        L"original", &faceId, 0.0f, 0.0f) && store.SaveDatabase(),
          "seed account mutation fixture");
    // The isolated nonexistent endpoint cannot acknowledge the persisted change.
    const std::wstring pipeName = L"\\\\.\\pipe\\LockFace.NoMutationService." +
        std::to_wstring(GetCurrentProcessId()) + L"." + std::to_wstring(GetTickCount64());
    facelogin::EnrollmentServiceClient client(pipeName);
    facelogin::EnrollmentAccounts accounts(dataDir.wstring(), identity, client);
    Check(!accounts.RenameFace(faceId, L"renamed") && accounts.GetSaveStatus() == "reload_failed",
          "rename distinguishes persisted change from missing service acknowledgement");
    Check(accounts.GetFacesJson().find("renamed") != std::string::npos,
          "renamed label can be reloaded from disk despite missing acknowledgement");
    Check(!accounts.DeleteFace(-1) && accounts.GetSaveStatus() == "failed",
          "failed validation clears a preceding persisted outcome");
    Check(!accounts.DeleteFace(faceId) && accounts.GetSaveStatus() == "failed" && accounts.GetFaceCount() == 1,
          "missing service cannot acknowledge deletion or change the database");
    Check(!accounts.ClearAllFaces() && accounts.GetSaveStatus() == "failed" && accounts.GetFaceCount() == 1,
          "missing service leaves withdrawal incomplete and retryable");
}

} // namespace

int main() {
    failures += RunPreviewFrameEncoderTests();
    failures += RunPrivacyWithdrawalTests();
    std::error_code error;
    const auto tempDir = fs::temp_directory_path(error);
    if (error) return 1;
    const auto dataDir = tempDir / (L"LockFaceEnrollmentSupportTest-" +
        std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetTickCount64()));
    if (!fs::create_directory(dataDir, error) || error) return 1;
    TestLogs(dataDir);
    TestServiceClient();
    TestPasswordEndpointVerification();
    TestDeletionEndpointVerification();
    TestPipeClientAuthorization();
    TestSessionIdentity();
    {
        std::wstring domain, sam;
        const auto sid = facelogin::session_identity::GetCurrentProcessUserSid();
        Check(facelogin::account_identity::ResolveUserSid(sid, domain, sam) && !domain.empty() && !sam.empty(), "real token SID resolves to user authority and SAM");
        Check(!facelogin::account_identity::ResolveUserSid(L"invalid-sid", domain, sam) && domain.empty() && sam.empty(), "invalid SID fails and clears outputs");
        Check(!facelogin::account_identity::ResolveUserSid(L"S-1-5-32-544", domain, sam), "group SID rejected as login identity");
    }
    TestMachineSidRepair(dataDir / L"sidrepair");
    TestCaptureFrameWatchdog();
    TestAccountMutationOutcomes(dataDir / L"mutations");
    // Only remove the unique directory successfully created by this test.
    fs::remove_all(dataDir, error);
    Check(!error, "remove temporary test fixtures");
    std::printf("EnrollmentSupportTest: %s (%d failures)\n", failures ? "FAILED" : "PASS", failures);
    return failures ? 1 : 0;
}
