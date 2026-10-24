#include "service_client.h"
#include "../common/ipc_protocol.h"
#include "../common/logger.h"
#include "../common/text_util.h"
#include "../common/secure_clear.h"

#include <utility>

namespace facelogin {

EnrollmentServiceClient::EnrollmentServiceClient()
    : EnrollmentServiceClient(ipc::PIPE_NAME) {}

EnrollmentServiceClient::EnrollmentServiceClient(std::wstring pipeName)
    : m_pipeName(std::move(pipeName)) {}

static bool SendControlAck(HANDLE hPipe) {
    std::wstring ack(ipc::MSG_CONTROL_ACK);
    ack.push_back(L'\0');
    const DWORD expected = static_cast<DWORD>(ack.size() * sizeof(wchar_t));
    DWORD written = 0;
    return WriteFile(hPipe, ack.c_str(), expected, &written, nullptr) &&
           written == expected;
}

// Outcome of one request/response round trip. The empty string is never a
// legitimate reply on these message types, so `ok` false always means the
// exchange failed with `response` empty and `error` holding the Windows
// error for the caller's retry/context logging; `ok` true hands the
// response to the caller for interpretation.
struct RequestAckOutcome {
    bool ok = false;
    std::wstring response;
    DWORD error = ERROR_SUCCESS;
};

// One request/response round trip on a connected public pipe: write the
// request, poll up to replyTimeoutMs for the reply, read the complete
// response message, then acknowledge with CONTROL_ACK so the service can
// disconnect. Callers own the handle (message mode, retries, closing).
static RequestAckOutcome RequestAckRoundTrip(HANDLE hPipe,
                                              const std::wstring& request,
                                              DWORD replyTimeoutMs,
                                              DWORD pollStepMs,
                                              const wchar_t* ackContext) {
    RequestAckOutcome outcome;

    std::wstring message;
    // Reserve the terminator before copying a secret: push_back must not
    // reallocate and leave an uncleared password in the old allocation.
    message.reserve(request.size() + 1);
    SecureWStringGuard messageGuard(message);
    message.append(request);
    message.push_back(L'\0');
    const DWORD expectedBytes =
        static_cast<DWORD>(message.size() * sizeof(wchar_t));
    DWORD written = 0;
    if (!WriteFile(hPipe, message.c_str(), expectedBytes, &written, nullptr)) {
        outcome.error = GetLastError();
        return outcome;
    }
    if (written != expectedBytes) {
        outcome.error = ERROR_WRITE_FAULT;
        return outcome;
    }

    DWORD bytesAvailable = 0;
    bool pipeBroken = false;
    for (DWORD waited = 0; waited < replyTimeoutMs; waited += pollStepMs) {
        if (!PeekNamedPipe(hPipe, nullptr, 0, nullptr, &bytesAvailable, nullptr)) {
            outcome.error = GetLastError();
            pipeBroken = true;
            break;
        }
        if (bytesAvailable > 0) break;
        Sleep(pollStepMs);
    }
    if (pipeBroken) return outcome;
    if (bytesAvailable == 0) {
        outcome.error = ERROR_TIMEOUT;
        return outcome;
    }

    // Message-mode pipe: a message larger than the read buffer returns
    // ERROR_MORE_DATA with a partial chunk; keep reading until ReadFile
    // reports the message complete.
    std::wstring response;
    wchar_t buffer[4096];
    DWORD bytesRead = 0;
    for (;;) {
        const BOOL ok = ReadFile(hPipe, buffer,
                                 static_cast<DWORD>(sizeof(buffer) - sizeof(wchar_t)),
                                 &bytesRead, nullptr);
        if (ok || GetLastError() == ERROR_MORE_DATA) {
            response.append(buffer, bytesRead / sizeof(wchar_t));
            if (ok) break;
            continue;
        }
        outcome.error = GetLastError();
        return outcome;
    }
    while (!response.empty() && response.back() == L'\0') {
        response.pop_back();
    }
    if (response.empty()) {
        outcome.error = ERROR_READ_FAULT;
        return outcome;
    }

    if (!SendControlAck(hPipe)) {
        FACELOGIN_WARN(L"Failed to acknowledge %s response: %lu",
                       ackContext, GetLastError());
    }
    outcome.ok = true;
    outcome.response = std::move(response);
    return outcome;
}

// A password must never be sent to an unverified pipe-name squatter.
// Match both the SCM service PID and its SYSTEM token before writing anything.
static bool IsSystemServicePipe(HANDLE pipe) {
    ULONG pid = 0;
    if (!GetNamedPipeServerProcessId(pipe, &pid)) return false;
    SC_HANDLE scm = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT);
    if (!scm) return false;
    SC_HANDLE service = OpenServiceW(scm, L"LockFaceService", SERVICE_QUERY_STATUS);
    SERVICE_STATUS_PROCESS status{};
    DWORD bytes = 0;
    const bool matches = service && QueryServiceStatusEx(service, SC_STATUS_PROCESS_INFO,
        reinterpret_cast<BYTE*>(&status), sizeof(status), &bytes) &&
        status.dwCurrentState == SERVICE_RUNNING && status.dwProcessId == pid;
    if (service) CloseServiceHandle(service);
    CloseServiceHandle(scm);
    if (!matches) return false;
    HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!process) return false;
    HANDLE token = nullptr;
    bool system = false;
    if (OpenProcessToken(process, TOKEN_QUERY, &token)) {
        BYTE user[sizeof(TOKEN_USER) + SECURITY_MAX_SID_SIZE]{};
        DWORD size = 0;
        system = GetTokenInformation(token, TokenUser, user, sizeof(user), &size) &&
            IsWellKnownSid(reinterpret_cast<TOKEN_USER*>(user)->User.Sid, WinLocalSystemSid);
        CloseHandle(token);
    }
    CloseHandle(process);
    return system;
}

