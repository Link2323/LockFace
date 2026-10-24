#include "LockFaceCredential.h"
#include "LockFaceProvider.h"
#include "dll_lifetime.h"
#include "input_trigger_policy.h"
#include "resource.h"
#include "native_logon.h"
#include "../common/account_identity.h"
#include "../common/logger.h"
#include "../common/ipc_protocol.h"
#include "../common/secure_clear.h"
#include <wincred.h>
// winnt.h (via windows.h above) and ntstatus.h both define the STATUS_*
// constants with identical values; silence the benign C4005 redefinitions.
#pragma warning(push)
#pragma warning(disable : 4005)
#include <ntstatus.h>
#pragma warning(pop)
#include <ntsecapi.h>
#include <shlwapi.h>
#include <process.h>
#include <cassert>

#pragma comment(lib, "credui.lib")
#pragma comment(lib, "ntdll.lib")

// ============================================================================
// Input-detection thread (LOGON + unlock scenarios)
// ============================================================================
//
// Input intent is edge-based, not time-based. Raw Input supplies keyboard
// MAKE/BREAK events; physical mouse-button states are sampled because the
// lock-screen wallpaper swallows its first click. Mouse movement never enters
// the policy and therefore cannot trigger recognition. The pure policy also
// drains Win+L residue without assuming any keyboard repeat delay/rate.
//
// Secure-desktop observations (physical Windows 11 testing, 2026-08-29):
// Raw keyboard BREAK reaches this sink during wallpaper dismissal even when
// MAKE is swallowed; raw mouse DOWN/UP does not. Consequently one keyboard
// press can trigger, while the first mouse click dismisses the wallpaper and
// the second click (visible through GetAsyncKeyState on the credential view)
// triggers recognition.

static thread_local facelogin::credential_provider::InputTriggerPolicy*
    t_inputTriggerPolicy = nullptr;

static LRESULT CALLBACK RawInputSinkWndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_INPUT) {
        RAWINPUT raw = {};
        UINT size = sizeof(raw);
        if (GetRawInputData(reinterpret_cast<HRAWINPUT>(lp), RID_INPUT,
                            &raw, &size, sizeof(RAWINPUTHEADER)) != static_cast<UINT>(-1) &&
            raw.header.dwType == RIM_TYPEKEYBOARD) {
            const RAWKEYBOARD& kb = raw.data.keyboard;
            // 0xFF is the documented fake-key marker and must be ignored.
            if (t_inputTriggerPolicy && kb.VKey != 0 && kb.VKey != 0xFF) {
                t_inputTriggerPolicy->ObserveKey(
                    kb.VKey, (kb.Flags & RI_KEY_BREAK) == 0);
            }
        }
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

struct RawInputSink {
    HWND hwnd = nullptr;
    ATOM classAtom = 0;
    HINSTANCE hInst = nullptr;
    wchar_t className[64] = {};

    bool Register();
    void Unregister() noexcept;
};

bool RawInputSink::Register() {
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                           GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       reinterpret_cast<LPCWSTR>(&RawInputSinkWndProc), &hInst);
    wsprintfW(className, L"LockFaceRawSink_%08lx", GetCurrentThreadId());

    WNDCLASSW wc = {};
    wc.lpfnWndProc = RawInputSinkWndProc;
    wc.hInstance = hInst;
    wc.lpszClassName = className;
    classAtom = RegisterClassW(&wc);
    if (!classAtom) {
        FACELOGIN_WARN(L"[InputThread] raw-input class registration failed (GLE=%lu)",
                       GetLastError());
        return false;
    }

    // HWND_MESSAGE parent: invisible message-only window, exactly what a
    // RIDEV_INPUTSINK registration needs as its delivery address.
    hwnd = CreateWindowExW(0, className, L"", 0, 0, 0, 0, 0,
                           HWND_MESSAGE, nullptr, hInst, nullptr);
    if (!hwnd) {
        FACELOGIN_WARN(L"[InputThread] raw-input sink window creation failed (GLE=%lu)",
                       GetLastError());
        Unregister();
        return false;
    }

    RAWINPUTDEVICE rid = {};
    rid.usUsagePage = 0x01;         // generic desktop
    rid.usUsage = 0x06;             // keyboard
    rid.dwFlags = RIDEV_INPUTSINK;  // deliver even without foreground
    rid.hwndTarget = hwnd;
    if (!RegisterRawInputDevices(&rid, 1, sizeof(rid))) {
        FACELOGIN_WARN(L"[InputThread] RegisterRawInputDevices failed (GLE=%lu) — "
                       L"key presses degrade to credential-view-only detection",
                       GetLastError());
        Unregister();
        return false;
    }
    return true;
}

void RawInputSink::Unregister() noexcept {
    if (hwnd) {
        RAWINPUTDEVICE rid = {};
        rid.usUsagePage = 0x01;
        rid.usUsage = 0x06;
        rid.dwFlags = RIDEV_REMOVE;  // requires hwndTarget == nullptr
        rid.hwndTarget = nullptr;
        RegisterRawInputDevices(&rid, 1, sizeof(rid));
        DestroyWindow(hwnd);
        hwnd = nullptr;
    }
    if (classAtom) {
        UnregisterClassW(className, hInst);
        classAtom = 0;
    }
}

