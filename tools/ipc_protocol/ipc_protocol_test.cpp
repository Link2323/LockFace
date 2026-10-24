#include "../../common/ipc_protocol.h"
#include "../../common/secure_clear.h"

#include <cstdio>
#include <cstring>
#include <string>

namespace {

using facelogin::ipc::AuthResult;

int g_failures = 0;

void Check(bool condition, const char* description) {
    if (!condition) {
        std::fprintf(stderr, "FAIL: %s\n", description);
        ++g_failures;
    }
}

void TestMsaRoundTrip() {
    const std::wstring message = facelogin::ipc::BuildAuthSuccessMessage(
        L"S-1-5-21-100", L"alice@example.test", L"DESKTOP", L"alice",
        L"p:a:ss:word");
    const AuthResult parsed = facelogin::ipc::ParseAuthMessage(message);
    Check(parsed.status == AuthResult::Status::Success,
          "MSA success message parses");
    Check(parsed.sid == L"S-1-5-21-100" &&
          parsed.upn == L"alice@example.test" &&
          parsed.domain == L"DESKTOP" && parsed.username == L"alice" &&
          parsed.password == L"p:a:ss:word",
          "MSA fields and password colons survive round trip");
}

void TestLocalAccountRoundTrip() {
    const std::wstring message = facelogin::ipc::BuildAuthSuccessMessage(
        L"S-1-5-21-200", L"", L"DESKTOP", L"local-user", L"local-password");
    Check(message ==
              L"AUTH_SUCCESS:S-1-5-21-200::DESKTOP\\local-user:local-password",
          "local account wire format preserves the empty UPN field");
    const AuthResult parsed = facelogin::ipc::ParseAuthMessage(message);
    Check(parsed.status == AuthResult::Status::Success &&
          parsed.sid == L"S-1-5-21-200" && parsed.upn.empty() &&
          parsed.domain == L"DESKTOP" && parsed.username == L"local-user" &&
          parsed.password == L"local-password",
          "local account success message round trips");
}

void TestNonAsciiPasswordRoundTrip() {
    // Audit 1.9.0 finding 1 acceptance: colons AND non-ASCII characters must
    // both survive the single-copy in-place parse.
    const std::wstring password = L"密码:pässwörd::密码";
    const std::wstring message = facelogin::ipc::BuildAuthSuccessMessage(
        L"S-1-5-21-300", L"bob@example.test", L"DESKTOP", L"bob", password);
    const AuthResult parsed = facelogin::ipc::ParseAuthMessage(message);
    Check(parsed.status == AuthResult::Status::Success &&
              parsed.password == password,
          "password with colons and non-ASCII characters round trips");
}

// ----------------------------------------------------------------------------
// Memory-hygiene regression (audit 1.9.0 finding 1, acceptance 1, parser
// scope): after the caller-owned buffers are cleared, the plaintext password
// must not linger in any committed private memory region. The parser must not
// create intermediate password copies — std::wstring frees without scrubbing,
// so a stale substr copy stays readable in the heap until the pages are
// reused. The needle is a function-local static: it lives in the image's
// read-only data, which the scan skips by memory type (MEM_IMAGE), so the
// pattern itself can never be a false positive. Only meaningful in Release:
// the debug CRT overwrites freed blocks with fill patterns, masking stale
// copies (the test still passes there, just less sharply).
// ----------------------------------------------------------------------------

bool IsReadableProtection(DWORD protect) {
    switch (protect) {
    case PAGE_READONLY:
    case PAGE_READWRITE:
    case PAGE_WRITECOPY:
    case PAGE_EXECUTE_READ:
    case PAGE_EXECUTE_READWRITE:
    case PAGE_EXECUTE_WRITECOPY:
        return true;
    default:
        return false;  // PAGE_NOACCESS, guard/no-cache modifiers, etc.
    }
}

size_t CountPatternInPrivateMemory(const wchar_t* pattern, size_t chars) {
    const wchar_t first = pattern[0];
    const BYTE* needle = reinterpret_cast<const BYTE*>(pattern);
    const size_t bytes = chars * sizeof(wchar_t);
    SYSTEM_INFO si = {};
    GetSystemInfo(&si);
    MEMORY_BASIC_INFORMATION mbi = {};
    size_t matches = 0;
    for (BYTE* address = static_cast<BYTE*>(si.lpMinimumApplicationAddress);
         address < si.lpMaximumApplicationAddress;) {
        if (!VirtualQuery(address, &mbi, sizeof(mbi))) break;
        if (mbi.State == MEM_COMMIT && mbi.Type == MEM_PRIVATE &&
            IsReadableProtection(mbi.Protect) && mbi.RegionSize >= bytes) {
            const BYTE* base = static_cast<const BYTE*>(mbi.BaseAddress);
            const BYTE* last = base + mbi.RegionSize - bytes;
            for (const BYTE* p = base; p <= last; ++p) {
                if (*reinterpret_cast<const wchar_t*>(p) == first &&
                    std::memcmp(p, needle, bytes) == 0) {
                    ++matches;
                }
            }
        }
        address = static_cast<BYTE*>(mbi.BaseAddress) + mbi.RegionSize;
    }
    return matches;
}

void TestParseLeavesNoPasswordCopiesInMemory() {
    static const wchar_t kSecret[] =
        L"S3cret-密码-Пароль:pw-0123456789-unique";  // >7 wchars: no SSO
    std::wstring message;
    {
        // Bind the needle to a named wstring: passing kSecret directly would
        // materialize an un-scrubbed temporary wstring for the const&
        // parameter and the scan would (correctly) find that test artifact.
        std::wstring passwordArg(kSecret);
        message = facelogin::ipc::BuildAuthSuccessMessage(
            L"S-1-5-21-400", L"carol@example.test", L"DESKTOP", L"carol",
            passwordArg);
        facelogin::SecureClearWString(passwordArg);
    }
    AuthResult parsed = facelogin::ipc::ParseAuthMessage(message);
    Check(parsed.status == AuthResult::Status::Success &&
              parsed.password == kSecret,
          "memory-hygiene setup: password parses");
    // Emulate the CP receiver: clear every owned password-bearing buffer.
    facelogin::SecureClearWString(parsed.password);
    facelogin::SecureClearWString(message);
    const size_t stale = CountPatternInPrivateMemory(
        kSecret, (sizeof(kSecret) / sizeof(kSecret[0])) - 1);
    Check(stale == 0,
          "no plaintext password remains in private memory after parse+clear");
    if (stale != 0) {
        std::fprintf(stderr, "found %zu stale copy(ies)\n", stale);
    }
}

void TestLegacyFormatIsRejected() {
    const AuthResult domainUser = facelogin::ipc::ParseAuthMessage(
        L"AUTH_SUCCESS:OLDPC\\legacy:old:password");
    Check(domainUser.status == AuthResult::Status::Error,
          "legacy DOMAIN\\USER success format is rejected");

    const AuthResult bareUser = facelogin::ipc::ParseAuthMessage(
        L"AUTH_SUCCESS:legacy:password");
    Check(bareUser.status == AuthResult::Status::Error,
          "legacy bare username success format is rejected");

    const AuthResult colonHeavyLegacy = facelogin::ipc::ParseAuthMessage(
        L"AUTH_SUCCESS:OLDPC\\legacy:one:two:three");
    Check(colonHeavyLegacy.status == AuthResult::Status::Error,
          "legacy success message with multiple password colons is rejected");

    const AuthResult missingDomain = facelogin::ipc::ParseAuthMessage(
        L"AUTH_SUCCESS:S-1-5-21-100::local-user:password");
    Check(missingDomain.status == AuthResult::Status::Error,
          "current-format success message without DOMAIN\\USER is rejected");
}

void TestTerminalMessages() {
    Check(facelogin::ipc::ParseAuthMessage(L"AUTH_TIMEOUT").status ==
              AuthResult::Status::Timeout,
          "timeout terminal parses");
    const std::wstring errorMessage =
        facelogin::ipc::BuildAuthErrorMessage(L"camera unavailable");
    const AuthResult error = facelogin::ipc::ParseAuthMessage(errorMessage);
    Check(error.status == AuthResult::Status::Error &&
          error.errorMessage == L"camera unavailable",
          "error terminal round trips");
}

void TestAcknowledgementMessages() {
    Check(std::wstring(facelogin::ipc::MSG_AUTH_ACK) == L"AUTH_ACK",
          "authentication delivery ACK wire value is stable");
    Check(std::wstring(facelogin::ipc::MSG_CONTROL_ACK) == L"CONTROL_ACK",
          "control-response delivery ACK wire value is stable");
    Check(facelogin::ipc::PIPE_ACK_TIMEOUT_MS == 2000,
          "response ACK deadline remains bounded at two seconds");
}

void TestMalformedMessagesFailClosed() {
    const AuthResult empty = facelogin::ipc::ParseAuthMessage(L"");
    Check(empty.status == AuthResult::Status::Error && !empty.errorMessage.empty(),
          "empty message fails closed");

    const AuthResult truncated = facelogin::ipc::ParseAuthMessage(
        L"AUTH_SUCCESS:no-password-separator");
    Check(truncated.status == AuthResult::Status::Error &&
          !truncated.errorMessage.empty(),
          "truncated success message fails closed");

    const AuthResult unknown = facelogin::ipc::ParseAuthMessage(L"UNRECOGNIZED");
    Check(unknown.status == AuthResult::Status::Error &&
          !unknown.errorMessage.empty(),
          "unknown message fails closed");
}

} // namespace

int wmain() {
    TestMsaRoundTrip();
    TestLocalAccountRoundTrip();
    TestNonAsciiPasswordRoundTrip();
    TestParseLeavesNoPasswordCopiesInMemory();
    TestLegacyFormatIsRejected();
    TestTerminalMessages();
    TestAcknowledgementMessages();
    TestMalformedMessagesFailClosed();
    if (g_failures != 0) {
        std::fprintf(stderr, "IpcProtocolTest: %d failure(s)\n", g_failures);
        return 1;
    }
    std::puts("IpcProtocolTest: all checks passed");
    return 0;
}
