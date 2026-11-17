#include "commands.h"

#include <linux/input-event-codes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wlr/types/wlr_output.h>
#include <wlr/types/wlr_scene.h>
#include <wlr/types/wlr_seat.h>
#include <wlr/types/wlr_xdg_shell.h>

#include "agent.h"
#include "control.h"
#include "frame.h"
#include "input.h"
#include "json.h"
#include "screenshot.h"
#include "window.h"

#define MAX_ARGS 32

/* ---------- 参数小工具 ---------- */

static uint32_t button_code(const char *name) {
	if (!name || !strcmp(name, "left") || !strcmp(name, "1")) return BTN_LEFT;
	if (!strcmp(name, "right") || !strcmp(name, "3")) return BTN_RIGHT;
	if (!strcmp(name, "middle") || !strcmp(name, "2")) return BTN_MIDDLE;
	return 0;
}

// 在 args 里找 "key=value"，返回 value；没有则 NULL。
static const char *kv(char **args, int nargs, const char *key) {
	size_t kl = strlen(key);
	for (int i = 0; i < nargs; i++) {
		if (!strncmp(args[i], key, kl) && args[i][kl] == '=') {
			return args[i] + kl + 1;
		}
	}
	return NULL;
}

static int kv_int(char **args, int nargs, const char *key, int def) {
	const char *v = kv(args, nargs, key);
	return v ? atoi(v) : def;
}

// 就地解开 \n \t \\ 转义。
static void unescape(char *s) {
	char *w = s;
	for (char *r = s; *r; r++) {
		if (*r == '\\' && r[1]) {
			r++;
			*w++ = *r == 'n' ? '\n' : *r == 't' ? '\t' : *r;
		} else {
			*w++ = *r;
		}
	}
	*w = '\0';
}

/* ---------- 不针对 agent 的命令 ---------- */

static void cmd_agents(struct conn *c, struct server *srv) {
	struct sbuf b = {0};
	sb_printf(&b, "{\"ok\":true,\"policy\":\"%s\",\"grace_ms\":%d,\"suspend\":%s,\"agents\":[",
		srv->policy == POLICY_ALWAYS ? "always" : "observe", srv->grace_ms,
		srv->use_suspend ? "true" : "false");
	bool first = true;
	struct agent *a;
	wl_list_for_each(a, &srv->agents, link) {
		sb_printf(&b, "%s{\"name\":", first ? "" : ",");
		sb_json_str(&b, a->name);
		sb_printf(&b, ",\"socket\":");
		sb_json_str(&b, a->sock_path);
		sb_printf(&b, ",\"output\":");
		sb_json_str(&b, a->output->name);
		sb_printf(&b, ",\"width\":%d,\"height\":%d}", a->output->width, a->output->height);
		first = false;
	}
	sb_printf(&b, "]}");
	conn_send(c, &b);
}

// type AGENT TEXT：行内剩余部分整体作为文本，所以不走空格分词。
static void cmd_type(struct conn *c, struct server *srv, char *rest) {
	char *sp = strchr(rest, ' ');
	if (!sp) {
		conn_reply_error(c, "usage: type AGENT TEXT");
		return;
	}
	*sp = '\0';
	struct agent *a = agent_by_name(srv, rest);
	if (!a) {
		conn_reply_error(c, "no agent '%s'", rest);
		return;
	}
	char *text = sp + 1;
	unescape(text);
	int missing = type_text(a, text);
	agent_note_action(a);
	struct sbuf b = {0};
	sb_printf(&b, "{\"ok\":true,\"missing\":%d}", missing);
	conn_send(c, &b);
}

/* ---------- 针对某个 agent 的命令 ---------- */
// 处理函数返回 false 表示挂起。args[0] 是命令名，args[1] 是 agent 名。

static bool cmd_screenshot(struct conn *c, struct agent *a, char **args, int n) {
	if (a->shot) {
		conn_reply_error(c, "screenshot already pending for %s", a->name);
		return true;
	}
	const char *thr = kv(args, n, "threshold");
	shot_start(a, c, args[2],
		kv_int(args, n, "settle", 300),
		kv_int(args, n, "timeout", 5000),
		thr ? strtoull(thr, NULL, 10) : 4096); // 默认忽略小于 64x64 的损坏（光标闪烁）
	return false;
}

static bool cmd_move(struct conn *c, struct agent *a, char **args, int n) {
	pointer_move(a, atof(args[2]), atof(args[3]));
	agent_note_action(a);
	conn_reply_ok(c);
	return true;
}

