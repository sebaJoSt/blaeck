#include <Blaeck.h>

struct AvailableOnlyServer
{
  Client &available();
};

void mustNotCompile(AvailableOnlyServer &server)
{
  Blaeck device;
  device.begin(server);
}
