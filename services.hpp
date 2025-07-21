#include <vsomeip/vsomeip.hpp>
#include <sstream>

#define RADIO_SERVICE_ID 0x1000
#define RADIO_INSTANCE_ID 1

#define RADIO_IS_PLAYING_METHOD_ID 1
#define RADIO_SET_PLAYING_METHOD_ID 2
#define RADIO_GET_VOLUME_METHOD_ID 3
#define RADIO_CHANGE_VOLUME_METHOD_ID 4
#define RADIO_SWITCH_STATION_METHOD_ID 5

#define RADIO_EVENTGROUP_ID 0x4001
#define RADIO_VOLUME_EVENT_ID 0x8000 + 1
#define RADIO_STATION_EVENT_ID 0x8000 + 2
#define RADIO_SONG_EVENT_ID 0x8000 + 3
#define RADIO_ARTIST_EVENT_ID 0x8000 + 4

#define ENGINE_SERVICE_ID 0x1001
#define ENGINE_INSTANCE_ID 1

#define ENGINE_GET_REVERSE_METHOD_ID 1

#define ENGINE_EVENTGROUP_ID 0x4002
#define ENGINE_REVERSE_EVENT_ID 0x8000 + 1

static uint32_t inline
payload_get_arg(std::shared_ptr<vsomeip::payload> its_payload)
{
  vsomeip::byte_t *data = its_payload->get_data();
  vsomeip::length_t l = its_payload->get_length();

  if (l < 4)
    return 0;

  return (uint32_t)data[0]<< 24 | data[1]<< 16 | data[2]<< 8 | data[3];
}

static std::string inline
payload_get_string(std::shared_ptr<vsomeip::payload> its_payload)
{
  vsomeip::byte_t *data = its_payload->get_data();
  vsomeip::length_t l = its_payload->get_length();
  std::stringstream ss;

  for (vsomeip::length_t i=0; i<l; i++)
    ss << (unsigned char)data[i];

  return ss.str();
}


static std::shared_ptr<vsomeip::payload> inline
payload_with_arg(uint32_t arg)
{
  std::shared_ptr<vsomeip::payload> its_payload = vsomeip::runtime::get()->create_payload();
  std::vector<vsomeip::byte_t> its_payload_data;
  its_payload_data.push_back((arg >> 24) & 0xff);
  its_payload_data.push_back((arg >> 16) & 0xff);
  its_payload_data.push_back((arg >> 8) & 0xff);
  its_payload_data.push_back((arg >> 0) & 0xff);
  its_payload->set_data(its_payload_data);

  return its_payload;
}

static std::shared_ptr<vsomeip::payload> inline
payload_with_string(std::string arg)
{
  std::shared_ptr<vsomeip::payload> its_payload = vsomeip::runtime::get()->create_payload();
  std::vector<vsomeip::byte_t> its_payload_data;
  for (vsomeip::byte_t i=0; i<arg.size(); i++) {
      its_payload_data.push_back(arg[i]);
  }
  its_payload->set_data(its_payload_data);

  return its_payload;
}

static std::shared_ptr< vsomeip::message > inline
new_request (uint32_t method, uint32_t arg)
{
  std::shared_ptr< vsomeip::message > request;
  request = vsomeip::runtime::get()->create_request();
  request->set_service(RADIO_SERVICE_ID);
  request->set_instance(RADIO_INSTANCE_ID);
  request->set_method(method);

  std::shared_ptr<vsomeip::payload> its_payload = payload_with_arg (arg);
  request->set_payload(its_payload);

  return request;
}

static uint32_t inline
get_arg0(const std::shared_ptr<vsomeip::message> _request) {
    std::shared_ptr<vsomeip::payload> its_payload = _request->get_payload();

    return payload_get_arg(its_payload);
}

static std::string inline
get_arg0_string(const std::shared_ptr<vsomeip::message> _request) {
    std::shared_ptr<vsomeip::payload> its_payload = _request->get_payload();

    return payload_get_string(its_payload);
}

static std::shared_ptr<vsomeip::message> inline
response (const std::shared_ptr<vsomeip::message> &_request,
          uint32_t arg)
{
    std::shared_ptr<vsomeip::message> its_response = vsomeip::runtime::get()->create_response(_request);
    std::shared_ptr<vsomeip::payload> its_payload = payload_with_arg (arg);

    its_response->set_payload(its_payload);

    return its_response;
}
