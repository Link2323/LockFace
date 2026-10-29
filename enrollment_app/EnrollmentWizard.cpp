#include "privacy_withdrawal.h"
#include "EnrollmentWizard.h"
#include "preview_exposure.h"
#include "preview_frame_encoder.h"
#include "enrollment_templates.h"
#include "enrollment_accounts.h"
#include "log_viewer.h"
#include "../common/logger.h"
#include "../common/acl_util.h"
#include "../common/registry_util.h"
#include "../common/config_util.h"
#include "../common/text_util.h"
#include "../common/json_util.h"
#include "../common/frame_image.h"
#include "../common/model_hashes.h"
#include "../common/sha256_util.h"
#include <shlobj.h>
#include <thread>
#include <chrono>
#include <cmath>
#include <sstream>
#include <iomanip>

#pragma comment(lib, "ole32.lib")

namespace {

// Preview pacing tiers (see StartPreview). Foreground: the image encodes at
// the camera's own rate and only SCRFD inference is scheduled toward a tier
// interval — 15 fps with a face on camera, 5 fps after none for a while.
// Unfocused (visible but not in the foreground): the image keeps the camera
// rate and detection pins to the no-face interval. Minimized: WebviewHost
// releases the camera entirely (like leaving the cam tab) and restarts it on
// restore — the background interval below only covers the fallback case of a
// capture in flight that kept the camera alive across a minimize. A capture
// in flight always runs at full rate.
constexpr std::chrono::milliseconds kFaceFrameInterval(66);
constexpr std::chrono::milliseconds kNoFaceFrameInterval(200);
constexpr std::chrono::milliseconds kBackgroundFrameInterval(300);
constexpr std::chrono::seconds kFaceHoldTime(3);

} // namespace

