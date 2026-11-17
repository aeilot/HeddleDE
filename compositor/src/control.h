// 控制套接字：Unix 流套接字，每行一条命令，每条回一行 JSON。
// 同一连接上的命令按顺序执行；截图会挂起该连接直到完成。命令本身见 commands.c。
#ifndef HEDDLE_CONTROL_H
#define HEDDLE_CONTROL_H

#include "heddle.h"

struct conn;
struct sbuf;

// 在 runtime_dir/control 上监听，设置 srv->ctl_path / ctl_fd / ctl_src。
bool control_listen(struct server *srv);

// 发出 b 加换行并清空 b。
void conn_send(struct conn *c, struct sbuf *b);
void conn_reply_ok(struct conn *c);
void conn_reply_error(struct conn *c, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
// 挂起的命令已回复：继续处理该连接上排队的行（连接可能因此被释放）。
void conn_unblock(struct conn *c);

#endif
