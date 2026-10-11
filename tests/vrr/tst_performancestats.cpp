#include "assertions.h"
#include "../../app/streaming/video/performancestats.h"
#include <iostream>

using PerformanceStats::Snapshot;
int main(int argc, char** argv)
{
    Snapshot s;
    s.width = 3840; s.height = 2160; s.codec = "PyroWave 10-bit HDR 4:4:4";
    s.receivedFps = s.decodedFps = s.renderedFps = 110.0;
    s.rttMs = 2; s.decodeMs = 3.85; s.queueMs = 6.42;
    s.vrr = s.adaptive = true;
    s.bufferUs = 5700; s.bufferCapUs = 36364;
    s.targetPpm = 999500; s.historyUs = 300000000; s.toleranceUs = 500;
    s.input.samples = s.input.recentEligible = 22000;
    s.input.recentMissed = 11; s.input.recentAbsorbed = 200;
    s.input.windowUs = 300000000; s.input.targetUs = 5420;
    s.input.tailUncertain = false;
    s.display.intervals = 220; s.display.deviations = 4; s.display.uncertain = 2;
    s.display.lastObservedUs = s.nowUs = 10000000;
    auto text = PerformanceStats::format(s);
    assert(text.find("99.95% (21989/22000 frames)") != std::string::npos);
    assert(text.find("4/220 (1.82%) | uncertain 2") != std::string::npos);
    assert(text.find("DISPLAY >0.50 ms (2s matched)") != std::string::npos);
    assert(text.find("history 300s | tolerance 0.50 ms") != std::string::npos);
    if (argc > 1 && std::string(argv[1]) == "--fixture") { std::cout << text; return 0; }
    // A legitimate measured zero differs from missing or stale feedback.
    s.display.deviations = 0;
    assert(PerformanceStats::format(s).find("0/220 (0.00%)") != std::string::npos);
    ++s.nowUs; s.nowUs += 1000000;
    assert(PerformanceStats::format(s).find("N/A (no recent matched feedback)") != std::string::npos);
    s.nowUs = s.display.lastObservedUs; s.display.intervals = 0;
    assert(PerformanceStats::format(s).find("N/A (no recent matched feedback)") != std::string::npos);
    s.input.recentEligible = 0;
    assert(PerformanceStats::format(s).find("Buffer coverage (300s): N/A") != std::string::npos);
    s.vrr = false;
    assert(PerformanceStats::format(s).find("DISPLAY") == std::string::npos);

    // Exact 500 us is accepted; only strictly larger certain residuals count.
    for (uint64_t error : {499, 500, 501}) {
        Vrr13::CadenceDetection cadence;
        cadence.observe({1, 10000, 1000000, 1100000, 0, true}, 500);
        cadence.observe({2, 20000, 1010000 + error, 1200000, 0, true}, 500);
        assert(cadence.stats().intervals == 1);
        assert(cadence.stats().deviations == (error > 500 ? 1 : 0));
        assert(cadence.stats().uncertain == 0);
    }
    // Producer-buffer sizing consumes the selected 500 us once and keeps
    // Claude's free VRR room independent from the actual applied buffer.
    for (uint64_t room : {0, 760}) for (uint64_t error : {499, 500, 501}) {
        Vrr13::InputPercentileBuffer input;
        input.reset(5000);
        input.observe(1, 1000000, 1000000, int64_t(5000 + room + error), true,
            0, 20000, 999500, 300000000, 1000, 250, 0, 500, room);
        assert(input.stats().targetUs == 5000 + error - 500);
        assert(input.stats().recentMissed == (error > 500 ? 1 : 0));
        assert(input.stats().rawRequiredQuantileUs == int64_t(5000 + room + error));
    }
    Vrr13::CadenceDetection cadence;
    cadence.observe({1, 10000, 1000000, 1100000, 1, true}, 500);
    cadence.observe({2, 20000, 1010501, 1200000, 1, true}, 500);
    assert(cadence.stats().deviations == 0 && cadence.stats().uncertain == 1);
    // Missing identity breaks adjacency and contributes no fake clean pair.
    cadence.observe({4, 40000, 1030000, 1300000, 0, true}, 500);
    assert(cadence.stats().intervals == 1);
    cadence.observe({5, 50000, 1040000, 1400000, 0, true}, 500);
    assert(cadence.stats().intervals == 2);
    // Rolling 2 s buckets expire old errors and their denominator together.
    cadence.observe({6, 60000, 1050000, 3500000, 0, true}, 500);
    assert(cadence.stats().intervals == 1 && cadence.stats().deviations == 0);
    std::cout << "Compact stats, N/A feedback, coverage, strict tolerance and matched-window denominator passed\n";
}
