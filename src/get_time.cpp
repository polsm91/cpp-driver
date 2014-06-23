#include "get_time.hpp"

#if defined(WIN32) || defined(_WIN32)
#include <Windows.h>
#elif defined(__APPLE__) && defined(__MACH__)
#include <mach/mach.h>
#include <mach/clock.h>
#else
#include <time.h>
#endif

namespace cass {

#if defined(WIN32) || defined(_WIN32)

uint64_t get_time_since_epoch() {
  _FILETIME ft;
  GetSystemTimeAsFileTime(&ft);
  uint64_t ns100 = (static_cast<uint64_t>(ft.dwHighDateTime) << 32 | static_cast<uint64_t>(ft.dwHighDateTime))
    - 116444736000000000LL; // 100 nanosecond increments between Jan. 1, 1601 - Jan. 1, 1970
  return ns100 / 10000; // 100 nanoseconds to milliseconds
}

#elif defined(__APPLE__) && defined(__MACH__)

uint64_t get_time_since_epoch() {
  clock_serv_t clock_serv;
  mach_timespec_t ts;
  host_get_clock_service(mach_host_self(), CALENDAR_CLOCK, &clock_serv);
  clock_get_time(clock_serv, &ts);
  mach_port_deallocate(mach_task_self(), clock_serv);
  return ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

#else

uint64_t get_time_since_epoch() {
  struct timespec ts;
  clock_gettime(CLOCK_REALTIME, &ts);
  return ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

#endif

}
