#pragma once

#include <windows.h>

namespace facelogin::acl_detail {

// Shared policy for filesystem enforcement and security-descriptor tests.
bool MeetsDirectoryPolicy(PSECURITY_DESCRIPTOR sd, bool allowUserRead, bool requireProtected);

} // namespace facelogin::acl_detail
