#include <iomanip>
#include <iostream>
#include <condition_variable>
#include <thread>
#include <stdio.h>
#include <unistd.h>
#include <termios.h>

#include <vsomeip/vsomeip.hpp>

using namespace std::literals::chrono_literals;

#include "services.hpp"

std::shared_ptr< vsomeip::application > app;
std::mutex mutex;
std::condition_variable condition;

bool running = true;
bool data_changed = false;
bool service_available = false;
bool in_reverse = false;
std::string radio_station = "";
std::string song = "";
std::string artist = "";
uint32_t volume = 0;
bool sent_off = false;

static std::string volume_meter(uint32_t volume)
{
    std::string vol_str = "";
    for (uint32_t i = 0; i <= 20; i++) {
      if (volume > i * 5 || volume == 100)
        vol_str += "#";
      else
        vol_str += " ";
    }
    return vol_str;
}

void on_volume_message(const std::shared_ptr<vsomeip::message> &_event) {
    std::unique_lock<std::mutex> its_lock(mutex);
    volume = get_arg0(_event);
    data_changed = true;
    condition.notify_one();
}

void on_song_message(const std::shared_ptr<vsomeip::message> &_event) {
    std::unique_lock<std::mutex> its_lock(mutex);
    song = get_arg0_string(_event);
    data_changed = true;
    condition.notify_one();
}

void on_artist_message(const std::shared_ptr<vsomeip::message> &_event) {
    std::unique_lock<std::mutex> its_lock(mutex);
    artist = get_arg0_string(_event);
    data_changed = true;
    condition.notify_one();
}

void on_station_message(const std::shared_ptr<vsomeip::message> &_event) {
    radio_station = get_arg0_string(_event);
    data_changed = true;
    condition.notify_one();
}

void input_thread() {
    while (true) {
        int c = getchar();

        switch (c) {
        case 'q':
            {
                std::unique_lock<std::mutex> its_lock(mutex);
                running = false;
                condition.notify_one();
                app->stop();
            }
            return; /* End thread */
            break;
        case '+':
            {
                std::shared_ptr< vsomeip::message > request = new_request (RADIO_CHANGE_VOLUME_METHOD_ID, 10);
                app->send(request);
            }
            break;
        case '-':
            {
                std::shared_ptr< vsomeip::message > request = new_request (RADIO_CHANGE_VOLUME_METHOD_ID, (uint32_t)(int32_t)-10);
                app->send(request);
            }
            break;
        case ' ':
            {
                std::shared_ptr< vsomeip::message > request = new_request (RADIO_SWITCH_STATION_METHOD_ID, 0);
                app->send(request);
            }
            break;
        case '\e':
            {
                std::shared_ptr< vsomeip::message > request = new_request (RADIO_SET_PLAYING_METHOD_ID, sent_off ? 1 : 0);
                sent_off = !sent_off;
                app->send(request);
            }
            break;
        default:
            // pass
            break;
        }
    }
}

void run() {
    std::unique_lock<std::mutex> its_lock(mutex);

    app->register_message_handler(
                RADIO_SERVICE_ID, RADIO_INSTANCE_ID, RADIO_VOLUME_EVENT_ID,
                on_volume_message);
    app->register_message_handler(
                RADIO_SERVICE_ID, RADIO_INSTANCE_ID, RADIO_STATION_EVENT_ID,
                on_station_message);
    app->register_message_handler(
                RADIO_SERVICE_ID, RADIO_INSTANCE_ID, RADIO_SONG_EVENT_ID,
                on_song_message);
    app->register_message_handler(
                RADIO_SERVICE_ID, RADIO_INSTANCE_ID, RADIO_ARTIST_EVENT_ID,
                on_artist_message);

    std::set<vsomeip::eventgroup_t> its_groups;
    its_groups.insert(RADIO_EVENTGROUP_ID);
    app->request_event(RADIO_SERVICE_ID,
                       RADIO_INSTANCE_ID,
                       RADIO_SONG_EVENT_ID,
                       its_groups,
                       vsomeip::event_type_e::ET_FIELD);
    app->request_event(RADIO_SERVICE_ID,
                       RADIO_INSTANCE_ID,
                       RADIO_ARTIST_EVENT_ID,
                       its_groups,
                       vsomeip::event_type_e::ET_FIELD);
    app->request_event(RADIO_SERVICE_ID,
                       RADIO_INSTANCE_ID,
                       RADIO_VOLUME_EVENT_ID,
                       its_groups,
                       vsomeip::event_type_e::ET_FIELD);
    app->request_event(RADIO_SERVICE_ID,
                       RADIO_INSTANCE_ID,
                       RADIO_STATION_EVENT_ID,
                       its_groups,
                       vsomeip::event_type_e::ET_FIELD);
    app->subscribe(RADIO_SERVICE_ID, RADIO_INSTANCE_ID, RADIO_EVENTGROUP_ID);

    while (running) {
        if (data_changed) {
            std::cout
                << "\e[s"       // Save cursor
                << "\e[6;1H"   // Goto row 6
                << "\e[1J"     // Clear to start of screen
                << "\e[1;1H"   // Goto row 1;
              ;
            if (service_available) {
                std::cout
                    << "Station: " << radio_station << std::endl
                    << "Song:    " << song << std::endl
                    << "Artist:  " << artist << std::endl
                    << "Volume:  [" << volume_meter(volume) << "] " << std::endl
                    << (in_reverse ?
                        "------------------------------[REVERSING]-" :
                        "------------------------------------------")
                    << std::endl;
            } else {
                std::cout
                    << "Connecting to radio service";
            }
            std::cout
                << "\e[u"        // Restore cursor
                << std::flush;
            data_changed = false;
        }

        condition.wait_for(its_lock, 5s);
    }
}