namespace facelogin {

EnrollmentWizard::EnrollmentWizard() {
    std::wstring regData = ReadRegString(REGVAL_DATA_PATH, L"");
    if (!regData.empty()) {
        m_dataDir = regData;
    } else {
        wchar_t programData[MAX_PATH];
        if (SUCCEEDED(SHGetFolderPathW(nullptr, CSIDL_COMMON_APPDATA, nullptr, 0, programData))) {
            m_dataDir = std::wstring(programData) + L"\\LockFace";
        } else {
            m_dataDir = L"C:\\ProgramData\\LockFace";
        }
    }
    // Secure the implicit fallback, for which this process may be the first
    // creator. Configured paths belong to the installer/service: the Console
    // and Service EXEs have different build output directories, so the
    // Console's EXE path cannot identify the service's development tree.
    // Failure is non-fatal here; the service enforces its own startup checks.
    if (!regData.empty()) {
        CreateDirectoryW(m_dataDir.c_str(), nullptr);
    } else if (!EnsureSecureDirectory(m_dataDir)) {
        FACELOGIN_ERROR(L"Cannot secure data directory: %s", m_dataDir.c_str());
    }

    // The console's own log file, read back by the log viewer's "Console"
    // source like any other file-based source. v1.7.x named it enrollment.log:
    // MoveFile carries that file over on the first run of the renamed build
    // (fails silently when console.log already exists); pre-UTF-8 content is
    // still rotated aside by RotateAsideLegacyUtf16 inside SetLogFile.
    std::wstring logPath = m_dataDir + L"\\log\\console.log";
    MoveFileW((m_dataDir + L"\\log\\enrollment.log").c_str(), logPath.c_str());
    Logger::Instance().SetLogFile(logPath);
    Logger::Instance().SetMinLevel(LogLevel::Info);

    FACELOGIN_INFO(L"=== Enrollment Wizard started ===");

    m_frameEncoder = std::make_unique<PreviewFrameEncoder>();

    m_identity = session_identity::ReadSessionIdentity();

    m_webcam     = std::make_unique<WebcamCaptureDS>();
    m_accounts = std::make_unique<EnrollmentAccounts>(m_dataDir, m_identity, m_serviceClient);
    // Retry intent that could not reach the service before the last exit. Do
    // this before any preview or account repair; failures retain the UI gate.
    if (PrivacyWithdrawal().Read() == PrivacyWithdrawalState::Pending) {
        PrivacyWithdrawal().Run([this]() { return m_accounts->WithdrawConsent(); });
    }

    m_config = LoadConfig(m_dataDir);
    m_antiSpoofThreshold = m_config.anti_spoof_threshold;
}

EnrollmentWizard::~EnrollmentWizard() {
    StopPreview();
    // The bounded pipe request owns no camera/COM objects. Keep the controller
    // and service client alive until its final status has been published.
    if (m_configSaveThread.joinable()) m_configSaveThread.join();
}

// ============================================================================
// Preview Control
// ============================================================================

bool EnrollmentWizard::StartPreview() {
    if (PrivacyWithdrawal().Read() != PrivacyWithdrawalState::None) {
        FACELOGIN_WARN(L"Enrollment camera refused: withdrawal pending or unreadable");
        return false;
    }
    if (m_previewRunning) return true;

    // The service/credential provider may still be releasing the camera just
    // after unlock. Absorb that short hand-off window instead of reporting a
    // spurious preview failure to the UI.
    constexpr int kInitRetries = 5;
    bool webcamOk = false;
    for (int attempt = 0; attempt < kInitRetries && !webcamOk; ++attempt) {
        webcamOk = m_webcam->Initialize(1280, 720,
                                        Utf8ToWstr(m_config.camera_device));
        if (!webcamOk && attempt + 1 < kInitRetries) {
            FACELOGIN_WARN(L"Webcam init attempt %d failed — retrying", attempt + 1);
            std::this_thread::sleep_for(std::chrono::milliseconds(200));
        }
    }
    if (!webcamOk) {
        FACELOGIN_ERROR(L"Failed to initialize webcam%s",
                        m_config.camera_device.empty() ? L"" : L" (configured device)");
        return false;
    }

    m_previewExposure = std::make_unique<PreviewExposure>(
        *m_webcam, m_dataDir + L"\\camera_tune.state");
    m_previewExposure->Begin();

    std::wstring modelsDir = m_dataDir + L"\\models";

    // Load SCRFD ONNX detector (gnkps variant — provides the 5 alignment
    // keypoints directly; no separate landmark model anymore).
    // 10g tier: ~3.4x faster than 34g at -1% WIDER Face (96.17→95.19).
    // Sessions are process-local and independent of the camera graph. Keep
    // initialized models across tab changes, minimize and session unlock.
    if (!m_onnxDetector || !m_onnxDetector->IsInitialized()) {
        m_onnxDetector = std::make_unique<OnnxDetector>();
        std::wstring detPath = modelsDir + L"\\det_10g_gnkps.onnx";
        if (!m_onnxDetector->Initialize(detPath)) {
            FACELOGIN_ERROR(L"SCRFD detector failed to load — enrollment unavailable");
            m_webcam->Shutdown();
            return false;
        }
    }

    // Load the pinned OpenCV SFace recognizer.
    if (!m_onnxRecognizer || !m_onnxRecognizer->IsInitialized()) {
        m_onnxRecognizer = std::make_unique<OnnxRecognizer>();
        std::wstring onnxPath = modelsDir + L"\\face_recognition_sface_2021dec.onnx";
        if (!VerifyModelIntegrity(onnxPath, model_hashes::kRecognizer, L"OpenCV SFace recognizer") ||
            !m_onnxRecognizer->Initialize(onnxPath)) {
            FACELOGIN_ERROR(L"ONNX recognizer failed to load — enrollment unavailable");
            m_webcam->Shutdown();
            return false;
        }
    }

    // Anti-spoof is mandatory for enrollment.  Allowing capture without it
    // would let a photograph become the trusted template for later logons.
    if (!m_antiSpoof || !m_antiSpoof->IsInitialized()) {
        m_antiSpoof = std::make_unique<OnnxAntiSpoof>();
        std::wstring miniFasV2Path = modelsDir + L"\\MiniFASNetV2.onnx";
        std::wstring miniFasV1SePath = modelsDir + L"\\MiniFASNetV1SE.onnx";
        if (!m_antiSpoof->Initialize(miniFasV2Path, miniFasV1SePath) ||
            !m_antiSpoof->IsInitialized()) {
            FACELOGIN_ERROR(L"Dual MiniFAS PAD unavailable — enrollment refused");
            m_antiSpoof.reset();
            m_webcam->Shutdown();
            return false;
        }
    }

    m_previewExposure->Prepare(*m_onnxDetector, m_config.camera_rotation);

    FACELOGIN_INFO(L"Liveness method: antispoof (mandatory) | Preview started: 1280x720");

    m_previewRunning = true;
    m_frameRunning = true;

    // Single background thread: GrabFrame → JPEG encode → detect → update caches.
    // The UI thread stays completely free; JS polls the caches via GetLatest*().
    m_frameThread = std::thread([this]() {
        int reinitAttempts = 0;
        unsigned long long lastCameraSequence = 0;
        // Pacing state for the loop below. Foreground: the image keeps the
        // camera's own rate and only SCRFD inference follows the tier
        // schedule; unfocused pins the no-face tier; minimized drops the
        // whole pipeline to the lowest tier. Seeding lastFaceSeen with "now"
        // runs the first seconds at full detection rate before the no-face
        // tier kicks in.
        auto lastFaceSeen = std::chrono::steady_clock::now();
        auto nextDetectDue = lastFaceSeen;
        std::optional<OnnxDetector::Detection> lastDet;
        while (m_frameRunning) {
            // Config snapshot: SetConfig replaces m_config on the UI thread
            // while this loop runs (rotation applies live; only a camera
            // change restarts the preview), so copy the two fields the loop
            // reads under the config lock once per iteration.
            std::string cameraDevice;
            int cameraRotation = 0;
            {
                std::lock_guard<std::mutex> lock(m_configMutex);
                cameraDevice = m_config.camera_device;
                cameraRotation = m_config.camera_rotation;
            }
            FrameImage frame;
            unsigned long long frameSequence = 0;
            if (!m_webcam->GrabFrame(frame, &frameSequence, lastCameraSequence)) {
                // GrabFrame self-shuts down after repeated failures, which is
                // common when a camera is taken over during lock/resume.
                // Rebuild it here with a bounded retry budget so the preview
                // recovers without spinning forever on a dead capture graph.
                if (!m_webcam->IsInitialized() && m_frameRunning) {
                    if (++reinitAttempts > 3) {
                        FACELOGIN_ERROR(L"Preview camera re-init exceeded limit");
                        break;
                    }
                    std::this_thread::sleep_for(std::chrono::milliseconds(200));
                    if (!m_frameRunning) break;
                    if (m_webcam->Initialize(1280, 720,
                                             Utf8ToWstr(cameraDevice))) {
                        FACELOGIN_INFO(L"Preview camera re-initialized (%d/3)",
                                       reinitAttempts);
                        m_previewExposure->CameraReopened();
                        reinitAttempts = 0;
                    } else {
                        FACELOGIN_WARN(L"Preview camera re-init %d/3 failed",
                                       reinitAttempts);
                    }
                }
                // No fresh buffer is normal between camera callbacks. The
                // freshness check precedes pixel copying; a short sleep avoids
                // busy polling while adding at most one slice of preview lag.
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
                continue;
            }
            reinitAttempts = 0;
            lastCameraSequence = frameSequence;

            RotateFrame(frame, cameraRotation);

            // Window-state tiers. Minimized: nobody can see the preview at
            // all, so the whole pipeline — grab, detect, encode — drops to
            // the low duty cycle below. Unfocused (visible but not in the
            // foreground): the image keeps the camera rate and only
            // detection pins to the no-face tier. A capture in flight wins
            // full rate in every state.
            const bool captureActive = m_capture.IsRunning();
            const bool minimized = !captureActive &&
                m_minimized.load(std::memory_order_relaxed);
            const bool unfocused = !captureActive && !minimized &&
                m_unfocused.load(std::memory_order_relaxed);
            const auto now = std::chrono::steady_clock::now();

            // Overlay: SCRFD runs on its tier schedule (every frame during a
            // capture or while minimized); frames in between reuse the last
            // detection, so the box and its yaw readout may lag the video by
            // one interval at reduced detection rates. Exposure observation
            // runs only on frames that really detected — a null result means
            // "no face" to that logic, and a skipped frame must not read as
            // one.
            bool detectedThisFrame = false;
            if (m_onnxDetector &&
                (captureActive || minimized || now >= nextDetectDue)) {
                auto det = m_onnxDetector->DetectLargestFace(frame);
                if (det) lastFaceSeen = now;
                m_previewExposure->Observe(frame, det ? &*det : nullptr, frameSequence,
                    *m_onnxDetector, cameraRotation,
                    [this] { return !m_frameRunning || m_capture.IsRunning(); });
                lastDet = std::move(det);
                detectedThisFrame = true;
                nextDetectDue = now +
                    ((minimized || unfocused)
                         ? kNoFaceFrameInterval
                         : (now - lastFaceSeen <= kFaceHoldTime)
                               ? kFaceFrameInterval
                               : kNoFaceFrameInterval);
            }

            // Foreground and unfocused: encode every frame for a smooth
            // preview. Minimized: encode on detection frames only — a
            // low-rate slideshow for a hidden page.
            std::string b64;
            // Scale the encoded frame was shrunk by; the overlay JSON below
            // must land in ENCODED-pixel coordinates, because js/preview.js
            // maps face boxes against the loaded image's natural size.
            float encodeScale = 1.0f;
            if (!minimized || detectedThisFrame) {
                // Encode at the canvas backing size js/preview.js reported:
                // JPEG cost scales with pixel count and upscaling adds no
                // detail, so a normal window encodes at its own size instead
                // of the full 1280×720. Aspect is kept (fit inside the
                // target, never upscale) so the JS contain mapping is
                // unchanged. The full frame still enters the cache — capture
                // reads it at camera resolution.
                const std::uint32_t packed =
                    m_previewEncodeSize.load(std::memory_order_relaxed);
                const int tgtW = static_cast<int>(packed >> 16);
                const int tgtH = static_cast<int>(packed & 0xFFFF);
                if (frame.nc() > 0 && frame.nr() > 0 &&
                    tgtW > 0 && tgtH > 0 &&
                    (tgtW < frame.nc() || tgtH < frame.nr())) {
                    const double scaleW = static_cast<double>(tgtW) / frame.nc();
                    const double scaleH = static_cast<double>(tgtH) / frame.nr();
                    const double scale = scaleW < scaleH ? scaleW : scaleH;
                    FrameImage scaled(
                        static_cast<int>(frame.nr() * scale),
                        static_cast<int>(frame.nc() * scale));
                    ResizeBilinear(frame, scaled);
                    encodeScale = static_cast<float>(scale);
                    m_frameEncoder->Encode(scaled, b64);
                } else {
                    m_frameEncoder->Encode(frame, b64);
                }
            }

            std::string faceJson = "[]";
            if (lastDet) {
                std::vector<facelogin::FaceWithKps> faces;
                FaceWithKps fwl;
                fwl.rect = FaceRect(
                    static_cast<long>(lastDet->x1 * encodeScale),
                    static_cast<long>(lastDet->y1 * encodeScale),
                    static_cast<long>(lastDet->x2 * encodeScale),
                    static_cast<long>(lastDet->y2 * encodeScale));
                for (int k = 0; k < 10; k++)
                    fwl.kps[k] = lastDet->kps[k] * encodeScale;
                faces.push_back(std::move(fwl));
                faceJson = FacesToJson(faces);
            }

            {
                std::lock_guard<std::mutex> lock(m_frameCacheMutex);
                // An unencoded background frame must not clear the last
                // preview image — the stale base64 keeps the hidden page
                // from repainting.
                bool changed = false;
                if (!b64.empty() && b64 != m_latestFrameB64) {
                    m_latestFrameB64 = std::move(b64);
                    changed = true;
                }
                if (faceJson != m_latestFacesJson) {
                    m_latestFacesJson = std::move(faceJson);
                    changed = true;
                }
                if (changed) ++m_previewVersion;
                // Zero-copy publish: the grabbed frame moves into the cache
                // and every reader borrows it immutable — no pixel copies
                // under the lock, on either side.
                m_latestFrame = std::make_shared<const FrameImage>(std::move(frame));
                m_latestFrameSequence = frameSequence;
            }

            // Minimized pacing: hold the whole loop at the low cadence until
            // the window is restored, a capture starts, or the preview
            // stops. Sliced so each of those is honored within one slice.
            if (minimized) {
                const auto frameStart = std::chrono::steady_clock::now();
                while (m_frameRunning && !m_capture.IsRunning() &&
                       m_minimized.load(std::memory_order_relaxed)) {
                    const auto remaining = std::chrono::duration_cast<
                        std::chrono::milliseconds>(frameStart +
                            kBackgroundFrameInterval -
                            std::chrono::steady_clock::now()).count();
                    if (remaining <= 0) break;
                    std::this_thread::sleep_for(
                        std::chrono::milliseconds(remaining < 30 ? remaining : 30));
                }
            }
        }
    });

    return true;
}

void EnrollmentWizard::StopPreview() {
    m_previewRunning = false;
    m_frameRunning = false;

    // Pause the DirectShow graph first, but keep COM references alive until
    // the worker threads have exited. Releasing the graph before join would
    // race with GrabFrame; waiting for join before pausing can
    // hang the UI when the camera was taken over by LogonUI.
    if (m_webcam)
        m_webcam->Pause();

    m_capture.Stop();
    if (m_frameThread.joinable())
        m_frameThread.join();

    if (m_webcam)
        m_webcam->Shutdown();
    m_previewExposure.reset();
    // A new viewing/capture session must not consume a frame from the old graph.
    std::lock_guard<std::mutex> lock(m_frameCacheMutex);
    m_latestFrame.reset();
    m_latestFrameB64.clear();
    m_latestFacesJson = "[]";
    ++m_previewVersion;
}

void EnrollmentWizard::SetPreviewSize(int width, int height) {
    // Sane clamp; the frame thread re-validates against the frame itself.
    if (width < 0) width = 0;
    if (height < 0) height = 0;
    if (width > 0xFFFF) width = 0xFFFF;
    if (height > 0xFFFF) height = 0xFFFF;
    const std::uint32_t packed =
        (static_cast<std::uint32_t>(width) << 16) |
        static_cast<std::uint32_t>(height);
    // One-shot confirmation that the JS bridge actually delivered a target —
    // if this line never appears, the frame thread is still encoding 720p.
    const auto previous = m_previewEncodeSize.exchange(packed, std::memory_order_relaxed);
    if (packed != 0 && previous == 0) {
        FACELOGIN_INFO(L"Preview encode target: %dx%d (canvas backing)",
                       width, height);
    }
}

// ============================================================================
// Per-frame Processing (polled by JS)
// ============================================================================

// Compare the client's cursor under the snapshot lock before copying the JPEG.
// On change, return the version, image and overlay together. Resetting a camera
// session advances the version too, so a page cannot retain an old cursor.
std::string EnrollmentWizard::GetLatestFrameAndFaces(const std::string& knownVersion) {
    std::lock_guard<std::mutex> lock(m_frameCacheMutex);
    const std::string version = std::to_string(m_previewVersion);
    if (knownVersion == version) return {};
    std::string result;
    result.reserve(version.size() + m_latestFrameB64.size() + m_latestFacesJson.size() + 2);
    result += version;
    result += "\x1E";
    result += m_latestFrameB64;
    result += "\x1E";  // record separator
    result += m_latestFacesJson;
    return result;
}

// ============================================================================
// Face Detection -> JSON
// ============================================================================

std::string EnrollmentWizard::FacesToJson(
    const std::vector<facelogin::FaceWithKps>& faces) {
    std::ostringstream js;
    js << "[";
    for (size_t fi = 0; fi < faces.size(); fi++) {
        if (fi > 0) js << ",";
        const auto& f = faces[fi];
        js << "{"
           << "\"x\":" << f.rect.left()
           << ",\"y\":" << f.rect.top()
           << ",\"w\":" << static_cast<int>(f.rect.width())
           << ",\"h\":" << static_cast<int>(f.rect.height())
           << ",\"landmarks\":[";
        for (int i = 0; i < 5; i++) {
            if (i > 0) js << ",";
            js << static_cast<int>(f.kps[i * 2]) << ","
               << static_cast<int>(f.kps[i * 2 + 1]);
        }
        js << "]";
        // Estimated head yaw in degrees (positive = turned toward own left).
        // Exposed for the preview overlay and multi-angle enrollment gating.
        js << ",\"yaw\":" << EstimateYawDeg(f.kps);
        // While capturing, the JS overlay color-codes the yaw readout against
        // this angle's gate.
        if (m_capture.IsRunning())
            js << ",\"targetYaw\":" << m_capture.Status().targetYaw;
        js << "}";
    }
    js << "]";
    return js.str();
}

// ============================================================================
// Enrollment actions (called from JS)
// ============================================================================

std::string EnrollmentWizard::GetUsername() const {
    // Return UPN for MSA accounts, SAM name for local accounts
    const std::wstring& display = m_identity.upn.empty() ? m_identity.username : m_identity.upn;
    return WstrToUtf8(display);
}

bool EnrollmentWizard::CaptureFaceSamples(int angleIndex) {
    if (PrivacyWithdrawal().Read() != PrivacyWithdrawalState::None) {
        FACELOGIN_WARN(L"Enrollment camera refused: withdrawal pending or unreadable");
        return false;
    }
    if (!m_previewRunning || !m_antiSpoof || !m_onnxDetector || !m_onnxRecognizer) {
        FACELOGIN_ERROR(L"CaptureFaceSamples refused: preview or mandatory models are unavailable");
        return false;
    }
    return m_capture.Start(angleIndex, *m_onnxDetector, *m_onnxRecognizer,
        *m_antiSpoof, m_antiSpoofThreshold,
        [this](std::uint64_t& sequence) -> std::shared_ptr<const FrameImage> {
            std::lock_guard<std::mutex> lock(m_frameCacheMutex);
            if (!m_latestFrame || m_latestFrameSequence == sequence) return nullptr;
            sequence = m_latestFrameSequence;
            return m_latestFrame;   // shared borrow; no pixel copy under the lock
        });
}

void EnrollmentWizard::CancelCapture() { m_capture.Cancel(); }

// ============================================================================
// Multi-angle capture status (polled by JS during capture)
// ============================================================================

std::string EnrollmentWizard::GetCaptureStatus() {
    const auto status = m_capture.Status();
    std::ostringstream js;
    js << "{\"angle\":" << status.angle
       << ",\"label\":\"" << WstrToUtf8(status.label) << "\""
       << ",\"targetYaw\":" << status.targetYaw
       << ",\"collected\":" << status.collected
       << ",\"target\":" << EnrollmentCapture::kAngleTargetFrames
       << ",\"yaw\":" << status.yaw
       << ",\"total\":" << status.total
       << ",\"livenessChecking\":" << (status.checking ? "true" : "false")
       << ",\"livenessPassed\":" << (status.passed ? "true" : "false")
       << ",\"cancelled\":" << (status.cancelled ? "true" : "false")
       << ",\"failureReason\":\"" << CaptureFailureName(status.failure) << "\""
       << ",\"done\":" << (status.done ? "true" : "false") << "}";
    return js.str();
}

std::string EnrollmentWizard::GetUserSid() const {
    return WstrToUtf8(m_identity.sid);
}

bool EnrollmentWizard::ValidatePassword(const std::wstring& password) {
    return session_identity::ValidatePassword(m_identity, password);
}

bool EnrollmentWizard::SaveEnrollment(const std::wstring& password,
                                      const std::wstring& label) {
    return SaveEnrollmentImpl(password, label);
}

bool EnrollmentWizard::SaveEnrollmentImpl(const std::wstring& password,
                                          const std::wstring& label, bool append) {
    m_enrollmentSaveStatus = "failed";
    if (PrivacyWithdrawal().Read() != PrivacyWithdrawalState::None) {
        FACELOGIN_WARN(L"Enrollment save refused: withdrawal pending or unreadable");
        return false;
    }
    EnrollmentSamples samples;
    if (!m_capture.CopyCompletedSamples(samples)) return false;
    std::vector<EnrollmentTemplate> templates;
    if (!BuildEnrollmentTemplates(samples, templates)) return false;
    EnrollmentSaveOutcome outcome;
    const bool acknowledged = m_accounts->SaveTemplates(templates, password, label, append, outcome);
    if (outcome.persisted) {
        // A failed reload ACK does not undo the save or permit reusing its proof.
        m_capture.ClearSavedSamples();
        m_enrollmentSaveStatus = acknowledged ? "saved" : "reload_failed";
    }
    return acknowledged;
}

int EnrollmentWizard::GetFaceCount() { return m_accounts->GetFaceCount(); }
std::string EnrollmentWizard::GetFacesJson() { return m_accounts->GetFacesJson(); }
bool EnrollmentWizard::SaveEnrollmentAppend(const std::wstring& label) {
    return SaveEnrollmentImpl(L"", label, true);
}
bool EnrollmentWizard::DeleteFace(int faceId) { return m_accounts->DeleteFace(faceId); }
bool EnrollmentWizard::ClearAllFaces() { return m_accounts->ClearAllFaces(); }
bool EnrollmentWizard::WithdrawPrivacyConsent() {
    StopPreview();
    m_capture.ClearSavedSamples();
    return PrivacyWithdrawal().Run([this]() { return m_accounts->WithdrawConsent(); });
}
bool EnrollmentWizard::RenameFace(int faceId, const std::wstring& label) {
    return m_accounts->RenameFace(faceId, label);
}
std::string EnrollmentWizard::GetAccountSaveStatus() const { return m_accounts->GetSaveStatus(); }
std::string EnrollmentWizard::CheckAccountTypeChanged() { return m_accounts->CheckAccountTypeChanged(); }
bool EnrollmentWizard::RefreshAccountIdentity(const std::wstring& password) {
    return m_accounts->RefreshAccountIdentity(password);
}
bool EnrollmentWizard::ClearStaleAccountUpn() { return m_accounts->ClearStaleAccountUpn(); }
bool EnrollmentWizard::AutoRepairEmptyUpnOnStartup() { return m_accounts->AutoRepairEmptyUpnOnStartup(); }
bool EnrollmentWizard::AutoRepairMachineSidOnStartup() { return m_accounts->AutoRepairMachineSidOnStartup(); }

// ============================================================================
// Camera device enumeration
// ============================================================================

std::string EnrollmentWizard::GetCameraList() {
    auto devices = WebcamCaptureDS::ListCameras();
    std::ostringstream js;
    js << "[";
    for (size_t i = 0; i < devices.size(); i++) {
        if (i > 0) js << ",";
        // JSON-escape name/path (device paths contain backslashes).
        js << "{\"path\":\"" << JsonEscapeUtf8(devices[i].devicePath)
           << "\",\"name\":\"" << JsonEscapeUtf8(devices[i].friendlyName) << "\"}";
    }
    js << "]";
    return js.str();
}

// ============================================================================
// Configuration
// ============================================================================

std::string EnrollmentWizard::GetConfig() const {
    return ConfigToJson(m_config);
}

std::string EnrollmentWizard::GetLearningStatus() {
    return m_serviceClient.QueryLearningStatus();
}

bool EnrollmentWizard::BeginConfigSave(const std::string& json) {
    if (m_configSaveState.load() == ConfigSaveState::Pending) return false;
    if (m_configSaveThread.joinable()) m_configSaveThread.join();
    m_configSaveState.store(ConfigSaveState::SaveFailed);
    AppConfig newConfig = ConfigFromJson(json);
    // A running preview must already have the mandatory model.  Refuse to
    // mutate live settings if that invariant has somehow been broken.
    if (m_previewRunning && (!m_antiSpoof || !m_antiSpoof->IsInitialized())) {
        FACELOGIN_ERROR(L"BeginConfigSave: anti-spoof unavailable while preview is running");
        return false;
    }

    bool cameraChanged = (newConfig.camera_device != m_config.camera_device);

    if (!SaveConfig(m_dataDir, newConfig)) {
        FACELOGIN_ERROR(L"Failed to save config.json");
        return false;
    }
    {
        std::lock_guard<std::mutex> lock(m_configMutex);
        m_config = newConfig;
    }
    m_antiSpoofThreshold = newConfig.anti_spoof_threshold;

    FACELOGIN_INFO(L"Configuration updated: thr=%.2f antiSpoof=%.3f rotation=%d",
                  m_config.match_threshold, m_config.anti_spoof_threshold,
                  m_config.camera_rotation);

    // If the camera selection changed and the preview is running, restart the
    // preview so the new camera takes effect immediately.
    if (cameraChanged && m_previewRunning) {
        FACELOGIN_INFO(L"Camera selection changed — restarting preview");
        RestartPreview();
    }

    // Only the bounded pipe wait runs off-thread. In particular, keep camera
    // teardown/restart above on the UI thread's existing COM apartment.
    m_configSaveState.store(ConfigSaveState::Pending);
    m_configSaveThread = std::thread([this]() {
        const bool confirmed = m_serviceClient.ReloadConfig();
        m_configSaveState.store(confirmed ? ConfigSaveState::Saved
                                         : ConfigSaveState::ReloadFailed);
    });
    return true;
}

std::string EnrollmentWizard::GetConfigSaveStatus() const {
    switch (m_configSaveState.load()) {
    case ConfigSaveState::Pending: return "pending";
    case ConfigSaveState::Saved: return "saved";
    case ConfigSaveState::ReloadFailed: return "reload_failed";
    case ConfigSaveState::SaveFailed: return "save_failed";
    default: return "idle";
    }
}

bool EnrollmentWizard::RestartPreview() {
    StopPreview();
    return StartPreview();
}

// ============================================================================
// Log Viewer
// ============================================================================

std::string EnrollmentWizard::GetLogLines() {
    return ReadEnrollmentLog(m_dataDir, EnrollmentLogSource::Console);
}

std::string EnrollmentWizard::GetServiceLogLines() {
    return ReadEnrollmentLog(m_dataDir, EnrollmentLogSource::Service);
}

std::string EnrollmentWizard::GetAuthWorkerLogLines() {
    return ReadEnrollmentLog(m_dataDir, EnrollmentLogSource::AuthWorker);
}

std::string EnrollmentWizard::GetCredentialProviderLogLines() {
    return ReadEnrollmentLog(m_dataDir, EnrollmentLogSource::CredentialProvider);
}

bool EnrollmentWizard::ClearFileLog(const std::wstring& source) {
    if (source == L"service") return ClearEnrollmentLog(m_dataDir, EnrollmentLogSource::Service);
    if (source == L"worker") return ClearEnrollmentLog(m_dataDir, EnrollmentLogSource::AuthWorker);
    if (source == L"cp") return ClearEnrollmentLog(m_dataDir, EnrollmentLogSource::CredentialProvider);
    return false;
}

bool EnrollmentWizard::ClearLog() {
    return ClearEnrollmentLog(m_dataDir, EnrollmentLogSource::Console);
}

} // namespace facelogin
