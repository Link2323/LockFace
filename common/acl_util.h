#pragma once

#include <string>

namespace facelogin {

// True for a real directory owned by SYSTEM, Administrators or
// TrustedInstaller whose DACL grants no non-admin write-class rights,
// including inherit-only grants. Unknown ACE forms, reparse points and
// inspection errors are not trusted. This is the production-mode check.
bool IsDirectoryAdminWriteProtected(const std::wstring& dir);

// Create with Administrators ownership and a protected, inheritable DACL:
// SYSTEM/Administrators full control; optionally Users read & execute.
// Reject existing low-privilege owners and reparse points, without taking
// ownership. Repair trusted-owner objects and verify the requested policy.
// With allowUserRead=false, enforce the policy on every existing file and
// directory below dir, including explicit/protected file ACEs.
// Failures log the operation, path and error. Ancestor path security remains
// the caller's responsibility; this is not a general adversarial path resolver.
bool EnsureSecureDirectory(const std::wstring& dir, bool allowUserRead = true);

} // namespace facelogin
