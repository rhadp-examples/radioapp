#include <cstdint>

struct song_info_t {
  const char *artist;
  const char *name;
  uint32_t year;
};

struct radio_station_info_t {
  const char *name;
  song_info_t *songs;
  uint32_t n_songs;
  uint32_t id;
};

void radio_stations_init(void);
uint32_t n_radio_stations(void);
const radio_station_info_t *get_radio_stations(uint32_t id);
