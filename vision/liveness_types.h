#pragma once

#include <chrono>

namespace facelogin {

// Calibration and validation use independent five-frame attempts. The
// strict tier (unlock_frames=5) mirrors that exactly: five valid frames,
// every frame must pass, identity binds counted frames 1/3/5. The standard
// tier (3) counts three frames with every frame binding identity — the
// user-facing default since 2026-10-24, previously the opt-in fast-unlock
// mode. The ultra-fast tier (2) counts two frames, every frame binding.
// Both reduced tiers are user-facing speed/security trade-offs, NOT
// calibration results (docs/work/completed/threshold-calibration.md is
// five-frame; neither 3- nor 2-frame mode was recalibrated and both keep
// the same per-frame 0.281 threshold). An attack must pass every counted
// frame, so per-frame false-accept q yields q^5 / q^3 / q^2.
inline int AntiSpoofCheckCount(int unlockFrames) {
    return (unlockFrames == 5 || unlockFrames == 2) ? unlockFrames : 3;
}

// Identity bindings the parent must accept before a round can succeed:
// the 5-frame tier binds counted frames 1/3/5 (three probes), the reduced
// tiers bind every counted frame.
inline int BindingCountRequired(int unlockFrames) {
    return unlockFrames == 2 ? 2 : 3;
}

inline int AntiSpoofPassRequired(int checkCount) {
    return checkCount;
}

// Pure timing gate for the anti-spoof loop's fast-fail decisions, kept free
// of models, cameras and wall-clock reads so the semantics stay
// unit-testable:
//   - the 2.5 s empty-scene timer times from the PAD window start;
//   - the 2 s persistent-attack timer times from the FIRST detected face,
//     so a user who steps into the frame late still reaches PAD inference
//     instead of an instant attack verdict;
//   - the attack fast fail applies only before the first PAD pass — after a
//     pass, all-fail sequences are bounded by the every-frame-pass rule and
//     the 8 s window, never by a second attack timer;
//   - identity Retry has its own 2 s clock, independent of PAD passes. An
//     accepted binding resets it; sensor settling pauses it with the others.
struct LivenessTiming {
    using TimePoint = std::chrono::steady_clock::time_point;

    static constexpr double kEmptySceneSeconds = 2.5;
    static constexpr double kAttackSeconds = 2.0;
    static constexpr double kIdentityRetrySeconds = 2.0;
    static constexpr double kWindowSeconds = 8.0;

    explicit LivenessTiming(TimePoint windowStart)
        : m_windowStart(windowStart), m_noFaceStart(windowStart) {}

    // Per-iteration check before grabbing a frame.
    bool WindowExpired(TimePoint now) const {
        return Elapsed(now, m_windowStart) >= kWindowSeconds;
    }

    // The scene has stayed empty: no face has ever been detected.
    bool ShouldFailEmptyScene(TimePoint now) const {
        return !m_anyFaceSeen && Elapsed(now, m_noFaceStart) >= kEmptySceneSeconds;
    }

    // Call after a frame with a detected face.
    void OnFaceDetected(TimePoint now) {
        if (!m_anyFaceSeen) {
            m_anyFaceSeen = true;
            m_firstFaceSeenAt = now;
        }
    }

    // Bracket a pre-binding exposure correction (auth_pipeline.cpp) with
    // Suspend/Resume: sensor settling is capture preparation, not liveness
    // evaluation, so the paused span is subtracted by shifting every timer
    // origin forward. Without this, a bounded tune that starts before the
    // first counted frame burns the 2.5 s empty-scene budget (and, once a
    // face has been seen, the 2 s attack budget) while the loop is not even
    // evaluating frames. The 8 s PAD window pauses with them.
    void Suspend(TimePoint now) {
        m_suspendedAt = now;
        m_suspended = true;
    }
    void Resume(TimePoint now) {
        if (!m_suspended) return;
        m_suspended = false;
        const auto shift = now - m_suspendedAt;
        m_windowStart += shift;
        m_noFaceStart += shift;
        // Unread while !m_anyFaceSeen; shifting it anyway keeps one rule.
        m_firstFaceSeenAt += shift;
        if (m_identityRetryPending) m_identityRetryStartedAt += shift;
    }

    // Call after a frame with a detected face; must never fire on the frame
    // that first sees the face (that face still gets a PAD inference).
    bool ShouldFailPersistentAttack(TimePoint now) const {
        return m_anyFaceSeen && m_passCount == 0 &&
               Elapsed(now, m_firstFaceSeenAt) >= kAttackSeconds;
    }

    void OnPadScore(bool passed) {
        if (passed) ++m_passCount;
    }

    // Called only after an explicit binding Retry. Camera/embedding failures
    // do not start this clock, and expiry is checked on a fresh identity miss.
    bool OnIdentityRetry(TimePoint now) {
        if (!m_identityRetryPending) {
            m_identityRetryPending = true;
            m_identityRetryStartedAt = now;
        }
        return Elapsed(now, m_identityRetryStartedAt) >= kIdentityRetrySeconds;
    }

    void OnIdentityAccepted() { m_identityRetryPending = false; }
    bool IdentityRetryPending() const { return m_identityRetryPending; }

    bool AnyFaceSeen() const { return m_anyFaceSeen; }
    int PassCount() const { return m_passCount; }

private:
    static double Elapsed(TimePoint now, TimePoint since) {
        return std::chrono::duration<double>(now - since).count();
    }

    TimePoint m_windowStart;
    TimePoint m_noFaceStart;
    bool m_anyFaceSeen = false;
    TimePoint m_firstFaceSeenAt{};
    bool m_identityRetryPending = false;
    TimePoint m_identityRetryStartedAt{};
    int m_passCount = 0;
    bool m_suspended = false;
    TimePoint m_suspendedAt{};
};

} // namespace facelogin
