// Heddle：面向 agent 的无头 Wayland 合成器（第 2、3 步原型）。
//
// - 每个 agent 一个 Wayland 套接字、一个无头输出、一个 seat、一张场景图（agent.c）。
// - 全局对象按客户端过滤：应用只看到自己 agent 的 seat 和输出（agent.c）。
// - 控制套接字（按行文本命令，按行 JSON 回复）负责截图、注入输入、列窗口、统计
//   （control.c、commands.c、screenshot.c、input.c）。
// - 出帧策略：always 或 observe（frame.c）。
//
// 基于 wlroots 0.18，渲染固定为 pixman。本文件只做参数解析、启动和退出。

#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include <wlr/backend.h>
#include <wlr/backend/headless.h>
#include <wlr/render/allocator.h>
#include <wlr/render/pixman.h>
#include <wlr/render/wlr_renderer.h>
#include <wlr/types/wlr_compositor.h>
#include <wlr/types/wlr_data_device.h>
#include <wlr/types/wlr_primary_selection_v1.h>
#include <wlr/types/wlr_single_pixel_buffer_v1.h>
#include <wlr/types/wlr_subcompositor.h>
#include <wlr/types/wlr_viewporter.h>
#include <wlr/util/log.h>

#include "agent.h"
#include "control.h"
#include "frame.h"
#include "heddle.h"
#include "window.h"

#define MAX_AGENTS 64

struct options {
	const char *agents[MAX_AGENTS];
	int nagents;
	const char *runtime_dir; // NULL 表示 $XDG_RUNTIME_DIR/heddle
	enum wlr_log_importance loglevel;
};

static void usage(const char *argv0) {
	fprintf(stderr,
		"usage: %s [options]\n"
		"  -a NAME        add an agent (repeatable; default: one agent 'a0')\n"
		"  -s WxH         output size (default 1920x1080)\n"
		"  -r HZ          output refresh (default 60)\n"
		"  -p POLICY      frame policy: always | observe (default always)\n"
		"  -g MS          grace period after actions in observe mode (default 2000)\n"
		"  -S             also send xdg_toplevel suspended while gated\n"
		"  -w             do not maximize toplevels (stack/cascade instead)\n"
		"  -R DIR         runtime dir (default $XDG_RUNTIME_DIR/heddle)\n"
		"  -v             verbose wlroots logging\n", argv0);
}

// 返回 -1 表示继续运行，否则为应退出的状态码。
static int parse_args(int argc, char **argv, struct server *srv, struct options *opts) {
	srv->width = 1920;
	srv->height = 1080;
	srv->refresh_mhz = 60000;
	srv->policy = POLICY_ALWAYS;
	srv->grace_ms = 2000;
	srv->maximize = true;
	opts->loglevel = WLR_INFO;

	int opt;
	while ((opt = getopt(argc, argv, "a:s:r:p:g:SwR:vh")) != -1) {
		switch (opt) {
		case 'a':
			if (opts->nagents < MAX_AGENTS) opts->agents[opts->nagents++] = optarg;
			break;
		case 's':
			if (sscanf(optarg, "%dx%d", &srv->width, &srv->height) != 2) { usage(argv[0]); return 1; }
			break;
		case 'r': srv->refresh_mhz = atoi(optarg) * 1000; break;
		case 'p':
			if (!strcmp(optarg, "always")) srv->policy = POLICY_ALWAYS;
			else if (!strcmp(optarg, "observe")) srv->policy = POLICY_OBSERVE;
			else { usage(argv[0]); return 1; }
			break;
		case 'g': srv->grace_ms = atoi(optarg); break;
		case 'S': srv->use_suspend = true; break;
		case 'w': srv->maximize = false; break;
		case 'R': opts->runtime_dir = optarg; break;
		case 'v': opts->loglevel = WLR_DEBUG; break;
		default: usage(argv[0]); return opt == 'h' ? 0 : 1;
		}
	}
	if (opts->nagents == 0) {
		opts->agents[opts->nagents++] = "a0";
	}
	return -1;
}

static bool setup_runtime_dir(struct server *srv, const char *dir) {
	if (dir) {
		snprintf(srv->runtime_dir, sizeof(srv->runtime_dir), "%s", dir);
	} else {
		const char *xdg = getenv("XDG_RUNTIME_DIR");
		if (!xdg) {
			fprintf(stderr, "XDG_RUNTIME_DIR not set; pass -R\n");
			return false;
		}
		snprintf(srv->runtime_dir, sizeof(srv->runtime_dir), "%s/heddle", xdg);
	}
	mkdir(srv->runtime_dir, 0700);
	return true;
}