bool EnrollmentServiceClient::ProtectPassword(const std::wstring& password,
                                             std::vector<uint8_t>& encrypted) const {
    encrypted.clear();
    if (password.empty() || password.size() > ipc::MAX_PASSWORD_CHARS ||
        password.find(L'\0') != std::wstring::npos) return false;
    HANDLE pipe = INVALID_HANDLE_VALUE;
    for (unsigned attempt = 0; attempt < 5; ++attempt) {
        pipe = CreateFileW(m_pipeName.c_str(), GENERIC_READ | GENERIC_WRITE, 0,
                          nullptr, OPEN_EXISTING, SECURITY_SQOS_PRESENT | SECURITY_IDENTIFICATION,
                          nullptr);
        if (pipe != INVALID_HANDLE_VALUE || GetLastError() != ERROR_PIPE_BUSY) break;
        WaitNamedPipeW(m_pipeName.c_str(), 1000);
    }
    if (pipe == INVALID_HANDLE_VALUE) return false;
    DWORD mode = PIPE_READMODE_MESSAGE;
    if (!IsSystemServicePipe(pipe) || !SetNamedPipeHandleState(pipe, &mode, nullptr, nullptr)) {
        CloseHandle(pipe);
        FACELOGIN_ERROR(L"Password protection refused: unverified service endpoint");
        return false;
    }
    std::wstring request(ipc::MSG_PROTECT_PASSWORD_PREFIX);
    SecureWStringGuard guard(request);
    request.append(password);
    auto outcome = RequestAckRoundTrip(pipe, request, 5000, 25, L"password protection");
    CloseHandle(pipe);
    const std::wstring prefix(ipc::MSG_PROTECT_PASSWORD_OK_PREFIX);
    if (!outcome.ok || outcome.response.compare(0, prefix.size(), prefix) != 0) return false;
    const size_t digits = outcome.response.size() - prefix.size();
    if (digits < 4 || digits > 8192 || digits % 2) return false;
    auto hex = [](wchar_t c) -> int {
        if (c >= L'0' && c <= L'9') return c - L'0';
        if (c >= L'A' && c <= L'F') return c - L'A' + 10;
        return -1;
    };
    for (size_t i = prefix.size(); i < outcome.response.size(); i += 2) {
        const int hi = hex(outcome.response[i]), lo = hex(outcome.response[i + 1]);
        if (hi < 0 || lo < 0) { encrypted.clear(); return false; }
        encrypted.push_back(static_cast<uint8_t>((hi << 4) | lo));
    }
    return true;
}

