#pragma once

#include <chrono>
#include <cstdint>

namespace facelogin {

enum class EnrollmentCaptureFailure {
    None, NoFrames, NoFace, LivenessFailed, LivenessTimeout, InferenceFailed, SampleFailed
};

inline const char* CaptureFailureName(EnrollmentCaptureFailure reason) {
    switch (reason) {
    case EnrollmentCaptureFailure::NoFrames: return "no_frames";
    case EnrollmentCaptureFailure::NoFace: return "no_face";
    case EnrollmentCaptureFailure::LivenessFailed: return "liveness_failed";
    case EnrollmentCaptureFailure::LivenessTimeout: return "liveness_timeout";
    case EnrollmentCaptureFailure::InferenceFailed: return "inference_failed";
    case EnrollmentCaptureFailure::SampleFailed: return "sample_failed";
    default: return "";
    }
}

// Fresh camera frames, rather than collected samples, drive this watchdog:
// waiting for a head turn is healthy, while repeated cached frames are not.
class CaptureFrameWatchdog {
public:
    using Clock = std::chrono::steady_clock;
    explicit CaptureFrameWatchdog(Clock::time_point now = Clock::now()) : m_lastFresh(now) {}
    std::uint64_t LastSequence() const { return m_sequence; }
    bool AcceptFrame(std::uint64_t sequence, Clock::time_point now = Clock::now()) {
        if (sequence == m_sequence) return false;
        m_sequence = sequence;
        m_lastFresh = now;
        return true;
    }
    bool TimedOut(Clock::time_point now = Clock::now()) const {
        return now - m_lastFresh >= std::chrono::seconds(5);
    }

private:
    std::uint64_t m_sequence = 0;
    Clock::time_point m_lastFresh;
};

} // namespace facelogin
