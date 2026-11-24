#include <array>
#include <cassert>
#include <cstdint>
#include <cstring>

class CaptureClock {
public:
    float sample(float input, bool paused) {
        if (paused && !held_) {
            phase_ = static_cast<float>(input - correction_);
        } else if (!paused && held_) {
            correction_ = static_cast<double>(input) - phase_;
        }
        held_ = paused;
        return paused ? phase_ : static_cast<float>(input - correction_);
    }

private:
    bool held_ = false;
    float phase_ = 0;
    double correction_ = 0;
};

using JobTiming = std::array<std::uint8_t, 24>;

JobTiming timingForJob(JobTiming timing, bool paused, bool selectedLayer) {
    if (paused && selectedLayer) {
        std::memset(timing.data(), 0, sizeof(std::uint32_t));
    }
    return timing;
}

int main() {
    JobTiming original{};
    for (std::size_t i = 0; i < original.size(); ++i) {
        original[i] = static_cast<std::uint8_t>(i + 1);
    }
    const auto held = timingForJob(original, true, true);
    for (std::size_t i = 0; i < held.size(); ++i) {
        assert(held[i] == (i < 4 ? 0 : original[i]));
        assert(original[i] == i + 1);
    }
    assert(timingForJob(original, true, false) == original);
    assert(timingForJob(original, false, true) == original);
    std::array<CaptureClock, 3> clocks{};
    for (int i = 0; i < 3; ++i) {
        const float start = static_cast<float>(i * 100);
        auto &clock = clocks[i];
        assert(clock.sample(start, false) == start);
        assert(clock.sample(start + 1, true) == start + 1);
        assert(clock.sample(start + 15, true) == start + 1);
        assert(clock.sample(start + 20, false) == start + 1);
        assert(clock.sample(start + 22, false) == start + 3);
        assert(clock.sample(start + 23, true) == start + 4);
        assert(clock.sample(start + 40, false) == start + 4);
    }
    return 0;
}