unsigned __stdcall LockFaceCredential::InputDetectionThreadProc(void* pParam) {
    std::unique_ptr<WorkerContext> context(static_cast<WorkerContext*>(pParam));
    LockFaceCredential* pCred = context->credential;
    using facelogin::credential_provider::InputTriggerPolicy;
    using facelogin::credential_provider::InputTriggerKind;

    constexpr DWORD kPollIntervalMs = 10;
    constexpr DWORD kMouseSelectionSettleMs = 100;
    constexpr int kFirstVk = 0x01;
    constexpr int kLastVk = 0xFE;
    constexpr int kMouseButtons[] = {
        VK_LBUTTON, VK_RBUTTON, VK_MBUTTON, VK_XBUTTON1, VK_XBUTTON2
    };

    auto isMouseButton = [&](int vk) noexcept {
        for (int button : kMouseButtons) {
            if (vk == button) return true;
        }
        return false;
    };

    InputTriggerPolicy policy(context->round);
    const bool failureRetryRound =
        context->round ==
        facelogin::credential_provider::InputDetectionRound::FailureRetry;
    t_inputTriggerPolicy = &policy;

    RawInputSink rawSink;
    const bool rawKeyboardAvailable = rawSink.Register();

    std::array<bool, 256> sampledKeyDown{};
    std::array<bool, 256> sampledMouseDown{};

    // Seed inputs already held at watcher start. Mouse buttons are always
    // conservative because the first click belongs to the wallpaper. Keyboard
    // keys are seeded only for a visible failure tile; on the initial lock
    // round an already-started ordinary press must still trigger on BREAK,
    // while the policy independently drains the ambiguous L/Win tail.
    for (int vk = kFirstVk; vk <= kLastVk; ++vk) {
        const bool down = (GetAsyncKeyState(vk) & 0x8000) != 0;
        if (isMouseButton(vk)) {
            sampledMouseDown[vk] = down;
            if (down) policy.SeedMouseButtonDown(static_cast<std::uint16_t>(vk));
        } else if (!rawKeyboardAvailable) {
            sampledKeyDown[vk] = down;
            if (down && failureRetryRound) {
                policy.SeedKeyDown(static_cast<std::uint16_t>(vk));
            }
        } else if (down && failureRetryRound) {
            policy.SeedKeyDown(static_cast<std::uint16_t>(vk));
        }
    }

    bool shouldTrigger = false;
    InputTriggerKind triggerKind = InputTriggerKind::None;
    while (WaitForSingleObject(context->stop, 0) != WAIT_OBJECT_0) {
        MSG msg = {};
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            DispatchMessageW(&msg);
        }

        // Mouse movement is deliberately absent. Only physical button state
        // transitions reach the policy, so brushing the mouse cannot open the
        // camera. The wallpaper consumes the first click; the second is seen
        // here after the credential view becomes active.
        for (int vk : kMouseButtons) {
            const bool down = (GetAsyncKeyState(vk) & 0x8000) != 0;
            if (down != sampledMouseDown[vk]) {
                sampledMouseDown[vk] = down;
                policy.ObserveMouseButton(static_cast<std::uint16_t>(vk), down);
            }
        }

        // If Raw Input registration fails, retain a credential-view-only
        // fallback using physical keyboard edges. It cannot see the wallpaper
        // keypress, but it still avoids movement and held-key false triggers.
        if (!rawKeyboardAvailable) {
            for (int vk = kFirstVk; vk <= kLastVk; ++vk) {
                if (isMouseButton(vk)) continue;
                const bool down = (GetAsyncKeyState(vk) & 0x8000) != 0;
                if (down != sampledKeyDown[vk]) {
                    sampledKeyDown[vk] = down;
                    policy.ObserveKey(static_cast<std::uint16_t>(vk), down);
                }
            }
        }

        if (policy.triggered()) {
            shouldTrigger = true;
            triggerKind = policy.triggerKind();
            break;
        }

        DWORD wait = MsgWaitForMultipleObjectsEx(
            1, &context->stop, kPollIntervalMs, QS_ALLINPUT,
            MWMO_ALERTABLE | MWMO_INPUTAVAILABLE);
        if (wait == WAIT_OBJECT_0) {
            break;
        }
    }

    t_inputTriggerPolicy = nullptr;
    rawSink.Unregister();  // idempotent no-op if registration failed

    // A mouse click can target another sign-in option. LogonUI dispatches the
    // tile switch at the end of that click and calls SetDeselected, which
    // signals m_hInputStop. Give that UI transition one short cancellable
    // window before opening the authentication pipe. Keyboard presses retain
    // their immediate path.
    if (shouldTrigger && triggerKind == InputTriggerKind::MouseButton &&
        WaitForSingleObject(context->stop, kMouseSelectionSettleMs) ==
            WAIT_OBJECT_0) {
        shouldTrigger = false;
    }

    // Deselect/UnAdvise wins a race with the qualifying edge: once the stop
    // event is signaled this watcher must not open a new pipe while its owner
    // is joining the thread and tearing the selected tile down.
    if (shouldTrigger &&
        WaitForSingleObject(context->stop, 0) != WAIT_OBJECT_0) {
        pCred->QueueEvent(EventKind::Input, context->generation, true);
    }

    pCred->m_inputThreadRunning = false;
    pCred->Release();
    return 0;
}

// Idle prompt shown on the Waiting tile — one constant so the deselect reset
// and GetStringValue can never drift apart.
static const wchar_t kWaitingPrompt[] = L"按下任意按键以开始人脸识别";

// ============================================================================
// Construction / Destruction
// ============================================================================

LockFaceCredential::LockFaceCredential() {
    LockFaceDllAddRef();
    InitializeCriticalSection(&m_cs);

    FACELOGIN_DEBUG(L"LockFaceCredential created");
}

LockFaceCredential::~LockFaceCredential() {
    AssertOwner();
    StopBackgroundWork();
    m_pCredentialEvents.Reset();
    m_pProviderEvents.Reset();
    facelogin::SecureClearWString(m_password);
    ClearPackedCredentials();
    if (m_dispatchWindow) DestroyWindow(m_dispatchWindow);
    if (m_dispatchClass[0]) UnregisterClassW(m_dispatchClass, m_dispatchInstance);
    DeleteCriticalSection(&m_cs);

    FACELOGIN_DEBUG(L"LockFaceCredential destroyed");
    LockFaceDllRelease();
}

void LockFaceCredential::Initialize(LockFaceProvider* pProvider, CREDENTIAL_PROVIDER_USAGE_SCENARIO scenario) {
    m_usageScenario = scenario;
    UNREFERENCED_PARAMETER(pProvider);
    AssertOwner();
    FACELOGIN_DEBUG(L"LockFaceCredential initialized with provider");
}

void LockFaceCredential::AdviseProvider(ICredentialProviderEvents* pEvents, UINT_PTR upAdviseContext) {
    AssertOwner();
    m_pProviderEvents = pEvents;
    m_upAdviseContext = upAdviseContext;
}

void LockFaceCredential::UnadviseProvider() {
    AssertOwner();
    StopBackgroundWork();
    m_pProviderEvents.Reset();
    m_upAdviseContext = 0;
}

// Worker messages contain values, never COM pointers. Posting is nonblocking,
// so joining a worker on the STA cannot wait for an STA COM call to finish.
namespace { constexpr UINT kDispatchMessage = WM_APP + 0x431; }

LockFaceCredential::PendingEvent::~PendingEvent() {
    facelogin::SecureClearWString(message);
}

void LockFaceCredential::AssertOwner() const {
    assert(GetCurrentThreadId() == m_ownerThread);
}

bool LockFaceCredential::EnsureDispatcher() {
    AssertOwner();
    if (m_dispatchWindow) return true;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                      GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                      reinterpret_cast<LPCWSTR>(&DispatchWndProc), &m_dispatchInstance);
    swprintf_s(m_dispatchClass, L"LockFaceDispatch_%p", static_cast<void*>(this));
    WNDCLASSW wc{};
    wc.lpfnWndProc = DispatchWndProc;
    wc.hInstance = m_dispatchInstance;
    wc.lpszClassName = m_dispatchClass;
    if (!RegisterClassW(&wc)) return false;
    m_dispatchWindow = CreateWindowExW(0, m_dispatchClass, L"", 0,
        0, 0, 0, 0, HWND_MESSAGE, nullptr, m_dispatchInstance, this);
    if (!m_dispatchWindow) {
        const auto error = GetLastError();
        UnregisterClassW(m_dispatchClass, m_dispatchInstance);
        SetLastError(error);
        return false;
    }
    return true;
}

