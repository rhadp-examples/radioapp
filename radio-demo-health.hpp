#pragma once

#include "demo-health.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <limits>

// Demo-only radio health measurements. The class is independent of vsomeip
// so that threshold handling and the wire values can be tested in isolation.
namespace radio_demo {

struct CpuSample {
    double cpu_ms;
    double wall_ms;
    double off_cpu_ms;
    double cpu_share_percent;
    double deadline_ms;
    double overrun_ms;
};

inline std::uint32_t bounded_u32(double value) {
    if (!(value > 0.0))
        return 0;
    if (value >= static_cast<double>(std::numeric_limits<std::uint32_t>::max()))
        return std::numeric_limits<std::uint32_t>::max();
    return static_cast<std::uint32_t>(std::lround(value));
}

class HealthTracker {
public:
    using clock = std::chrono::steady_clock;

    HealthTracker(unsigned target_cpu_ms, std::uint64_t requested_ram_bytes)
        : target_cpu_ms_(target_cpu_ms) {
        health_.ram_requested_bytes = requested_ram_bytes;
    }

    CpuSample record_cpu(clock::duration elapsed, std::uint64_t cpu_ns,
                         clock::time_point now) {
        const double wall_ms = std::chrono::duration<double, std::milli>(elapsed).count();
        const double cpu_ms = static_cast<double>(cpu_ns) / 1000000.0;
        const double off_ms = std::max(0.0, wall_ms - cpu_ms);
        const double share = wall_ms > 0.0 ? 100.0 * cpu_ms / wall_ms : 0.0;
        const double deadline_ms = target_cpu_ms_ * 1.10 + 10.0;
        const double overrun_ms = std::max(0.0, wall_ms - deadline_ms);

        health_.cpu_metrics_valid = true;
        health_.cpu_share_centi_percent = bounded_u32(std::min(100.0, share) * 100.0);
        health_.cpu_wall_ms = bounded_u32(wall_ms);
        health_.cpu_off_ms = bounded_u32(off_ms);
        health_.cpu_overrun_ms = bounded_u32(overrun_ms);
        if (overrun_ms > 0.0) {
            if (health_.cpu_deadline_misses != std::numeric_limits<std::uint32_t>::max())
                ++health_.cpu_deadline_misses;
            cpu_warning_until_ = now + std::chrono::seconds(2);
        }
        refresh_cpu(now);
        return {cpu_ms, wall_ms, off_ms, share, deadline_ms, overrun_ms};
    }

    // Update RAM accounting after a new song's allocation attempt. A failure
    // increments the cumulative counter; the caller owns the 15-second hold.
    void record_ram_allocation(bool acquired, std::uint64_t allocated_bytes,
                               bool warning_active) {
        health_.ram_metrics_valid = true;
        health_.ram_allocated_bytes = allocated_bytes;
        if (!acquired && health_.ram_allocation_failures !=
                         std::numeric_limits<std::uint32_t>::max())
            ++health_.ram_allocation_failures;
        health_.ram = warning_active ? demo_health::Level::Failed
                                     : demo_health::Level::Normal;
    }

    // Reflect pause, obsolete-song cancellation and warning recovery without
    // marking a disabled or not-yet-exercised RAM demo as healthy.
    void refresh_ram(std::uint64_t allocated_bytes, bool warning_active) {
        if (!health_.ram_metrics_valid)
            return;
        health_.ram_allocated_bytes = allocated_bytes;
        health_.ram = warning_active ? demo_health::Level::Failed
                                     : demo_health::Level::Normal;
    }

    demo_health::Message snapshot(clock::time_point now) {
        refresh_cpu(now);
        return health_;
    }

private:
    void refresh_cpu(clock::time_point now) {
        if (health_.cpu_metrics_valid)
            health_.cpu = now < cpu_warning_until_
                    ? demo_health::Level::Degraded : demo_health::Level::Normal;
    }

    unsigned target_cpu_ms_;
    clock::time_point cpu_warning_until_{};
    demo_health::Message health_;
};

} // namespace radio_demo
