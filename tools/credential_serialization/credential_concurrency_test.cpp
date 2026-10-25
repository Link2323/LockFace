#include "../../credential_provider/LockFaceCredential.h"
#include "../../credential_provider/LockFaceProvider.h"
#include "../../credential_provider/resource.h"
#include "../../credential_provider/waiting_prompt_policy.h"
#include "../../credential_provider/input_trigger_policy.h"
#include <thread>
#include <iostream>
#include <atomic>
#include <cstdlib>

using Microsoft::WRL::ComPtr;
using State = facelogin::credential_provider::AuthState;
static int failures = 0;
static void Check(bool ok, const char* label) {
    if (!ok) { ++failures; std::cerr << "FAIL: " << label << '\n'; }
}
static void Wait(HANDLE handle) {
    if (WaitForSingleObject(handle, 5000) != WAIT_OBJECT_0) {
        std::cerr << "FAIL: barrier timed out\n";
        std::exit(2);
    }
}
struct Event {
    HANDLE handle = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    ~Event() { CloseHandle(handle); }
    void Set() { SetEvent(handle); }
};
static void Pump() {
    MSG message{};
    while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) DispatchMessageW(&message);
}
static const std::wstring kSuccess =
    L"AUTH_SUCCESS:S-1-5-21-1-2-3-1001:dummy@example.test:DOMAIN\\Dummy:测试:password";

struct CredentialConcurrencyTestAccess {
    using Kind = LockFaceCredential::EventKind;
    static std::uint64_t Arm(LockFaceCredential& c) {
        c.m_state = State::Authenticating;
        c.m_authDeadline.store(facelogin::ipc::MakeAuthDeadline());
        ++c.m_generation;
        EnterCriticalSection(&c.m_cs);
        c.m_acceptEvents = true;
        LeaveCriticalSection(&c.m_cs);
        return c.m_generation;
    }
    static void Publish(LockFaceCredential& c, std::uint64_t generation,
                        Kind kind = Kind::Response, const std::wstring& text = kSuccess) {
        c.QueueEvent(kind, generation, true, text);
    }
    static bool Empty(LockFaceCredential& c) { return c.m_pendingEvents.empty(); }
    static State GetState(LockFaceCredential& c) { return c.m_state; }
    static bool Complete(LockFaceCredential& c) {
        return c.m_state == State::Ready && c.m_sid == L"S-1-5-21-1-2-3-1001" &&
            c.m_upn == L"dummy@example.test" && c.m_domain == L"DOMAIN" &&
            c.m_username == L"Dummy" && c.m_password == L"测试:password";
    }
    static void Barrier(LockFaceCredential& c, std::function<void(int)> barrier) {
        c.m_testBarrier = std::move(barrier);
    }
    static void Start(LockFaceCredential& c, const std::wstring& name) {
        c.m_testPipeName = name;
        c.m_selected = true;
        c.StartAuth();
    }
    static void PrepareReportRetry(LockFaceCredential& c, const std::wstring& name) {
        c.m_testPipeName = name;
        c.m_selected = true;
        c.m_state = State::Ready;
    }
    static bool AuthRunning(LockFaceCredential& c) {
        return c.m_state == State::Authenticating && c.m_hAuthThread && c.m_hAuthStop;
    }
    static bool Stopped(LockFaceCredential& c) {
        return !c.m_hAuthThread && !c.m_hAuthStop && !c.m_hInputThread && !c.m_hInputStop;
    }
    static std::uint64_t PrepareInput(LockFaceCredential& c) {
        const auto generation = Arm(c);
        c.m_state = State::Waiting;
        c.m_selected = true;
        return generation;
    }
    static void InputCandidate(LockFaceCredential& c, HANDLE stop,
                               std::uint64_t generation,
                               facelogin::credential_provider::InputTriggerKind kind) {
        const LockFaceCredential::WorkerContext context{
            &c, stop, generation, {},
            facelogin::credential_provider::InputDetectionRound::InitialLock};
        c.QueueInputAfterSelection(context, kind);
    }
    static bool HasOneInput(LockFaceCredential& c) {
        return c.m_pendingEvents.size() == 1 &&
            c.m_pendingEvents.front()->kind == Kind::Input;
    }
    static void Attach(LockFaceProvider& p, LockFaceCredential* c) {
        p.m_pCredential = c; // transfers the construction reference
        c->Initialize(&p);
    }
};
using Access = CredentialConcurrencyTestAccess;

