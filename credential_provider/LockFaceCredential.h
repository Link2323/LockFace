#pragma once

#include <windows.h>
#include <credentialprovider.h>
#include <atomic>
#include <string>
#include <memory>
#include <deque>
#include <cstdint>
#include <wrl/client.h>

#include "pipe_client.h"
#include "auth_interaction_policy.h"
#include "input_trigger_policy.h"

// Forward declarations
class LockFaceProvider;

// ============================================================================
// LockFaceCredential — ICredentialProviderCredential implementation
//
// The actual credential tile shown on the lock screen. Handles:
//   1. Connecting to the face recognition service via named pipe
//   2. Waiting for face authentication result
//   3. Serializing native domain/local credentials or online credentials
//   4. Auto-logon two-pass pattern
//
// State machine:
//   Waiting        — Initial state, trying to establish pipe connection
//   Authenticating — Pipe connected, waiting for face recognition result
//   Ready          — Credentials received, ready to serialize
//   Failed         — Auth timed out or error
// ============================================================================

class LockFaceCredential : public ICredentialProviderCredential {
public:
#ifdef FACELOGIN_SERIALIZATION_TESTS
    friend struct CredentialSerializationTestAccess;
    friend struct CredentialConcurrencyTestAccess;
#endif
    LockFaceCredential();
    virtual ~LockFaceCredential();

    // Called by LockFaceProvider after creation
    void Initialize(LockFaceProvider* pProvider, CREDENTIAL_PROVIDER_USAGE_SCENARIO scenario = CPUS_LOGON);

    // Provider-level advise/unadvise (called by LockFaceProvider::Advise/UnAdvise)
    void AdviseProvider(ICredentialProviderEvents* pEvents, UINT_PTR upAdviseContext);
    void UnadviseProvider();

    // IUnknown
    STDMETHODIMP QueryInterface(REFIID riid, void** ppv) override;
    STDMETHODIMP_(ULONG) AddRef() override;
    STDMETHODIMP_(ULONG) Release() override;

    // ICredentialProviderCredential
    STDMETHODIMP Advise(ICredentialProviderCredentialEvents* pcpce) override;
    STDMETHODIMP_(HRESULT) UnAdvise() override;
    STDMETHODIMP SetSelected(BOOL* pbAutoLogon) override;
    STDMETHODIMP SetDeselected() override;
    STDMETHODIMP GetFieldState(DWORD dwFieldID,
                               CREDENTIAL_PROVIDER_FIELD_STATE* pcpfs,
                               CREDENTIAL_PROVIDER_FIELD_INTERACTIVE_STATE* pcpfis) override;
    STDMETHODIMP GetStringValue(DWORD dwFieldID, PWSTR* ppwsz) override;
    STDMETHODIMP GetBitmapValue(DWORD dwFieldID, HBITMAP* phbmp) override;
    STDMETHODIMP GetCheckboxValue(DWORD dwFieldID, BOOL* pbChecked, PWSTR* ppwszLabel) override;
    STDMETHODIMP GetSubmitButtonValue(DWORD dwFieldID, DWORD* pdwAdjacentTo) override;
    STDMETHODIMP GetComboBoxValueCount(DWORD dwFieldID, DWORD* pcItems, DWORD* pdwSelectedItem) override;
    STDMETHODIMP GetComboBoxValueAt(DWORD dwFieldID, DWORD dwItem, PWSTR* ppwszItem) override;
    STDMETHODIMP SetStringValue(DWORD dwFieldID, LPCWSTR pwz) override;
    STDMETHODIMP SetCheckboxValue(DWORD dwFieldID, BOOL bChecked) override;
    STDMETHODIMP SetComboBoxSelectedValue(DWORD dwFieldID, DWORD dwSelectedItem) override;
    STDMETHODIMP CommandLinkClicked(DWORD dwFieldID) override;
    STDMETHODIMP GetSerialization(
        CREDENTIAL_PROVIDER_GET_SERIALIZATION_RESPONSE* pcpgsr,
        CREDENTIAL_PROVIDER_CREDENTIAL_SERIALIZATION* pcpcs,
        PWSTR* ppwszOptionalStatusText,
        CREDENTIAL_PROVIDER_STATUS_ICON* pcpsiOptionalStatusIcon) override;
    STDMETHODIMP ReportResult(NTSTATUS ntsStatus, NTSTATUS ntsSubstatus,
                              PWSTR* ppwszOptionalStatusText,
                              CREDENTIAL_PROVIDER_STATUS_ICON* pcpsiOptionalStatusIcon) override;

private:
    using State = facelogin::credential_provider::AuthState;

    // Switch to the password credential provider (fallback)
    HRESULT SwitchToPasswordProvider();

    // Pack credentials into the serialization format. On success the packed
    // bytes are ALSO retained in m_packedCreds so GetSerialization can serve
    // LogonUI a fresh CoTaskMemAlloc copy on any later call (m_password is
    // zeroed on every return path — the cache, not the pipe, is what makes
    // repeated serialization possible).
    HRESULT PackCredentials(CREDENTIAL_PROVIDER_CREDENTIAL_SERIALIZATION* pcpcs);

    // Zero and release the retained packed credential bytes. Called whenever
    // the Ready handoff ends: ReportResult (submitted or rejected), terminal
    // failure presentation, and destruction.
    void ClearPackedCredentials();

    // Trigger re-enumeration of credentials (via CredentialsChanged)
    void TriggerReEnumeration();

    // Start the authentication pipeline (connect pipe + send AUTH_REQUEST).
    // Called on the owner STA after a queued input edge or explicit retry.
    void StartAuth();

