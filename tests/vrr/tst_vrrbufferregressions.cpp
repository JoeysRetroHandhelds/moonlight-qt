// Production-policy regression gate for the adaptive VRR buffer.
//
// Most contracts here guard a buffer regression that already reached a live
// build; the rest pin documented policy nothing else exercised live. The
// tests written when those bugs were fixed pinned the policy revision current
// at the time (input hitch policy 0-9, stable adaptation 1/2, bounded catch-up
// off), so every later default revision started with no coverage for them. Here every controller is built exactly
// as a live session builds it (vrrproductionsession.h, compared with the
// worker by tst_vrrpacingworker) and nothing pins a revision: whatever policy
// becomes the default inherits every contract the moment it is selected.
//
// Bounds come from the documented policy (release law, rise rate, profile
// allowance, retiming budgets) plus explicit slack, not from whatever the
// implementation happened to produce. Frames follow the worker's call order.
// Presented stutter is measured from the simulated submissions themselves,
// never from the controller's own miss counters.
//
// Run with scenario names to select a subset, e.g.
//   tst_vrrbufferregressions start-release rate-round-trip

#include "vrrproductionsession.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cinttypes>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <future>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

namespace {

std::atomic<int> g_Failures{0};
std::atomic<int> g_Checks{0};
bool g_Strict = false;
std::mutex g_KnownLock;

// Scenarios run concurrently; each collects its lines and main prints them
// in scenario order. The bool marks a failure line for stderr.
using Log = std::vector<std::pair<bool, std::string>>;
thread_local Log* t_Log = nullptr;

void emit(bool failure, const char* format, va_list args)
{
    char line[1024] = {};
    std::vsnprintf(line, sizeof(line), format, args);
    if (t_Log) t_Log->emplace_back(failure, line);
    else std::fputs(line, failure ? stderr : stdout);
}

void report(const char* format, ...)
{
    va_list args;
    va_start(args, format);
    emit(false, format, args);
    va_end(args);
}

void reportLine(bool failure, const char* format, ...)
{
    va_list args;
    va_start(args, format);
    emit(failure, format, args);
    va_end(args);
}

// Contracts the shipped policy violates today, each with its evidence. They
// print on every run without failing the gate (--strict fails them). Once
// none of an entry's checks fail any more, the gate fails until the entry is
// deleted, so a fix hardens the contract instead of leaving it advisory.
struct KnownRegression {
    const char* scenarioPrefix;
    const char* contracts[2];
    const char* evidence;
    size_t failed, passed;
};
KnownRegression g_Known[] = {
    {"rate-drop/", {"a rate drop gives back the moved mapping promptly", "no excess remains 10 s after a rate drop"},
     "f96b0524 reintroduced by input policy 10 (181415ec): layer decreases drain at the 250 us/s release law, so the "
     "moved mapping keeps 3-4.6 ms of extra latency for 15 s after a 116 -> 30 FPS drop. Revisions 6/8 with the "
     "rate-drop snap and revision 9's T-per-frame drain return it within about 3 s.", 0, 0},
};

KnownRegression* knownRegression(const std::string& scenario, const char* contract)
{
    for (auto& known : g_Known) {
        if (scenario.compare(0, std::strlen(known.scenarioPrefix), known.scenarioPrefix) != 0) continue;
        for (const char* listed : known.contracts)
            if (listed && std::strcmp(listed, contract) == 0) return &known;
    }
    return nullptr;
}

void check(bool ok, const std::string& scenario, const char* contract, const char* format, ...)
{
    ++g_Checks;
    KnownRegression* known = knownRegression(scenario, contract);
    if (known) {
        std::lock_guard<std::mutex> lock(g_KnownLock);
        ++(ok ? known->passed : known->failed);
    }
    if (ok) return;
    char detail[512] = {};
    va_list args;
    va_start(args, format);
    std::vsnprintf(detail, sizeof(detail), format, args);
    va_end(args);
    if (known && !g_Strict) {
        reportLine(false, "KNOWN [%s] %s: %s\n", scenario.c_str(), contract, detail);
        return;
    }
    ++g_Failures;
    reportLine(true, "FAIL [%s] %s: %s\n", scenario.c_str(), contract, detail);
}

struct Preset { int mode; const char* name; };
constexpr Preset kPresets[] = {{2, "low-latency"}, {1, "balanced"}, {0, "smooth"}};

struct Smoothing { bool smooth, absorb; const char* name; };
// The default preference is Reduce judder on, Absorb off.
constexpr Smoothing kSmoothing[] = {
    {false, false, "smoothing-off"}, {true, false, "reduce-judder"}, {true, true, "absorb"}};

std::string label(const char* scenario, const Preset& preset, const Smoothing& smoothing)
{
    return std::string(scenario) + "/" + preset.name + "/" + smoothing.name;
}

// xorshift64*: identical sequences on every standard library.
struct Rng {
    uint64_t state;
    explicit Rng(uint64_t seed) : state(seed * 0x9E3779B97F4A7C15ULL + 0x632BE59BD9B4E019ULL) {}
    uint64_t next()
    {
        state ^= state >> 12;
        state ^= state << 25;
        state ^= state >> 27;
        return state * 2685821657736338717ULL;
    }
    uint64_t below(uint64_t n) { return n ? next() % n : 0; }
};

constexpr uint64_t kOriginUs = 10000000;
// Network, reassembly and decode of an on-time frame. The mapping absorbs it.
constexpr uint64_t kBaseLatencyUs = 1000;
constexpr uint32_t k116Ticks = 776; // 90 kHz RTP ticks per frame
constexpr uint32_t k30Ticks = 3000;

struct Frame {
    uint64_t sourceUs = 0, decodedUs = 0, targetUs = 0, submittedUs = 0;
    uint64_t appliedUs = 0, recoveryUs = 0, carryUs = 0, extraUs = 0;
};

struct Input {
    uint64_t latenessUs = 0;
    uint32_t lostPackets = 0;
    uint64_t prepareUs = 500;
    // Fixed wake-up/present-call delay after the target (3c32553e: 0.28 ms).
    uint64_t presentLatencyUs = 0;
    // Wait between reassembly and decode submission: client decoder backlog.
    uint64_t decoderQueueUs = 0;
};

// One production controller driven through the worker's per-frame calls.
class Stream {
public:
    explicit Stream(const VrrSessionConfig& session, uint64_t seedUs = 0, int64_t driftPpm = 0) :
        m_Session(session),
        m_Controller(session, true, vrrTimingParametersForSession(session)),
        m_DriftPpm(driftPpm)
    {
        // Same call the worker makes with the previous session's settled delay.
        if (seedUs) m_Controller.seedPlayoutDelayStart(seedUs);
    }

