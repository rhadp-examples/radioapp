#include <csignal>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <cstdlib>
#include <condition_variable>
#include <thread>
#include <chrono>

using namespace std::literals::chrono_literals;

#include <vsomeip/vsomeip.hpp>

#include "services.hpp"
#include "radio-stations.hpp"

class radio_service {
public:
    radio_service() :
            app(vsomeip::runtime::get()->create_application("Radio")),
            verbose(false),
            main_blocked(false),
            main_running(true),
            is_offered(false),
            engine_available(false),
            next_volume(50),
            next_radio_station(0),
            next_playing(false),
            next_reversed(false),
            main_thread(std::bind(&radio_service::run, this)) {
    }

    bool init() {
        std::lock_guard<std::mutex> its_lock(main_mutex);

        if (!app->init()) {
            std::cerr << "Couldn't initialize application" << std::endl;
            return false;
        }

        app->register_message_handler(
                RADIO_SERVICE_ID,
                RADIO_INSTANCE_ID,
                RADIO_SET_PLAYING_METHOD_ID,
                std::bind(&radio_service::on_message_set_playing, this,
                          std::placeholders::_1));
        app->register_message_handler(
                RADIO_SERVICE_ID,
                RADIO_INSTANCE_ID,
                RADIO_IS_PLAYING_METHOD_ID,
                std::bind(&radio_service::on_message_is_playing, this,
                          std::placeholders::_1));
        app->register_message_handler(
                RADIO_SERVICE_ID,
                RADIO_INSTANCE_ID,
                RADIO_GET_VOLUME_METHOD_ID,
                std::bind(&radio_service::on_message_get_volume, this,
                          std::placeholders::_1));
        app->register_message_handler(
                RADIO_SERVICE_ID,
                RADIO_INSTANCE_ID,
                RADIO_CHANGE_VOLUME_METHOD_ID,
                std::bind(&radio_service::on_message_change_volume, this,
                          std::placeholders::_1));
        app->register_message_handler(
                RADIO_SERVICE_ID,
                RADIO_INSTANCE_ID,
                RADIO_SWITCH_STATION_METHOD_ID,
                std::bind(&radio_service::on_message_switch_station, this,
                          std::placeholders::_1));

        std::set<vsomeip::eventgroup_t> its_groups_radio;
        its_groups_radio.insert(RADIO_EVENTGROUP_ID);
        app->offer_event(
                RADIO_SERVICE_ID,
                RADIO_INSTANCE_ID,
                RADIO_VOLUME_EVENT_ID,
                its_groups_radio,
                vsomeip::event_type_e::ET_FIELD, std::chrono::milliseconds::zero(),
                false, true, nullptr, vsomeip::reliability_type_e::RT_UNKNOWN);
        app->offer_event(
                RADIO_SERVICE_ID,
                RADIO_INSTANCE_ID,
                RADIO_STATION_EVENT_ID,
                its_groups_radio,
                vsomeip::event_type_e::ET_FIELD, std::chrono::milliseconds::zero(),
                false, true, nullptr, vsomeip::reliability_type_e::RT_UNKNOWN);
        app->offer_event(
                RADIO_SERVICE_ID,
                RADIO_INSTANCE_ID,
                RADIO_SONG_EVENT_ID,
                its_groups_radio,
                vsomeip::event_type_e::ET_FIELD, std::chrono::milliseconds::zero(),
                false, true, nullptr, vsomeip::reliability_type_e::RT_UNKNOWN);
        app->offer_event(
                RADIO_SERVICE_ID,
                RADIO_INSTANCE_ID,
                RADIO_ARTIST_EVENT_ID,
                its_groups_radio,
                vsomeip::event_type_e::ET_FIELD, std::chrono::milliseconds::zero(),
                false, true, nullptr, vsomeip::reliability_type_e::RT_UNKNOWN);

        app->register_availability_handler(ENGINE_SERVICE_ID, ENGINE_INSTANCE_ID,
                std::bind(&radio_service::on_engine_availability, this,
                          std::placeholders::_1, std::placeholders::_2, std::placeholders::_3));
        app->request_service(ENGINE_SERVICE_ID, ENGINE_INSTANCE_ID);
        std::set<vsomeip::eventgroup_t> its_groups;
        its_groups.insert(ENGINE_EVENTGROUP_ID);
        app->request_event(
                ENGINE_SERVICE_ID,
                ENGINE_INSTANCE_ID,
                ENGINE_REVERSE_EVENT_ID,
                its_groups,
                vsomeip::event_type_e::ET_FIELD);
        app->subscribe(ENGINE_SERVICE_ID, ENGINE_INSTANCE_ID, ENGINE_EVENTGROUP_ID);

        app->register_message_handler(
                ENGINE_SERVICE_ID, ENGINE_INSTANCE_ID, ENGINE_REVERSE_EVENT_ID,
                std::bind(&radio_service::on_engine_reverse_event, this,
                        std::placeholders::_1));

        main_blocked = true;
        main_condition.notify_one();
        return true;
    }

    void start() {
        set_playing(true);
        app->start();
    }

