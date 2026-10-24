#pragma once

#include <windows.h>
#include <dpapi.h>
#include <vector>
#include <string>
#include <cstdint>

namespace facelogin {

// DPAPI utility functions for encrypting/decrypting sensitive data.
// User-scope protection. Production credentials are protected by the SYSTEM
// service, not by the interactive enrollment process.
class DpapiUtil {
public:
    // Encrypt plaintext data with DPAPI (current identity scope).
    // Returns empty vector on failure.
    static std::vector<uint8_t> Protect(const std::vector<uint8_t>& plaintext);
    static std::vector<uint8_t> Protect(const std::wstring& plaintext);
    static std::vector<uint8_t> Protect(const uint8_t* data, size_t size);

    // Decrypt data previously encrypted with Protect().
    // Returns empty vector on failure.
    static std::vector<uint8_t> Unprotect(const std::vector<uint8_t>& ciphertext);
    static std::vector<uint8_t> Unprotect(const uint8_t* data, size_t size);

    static bool IsSystemContext();

private:
    static constexpr DWORD kFlags = CRYPTPROTECT_UI_FORBIDDEN;
};

} // namespace facelogin
