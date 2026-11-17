// 截图：放行帧时钟，等画面稳定（或超时）后把最后一帧写成 PNG，再回复控制连接。
//
// 每 20 ms 检查一次：自上次有效损坏起已过 settle_ms、且没有待合成的损坏，判为 stable；
// 超过 timeout_ms 则按 timeout 返回当前帧。
#ifndef HEDDLE_SCREENSHOT_H
#define HEDDLE_SCREENSHOT_H

#include "heddle.h"

struct conn;

struct shot {
	struct conn *conn; // 等回复的控制连接；连接断开后为 NULL，截图照常完成
	char path[512];
	int settle_ms, timeout_ms;
	uint64_t threshold_px; // 单次提交损坏不超过此面积时不算画面变化
	int64_t start_ms;
	uint64_t frames_at_start;
};

void screenshot_init_agent(struct agent *a);

// 开始截图。调用方先确认 a->shot 为 NULL。
void shot_start(struct agent *a, struct conn *c, const char *path,
	int settle_ms, int timeout_ms, uint64_t threshold_px);
// 每帧之后调用：满足稳定或超时条件就完成截图。
void shot_try_finish(struct agent *a);
// 控制连接关闭时调用：它挂起的截图不再回复。
void shot_detach_conn(struct server *srv, struct conn *c);

#endif