    // Start / stop the background input-detection thread (LOGON + unlock).
    // The thread body is a static member (natural private access, no friend).
    static unsigned __stdcall InputDetectionThreadProc(void* pParam);
    void StartInputDetectionThread();
    void StopInputDetectionThread();

    // (Re)arm the input-detection watcher for the in-place failure tile: a
    // qualifying press (key or mouse button) then requests another face
    // round via StartExplicitRetry. Selects fresh-edge retry semantics
    // (Advise refuses to arm in failure state); no-op while running.
    void ArmFailureRetryDetection();

    // Apply queued pipe results on the owner STA
    void OnPipeResponse(bool success, const std::wstring& message);
    void OnPipeStatus(const std::wstring& message);

    // All auth state, strings, packed credentials, event interfaces and thread
    // handles belong to the creating STA. Workers only publish value messages
    // through m_cs; they never call COM or mutate the tile's state.
    enum class EventKind { Input, Status, Response, ConnectFailure, WaitingPrompt };
    struct PendingEvent {
        EventKind kind;
        std::uint64_t generation;
        bool success;
        std::wstring message;
        ~PendingEvent();
    };
    struct WorkerContext {
        LockFaceCredential* credential;
        HANDLE stop;
        std::uint64_t generation;
        facelogin::ipc::AuthDeadline deadline;
        facelogin::credential_provider::InputDetectionRound round;
        const wchar_t* pipeName = nullptr; // tests only; normal rounds use ipc::PIPE_NAME
    };
    bool EnsureDispatcher();
    static LRESULT CALLBACK DispatchWndProc(HWND, UINT, WPARAM, LPARAM);
    void QueueEvent(EventKind kind, std::uint64_t generation, bool success,
                    const std::wstring& message = {});
    void DispatchEvent();
    void StopBackgroundWork();
    void StopAuthThread();
    static unsigned __stdcall AuthThreadProc(void* context);
    void NotifyStatusText();
    void NotifyTileFields(bool failure, bool showRetryHint);
    void AssertOwner() const;

    void SetStatusText(const std::wstring& text);
    std::wstring SnapshotStatusText();

    // Present a terminal, retryable failure in-place on the selected tile
    // (no re-enumeration — LogonUI must not disturb password entry on other
    // tiles), then re-arm the passive watcher so a qualifying press can
    // request another round (ArmFailureRetryDetection).
    void PresentRetryableFailure(State failureState,
                                 const std::wstring& statusText, bool armRetry = true);
    void StartExplicitRetry();

    LONG m_refCount = 1;
    Microsoft::WRL::ComPtr<ICredentialProviderCredentialEvents> m_pCredentialEvents;
    Microsoft::WRL::ComPtr<ICredentialProviderEvents> m_pProviderEvents;
    UINT_PTR m_upAdviseContext = 0;

    State m_state = State::Waiting;
#ifdef FACELOGIN_CONCURRENCY_TESTS
    std::function<void(int)> m_testBarrier;
    std::wstring m_testPipeName;
#endif
    const DWORD m_ownerThread = GetCurrentThreadId();
    HWND m_dispatchWindow = nullptr;
    HINSTANCE m_dispatchInstance = nullptr;
    wchar_t m_dispatchClass[80]{};
    std::deque<std::unique_ptr<PendingEvent>> m_pendingEvents;
    bool m_acceptEvents = false; // protected by m_cs
    std::uint64_t m_generation = 0; // owner only; copied into WorkerContext
    bool m_selected = false;
    HANDLE m_hAuthThread = nullptr;
    HANDLE m_hAuthStop = nullptr;

    // Received credentials (zeroed after serialization)
    CREDENTIAL_PROVIDER_USAGE_SCENARIO m_usageScenario = CPUS_LOGON;
    std::wstring m_sid;
    std::wstring m_upn;
    std::wstring m_domain;
    std::wstring m_username;
    std::wstring m_password;

    // Status text applied on the owner STA from queued service messages
    std::wstring m_statusText;

    // Auth timeout tracking (so we don't block LogonUI forever)
    std::atomic<facelogin::ipc::AuthDeadline> m_authDeadline{};

    // Selects the edge-policy semantics for the watcher. The initial lock
    // round accepts an ordinary orphan BREAK because the wallpaper may have
    // swallowed its MAKE; a visible failure tile requires a fresh MAKE and
    // seeds already-held inputs before watching. Written before thread start.
    facelogin::credential_provider::InputDetectionRound m_inputDetectionRound =
        facelogin::credential_provider::InputDetectionRound::InitialLock;
    HANDLE m_hInputThread = nullptr;   // background input-detection thread
    // Manual-reset: within one round several waits poll this event
    // (the watcher loop and the mouse-settle window), and no wait may
    // consume the signal the others still need to observe. A fresh event is
    // created for every round, so it never needs resetting.
    HANDLE m_hInputStop = nullptr;     // event: signal to stop the thread
    // Set before the thread starts, cleared by the thread itself on exit and
    // by StopInputDetectionThread after the join — atomic because it is the
    // handshake between those two threads (never a torn "running" read).
    std::atomic<bool> m_inputThreadRunning = false;

    // Retained packed credential (CredPackAuthenticationBufferW output —
    // contains the plaintext password). Owned with new[]/SecureZeroMemory.
    // Lives only while a Ready handoff is pending: created by the first
    // PackCredentials, cleared by ClearPackedCredentials.
    BYTE* m_packedCreds = nullptr;
    DWORD m_cbPackedCreds = 0;
    ULONG m_ulAuthPackage = 0;

    // Protects only the worker-to-STA queue and its acceptance gate.
    CRITICAL_SECTION m_cs;
};
