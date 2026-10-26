#pragma once

#include "session_identity.h"
#include "service_client.h"
#include "enrollment_capture.h"

#include <windows.h>
#include <vector>
#include <string>
#include <memory>
#include <thread>
#include <mutex>
#include <atomic>
#include <chrono>
#include <cstdint>

#include "../common/frame_image.h"
#include "../vision/face_align.h"
#include "../vision/liveness_types.h"
#include "../vision/onnx_models.h"
#include "../vision/webcam_capture_dshow.h"
#include "../common/config_util.h"

namespace facelogin {

class PreviewExposure;
class PreviewFrameEncoder;
class EnrollmentAccounts;

// Backend controller for face enrollment.
// All UI is handled by WebView2 + HTML; this class provides data & actions
// to JavaScript via a COM host object.
class EnrollmentWizard {
public:
    EnrollmentWizard();
    ~EnrollmentWizard();

    // === Called from JS via host object ===

    bool StartPreview();
    void StopPreview();
    std::string GetUsername() const;
    std::string GetUserSid() const;
    std::string GetAccountType() const { return m_identity.accountType; }
    // Asynchronous multi-angle capture for one head-yaw position:
    //   0 = front (0°), 1 = left (+30°), 2 = right (−30°).
    // Each call collects 5 yaw-gated frames (|yaw − target| ≤ 10°, face ≥ 60px,
    // detection score ≥ 0.5); JS drives 0 → 1 → 2 in sequence. Every angle
    // runs the liveness check first. Samples accumulate inside the capture
    // session, grouped by angle, so SaveEnrollment can create one
    // face record per angle without ever averaging across angles.
    bool CaptureFaceSamples(int angleIndex);
    // Cooperative cancellation; completion is reported after the worker exits.
    void CancelCapture();
    // Capture progress for the JS UI:
    // {"angle":0,"label":"正面","targetYaw":0,"collected":3,"target":5,
    //  "yaw":12.3,"total":8,"livenessChecking":false,"livenessPassed":true,
    //  "cancelled":false,"done":false,"failureReason":""}
    std::string GetCaptureStatus();
    bool ValidatePassword(const std::wstring& password);
    // Save enrollment with the given password. label names this face (may be
    // empty — the backend falls back to L"脸N").
    bool SaveEnrollment(const std::wstring& password, const std::wstring& label = L"");

    // Multi-face management (1.3.0). The current account can enroll several
    // faces; each save appends one face instead of replacing the old one.
    // Number of faces enrolled for the current account (0 = not enrolled).
    int GetFaceCount();
    // JSON list of the current account's faces: [{"id":1,"label":"脸1"},...]
    std::string GetFacesJson();
    // Append a new face for the current account without re-entering a password
    // (identity proven by the session token SID). label may be empty.
    bool SaveEnrollmentAppend(const std::wstring& label = L"");
    // UI-thread result of the most recent enrollment save (including append).
    std::string GetEnrollmentSaveStatus() const { return m_enrollmentSaveStatus; }
    // Delete one face of the current account (removes the account if it was
    // the last face). Returns false on unknown id / not enrolled.
    bool DeleteFace(int faceId);
    // Remove all faces of the current account (= remove the account).
    bool ClearAllFaces();
    // Stops capture, discards unsaved proof, then removes this account via service.
    bool WithdrawPrivacyConsent();
    // Rename one face of the current account.
    bool RenameFace(int faceId, const std::wstring& label);
    std::string GetAccountSaveStatus() const;