// Notify the LockFace service to reload the user database after a write and
// keep the connection alive until the service acknowledges RELOAD_OK. The
// service accepts connections with an event-driven ConnectNamedPipe and waits
// for CONTROL_ACK before disconnecting, so both directions have an explicit,
// bounded delivery handshake.
bool EnrollmentServiceClient::ReloadDatabase() const {
    constexpr DWORD kAttempts = 5;
    constexpr DWORD kBusyWaitMs = 1000;
    constexpr DWORD kReplyTimeoutMs = 5000;

    DWORD lastError = ERROR_SUCCESS;
    for (DWORD attempt = 1; attempt <= kAttempts; ++attempt) {
        HANDLE hPipe = CreateFileW(m_pipeName.c_str(), GENERIC_READ | GENERIC_WRITE,
                                   0, nullptr, OPEN_EXISTING, 0, nullptr);
        if (hPipe == INVALID_HANDLE_VALUE) {
            lastError = GetLastError();
            if (lastError == ERROR_PIPE_BUSY) {
                WaitNamedPipeW(m_pipeName.c_str(), kBusyWaitMs);
                continue;
            }
            FACELOGIN_ERROR(L"Database reload connection failed (attempt %lu/%lu): %lu",
                            attempt, kAttempts, lastError);
            break;
        }

        DWORD readMode = PIPE_READMODE_MESSAGE;
        SetNamedPipeHandleState(hPipe, &readMode, nullptr, nullptr);

        RequestAckOutcome outcome =
            RequestAckRoundTrip(hPipe, ipc::MSG_RELOAD_DB, kReplyTimeoutMs, 25,
                                L"database reload");
        CloseHandle(hPipe);
        if (outcome.ok) {
            if (outcome.response == ipc::MSG_RELOAD_OK) {
                FACELOGIN_INFO(L"Service confirmed database reload");
                return true;
            }
            FACELOGIN_ERROR(L"Service rejected database reload: %s",
                            outcome.response.c_str());
            return false;
        }
        lastError = outcome.error;
        FACELOGIN_WARN(L"Database reload round trip failed (attempt %lu/%lu): %lu",
                       attempt, kAttempts, lastError);
        Sleep(50);
    }

    FACELOGIN_ERROR(L"users.dat was saved but the service did not confirm reload: %lu",
                    lastError);
    return false;
}

// Learning-observability query. Unlike the log views (which read files to
// stay off the single-instance pipe), this needs live learner state, so it
// takes the public pipe for one short request/response round trip. The view
// throttles to one query per 10s for the same reason.
std::string EnrollmentServiceClient::QueryLearningStatus() const {
    HANDLE hPipe = CreateFileW(m_pipeName.c_str(), GENERIC_READ | GENERIC_WRITE, 0,
                               nullptr, OPEN_EXISTING, 0, nullptr);
    if (hPipe == INVALID_HANDLE_VALUE) {
        return "{\"error\":\"Service not running\"}";
    }
    // The service assembles the response synchronously — no model or worker
    // work happens on this path — so a short bounded wait is enough.
    RequestAckOutcome outcome =
        RequestAckRoundTrip(hPipe, ipc::MSG_LEARNING_STATUS, 3000, 50,
                            L"learning status");
    CloseHandle(hPipe);
    if (!outcome.ok) {
        return "{\"error\":\"Service did not respond\"}";
    }
    return WstrToUtf8(outcome.response);
}

bool EnrollmentServiceClient::ReloadConfig() const {
    HANDLE hPipe = CreateFileW(m_pipeName.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr,
                               OPEN_EXISTING, 0, nullptr);
    if (hPipe == INVALID_HANDLE_VALUE) {
        FACELOGIN_ERROR(L"Configuration saved but service is unavailable for reload: %lu",
                        GetLastError());
        return false;
    }
    // A service-mode CONFIG_RELOAD can replace a preloaded worker so the
    // selected camera backend takes effect before acknowledging. Allow its
    // bounded startup handshake to complete.
    RequestAckOutcome outcome =
        RequestAckRoundTrip(hPipe, ipc::MSG_CONFIG_RELOAD, 25000, 50,
                            L"configuration reload");
    CloseHandle(hPipe);
    if (!outcome.ok) {
        return false;
    }
    if (outcome.response != ipc::MSG_CONFIG_RELOAD_OK) {
        FACELOGIN_ERROR(L"Service rejected configuration reload: %s",
                        outcome.response.c_str());
        return false;
    }
    return true;
}

} // namespace facelogin
