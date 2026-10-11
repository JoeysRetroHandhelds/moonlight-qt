#include "streaming/kwinscanoutguard.h"
#include "protocols/kde-output-device-v2-server-protocol.h"
#include "protocols/kde-output-management-v2-server-protocol.h"
#include <wayland-server.h>
#include <sys/socket.h>
#include <unistd.h>
#include <QCoreApplication>
#include <cassert>
#include <atomic>
#include <chrono>
#include <string>
#include <thread>
#include <vector>

using namespace std::chrono_literals;

struct FakeOutput {
    std::string name;
    bool enabled;
    bool hdr;
    uint32_t sdrBrightness;
    uint32_t colorPowerTradeoff;
};

// An isolated stand-in for KWin's output management, never the user's
// desktop compositor. Only the requests the guard may send are implemented;
// any other one would call a null handler and crash the test.
struct FakeKWin {
    wl_display* display = wl_display_create();
    std::vector<FakeOutput> outputs;
    std::vector<std::string> sdrRequests; // "DP-1=300"
    int applies = 0;
    const char* rejectWith = nullptr;
    std::atomic<bool> stop{false};
    std::thread thread;

    explicit FakeKWin(std::vector<FakeOutput> fakeOutputs, bool advertiseManagement = true, uint32_t deviceVersion = 16)
        : outputs(std::move(fakeOutputs))
    {
        for (FakeOutput& output : outputs)
            wl_global_create(display, &kde_output_device_v2_interface, deviceVersion, &output, bindDevice);
        if (advertiseManagement)
            wl_global_create(display, &kde_output_management_v2_interface, 16, this, bindManagement);
    }

    static void bindDevice(wl_client* client, void* data, uint32_t version, uint32_t id) {
        auto* output = static_cast<FakeOutput*>(data);
        wl_resource* device = wl_resource_create(client, &kde_output_device_v2_interface, version, id);
        wl_resource_set_implementation(device, nullptr, output, nullptr);
        // KWin announces modes as server-created objects.
        wl_resource* mode = wl_resource_create(client, &kde_output_device_mode_v2_interface, 1, 0);
        kde_output_device_v2_send_mode(device, mode);
        kde_output_device_mode_v2_send_size(mode, 3840, 2160);
        kde_output_device_v2_send_current_mode(device, mode);
        kde_output_device_v2_send_name(device, output->name.c_str());
        kde_output_device_v2_send_enabled(device, output->enabled);
        kde_output_device_v2_send_high_dynamic_range(device, output->hdr);
        kde_output_device_v2_send_sdr_brightness(device, output->sdrBrightness);
        if (version >= KDE_OUTPUT_DEVICE_V2_COLOR_POWER_TRADEOFF_SINCE_VERSION)
            kde_output_device_v2_send_color_power_tradeoff(device, output->colorPowerTradeoff);
        kde_output_device_v2_send_done(device);
    }

    static void bindManagement(wl_client* client, void* data, uint32_t version, uint32_t id) {
        static const struct kde_output_configuration_v2_interface configuration = [] {
            struct kde_output_configuration_v2_interface c{};
            c.set_sdr_brightness = [](wl_client*, wl_resource* r, wl_resource* device, uint32_t nits) {
                auto* kwin = static_cast<FakeKWin*>(wl_resource_get_user_data(r));
                auto* output = static_cast<FakeOutput*>(wl_resource_get_user_data(device));
                kwin->sdrRequests.push_back(output->name + "=" + std::to_string(nits));
            };
            c.apply = [](wl_client*, wl_resource* r) {
                auto* kwin = static_cast<FakeKWin*>(wl_resource_get_user_data(r));
                ++kwin->applies;
                if (!kwin->rejectWith) {
                    kde_output_configuration_v2_send_applied(r);
                    return;
                }
                kde_output_configuration_v2_send_failure_reason(r, kwin->rejectWith);
                kde_output_configuration_v2_send_failed(r);
            };
            c.destroy = [](wl_client*, wl_resource* r) { wl_resource_destroy(r); };
            return c;
        }();
        static const struct kde_output_management_v2_interface management{
            [](wl_client* client, wl_resource* manager, uint32_t id) {
                wl_resource* r = wl_resource_create(client, &kde_output_configuration_v2_interface,
                                                    wl_resource_get_version(manager), id);
                wl_resource_set_implementation(r, &configuration, wl_resource_get_user_data(manager), nullptr);
            }
        };
        wl_resource* r = wl_resource_create(client, &kde_output_management_v2_interface, version, id);
        wl_resource_set_implementation(r, &management, data, nullptr);
    }