static bool cmd_click(struct conn *c, struct agent *a, char **args, int n) {
	uint32_t btn = button_code(kv(args, n, "button"));
	if (!btn) {
		conn_reply_error(c, "bad button");
		return true;
	}
	int count = kv_int(args, n, "count", 1);
	pointer_move(a, atof(args[2]), atof(args[3]));
	for (int i = 0; i < count; i++) {
		pointer_button(a, btn, true);
		pointer_button(a, btn, false);
	}
	agent_note_action(a);
	conn_reply_ok(c);
	return true;
}

static bool cmd_button(struct conn *c, struct agent *a, char **args, int n) {
	uint32_t btn = button_code(args[2]);
	if (!btn) {
		conn_reply_error(c, "bad button");
		return true;
	}
	pointer_button(a, btn, !strcmp(args[3], "down"));
	agent_note_action(a);
	conn_reply_ok(c);
	return true;
}

static bool cmd_scroll(struct conn *c, struct agent *a, char **args, int n) {
	pointer_scroll(a, atoi(args[2]), atoi(args[3]));
	agent_note_action(a);
	conn_reply_ok(c);
	return true;
}

static bool cmd_key(struct conn *c, struct agent *a, char **args, int n) {
	char err[128];
	bool ok = key_combo(a, args[2], err, sizeof(err));
	agent_note_action(a);
	if (ok) {
		conn_reply_ok(c);
	} else {
		conn_reply_error(c, "%s", err);
	}
	return true;
}

static bool key_updown(struct conn *c, struct agent *a, const char *name, bool pressed) {
	uint32_t code;
	bool shift;
	bool ok = name_to_key(a, name, &code, &shift);
	if (ok) {
		key_event(a, code, pressed);
	}
	agent_note_action(a);
	if (ok) {
		conn_reply_ok(c);
	} else {
		conn_reply_error(c, "unknown key '%s'", name);
	}
	return true;
}

static bool cmd_keydown(struct conn *c, struct agent *a, char **args, int n) {
	return key_updown(c, a, args[2], true);
}

static bool cmd_keyup(struct conn *c, struct agent *a, char **args, int n) {
	return key_updown(c, a, args[2], false);
}

static bool cmd_windows(struct conn *c, struct agent *a, char **args, int n) {
	struct sbuf b = {0};
	sb_printf(&b, "{\"ok\":true,\"windows\":[");
	bool first = true;
	struct toplevel *t;
	wl_list_for_each(t, &a->toplevels, link) {
		if (!t->mapped) {
			continue;
		}
		int x = 0, y = 0;
		wlr_scene_node_coords(&t->tree->node, &x, &y);
		struct wlr_box geo;
		wlr_xdg_surface_get_geometry(t->xdg->base, &geo);
		struct client_info *ci = client_lookup(a->srv, wl_resource_get_client(t->xdg->resource));
		sb_printf(&b, "%s{\"id\":%u,\"pid\":%d,\"app_id\":", first ? "" : ",", t->id, ci ? ci->pid : -1);
		sb_json_str(&b, t->xdg->app_id);
		sb_printf(&b, ",\"title\":");
		sb_json_str(&b, t->xdg->title);
		// x,y 为窗口几何框（去掉客户端阴影后）在输出上的位置；
		// surface_x/y 为表面左上角，客户端自绘阴影时二者不同。
		sb_printf(&b, ",\"x\":%d,\"y\":%d,\"width\":%d,\"height\":%d,"
			"\"surface_x\":%d,\"surface_y\":%d,\"focused\":%s}",
			x, y, geo.width, geo.height, x - geo.x, y - geo.y,
			a->seat->keyboard_state.focused_surface == t->xdg->base->surface ? "true" : "false");
		first = false;
	}
	sb_printf(&b, "]}");
	conn_send(c, &b);
	return true;
}

