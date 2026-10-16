#pragma once

#include <QString>
#include <QStringList>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

// KWin (Plasma Wayland) can lose direct scanout of a fullscreen stream without
// any visible sign, after which every frame is composited: about 5 ms more
// submit-to-screen latency and a full-screen GPU pass that competes with
// decoding. Two such states were seen after a TV hotplug with KWin 6.4.3:
//
//  - A short window animation effect (e.g. fadingpopups) stays active forever,
//    because its timeline only advances when its window is painted. Any active
//    effect that does not opt out blocks direct scanout on every output.
//  - The atomic test for moving the HDR gamma 2.2 -> PQ conversion into the
//    CRTC GAMMA_LUT fails, so KWin falls back to a shadow buffer until some
//    output setting changes.
//
// While a stream runs, this guard reloads such a stuck effect over D-Bus and
// asks KWin to apply its current output configuration (legacy KWin receives
// only its unchanged SDR reference value). Recovery needs two stable fullscreen fallback observations
// and is attempted at most once per stream. Every action is logged.
//
// Everything here works in the Flatpak (wayland socket, /dev/dri, and
// --talk-name=org.kde.KWin) and the AppImage, which links neither
// libwayland-client nor libdrm: libwayland-client is loaded at runtime and KMS
// state is read with plain ioctls.
class KWinScanoutGuard
{
public:
    // Returns nullptr outside Wayland sessions. On a compositor other than
    // KWin the guard's thread exits after one registry query.
    // MOONLIGHT_KWIN_SCANOUT_GUARD=0 disables the guard.
    static std::unique_ptr<KWinScanoutGuard> start(bool fullscreen);
    ~KWinScanoutGuard();
    KWinScanoutGuard(const KWinScanoutGuard&) = delete;
    KWinScanoutGuard& operator=(const KWinScanoutGuard&) = delete;

    // Direct scanout only applies to a fullscreen window, so stuck effects are
    // left alone while the stream is windowed.
    void setFullscreen(bool fullscreen) { m_Fullscreen = fullscreen; }

    // Tracks how long each short animation effect has been continuously
    // active and reports the ones that are stuck.
    class StuckEffects
    {
    public:
        using Clock = std::chrono::steady_clock;
        static constexpr std::chrono::seconds k_StuckAfter{6};
        static constexpr int k_MaxReloadsPerEffect = 3;

        static bool isTransientAnimation(const QString& effect);
        // Returns the effects to reload now. A reported effect counts as
        // reloaded and starts a new observation.
        QStringList update(Clock::time_point now, const QStringList& activeEffects);
        void reset() { m_FirstSeen.clear(); }

    private:
        std::map<QString, Clock::time_point> m_FirstSeen;
        std::map<QString, int> m_Reloads;
    };

    enum class HdrOffload {
        Unknown, // No readable KMS device, or the state is not visible.
        Ok,
        // An active HDR connector's CRTC has no GAMMA_LUT: KWin composites
        // through a shadow buffer (or prefers color accuracy).
        ShadowBuffer,
    };
    // shadowOutputs receives the affected connector names, e.g. "DP-1".
    static HdrOffload probeHdrOffload(QStringList* shadowOutputs = nullptr);

    struct HdrRetryResult {
        bool available = false; // KWin output management was reachable.
        QStringList retried;    // Outputs named by an accepted unchanged apply.
        QStringList skipped;    // Outputs left alone, with the reason.
        bool applied = false;
        QString failure;
    };
    // Requests an unchanged output-configuration apply, after checking the
    // named enabled HDR outputs prefer efficiency. Registry-based KWin needs
    // no setting setters; legacy KWin receives only the current SDR reference.
    // Acceptance is not proof that hardware color offload recovered.
    // An empty outputs list means every output. connectedFd (taken over) is
    // for tests; -1 connects to WAYLAND_DISPLAY.
    static HdrRetryResult retryHdrColorOffload(const QStringList& outputs = {}, int connectedFd = -1);

    class HdrRecovery
    {
    public:
        using Clock = std::chrono::steady_clock;
        static constexpr std::chrono::seconds k_StableFor{2};
        // An issued attempt consumes the stream budget even if KWin rejects
        // it or the reply is lost. Healthy/unknown/hotplug/windowed states
        // cannot start a repeated configuration-apply loop.
        QStringList update(Clock::time_point now, HdrOffload state, QStringList outputs, bool fullscreen);
        bool attempted() const { return m_Attempted; }
    private:
        QStringList m_Pending;
        Clock::time_point m_FirstSeen{};
        bool m_Attempted = false;
    };

private:
    static HdrOffload probeHdrOffload(QStringList* shadowOutputs, QStringList* activeHdrOutputs);
    explicit KWinScanoutGuard(bool fullscreen);
    void run();
    bool waitFor(std::chrono::steady_clock::duration duration);

    std::atomic<bool> m_Fullscreen;
    std::mutex m_Mutex;
    std::condition_variable m_Wake;
    bool m_Stop = false;
    std::thread m_Thread;
};