    const Frame& step(uint32_t ticks, const Input& input = {})
    {
        m_Ticks += ticks;
        const uint64_t elapsedUs = m_Ticks * 1000000 / 90000;
        const int64_t driftUs = int64_t(elapsedUs) * m_DriftPpm / 1000000;
        Frame f;
        f.sourceUs = uint64_t(int64_t(kOriginUs + elapsedUs) + driftUs);
        f.decodedUs = f.sourceUs + kBaseLatencyUs + input.latenessUs + input.decoderQueueUs;
        // Start near the RTP wrap so every long scenario crosses it.
        PacedFrame frame(nullptr, m_FrameNumber++, uint32_t(m_Ticks + 0xFFF00000u), true, f.decodedUs);
        // Reassembly normally hands straight to the decoder. Reassembled-to-
        // submit time is client decoder backlog, which input accounting
        // excludes by design; a constant queue would under-protect by its length.
        const uint64_t submitUs = f.decodedUs - 600;
        frame.setDeliveryTimeline(submitUs - input.decoderQueueUs - 200, submitUs - input.decoderQueueUs, submitUs);
        frame.setLostPackets(input.lostPackets);
        const uint64_t now = std::max(f.decodedUs, m_LastSubmittedUs);
        const auto d = m_Controller.schedule(frame, now);
        const uint64_t ready = std::max(now, d.renderStartUs) + input.prepareUs;
        f.targetUs = d.targetUs;
        f.submittedUs = std::max(ready, d.targetUs) + input.presentLatencyUs;
        m_Controller.notePreparationDuration(input.prepareUs, 0, ready);
        m_Controller.noteSchedulerDelays(0, 0, true);
        m_Controller.noteSubmission(true, false, f.submittedUs, f.submittedUs);
        m_LastSubmittedUs = f.submittedUs;
        f.appliedUs = d.playoutDelayUs;
        f.recoveryUs = d.recoveryAddedUs;
        f.carryUs = d.trajectory.carryUs;
        f.extraUs = d.plannedExtraDelayUs();
        m_Frames.push_back(f);
        return m_Frames.back();
    }

