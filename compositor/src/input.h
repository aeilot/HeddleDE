// 输入注入：每个 agent 一个 seat（名字即 agent 名）、一个虚拟键盘和指针。
// 只由控制命令调用，不提供 virtual-keyboard / virtual-pointer 协议。
#ifndef HEDDLE_INPUT_H
#define HEDDLE_INPUT_H

#include <stddef.h>

#include "heddle.h"

// 建 seat 和键盘（默认 xkb 键位表），指针放在输出中央；剪贴板按 seat 分开。
void input_init_agent(struct agent *a);

void pointer_move(struct agent *a, double x, double y); // 输出坐标，越界会夹到边上
void pointer_button(struct agent *a, uint32_t button, bool pressed); // 按下时聚焦所点窗口
void pointer_scroll(struct agent *a, int dx, int dy); // 滚轮格数，正数向下/向右

// 键名为 xkb keysym 名，另认 ctrl/shift/alt/super/enter/esc 等别名。
bool name_to_key(struct agent *a, const char *name, uint32_t *evdev, bool *shift);
void key_event(struct agent *a, uint32_t evdev_code, bool pressed);
// "ctrl+shift+t"：依次按下，再倒序松开。
bool key_combo(struct agent *a, const char *combo, char *err, size_t errlen);
// 逐字符敲入 UTF-8 文本，返回键位表里没有、因而跳过的字符数。
int type_text(struct agent *a, const char *text);

#endif