struct TileEvents final : ICredentialProviderCredentialEvents {
    ULONG refs = 1;
    DWORD owner = GetCurrentThreadId();
    int strings = 0, fields = 0;
    std::function<void(ICredentialProviderCredential*)> onString;
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** out) override {
        *out = nullptr;
        if (iid != IID_IUnknown && iid != IID_ICredentialProviderCredentialEvents) return E_NOINTERFACE;
        *out = static_cast<ICredentialProviderCredentialEvents*>(this); AddRef(); return S_OK;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return ++refs; }
    ULONG STDMETHODCALLTYPE Release() override { auto n = --refs; if (!n) delete this; return n; }
    HRESULT STDMETHODCALLTYPE SetFieldString(ICredentialProviderCredential* c, DWORD, LPCWSTR text) override {
        Check(GetCurrentThreadId() == owner, "all tile callbacks stay on advising STA");
        ++strings;
        const std::wstring before(text);
        if (onString) onString(c);
        Check(before == text, "notification text survives reentrant UnAdvise");
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE SetFieldState(ICredentialProviderCredential*, DWORD, CREDENTIAL_PROVIDER_FIELD_STATE) override {
        Check(GetCurrentThreadId() == owner, "field notification stays on STA"); ++fields; return S_OK;
    }
    HRESULT STDMETHODCALLTYPE SetFieldInteractiveState(ICredentialProviderCredential*, DWORD, CREDENTIAL_PROVIDER_FIELD_INTERACTIVE_STATE) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE SetFieldCheckbox(ICredentialProviderCredential*, DWORD, BOOL, LPCWSTR) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE SetFieldBitmap(ICredentialProviderCredential*, DWORD, HBITMAP) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE SetFieldComboBoxSelectedItem(ICredentialProviderCredential*, DWORD, DWORD) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE DeleteFieldComboBoxItem(ICredentialProviderCredential*, DWORD, DWORD) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE AppendFieldComboBoxItem(ICredentialProviderCredential*, DWORD, LPCWSTR) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE SetFieldSubmitButton(ICredentialProviderCredential*, DWORD, DWORD) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE OnCreatingWindow(HWND* out) override { *out = nullptr; return S_OK; }
};
struct ProviderEvents final : ICredentialProviderEvents {
    ULONG refs = 1;
    DWORD owner = GetCurrentThreadId();
    int changes = 0;
    std::function<void()> onChange;
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** out) override {
        *out = nullptr;
        if (iid != IID_IUnknown && iid != IID_ICredentialProviderEvents) return E_NOINTERFACE;
        *out = static_cast<ICredentialProviderEvents*>(this); AddRef(); return S_OK;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return ++refs; }
    ULONG STDMETHODCALLTYPE Release() override { auto n = --refs; if (!n) delete this; return n; }
    HRESULT STDMETHODCALLTYPE CredentialsChanged(UINT_PTR context) override {
        Check(GetCurrentThreadId() == owner && context == 123, "provider event STA and context");
        ++changes;
        if (onChange) onChange();
        return S_OK;
    }
};

template<class T> static ComPtr<T> Make() { ComPtr<T> p; p.Attach(new T); return p; }

STDAPI DllCanUnloadNow();
STDAPI DllGetClassObject(REFCLSID, REFIID, void**);

