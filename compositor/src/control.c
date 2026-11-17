#include "control.h"

#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>
#include <wlr/util/log.h>

#include "commands.h"
#include "json.h"
#include "screenshot.h"
#include "util.h"

#define MAX_LINE 65536

struct conn {
	struct server *srv;
	int fd;
	struct wl_event_source *src;
	char buf[MAX_LINE]; // 已读未处理的字节
	size_t len;
	bool busy; // 有命令挂起（截图在等稳定），暂停处理后续行
	bool dead;
};

/* ---------- 回复 ---------- */

void conn_send(struct conn *c, struct sbuf *b) {
	sb_putn(b, "\n", 1);
	size_t off = 0;
	while (off < b->len && !c->dead) {
		ssize_t n = write(c->fd, b->data + off, b->len - off);
		if (n < 0) {
			if (errno == EINTR) {
				continue;
			}
			c->dead = true;
			break;
		}
		off += n;
	}
	sb_free(b);
}

void conn_reply_error(struct conn *c, const char *fmt, ...) {
	char msg[512];
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(msg, sizeof(msg), fmt, ap);
	va_end(ap);
	struct sbuf b = {0};
	sb_printf(&b, "{\"ok\":false,\"error\":");
	sb_json_str(&b, msg);
	sb_printf(&b, "}");
	conn_send(c, &b);
}

void conn_reply_ok(struct conn *c) {
	struct sbuf b = {0};
	sb_printf(&b, "{\"ok\":true}");
	conn_send(c, &b);
}

/* ---------- 连接生命周期 ---------- */

static void conn_destroy(struct conn *c) {
	shot_detach_conn(c->srv, c); // 挂起的截图照常完成，只是不回复
	wl_event_source_remove(c->src);
	close(c->fd);
	free(c);
}

// 逐行执行缓冲区里的命令，直到没有完整的行、或有命令挂起。
static void conn_process(struct conn *c) {
	while (!c->busy && !c->dead) {
		char *nl = memchr(c->buf, '\n', c->len);
		if (!nl) {
			break;
		}
		*nl = '\0';
		size_t consumed = nl - c->buf + 1;
		char line[MAX_LINE];
		memcpy(line, c->buf, consumed);
		memmove(c->buf, c->buf + consumed, c->len - consumed);
		c->len -= consumed;
		size_t ll = strlen(line);
		if (ll && line[ll - 1] == '\r') {
			line[ll - 1] = '\0';
		}
		if (!commands_dispatch(c, c->srv, line)) {
			c->busy = true;
		}
	}
}

void conn_unblock(struct conn *c) {
	c->busy = false;
	conn_process(c);
	if (c->dead) {
		conn_destroy(c);
	}
}

static int handle_conn(int fd, uint32_t mask, void *data) {
	struct conn *c = data;
	if (mask & (WL_EVENT_HANGUP | WL_EVENT_ERROR)) {
		c->dead = true;
	} else {
		ssize_t r = read(fd, c->buf + c->len, sizeof(c->buf) - 1 - c->len);
		if (r <= 0) {
			c->dead = true;
		} else {
			c->len += r;
			if (c->len >= sizeof(c->buf) - 1) {
				conn_reply_error(c, "line too long");
				c->dead = true;
			}
		}
	}
	if (!c->dead) {
		conn_process(c);
	}
	if (c->dead) {
		conn_destroy(c);
	}
	return 0;
}

static int handle_ctl_accept(int fd, uint32_t mask, void *data) {
	struct server *srv = data;
	int cfd = accept4(fd, NULL, NULL, SOCK_CLOEXEC);
	if (cfd < 0) {
		return 0;
	}
	struct conn *c = calloc(1, sizeof(*c));
	c->srv = srv;
	c->fd = cfd;
	c->src = wl_event_loop_add_fd(srv->loop, cfd, WL_EVENT_READABLE, handle_conn, c);
	return 0;
}

bool control_listen(struct server *srv) {
	snprintf(srv->ctl_path, sizeof(srv->ctl_path), "%s/control", srv->runtime_dir);
	srv->ctl_fd = listen_unix(srv->ctl_path);
	if (srv->ctl_fd < 0) {
		wlr_log_errno(WLR_ERROR, "listen %s", srv->ctl_path);
		return false;
	}
	srv->ctl_src = wl_event_loop_add_fd(srv->loop, srv->ctl_fd, WL_EVENT_READABLE,
		handle_ctl_accept, srv);
	return true;
}
