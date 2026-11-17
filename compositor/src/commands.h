// 控制命令的解析与执行。协议说明见 compositor/README.md。
#ifndef HEDDLE_COMMANDS_H
#define HEDDLE_COMMANDS_H

#include "heddle.h"

struct conn;

// 执行一行命令并回复。返回 false 表示命令挂起（截图），稍后由 conn_unblock 继续。
// line 会被就地改写。
bool commands_dispatch(struct conn *c, struct server *srv, char *line);

#endif