    int connect() {
        int fd[2];
        assert(socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, fd) == 0);
        assert(wl_client_create(display, fd[0]));
        thread = std::thread([this] {
            while (!stop) {
                wl_event_loop_dispatch(wl_display_get_event_loop(display), 5);
                wl_display_flush_clients(display);
            }
        });
        return fd[1];
    }
    // Stops serving so the recorded requests can be read.
    void finish() {
        stop = true;
        if (thread.joinable()) thread.join();
    }
    ~FakeKWin() {
        finish();
        wl_display_destroy_clients(display);
        wl_display_destroy(display);
    }
};

const FakeOutput k_HdrTv{"DP-1", true, true, 300, KDE_OUTPUT_DEVICE_V2_COLOR_POWER_TRADEOFF_EFFICIENCY};
const FakeOutput k_AccurateHdr{"HDMI-A-1", true, true, 203, KDE_OUTPUT_DEVICE_V2_COLOR_POWER_TRADEOFF_ACCURACY};
const FakeOutput k_SdrMonitor{"DP-2", true, false, 0, KDE_OUTPUT_DEVICE_V2_COLOR_POWER_TRADEOFF_EFFICIENCY};
const FakeOutput k_DisabledPanel{"eDP-1", false, true, 400, KDE_OUTPUT_DEVICE_V2_COLOR_POWER_TRADEOFF_EFFICIENCY};

void testHdrRetry()
{
    {
        // Only the enabled HDR output that prefers efficiency is re-applied,
        // with its current SDR brightness, in a single configuration.
        FakeKWin kwin({k_HdrTv, k_AccurateHdr, k_SdrMonitor, k_DisabledPanel});
        const auto result = KWinScanoutGuard::retryHdrColorOffload({}, kwin.connect());
        kwin.finish();
        assert(result.available && result.applied && result.failure.isEmpty());
        assert(result.retried == QStringList{"DP-1 at 300 nits"});
        assert(result.skipped == (QStringList{"HDMI-A-1 (prefers color accuracy)", "DP-2 (SDR)"}));
        assert(kwin.sdrRequests == std::vector<std::string>{"DP-1=300"});
        assert(kwin.applies == 1);
    }
    {
        // Outputs not named by the KMS probe are left untouched, and nothing
        // is applied when no named output may be retried.
        FakeKWin kwin({k_HdrTv, k_SdrMonitor});
        const auto result = KWinScanoutGuard::retryHdrColorOffload({"DP-2"}, kwin.connect());
        kwin.finish();
        assert(result.available && !result.applied && result.retried.isEmpty());
        assert(result.skipped == QStringList{"DP-2 (SDR)"});
        assert(kwin.sdrRequests.empty() && kwin.applies == 0);
    }
    {
        // KWin's failure reason is reported.
        FakeKWin kwin({k_HdrTv});
        kwin.rejectWith = "test rejection";
        const auto result = KWinScanoutGuard::retryHdrColorOffload({"DP-1"}, kwin.connect());
        kwin.finish();
        assert(result.available && !result.applied && result.failure == "test rejection");
        assert(kwin.applies == 1);
    }
    {
        // Before kde_output_device_v2 version 10 there is no color/power
        // preference, so an HDR output is retried.
        FakeKWin kwin({k_HdrTv}, true, 9);
        const auto result = KWinScanoutGuard::retryHdrColorOffload({}, kwin.connect());
        kwin.finish();
        assert(result.applied && result.retried == QStringList{"DP-1 at 300 nits"});
    }
    {
        // A compositor without KWin's output management.
        FakeKWin kwin({}, false);
        const auto result = KWinScanoutGuard::retryHdrColorOffload({}, kwin.connect());
        assert(!result.available && result.failure.contains("kde_output_management_v2"));
    }
    {
        // A compositor that never answers must not block the guard's thread.
        int fd[2];
        assert(socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, fd) == 0);
        const auto start = std::chrono::steady_clock::now();
        const auto result = KWinScanoutGuard::retryHdrColorOffload({}, fd[1]);
        assert(std::chrono::steady_clock::now() - start < 5s);
        assert(!result.available && !result.failure.isEmpty());
        close(fd[0]);
    }
}

