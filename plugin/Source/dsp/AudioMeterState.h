#pragma once

#include <atomic>
#include <cstdint>
#include <optional>

namespace mixsense
{
// One audio producer and any number of readers. Atomics avoid data races;
// sequence checks keep a reading coherent. Readers make at most two attempts.
class AudioMeterState
{
public:
    struct Snapshot
    {
        float reductionDb = 0.0f, inputPeakDb = -100.0f;
        std::uint64_t generation = 0;
        bool audioRunning = false;
    };
    void publish(float reduction, float input, bool running) noexcept
    {
        sequence.fetch_add(1);
        reductionDb.store(reduction); inputPeakDb.store(input); audioRunning.store(running);
        sequence.fetch_add(1);
    }
    std::optional<Snapshot> read() const noexcept
    {
        for (int attempt = 0; attempt < 2; ++attempt)
        {
            const auto before = sequence.load();
            if (before & 1u) continue;
            Snapshot result { reductionDb.load(), inputPeakDb.load(), before, audioRunning.load() };
            if (sequence.load() == before) return result;
        }
        return std::nullopt;
    }
private:
    static_assert(std::atomic<float>::is_always_lock_free
                  && std::atomic<std::uint64_t>::is_always_lock_free
                  && std::atomic<bool>::is_always_lock_free, "Meters require lock-free atomics");
    std::atomic<std::uint64_t> sequence { 0 };
    std::atomic<float> reductionDb { 0.0f }, inputPeakDb { -100.0f };
    std::atomic<bool> audioRunning { false };
};
}