// display、无头后端、pixman 渲染器，以及应用需要的全局对象。
static bool server_init(struct server *srv) {
	wl_list_init(&srv->agents);
	wl_list_init(&srv->clients);
	srv->display = wl_display_create();
	srv->loop = wl_display_get_event_loop(srv->display);
	wl_display_set_global_filter(srv->display, agent_global_filter, srv);

	srv->backend = wlr_headless_backend_create(srv->loop);
	srv->renderer = wlr_pixman_renderer_create();
	if (!srv->backend || !srv->renderer) {
		wlr_log(WLR_ERROR, "failed to create backend/renderer");
		return false;
	}
	wlr_renderer_init_wl_display(srv->renderer, srv->display);
	srv->allocator = wlr_allocator_autocreate(srv->backend, srv->renderer);
	if (!srv->allocator) {
		wlr_log(WLR_ERROR, "failed to create allocator");
		return false;
	}

	srv->compositor = wlr_compositor_create(srv->display, 6, srv->renderer);
	wlr_subcompositor_create(srv->display);
	wlr_data_device_manager_create(srv->display);
	wlr_primary_selection_v1_device_manager_create(srv->display);
	wlr_viewporter_create(srv->display);
	wlr_single_pixel_buffer_manager_v1_create(srv->display);
	// 不创建 screencopy、virtual-keyboard、virtual-pointer、data-control：
	// 截图和输入只走控制套接字，避免应用跨 agent 读画面或注入。

	frame_init_server(srv);
	window_init(srv);

	if (!wlr_backend_start(srv->backend)) {
		wlr_log(WLR_ERROR, "failed to start backend");
		return false;
	}
	return true;
}

static int handle_signal(int sig, void *data) {
	wl_display_terminate(data);
	return 0;
}

// 就绪标记（内容为 pid），供脚本等待。
static void write_ready_file(const char *path) {
	FILE *f = fopen(path, "w");
	if (f) {
		fprintf(f, "%d\n", getpid());
		fclose(f);
	}
}

static void server_finish(struct server *srv, const char *ready_path) {
	unlink(ready_path);
	unlink(srv->ctl_path);
	struct agent *a;
	wl_list_for_each(a, &srv->agents, link) {
		unlink(a->sock_path);
	}
	wl_display_destroy_clients(srv->display);
	wlr_backend_destroy(srv->backend);
	wl_display_destroy(srv->display);
}

int main(int argc, char **argv) {
	struct server srv = {0};
	struct options opts = {0};
	int rc = parse_args(argc, argv, &srv, &opts);
	if (rc >= 0) {
		return rc;
	}
	wlr_log_init(opts.loglevel, NULL);
	// 单个全屏窗口时 wlroots 会直接把客户端缓冲区送上输出（direct scanout），
	// 截图要读的是合成器自己的帧，客户端缓冲区在 pixman 下转纹理会失败。
	setenv("WLR_SCENE_DISABLE_DIRECT_SCANOUT", "1", 0);

	if (!setup_runtime_dir(&srv, opts.runtime_dir) || !server_init(&srv)) {
		return 1;
	}
	for (int i = 0; i < opts.nagents; i++) {
		if (agent_by_name(&srv, opts.agents[i]) || !agent_create(&srv, opts.agents[i])) {
			wlr_log(WLR_ERROR, "failed to create agent %s", opts.agents[i]);
			return 1;
		}
	}
	if (!control_listen(&srv)) {
		return 1;
	}
	signal(SIGPIPE, SIG_IGN);
	wl_event_loop_add_signal(srv.loop, SIGTERM, handle_signal, srv.display);
	wl_event_loop_add_signal(srv.loop, SIGINT, handle_signal, srv.display);

	wlr_log(WLR_INFO, "heddle: control socket %s, policy %s", srv.ctl_path,
		srv.policy == POLICY_ALWAYS ? "always" : "observe");
	char ready[320];
	snprintf(ready, sizeof(ready), "%s/ready", srv.runtime_dir);
	write_ready_file(ready);

	wl_display_run(srv.display);

	server_finish(&srv, ready);
	return 0;
}