static void ComFactoryOwnership() {
    Check(DllCanUnloadNow() == S_OK, "no live instances allow unload");
    IClassFactory* factory = nullptr;
    Check(SUCCEEDED(DllGetClassObject(CLSID_LockFaceProvider, IID_IClassFactory,
          reinterpret_cast<void**>(&factory))) && factory, "actual COM class factory");
    if (!factory) return;
    Check(DllCanUnloadNow() == S_FALSE, "factory pins DLL");
    void* unsupported = nullptr;
    Check(factory->CreateInstance(nullptr, IID_ICredentialProviderCredential, &unsupported) == E_NOINTERFACE &&
          !unsupported, "failed CreateInstance drops construction reference");
    ICredentialProvider* provider = nullptr;
    Check(SUCCEEDED(factory->CreateInstance(nullptr, IID_ICredentialProvider,
          reinterpret_cast<void**>(&provider))) && provider, "factory creates provider");
    factory->Release();
    if (!provider) return;
    auto credential = new LockFaceCredential;
    Access::Attach(*static_cast<LockFaceProvider*>(provider), credential);
    credential->Advise(nullptr);
    BOOL autoLogon = FALSE;
    credential->SetSelected(&autoLogon);
    credential->AddRef();
    Check(provider->Release() == 0, "factory transfers exactly one provider reference");
    Check(Access::Stopped(*credential), "real COM release joins worker without UnAdvise");
    Check(DllCanUnloadNow() == S_FALSE, "surviving credential pins DLL after provider destruction");
    credential->Release();
    Pump();
    Check(DllCanUnloadNow() == S_OK, "all instances released, including failed QI");
    std::cout << "PASS COM factory ownership and DLL unload accounting\n";
}

static void PublicationTeardownInterleavings() {
    for (int publication : {0, 1}) for (int gate : {2, 3}) {
        auto credential = Make<LockFaceCredential>();
        auto events = Make<TileEvents>();
        for (int i = 0; i < 1000; ++i) {
            Check(SUCCEEDED(credential->Advise(events.Get())), "Advise dispatcher");
            const auto generation = Access::Arm(*credential.Get());
            Event paused, resume, finished;
            Access::Barrier(*credential.Get(), [&](int point) {
                if (point == publication) { paused.Set(); Wait(resume.handle); }
                if (point == gate) { resume.Set(); Wait(finished.handle); }
            });
            std::thread worker([&] {
                CoInitializeEx(nullptr, COINIT_MULTITHREADED);
                Access::Publish(*credential.Get(), generation, Access::Kind::Response,
                                i % 2 ? kSuccess : facelogin::ipc::MSG_AUTH_TIMEOUT);
                finished.Set();
                CoUninitialize();
            });
            Wait(paused.handle);
            credential->UnAdvise();
            worker.join();
            Access::Barrier(*credential.Get(), {});
            Pump();
            Check(Access::GetState(*credential.Get()) == State::Waiting &&
                  Access::Empty(*credential.Get()) && events->strings == 0 && events->refs == 1,
                  "teardown discards queued or racing terminal, releases sink");
        }
    }
    std::cout << "PASS publication/teardown: four interleavings x 1000\n";
}