    void rebase() { m_Controller.rebase(); }
    const std::vector<Frame>& frames() const { return m_Frames; }
    size_t size() const { return m_Frames.size(); }
    const VrrTimingController& controller() const { return m_Controller; }
    Vrr13::InputPercentileBuffer::Stats input() const { return m_Controller.intervalStats().inputPercentile; }
    uint64_t toleranceUs() const { return uint64_t(m_Session.timingOptions.toleranceUs); }
    uint64_t windowUs() const { return uint64_t(m_Session.timingOptions.historySeconds) * 1000000; }
    // Profile allowance: the share of frames the selected smoothness may miss.
    uint64_t allowancePpm() const { return uint64_t(10000 - m_Session.timingOptions.targetHundredths) * 100; }
    uint64_t riseUsPerSecond() const { return m_Controller.parameters().playoutInputPercentileRiseUsPerSecond; }
    uint64_t fallUsPerSecond() const { return m_Controller.parameters().playoutInputPercentileFallUsPerSecond; }
    // InputPercentileBuffer's proportional release time constant.
    uint64_t releaseTauUs() const { return std::clamp<uint64_t>(windowUs() / 30, 1000000, 10000000); }
    // Absorb's fixed pad delays the whole schedule, so it covers that much
    // input lateness itself. Protection is the input buffer plus this pad.
    uint64_t padUs() const { return m_Controller.absorbReserveUs(); }

private:
    VrrSessionConfig m_Session;
    VrrTimingController m_Controller;
    int64_t m_DriftPpm = 0;
    uint64_t m_Ticks = 0, m_LastSubmittedUs = 0;
    int m_FrameNumber = 0;
    std::vector<Frame> m_Frames;
};

// Presented interval stretched beyond the tolerance relative to the source
// interval: what the viewer sees as a hitch. Catch-up compressions are not.
size_t hitches(const std::vector<Frame>& f, size_t begin, size_t end, uint64_t toleranceUs)
{
    size_t count = 0;
    for (size_t i = std::max<size_t>(begin, 1); i < std::min(end, f.size()); ++i) {
        const int64_t source = int64_t(f[i].sourceUs) - int64_t(f[i - 1].sourceUs);
        const int64_t presented = int64_t(f[i].submittedUs) - int64_t(f[i - 1].submittedUs);
        count += presented - source > int64_t(toleranceUs);
    }
    return count;
}

template<class Value>
uint64_t maximum(const std::vector<Frame>& f, size_t begin, size_t end, Value value)
{
    uint64_t result = 0;
    for (size_t i = begin; i < std::min(end, f.size()); ++i) result = std::max(result, value(f[i]));
    return result;
}

template<class Value>
uint64_t minimum(const std::vector<Frame>& f, size_t begin, size_t end, Value value)
{
    uint64_t result = UINT64_MAX;
    for (size_t i = begin; i < std::min(end, f.size()); ++i) result = std::min(result, value(f[i]));
    return result;
}

template<class Value>
uint64_t mean(const std::vector<Frame>& f, size_t begin, size_t end, Value value)
{
    uint64_t total = 0, count = 0;
    for (size_t i = begin; i < std::min(end, f.size()); ++i, ++count) total += value(f[i]);
    return count ? total / count : 0;
}

uint64_t applied(const Frame& f) { return f.appliedUs; }
uint64_t latency(const Frame& f) { return f.submittedUs - f.sourceUs; }

// First index from which the applied buffer stays at or below the limit for
// the rest of the run; f.size() when it never settles.
size_t settledAtOrBelow(const std::vector<Frame>& f, size_t begin, uint64_t limit)
{
    size_t settled = f.size();
    for (size_t i = f.size(); i-- > begin;) {
        if (f[i].appliedUs > limit) break;
        settled = i;
    }
    return settled;
}

// Largest buffer removal between consecutive frames, and over any window.
uint64_t largestDrop(const std::vector<Frame>& f, size_t begin, size_t end, size_t span = 1)
{
    uint64_t result = 0;
    for (size_t i = begin; i + span < std::min(end, f.size()); ++i)
        if (f[i].appliedUs > f[i + span].appliedUs) result = std::max(result, f[i].appliedUs - f[i + span].appliedUs);
    return result;
}

// Changes of direction in the applied buffer, ignoring movement below the
// hysteresis. A held disturbance should give a held buffer, not probes.
size_t reversals(const std::vector<Frame>& f, size_t begin, size_t end, uint64_t hysteresisUs)
{
    size_t count = 0;
    int direction = 0;
    uint64_t anchor = begin < f.size() ? f[begin].appliedUs : 0;
    for (size_t i = begin; i < std::min(end, f.size()); ++i) {
        const uint64_t value = f[i].appliedUs;
        const int now = value >= anchor + hysteresisUs ? 1 : value + hysteresisUs <= anchor ? -1 : 0;
        if (!now) continue;
        if (direction && now != direction) ++count;
        direction = now;
        anchor = value;
    }
    return count;
}

uint64_t percentile(std::vector<uint64_t> values, unsigned perMille)
{
    if (values.empty()) return 0;
    const size_t rank = std::max<size_t>(1, (values.size() * perMille + 999) / 1000);
    std::nth_element(values.begin(), values.begin() + rank - 1, values.end());
    return values[rank - 1];
}

// Change in presented interval from one pair to the next.
std::vector<uint64_t> presentedJerk(const std::vector<Frame>& f, size_t begin, size_t end)
{
    std::vector<uint64_t> jerk;
    for (size_t i = std::max<size_t>(begin, 2); i < std::min(end, f.size()); ++i) {
        const int64_t a = int64_t(f[i - 1].submittedUs) - int64_t(f[i - 2].submittedUs);
        const int64_t b = int64_t(f[i].submittedUs) - int64_t(f[i - 1].submittedUs);
        jerk.push_back(uint64_t(a > b ? a - b : b - a));
    }
    return jerk;
}

size_t frames116(double seconds) { return size_t(seconds * 90000 / k116Ticks); }
double seconds116(size_t frames) { return double(frames) * k116Ticks / 90000; }

// 1. Clean input must drain the cold-start or cached starting delay at the
// release law and then never buy delay again. Regression: Codex "paced
// attack revision 2" floated at its starting delay; 2ccca878 and 0d2f9adc
// left stale plateaus. The worker seeds each session from the previous
// session's settled delay, so a buffer that cannot leave its seed ratchets
// across sessions.
void testStartingDelayReleasesOnCleanInput()
{
    for (const auto& preset : kPresets) for (const auto& smoothing : kSmoothing) for (uint64_t seed : {uint64_t(0), uint64_t(4000)}) {
        const std::string name = label("start-release", preset, smoothing) + (seed ? "/cached-4ms" : "/cold");
        Stream s(vrrProductionSession(preset.mode, 116, 120, smoothing.smooth, smoothing.absorb), seed);
        Rng rng(11 + uint64_t(preset.mode) * 3 + seed);
        const size_t total = frames116(90);
        for (size_t i = 0; i < total; ++i) s.step(k116Ticks, {rng.below(400)});
        const auto& f = s.frames();
        const auto in = s.input();
        const uint64_t start = f[0].appliedUs, floor = in.minimumUs + 250;
        const size_t settled = settledAtOrBelow(f, 0, floor);
        const double boundSeconds = double(start > floor ? start - floor : 0) / double(s.fallUsPerSecond()) +
            3.0 * double(s.releaseTauUs()) / 1e6 + 5.0;
        const uint64_t perSecond = largestDrop(f, 0, total, frames116(1));
        const size_t visible = hitches(f, frames116(2), total, s.toleranceUs());
        report("%-52s start %5" PRIu64 " floor %5" PRIu64 " settled %5.1fs (bound %4.1fs) drop/s %4" PRIu64 " hitches %zu missed %" PRIu64 "\n",
            name.c_str(), start, floor, seconds116(settled), boundSeconds, perSecond, visible, in.missed);
        if (seed) check(start + 250 >= std::min(seed, in.maximumUs), name, "a cached starting delay is honored",
            "first frame %" PRIu64 " us for a %" PRIu64 " us seed", start, seed);
        check(settled < total && seconds116(settled) <= boundSeconds, name,
            "clean input drains the starting delay to the floor at the release law",
            "start %" PRIu64 " us reached floor+250 at %.1f s; release law allows %.1f s", start,
            settled < total ? seconds116(settled) : -1.0, boundSeconds);
        check(perSecond <= s.fallUsPerSecond() + 50, name, "release never outruns the fall rate (no visible step)",
            "dropped %" PRIu64 " us within one second; fall rate %" PRIu64 " us/s", perSecond, s.fallUsPerSecond());
        check(visible == 0 && in.missed == 0, name, "clean input causes no presented hitch and no scored miss",
            "%zu presented hitches, %" PRIu64 " scored misses", visible, in.missed);
    }
}

// 2. Misses that stay inside the selected allowance must not buy latency:
// the profile promises e.g. 99.5% smooth intervals, not zero misses.
// Regression class: buffers that "looked too high" (3c32553e, 0d2f9adc).
void testMissesWithinAllowanceBuyNoDelay()
{
    for (const auto& preset : kPresets) for (const auto& smoothing : kSmoothing) {
        const std::string name = label("within-allowance", preset, smoothing);
        Stream s(vrrProductionSession(preset.mode, 116, 120, smoothing.smooth, smoothing.absorb), 1000);
        // A quarter of each profile's allowance: 0.25%, 0.125%, 0.0125%.
        const size_t spacing = size_t(4000000 / s.allowancePpm());
        Rng rng(23 + uint64_t(preset.mode));
        const size_t total = frames116(180);
        for (size_t i = 0; i < total; ++i)
            s.step(k116Ticks, {i % spacing == spacing / 2 ? 3000 : rng.below(400)});
        const auto& f = s.frames();
        const auto in = s.input();
        const uint64_t peak = maximum(f, frames116(1), total, applied);
        report("%-52s spike every %5zu peak %5" PRIu64 " floor %5" PRIu64 " missed %" PRIu64 "/%" PRIu64 "\n",
            name.c_str(), spacing, peak, in.minimumUs, in.missed, in.eligible);
        check(peak <= in.minimumUs + 250, name, "misses inside the profile allowance never buy latency",
            "buffer reached %" PRIu64 " us (floor %" PRIu64 ") for one 3 ms spike every %zu frames",
            peak, in.minimumUs, spacing);
    }
}

// 3. Late input beyond the allowance must earn protection at the rise rate
// and then keep presented hitches inside the allowance, without buying more
// than the measured lateness. Regression: 5b062c4b (slow relearning missed
// 198 of 200 frames), and growth blocked by hitch gating.
void testGenuineLateInputEarnsProtection()
{
    for (const auto& preset : kPresets) for (const auto& smoothing : kSmoothing) {
        const std::string name = label("earn-protection", preset, smoothing);
        Stream s(vrrProductionSession(preset.mode, 116, 120, smoothing.smooth, smoothing.absorb), 1000);
        Rng rng(31 + uint64_t(preset.mode));
        const size_t total = frames116(60);
        uint64_t largest = 0;
        for (size_t i = 0; i < total; ++i) {
            const uint64_t lateness = rng.below(400) + (i % 20 == 10 ? 3000 : 0);
            largest = std::max(largest, lateness);
            s.step(k116Ticks, {lateness});
        }
        const auto& f = s.frames();
        const uint64_t pad = s.padUs(), need = 3000 - s.toleranceUs();
        size_t protectedAt = total;
        for (size_t i = 0; i < total; ++i) if (f[i].appliedUs + pad >= need - 250) { protectedAt = i; break; }
        const double boundSeconds = double(need) / double(s.riseUsPerSecond()) + 2.0;
        const size_t settledFrom = frames116(10);
        const size_t visible = hitches(f, settledFrom, total, s.toleranceUs());
        const size_t allowed = size_t((total - settledFrom) * s.allowancePpm() / 1000000) + 3;
        const uint64_t peak = maximum(f, 0, total, applied) + pad;
        report("%-52s protected %4.1fs (bound %3.1fs) hitches %3zu (allowed %3zu) peak %5" PRIu64 "\n",
            name.c_str(), protectedAt < total ? seconds116(protectedAt) : -1.0, boundSeconds, visible, allowed, peak);
        check(protectedAt < total && seconds116(protectedAt) <= boundSeconds, name,
            "late input beyond the allowance earns protection at the rise rate",
            "reached %" PRIu64 " us at %.1f s; rise rate allows %.1f s", need - 250,
            protectedAt < total ? seconds116(protectedAt) : -1.0, boundSeconds);
        check(visible <= allowed, name, "earned protection keeps presented hitches inside the profile allowance",
            "%zu hitches in %zu frames; allowance %zu", visible, total - settledFrom, allowed);
        // The tolerance is permissible excess, not standing protection.
        check(peak <= largest - s.toleranceUs() + 250, name,
            "protection never exceeds the largest lateness beyond the tolerance",
            "buffer plus pad peaked at %" PRIu64 " us for at most %" PRIu64 " us of lateness, tolerance %" PRIu64 " us",
            peak, largest, s.toleranceUs());
    }
}

// 4. Protection must hold while its evidence is inside the selected history,
// and must release once that history has expired, without oscillating while
// the disturbance continues. Regression: 2ccca878 and 0d2f9adc (stale
// plateaus), 7d1148f9 (speculative drain probes moved the buffer).
void testPlateauFollowsSelectedHistory()
{
    for (const auto& preset : kPresets) for (const auto& smoothing : kSmoothing) {
        const std::string name = label("plateau-history", preset, smoothing);
        Stream s(vrrProductionSession(preset.mode, 116, 120, smoothing.smooth, smoothing.absorb), 1000);
        Rng rng(41 + uint64_t(preset.mode));
        const size_t disturbed = frames116(30);
        const size_t total = disturbed + frames116(double(s.windowUs()) / 1e6 + 60);
        for (size_t i = 0; i < total; ++i)
            s.step(k116Ticks, {rng.below(400) + (i < disturbed && i % 20 == 10 ? 3000 : 0)});
        const auto& f = s.frames();
        const auto in = s.input();
        const uint64_t pad = s.padUs();
        const uint64_t plateau = mean(f, disturbed - frames116(5), disturbed, applied);
        const uint64_t held = minimum(f, disturbed, disturbed + frames116(double(s.windowUs()) / 2e6), applied);
        const size_t released = settledAtOrBelow(f, disturbed, in.minimumUs + 250);
        const double releaseSeconds = released < total ? seconds116(released - disturbed) : -1.0;
        const double boundSeconds = double(s.windowUs()) / 1e6 + double(plateau) / double(s.fallUsPerSecond()) +
            3.0 * double(s.releaseTauUs()) / 1e6 + 5.0;
        const size_t moves = reversals(f, frames116(10), disturbed, 200);
        report("%-52s plateau %5" PRIu64 " held %5" PRIu64 " released %5.1fs (bound %5.1fs) reversals %zu\n",
            name.c_str(), plateau + pad, held + pad, releaseSeconds, boundSeconds, moves);
        check(plateau + pad >= 2000, name, "a held disturbance establishes protection",
            "buffer plus pad %" PRIu64 " us", plateau + pad);
        check(held + 500 >= plateau, name, "protection holds while its evidence is inside the selected history",
            "fell to %" PRIu64 " us from %" PRIu64 " us within half the %" PRIu64 " s history",
            held, plateau, s.windowUs() / 1000000);
        check(released < total && releaseSeconds <= boundSeconds, name,
            "stale protection releases once its history expires",
            "floor+250 reached %.1f s after the disturbance; allowed %.1f s", releaseSeconds, boundSeconds);
        check(moves <= 2, name, "a steady disturbance holds a steady buffer (no probe/trial oscillation)",
            "%zu direction reversals over 200 us", moves);
    }
}

// 5. Leaving a rate and coming back must restore that rate's protection at
// once, never add delay at the slower rate, and never step the buffer down by
// more than the tolerance in one frame. Regression: 5b062c4b (returning to
// 110 FPS took minutes), 1a5bef09/181415ec (layer transitions stepped).
void testRateRoundTripRestoresProtection()
{
    for (const auto& preset : kPresets) for (const auto& smoothing : kSmoothing) {
        const std::string name = label("rate-round-trip", preset, smoothing);
        Stream s(vrrProductionSession(preset.mode, 116, 120, smoothing.smooth, smoothing.absorb), 1000);
        Rng rng(53 + uint64_t(preset.mode));
        const auto fast = [&](size_t count) {
            for (size_t i = 0; i < count; ++i)
                s.step(k116Ticks, {rng.below(400) + (s.size() % 20 == 10 ? 3000 : 0)});
        };
        fast(frames116(40));
        const size_t slowBegin = s.size();
        const uint64_t learned = s.frames().back().appliedUs;
        for (size_t i = 0; i < 600; ++i) s.step(k30Ticks, {rng.below(400)});
        const size_t returnBegin = s.size();
        fast(frames116(20));
        const auto& f = s.frames();
        const uint64_t pad = s.padUs();
        const uint64_t slowPeak = maximum(f, slowBegin, returnBegin, applied);
        const uint64_t slowStep = largestDrop(f, slowBegin, returnBegin + 1);
        const uint64_t restored = f[returnBegin + 10].appliedUs;
        // Twice the allowance: a 20 s window is short for a long-history goal.
        // Relearning through misses (5b062c4b: 198 of 200) is far outside it.
        const size_t returnFrames = s.size() - returnBegin;
        const size_t visible = hitches(f, returnBegin + 1, s.size(), s.toleranceUs());
        const size_t allowed = size_t(2 * returnFrames * s.allowancePpm() / 1000000) + 3;
        report("%-52s learned %5" PRIu64 " slow peak %5" PRIu64 " step %4" PRIu64 " restored %5" PRIu64 " return hitches %zu (allowed %zu)\n",
            name.c_str(), learned + pad, slowPeak + pad, slowStep, restored + pad, visible, allowed);
        check(learned + pad >= 2000, name, "the fast rate establishes protection",
            "buffer plus pad %" PRIu64 " us", learned + pad);
        check(slowPeak <= learned + 250, name, "a slower rate never adds standing delay",
            "30 FPS buffer peaked at %" PRIu64 " us after %" PRIu64 " us at 116 FPS", slowPeak, learned);
        check(slowStep <= s.toleranceUs(), name, "a layer change never removes more than T in one frame",
            "single-frame drop %" PRIu64 " us, tolerance %" PRIu64 " us", slowStep, s.toleranceUs());
        check(restored + 250 >= learned, name, "returning to a rate seen before restores its protection at once",
            "%" PRIu64 " us ten frames after return; that rate had learned %" PRIu64 " us", restored, learned);
        check(visible <= allowed, name, "the return is covered without relearning through hitches",
            "%zu hitches in the 20 s back; allowed %zu", visible, allowed);

        // The same trip returning to clean input: the layer's earlier evidence
        // is still inside its history, so its protection must hold instead of
        // releasing as if the layer had been relearned from the clean frames.
        Stream clean(vrrProductionSession(preset.mode, 116, 120, smoothing.smooth, smoothing.absorb), 1000);
        Rng cleanRng(59 + uint64_t(preset.mode));
        for (size_t i = 0; i < frames116(40); ++i)
            clean.step(k116Ticks, {cleanRng.below(400) + (i % 20 == 10 ? 3000 : 0)});
        const uint64_t cleanLearned = clean.frames().back().appliedUs;
        for (size_t i = 0; i < 600; ++i) clean.step(k30Ticks, {cleanRng.below(400)});
        const size_t cleanReturn = clean.size();
        const size_t holdFrames = frames116(double(clean.windowUs()) / 4e6);
        for (size_t i = 0; i < holdFrames; ++i) clean.step(k116Ticks, {cleanRng.below(400)});
        const uint64_t held = minimum(clean.frames(), cleanReturn + frames116(1), clean.size(), applied);
        report("%-52s clean return holds %5" PRIu64 " of %5" PRIu64 " for %.0f s\n", name.c_str(),
            held + pad, cleanLearned + pad, seconds116(holdFrames));
        check(held + 500 >= cleanLearned, name, "a returning rate keeps its evidence for the rest of its history",
            "fell to %" PRIu64 " us from %" PRIu64 " us within %.0f s of a clean return; the %" PRIu64 " s history still holds its misses",
            held, cleanLearned, seconds116(holdFrames), clean.windowUs() / 1000000);
    }
}

// 6. An abrupt rate drop that moves the source mapping later must not leave
// the old protection standing on top of it. 116 FPS with one frame in ten
// arriving E early holds the mapping minimum on the early frames, so the
// buffer covers E. At 30 FPS the early frames stop: the mapping slews E later
// and the evidence needs E less buffer. Regression: f96b0524 (about 6 ms of
// excess latency for 25 s after a 105 -> 30 FPS drop).
void testRateDropReleasesMappingExcess()
{
    for (const auto& preset : kPresets) for (const auto& smoothing : kSmoothing) {
        const std::string name = label("rate-drop", preset, smoothing);
        const auto session = vrrProductionSession(preset.mode, 116, 120, smoothing.smooth, smoothing.absorb);
        // The largest early offset each profile's buffer ceiling can protect.
        const uint64_t early = preset.mode == 2 ? 3000 : preset.mode == 1 ? 6000 : 8000;
        Stream s(session, 1000);
        Rng rng(61 + uint64_t(preset.mode));
        for (size_t i = 0; i < 3000; ++i) s.step(k116Ticks, {i % 10 == 0 ? 0 : early + rng.below(600)});
        const size_t drop = s.size();
        const uint64_t protection = s.frames().back().appliedUs + s.padUs();
        for (size_t i = 0; i < 450; ++i) s.step(k30Ticks, {early + rng.below(600)});
        // The same 30 FPS input from a session that never ran faster.
        Stream reference(session, 1000);
        Rng referenceRng(67 + uint64_t(preset.mode));
        for (size_t i = 0; i < 1050; ++i) reference.step(k30Ticks, {early + referenceRng.below(600)});
        const auto age = [](const Frame& f) { return f.submittedUs - f.decodedUs; };
        const auto& f = s.frames();
        const uint64_t settledAge = mean(reference.frames(), 600, 1050, age);
        const uint64_t firstAge = mean(f, drop, drop + 450, age);
        const uint64_t lateAge = mean(f, drop + 300, drop + 450, age);
        const uint64_t firstExcess = firstAge > settledAge ? firstAge - settledAge : 0;
        const uint64_t lateExcess = lateAge > settledAge ? lateAge - settledAge : 0;
        report("%-52s protection %5" PRIu64 " decode-to-present after drop %5" PRIu64 " (0-15 s) %5" PRIu64 " (10-15 s) settled %5" PRIu64 "\n",
            name.c_str(), protection, firstAge, lateAge, settledAge);
        check(protection + 1000 >= early, name, "the fast rate protects the late majority",
            "buffer plus pad %" PRIu64 " us for %" PRIu64 " us of lateness", protection, early);
        // Following the target (f96b0524) clears the excess within about 3 s,
        // well inside these bounds; draining it at the release law does not.
        check(firstExcess <= 1000, name, "a rate drop gives back the moved mapping promptly",
            "decode-to-present averaged %" PRIu64 " us over the 15 s after the drop; a fresh 30 FPS session settles at %" PRIu64 " us",
            firstAge, settledAge);
        check(lateExcess <= 500, name, "no excess remains 10 s after a rate drop",
            "%" PRIu64 " us excess 10-15 s after the drop", lateExcess);
    }
}

// 7. Host/client clock drift must neither ratchet retained protection up nor
// erode it. Regression: 409a5661 (drift x age moved every retained
// requirement; a slow client clock ratcheted the buffer, a fast one eroded it).
void testClockDriftNeitherRatchetsNorErodes()
{
    for (const auto& smoothing : kSmoothing) {
        const Preset& preset = kPresets[1];
        struct Run { uint64_t meanUs, trendUs; size_t hitches; uint64_t padUs; };
        const auto run = [&](int64_t ppm) {
            Stream s(vrrProductionSession(preset.mode, 116, 120, smoothing.smooth, smoothing.absorb), 1000, ppm);
            Rng rng(71);
            const size_t total = frames116(240);
            // One frame in 50 is 3 ms late, beyond the allowance, so real
            // protection is retained while the clocks drift 12 ms apart.
            for (size_t i = 0; i < total; ++i)
                s.step(k116Ticks, {rng.below(600) + (i % 50 == 25 ? 3000 : 0)});
            const auto& f = s.frames();
            const uint64_t early = mean(f, frames116(120), frames116(160), applied);
            const uint64_t late = mean(f, frames116(200), total, applied);
            return Run{mean(f, frames116(120), total, applied), late > early ? late - early : early - late,
                       hitches(f, frames116(60), total, s.toleranceUs()), s.padUs()};
        };
        const Run control = run(0);
        check(control.meanUs + control.padUs >= 2000, label("clock-drift", preset, smoothing),
            "the drift fixture retains real protection", "mean buffer plus pad %" PRIu64 " us",
            control.meanUs + control.padUs);
        for (int64_t ppm : {int64_t(50), int64_t(-50)}) {
            const std::string name = label("clock-drift", preset, smoothing) + (ppm > 0 ? "/+50ppm" : "/-50ppm");
            const Run drifted = run(ppm);
            const uint64_t difference = drifted.meanUs > control.meanUs ? drifted.meanUs - control.meanUs :
                control.meanUs - drifted.meanUs;
            report("%-52s mean %5" PRIu64 " (no drift %5" PRIu64 ") trend %4" PRIu64 " hitches %3zu (no drift %3zu)\n",
                name.c_str(), drifted.meanUs, control.meanUs, drifted.trendUs, drifted.hitches, control.hitches);
            // The original defect moved retained requirements by drift x age:
            // about 3 ms here. Ordinary mapping catch-up is a small fraction.
            check(difference <= std::max<uint64_t>(300, control.meanUs / 8), name,
                "clock drift neither ratchets nor erodes retained protection",
                "mean buffer %" PRIu64 " us with drift, %" PRIu64 " us without", drifted.meanUs, control.meanUs);
            check(drifted.trendUs <= 300, name, "retained protection does not trend with drift",
                "mean buffer moved %" PRIu64 " us between minutes 2-3 and 3.3-4", drifted.trendUs);
            check(drifted.hitches <= control.hitches * 2 + 10, name, "clock drift does not erode coverage",
                "%zu presented hitches with drift, %zu without", drifted.hitches, control.hitches);
        }
    }
}

// 8. Late frames that lost packets, slow preparation after input, and time
// spent in the client's own decoder queue must not buy input buffer.
// Regression: 39d10abf (loss must cost detail, not timing), post-input
// render/scheduler work relabeled as input misses, and the buffer growing
// from its own backlog.
void testLossAndDownstreamWorkBuyNoInputDelay()
{
    for (const auto& preset : kPresets) for (const auto& smoothing : kSmoothing) {
        const auto session = vrrProductionSession(preset.mode, 116, 120, smoothing.smooth, smoothing.absorb);
        const size_t total = frames116(60);
        {
            const std::string name = label("lossy-late-frames", preset, smoothing);
            Stream s(session, 1000);
            Rng rng(83 + uint64_t(preset.mode));
            for (size_t i = 0; i < total; ++i) {
                const bool lossy = i % 20 == 10;
                s.step(k116Ticks, {rng.below(400) + (lossy ? 3000 : 0), lossy ? 4u : 0u});
            }
            const auto in = s.input();
            const uint64_t peak = maximum(s.frames(), frames116(1), total, applied);
            report("%-52s peak %5" PRIu64 " floor %5" PRIu64 " missed %" PRIu64 "\n", name.c_str(), peak, in.minimumUs, in.missed);
            check(peak <= in.minimumUs + 250 && in.missed == 0, name,
                "late frames with lost packets never buy input buffer",
                "buffer peaked at %" PRIu64 " us (floor %" PRIu64 "), %" PRIu64 " scored misses", peak, in.minimumUs, in.missed);
        }
        {
            const std::string name = label("slow-preparation", preset, smoothing);
            Stream s(session, 1000);
            Rng rng(89 + uint64_t(preset.mode));
            for (size_t i = 0; i < total; ++i) {
                Input input{rng.below(400)};
                input.prepareUs = 2000 + (i % 20 == 10 ? 3000 : 0);
                input.presentLatencyUs = 300;
                s.step(k116Ticks, input);
            }
            const auto in = s.input();
            const uint64_t peak = maximum(s.frames(), frames116(1), total, applied);
            report("%-52s peak %5" PRIu64 " floor %5" PRIu64 " missed %" PRIu64 "\n", name.c_str(), peak, in.minimumUs, in.missed);
            check(peak <= in.minimumUs + 250 && in.missed == 0, name,
                "preparation and present work after input never buy input buffer",
                "buffer peaked at %" PRIu64 " us (floor %" PRIu64 "), %" PRIu64 " scored misses", peak, in.minimumUs, in.missed);
        }
        {
            const std::string name = label("decoder-backlog", preset, smoothing);
            Stream s(session, 1000);
            Rng rng(79 + uint64_t(preset.mode));
            for (size_t i = 0; i < total; ++i) {
                Input input{rng.below(400)};
                input.decoderQueueUs = i % 20 == 10 ? 3000 : 0;
                s.step(k116Ticks, input);
            }
            const auto in = s.input();
            const uint64_t peak = maximum(s.frames(), frames116(1), total, applied);
            report("%-52s peak %5" PRIu64 " floor %5" PRIu64 " missed %" PRIu64 "\n", name.c_str(), peak, in.minimumUs, in.missed);
            check(peak <= in.minimumUs + 250 && in.missed == 0, name,
                "time in the client's decoder queue never buys input buffer",
                "buffer peaked at %" PRIu64 " us (floor %" PRIu64 "), %" PRIu64 " scored misses", peak, in.minimumUs, in.missed);
        }
    }
}

// 9. Between late frames the schedule must return to its plan: catch-up
// carry must drain despite a normal wake-up overshoot on every frame.
// Regression: 3c32553e (the VRR17 floor re-armed under bounded catch-up and
// carried 4-8 ms on top of the buffer; 81% of frames were pushed past plan).
void testCatchUpCarryDrainsBetweenLateFrames()
{
    // Low Latency cannot protect a 6 ms spike (ceiling half a frame), and
    // one spike in 400 is inside Balanced's allowance: neither grows the
    // buffer, so recovery alone must absorb every spike.
    for (const auto& smoothing : kSmoothing) for (int index : {0, 1}) {
        const Preset& preset = kPresets[index];
        const std::string name = label("catch-up-carry", preset, smoothing);
        Stream s(vrrProductionSession(preset.mode, 116, 120, smoothing.smooth, smoothing.absorb), 1000);
        const size_t spacing = index == 0 ? 200 : 400;
        Rng rng(97 + uint64_t(index));
        const size_t total = frames116(60);
        for (size_t i = 0; i < total; ++i) {
            Input input{rng.below(300) + (i % spacing == spacing / 2 ? 6000 : 0)};
            input.presentLatencyUs = 280;
            s.step(k116Ticks, input);
        }
        const auto& f = s.frames();
        std::vector<uint64_t> carry, recovery;
        size_t tail = 0, tailPushed = 0;
        for (size_t i = frames116(10); i < total; ++i) {
            carry.push_back(f[i].carryUs);
            recovery.push_back(f[i].recoveryUs);
            if ((i + spacing / 2) % spacing >= spacing * 9 / 10) {
                ++tail;
                tailPushed += f[i].carryUs != 0 || f[i].recoveryUs != 0;
            }
        }
        const uint64_t carryLimit = s.controller().catchupCarryLimitUs();
        const uint64_t peakApplied = maximum(f, frames116(10), total, applied);
        report("%-52s carry p50 %4" PRIu64 " p90 %4" PRIu64 " (limit %4" PRIu64 ") recovery p50 %4" PRIu64 " tail pushed %zu/%zu buffer %5" PRIu64 "\n",
            name.c_str(), percentile(carry, 500), percentile(carry, 900), carryLimit, percentile(recovery, 500),
            tailPushed, tail, peakApplied);
        check(percentile(carry, 500) == 0 && percentile(recovery, 500) == 0, name,
            "between late frames the schedule returns to its plan",
            "carry p50 %" PRIu64 " us, recovery p50 %" PRIu64 " us", percentile(carry, 500), percentile(recovery, 500));
        check(tail > 0 && tailPushed == 0, name, "all carry drains before the next late frame",
            "%zu of %zu frames just before a spike still carried delay", tailPushed, tail);
        check(percentile(carry, 900) <= carryLimit, name, "carry stays within the catch-up ceiling",
            "carry p90 %" PRIu64 " us, limit %" PRIu64 " us", percentile(carry, 900), carryLimit);
    }
}

// 10. Earned protection must expire on time even when every later frame is
// ineligible (lost packets) and the source epoch keeps restarting.
// Regression: 75e97d26 (layers expired by diagnostic activity, so invalid
// observations kept stale protection alive).
void testExpiryThroughInvalidEvidence()
{
    for (const auto& preset : kPresets) for (const auto& smoothing : kSmoothing) {
        const std::string name = label("invalid-expiry", preset, smoothing);
        Stream s(vrrProductionSession(preset.mode, 116, 120, smoothing.smooth, smoothing.absorb), 1000);
        Rng rng(101 + uint64_t(preset.mode));
        const size_t earned = frames116(30);
        for (size_t i = 0; i < earned; ++i) s.step(k116Ticks, {rng.below(400) + (i % 20 == 10 ? 3000 : 0)});
        const uint64_t learned = s.frames().back().appliedUs;
        const size_t total = earned + frames116(double(s.windowUs()) / 1e6 + 60);
        for (size_t i = earned; i < total; ++i) {
            if (i % 60 == 0) s.rebase();
            s.step(k116Ticks, {rng.below(400), 1});
        }
        const auto in = s.input();
        const uint64_t last = s.frames().back().appliedUs;
        report("%-52s learned %5" PRIu64 " after history %5" PRIu64 " floor %5" PRIu64 "\n",
            name.c_str(), learned + s.padUs(), last, in.minimumUs);
        check(learned + s.padUs() >= 2000, name, "the disturbance establishes protection",
            "buffer plus pad %" PRIu64 " us", learned + s.padUs());
        check(last <= in.minimumUs + 500, name, "protection expires through invalid observations and epochs",
            "still %" PRIu64 " us (floor %" PRIu64 ") a full history after the last valid input", last, in.minimumUs);
    }
}

// 11. Smoothing may retime frames inside its own budget (Reduce judder T,
// Absorb a fixed 2T pad) but may never buy input buffer or a standing delay
// beyond that budget, and Reduce judder must actually even out short/long
// source pairs. Guards the current revisions 8/9 (immediate fixed-reserve
// smoothing) and their predecessors' T-per-second actuator, which passed
// short/long pairs through unchanged.
void testSmoothingStaysInsideItsBudget()
{
    for (int rate : {90, 116}) {
        const uint32_t ticks = uint32_t(90000 / rate);
        struct Run { uint64_t peakApplied, floor, extraP50, latency, jerkP95; };
        Run runs[3] = {};
        for (int mode = 0; mode < 3; ++mode) {
            const auto& smoothing = kSmoothing[mode];
            Stream s(vrrProductionSession(1, rate, 120, smoothing.smooth, smoothing.absorb), 1000);
            Rng sourceRng(103), inputRng(107);
            const size_t total = size_t(40 * rate);
            for (size_t i = 0; i < total; ++i) {
                // 0.5-1 ms of host jitter on every interval, alternating short/long.
                const uint32_t jitter = uint32_t(45 + sourceRng.below(46));
                s.step(i % 2 ? ticks + jitter : ticks - jitter, {inputRng.below(300)});
            }
            const auto& f = s.frames();
            const size_t from = size_t(10 * rate);
            std::vector<uint64_t> extra;
            for (size_t i = from; i < total; ++i) extra.push_back(f[i].extraUs);
            runs[mode] = {maximum(f, from, total, applied), s.input().minimumUs, percentile(extra, 500),
                          mean(f, from, total, latency), percentile(presentedJerk(f, from, total), 950)};
        }
        const uint64_t tolerance = uint64_t(vrrProductionSession(1, rate, 120).timingOptions.toleranceUs);
        for (int mode = 0; mode < 3; ++mode) {
            const auto& smoothing = kSmoothing[mode];
            const std::string name = std::string("smoothing-budget/balanced/") + smoothing.name + "/" + std::to_string(rate) + "fps";
            const Run& r = runs[mode];
            const uint64_t budget = !smoothing.smooth ? 0 : smoothing.absorb ? 2 * tolerance : tolerance;
            report("%-52s buffer %5" PRIu64 " extra p50 %4" PRIu64 " (budget %4" PRIu64 ") latency %5" PRIu64 " (off %5" PRIu64 ") jerk p95 %4" PRIu64 " (off %4" PRIu64 ")\n",
                name.c_str(), r.peakApplied, r.extraP50, budget, r.latency, runs[0].latency, r.jerkP95, runs[0].jerkP95);
            check(r.peakApplied <= r.floor + 250, name, "smoothing never buys input buffer",
                "input buffer peaked at %" PRIu64 " us (floor %" PRIu64 ")", r.peakApplied, r.floor);
            check(r.extraP50 <= budget + 50, name, "typical retiming stays inside the smoothing budget",
                "planned extra delay p50 %" PRIu64 " us, budget %" PRIu64 " us", r.extraP50, budget);
            check(r.latency <= runs[0].latency + budget + 250, name, "smoothing adds no standing delay beyond its budget",
                "mean latency %" PRIu64 " us vs %" PRIu64 " us with smoothing off", r.latency, runs[0].latency);
            // Pass-through measures 1.0x; Reduce judder currently about 0.69x.
            if (smoothing.smooth)
                check(r.jerkP95 * 5 <= runs[0].jerkP95 * 4, name, "smoothing actually evens out short/long source pairs",
                    "presented jerk p95 %" PRIu64 " us vs %" PRIu64 " us with smoothing off", r.jerkP95, runs[0].jerkP95);
        }
    }
}

} // namespace

