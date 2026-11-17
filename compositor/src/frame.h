// 出帧策略与表面提交统计。
//
// always：照常出帧。observe：只有"放行"时才合成、才发 wl_surface.frame 回调；
// 每个输入动作放行 grace_ms，截图期间也放行。停帧时帧事件丢弃并计入 frames_skipped。
#ifndef HEDDLE_FRAME_H
#define HEDDLE_FRAME_H

#include "heddle.h"

// 注册 new_surface，统计每次提交的损坏面积。
void frame_init_server(struct server *srv);
// 挂上输出的帧事件、建放行计时器，并放行一个 grace 期让应用画出首帧。
void frame_init_agent(struct agent *a);

// 按当前策略、截图状态和放行期重新决定是否放行。
void agent_update_running(struct agent *a);
// 输入动作之后调用：放行 grace_ms。
void agent_note_action(struct agent *a);
// 放行至少 ms 毫秒（已有更晚的截止时间则保留）。
void agent_run_for(struct agent *a, int ms);
// 立即结束放行期（截图中仍放行）。
void agent_gate(struct agent *a);

#endif