void testStuckEffects()
{
    using Clock = KWinScanoutGuard::StuckEffects::Clock;
    const Clock::time_point t0{};
    {
        KWinScanoutGuard::StuckEffects stuck;
        const QStringList active{"blur", "contrast", "fadingpopups"};
        assert(stuck.update(t0, active).isEmpty());
        assert(stuck.update(t0 + 4s, active).isEmpty());
        assert(stuck.update(t0 + 6s, active) == QStringList{"fadingpopups"});
        // A reload starts a new observation.
        assert(stuck.update(t0 + 8s, active).isEmpty());
        assert(stuck.update(t0 + 12s, active) == QStringList{"fadingpopups"});
    }
    {
        // Separate short animations are not one stuck effect.
        KWinScanoutGuard::StuckEffects stuck;
        assert(stuck.update(t0, {"slide"}).isEmpty());
        assert(stuck.update(t0 + 2s, {}).isEmpty());
        assert(stuck.update(t0 + 4s, {"slide"}).isEmpty());
        assert(stuck.update(t0 + 9s, {"slide"}).isEmpty());
        assert(stuck.update(t0 + 10s, {"slide"}) == QStringList{"slide"});
    }
    {
        // Effects that are active by design are never reloaded.
        KWinScanoutGuard::StuckEffects stuck;
        const QStringList persistent{"blur", "contrast", "zoom", "diminactive", "kscreen", "colorblindnesscorrection"};
        for (int s = 0; s <= 60; s += 2)
            assert(stuck.update(t0 + std::chrono::seconds(s), persistent).isEmpty());
    }
    {
        // An effect that keeps coming back is reloaded at most three times.
        KWinScanoutGuard::StuckEffects stuck;
        int reloads = 0;
        for (int s = 0; s <= 120; s += 2)
            reloads += stuck.update(t0 + std::chrono::seconds(s), {"fade"}).size();
        assert(reloads == KWinScanoutGuard::StuckEffects::k_MaxReloadsPerEffect);
    }
    {
        // Leaving fullscreen forgets what was seen.
        KWinScanoutGuard::StuckEffects stuck;
        assert(stuck.update(t0, {"scale"}).isEmpty());
        stuck.reset();
        assert(stuck.update(t0 + 6s, {"scale"}).isEmpty());
        assert(stuck.update(t0 + 12s, {"scale"}) == QStringList{"scale"});
    }
}

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);
    testHdrRetry();
    testStuckEffects();
    // The KMS probe only reads; on a machine without a readable card it says so.
    const auto hdr = KWinScanoutGuard::probeHdrOffload();
    assert(hdr == KWinScanoutGuard::HdrOffload::Ok || hdr == KWinScanoutGuard::HdrOffload::ShadowBuffer ||
           hdr == KWinScanoutGuard::HdrOffload::Unknown);
    return 0;
}