LRESULT CALLBACK LockFaceCredential::DispatchWndProc(HWND window, UINT message,
                                                     WPARAM wp, LPARAM lp) {
    if (message == WM_NCCREATE) {
        auto create = reinterpret_cast<CREATESTRUCTW*>(lp);
        SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(create->lpCreateParams));
    }
    auto credential = reinterpret_cast<LockFaceCredential*>(GetWindowLongPtrW(window, GWLP_USERDATA));
    if (message == kDispatchMessage && credential) {
        // CredentialsChanged/SetFieldString may synchronously release every
        // external reference. Keep this dispatch alive through that reentry.
        credential->AddRef();
        credential->DispatchEvent();
        credential->Release();
        return 0;
    }
    if (message == WM_NCDESTROY) SetWindowLongPtrW(window, GWLP_USERDATA, 0);
    return DefWindowProcW(window, message, wp, lp);
}

void LockFaceCredential::QueueEvent(EventKind kind, std::uint64_t generation,
                                    bool success, const std::wstring& message) {
#ifdef FACELOGIN_CONCURRENCY_TESTS
    if (m_testBarrier) m_testBarrier(0);
#endif
    auto event = std::make_unique<PendingEvent>();
    event->kind = kind;
    event->generation = generation;
    event->success = success;
    event->message = message;
    EnterCriticalSection(&m_cs);
    if (m_acceptEvents) {
        m_pendingEvents.push_back(std::move(event));
        if (!PostMessageW(m_dispatchWindow, kDispatchMessage, 0, 0)) {
            m_pendingEvents.pop_back(); // wipes sensitive message on failure
            FACELOGIN_ERROR(L"Failed to post credential update: %lu", GetLastError());
        }
    }
    LeaveCriticalSection(&m_cs);
#ifdef FACELOGIN_CONCURRENCY_TESTS
    if (m_testBarrier) m_testBarrier(1);
#endif
}

void LockFaceCredential::DispatchEvent() {
    AssertOwner();
    std::unique_ptr<PendingEvent> event;
    EnterCriticalSection(&m_cs);
    if (m_acceptEvents && !m_pendingEvents.empty()) {
        event = std::move(m_pendingEvents.front());
        m_pendingEvents.pop_front();
    }
    LeaveCriticalSection(&m_cs);
    if (!event || event->generation != m_generation) return;
    switch (event->kind) {
    case EventKind::Input:
        StopInputDetectionThread();
        if (!m_selected) return;
        if (facelogin::credential_provider::IsRetryableFailure(m_state)) StartExplicitRetry();
        else if (m_state == State::Waiting) StartAuth();
        break;
    case EventKind::Status:
        OnPipeStatus(event->message);
        break;
    case EventKind::Response:
        StopAuthThread();
        OnPipeResponse(event->success, event->message);
        break;
    case EventKind::ConnectFailure:
        if (m_state == State::Authenticating) {
            PresentRetryableFailure(event->success ? State::Failed : State::Error,
                event->success ? L"识别超时，请重试" : L"人脸登录服务不可用", false);
        }
        break;
    }
}

void LockFaceCredential::StopBackgroundWork() {
    AssertOwner();
#ifdef FACELOGIN_CONCURRENCY_TESTS
    if (m_testBarrier) m_testBarrier(2);
#endif
    EnterCriticalSection(&m_cs);
    m_acceptEvents = false;
    LeaveCriticalSection(&m_cs);
#ifdef FACELOGIN_CONCURRENCY_TESTS
    if (m_testBarrier) m_testBarrier(3);
#endif
    m_selected = false;
    ++m_generation;
    StopInputDetectionThread();
    StopAuthThread();
    EnterCriticalSection(&m_cs);
    m_pendingEvents.clear();
    LeaveCriticalSection(&m_cs);
    // Preserve a completed Ready handoff across success re-enumeration.
    if (m_state != State::Ready) {
        facelogin::SecureClearWString(m_password);
        ClearPackedCredentials();
        if (m_state == State::Authenticating) m_state = State::Waiting;
    }
    m_authDeadline.store({});
}

void LockFaceCredential::NotifyStatusText() {
    AssertOwner();
    auto events = m_pCredentialEvents;
    const auto text = m_statusText;
    if (events) events->SetFieldString(this, kFieldStatus, text.c_str());
}

void LockFaceCredential::NotifyTileFields(bool failure, bool showRetryHint) {
    AssertOwner();
    auto events = m_pCredentialEvents;
    const auto generation = m_generation;
    if (!events) return;
    events->SetFieldState(this, kFieldSwitchLink,
        failure ? CPFS_HIDDEN : CPFS_DISPLAY_IN_DESELECTED_TILE);
    if (generation != m_generation || events.Get() != m_pCredentialEvents.Get()) return;
    events->SetFieldState(this, kFieldRetryHint,
        showRetryHint ? CPFS_DISPLAY_IN_SELECTED_TILE : CPFS_HIDDEN);
}

// ============================================================================
// IUnknown
// ============================================================================

STDMETHODIMP LockFaceCredential::QueryInterface(REFIID riid, void** ppv) {
    *ppv = nullptr;

    if (riid == IID_IUnknown ||
        riid == IID_ICredentialProviderCredential) {
        *ppv = static_cast<ICredentialProviderCredential*>(this);
        AddRef();
        return S_OK;
    }

    return E_NOINTERFACE;
}

STDMETHODIMP_(ULONG) LockFaceCredential::AddRef() {
    return InterlockedIncrement(&m_refCount);
}

STDMETHODIMP_(ULONG) LockFaceCredential::Release() {
    LONG count = InterlockedDecrement(&m_refCount);
    if (count == 0) {
        delete this;
    }
    return count;
}

// ============================================================================
// ICredentialProviderCredential — Advise/UnAdvise
// ============================================================================

STDMETHODIMP LockFaceCredential::Advise(ICredentialProviderCredentialEvents* pcpce) {
    AssertOwner();
    if (!EnsureDispatcher()) return HRESULT_FROM_WIN32(GetLastError());
    m_pCredentialEvents = pcpce;

    // Guard: if we already have credentials ready from a previous
    // auth round, don't restart the flow.  This prevents an infinite
    // loop where OnPipeResponse → CredentialsChanged → Advise()
    // overwrites Ready back to Authenticating.
    if (m_state == State::Ready && (!m_password.empty() || m_packedCreds)) {
        FACELOGIN_INFO(L"Advise: credentials already ready, skipping auth restart");
        return S_OK;
    }

    // A terminal failure must NOT (re)start the watcher from Advise:
    // LogonUI calls Advise for every enumeration — including flows that
    // never select this tile (PIN reset wizard) and moments when another
    // tile is focused — and the watcher polls GLOBAL input, so starting it
    // here would interpret password-entry keystrokes elsewhere as face
    // retries. The failure watcher is armed only from
    // PresentRetryableFailure/SetSelected, i.e. strictly while this tile is
    // the selected one. ArmFailureRetryDetection selects retry-round edge
    // semantics itself.
    if (facelogin::credential_provider::IsRetryableFailure(m_state)) {
        FACELOGIN_INFO(L"Advise: terminal failure — retry watcher NOT started "
                       L"here (armed tile-scoped instead)");
        return S_OK;
    }

    // Guard: if we're already authenticating and have a live pipe,
    // don't create a second connection.
    if (m_state == State::Authenticating && m_hAuthThread) {
        FACELOGIN_INFO(L"Advise: already authenticating, skipping auth restart");
        return S_OK;
    }

    // Activation lives in SetSelected, NOT here: LogonUI calls Advise for
    // every credential enumeration — including flows that never select this
    // tile (the MSA PIN reset wizard reuses the LogonUI credential list) —
    // and the input watcher polls GLOBAL input, so starting it at Advise let
    // typing in those flows trigger the camera and interrupt the wizard.
    // Advise still selects initial-lock edge semantics; SetSelected starts the
    // watcher only when the user actually lands on this tile.
    // (CredUI/PLAP never reach this point — SetUsageScenario already
    // returned E_NOTIMPL for them.)
    if (!facelogin::credential_provider::ShouldStartInputDetection(m_state)) {
        FACELOGIN_INFO(L"Advise: state does not permit passive input detection");
        return S_OK;
    }
    m_inputDetectionRound =
        facelogin::credential_provider::InputDetectionRound::InitialLock;

    return S_OK;
}

