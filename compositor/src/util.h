// 杂项：时钟、损坏面积、Unix 监听套接字。
#ifndef HEDDLE_UTIL_H
#define HEDDLE_UTIL_H

#include <pixman.h>
#include <stdint.h>

int64_t now_ms(void);     // CLOCK_MONOTONIC，毫秒
uint32_t now_msec32(void); // 输入事件的时间戳

uint64_t region_area(pixman_region32_t *r);

// 建一个只有本用户可连的 Unix 流套接字并开始监听；失败返回 -1 并设置 errno。
int listen_unix(const char *path);

#endif
