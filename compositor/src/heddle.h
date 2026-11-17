// Heddle 的核心类型：server（整个合成器）、agent（每个 agent 一份）、client_info（每个 Wayland 客户端）。
// 各模块对外的函数声明在各自的头文件里。
#ifndef HEDDLE_H
#define HEDDLE_H

#include <stdbool.h>
#include <stdint.h>
#include <sys/types.h>
#include <wayland-server-core.h>
#include <wlr/types/wlr_keyboard.h>

enum frame_policy {
	POLICY_ALWAYS,  // 普通合成器：有损坏就出帧
	POLICY_OBSERVE, // 只在动作后的放行期和截图时出帧
};

struct server {
	struct wl_display *display;
	struct wl_event_loop *loop;
	struct wlr_backend *backend;
	struct wlr_renderer *renderer;
	struct wlr_allocator *allocator;
	struct wlr_compositor *compositor;
	struct wlr_xdg_shell *xdg_shell;

	struct wl_listener new_surface;  // frame.c
	struct wl_listener new_toplevel; // window.c
	struct wl_listener new_popup;    // window.c

	struct wl_list agents;  // agent.link
	struct wl_list clients; // client_info.link

	// 命令行选项
	int width, height, refresh_mhz;
	enum frame_policy policy;
	int grace_ms;
	bool use_suspend;
	bool maximize;
	char runtime_dir[256];

	// 控制套接字（control.c）
	char ctl_path[300];
	int ctl_fd;
	struct wl_event_source *ctl_src;

	uint32_t next_window_id;
};

struct agent {
	struct wl_list link;
	struct server *srv;
	char name[64];
	char dir[320];
	char sock_path[384];
	int listen_fd;
	struct wl_event_source *listen_src;

	struct wlr_output *output;
	struct wlr_scene *scene;
	struct wlr_scene_output *scene_output;
	struct wl_listener output_frame;

	// 输入（input.c）
	struct wlr_seat *seat;
	struct wl_listener request_set_selection;
	struct wl_listener request_set_primary_selection;
	struct wlr_keyboard keyboard;
	double px, py; // 指针位置，输出坐标

	struct wl_list toplevels; // toplevel.link，表头 = 最上层

	// 出帧策略（frame.c）
	bool running;         // 当前是否放行帧
	int64_t run_until_ms; // 动作后放行期截止
	struct wl_event_source *grace_timer;
	int64_t last_change_ms; // 上次有效损坏的时间，截图稳定判定用
	struct wlr_buffer *last_buffer;

	// 截图（screenshot.c）
	struct shot *shot; // 正在等稳定的截图，没有则为 NULL
	struct wl_event_source *tick_timer;

	// 统计（stats reset 清零）
	uint64_t frames_rendered, frames_skipped, frame_done_sent;
	uint64_t commits, damage_px;
	uint64_t shots_stable, shots_timeout;
};

struct client_info {
	struct wl_list link;
	struct wl_client *client;
	struct agent *agent;
	pid_t pid;
	uint64_t commits, damage_px;
	struct wl_listener destroy;
};

#endif
