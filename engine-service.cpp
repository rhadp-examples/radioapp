#include <atomic>
#include <cerrno>
#include <cstdint>
#include <csignal>
#include <cstdlib>
#include <ctime>
#include <iostream>
#include <condition_variable>
#include <functional>
#include <thread>
#include <chrono>

using namespace std::literals::chrono_literals;

#include <vsomeip/vsomeip.hpp>

#include "services.hpp"

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

        std::unique_lock<std::mutex> its_lock(main_mutex);
        // Wait for init() before offering the service or entering the loop.
        while (!main_blocked)
            main_condition.wait(its_lock);

        if (demo_cpu_ms > 0) {
            std::cout << "ENGINE: CPU demo enabled (" << demo_cpu_ms
                      << " ms thread CPU time per cycle, mutex "
                      << (demo_hold_mutex ? "held" : "released") << ")"
                      << std::endl;
        }

        offer();

        while (main_running) {
            auto timeout = 10s;

            if (in_reverse) {
                timeout = 4s;
                std::cout << "ENGINE: Reversing..." << std::endl;
            } else {
                std::cout << "ENGINE: Driving..." << std::endl;
            }

            bool timed_out = false;
            if (demo_cpu_ms > 0) {
                // Preserve the original 10 s / 4 s switching periods while
                // keeping the worker busy between direction changes.
                const auto deadline = std::chrono::steady_clock::now() + timeout;
                while (main_running && std::chrono::steady_clock::now() < deadline) {
                    // Releasing the mutex isolates CPU scheduling contention.
                    // Holding it additionally blocks get_reverse callbacks.
                    if (!demo_hold_mutex)
                        its_lock.unlock();

                    demo_cpu_work(demo_cpu_ms);

                    if (!demo_hold_mutex)
                        its_lock.lock();

                    // Briefly release the mutex for callbacks and shutdown.
                    main_condition.wait_for(its_lock, 2ms);
                }
                timed_out = main_running && std::chrono::steady_clock::now() >= deadline;
            } else {
                // Preserve the original behavior when the demo is disabled.
                timed_out = main_condition.wait_for(its_lock, timeout) == std::cv_status::timeout;
            }

            if (timed_out && main_running) {
                // Change direction and publish the new SOME/IP field value.
                in_reverse = !in_reverse;
                std::shared_ptr<vsomeip::payload> payload = get_reverse_payload();

                app->notify(ENGINE_SERVICE_ID, ENGINE_INSTANCE_ID, ENGINE_REVERSE_EVENT_ID, payload);
            }
        }
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