int main(int argc, char** argv)
{
    struct Scenario { const char* name; void (*run)(); };
    const Scenario scenarios[] = {
        {"start-release", testStartingDelayReleasesOnCleanInput},
        {"within-allowance", testMissesWithinAllowanceBuyNoDelay},
        {"earn-protection", testGenuineLateInputEarnsProtection},
        {"plateau-history", testPlateauFollowsSelectedHistory},
        {"rate-round-trip", testRateRoundTripRestoresProtection},
        {"rate-drop", testRateDropReleasesMappingExcess},
        {"clock-drift", testClockDriftNeitherRatchetsNorErodes},
        {"loss-and-downstream", testLossAndDownstreamWorkBuyNoInputDelay},
        {"catch-up-carry", testCatchUpCarryDrainsBetweenLateFrames},
        {"invalid-expiry", testExpiryThroughInvalidEvidence},
        {"smoothing-budget", testSmoothingStaysInsideItsBudget},
    };
    std::vector<const char*> selection;
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--strict") == 0) g_Strict = true;
        else selection.push_back(argv[i]);
    }
    const auto begin = std::chrono::steady_clock::now();
    // Every scenario owns its controllers, so they run concurrently. Output
    // stays in scenario order.
    std::vector<std::pair<const char*, std::future<Log>>> running;
    for (const auto& scenario : scenarios) {
        bool selected = selection.empty();
        for (const char* name : selection) selected |= std::strcmp(name, scenario.name) == 0;
        if (!selected) continue;
        running.emplace_back(scenario.name, std::async(std::launch::async, [run = scenario.run, name = scenario.name] {
            Log log;
            t_Log = &log;
            const auto started = std::chrono::steady_clock::now();
            run();
            report("-- %s: %.2f s\n", name,
                std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count());
            t_Log = nullptr;
            return log;
        }));
    }
    for (auto& [name, result] : running) {
        for (const auto& [failure, line] : result.get()) {
            std::fflush(failure ? stdout : stderr);
            std::fputs(line.c_str(), failure ? stderr : stdout);
        }
    }
    const double elapsed = std::chrono::duration<double>(std::chrono::steady_clock::now() - begin).count();
    for (auto& known : g_Known) {
        if (known.failed) {
            std::printf("OPEN KNOWN REGRESSION (%zu failing checks under %s): %s\n",
                known.failed, known.scenarioPrefix, known.evidence);
        }
        else if (known.passed) {
            ++g_Failures;
            std::fprintf(stderr, "FIXED KNOWN REGRESSION under %s: every check passes now; delete its g_Known entry "
                "so the contract fails hard from here on.\n", known.scenarioPrefix);
        }
    }
    std::fflush(stdout);
    if (g_Failures) {
        std::fprintf(stderr, "VRR buffer regressions: %d of %d contracts FAILED (%.1f s)\n",
            g_Failures.load(), g_Checks.load(), elapsed);
        return 1;
    }
    std::printf("VRR buffer regressions: all %d contracts passed (%.1f s)\n", g_Checks.load(), elapsed);
    return 0;
}