STDMETHODIMP LockFaceCredential::UnAdvise() {
    AssertOwner();
    StopBackgroundWork();
    m_pCredentialEvents.Reset();
    return S_OK;
}

// ============================================================================
// ICredentialProviderCredential — SetSelected/SetDeselected
// ============================================================================

STDMETHODIMP LockFaceCredential::SetSelected(BOOL* pbAutoLogon) {
    AssertOwner();
    m_selected = true;
    if (m_state == State::Ready) {
        // Credentials ready (bg thread finished auth):
        // enable auto-logon so LogonUI calls GetSerialization to pack creds.
        *pbAutoLogon = TRUE;
    } else {
        // Still Waiting: no auto-logon; we wait for the bg thread.
        *pbAutoLogon = FALSE;
    }

    // The user actually landed on this tile (LogonUI focuses it right after
    // the Advise pass of the same enumeration round — milliseconds apart),
    // so starting the passive input watcher here keeps the keypress→auth
    // latency identical while never listening during flows that only Advise
    // us (PIN reset wizard, other tiles focused). Re-selecting after a
    // deselect restarts the watcher the same way; a re-selected FAILED tile
    // re-arms the failure watcher with fresh-edge semantics, so the tail of
    // the key/click that performed the re-selection cannot trigger a retry.
    if (facelogin::credential_provider::ShouldStartInputDetection(m_state) &&
        !m_inputThreadRunning) {
        if (facelogin::credential_provider::IsRetryableFailure(m_state)) {
            FACELOGIN_INFO(L"SetSelected: re-arming passive retry detection");
            ArmFailureRetryDetection();
        } else {
            FACELOGIN_INFO(L"SetSelected: starting passive input detection");
            StartInputDetectionThread();
        }
    }

    return S_OK;
}

STDMETHODIMP LockFaceCredential::SetDeselected() {
    Microsoft::WRL::ComPtr<LockFaceCredential> lifetime(this);
    AssertOwner();
    StopBackgroundWork();
    if (m_state == State::Waiting) {
        SetStatusText(kWaitingPrompt);
        NotifyStatusText();
    }
    return S_OK;
}

// ============================================================================
// ICredentialProviderCredential — Field State / Values
// ============================================================================

STDMETHODIMP LockFaceCredential::GetFieldState(
    DWORD dwFieldID,
    CREDENTIAL_PROVIDER_FIELD_STATE* pcpfs,
    CREDENTIAL_PROVIDER_FIELD_INTERACTIVE_STATE* pcpfis) {

    *pcpfs = CPFS_DISPLAY_IN_SELECTED_TILE;
    *pcpfis = CPFIS_NONE;

    switch (dwFieldID) {
    case kFieldTitle: // "Face Login" label — kept on the failure tile too (user
        // decision 2026-08-29: a title-less tile reads worse than a
        // three-line one; line count was never the complaint).
        *pcpfs = CPFS_DISPLAY_IN_BOTH;
        break;

    case kFieldStatus: // Status text — always visible so error states are seen
        *pcpfs = CPFS_DISPLAY_IN_BOTH;
        break;

    case kFieldSubmit: // Submit button — hidden in both scenarios
        *pcpfs = CPFS_HIDDEN;
        break;

    case kFieldSwitchLink: // Password-switch link, deselected tile-list view
        // only. The failure tile carries NO link: retries there are
        // input-triggered (ArmFailureRetryDetection) and the retry-hint field
        // carries the hint instead.
        *pcpfs = facelogin::credential_provider::IsRetryableFailure(m_state)
            ? CPFS_HIDDEN
            : CPFS_DISPLAY_IN_DESELECTED_TILE;
        break;

    case kFieldRetryHint: // "请按任意键重试" hint line under the failure reason
        // (its own field because tile text fields ignore "\r\n" — 装机实测
        // 2026-08-29). Enumeration shows it for any retryable failure:
        // a re-selected tile has SetSelected-armed detection, so the hint
        // is truthful by the time it renders. The one unarmed case (dead
        // service) hides it in-place from PresentRetryableFailure.
        *pcpfs = facelogin::credential_provider::IsRetryableFailure(m_state)
            ? CPFS_DISPLAY_IN_SELECTED_TILE
            : CPFS_HIDDEN;
        break;

    default:
        return E_INVALIDARG;
    }

    return S_OK;
}

STDMETHODIMP LockFaceCredential::GetStringValue(DWORD dwFieldID, PWSTR* ppwsz) {
    AssertOwner();
    *ppwsz = nullptr;

    switch (dwFieldID) {
    case kFieldTitle:
        return SHStrDupW(L"人脸登录", ppwsz);

    case kFieldStatus:
        switch (m_state) {
        case State::Waiting:
            return SHStrDupW(kWaitingPrompt, ppwsz);
        case State::Ready:
            return SHStrDupW(L"人脸识别成功，正在解锁...", ppwsz);
        case State::Authenticating:
        case State::Failed:
        case State::Error: {
            // Surface the specific service-provided text when one exists
            // (live "正在识别...", failure reasons like "未检测到人脸",
            // service errors like "人脸登录服务不可用"); otherwise the
            // per-state generic fallback. State and text are both STA-owned.
            const std::wstring snapshot = SnapshotStatusText();
            if (!snapshot.empty()) {
                return SHStrDupW(snapshot.c_str(), ppwsz);
            }
            return SHStrDupW(m_state == State::Authenticating
                                 ? L"正在识别..."
                                 : m_state == State::Failed
                                       ? L"人脸识别失败"
                                       : L"人脸登录服务不可用",
                             ppwsz);
        }
        default:
            return SHStrDupW(L"", ppwsz);
        }

    case kFieldSubmit: // Submit button
        return SHStrDupW(L"", ppwsz);

    case kFieldSwitchLink: // Command link — password switch, deselected list view only
        return SHStrDupW(L"切换到密码登录", ppwsz);

    case kFieldRetryHint: // Retry hint line (visibility is state-driven, see GetFieldState)
        return SHStrDupW(L"请按任意键重试", ppwsz);

    default:
        return E_INVALIDARG;
    }
}

STDMETHODIMP LockFaceCredential::GetBitmapValue(DWORD dwFieldID, HBITMAP* phbmp) {
    UNREFERENCED_PARAMETER(dwFieldID);
    *phbmp = nullptr;
    return E_NOTIMPL;
}

