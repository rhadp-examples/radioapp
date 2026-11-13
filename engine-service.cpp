#include <atomic>
#include <cerrno>
#include <cstdint>
#include <csignal>
#include <cstdlib>
#include <ctime>
#include <iostream>
#include <condition_variable>
#include <functional>
#include <algorithm>
#include <cmath>
#include <iomanip>
#include <limits>
#include <sstream>
#include <set>
#include <string>
#include <thread>
#include <chrono>

using namespace std::literals::chrono_literals;

#include <vsomeip/vsomeip.hpp>

#include "services.hpp"
#include "demo-health.hpp"
#include "engine-demo-memory.hpp"

namespace {

// Use consumed thread CPU time so competing workloads increase the wall-clock
// duration of this computation without changing its target CPU-time budget.
std::uint64_t thread_cpu_time_ns() {
    timespec ts{};
    clock_gettime(CLOCK_THREAD_CPUTIME_ID, &ts);
    return static_cast<std::uint64_t>(ts.tv_sec) * 1000000000ULL
         + static_cast<std::uint64_t>(ts.tv_nsec);
}

// Keep the demo workload separate from normal engine state processing.
void demo_cpu_work(unsigned cpu_ms) {
    const std::uint64_t start_ns = thread_cpu_time_ns();
    const std::uint64_t duration_ns = static_cast<std::uint64_t>(cpu_ms) * 1000000ULL;
    std::uint64_t state = 0x9e3779b97f4a7c15ULL;

    while (thread_cpu_time_ns() - start_ns < duration_ns) {
        for (unsigned i = 0; i < 16384; ++i) {
            state ^= state << 13;
            state ^= state >> 7;
            state ^= state << 17;
        }
    }

    // Prevent optimizing builds from eliminating the computation.
    static volatile std::uint64_t sink;
    sink ^= state;
}

unsigned demo_cpu_duration_ms() {
    const char *value = std::getenv("ENGINE_DEMO_CPU_MS");
    if (value == nullptr || *value == '\0')
        return 0;

    char *end = nullptr;
    errno = 0;
    const unsigned long parsed = std::strtoul(value, &end, 10);
    if (value[0] == '-' || value[0] == '+' || end == value || *end != '\0'
            || errno != 0 || parsed > 5000UL) {
        std::cerr << "ENGINE: Invalid ENGINE_DEMO_CPU_MS (expected 0..5000): "
                  << value << std::endl;
        return 0;
    }
    return static_cast<unsigned>(parsed);
}

// Convert floating-point durations to bounded wire-format integers.
std::uint32_t clamp_u32(double value) {
    if (!(value > 0.0))
        return 0;
    if (value >= static_cast<double>(std::numeric_limits<std::uint32_t>::max()))
        return std::numeric_limits<std::uint32_t>::max();
    return static_cast<std::uint32_t>(std::lround(value));
}

// A missed calculation deadline represents synthetic CPU interference.
// Hold the warning briefly to avoid flickering between samples.
class engine_cpu_health {
public:
    void record(demo_health::Message &health, unsigned target_ms,
                std::chrono::steady_clock::duration wall_elapsed,
                std::uint64_t cpu_elapsed_ns) {
        const double wall_ms = std::chrono::duration<double, std::milli>(wall_elapsed).count();
        const double cpu_ms = static_cast<double>(cpu_elapsed_ns) / 1000000.0;
        const double off_ms = std::max(0.0, wall_ms - cpu_ms);
        const double deadline_ms = target_ms * 1.10 + 10.0;
        const double overrun_ms = std::max(0.0, wall_ms - deadline_ms);
        const auto now = std::chrono::steady_clock::now();

        health.cpu_metrics_valid = true;
        health.cpu_share_centi_percent = clamp_u32(
                wall_ms > 0.0 ? 10000.0 * cpu_ms / wall_ms : 0.0);
        health.cpu_wall_ms = clamp_u32(wall_ms);
        health.cpu_off_ms = clamp_u32(off_ms);
        health.cpu_overrun_ms = clamp_u32(overrun_ms);
        if (overrun_ms > 0.0) {
            if (health.cpu_deadline_misses < std::numeric_limits<std::uint32_t>::max())
                ++health.cpu_deadline_misses;
            warning_until_ = now + std::chrono::seconds(2);
        }
        health.cpu = now < warning_until_
                ? demo_health::Level::Degraded : demo_health::Level::Normal;

        // Keep journal output useful even when no deadline is missed.
        if (!reported_ || now >= next_log_at_) {
            std::cout << std::fixed << std::setprecision(1)
                      << "ENGINE: CPU demo metrics: target_cpu=" << target_ms
                      << "ms actual_cpu=" << cpu_ms
                      << "ms wall=" << wall_ms
                      << "ms off_cpu=" << off_ms
                      << "ms cpu_share=" << (wall_ms > 0.0 ? 100.0 * cpu_ms / wall_ms : 0.0)
                      << "% deadline=" << deadline_ms
                      << "ms overrun=" << overrun_ms
                      << "ms misses_total=" << health.cpu_deadline_misses << std::endl;
            reported_ = true;
            next_log_at_ = now + std::chrono::seconds(1);
        }
    }
private:
    std::chrono::steady_clock::time_point warning_until_{};
    std::chrono::steady_clock::time_point next_log_at_{};
    bool reported_ = false;
};

} // namespace

