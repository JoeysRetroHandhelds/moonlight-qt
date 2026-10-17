#pragma once
#include "inputpercentilebuffer.h"
#include <deque>

namespace Vrr13 {
// Reporting only. Each actual eligible outcome enters once and ages on the
// session clock, independent of which rate layer currently owns adaptation.
class InputCoverageHistory {
public:
    void advance(uint64_t nowUs, uint64_t windowUs) {
        // A stale/cancelled callback cannot rewind the reporting clock or
        // erase history. The controller explicitly resets a new session.
        if (nowUs < m_LastUs) return;
        m_LastUs = nowUs;
        windowUs = std::clamp(windowUs, InputPercentileBuffer::MinimumWindowUs,
                             InputPercentileBuffer::MaximumWindowUs);
        while (!m_Outcomes.empty() && nowUs - m_Outcomes.front().atUs >= windowUs)
            pop();
    }
    void observe(uint64_t nowUs, uint64_t windowUs, bool eligible, bool absorbed, bool missed) {
        if (!nowUs || nowUs < m_LastUs) return;
        advance(nowUs, windowUs);
        if (!eligible) return;
        m_Outcomes.push_back({nowUs, absorbed, missed});
        m_Absorbed += absorbed; m_Missed += missed;
        while (m_Outcomes.size() > InputPercentileBuffer::MaximumSamples) pop();
    }
    void report(InputPercentileBuffer::Stats& stats) const {
        stats.recentEligible = m_Outcomes.size();
        stats.recentAbsorbed = m_Absorbed;
        stats.recentMissed = m_Missed;
    }
private:
    struct Outcome { uint64_t atUs; bool absorbed, missed; };
    void pop() {
        m_Absorbed -= m_Outcomes.front().absorbed;
        m_Missed -= m_Outcomes.front().missed;
        m_Outcomes.pop_front();
    }
    std::deque<Outcome> m_Outcomes;
    uint64_t m_LastUs = 0, m_Absorbed = 0, m_Missed = 0;
};
}