STDMETHODIMP LockFaceCredential::GetSubmitButtonValue(DWORD dwFieldID, DWORD* pdwAdjacentTo) {
    if (dwFieldID == kFieldSubmit) {
        *pdwAdjacentTo = kFieldStatus; // Next to the status text field
        return S_OK;
    }
    return E_INVALIDARG;
}

STDMETHODIMP LockFaceCredential::GetCheckboxValue(DWORD dwFieldID, BOOL* pbChecked, PWSTR* ppwszLabel) {
    UNREFERENCED_PARAMETER(dwFieldID);
    UNREFERENCED_PARAMETER(pbChecked);
    UNREFERENCED_PARAMETER(ppwszLabel);
    return E_NOTIMPL;
}

STDMETHODIMP LockFaceCredential::GetComboBoxValueCount(DWORD dwFieldID, DWORD* pcItems, DWORD* pdwSelectedItem) {
    UNREFERENCED_PARAMETER(dwFieldID);
    UNREFERENCED_PARAMETER(pcItems);
    UNREFERENCED_PARAMETER(pdwSelectedItem);
    return E_NOTIMPL;
}

STDMETHODIMP LockFaceCredential::GetComboBoxValueAt(DWORD dwFieldID, DWORD dwItem, PWSTR* ppwszItem) {
    UNREFERENCED_PARAMETER(dwFieldID);
    UNREFERENCED_PARAMETER(dwItem);
    UNREFERENCED_PARAMETER(ppwszItem);
    return E_NOTIMPL;
}

STDMETHODIMP LockFaceCredential::SetStringValue(DWORD dwFieldID, LPCWSTR pwz) {
    UNREFERENCED_PARAMETER(dwFieldID);
    UNREFERENCED_PARAMETER(pwz);
    return E_NOTIMPL;
}

STDMETHODIMP LockFaceCredential::SetCheckboxValue(DWORD dwFieldID, BOOL bChecked) {
    UNREFERENCED_PARAMETER(dwFieldID);
    UNREFERENCED_PARAMETER(bChecked);
    return E_NOTIMPL;
}

STDMETHODIMP LockFaceCredential::SetComboBoxSelectedValue(DWORD dwFieldID, DWORD dwSelectedItem) {
    UNREFERENCED_PARAMETER(dwFieldID);
    UNREFERENCED_PARAMETER(dwSelectedItem);
    return E_NOTIMPL;
}

STDMETHODIMP LockFaceCredential::CommandLinkClicked(DWORD dwFieldID) {
    Microsoft::WRL::ComPtr<LockFaceCredential> lifetime(this);
    AssertOwner();
    if (dwFieldID == kFieldSwitchLink) {
        if (facelogin::credential_provider::IsRetryableFailure(m_state)) {
            // UI-unreachable since the failure tile hides the switch link —
            // kept as defense (a hidden command link must still do the right
            // thing if LogonUI ever dispatches it, e.g. via keyboard focus).
            FACELOGIN_INFO(L"User explicitly requested face authentication retry");
            StartExplicitRetry();
            return S_OK;
        }
        FACELOGIN_INFO(L"User clicked 'Switch to password login'");
        SwitchToPasswordProvider();
        return S_OK;
    }
    return E_INVALIDARG;
}

// ============================================================================
// ICredentialProviderCredential — GetSerialization (THE KEY METHOD)
// ============================================================================

STDMETHODIMP LockFaceCredential::GetSerialization(
    CREDENTIAL_PROVIDER_GET_SERIALIZATION_RESPONSE* pcpgsr,
    CREDENTIAL_PROVIDER_CREDENTIAL_SERIALIZATION* pcpcs,
    PWSTR* ppwszOptionalStatusText,
    CREDENTIAL_PROVIDER_STATUS_ICON* pcpsiOptionalStatusIcon) {
    AssertOwner();
    Microsoft::WRL::ComPtr<LockFaceCredential> lifetime(this);

    *pcpgsr = CPGSR_NO_CREDENTIAL_NOT_FINISHED;
    *ppwszOptionalStatusText = nullptr;
    *pcpsiOptionalStatusIcon = CPSI_NONE;

    ZeroMemory(pcpcs, sizeof(*pcpcs));

    // Unlock scenario: if we're in Waiting state, the background input-
    // detection thread is still waiting for user input. Return "not
    // finished" — no credentials yet.
    if (m_state == State::Waiting) {
        FACELOGIN_DEBUG(L"GetSerialization: still Waiting for user input");
        return S_OK;
    }

    // If we're in Error state, service is not available — don't block login
    if (m_state == State::Error) {
        *pcpgsr = CPGSR_NO_CREDENTIAL_FINISHED;
        return S_OK;
    }

    // Auth failed earlier — don't retry, let user use password
    if (m_state == State::Failed) {
        *pcpgsr = CPGSR_NO_CREDENTIAL_FINISHED;
        return S_OK;
    }

    if (m_state == State::Authenticating) {
        // The background reader enforces this deadline even without LogonUI polls.
        const auto authDeadline = m_authDeadline.load();
        if (authDeadline != facelogin::ipc::AuthDeadline{} &&
            facelogin::ipc::RemainingAuthTimeMs(authDeadline) == 0) {
            FACELOGIN_WARN(L"Auth timed out waiting for service response");
            PresentRetryableFailure(State::Failed, L"识别超时，请重试");
            *pcpgsr = CPGSR_NO_CREDENTIAL_FINISHED;
            return S_OK;
        }
        // Not ready yet — LogonUI re-polls GetSerialization when auto-logon
        // is set; OnPipeResponse flips the state and re-enumerates.
        return S_OK;
    }

    // State::Ready — hand LogonUI the credential. The FIRST call packs
    // fresh (retaining a copy in m_packedCreds, because m_password is
    // zeroed on every pack return — see PackCredentials); any later call
    // re-serves from that retained copy.
    if (m_packedCreds) {
        BYTE* copy = static_cast<BYTE*>(CoTaskMemAlloc(m_cbPackedCreds));
        if (!copy) {
            FACELOGIN_ERROR(L"GetSerialization: CoTaskMemAlloc for cached credential failed");
            return E_OUTOFMEMORY;
        }
        memcpy(copy, m_packedCreds, m_cbPackedCreds);
        pcpcs->rgbSerialization = copy;
        pcpcs->cbSerialization = m_cbPackedCreds;
        pcpcs->ulAuthenticationPackage = m_ulAuthPackage;
        pcpcs->clsidCredentialProvider = CLSID_LockFaceProvider;
        *pcpgsr = CPGSR_RETURN_CREDENTIAL_FINISHED;
        return S_OK;
    }

    if (m_password.empty()) {
        // Ready without credentials and without a retained pack cannot
        // happen (Ready is only entered with a full result) — fail closed
        // rather than spinning NOT_FINISHED forever.
        FACELOGIN_ERROR(L"GetSerialization: Ready state has no credentials to serialize");
        *pcpgsr = CPGSR_NO_CREDENTIAL_FINISHED;
        return S_OK;
    }

    // NOTE: never log the password or any part of it — it is a credential.
    HRESULT hr = PackCredentials(pcpcs);
    if (SUCCEEDED(hr)) {
        *pcpgsr = CPGSR_RETURN_CREDENTIAL_FINISHED;
    } else {
        FACELOGIN_ERROR(L"PackCred FAILED: hr=0x%08X", hr);
        // A failed pack leaves no retained copy and the password is already
        // zeroed — later polls could not succeed either. Present a terminal
        // failure instead of spinning NOT_FINISHED.
        PresentRetryableFailure(State::Error, L"凭据封装失败，请使用密码登录");
        *pcpgsr = CPGSR_NO_CREDENTIAL_FINISHED;
    }
    return S_OK;
}

