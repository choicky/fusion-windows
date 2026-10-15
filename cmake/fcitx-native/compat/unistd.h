#pragma once
#include <direct.h>
#include <fcntl.h>
#include <io.h>
#include <process.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#ifndef _SSIZE_T_DEFINED
#define _SSIZE_T_DEFINED
typedef intptr_t ssize_t;
#endif
#ifndef _PID_T_DEFINED
#define _PID_T_DEFINED
typedef int pid_t;
#endif
typedef int mode_t;
typedef int clockid_t;
#define CLOCK_REALTIME 0
#define CLOCK_MONOTONIC 1
#ifndef S_ISREG
#define S_ISREG(m) (((m) & _S_IFMT) == _S_IFREG)
#define S_ISDIR(m) (((m) & _S_IFMT) == _S_IFDIR)
#endif
#ifndef R_OK
#define R_OK 4
#define W_OK 2
#define X_OK 0
#define F_OK 0
#endif
#ifdef FUSION_PLATFORM_BUILD
#define FUSION_PLATFORM_API __declspec(dllexport)
#else
#define FUSION_PLATFORM_API __declspec(dllimport)
#endif
#ifdef __cplusplus
extern "C" {
#endif
FUSION_PLATFORM_API int fusion_win_clock_gettime(clockid_t id, struct timespec *ts);
FUSION_PLATFORM_API int fusion_win_mkstemp(char *name);
FUSION_PLATFORM_API unsigned fusion_win_sleep(unsigned seconds);
#ifdef __cplusplus
}
#define LIBIMECORE_DEPRECATED [[deprecated]]
#endif
#define clock_gettime fusion_win_clock_gettime
#define mkstemp fusion_win_mkstemp
#define sleep fusion_win_sleep
