#pragma once

#include <string>

namespace facelogin {

// Directory containing the running EXE (canonical, no trailing backslash;
// empty on failure). The trust anchor for the checks below.
std::wstring GetExeDirPath();

// True when the EXE's own directory is admin-write-protected — production
// deployment. This is the trust-mode switch ResolveSecureDataDir uses;
// callers reuse it to decide whether a data directory at the EXE dir is an
// install location (must stay untouched) or a developer build tree.
bool IsProductionExePath();

// Resolves the runtime data directory with a security whitelist check.
//
// Trust model (defense against DataPath registry redirection — security #3):
//   The allowed data directory depends on the EXE directory's owner and ACL.
//
//   Production mode (EXE in an admin-write-protected directory — typically
//   Program Files on any drive, verified via its owner/DACL rather than the path
//   prefix):
//     Only the EXE's own directory is trusted. The registry DataPath value
//     MUST point there; any other value (including the %ProgramData%\LockFace
//     fallback) is rejected. This closes the redirection attack surface: an
//     attacker who can change DataPath gains nothing, because the only
//     permitted target is already an admin-protected install directory.
//
//   Development mode (EXE in a low-privilege-writable location, e.g.
//   build\ under the developer's profile):
//     The EXE directory, %ProgramData%\LockFace, and the caller-supplied
//     dataDirHint are all trusted. This keeps development harnesses usable
//     on developer machines that are single-user and trusted; the service
//     additionally locks down the fallback directory's DACL at startup
//     (common/acl_util.h, EnsureSecureDirectory).
//
// dataDirHint: caller-supplied override (non-empty in dev harnesses; empty
//              in production, where the registry is authoritative).
// outReason:   if the function returns empty, set to a short reason string
//              suitable for logging.
// Returns:     the trusted data directory, or empty on rejection (fail-closed).
std::wstring ResolveSecureDataDir(const std::wstring& dataDirHint,
                                  std::wstring* outReason = nullptr);

} // namespace facelogin