    void stop() {
        main_running = false;
        main_blocked = true;
        main_condition.notify_one();
        app->clear_all_handler();
        app->unsubscribe(ENGINE_SERVICE_ID, ENGINE_INSTANCE_ID, ENGINE_EVENTGROUP_ID);
        app->release_event(ENGINE_SERVICE_ID, ENGINE_INSTANCE_ID, ENGINE_REVERSE_EVENT_ID);
        app->release_service(ENGINE_SERVICE_ID, ENGINE_INSTANCE_ID);
        stop_offer();
        main_thread.join();
        app->stop();
    }

    void offer() {
        app->offer_service(RADIO_SERVICE_ID, RADIO_INSTANCE_ID);
        is_offered = true;
    }

    void stop_offer() {
        app->stop_offer_service(RADIO_SERVICE_ID, RADIO_INSTANCE_ID);
    }

    void run() {
        uint32_t current_volume = next_volume;
        bool currently_playing = false;
        bool current_reversed = false;
        uint32_t saved_volume;
        bool next_volume_is_auto = false;
        bool volume_is_auto = false;
        uint32_t current_station = 0xffffffff;
        const radio_station_info_t *station_info = NULL;
        uint32_t current_song = 0;
        bool goto_next_song = true;
        std::chrono::time_point<std::chrono::system_clock> next_song_at;
        std::unique_lock<std::mutex> its_lock(main_mutex);
        std::cout << "RADIO: Started main thread" << std::endl;

        offer();

        while (main_running) {
            bool display = false;

            if (next_reversed != current_reversed) {
                if (next_reversed) {
                    saved_volume = current_volume;
                    next_volume = 30;
                    next_volume_is_auto = true;
                    std::cout << "RADIO: Lowering volume due to reverse" << std::endl;
                } else {
                    if (volume_is_auto) {
                        next_volume = saved_volume;
                        std::cout << "RADIO: Restoring volume due to cancelled reverse" << std::endl;
                    }
                }
                current_reversed = next_reversed;
            }

            if (next_volume != current_volume) {
                current_volume = next_volume;
                volume_is_auto = next_volume_is_auto;
                next_volume_is_auto = false;
                display = true;

                app->notify(RADIO_SERVICE_ID, RADIO_INSTANCE_ID, RADIO_VOLUME_EVENT_ID, payload_with_arg (current_volume));
            }

            if (next_radio_station != current_station) {
                current_station = next_radio_station;
                goto_next_song = true;
                station_info = get_radio_stations(current_station);
                display = true;
                app->notify(RADIO_SERVICE_ID, RADIO_INSTANCE_ID, RADIO_STATION_EVENT_ID, payload_with_string (station_info->name));
            }

            if (next_playing) {
                if (!currently_playing) {
                    std::cout << "RADIO: Started playing" << std::endl;
                    goto_next_song = true;
                }
                currently_playing = true;

                if (goto_next_song) {
                    auto old = current_song;
                    do {
                        current_song = rand() % station_info->n_songs;
                    } while (old == current_song);
                    goto_next_song = false;
                    next_song_at = std::chrono::system_clock::now() + 3s + (rand() % 3000) * 1ms;
                    display = true;
                }

                if (display) {
                    struct song_info_t *song = &station_info->songs[current_song];
                    app->notify(RADIO_SERVICE_ID, RADIO_INSTANCE_ID, RADIO_SONG_EVENT_ID, payload_with_string (song->name));
                    app->notify(RADIO_SERVICE_ID, RADIO_INSTANCE_ID, RADIO_ARTIST_EVENT_ID, payload_with_string (song->artist));
                    std::cout << "RADIO: Playing song \""
                              << song->name << "\" by " << song->artist
                              << " (on " << station_info->name << ") "
                              << std::dec << current_volume << "% volume"
                              << std::endl;
                }
            } else {
                if (currently_playing)
                    std::cout << "RADIO: Paused playing" << std::endl;
                app->notify(RADIO_SERVICE_ID, RADIO_INSTANCE_ID, RADIO_SONG_EVENT_ID, payload_with_string ("-off-"));
                app->notify(RADIO_SERVICE_ID, RADIO_INSTANCE_ID, RADIO_ARTIST_EVENT_ID, payload_with_string (""));
                currently_playing = false;
            }

            if (main_condition.wait_until(its_lock, next_song_at) ==  std::cv_status::timeout) {
                goto_next_song = true;
            }
        }
    }

    void on_message_set_playing(const std::shared_ptr<vsomeip::message> &_request) {
        bool arg = get_arg0(_request) != 0;
        bool was_playing;

        if (verbose)
            std::cout << "RADIO: Received set_playing "
                      << arg
                      << " from Client/Session ["
                      << std::setw(4) << std::setfill('0') << std::hex << _request->get_client() << "/"
                      << std::setw(4) << std::setfill('0') << std::hex << _request->get_session() << "] "
                      << std::endl;

        was_playing = set_playing (arg);

        std::shared_ptr<vsomeip::message> its_response = response (_request, was_playing ? 1 : 0);
        app->send(its_response);
    }