static void DispatchAndReentry() {
    auto credential = Make<LockFaceCredential>();
    auto tile = Make<TileEvents>();
    auto provider = Make<ProviderEvents>();
    credential->Advise(tile.Get());
    credential->AdviseProvider(provider.Get(), 123);
    auto generation = Access::Arm(*credential.Get());
    std::thread worker([&] {
        Access::Publish(*credential.Get(), generation, Access::Kind::Status, L"正在识别...");
        Access::Publish(*credential.Get(), generation);
    });
    worker.join();
    Check(Access::GetState(*credential.Get()) == State::Authenticating && tile->strings == 0,
          "publishing never mutates state or calls COM");
    provider->onChange = [&] { credential->UnAdvise(); credential->UnadviseProvider(); };
    Pump();
    // One status notification only: the success handoff deliberately pushes
    // no banner and keeps the in-flight "正在识别..." text.
    Check(Access::Complete(*credential.Get()) && tile->strings == 1 && provider->changes == 1,
          "coherent success and reentrant re-enumeration preserve Ready handoff");
    credential->Advise(tile.Get());
    BOOL autoLogon = FALSE;
    credential->SetSelected(&autoLogon);
    Check(autoLogon, "Ready survives re-Advise and auto logon selection");
    credential->UnAdvise();

    credential->Advise(tile.Get());
    generation = Access::Arm(*credential.Get());
    auto old = generation;
    credential->UnAdvise();
    credential->Advise(tile.Get());
    generation = Access::Arm(*credential.Get());
    Access::Publish(*credential.Get(), old);
    Pump();
    Check(Access::GetState(*credential.Get()) == State::Authenticating,
          "stale success cannot complete a new round");
    tile->onString = [](ICredentialProviderCredential* c) { c->UnAdvise(); };
    int fields = tile->fields;
    Access::Publish(*credential.Get(), generation, Access::Kind::Response, L"AUTH_ERROR:检测失败，请重试");
    Pump();
    Check(tile->fields == fields && tile->refs == 1 && Access::Stopped(*credential.Get()),
          "failure notification reentry prevents subsequent calls and watcher rearm");
    tile->onString = {};

    // Destruction during a status callback: provider teardown omits no joins,
    // and the dispatch reference keeps the Credential alive until return.
    auto p = new LockFaceProvider;
    auto c = new LockFaceCredential;
    Access::Attach(*p, c);
    c->Advise(tile.Get());
    p->Advise(provider.Get(), 123);
    generation = Access::Arm(*c);
    tile->onString = [&](ICredentialProviderCredential*) { p->Release(); };
    Access::Publish(*c, generation, Access::Kind::Status, L"status");
    Pump();
    tile->onString = {};
    Check(tile->refs == 1 && provider->refs == 1, "provider destruction releases both event sinks");
    std::cout << "PASS STA dispatch, stale epochs, reentry and release during notification\n";
}