class engine_service {
public:
    engine_service() :
            app(vsomeip::runtime::get()->create_application("Engine")),
            is_registered(false),
            main_blocked(false),
            main_running(true),
            is_offered(false),
            in_reverse(false),
            main_thread(std::bind(&engine_service::run, this)) {
    }

    bool init() {
        std::lock_guard<std::mutex> its_lock(main_mutex);

        if (!app->init()) {
            std::cerr << "Couldn't initialize application" << std::endl;
            return false;
        }

        app->register_state_handler(
                std::bind(&engine_service::on_state, this,
                        std::placeholders::_1));

        app->register_message_handler(
                ENGINE_SERVICE_ID,
                ENGINE_INSTANCE_ID,
                ENGINE_GET_REVERSE_METHOD_ID,
                std::bind(&engine_service::on_get_reverse, this,
                          std::placeholders::_1));

        std::set<vsomeip::eventgroup_t> its_groups;
        its_groups.insert(ENGINE_EVENTGROUP_ID);
        app->offer_event(
                ENGINE_SERVICE_ID,
                ENGINE_INSTANCE_ID,
                ENGINE_REVERSE_EVENT_ID,
                its_groups,
                vsomeip::event_type_e::ET_FIELD, std::chrono::milliseconds::zero(),
                false, true, nullptr, vsomeip::reliability_type_e::RT_UNKNOWN);
        app->offer_event(
                ENGINE_SERVICE_ID,
                ENGINE_INSTANCE_ID,
                ENGINE_HEALTH_EVENT_ID,
                std::set<vsomeip::eventgroup_t>{ENGINE_HEALTH_EVENTGROUP_ID},
                vsomeip::event_type_e::ET_FIELD, std::chrono::milliseconds::zero(),
                false, true, nullptr, vsomeip::reliability_type_e::RT_UNKNOWN);

        main_blocked = true;
        main_condition.notify_one();
        return true;
    }

    void start() {
        app->start();
    }

    void stop() {
        main_running = false;
        main_blocked = true;
        main_condition.notify_one();
        app->clear_all_handler();
        stop_offer();
        main_thread.join();
        app->stop();
    }

    void offer() {
        app->offer_service(ENGINE_SERVICE_ID, ENGINE_INSTANCE_ID);
        is_offered = true;
    }

    void stop_offer() {
        app->stop_offer_service(ENGINE_SERVICE_ID, ENGINE_INSTANCE_ID);
    }

    void on_state(vsomeip::state_type_e _state) {
        std::cout << "Application " << app->get_name() << " is "
        << (_state == vsomeip::state_type_e::ST_REGISTERED ?
                "registered." : "deregistered.") << std::endl;

        if (_state == vsomeip::state_type_e::ST_REGISTERED) {
            if (!is_registered) {
                is_registered = true;
            }
        } else {
            is_registered = false;
        }
    }

    // Call with lock held
    std::shared_ptr<vsomeip::payload> get_reverse_payload() {
        return payload_with_arg (in_reverse ? 1 : 0);
    }