void on_availability(vsomeip::service_t _service, vsomeip::instance_t _instance, bool _is_available) {
    std::unique_lock<std::mutex> its_lock(mutex);
    service_available = _is_available;
    data_changed = true;
    condition.notify_one();
}

void on_engine_availability(vsomeip::service_t _service, vsomeip::instance_t _instance, bool _is_available) {
    std::unique_lock<std::mutex> its_lock(mutex);
    if (!_is_available)
        {
            in_reverse = false;
            data_changed = true;
            condition.notify_one();
        }
}

void on_engine_reverse_event(const std::shared_ptr<vsomeip::message> &_event) {
    std::unique_lock<std::mutex> its_lock(mutex);
    bool reversed = get_arg0(_event) != 0;

    in_reverse = reversed;
    data_changed = true;
    condition.notify_one();
}

int main(int argc, const char *argv[]) {
    struct termios old_tio, new_tio;
    unsigned char c;

    tcgetattr(STDIN_FILENO,&old_tio);
    new_tio=old_tio;

    /* disable canonical mode (buffered i/o) and local echo */
    new_tio.c_lflag &=(~ICANON & ~ECHO);
    tcsetattr(STDIN_FILENO,TCSANOW,&new_tio);

    std::cout
      << "\e[2J"     // Clear screen
      << "\e[1;1H"  // Goto row 1
      << "\e[6;1H"  // Goto row 6
      << " Usage: volume: +/-, station: SPACE on/off: ESC, quit: Q" << std::endl;

    app = vsomeip::runtime::get()->create_application("Radio client");
    app->init();
    app->register_availability_handler(RADIO_SERVICE_ID, RADIO_INSTANCE_ID, on_availability);
    app->request_service(RADIO_SERVICE_ID, RADIO_INSTANCE_ID);

    app->register_availability_handler(ENGINE_SERVICE_ID, ENGINE_INSTANCE_ID, on_engine_availability);
    app->request_service(ENGINE_SERVICE_ID, ENGINE_INSTANCE_ID);
    std::set<vsomeip::eventgroup_t> its_groups;
    its_groups.insert(ENGINE_EVENTGROUP_ID);
    app->request_event(ENGINE_SERVICE_ID,
                       ENGINE_INSTANCE_ID,
                       ENGINE_REVERSE_EVENT_ID,
                       its_groups,
                       vsomeip::event_type_e::ET_FIELD);
    app->subscribe(ENGINE_SERVICE_ID, ENGINE_INSTANCE_ID, ENGINE_EVENTGROUP_ID);

    app->register_message_handler(ENGINE_SERVICE_ID, ENGINE_INSTANCE_ID, ENGINE_REVERSE_EVENT_ID,
                                  on_engine_reverse_event);

    std::thread input(input_thread);
    std::thread sender(run);
    app->start();

    sender.join();
    input.join();


    app->clear_all_handler();
    app->release_service(RADIO_SERVICE_ID, RADIO_INSTANCE_ID);
    app->release_service(ENGINE_SERVICE_ID, ENGINE_INSTANCE_ID);

    tcsetattr(STDIN_FILENO,TCSANOW,&old_tio);

    // Work around dlt-daemon timeout on exit
    _exit(0);
    return 0;
}