    void on_message_is_playing(const std::shared_ptr<vsomeip::message> &_request) {
        if (verbose)
            std::cout << "RADIO: Received is_playing "
                      << "from Client/Session ["
                      << std::setw(4) << std::setfill('0') << std::hex << _request->get_client() << "/"
                      << std::setw(4) << std::setfill('0') << std::hex << _request->get_session() << "] "
                      << std::endl;

        std::shared_ptr<vsomeip::message> its_response = response (_request, get_playing () ? 1 : 0);
        app->send(its_response);
    }

    void on_message_change_volume(const std::shared_ptr<vsomeip::message> &_request) {
        int32_t arg = (int32_t)get_arg0(_request);
        uint32_t result;

        if (verbose)
            std::cout << "RADIO: Received change_volume "
                      << arg
                      << " from Client/Session ["
                      << std::setw(4) << std::setfill('0') << std::hex << _request->get_client() << "/"
                      << std::setw(4) << std::setfill('0') << std::hex << _request->get_session() << "] "
                      << std::endl;

        result = change_volume (arg);

        std::shared_ptr<vsomeip::message> its_response = response (_request, result);
        app->send(its_response);
    }

    void on_message_get_volume(const std::shared_ptr<vsomeip::message> &_request) {
        if (verbose)
            std::cout << "RADIO: Received get_volume "
                      << "from Client/Session ["
                      << std::setw(4) << std::setfill('0') << std::hex << _request->get_client() << "/"
                      << std::setw(4) << std::setfill('0') << std::hex << _request->get_session() << "] "
                      << std::endl;

        uint32_t volume = get_volume();

        std::shared_ptr<vsomeip::message> its_response = response (_request, volume);
        app->send(its_response);
    }

    void on_message_switch_station(const std::shared_ptr<vsomeip::message> &_request) {
        if (verbose)
            std::cout << "RADIO: Received switch_station "
                      << "from Client/Session ["
                      << std::setw(4) << std::setfill('0') << std::hex << _request->get_client() << "/"
                      << std::setw(4) << std::setfill('0') << std::hex << _request->get_session() << "] "
                      << std::endl;

        switch_station();

        std::shared_ptr<vsomeip::message> its_response = response (_request, 0);
        app->send(its_response);
    }

    void on_engine_reverse_event(const std::shared_ptr<vsomeip::message> &_event) {
        bool reversed = get_arg0(_event) != 0;
        if (verbose)
            std::cout << "RADIO: Received event: "
                      << (reversed ? "engine reversed" : "engine forward")
                      << std::endl;
        set_reversed(reversed);
    }

    void on_engine_availability(vsomeip::service_t _service, vsomeip::instance_t _instance, bool _is_available) {
        std::cout << "Engine Service is "
                << (_is_available ? "available." : "NOT available.")
                << std::endl;

        std::lock_guard<std::mutex> its_lock(main_mutex);
        engine_available = _is_available;
    }

private:
    bool set_playing (bool new_playing) {
        std::unique_lock<std::mutex> its_lock(main_mutex);
        bool old = new_playing;

        if (next_playing != new_playing) {
            next_playing = new_playing;
            main_condition.notify_one();
        }

        return old;
    }

    bool get_playing (void) {
        std::unique_lock<std::mutex> its_lock(main_mutex);
        return next_playing;
    }

    uint32_t change_volume (int32_t delta_volume) {
        std::unique_lock<std::mutex> its_lock(main_mutex);
        int32_t new_volume;

        new_volume = (int32_t)next_volume + delta_volume;
        if (new_volume < 0)
            new_volume = 0;
        else {
            if (new_volume > 100)
                new_volume = 100;
        }

        if (next_volume != new_volume) {
            next_volume = new_volume;
            main_condition.notify_one();
        }

        return new_volume;
    }

    uint32_t get_volume (void) {
        std::unique_lock<std::mutex> its_lock(main_mutex);
        return next_volume;
    }

    void switch_station (void) {
        std::unique_lock<std::mutex> its_lock(main_mutex);

        next_radio_station = (next_radio_station + 1) % n_radio_stations();
        main_condition.notify_one();
    }

    void set_reversed (bool new_reversed) {
        std::unique_lock<std::mutex> its_lock(main_mutex);
        next_reversed = new_reversed;
        main_condition.notify_one();
    }

    std::shared_ptr< vsomeip::application > app;

    std::thread main_thread;
    std::mutex main_mutex;
    std::condition_variable main_condition;
    bool main_blocked;
    bool main_running;
    bool is_offered;
    bool engine_available;

    bool verbose;

    uint32_t next_volume;
    uint32_t next_radio_station;
    bool next_playing;
    bool next_reversed;
};

radio_service *its_radio_ptr(nullptr);
static void handle_signal(int _signal) {
    if (its_radio_ptr != nullptr &&
        (_signal == SIGINT || _signal == SIGTERM))
        its_radio_ptr->stop();
}

int main() {
    radio_service its_radio;

    its_radio_ptr = &its_radio;
    signal(SIGINT, handle_signal);
    signal(SIGTERM, handle_signal);

    if (!its_radio.init()) {
        return 1;
    }

    its_radio.start();

    // Work around dlt-daemon timeout on exit
    _exit(0);
}