    void on_get_reverse(const std::shared_ptr<vsomeip::message> &_message) {
        std::cout << "ENGINE: Received get_reverse" << std::endl;
        std::shared_ptr<vsomeip::message> its_response = vsomeip::runtime::get()->create_response(_message);

        {
            std::lock_guard<std::mutex> its_lock(main_mutex);
            its_response->set_payload(get_reverse_payload());
        }

        app->send(its_response);
    }

    void run() {
        const unsigned demo_cpu_ms = demo_cpu_duration_ms();
        const char *lock_env = std::getenv("ENGINE_DEMO_HOLD_MUTEX");
        const bool demo_hold_mutex = lock_env != nullptr && lock_env[0] == '1'
                                   && lock_env[1] == '\0';
        const std::uint64_t demo_ram_bytes = engine_demo::demo_ram_size_bytes();
        engine_demo::demo_ram_buffer ram_working_set;
        engine_demo::demo_ram_warning_state ram_warning;
        bool ram_allocation_failed = false;
        engine_cpu_health cpu_monitor;
        demo_health::Message health;
        health.ram_requested_bytes = demo_ram_bytes;
        auto next_health_report = std::chrono::steady_clock::now();

        std::unique_lock<std::mutex> its_lock(main_mutex);
        // Wait for init() before offering the service or entering the loop.
        while (!main_blocked)
            main_condition.wait(its_lock);

        if (demo_cpu_ms > 0) {
            std::cout << "ENGINE: CPU demo enabled (" << demo_cpu_ms
                      << " ms thread CPU time per cycle, mutex "
                      << (demo_hold_mutex ? "held" : "released") << ")"
                      << std::endl;
        } else {
            std::cout << "ENGINE: CPU demo disabled" << std::endl;
        }
        if (demo_ram_bytes > 0) {
            std::cout << "ENGINE: RAM demo enabled ("
                      << demo_ram_bytes / (1024ULL * 1024ULL)
                      << " MiB per direction phase, 15s warning hold)" << std::endl;
        } else {
            std::cout << "ENGINE: RAM demo disabled" << std::endl;
        }

        offer();
        // Publish an initial forward state so newly connected clients do not
        // wait for the first 10-second direction transition.
        app->notify(ENGINE_SERVICE_ID, ENGINE_INSTANCE_ID,
                    ENGINE_REVERSE_EVENT_ID, get_reverse_payload());
        bool begin_phase = true;
        auto phase_deadline = std::chrono::steady_clock::now();

        // The health event is a field and also a heartbeat. The sequence
        // counter makes each sample distinct, even if measurements are stable.
        const auto publish_health = [&] (bool force) {
            const auto now = std::chrono::steady_clock::now();
            if (!force && now < next_health_report)
                return;
            ++health.sequence;
            const auto bytes = demo_health::encode(health);
            auto payload = vsomeip::runtime::get()->create_payload();
            payload->set_data(bytes);
            app->notify(ENGINE_SERVICE_ID, ENGINE_INSTANCE_ID,
                        ENGINE_HEALTH_EVENT_ID, payload);
            next_health_report = now + std::chrono::seconds(1);
        };

        while (main_running) {
            if (begin_phase) {
                begin_phase = false;
                std::cout << (in_reverse ? "ENGINE: Reversing..." : "ENGINE: Driving...")
                          << std::endl;

                if (demo_ram_bytes > 0) {
                    std::string reason;
                    std::uint64_t rolled_back_bytes = 0;
                    const auto allocation_started = std::chrono::steady_clock::now();

                    // Do not block the get_reverse callback while touching RAM.
                    its_lock.unlock();
                    const bool acquired = ram_working_set.acquire(
                            demo_ram_bytes, reason, rolled_back_bytes);
                    its_lock.lock();
                    if (!main_running)
                        break;

                    const auto now = std::chrono::steady_clock::now();
                    const double elapsed_ms = std::chrono::duration<double, std::milli>(
                            now - allocation_started).count();
                    health.ram_metrics_valid = true;
                    health.ram_allocated_bytes = acquired ? demo_ram_bytes : 0;
                    ram_allocation_failed = !acquired;
                    if (acquired) {
                        ram_warning.clear_allocation_failure();
                    } else {
                        ram_warning.record_failure(now);
                        if (health.ram_allocation_failures <
                                std::numeric_limits<std::uint32_t>::max())
                            ++health.ram_allocation_failures;
                    }
                    health.ram = ram_warning.active(now)
                            ? demo_health::Level::Failed : demo_health::Level::Normal;

                    std::cout << std::fixed << std::setprecision(1)
                              << "ENGINE: RAM demo: requested="
                              << demo_ram_bytes / (1024.0 * 1024.0)
                              << "MiB result=" << (acquired ? "OK" : "FAILED")
                              << " duration=" << elapsed_ms
                              << "ms allocated=" << health.ram_allocated_bytes / (1024.0 * 1024.0)
                              << "MiB rolled_back=" << rolled_back_bytes / (1024.0 * 1024.0)
                              << "MiB";
                    if (!acquired)
                        std::cout << " reason=" << reason << " (warning >=15s)";
                    std::cout << std::endl;
                }
                // Start each 10s forward / 4s reverse phase after setup.
                phase_deadline = std::chrono::steady_clock::now()
                        + (in_reverse ? 4s : 10s);
                publish_health(true);
            }

            if (!main_running)
                break;

            if (demo_cpu_ms > 0) {
                // With HOLD_MUTEX=0, interference is scheduler contention;
                // with HOLD_MUTEX=1, it also delays get_reverse callbacks.
                if (!demo_hold_mutex)
                    its_lock.unlock();
                const auto work_started = std::chrono::steady_clock::now();
                const std::uint64_t cpu_started_ns = thread_cpu_time_ns();
                demo_cpu_work(demo_cpu_ms);
                const std::uint64_t consumed_cpu_ns = thread_cpu_time_ns() - cpu_started_ns;
                const auto work_elapsed = std::chrono::steady_clock::now() - work_started;
                if (!demo_hold_mutex)
                    its_lock.lock();
                cpu_monitor.record(health, demo_cpu_ms, work_elapsed, consumed_cpu_ns);
                // Briefly yield the main mutex for requests and shutdown.
                main_condition.wait_for(its_lock, 2ms);
            } else {
                // Wake for the heartbeat and RAM recovery even without CPU work.
                main_condition.wait_until(its_lock, std::min(phase_deadline,
                        next_health_report));
            }

            if (!main_running)
                break;
            const auto now = std::chrono::steady_clock::now();
            if (demo_ram_bytes > 0 && !ram_allocation_failed &&
                health.ram == demo_health::Level::Failed &&
                !ram_warning.active(now)) {
                health.ram = demo_health::Level::Normal;
                std::cout << "ENGINE: RAM demo: warning cleared after 15s hold"
                          << std::endl;
                publish_health(true);
            }
            publish_health(false);

            if (now >= phase_deadline) {
                in_reverse = !in_reverse;
                // Preserve the original direction field and notification.
                app->notify(ENGINE_SERVICE_ID, ENGINE_INSTANCE_ID,
                            ENGINE_REVERSE_EVENT_ID, get_reverse_payload());
                begin_phase = true;
            }
        }
        ram_working_set.release();
    }

private:
    std::shared_ptr<vsomeip::application> app;
    bool is_registered;

    std::mutex main_mutex;
    std::condition_variable main_condition;
    std::atomic<bool> main_blocked;
    std::atomic<bool> main_running;
    bool is_offered;

    // Engine state:
    bool in_reverse;

    // Start the worker only after state and synchronization are initialized.
    std::thread main_thread;
};

engine_service *its_engine_ptr(nullptr);
static void handle_signal(int _signal) {
    if (its_engine_ptr != nullptr &&
        (_signal == SIGINT || _signal == SIGTERM))
        its_engine_ptr->stop();
}

int main() {
    engine_service its_engine;

    its_engine_ptr = &its_engine;
    signal(SIGINT, handle_signal);
    signal(SIGTERM, handle_signal);

    if (!its_engine.init()) {
      return 1;
    }

    its_engine.start();
    // Work around dlt-daemon timeout on exit
    _exit(0);
}
