#pragma once

#include <chrono>
#include <cstdint>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

namespace uevr::perf {
// Lightweight game-thread profiler for the mod's own per-tick work.
// Slots are registered once (cache the returned id in a static) and
// accumulate microseconds per frame; end_frame() folds the frame total
// into an exponential moving average and a 1-second peak.
class FrameProfiler {
public:
    struct Entry {
        std::string name{};
        double frame_us{};
        double ema_us{};
        double max_us{};
        double pending_max_us{};
    };

    static FrameProfiler& get() {
        static FrameProfiler instance{};
        return instance;
    }

    size_t slot(std::string_view name) {
        std::scoped_lock _{m_mutex};

        for (size_t i = 0; i < m_entries.size(); ++i) {
            if (m_entries[i].name == name) {
                return i;
            }
        }

        m_entries.emplace_back(Entry{std::string{name}});
        return m_entries.size() - 1;
    }

    void add(size_t id, double us) {
        std::scoped_lock _{m_mutex};

        if (id < m_entries.size()) {
            m_entries[id].frame_us += us;
        }
    }

    void end_frame() {
        static const size_t interval_slot = slot("Frame.interval (wall clock, not mod work)");

        std::scoped_lock _{m_mutex};

        const auto now = std::chrono::steady_clock::now();
        const bool roll_max = now - m_last_max_roll >= std::chrono::seconds(1);

        if (m_last_frame_end.time_since_epoch().count() != 0 && interval_slot < m_entries.size()) {
            m_entries[interval_slot].frame_us += std::chrono::duration<double, std::micro>(now - m_last_frame_end).count();
        }

        m_last_frame_end = now;

        double frame_total = 0.0;

        for (size_t i = 0; i < m_entries.size(); ++i) {
            auto& entry = m_entries[i];

            if (i != interval_slot && !entry.name.starts_with("Game.")) {
                frame_total += entry.frame_us;
            }

            entry.ema_us = entry.ema_us * (1.0 - EMA_ALPHA) + entry.frame_us * EMA_ALPHA;

            if (entry.frame_us > entry.pending_max_us) {
                entry.pending_max_us = entry.frame_us;
            }

            if (roll_max) {
                entry.max_us = entry.pending_max_us;
                entry.pending_max_us = 0.0;
            }

            entry.frame_us = 0.0;
        }

        m_total_ema_us = m_total_ema_us * (1.0 - EMA_ALPHA) + frame_total * EMA_ALPHA;

        if (frame_total > m_pending_total_max_us) {
            m_pending_total_max_us = frame_total;
        }

        if (roll_max) {
            m_total_max_us = m_pending_total_max_us;
            m_pending_total_max_us = 0.0;
            m_last_max_roll = now;
        }

        ++m_frames;
    }

    std::vector<Entry> snapshot() const {
        std::scoped_lock _{m_mutex};
        return m_entries;
    }

    double total_ema_us() const {
        std::scoped_lock _{m_mutex};
        return m_total_ema_us;
    }

    double total_max_us() const {
        std::scoped_lock _{m_mutex};
        return m_total_max_us;
    }

    uint64_t frames() const {
        std::scoped_lock _{m_mutex};
        return m_frames;
    }

    class Scope {
    public:
        explicit Scope(size_t id)
            : m_id{id},
              m_start{std::chrono::steady_clock::now()}
        {
        }

        ~Scope() {
            const auto us = std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - m_start).count();
            FrameProfiler::get().add(m_id, us);
        }

        Scope(const Scope&) = delete;
        Scope& operator=(const Scope&) = delete;

    private:
        size_t m_id{};
        std::chrono::steady_clock::time_point m_start{};
    };

private:
    static constexpr double EMA_ALPHA = 0.05;

    mutable std::mutex m_mutex{};
    std::vector<Entry> m_entries{};
    double m_total_ema_us{};
    double m_total_max_us{};
    double m_pending_total_max_us{};
    uint64_t m_frames{};
    std::chrono::steady_clock::time_point m_last_max_roll{};
    std::chrono::steady_clock::time_point m_last_frame_end{};
};
} // namespace uevr::perf

// Usage: UEVR_PERF_SCOPE("UObjectHook.scan");
#define UEVR_PERF_CONCAT_INNER(a, b) a##b
#define UEVR_PERF_CONCAT(a, b) UEVR_PERF_CONCAT_INNER(a, b)
#define UEVR_PERF_SCOPE(name) \
    static const size_t UEVR_PERF_CONCAT(_uevr_perf_slot_, __LINE__) = ::uevr::perf::FrameProfiler::get().slot(name); \
    ::uevr::perf::FrameProfiler::Scope UEVR_PERF_CONCAT(_uevr_perf_scope_, __LINE__){UEVR_PERF_CONCAT(_uevr_perf_slot_, __LINE__)}