// An isolated pipe exercises the real auth worker without touching the service.
static bool Transfer(HANDLE pipe, bool write, void* data, DWORD bytes) {
    Event done;
    OVERLAPPED operation{}; operation.hEvent = done.handle;
    DWORD transferred = 0;
    BOOL ok = write ? WriteFile(pipe, data, bytes, nullptr, &operation)
                    : ReadFile(pipe, data, bytes, nullptr, &operation);
    if (!ok && GetLastError() != ERROR_IO_PENDING) return false;
    if (WaitForSingleObject(done.handle, 5000) != WAIT_OBJECT_0) {
        CancelIoEx(pipe, &operation);
        GetOverlappedResult(pipe, &operation, &transferred, TRUE);
        return false;
    }
    return GetOverlappedResult(pipe, &operation, &transferred, FALSE) && transferred > 0;
}
static void RealWorker(bool cancel, bool retryInReport = false) {
    auto credential = Make<LockFaceCredential>();
    auto tile = Make<TileEvents>();
    credential->Advise(tile.Get());
    std::wstring name = L"\\\\.\\pipe\\LockFaceConcurrencyTest_" + std::to_wstring(GetCurrentProcessId());
    HANDLE pipe = CreateNamedPipeW(name.c_str(), PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED,
        PIPE_TYPE_MESSAGE | PIPE_READMODE_MESSAGE | PIPE_WAIT, 1, 8192, 8192, 0, nullptr);
    Check(pipe != INVALID_HANDLE_VALUE, "create isolated test pipe");
    Event requested, stopServer;
    std::atomic<bool> serverOk = true;
    std::thread server([&] {
        Event connected;
        OVERLAPPED operation{}; operation.hEvent = connected.handle;
        BOOL ok = ConnectNamedPipe(pipe, &operation);
        DWORD error = ok ? ERROR_SUCCESS : GetLastError();
        if (error == ERROR_IO_PENDING) Wait(connected.handle);
        else if (error != ERROR_SUCCESS && error != ERROR_PIPE_CONNECTED) serverOk = false;
        wchar_t request[256]{};
        if (!Transfer(pipe, false, request, sizeof(request)) || wcscmp(request, facelogin::ipc::MSG_AUTH_REQUEST)) serverOk = false;
        requested.Set();
        if (cancel) Wait(stopServer.handle);
        else {
            if (retryInReport) Wait(stopServer.handle);
            std::wstring status = L"STATUS:正在识别...";
            auto success = kSuccess;
            if (!Transfer(pipe, true, status.data(), static_cast<DWORD>((status.size()+1)*sizeof(wchar_t))) ||
                !Transfer(pipe, true, success.data(), static_cast<DWORD>((success.size()+1)*sizeof(wchar_t))) ||
                !Transfer(pipe, false, request, sizeof(request)) || wcscmp(request, facelogin::ipc::MSG_AUTH_ACK)) serverOk = false;
        }
    });
    if (retryInReport) {
        Access::PrepareReportRetry(*credential.Get(), name);
        bool retried = false;
        tile->onString = [&](ICredentialProviderCredential* c) {
            if (retried) return;
            retried = true;
            c->CommandLinkClicked(kFieldSwitchLink);
            Wait(requested.handle);
        };
        PWSTR text = nullptr;
        CREDENTIAL_PROVIDER_STATUS_ICON icon{};
        credential->ReportResult(static_cast<NTSTATUS>(0xC000006DL), 0, &text, &icon);
        CoTaskMemFree(text);
        tile->onString = {};
        Check(Access::AuthRunning(*credential.Get()), "ReportResult preserves retry started during COM callback");
        stopServer.Set();
    } else {
        Access::Start(*credential.Get(), name);
    }
    Wait(requested.handle);
    if (cancel) {
        const auto start = GetTickCount64();
        credential->SetDeselected();
        Check(GetTickCount64()-start < 1000 && Access::Stopped(*credential.Get()),
              "cancel silent pending read joins within one second");
        stopServer.Set();
    } else {
        const auto start = GetTickCount64();
        while (Access::GetState(*credential.Get()) == State::Authenticating && GetTickCount64()-start < 5000) {
            MsgWaitForMultipleObjects(0, nullptr, FALSE, 10, QS_ALLINPUT); Pump();
        }
        Check(Access::Complete(*credential.Get()) && Access::Stopped(*credential.Get()),
              "real worker delivers coherent success and reaps thread handles");
    }
    server.join();
    Check(serverOk, "pipe request/status/success/ACK control");
    CloseHandle(pipe);
    credential->UnAdvise();
}
static void WatcherTeardown() {
    DWORD before = 0, after = 0;
    for (int i = 0; i < 101; ++i) {
        auto p = new LockFaceProvider;
        auto c = new LockFaceCredential;
        Access::Attach(*p, c);
        c->Advise(nullptr);
        BOOL autoLogon = FALSE;
        c->SetSelected(&autoLogon);
        c->AddRef(); // inspect after Provider release without UnAdvise
        p->Release();
        Check(Access::Stopped(*c), "provider destruction joins watcher without prior UnAdvise");
        c->Release();
        Pump();
        if (i == 0) GetProcessHandleCount(GetCurrentProcess(), &before);
    }
    GetProcessHandleCount(GetCurrentProcess(), &after);
    Check(after <= before + 2, "100 watcher teardown rounds do not leak handles");
    std::cout << "PASS worker cancel/success and 100 watcher ownership rounds\n";
}
static void WaitingPromptTiming() {
    using namespace facelogin::credential_provider;
    WaitingPromptPolicy prompt;
    Check(!prompt.Update(0, false, false), "wallpaper starts without a prompt");
    Check(!prompt.Update(5000, false, false), "wallpaper dwell never pre-renders a prompt");
    Check(!prompt.Update(5010, true, false), "revealing credentials starts a fresh grace period");
    Check(!prompt.Update(5100, true, true), "held dismissal key keeps the tile blank");
    Check(!prompt.Update(6000, true, true), "long-held dismissal key never flashes a prompt");
    Check(!prompt.Update(6010, true, false), "release gets time to deliver its raw input edge");
    Check(prompt.Update(6310, true, false), "idle credential view asks for the additional press");
    Check(!prompt.Update(6400, false, false), "leaving credential view hides the prompt");
    Check(!prompt.Update(9000, true, false), "returning to credentials restarts the grace period");
    Check(prompt.Update(9300, true, false), "two-press path retains its instruction");

    // The watcher checks the input policy before publishing any prompt. A
    // wallpaper-swallowed MAKE must still go straight from blank to auth.
    WaitingPromptPolicy onePress;
    InputTriggerPolicy keyboard(InputDetectionRound::InitialLock);
    Check(!onePress.Update(5000, false, false), "ordinary key starts after long wallpaper dwell");
    Check(!onePress.Update(5010, true, true), "ordinary dismissal key is held during reveal");
    Check(keyboard.ObserveKey('A', false) && keyboard.triggered(),
          "orphan ordinary BREAK starts auth before prompt evaluation");

    WaitingPromptPolicy twoPresses;
    InputTriggerPolicy specialKey(InputDetectionRound::InitialLock);
    Check(!specialKey.ObserveKey('L', false), "ambiguous initial L still requires another press");
    Check(!twoPresses.Update(5000, true, false) && twoPresses.Update(5300, true, false),
          "drained L gets a hint only after the credential view is quiet");
}

