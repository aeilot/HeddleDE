#include "agent.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>
#include <wlr/backend/headless.h>
#include <wlr/types/wlr_output.h>
#include <wlr/types/wlr_scene.h>
#include <wlr/types/wlr_seat.h>
#include <wlr/util/log.h>

#include "frame.h"
#include "input.h"
#include "screenshot.h"
#include "util.h"

/* ---------- 客户端与 agent 的对应 ---------- */

struct client_info *client_lookup(struct server *srv, const struct wl_client *client) {
	struct client_info *ci;
	wl_list_for_each(ci, &srv->clients, link) {
		if (ci->client == client) {
			return ci;
		}
	}
	return NULL;
}

struct agent *client_agent(struct server *srv, const struct wl_client *client) {
	struct client_info *ci = client_lookup(srv, client);
	return ci ? ci->agent : NULL;
}

struct agent *agent_by_name(struct server *srv, const char *name) {
	struct agent *a;
	wl_list_for_each(a, &srv->agents, link) {
		if (strcmp(a->name, name) == 0) {
			return a;
		}
	}
	return NULL;
}

bool agent_global_filter(const struct wl_client *client, const struct wl_global *global, void *data) {
	struct server *srv = data;
	struct agent *owner = NULL, *a;
	wl_list_for_each(a, &srv->agents, link) {
		if ((a->seat && a->seat->global == global) ||
				(a->output && a->output->global == global)) {
			owner = a;
			break;
		}
	}
	if (!owner) {
		return true; // 公共全局对象（wl_compositor、xdg_wm_base 等）
	}
	return client_agent(srv, client) == owner;
}

static void client_destroy(struct wl_listener *listener, void *data) {
	struct client_info *ci = wl_container_of(listener, ci, destroy);
	wl_list_remove(&ci->link);
	wl_list_remove(&ci->destroy.link);
	free(ci);
}

// 每个 agent 自己的 Wayland 套接字由合成器 accept，所以知道新客户端属于谁。
static int handle_accept(int fd, uint32_t mask, void *data) {
	struct agent *a = data;
	int cfd = accept4(fd, NULL, NULL, SOCK_CLOEXEC);
	if (cfd < 0) {
		return 0;
	}
	// 先登记再建客户端，过滤器在客户端第一次取注册表时就要能查到它。
	struct client_info *ci = calloc(1, sizeof(*ci));
	ci->agent = a;
	wl_list_insert(&a->srv->clients, &ci->link);
	ci->client = wl_client_create(a->srv->display, cfd);
	if (!ci->client) {
		wl_list_remove(&ci->link);
		free(ci);
		close(cfd);
		return 0;
	}
	uid_t uid;
	gid_t gid;
	wl_client_get_credentials(ci->client, &ci->pid, &uid, &gid);
	ci->destroy.notify = client_destroy;
	wl_client_add_destroy_listener(ci->client, &ci->destroy);
	wlr_log(WLR_INFO, "agent %s: client pid %d connected", a->name, ci->pid);
	return 0;
}

/* ---------- 创建 ---------- */

static bool agent_listen(struct agent *a) {
	struct server *srv = a->srv;
	snprintf(a->dir, sizeof(a->dir), "%s/%s", srv->runtime_dir, a->name);
	mkdir(a->dir, 0700);
	snprintf(a->sock_path, sizeof(a->sock_path), "%s/wayland-0", a->dir);
	a->listen_fd = listen_unix(a->sock_path);
	if (a->listen_fd < 0) {
		wlr_log_errno(WLR_ERROR, "listen %s", a->sock_path);
		return false;
	}
	a->listen_src = wl_event_loop_add_fd(srv->loop, a->listen_fd, WL_EVENT_READABLE, handle_accept, a);
	return true;
}

static void agent_create_output(struct agent *a) {
	struct server *srv = a->srv;
	a->output = wlr_headless_add_output(srv->backend, srv->width, srv->height);
	wlr_output_init_render(a->output, srv->allocator, srv->renderer);
	struct wlr_output_state state;
	wlr_output_state_init(&state);
	wlr_output_state_set_enabled(&state, true);
	wlr_output_state_set_custom_mode(&state, srv->width, srv->height, srv->refresh_mhz);
	wlr_output_commit_state(a->output, &state);
	wlr_output_state_finish(&state);
	char desc[128];
	snprintf(desc, sizeof(desc), "heddle agent %s", a->name);
	wlr_output_set_description(a->output, desc);
	wlr_output_create_global(a->output, srv->display);

	a->scene = wlr_scene_create();
	a->scene_output = wlr_scene_output_create(a->scene, a->output);
	// 背景：纯灰，便于和未绘制区域区分
	float bg[4] = {0.2f, 0.2f, 0.22f, 1.0f};
	wlr_scene_rect_create(&a->scene->tree, srv->width, srv->height, bg);
}

bool agent_create(struct server *srv, const char *name) {
	struct agent *a = calloc(1, sizeof(*a));
	a->srv = srv;
	snprintf(a->name, sizeof(a->name), "%s", name);
	wl_list_init(&a->toplevels);

	if (!agent_listen(a)) {
		free(a);
		return false;
	}
	agent_create_output(a);
	input_init_agent(a);
	screenshot_init_agent(a);
	frame_init_agent(a); // 最后：启动时放行一个 grace 期

	wl_list_insert(srv->agents.prev, &a->link);
	wlr_log(WLR_INFO, "agent %s: WAYLAND_DISPLAY=%s output=%s", name, a->sock_path, a->output->name);
	return true;
}
