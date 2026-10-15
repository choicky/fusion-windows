#include <errno.h>
#include <share.h>
#include <string.h>
#include <windows.h>
extern "C" int fusion_win_clock_gettime(clockid_t id, struct timespec *ts) {
  if (!ts) {
    errno = EINVAL;
    return -1;
  }
  if (id == CLOCK_MONOTONIC) {
    LARGE_INTEGER counter, frequency;
    if (!QueryPerformanceCounter(&counter) || !QueryPerformanceFrequency(&frequency)) {
      errno = EINVAL;
      return -1;
    }
    ts->tv_sec = (time_t)(counter.QuadPart / frequency.QuadPart);
    ts->tv_nsec =
        (long)((counter.QuadPart % frequency.QuadPart) * 1000000000LL / frequency.QuadPart);
    return 0;
  }
  if (id == CLOCK_REALTIME) {
    FILETIME ft;
    ULARGE_INTEGER value;
    GetSystemTimePreciseAsFileTime(&ft);
    value.LowPart = ft.dwLowDateTime;
    value.HighPart = ft.dwHighDateTime;
    value.QuadPart -= 116444736000000000ULL;
    ts->tv_sec = (time_t)(value.QuadPart / 10000000ULL);
    ts->tv_nsec = (long)((value.QuadPart % 10000000ULL) * 100ULL);
    return 0;
  }
  errno = EINVAL;
  return -1;
}
extern "C" int fusion_win_mkstemp(char *name) {
  int fd = -1;
  if (_mktemp_s(name, strlen(name) + 1) != 0)
    return -1;
  if (_sopen_s(&fd, name, _O_CREAT | _O_EXCL | _O_RDWR | _O_BINARY, _SH_DENYRW,
               _S_IREAD | _S_IWRITE) != 0)
    return -1;
  return fd;
}
extern "C" unsigned fusion_win_sleep(unsigned seconds) {
  while (seconds--)
    Sleep(1000);
  return 0;
}
