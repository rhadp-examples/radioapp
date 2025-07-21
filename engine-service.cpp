#include <csignal>
#include <iostream>
#include <condition_variable>
#include <thread>
#include <chrono>

using namespace std::literals::chrono_literals;

#include <vsomeip/vsomeip.hpp>

#include "services.hpp"

class engine_service {
public:
    engine_service() :
            app(vsomeip::runtime::get()->create_application("Engine")),
            is_registered(false),
            main_blocked(false),
            main_running(true),
            in_reverse(false),
            is_offered(false),
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
        std::unique_lock<std::mutex> its_lock(main_mutex);
        while (!main_blocked)
            main_condition.wait(its_lock);

        offer();

        while (main_running) {
            auto timeout = 10s;

            if (in_reverse) {
                timeout = 4s;
                std::cout << "ENGINE: Reversing..." << std::endl;
            } else {
                std::cout << "ENGINE: Driving..." << std::endl;
            }

            // Sleep a bit
            bool timed_out = main_condition.wait_for(its_lock, timeout) ==  std::cv_status::timeout;

            if (timed_out) {
                // Change direction
                in_reverse = !in_reverse;
                std::shared_ptr<vsomeip::payload> payload = get_reverse_payload();

                app->notify(ENGINE_SERVICE_ID, ENGINE_INSTANCE_ID, ENGINE_REVERSE_EVENT_ID, payload);
            }
        }
    }

private:
    std::shared_ptr<vsomeip::application> app;
    bool is_registered;

    // main_blocked / is_offered must be initialized before starting the threads!
    std::thread main_thread;

    std::mutex main_mutex;
    std::condition_variable main_condition;
    bool main_blocked;
    bool main_running;
    bool is_offered;

    // Engine state:
    bool in_reverse;
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