static bool cmd_stats(struct conn *c, struct agent *a, char **args, int n) {
	bool reset = n >= 3 && !strcmp(args[2], "reset");
	struct sbuf b = {0};
	sb_printf(&b, "{\"ok\":true,\"agent\":");
	sb_json_str(&b, a->name);
	sb_printf(&b, ",\"running\":%s,\"frames_rendered\":%llu,\"frames_skipped\":%llu,"
		"\"frame_done_sent\":%llu,\"commits\":%llu,\"damage_px\":%llu,"
		"\"shots_stable\":%llu,\"shots_timeout\":%llu,\"clients\":[",
		a->running ? "true" : "false",
		(unsigned long long)a->frames_rendered, (unsigned long long)a->frames_skipped,
		(unsigned long long)a->frame_done_sent, (unsigned long long)a->commits,
		(unsigned long long)a->damage_px, (unsigned long long)a->shots_stable,
		(unsigned long long)a->shots_timeout);
	bool first = true;
	struct client_info *ci;
	wl_list_for_each(ci, &a->srv->clients, link) {
		if (ci->agent != a) {
			continue;
		}
		sb_printf(&b, "%s{\"pid\":%d,\"commits\":%llu,\"damage_px\":%llu}", first ? "" : ",",
			ci->pid, (unsigned long long)ci->commits, (unsigned long long)ci->damage_px);
		first = false;
		if (reset) {
			ci->commits = ci->damage_px = 0;
		}
	}
	sb_printf(&b, "]}");
	conn_send(c, &b);
	if (reset) {
		a->frames_rendered = a->frames_skipped = a->frame_done_sent = 0;
		a->commits = a->damage_px = a->shots_stable = a->shots_timeout = 0;
	}
	return true;
}

// 手动放行 MS 毫秒（默认 grace_ms），供测量脚本使用。
static bool cmd_run(struct conn *c, struct agent *a, char **args, int n) {
	agent_run_for(a, n >= 3 ? atoi(args[2]) : a->srv->grace_ms);
	conn_reply_ok(c);
	return true;
}

static bool cmd_gate(struct conn *c, struct agent *a, char **args, int n) {
	agent_gate(a);
	conn_reply_ok(c);
	return true;
}

struct agent_command {
	const char *name;
	int min_args; // 含命令名和 agent 名
	const char *usage;
	bool (*fn)(struct conn *c, struct agent *a, char **args, int n);
};

static const struct agent_command agent_commands[] = {
	{"screenshot", 3, "screenshot AGENT PATH [settle=ms] [timeout=ms] [threshold=px]", cmd_screenshot},
	{"move",       4, "move AGENT X Y", cmd_move},
	{"click",      4, "click AGENT X Y [button=left|right|middle] [count=N]", cmd_click},
	{"button",     4, "button AGENT left|right|middle down|up", cmd_button},
	{"scroll",     4, "scroll AGENT DX DY", cmd_scroll},
	{"key",        3, "key AGENT COMBO", cmd_key},
	{"keydown",    3, "keydown AGENT KEY", cmd_keydown},
	{"keyup",      3, "keyup AGENT KEY", cmd_keyup},
	{"windows",    2, "windows AGENT", cmd_windows},
	{"stats",      2, "stats AGENT [reset]", cmd_stats},
	{"run",        2, "run AGENT [MS]", cmd_run},
	{"gate",       2, "gate AGENT", cmd_gate},
};

static const struct agent_command *find_command(const char *name) {
	for (size_t i = 0; i < sizeof(agent_commands) / sizeof(agent_commands[0]); i++) {
		if (!strcmp(agent_commands[i].name, name)) {
			return &agent_commands[i];
		}
	}
	return NULL;
}

/* ---------- 分发 ---------- */

bool commands_dispatch(struct conn *c, struct server *srv, char *line) {
	if (!strncmp(line, "type ", 5)) {
		cmd_type(c, srv, line + 5);
		return true;
	}

	char *args[MAX_ARGS];
	int n = 0;
	char *save = NULL;
	for (char *tok = strtok_r(line, " \t", &save); tok && n < MAX_ARGS;
			tok = strtok_r(NULL, " \t", &save)) {
		args[n++] = tok;
	}
	if (n == 0) {
		return true;
	}

	const char *name = args[0];
	if (!strcmp(name, "agents")) {
		cmd_agents(c, srv);
		return true;
	}
	if (!strcmp(name, "ping")) {
		conn_reply_ok(c);
		return true;
	}

	const struct agent_command *cmd = find_command(name);
	if (!cmd) {
		conn_reply_error(c, "unknown command '%s'", name);
		return true;
	}
	if (n < 2) {
		conn_reply_error(c, "usage: %s", cmd->usage);
		return true;
	}
	struct agent *a = agent_by_name(srv, args[1]);
	if (!a) {
		conn_reply_error(c, "no agent '%s'", args[1]);
		return true;
	}
	if (n < cmd->min_args) {
		conn_reply_error(c, "usage: %s", cmd->usage);
		return true;
	}
	return cmd->fn(c, a, args, n);
}