// ============================================================================
// ICredentialProviderCredential — ReportResult
// ============================================================================

STDMETHODIMP LockFaceCredential::ReportResult(
    NTSTATUS ntsStatus, NTSTATUS ntsSubstatus,
    PWSTR* ppwszOptionalStatusText,
    CREDENTIAL_PROVIDER_STATUS_ICON* pcpsiOptionalStatusIcon) {
    AssertOwner();
    Microsoft::WRL::ComPtr<LockFaceCredential> lifetime(this);

    *ppwszOptionalStatusText = nullptr;
    *pcpsiOptionalStatusIcon = CPSI_NONE;

    // The handoff is over (accepted or rejected) — the retained packed
    // credential must not outlive it.
    ClearPackedCredentials();

    if (ntsStatus == STATUS_SUCCESS) {
        FACELOGIN_INFO(L"Authentication succeeded");
    } else {
        FACELOGIN_WARN(L"Authentication failed: status=0x%08X, substatus=0x%08X",
                      ntsStatus, ntsSubstatus);

        // A rejected stored credential is terminal for this face attempt.
        // Returning to Waiting would re-enable passive input detection after
        // LogonUI re-advises the tile and could again consume password-entry
        // keystrokes as face retries. Require an explicit retry instead.
        facelogin::SecureClearWString(m_password);
        PresentRetryableFailure(
            State::Failed,
            L"Windows 拒绝了保存的凭据，请使用密码登录并重新录入人脸");
    }

    return S_OK;
}

// ============================================================================
// Private: StartAuth — begin the authentication pipeline
// ============================================================================

void LockFaceCredential::StartAuth() {
    AssertOwner();
    if (!m_selected || m_state == State::Authenticating || m_state == State::Ready) return;
    StopInputDetectionThread();
    StopAuthThread();
    if (!EnsureDispatcher()) {
        PresentRetryableFailure(State::Error, L"人脸登录服务不可用");
        return;
    }
    ++m_generation;
    EnterCriticalSection(&m_cs);
    m_acceptEvents = true;
    m_pendingEvents.clear();
    LeaveCriticalSection(&m_cs);
    const auto deadline = facelogin::ipc::MakeAuthDeadline();
    m_authDeadline.store(deadline);
    m_state = State::Authenticating;
    SetStatusText(L"正在识别...");
    m_hAuthStop = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    auto context = new (std::nothrow) WorkerContext{
        this, m_hAuthStop, m_generation, deadline, m_inputDetectionRound};
#ifdef FACELOGIN_CONCURRENCY_TESTS
    if (context && !m_testPipeName.empty()) context->pipeName = m_testPipeName.c_str();
#endif
    if (m_hAuthStop && context) {
        AddRef();
        m_hAuthThread = reinterpret_cast<HANDLE>(
            _beginthreadex(nullptr, 0, AuthThreadProc, context, 0, nullptr));
        if (!m_hAuthThread) Release();
    }
    if (!m_hAuthThread) {
        delete context;
        StopAuthThread();
        PresentRetryableFailure(State::Error, L"人脸登录服务不可用");
        return;
    }
    NotifyStatusText();
}

unsigned __stdcall LockFaceCredential::AuthThreadProc(void* param) {
    std::unique_ptr<WorkerContext> context(static_cast<WorkerContext*>(param));
    auto* credential = context->credential;
    {
        // Sole ownership: the STA only signals context->stop and joins. No
        // thread ever destroys a pipe while a callback or OVERLAPPED uses it.
        facelogin::PipeClient client;
        const auto deadline = context->deadline;
        auto response = [&](bool success, const std::wstring& message) {
            credential->QueueEvent(EventKind::Response, context->generation, success, message);
        };
        if (client.Connect(facelogin::ipc::RemainingAuthTimeMs(deadline), context->pipeName, context->stop) &&
            client.SendMessage(facelogin::ipc::MSG_AUTH_REQUEST,
                               facelogin::ipc::RemainingAuthTimeMs(deadline), context->stop)) {
            if (!client.ReadResponses(response, [&](const std::wstring& message) {
                    credential->QueueEvent(EventKind::Status, context->generation, true, message);
                }, deadline, context->stop)) {
                response(false, L"");
            }
        } else if (WaitForSingleObject(context->stop, 0) != WAIT_OBJECT_0) {
            credential->QueueEvent(EventKind::ConnectFailure, context->generation,
                facelogin::ipc::RemainingAuthTimeMs(deadline) == 0);
        }
    }
    credential->Release();
    return 0;
}

void LockFaceCredential::StopAuthThread() {
    AssertOwner();
    if (m_hAuthStop) SetEvent(m_hAuthStop);
    if (m_hAuthThread) {
        WaitForSingleObject(m_hAuthThread, INFINITE);
        CloseHandle(m_hAuthThread);
        m_hAuthThread = nullptr;
    }
    if (m_hAuthStop) {
        CloseHandle(m_hAuthStop);
        m_hAuthStop = nullptr;
    }
}

// ============================================================================
// Private: StartInputDetectionThread / StopInputDetectionThread
// ============================================================================

void LockFaceCredential::StartInputDetectionThread() {
    AssertOwner();
    if (!m_selected || !EnsureDispatcher()) return;
    if (m_inputThreadRunning) {
        FACELOGIN_WARN(L"StartInputDetectionThread: thread already running");
        return;
    }

    // A naturally completed watcher clears m_inputThreadRunning itself, but
    // its signaled thread handle and stop event are still owner resources.
    // Reap them before starting another round instead of overwriting/leaking
    // the old handles.
    if (m_hInputThread || m_hInputStop) {
        StopInputDetectionThread();
    }

    // Manual-reset, see the member declaration: the watcher loop, the settle
    // window and StartAuth's cancellable pipe waits all observe one signal.
    m_hInputStop = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!m_hInputStop) {
        FACELOGIN_ERROR(L"Failed to create input stop event");
        return;
    }

    EnterCriticalSection(&m_cs);
    m_acceptEvents = true;
    LeaveCriticalSection(&m_cs);
    auto context = new (std::nothrow) WorkerContext{
        this, m_hInputStop, m_generation, {}, m_inputDetectionRound};
    if (!context) {
        CloseHandle(m_hInputStop);
        m_hInputStop = nullptr;
        return;
    }
    m_inputThreadRunning = true;
    AddRef();
    unsigned threadId = 0;
    m_hInputThread = reinterpret_cast<HANDLE>(
        _beginthreadex(nullptr, 0, InputDetectionThreadProc, context, 0, &threadId));
    if (!m_hInputThread || m_hInputThread == INVALID_HANDLE_VALUE) {
        delete context;
        Release();
        FACELOGIN_ERROR(L"Failed to start input detection thread");
        m_inputThreadRunning = false;
        CloseHandle(m_hInputStop);
        m_hInputStop = nullptr;
    }
}

