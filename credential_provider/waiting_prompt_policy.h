#pragma once

#include <cstdint>

namespace facelogin::credential_provider {

// Presentation only: this policy never decides whether authentication starts.
class WaitingPromptPolicy {
public:
    bool Update(std::uint64_t now, bool credentialViewActive,
                bool releaseMayTrigger) noexcept {
        if (!credentialViewActive || releaseMayTrigger) {
            m_timing = false;
            return false;
        }
        if (!m_timing) {
            m_timing = true;
            m_promptAt = now + 300;
        }
        return now >= m_promptAt;
    }

private:
    bool m_timing = false;
    std::uint64_t m_promptAt = 0;
};

} // namespace facelogin::credential_provider