    // Account-type change detection (symmetric MSA ↔ local conversions).
    // Windows keeps the same SID when a user converts their account between a
    // Microsoft account (MSA) and a local account, so the stored record is
    // matched by SID but may carry stale identity/password from the previous
    // account type:
    //   MSA→local:  record UPN still an MSA email, password is the old MSA one.
    //   local→MSA:  record UPN empty (local-era), password is the old local one.
    // JSON for JS: {"state":0} | {"state":1,"faces":N} |
    // {"state":2,"faces":N,"upn":"user@mail.com"}, where state 0 = normal /
    // nothing to refresh, 1 = stale MSA→local record detected, 2 = stale
    // local→MSA record detected (current session is an MSA but the record
    // UPN is empty or differs from the current email).
    std::string CheckAccountTypeChanged();
    // Validate the CURRENT password, then rewrite the stale record IN PLACE
    // (preserving all faces): state 1 clears the old MSA UPN (local account),
    // state 2 writes the current MSA email; both refresh username/SID and
    // re-encrypt the password via DPAPI. Refuses (returns false) unless a
    // stale record was detected (CheckAccountTypeChanged state != 0) and the
    // password validates.
    bool RefreshAccountIdentity(const std::wstring& password);

    // Light-weight dismiss path for the stale-account prompt. State 1 only:
    // clears the bogus MSA email from the current local account's record
    // WITHOUT validating or re-encrypting the password (no input needed).
    // Faces and the stored password are preserved; the lock-screen credential
    // then packs with domain\username instead of the misattributed email.
    // Returns false unless the record is in state 1.
    bool ClearStaleAccountUpn();

    // One-shot startup repair: if the current session is an MSA (per
    // ResolveSessionUpn) but the stored record for THIS session's SID still
    // has an empty UPN, write the resolved MSA email in place — preserving
    // username, SID, faces and the stored (already DPAPI-encrypted) password.
    //
    // Scope is deliberately narrow: only the current session user's own
    // record, and only when its UPN is empty. A non-empty but DIFFERENT UPN
    // may be a genuine re-binding (a user who switched Microsoft accounts) and
    // is left to RefreshAccountIdentity, which requires a password check.
    //
    // Without this, accounts enrolled while GetUserNameExW returned 1332
    // (no UPN) carry an empty UPN forever: the lock-screen credential then
    // packs with domain\username instead of routing through CloudAP, and
    // the account-type check keeps flagging a phantom local→MSA conversion.
    // Returns true iff a record was actually modified.
    bool AutoRepairEmptyUpnOnStartup();

    // Repair records whose SID is not a user account SID — the machine SID a
    // pre-fix build resolved on machines where computer name == user name
    // (upstream #24). Rewrites them with the authoritative session token SID
    // so appends/re-enrollments/identity refreshes stop being refused. Same
    // rules as AutoRepairEmptyUpnOnStartup: only the current session user's
    // own record, best-effort, never fatal.
    bool AutoRepairMachineSidOnStartup();

    // Configuration
    std::string GetConfig() const;
    // Apply local settings on the UI thread; wait for service ACK off-thread.
    // True means accepted. Poll GetConfigSaveStatus for the final outcome.
    bool BeginConfigSave(const std::string& json);
    std::string GetConfigSaveStatus() const;
    // LEARNING_STATUS pipe query → compact JSON ({"error":...} on failure).
    // Read-only data for the learning-observability view.
    std::string GetLearningStatus();
    bool RestartPreview();

    // Camera device enumeration for the settings UI.
    // Returns JSON: [{path, name}, ...] — path is the stable symbolic link,
    // name is the friendly display name.
    std::string GetCameraList();

    // Log viewer
    std::string GetLogLines();
    std::string GetServiceLogLines();
    std::string GetAuthWorkerLogLines();
    std::string GetCredentialProviderLogLines();
    bool ClearLog();
    bool ClearFileLog(const std::wstring& source);

    // Returns empty if knownVersion matches, otherwise an atomic snapshot:
    // "<version>\x1E<frame base64>\x1E<faces json>". Empty version forces a read.
    std::string GetLatestFrameAndFaces(const std::string& knownVersion);

    bool IsPreviewRunning() const { return m_previewRunning; }