void LockFaceCredential::StopInputDetectionThread() {
    AssertOwner();
    if (!m_hInputThread && !m_hInputStop) {
        return;
    }

    // Workers only post messages and never wait for this STA. Signal, join,
    // then close borrowed handles; never pump COM or terminate a thread here.
    if (m_hInputStop) {
        SetEvent(m_hInputStop);
    }

    if (m_hInputThread) {
        WaitForSingleObject(m_hInputThread, INFINITE);
        CloseHandle(m_hInputThread);
        m_hInputThread = nullptr;
    }

    if (m_hInputStop) {
        CloseHandle(m_hInputStop);
        m_hInputStop = nullptr;
    }

    m_inputThreadRunning = false;
}

void LockFaceCredential::ArmFailureRetryDetection() {
    if (!m_selected || m_inputThreadRunning) {
        return;
    }

    // Failure tiles are already fully visible: require a fresh MAKE/button
    // down and seed every physically held input before the watcher loop. This
    // rejects auto-repeat and release tails without a timing window.
    m_inputDetectionRound =
        facelogin::credential_provider::InputDetectionRound::FailureRetry;
    StartInputDetectionThread();
}

// ============================================================================
// Private: Credential Packing
// ============================================================================

HRESULT LockFaceCredential::PackCredentials(
    CREDENTIAL_PROVIDER_CREDENTIAL_SERIALIZATION* pcpcs) {

    // RAII: guarantee m_password is zeroed on EVERY return path, including
    // the three early-returns below (CredPack query failure / OOM / pack
    // failure). Previously only the success path cleared it, so a packing
    // failure left plaintext in the member wstring until the destructor —
    // and LogonUI re-polls GetSerialization, so the password sat there for
    // the credential's lifetime (security #9).
    facelogin::SecureWStringGuard pwdGuard(m_password);

    using namespace facelogin::account_identity;
    std::wstring domain = m_domain, username = m_username;
    wchar_t computer[MAX_COMPUTERNAME_LENGTH + 1]{};
    DWORD chars = ARRAYSIZE(computer);
    if (!GetComputerNameW(computer, &chars)) return HRESULT_FROM_WIN32(GetLastError());
    // Transport domain is the service's machine name, including for AD users.
    // SID lookup also repairs the routing of previously enrolled AD records.
    if (!m_sid.empty() && !ResolveUserSid(m_sid, domain, username)) {
        FACELOGIN_ERROR(L"PackCred: stored user SID could not be resolved");
        return HRESULT_FROM_WIN32(ERROR_NONE_MAPPED);
    }
    const Kind kind = ClassifyAuthority(domain, computer);
    const bool online = kind == Kind::MicrosoftAccount || kind == Kind::AzureAd ||
        (kind == Kind::Local && !m_upn.empty());
    if (kind == Kind::Unknown) return E_INVALIDARG;
    ULONG ulAuthPackage = 0;
    HANDLE hLsa = nullptr;
    NTSTATUS status = LsaConnectUntrusted(&hLsa);
    if (status != 0) return HRESULT_FROM_WIN32(LsaNtStatusToWinError(status));
    if (!hLsa) return E_FAIL;
    char nativePackage[] = "Negotiate";
    char onlinePackage[] = "MICROSOFT_AUTHENTICATION_PACKAGE_V1_0";
    char* package = online ? onlinePackage : nativePackage;
    LSA_STRING packageName{};
    packageName.Buffer = package;
    packageName.Length = static_cast<USHORT>(strlen(package));
    packageName.MaximumLength = packageName.Length + 1;
    status = LsaLookupAuthenticationPackage(hLsa, &packageName, &ulAuthPackage);
    LsaDeregisterLogonProcess(hLsa);
    if (status != 0) return HRESULT_FROM_WIN32(LsaNtStatusToWinError(status));

    DWORD cbPackedCreds = 0;
    BYTE* pPackedCreds = nullptr;
    if (!online) {
        HRESULT hr = facelogin::credential_provider::PackNativeLogon(
            domain, username, m_password, m_usageScenario, &pPackedCreds, &cbPackedCreds);
        if (FAILED(hr)) return hr;
    } else {
        // Preserve the proven MSA path: plain UPN, flags=0 and MSV1_0.
        std::wstring packedUser = m_upn.empty() ? domain + L"\\" + username : m_upn;
        if (!CredPackAuthenticationBufferW(0, packedUser.data(), m_password.data(), nullptr, &cbPackedCreds)) {
            const DWORD error = GetLastError();
            if (error != ERROR_INSUFFICIENT_BUFFER) return error ? HRESULT_FROM_WIN32(error) : E_FAIL;
        }
        if (!cbPackedCreds) return E_FAIL;
        pPackedCreds = static_cast<BYTE*>(CoTaskMemAlloc(cbPackedCreds));
        if (!pPackedCreds) return E_OUTOFMEMORY;
        const DWORD allocatedBytes = cbPackedCreds;
        if (!CredPackAuthenticationBufferW(0, packedUser.data(), m_password.data(), pPackedCreds, &cbPackedCreds)) {
            const DWORD error = GetLastError();
            SecureZeroMemory(pPackedCreds, allocatedBytes);
            CoTaskMemFree(pPackedCreds);
            return error ? HRESULT_FROM_WIN32(error) : E_FAIL;
        }
    }

    // Retain a private copy so GetSerialization can re-serve the credential
    // on later calls: the guard above zeroes m_password on EVERY return path
    // (security #9), so without this copy a re-polling LogonUI could never
    // serialize a second time. The copy lives only while the Ready handoff
    // is pending — ClearPackedCredentials bounds it at ReportResult, failure
    // presentation and destruction.
    ClearPackedCredentials();
    m_packedCreds = new (std::nothrow) BYTE[cbPackedCreds];
    if (!m_packedCreds) {
        SecureZeroMemory(pPackedCreds, cbPackedCreds);
        CoTaskMemFree(pPackedCreds);
        return E_OUTOFMEMORY;
    }
    memcpy(m_packedCreds, pPackedCreds, cbPackedCreds);
    m_cbPackedCreds = cbPackedCreds;
    m_ulAuthPackage = ulAuthPackage;

    pcpcs->rgbSerialization = pPackedCreds;
    pcpcs->cbSerialization = cbPackedCreds;
    pcpcs->ulAuthenticationPackage = ulAuthPackage;
    pcpcs->clsidCredentialProvider = CLSID_LockFaceProvider;

    FACELOGIN_INFO(L"PackCred SUCCESS: user=%s\\%s (UPN=%s), cbSerialization=%lu, ulAuthPackage=%lu",
                  domain.c_str(), username.c_str(),
                  m_upn.empty() ? L"<none>" : m_upn.c_str(),
                  cbPackedCreds, ulAuthPackage);
    return S_OK;
}

