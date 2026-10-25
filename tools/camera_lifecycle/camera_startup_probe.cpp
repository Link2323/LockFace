// Desktop-only, read-only startup comparison. No images or controls are saved.
// Run each sample in a fresh process, alternating immediate/after-frame externally.
#include "webcam_capture_dshow.h"
#include "exposure_warmup.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <string>
#include <thread>
#include <mmsystem.h>

using namespace facelogin;
using Clock = std::chrono::steady_clock;

int wmain(int argc, wchar_t** argv) {
    if (argc != 5 || std::wstring(argv[1]) != L"--device" || !argv[2][0] ||
        std::wstring(argv[3]) != L"--query" ||
        (std::wstring(argv[4]) != L"immediate" && std::wstring(argv[4]) != L"after-frame")) {
        std::fprintf(stderr, "Usage: CameraStartupProbe --device PATH --query immediate|after-frame\n");
        return 2;
    }
    const bool defer = std::wstring(argv[4]) == L"after-frame";
    if (!WebcamCaptureDS::InitializeCOM()) return 1;
    struct ComScope { ~ComScope() { WebcamCaptureDS::ShutdownCOM(); } } com;
    struct TimerScope {
        bool active = timeBeginPeriod(1) == TIMERR_NOERROR;
        ~TimerScope() { if (active) timeEndPeriod(1); }
    } timer;
    WebcamCaptureDS camera;
    if (!camera.Preload(argv[2])) return 1;
    const auto start = Clock::now();
    if (!camera.Initialize(640, 480, argv[2])) return 1;
    const auto ready = Clock::now();
    const auto ms = [&](Clock::time_point at) {
        return std::chrono::duration<double, std::milli>(at - ready).count();
    };

    // Only copies the callback's memory buffer. All COM/control calls remain
    // on the camera owner thread; join before graph destruction on every path.
    std::atomic<bool> firstAvailable{false};
    double firstMs = -1.0, stableMs = -1.0;
    unsigned long long firstSeq = 0;
    std::thread observer([&] {
        ExposureWarmup warmup;
        unsigned long long lastSeq = 0;
        while (Clock::now() - ready < std::chrono::seconds(5)) {
            FrameImage frame;
            unsigned long long seq = 0;
            if (camera.GrabFrame(frame, &seq) && seq > lastSeq) {
                lastSeq = seq;
                if (firstMs < 0.0) {
                    firstMs = ms(Clock::now());
                    firstSeq = seq;
                    firstAvailable.store(true);
                }
                if (warmup.Feed(MeanLuma(frame))) {
                    if (warmup.Settled()) stableMs = ms(Clock::now());
                    break;
                }
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    });
    if (defer) {
        while (!firstAvailable.load() && Clock::now() - ready < std::chrono::seconds(5))
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    long value = 0;
    bool manual = false;
    const auto queryStart = Clock::now();
    const bool queryOk = camera.GetExposure(&value, &manual);
    const auto queryDone = Clock::now();
    observer.join();
    const bool ok = queryOk && firstMs >= 0.0 && stableMs >= 0.0;
    std::printf("{\"mode\":\"%s\",\"ok\":%s,\"activate_ms\":%.3f,"
                "\"query_start_ms\":%.3f,\"query_done_ms\":%.3f,"
                "\"query_ms\":%.3f,\"first_ms\":%.3f,\"first_seq\":%llu,"
                "\"stable_ms\":%.3f,\"ready_gate_ms\":%.3f}\n",
                defer ? "after-frame" : "immediate", ok ? "true" : "false",
                std::chrono::duration<double, std::milli>(ready - start).count(),
                ms(queryStart), ms(queryDone),
                std::chrono::duration<double, std::milli>(queryDone - queryStart).count(),
                firstMs, firstSeq, stableMs, std::max(stableMs, ms(queryDone)));
    return ok ? 0 : 1;
}