static void MouseSelectionCancellation() {
    using facelogin::credential_provider::InputTriggerKind;
    auto credential = Make<LockFaceCredential>();
    credential->Advise(nullptr);
    auto generation = Access::PrepareInput(*credential.Get());
    Event cancelled;
    cancelled.Set();
    Access::InputCandidate(*credential.Get(), cancelled.handle, generation,
                           InputTriggerKind::MouseButton);
    Check(Access::Empty(*credential.Get()), "cancelled mouse candidate never queues input");

    Event stop, paused, resume;
    Access::Barrier(*credential.Get(), [&](int point) {
        if (point == 4) { paused.Set(); Wait(resume.handle); }
    });
    std::thread worker([&] {
        Access::InputCandidate(*credential.Get(), stop.handle, generation,
                               InputTriggerKind::MouseButton);
    });
    Wait(paused.handle);
    // The same stop event observed by the selection wait is manual-reset:
    // cancellation before/during that wait must survive the final zero wait.
    stop.Set();
    resume.Set();
    worker.join();
    Access::Barrier(*credential.Get(), {});
    Check(Access::Empty(*credential.Get()), "mouse selection wait retains cancellation signal");

    Event active;
    Access::InputCandidate(*credential.Get(), active.handle, generation,
                           InputTriggerKind::MouseButton);
    Check(Access::HasOneInput(*credential.Get()), "selected mouse candidate queues exactly one input");
    credential->SetDeselected();
    Pump();
    Check(Access::Empty(*credential.Get()) && Access::Stopped(*credential.Get()) &&
          Access::GetState(*credential.Get()) == State::Waiting,
          "deselect after mouse publication drops queued input without starting authentication");

    credential->Advise(nullptr);
    generation = Access::PrepareInput(*credential.Get());
    Access::InputCandidate(*credential.Get(), active.handle, generation,
                           InputTriggerKind::Keyboard);
    Check(Access::HasOneInput(*credential.Get()), "keyboard candidate retains direct input publication");
    credential->UnAdvise();
    Pump();
    std::cout << "PASS mouse selection cancellation and queued-input teardown\n";
}

int main() {
    if (FAILED(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED))) return 2;
    ComFactoryOwnership();
    WaitingPromptTiming();
    MouseSelectionCancellation();
    PublicationTeardownInterleavings();
    DispatchAndReentry();
    RealWorker(false);
    RealWorker(true);
    RealWorker(false, true);
    WatcherTeardown();
    Check(DllCanUnloadNow() == S_OK, "test suite leaves no COM instance references");
    CoUninitialize();
    std::cout << failures << " failures\n";
    return failures ? 1 : 0;
}