void LockFaceCredential::ClearPackedCredentials() {
    AssertOwner();
    if (m_packedCreds) {
        SecureZeroMemory(m_packedCreds, m_cbPackedCreds);
        delete[] m_packedCreds;
        m_packedCreds = nullptr;
    }
    m_cbPackedCreds = 0;
    m_ulAuthPackage = 0;
}

// ============================================================================
// Private: Switch to Password Provider
// ============================================================================

HRESULT LockFaceCredential::SwitchToPasswordProvider() {
    StopBackgroundWork();
    m_state = State::Failed;
    TriggerReEnumeration();

    // Also return NO_CREDENTIAL_FINISHED to deselect our tile
    // This causes LogonUI to show other providers
    // (Actually done in GetSerialization via state change)
    return S_OK;
}

void LockFaceCredential::SetStatusText(const std::wstring& text) {
    AssertOwner();
    m_statusText = text;
}

std::wstring LockFaceCredential::SnapshotStatusText() {
    AssertOwner();
    return m_statusText;
}

void LockFaceCredential::OnPipeStatus(const std::wstring& message) {
    AssertOwner();
    // A deselection may have torn the pipe down mid-flight; a late status
    // from that round must not overwrite the reset idle prompt.
    if (!facelogin::credential_provider::ShouldProcessPipeResponse(m_state)) {
        FACELOGIN_INFO(L"OnPipeStatus: dropped late status in state=%d",
                       static_cast<int>(m_state));
        return;
    }
    SetStatusText(message);
    // Use SetFieldString to update the status text in-place on the lock
    // screen, without triggering re-enumeration (which destroys the pipe).
    NotifyStatusText();
}

void LockFaceCredential::OnPipeResponse(bool success, const std::wstring& message) {
    AssertOwner();
    // Same late-result guard as OnPipeStatus: a response delivered after the
    // tile was deselected (auth aborted, state reset to Waiting) is stale
    // and must not overwrite state, credentials or tile text.
    if (!facelogin::credential_provider::ShouldProcessPipeResponse(m_state)) {
        FACELOGIN_INFO(L"OnPipeResponse: dropped late result in state=%d",
                       static_cast<int>(m_state));
        return;
    }
    if (success) {
        auto result = facelogin::ipc::ParseAuthMessage(message);
        facelogin::SecureWStringGuard passwordGuard(result.password);

        if (result.status == facelogin::ipc::AuthResult::Status::Success) {
            FACELOGIN_INFO(L"OnPipeResponse: Auth success: domain=%s, username=%s (UPN=%s)",
                          result.domain.c_str(), result.username.c_str(),
                          result.upn.c_str());
            // NOTE: the password itself is never logged — only metadata.
            m_sid = result.sid;
            m_upn = result.upn;
            m_domain = result.domain;
            m_username = result.username;
            facelogin::SecureClearWString(m_password);
            m_password.swap(result.password);
            m_state = State::Ready;
            // Push the success text immediately so the tile does not keep
            // showing the last in-flight status ("正在识别...") during
            // the re-enumeration gap before LogonUI calls GetStringValue.
            SetStatusText(L"人脸识别成功，正在解锁...");
            const auto generation = m_generation;
            NotifyStatusText();
            if (generation != m_generation) return;
            // Ask LogonUI to call GetSerialization again right away
            if (facelogin::credential_provider::ShouldReenumerateAfterTerminal(m_state)) {
                TriggerReEnumeration();
            }
            return;
        } else if (result.status == facelogin::ipc::AuthResult::Status::Timeout) {
            FACELOGIN_INFO(L"OnPipeResponse: Auth timeout");
            PresentRetryableFailure(State::Failed, L"识别超时，请重试");
            return;
        } else if (result.status == facelogin::ipc::AuthResult::Status::Error) {
            FACELOGIN_WARN(L"OnPipeResponse: Auth error: %s", result.errorMessage.c_str());
            // Surface the service's specific error (e.g. "检测到攻击，请使用真实人脸")
            // on the lock screen instead of the generic "service unavailable".
            PresentRetryableFailure(
                State::Error,
                result.errorMessage.empty()
                    ? L"人脸登录服务不可用"
                    : result.errorMessage);
            return;
        }
    } else {
        FACELOGIN_WARN(L"OnPipeResponse: Read failed — server disconnected?");
        PresentRetryableFailure(State::Error, L"人脸登录服务不可用");
        return;
    }
}

void LockFaceCredential::PresentRetryableFailure(
    State failureState,
    const std::wstring& statusText, bool armRetry) {
    if (!facelogin::credential_provider::IsRetryableFailure(failureState)) {
        FACELOGIN_ERROR(L"PresentRetryableFailure called with non-failure state=%d",
                        static_cast<int>(failureState));
        return;
    }

    AssertOwner();
    StopAuthThread();
    m_state = failureState;
    facelogin::SecureClearWString(m_password);
    m_authDeadline.store({});
    SetStatusText(statusText.empty() ? L"人脸识别失败" : statusText);

    // A terminal state ends any pending Ready handoff — the retained packed
    // credential (which carries the plaintext password) must not survive it.
    ClearPackedCredentials();

    // The reason line states WHAT failed; field 4 below owns "how to
    // retry" — strip instruction tails that would read as duplication next
    // to "请按任意键重试" (用户定稿 2026-08-29). Only the two tails whose
    // retry the hint actually serves; password/re-enroll instructions
    // (e.g. "请使用密码登录并重新录入人脸") stay — the hint does not
    // replace them.
    for (const wchar_t* tail : {L"，请重试", L"，请使用真实人脸"}) {
        const size_t tailLen = wcslen(tail);
        if (m_statusText.size() > tailLen &&
            m_statusText.compare(m_statusText.size() - tailLen, tailLen,
                                 tail) == 0) {
            m_statusText.erase(m_statusText.size() - tailLen);
            break;
        }
    }

    // A failed connection retains the existing no-passive-retry behavior.
    // Service terminal failures and Windows rejection can re-arm while selected.
    if (armRetry) ArmFailureRetryDetection();
    const auto generation = m_generation;
    NotifyStatusText();
    if (generation != m_generation) return;
    NotifyTileFields(true, m_inputThreadRunning);
    FACELOGIN_INFO(L"Terminal failure shown in-place; passive retry %s",
                   m_inputThreadRunning ? L"re-armed" : L"not armed (service down)");
}

void LockFaceCredential::StartExplicitRetry() {
    AssertOwner();
    if (!facelogin::credential_provider::IsRetryableFailure(m_state)) {
        return;
    }

    StopAuthThread();
    m_state = State::Waiting;
    const auto generation = m_generation;
    NotifyTileFields(false, false);
    if (generation == m_generation && m_selected) StartAuth();
}

// ============================================================================
// Private: Trigger Re-enumeration
// ============================================================================

void LockFaceCredential::TriggerReEnumeration() {
    AssertOwner();
    auto events = m_pProviderEvents;
    const auto context = m_upAdviseContext;
    if (events) events->CredentialsChanged(context);
}