    // Window focus/minimize state pushed by WebviewHost (WM_ACTIVATE /
    // WM_SIZE). Unfocused (window visible but not in the foreground): the
    // preview keeps the camera rate, only detection pins to the no-face
    // tier. Minimized: the whole frame pipeline drops to the background
    // cadence — nobody can see it at all. Safe from any thread; may be
    // called before StartPreview.
    void SetUnfocused(bool unfocused) {
        m_unfocused.store(unfocused, std::memory_order_relaxed);
    }
    void SetMinimized(bool minimized) {
        m_minimized.store(minimized, std::memory_order_relaxed);
    }
    // True while a capture (liveness check or sampling) is in flight. The
    // host checks it before releasing the camera on minimize: a capture must
    // not be killed by the window going to the taskbar.
    bool IsCaptureRunning() const { return m_capture.IsRunning(); }
    // Canvas backing size (physical px) for preview JPEG encoding, pushed by
    // js/preview.js whenever layout or DPI scale changes. The frame thread
    // scales frames to fit this before encoding; until the first report (or
    // for a zero size) frames encode at full camera resolution.
    void SetPreviewSize(int width, int height);

private:
    std::string FacesToJson(const std::vector<facelogin::FaceWithKps>& faces);

    std::unique_ptr<PreviewExposure> m_previewExposure;

    bool SaveEnrollmentImpl(const std::wstring& password,
                            const std::wstring& label, bool append = false);
    std::string m_enrollmentSaveStatus = "failed";

    enum class ConfigSaveState { Idle, Pending, Saved, ReloadFailed, SaveFailed };
    std::atomic<ConfigSaveState> m_configSaveState{ConfigSaveState::Idle};
    std::thread m_configSaveThread;

    // Camera & face processing
    std::unique_ptr<WebcamCaptureDS>  m_webcam;
    std::unique_ptr<OnnxDetector>   m_onnxDetector;   // SCRFD detection (+5 keypoints)
    std::unique_ptr<OnnxRecognizer> m_onnxRecognizer; // SFace recognition
    std::unique_ptr<OnnxAntiSpoof>  m_antiSpoof;
    std::unique_ptr<EnrollmentAccounts> m_accounts;

    // Configuration. m_configMutex serializes the frame thread's per-frame
    // snapshot against BeginConfigSave's whole-struct replacement on the UI thread
    // (rotation applies live; only a camera change restarts the preview).
    // The UI thread is the sole writer, so its own reads need no lock.
    AppConfig m_config;
    std::mutex m_configMutex;
    float m_antiSpoofThreshold = 0.281f;

    // Frame-grab thread (runs off UI thread — GrabFrame + JPEG encode + detection)
    std::thread m_frameThread;
    std::atomic<bool> m_frameRunning{false};
    // Window state pushed by WebviewHost (see SetUnfocused / SetMinimized);
    // read per-iteration by the frame thread. Minimized normally releases the
    // camera outright (WebviewHost WM_SIZE); the flag still drives the frame
    // thread's low cadence for the window between minimize and camera stop,
    // and for a capture that kept the camera alive across a minimize.
    std::atomic<bool> m_unfocused{false};
    std::atomic<bool> m_minimized{false};
    // Packed preview-encode target (width << 16 | height), written by the JS
    // bridge on the COM thread and read per-frame by the frame thread. 0 =
    // not reported — encode at full camera resolution.
    std::atomic<std::uint32_t> m_previewEncodeSize{0};

    std::unique_ptr<PreviewFrameEncoder> m_frameEncoder;

    // Per-frame caches (produced by frame thread; the images are consumed by
    // JS on the UI thread, the frame itself by the capture thread as an
    // immutable shared borrow — no pixel copies on either side of the lock).
    std::mutex  m_frameCacheMutex;
    std::string m_latestFrameB64;
    std::string m_latestFacesJson;
    std::uint64_t m_previewVersion = 0;  // guarded by m_frameCacheMutex; survives restarts
    std::shared_ptr<const FrameImage> m_latestFrame;   // capture reads via borrow
    std::uint64_t m_latestFrameSequence = 0;           // real camera sequence; guarded by m_frameCacheMutex

    // Preview state
    bool m_previewRunning = false;

    EnrollmentCapture m_capture;

    session_identity::SessionIdentity m_identity;
    EnrollmentServiceClient m_serviceClient;
    std::wstring m_dataDir;
};

} // namespace facelogin
