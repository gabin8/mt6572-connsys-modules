#ifndef _COMPAT_CUTILS_LOG_H
#define _COMPAT_CUTILS_LOG_H
#include <stdio.h>
#define ALOGI(...) do { printf("[I]" __VA_ARGS__); printf("\n"); } while (0)
#define ALOGW(...) do { printf("[W]" __VA_ARGS__); printf("\n"); } while (0)
#define ALOGE(...) do { fprintf(stderr, "[E]" __VA_ARGS__); fprintf(stderr, "\n"); } while (0)
#define ALOGD(...) do { printf("[D]" __VA_ARGS__); printf("\n"); } while (0)
#define ALOGV(...)
#endif
