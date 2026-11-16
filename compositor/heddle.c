// Heddle：面向 agent 的无头 Wayland 合成器（第 2、3 步原型）。
//
// - 每个 agent 一个 Wayland 套接字、一个无头输出、一个 seat、一张场景图。
// - 全局对象按客户端过滤：应用只看到自己 agent 的 seat 和输出。
// - 控制套接字（按行文本命令，按行 JSON 回复）负责截图、注入输入、列窗口、统计。
// - 出帧策略：always（照常出帧）或 observe（只在动作后的放行期和截图时出帧，
//   截图前等画面稳定或超时）。
//
// 基于 wlroots 0.18，渲染固定为 pixman。

#define _GNU_SOURCE
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <linux/input-event-codes.h>
#include <png.h>
#include <signal.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>
#include <wayland-server-core.h>
#include <xkbcommon/xkbcommon.h>
#include <drm_fourcc.h>

#include <wlr/backend.h>
#include <wlr/backend/headless.h>
#include <wlr/interfaces/wlr_keyboard.h>
#include <wlr/render/allocator.h>
#include <wlr/render/pixman.h>
#include <wlr/render/wlr_renderer.h>
#include <wlr/render/wlr_texture.h>
#include <wlr/types/wlr_compositor.h>
#include <wlr/types/wlr_data_device.h>
#include <wlr/types/wlr_keyboard.h>
#include <wlr/types/wlr_output.h>
#include <wlr/types/wlr_primary_selection.h>
#include <wlr/types/wlr_primary_selection_v1.h>
#include <wlr/types/wlr_scene.h>
#include <wlr/types/wlr_seat.h>
#include <wlr/types/wlr_single_pixel_buffer_v1.h>
#include <wlr/types/wlr_subcompositor.h>
#include <wlr/types/wlr_viewporter.h>
#include <wlr/types/wlr_xdg_shell.h>
#include <wlr/util/log.h>

#define MAX_LINE 65536

enum frame_policy { POLICY_ALWAYS, POLICY_OBSERVE };

struct server {
	struct wl_display *display;
	struct wl_event_loop *loop;
	struct wlr_backend *backend;
	struct wlr_renderer *renderer;
	struct wlr_allocator *allocator;
	struct wlr_compositor *compositor;
	struct wlr_xdg_shell *xdg_shell;

	struct wl_listener new_surface;
	struct wl_listener new_toplevel;
	struct wl_listener new_popup;

	struct wl_list agents;  // agent.link
	struct wl_list clients; // client_info.link

	int width, height, refresh_mhz;
	enum frame_policy policy;
	int grace_ms;
	bool use_suspend;
	bool maximize;
	char runtime_dir[256];

	int ctl_fd;
	struct wl_event_source *ctl_src;
	uint32_t next_window_id;
};

struct shot {
	struct conn *conn;
	char path[512];
	int settle_ms, timeout_ms;
	uint64_t threshold_px;
	int64_t start_ms;
	uint64_t frames_at_start;
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

	struct wlr_seat *seat;
	struct wl_listener request_set_selection;
	struct wl_listener request_set_primary_selection;
	struct wlr_keyboard keyboard;
	double px, py;

	struct wl_list toplevels; // toplevel.link, front = top

	// 出帧策略
	bool running;          // 当前是否放行帧
	int64_t run_until_ms;  // 动作后放行期截止
	struct wl_event_source *grace_timer;
	struct wl_event_source *tick_timer;
	struct shot *shot;
	int64_t last_change_ms;
	struct wlr_buffer *last_buffer;

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

struct surface_info {
	struct wlr_surface *surface;
	struct wl_listener commit;
	struct wl_listener destroy;
};

struct toplevel {
	struct wl_list link;
	struct agent *agent;
	uint32_t id;
	struct wlr_xdg_toplevel *xdg;
	struct wlr_scene_tree *tree;
	bool mapped;
	struct wl_listener map, unmap, commit, destroy;
};

struct popup {
	struct wlr_xdg_popup *xdg;
	struct wl_listener commit, destroy;
};

struct conn {
	struct server *srv;
	int fd;
	struct wl_event_source *src;
	char buf[MAX_LINE];
	size_t len;
	bool busy; // 有截图在等
	bool dead;
};

static int64_t now_ms(void) {
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static uint32_t now_msec32(void) {
	return (uint32_t)now_ms();
}

/* ---------- 小工具：JSON 输出 ---------- */

struct sbuf {
	char *data;
	size_t len, cap;
};

static void sb_putn(struct sbuf *b, const char *s, size_t n) {
	if (b->len + n + 1 > b->cap) {
		size_t cap = b->cap ? b->cap * 2 : 256;
		while (cap < b->len + n + 1) {
			cap *= 2;
		}
		b->data = realloc(b->data, cap);
		b->cap = cap;
	}
	memcpy(b->data + b->len, s, n);
	b->len += n;
	b->data[b->len] = '\0';
}

static void sb_printf(struct sbuf *b, const char *fmt, ...) {
	char tmp[1024];
	va_list ap;
	va_start(ap, fmt);
	int n = vsnprintf(tmp, sizeof(tmp), fmt, ap);
	va_end(ap);
	if (n < 0) {
		return;
	}
	if ((size_t)n >= sizeof(tmp)) {
		n = sizeof(tmp) - 1;
	}
	sb_putn(b, tmp, n);
}

static void sb_json_str(struct sbuf *b, const char *s) {
	sb_putn(b, "\"", 1);
	for (; s && *s; s++) {
		unsigned char c = *s;
		if (c == '"' || c == '\\') {
			char e[2] = {'\\', c};
			sb_putn(b, e, 2);
		} else if (c < 0x20) {
			sb_printf(b, "\\u%04x", c);
		} else {
			sb_putn(b, (const char *)&c, 1);
		}
	}
	sb_putn(b, "\"", 1);
}

static void conn_send(struct conn *c, struct sbuf *b) {
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
	free(b->data);
	b->data = NULL;
	b->len = b->cap = 0;
}

static void reply_error(struct conn *c, const char *fmt, ...) {
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

static void reply_ok(struct conn *c) {
	struct sbuf b = {0};
	sb_printf(&b, "{\"ok\":true}");
	conn_send(c, &b);
}

/* ---------- 客户端与 agent 的对应 ---------- */

static struct client_info *client_lookup(struct server *srv, const struct wl_client *client) {
	struct client_info *ci;
	wl_list_for_each(ci, &srv->clients, link) {
		if (ci->client == client) {
			return ci;
		}
	}
	return NULL;
}

static struct agent *client_agent(struct server *srv, const struct wl_client *client) {
	struct client_info *ci = client_lookup(srv, client);
	return ci ? ci->agent : NULL;
}

static struct agent *agent_by_name(struct server *srv, const char *name) {
	struct agent *a;
	wl_list_for_each(a, &srv->agents, link) {
		if (strcmp(a->name, name) == 0) {
			return a;
		}
	}
	return NULL;
}

// 全局对象过滤：seat 与输出只给所属 agent 的客户端看到。
static bool global_filter(const struct wl_client *client,
		const struct wl_global *global, void *data) {
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
		return true;
	}
	return client_agent(srv, client) == owner;
}

static void client_destroy(struct wl_listener *listener, void *data) {
	struct client_info *ci = wl_container_of(listener, ci, destroy);
	wl_list_remove(&ci->link);
	wl_list_remove(&ci->destroy.link);
	free(ci);
}

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

/* ---------- 出帧策略 ---------- */

static void agent_set_running(struct agent *a, bool running);
static void shot_try_finish(struct agent *a);

static void agent_suspend_toplevels(struct agent *a, bool suspended) {
	if (!a->srv->use_suspend) {
		return;
	}
	struct toplevel *t;
	wl_list_for_each(t, &a->toplevels, link) {
		if (t->xdg->base->initialized) {
			wlr_xdg_toplevel_set_suspended(t->xdg, suspended);
		}
	}
}

static bool agent_should_run(struct agent *a) {
	if (a->srv->policy == POLICY_ALWAYS) {
		return true;
	}
	return a->shot != NULL || now_ms() < a->run_until_ms;
}

static void agent_update_running(struct agent *a) {
	agent_set_running(a, agent_should_run(a));
}

static void agent_set_running(struct agent *a, bool running) {
	if (a->running == running) {
		return;
	}
	a->running = running;
	agent_suspend_toplevels(a, !running);
	if (running) {
		// 放行：安排一帧，帧事件里会补发帧回调，叫醒等回调的客户端。
		wlr_output_schedule_frame(a->output);
	}
}

static int handle_grace_timer(void *data) {
	struct agent *a = data;
	int64_t left = a->run_until_ms - now_ms();
	if (left > 0) {
		wl_event_source_timer_update(a->grace_timer, (int)left);
		return 0;
	}
	agent_update_running(a);
	return 0;
}

// 动作之后放行 grace_ms，让依赖帧时钟的逻辑在模型等待期跑完。
static void agent_note_action(struct agent *a) {
	int64_t until = now_ms() + a->srv->grace_ms;
	if (until > a->run_until_ms) {
		a->run_until_ms = until;
	}
	wl_event_source_timer_update(a->grace_timer, a->srv->grace_ms > 0 ? a->srv->grace_ms : 1);
	agent_update_running(a);
}

static void output_frame(struct wl_listener *listener, void *data) {
	struct agent *a = wl_container_of(listener, a, output_frame);
	if (!a->running) {
		a->frames_skipped++;
		return;
	}
	struct wlr_scene_output *so = a->scene_output;
	if (a->output->needs_frame || pixman_region32_not_empty(&so->pending_commit_damage) ||
			!a->last_buffer) {
		struct wlr_output_state state;
		wlr_output_state_init(&state);
		if (wlr_scene_output_build_state(so, &state, NULL)) {
			struct wlr_buffer *buf = (state.committed & WLR_OUTPUT_STATE_BUFFER) ? state.buffer : NULL;
			if (buf) {
				wlr_buffer_lock(buf);
			}
			if (wlr_output_commit_state(a->output, &state)) {
				if (buf) {
					if (a->last_buffer) {
						wlr_buffer_unlock(a->last_buffer);
					}
					a->last_buffer = buf;
					buf = NULL;
					a->frames_rendered++;
				}
			}
			if (buf) {
				wlr_buffer_unlock(buf);
			}
		}
		wlr_output_state_finish(&state);
	}
	struct timespec now;
	clock_gettime(CLOCK_MONOTONIC, &now);
	wlr_scene_output_send_frame_done(so, &now);
	a->frame_done_sent++;
	if (a->shot) {
		shot_try_finish(a);
	}
}

static uint64_t region_area(pixman_region32_t *r) {
	int n;
	pixman_box32_t *boxes = pixman_region32_rectangles(r, &n);
	uint64_t area = 0;
	for (int i = 0; i < n; i++) {
		area += (uint64_t)(boxes[i].x2 - boxes[i].x1) * (uint64_t)(boxes[i].y2 - boxes[i].y1);
	}
	return area;
}

/* 表面回调里需要 server 指针，用全局变量最省事。 */
static struct server *g_srv;

static void surface_commit_impl(struct wl_listener *listener, void *data) {
	struct surface_info *si = wl_container_of(listener, si, commit);
	struct wlr_surface *surface = si->surface;
	struct client_info *ci = client_lookup(g_srv, wl_resource_get_client(surface->resource));
	if (!ci) {
		return;
	}
	struct agent *a = ci->agent;
	uint64_t area = 0;
	if (surface->current.committed & WLR_SURFACE_STATE_BUFFER) {
		area = region_area(&surface->buffer_damage);
	}
	ci->commits++;
	ci->damage_px += area;
	a->commits++;
	a->damage_px += area;
	uint64_t threshold = a->shot ? a->shot->threshold_px : 0;
	if (area > threshold) {
		a->last_change_ms = now_ms();
	}
}

static void surface_destroy(struct wl_listener *listener, void *data) {
	struct surface_info *si = wl_container_of(listener, si, destroy);
	wl_list_remove(&si->commit.link);
	wl_list_remove(&si->destroy.link);
	free(si);
}

static void handle_new_surface(struct wl_listener *listener, void *data) {
	struct wlr_surface *surface = data;
	struct surface_info *si = calloc(1, sizeof(*si));
	si->surface = surface;
	si->commit.notify = surface_commit_impl;
	wl_signal_add(&surface->events.commit, &si->commit);
	si->destroy.notify = surface_destroy;
	wl_signal_add(&surface->events.destroy, &si->destroy);
}

/* ---------- 截图 ---------- */

static bool write_png(const char *path, struct wlr_buffer *buffer,
		struct wlr_renderer *renderer, char *err, size_t errlen) {
	struct wlr_texture *tex = wlr_texture_from_buffer(renderer, buffer);
	if (!tex) {
		snprintf(err, errlen, "texture_from_buffer failed");
		return false;
	}
	uint32_t w = tex->width, h = tex->height, stride = w * 4;
	uint8_t *pixels = malloc((size_t)stride * h);
	struct wlr_texture_read_pixels_options opts = {
		.data = pixels,
		.format = DRM_FORMAT_XRGB8888,
		.stride = stride,
	};
	bool ok = wlr_texture_read_pixels(tex, &opts);
	wlr_texture_destroy(tex);
	if (!ok) {
		free(pixels);
		snprintf(err, errlen, "read_pixels failed");
		return false;
	}
	char tmp[600];
	snprintf(tmp, sizeof(tmp), "%s.tmp", path);
	FILE *f = fopen(tmp, "wb");
	if (!f) {
		free(pixels);
		snprintf(err, errlen, "open %s: %s", tmp, strerror(errno));
		return false;
	}
	png_structp png = png_create_write_struct(PNG_LIBPNG_VER_STRING, NULL, NULL, NULL);
	png_infop info = png_create_info_struct(png);
	if (setjmp(png_jmpbuf(png))) {
		png_destroy_write_struct(&png, &info);
		fclose(f);
		free(pixels);
		snprintf(err, errlen, "png encode failed");
		return false;
	}
	png_init_io(png, f);
	png_set_compression_level(png, 1); // 截图延迟优先
	png_set_IHDR(png, info, w, h, 8, PNG_COLOR_TYPE_RGB, PNG_INTERLACE_NONE,
		PNG_COMPRESSION_TYPE_DEFAULT, PNG_FILTER_TYPE_DEFAULT);
	png_write_info(png, info);
	uint8_t *row = malloc((size_t)w * 3);
	for (uint32_t y = 0; y < h; y++) {
		const uint32_t *src = (const uint32_t *)(pixels + (size_t)y * stride);
		for (uint32_t x = 0; x < w; x++) {
			uint32_t p = src[x];
			row[x * 3 + 0] = (p >> 16) & 0xff;
			row[x * 3 + 1] = (p >> 8) & 0xff;
			row[x * 3 + 2] = p & 0xff;
		}
		png_write_row(png, row);
	}
	png_write_end(png, NULL);
	png_destroy_write_struct(&png, &info);
	fclose(f);
	free(row);
	free(pixels);
	if (rename(tmp, path) != 0) {
		snprintf(err, errlen, "rename: %s", strerror(errno));
		return false;
	}
	return true;
}

static void conn_resume(struct conn *c);

static void shot_complete(struct agent *a, const char *status) {
	struct shot *s = a->shot;
	a->shot = NULL;
	wl_event_source_timer_update(a->tick_timer, 0);
	struct conn *c = s->conn;
	int64_t latency = now_ms() - s->start_ms;
	char err[256] = "";
	bool ok = a->last_buffer && write_png(s->path, a->last_buffer, a->srv->renderer, err, sizeof(err));
	if (strcmp(status, "stable") == 0) {
		a->shots_stable++;
	} else {
		a->shots_timeout++;
	}
	if (c) {
		if (ok) {
			struct sbuf b = {0};
			sb_printf(&b, "{\"ok\":true,\"status\":\"%s\",\"latency_ms\":%lld,"
				"\"frames\":%llu,\"width\":%d,\"height\":%d,\"cursor\":[%d,%d],\"path\":",
				status, (long long)latency,
				(unsigned long long)(a->frames_rendered - s->frames_at_start),
				a->output->width, a->output->height, (int)a->px, (int)a->py);
			sb_json_str(&b, s->path);
			sb_printf(&b, "}");
			conn_send(c, &b);
		} else {
			reply_error(c, "screenshot failed: %s", a->last_buffer ? err : "no frame rendered");
		}
		c->busy = false;
		conn_resume(c);
	}
	free(s);
	agent_update_running(a);
}

static void shot_try_finish(struct agent *a) {
	struct shot *s = a->shot;
	if (!s) {
		return;
	}
	int64_t now = now_ms();
	bool rendered = a->frames_rendered > s->frames_at_start || a->last_buffer;
	bool pending = pixman_region32_not_empty(&a->scene_output->pending_commit_damage);
	if (rendered && !pending && now - a->last_change_ms >= s->settle_ms &&
			now - s->start_ms >= s->settle_ms) {
		shot_complete(a, "stable");
	} else if (now - s->start_ms >= s->timeout_ms) {
		shot_complete(a, "timeout");
	}
}

static int handle_tick(void *data) {
	struct agent *a = data;
	if (!a->shot) {
		return 0;
	}
	// 有未合成的损坏就再要一帧。
	if (pixman_region32_not_empty(&a->scene_output->pending_commit_damage)) {
		wlr_output_schedule_frame(a->output);
	}
	shot_try_finish(a);
	if (a->shot) {
		wl_event_source_timer_update(a->tick_timer, 20);
	}
	return 0;
}

/* ---------- 窗口 ---------- */

static struct toplevel *toplevel_at(struct agent *a, double x, double y,
		struct wlr_surface **surface, double *sx, double *sy) {
	struct wlr_scene_node *node = wlr_scene_node_at(&a->scene->tree.node, x, y, sx, sy);
	*surface = NULL;
	if (!node || node->type != WLR_SCENE_NODE_BUFFER) {
		return NULL;
	}
	struct wlr_scene_buffer *sb = wlr_scene_buffer_from_node(node);
	struct wlr_scene_surface *ss = wlr_scene_surface_try_from_buffer(sb);
	if (!ss) {
		return NULL;
	}
	*surface = ss->surface;
	struct wlr_scene_tree *tree = node->parent;
	while (tree && !tree->node.data) {
		tree = tree->node.parent;
	}
	return tree ? tree->node.data : NULL;
}

static void focus_toplevel(struct toplevel *t) {
	struct agent *a = t->agent;
	struct wlr_seat *seat = a->seat;
	struct wlr_surface *prev = seat->keyboard_state.focused_surface;
	struct wlr_surface *surface = t->xdg->base->surface;
	if (prev == surface) {
		return;
	}
	if (prev) {
		struct wlr_xdg_toplevel *pt = wlr_xdg_toplevel_try_from_wlr_surface(prev);
		if (pt) {
			wlr_xdg_toplevel_set_activated(pt, false);
		}
	}
	wlr_scene_node_raise_to_top(&t->tree->node);
	wl_list_remove(&t->link);
	wl_list_insert(&a->toplevels, &t->link);
	wlr_xdg_toplevel_set_activated(t->xdg, true);
	struct wlr_keyboard *kb = &a->keyboard;
	wlr_seat_keyboard_notify_enter(seat, surface, kb->keycodes, kb->num_keycodes, &kb->modifiers);
}

static void toplevel_map(struct wl_listener *listener, void *data) {
	struct toplevel *t = wl_container_of(listener, t, map);
	t->mapped = true;
	t->agent->last_change_ms = now_ms();
	focus_toplevel(t);
}

static void toplevel_unmap(struct wl_listener *listener, void *data) {
	struct toplevel *t = wl_container_of(listener, t, unmap);
	t->mapped = false;
	struct agent *a = t->agent;
	a->last_change_ms = now_ms();
	if (a->seat->keyboard_state.focused_surface == t->xdg->base->surface) {
		wlr_seat_keyboard_notify_clear_focus(a->seat);
		struct toplevel *o;
		wl_list_for_each(o, &a->toplevels, link) {
			if (o != t && o->mapped) {
				focus_toplevel(o);
				break;
			}
		}
	}
}

static void toplevel_commit(struct wl_listener *listener, void *data) {
	struct toplevel *t = wl_container_of(listener, t, commit);
	if (!t->xdg->base->initial_commit) {
		return;
	}
	struct server *srv = t->agent->srv;
	if (srv->maximize) {
		wlr_xdg_toplevel_set_size(t->xdg, srv->width, srv->height);
		wlr_xdg_toplevel_set_maximized(t->xdg, true);
	} else {
		wlr_xdg_toplevel_set_size(t->xdg, 0, 0);
	}
	if (srv->use_suspend && !t->agent->running) {
		wlr_xdg_toplevel_set_suspended(t->xdg, true);
	}
}

static void toplevel_destroy(struct wl_listener *listener, void *data) {
	struct toplevel *t = wl_container_of(listener, t, destroy);
	wl_list_remove(&t->link);
	wl_list_remove(&t->map.link);
	wl_list_remove(&t->unmap.link);
	wl_list_remove(&t->commit.link);
	wl_list_remove(&t->destroy.link);
	free(t);
}

static void handle_new_toplevel(struct wl_listener *listener, void *data) {
	struct server *srv = wl_container_of(listener, srv, new_toplevel);
	struct wlr_xdg_toplevel *xdg = data;
	struct agent *a = client_agent(srv, wl_resource_get_client(xdg->resource));
	if (!a) {
		wl_client_post_implementation_error(wl_resource_get_client(xdg->resource),
			"heddle: unknown client");
		return;
	}
	struct toplevel *t = calloc(1, sizeof(*t));
	t->agent = a;
	t->id = ++srv->next_window_id;
	t->xdg = xdg;
	t->tree = wlr_scene_xdg_surface_create(&a->scene->tree, xdg->base);
	t->tree->node.data = t;
	xdg->base->data = t->tree;
	if (!srv->maximize) {
		int n = wl_list_length(&a->toplevels);
		wlr_scene_node_set_position(&t->tree->node, 32 * (n % 8), 32 * (n % 8));
	}
	wl_list_insert(&a->toplevels, &t->link);
	t->map.notify = toplevel_map;
	wl_signal_add(&xdg->base->surface->events.map, &t->map);
	t->unmap.notify = toplevel_unmap;
	wl_signal_add(&xdg->base->surface->events.unmap, &t->unmap);
	t->commit.notify = toplevel_commit;
	wl_signal_add(&xdg->base->surface->events.commit, &t->commit);
	t->destroy.notify = toplevel_destroy;
	wl_signal_add(&xdg->events.destroy, &t->destroy);
}

static void popup_commit(struct wl_listener *listener, void *data) {
	struct popup *p = wl_container_of(listener, p, commit);
	if (!p->xdg->base->initial_commit) {
		return;
	}
	struct wlr_scene_tree *tree = p->xdg->base->data;
	struct wlr_scene_tree *parent_tree = tree ? tree->node.parent : NULL;
	int lx = 0, ly = 0;
	if (parent_tree) {
		wlr_scene_node_coords(&parent_tree->node, &lx, &ly);
	}
	struct agent *a = client_agent(g_srv, wl_resource_get_client(p->xdg->resource));
	if (a) {
		struct wlr_box box = {-lx, -ly, a->output->width, a->output->height};
		wlr_xdg_popup_unconstrain_from_box(p->xdg, &box);
	}
	wlr_xdg_surface_schedule_configure(p->xdg->base);
}

static void popup_destroy(struct wl_listener *listener, void *data) {
	struct popup *p = wl_container_of(listener, p, destroy);
	wl_list_remove(&p->commit.link);
	wl_list_remove(&p->destroy.link);
	free(p);
}

static void handle_new_popup(struct wl_listener *listener, void *data) {
	struct wlr_xdg_popup *xdg = data;
	struct popup *p = calloc(1, sizeof(*p));
	p->xdg = xdg;
	struct wlr_xdg_surface *parent = xdg->parent ?
		wlr_xdg_surface_try_from_wlr_surface(xdg->parent) : NULL;
	if (parent && parent->data) {
		struct wlr_scene_tree *parent_tree = parent->data;
		xdg->base->data = wlr_scene_xdg_surface_create(parent_tree, xdg->base);
	}
	p->commit.notify = popup_commit;
	wl_signal_add(&xdg->base->surface->events.commit, &p->commit);
	p->destroy.notify = popup_destroy;
	wl_signal_add(&xdg->events.destroy, &p->destroy);
}

/* ---------- 输入 ---------- */

static void keyboard_led_update(struct wlr_keyboard *kb, uint32_t leds) {
}

static const struct wlr_keyboard_impl keyboard_impl = {
	.name = "heddle-keyboard",
	.led_update = keyboard_led_update,
};

static void pointer_move(struct agent *a, double x, double y) {
	if (x < 0) x = 0;
	if (y < 0) y = 0;
	if (x > a->output->width - 1) x = a->output->width - 1;
	if (y > a->output->height - 1) y = a->output->height - 1;
	a->px = x;
	a->py = y;
	struct wlr_surface *surface;
	double sx, sy;
	toplevel_at(a, x, y, &surface, &sx, &sy);
	uint32_t t = now_msec32();
	if (surface) {
		wlr_seat_pointer_notify_enter(a->seat, surface, sx, sy);
		wlr_seat_pointer_notify_motion(a->seat, t, sx, sy);
	} else {
		wlr_seat_pointer_clear_focus(a->seat);
	}
	wlr_seat_pointer_notify_frame(a->seat);
}

static void pointer_button(struct agent *a, uint32_t button, bool pressed) {
	if (pressed) {
		struct wlr_surface *surface;
		double sx, sy;
		struct toplevel *t = toplevel_at(a, a->px, a->py, &surface, &sx, &sy);
		if (t) {
			focus_toplevel(t);
		}
	}
	wlr_seat_pointer_notify_button(a->seat, now_msec32(), button,
		pressed ? WL_POINTER_BUTTON_STATE_PRESSED : WL_POINTER_BUTTON_STATE_RELEASED);
	wlr_seat_pointer_notify_frame(a->seat);
}

static void pointer_scroll(struct agent *a, int dx, int dy) {
	uint32_t t = now_msec32();
	if (dy) {
		wlr_seat_pointer_notify_axis(a->seat, t, WL_POINTER_AXIS_VERTICAL_SCROLL,
			15.0 * dy, 120 * dy, WL_POINTER_AXIS_SOURCE_WHEEL,
			WL_POINTER_AXIS_RELATIVE_DIRECTION_IDENTICAL);
	}
	if (dx) {
		wlr_seat_pointer_notify_axis(a->seat, t, WL_POINTER_AXIS_HORIZONTAL_SCROLL,
			15.0 * dx, 120 * dx, WL_POINTER_AXIS_SOURCE_WHEEL,
			WL_POINTER_AXIS_RELATIVE_DIRECTION_IDENTICAL);
	}
	wlr_seat_pointer_notify_frame(a->seat);
}

static void key_event(struct agent *a, uint32_t evdev_code, bool pressed) {
	struct wlr_keyboard_key_event ev = {
		.time_msec = now_msec32(),
		.keycode = evdev_code,
		.update_state = true,
		.state = pressed ? WL_KEYBOARD_KEY_STATE_PRESSED : WL_KEYBOARD_KEY_STATE_RELEASED,
	};
	wlr_keyboard_notify_key(&a->keyboard, &ev);
	wlr_seat_set_keyboard(a->seat, &a->keyboard);
	wlr_seat_keyboard_notify_key(a->seat, ev.time_msec, evdev_code, ev.state);
	wlr_seat_keyboard_notify_modifiers(a->seat, &a->keyboard.modifiers);
}

// 在当前键位表里找能产生 keysym 的键码；需要 Shift 时 *shift = true。
static bool keysym_lookup(struct agent *a, xkb_keysym_t sym, uint32_t *evdev, bool *shift) {
	struct xkb_keymap *km = a->keyboard.keymap;
	xkb_keycode_t min = xkb_keymap_min_keycode(km), max = xkb_keymap_max_keycode(km);
	for (int level = 0; level < 2; level++) {
		for (xkb_keycode_t kc = min; kc <= max; kc++) {
			if (xkb_keymap_num_levels_for_key(km, kc, 0) <= (xkb_level_index_t)level) {
				continue;
			}
			const xkb_keysym_t *syms;
			int n = xkb_keymap_key_get_syms_by_level(km, kc, 0, level, &syms);
			for (int i = 0; i < n; i++) {
				if (syms[i] == sym) {
					*evdev = kc - 8;
					*shift = level == 1;
					return true;
				}
			}
		}
	}
	return false;
}

static const char *modifier_alias(const char *name) {
	if (!strcasecmp(name, "ctrl") || !strcasecmp(name, "control")) return "Control_L";
	if (!strcasecmp(name, "shift")) return "Shift_L";
	if (!strcasecmp(name, "alt")) return "Alt_L";
	if (!strcasecmp(name, "super") || !strcasecmp(name, "win") ||
			!strcasecmp(name, "meta") || !strcasecmp(name, "cmd")) return "Super_L";
	if (!strcasecmp(name, "enter")) return "Return";
	if (!strcasecmp(name, "esc")) return "Escape";
	if (!strcasecmp(name, "del")) return "Delete";
	if (!strcasecmp(name, "pgup")) return "Prior";
	if (!strcasecmp(name, "pgdn")) return "Next";
	return name;
}

static bool name_to_key(struct agent *a, const char *name, uint32_t *evdev, bool *shift) {
	const char *n = modifier_alias(name);
	xkb_keysym_t sym = xkb_keysym_from_name(n, XKB_KEYSYM_NO_FLAGS);
	if (sym == XKB_KEY_NoSymbol) {
		sym = xkb_keysym_from_name(n, XKB_KEYSYM_CASE_INSENSITIVE);
	}
	if (sym == XKB_KEY_NoSymbol && strlen(n) == 1) {
		sym = xkb_utf32_to_keysym((unsigned char)n[0]);
	}
	if (sym == XKB_KEY_NoSymbol) {
		return false;
	}
	return keysym_lookup(a, sym, evdev, shift);
}

// "ctrl+shift+t"：依次按下，再倒序松开。
static bool key_combo(struct agent *a, const char *combo, char *err, size_t errlen) {
	char tmp[256];
	snprintf(tmp, sizeof(tmp), "%s", combo);
	uint32_t codes[16];
	int n = 0;
	char *save = NULL;
	for (char *tok = strtok_r(tmp, "+", &save); tok && n < 15; tok = strtok_r(NULL, "+", &save)) {
		uint32_t code;
		bool shift;
		if (!name_to_key(a, tok, &code, &shift)) {
			snprintf(err, errlen, "unknown key '%s'", tok);
			return false;
		}
		if (shift && n == 0 && !strchr(combo, '+')) {
			uint32_t sc;
			bool dummy;
			if (name_to_key(a, "Shift_L", &sc, &dummy)) {
				codes[n++] = sc;
			}
		}
		codes[n++] = code;
	}
	for (int i = 0; i < n; i++) {
		key_event(a, codes[i], true);
	}
	for (int i = n - 1; i >= 0; i--) {
		key_event(a, codes[i], false);
	}
	return true;
}

static uint32_t utf8_next(const char **p) {
	const unsigned char *s = (const unsigned char *)*p;
	uint32_t cp;
	int len;
	if (s[0] < 0x80) { cp = s[0]; len = 1; }
	else if ((s[0] & 0xe0) == 0xc0) { cp = s[0] & 0x1f; len = 2; }
	else if ((s[0] & 0xf0) == 0xe0) { cp = s[0] & 0x0f; len = 3; }
	else { cp = s[0] & 0x07; len = 4; }
	for (int i = 1; i < len; i++) {
		if ((s[i] & 0xc0) != 0x80) { *p += i; return 0xfffd; }
		cp = (cp << 6) | (s[i] & 0x3f);
	}
	*p += len;
	return cp;
}

// 逐字符敲入；键位表里没有的字符计数返回（待做：像 wtype 那样临时上传键位表）。
static int type_text(struct agent *a, const char *text) {
	uint32_t shift_code;
	bool dummy;
	name_to_key(a, "Shift_L", &shift_code, &dummy);
	int missing = 0;
	const char *p = text;
	while (*p) {
		uint32_t cp = utf8_next(&p);
		xkb_keysym_t sym;
		if (cp == '\n') sym = XKB_KEY_Return;
		else if (cp == '\t') sym = XKB_KEY_Tab;
		else sym = xkb_utf32_to_keysym(cp);
		uint32_t code;
		bool shift;
		if (sym == XKB_KEY_NoSymbol || !keysym_lookup(a, sym, &code, &shift)) {
			missing++;
			continue;
		}
		if (shift) key_event(a, shift_code, true);
		key_event(a, code, true);
		key_event(a, code, false);
		if (shift) key_event(a, shift_code, false);
	}
	return missing;
}

static void seat_request_set_selection(struct wl_listener *listener, void *data) {
	struct agent *a = wl_container_of(listener, a, request_set_selection);
	struct wlr_seat_request_set_selection_event *ev = data;
	wlr_seat_set_selection(a->seat, ev->source, ev->serial);
}

static void seat_request_set_primary_selection(struct wl_listener *listener, void *data) {
	struct agent *a = wl_container_of(listener, a, request_set_primary_selection);
	struct wlr_seat_request_set_primary_selection_event *ev = data;
	wlr_seat_set_primary_selection(a->seat, ev->source, ev->serial);
}

/* ---------- 控制命令 ---------- */

static uint32_t button_code(const char *name) {
	if (!name || !strcmp(name, "left") || !strcmp(name, "1")) return BTN_LEFT;
	if (!strcmp(name, "right") || !strcmp(name, "3")) return BTN_RIGHT;
	if (!strcmp(name, "middle") || !strcmp(name, "2")) return BTN_MIDDLE;
	return 0;
}

static const char *kv(char **args, int nargs, const char *key) {
	size_t kl = strlen(key);
	for (int i = 0; i < nargs; i++) {
		if (!strncmp(args[i], key, kl) && args[i][kl] == '=') {
			return args[i] + kl + 1;
		}
	}
	return NULL;
}

static void cmd_windows(struct conn *c, struct agent *a) {
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
		struct wlr_box *g = &geo;
		struct wl_client *wc = wl_resource_get_client(t->xdg->resource);
		struct client_info *ci = client_lookup(a->srv, wc);
		sb_printf(&b, "%s{\"id\":%u,\"pid\":%d,\"app_id\":", first ? "" : ",", t->id, ci ? ci->pid : -1);
		sb_json_str(&b, t->xdg->app_id);
		sb_printf(&b, ",\"title\":");
		sb_json_str(&b, t->xdg->title);
		// x,y 为窗口几何框（去掉客户端阴影后）在输出上的位置；
		// surface_x/y 为表面左上角，客户端自绘阴影时二者不同。
		sb_printf(&b, ",\"x\":%d,\"y\":%d,\"width\":%d,\"height\":%d,"
			"\"surface_x\":%d,\"surface_y\":%d,\"focused\":%s}",
			x, y, g->width, g->height, x - g->x, y - g->y,
			a->seat->keyboard_state.focused_surface == t->xdg->base->surface ? "true" : "false");
		first = false;
	}
	sb_printf(&b, "]}");
	conn_send(c, &b);
}

static void cmd_stats(struct conn *c, struct agent *a, bool reset) {
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
}

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

// 返回 false 表示命令挂起（截图等稳定），连接暂停处理后续行。
static bool dispatch(struct conn *c, char *line) {
	struct server *srv = c->srv;
	// type 命令取整行剩余部分作文本，单独处理。
	if (!strncmp(line, "type ", 5)) {
		char *rest = line + 5;
		char *sp = strchr(rest, ' ');
		if (!sp) {
			reply_error(c, "usage: type AGENT TEXT");
			return true;
		}
		*sp = '\0';
		struct agent *a = agent_by_name(srv, rest);
		if (!a) {
			reply_error(c, "no agent '%s'", rest);
			return true;
		}
		// 支持 \n \t \\ 转义
		char *text = sp + 1, *w = text;
		for (char *r = text; *r; r++) {
			if (*r == '\\' && r[1]) {
				r++;
				*w++ = *r == 'n' ? '\n' : *r == 't' ? '\t' : *r;
			} else {
				*w++ = *r;
			}
		}
		*w = '\0';
		int missing = type_text(a, text);
		agent_note_action(a);
		struct sbuf b = {0};
		sb_printf(&b, "{\"ok\":true,\"missing\":%d}", missing);
		conn_send(c, &b);
		return true;
	}

	char *args[32];
	int n = 0;
	char *save = NULL;
	for (char *tok = strtok_r(line, " \t", &save); tok && n < 32; tok = strtok_r(NULL, " \t", &save)) {
		args[n++] = tok;
	}
	if (n == 0) {
		return true;
	}
	const char *cmd = args[0];
	if (!strcmp(cmd, "agents")) {
		cmd_agents(c, srv);
		return true;
	}
	if (!strcmp(cmd, "ping")) {
		reply_ok(c);
		return true;
	}
	if (n < 2) {
		reply_error(c, "usage: %s AGENT ...", cmd);
		return true;
	}
	struct agent *a = agent_by_name(srv, args[1]);
	if (!a) {
		reply_error(c, "no agent '%s'", args[1]);
		return true;
	}

	if (!strcmp(cmd, "screenshot")) {
		if (n < 3) {
			reply_error(c, "usage: screenshot AGENT PATH [settle=ms] [timeout=ms] [threshold=px]");
			return true;
		}
		if (a->shot) {
			reply_error(c, "screenshot already pending for %s", a->name);
			return true;
		}
		const char *v;
		struct shot *s = calloc(1, sizeof(*s));
		s->conn = c;
		snprintf(s->path, sizeof(s->path), "%s", args[2]);
		s->settle_ms = (v = kv(args, n, "settle")) ? atoi(v) : 300;
		s->timeout_ms = (v = kv(args, n, "timeout")) ? atoi(v) : 5000;
		// 默认忽略小于 64x64 的损坏（光标闪烁）。
		s->threshold_px = (v = kv(args, n, "threshold")) ? strtoull(v, NULL, 10) : 4096;
		s->start_ms = now_ms();
		s->frames_at_start = a->frames_rendered;
		a->shot = s;
		c->busy = true;
		bool was_running = a->running;
		agent_update_running(a);
		if (was_running) {
			wlr_output_schedule_frame(a->output);
		}
		wl_event_source_timer_update(a->tick_timer, 20);
		return false;
	}
	if (!strcmp(cmd, "move") && n >= 4) {
		pointer_move(a, atof(args[2]), atof(args[3]));
		agent_note_action(a);
		reply_ok(c);
		return true;
	}
	if (!strcmp(cmd, "click") && n >= 4) {
		uint32_t btn = button_code(kv(args, n, "button"));
		const char *cnt = kv(args, n, "count");
		int count = cnt ? atoi(cnt) : 1;
		if (!btn) {
			reply_error(c, "bad button");
			return true;
		}
		pointer_move(a, atof(args[2]), atof(args[3]));
		for (int i = 0; i < count; i++) {
			pointer_button(a, btn, true);
			pointer_button(a, btn, false);
		}
		agent_note_action(a);
		reply_ok(c);
		return true;
	}
	if (!strcmp(cmd, "button") && n >= 4) {
		uint32_t btn = button_code(args[2]);
		if (!btn) {
			reply_error(c, "bad button");
			return true;
		}
		pointer_button(a, btn, !strcmp(args[3], "down"));
		agent_note_action(a);
		reply_ok(c);
		return true;
	}
	if (!strcmp(cmd, "scroll") && n >= 4) {
		pointer_scroll(a, atoi(args[2]), atoi(args[3]));
		agent_note_action(a);
		reply_ok(c);
		return true;
	}
	if ((!strcmp(cmd, "key") || !strcmp(cmd, "keydown") || !strcmp(cmd, "keyup")) && n >= 3) {
		char err[128];
		bool ok;
		if (!strcmp(cmd, "key")) {
			ok = key_combo(a, args[2], err, sizeof(err));
		} else {
			uint32_t code;
			bool shift;
			ok = name_to_key(a, args[2], &code, &shift);
			if (ok) {
				key_event(a, code, !strcmp(cmd, "keydown"));
			} else {
				snprintf(err, sizeof(err), "unknown key '%s'", args[2]);
			}
		}
		agent_note_action(a);
		if (ok) {
			reply_ok(c);
		} else {
			reply_error(c, "%s", err);
		}
		return true;
	}
	if (!strcmp(cmd, "windows")) {
		cmd_windows(c, a);
		return true;
	}
	if (!strcmp(cmd, "stats")) {
		cmd_stats(c, a, n >= 3 && !strcmp(args[2], "reset"));
		return true;
	}
	if (!strcmp(cmd, "run")) {
		// 手动放行 N 毫秒（默认 grace_ms），供测量脚本使用。
		int ms = n >= 3 ? atoi(args[2]) : srv->grace_ms;
		int64_t until = now_ms() + ms;
		if (until > a->run_until_ms) {
			a->run_until_ms = until;
		}
		wl_event_source_timer_update(a->grace_timer, ms > 0 ? ms : 1);
		agent_update_running(a);
		reply_ok(c);
		return true;
	}
	if (!strcmp(cmd, "gate")) {
		a->run_until_ms = 0;
		agent_update_running(a);
		reply_ok(c);
		return true;
	}
	reply_error(c, "unknown command '%s'", cmd);
	return true;
}

static void conn_destroy(struct conn *c) {
	// 挂起的截图照常完成，只是不回复。
	struct agent *a;
	wl_list_for_each(a, &c->srv->agents, link) {
		if (a->shot && a->shot->conn == c) {
			a->shot->conn = NULL;
		}
	}
	wl_event_source_remove(c->src);
	close(c->fd);
	free(c);
}

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
		dispatch(c, line);
	}
}

static void conn_resume(struct conn *c) {
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
				reply_error(c, "line too long");
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

/* ---------- 启动 ---------- */

static int listen_unix(const char *path) {
	int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
	if (fd < 0) {
		return -1;
	}
	struct sockaddr_un addr = {.sun_family = AF_UNIX};
	if (strlen(path) >= sizeof(addr.sun_path)) {
		close(fd);
		errno = ENAMETOOLONG;
		return -1;
	}
	strcpy(addr.sun_path, path);
	unlink(path);
	if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0 || listen(fd, 128) < 0) {
		close(fd);
		return -1;
	}
	chmod(path, 0700);
	return fd;
}

static bool agent_create(struct server *srv, const char *name) {
	struct agent *a = calloc(1, sizeof(*a));
	a->srv = srv;
	snprintf(a->name, sizeof(a->name), "%s", name);
	wl_list_init(&a->toplevels);
	snprintf(a->dir, sizeof(a->dir), "%s/%s", srv->runtime_dir, name);
	mkdir(a->dir, 0700);
	snprintf(a->sock_path, sizeof(a->sock_path), "%s/wayland-0", a->dir);
	a->listen_fd = listen_unix(a->sock_path);
	if (a->listen_fd < 0) {
		wlr_log_errno(WLR_ERROR, "listen %s", a->sock_path);
		return false;
	}
	a->listen_src = wl_event_loop_add_fd(srv->loop, a->listen_fd, WL_EVENT_READABLE, handle_accept, a);

	a->output = wlr_headless_add_output(srv->backend, srv->width, srv->height);
	wlr_output_init_render(a->output, srv->allocator, srv->renderer);
	struct wlr_output_state state;
	wlr_output_state_init(&state);
	wlr_output_state_set_enabled(&state, true);
	wlr_output_state_set_custom_mode(&state, srv->width, srv->height, srv->refresh_mhz);
	wlr_output_commit_state(a->output, &state);
	wlr_output_state_finish(&state);
	char desc[128];
	snprintf(desc, sizeof(desc), "heddle agent %s", name);
	wlr_output_set_description(a->output, desc);
	wlr_output_create_global(a->output, srv->display);

	a->scene = wlr_scene_create();
	a->scene_output = wlr_scene_output_create(a->scene, a->output);
	// 背景：纯灰，便于和未绘制区域区分
	float bg[4] = {0.2f, 0.2f, 0.22f, 1.0f};
	wlr_scene_rect_create(&a->scene->tree, srv->width, srv->height, bg);

	a->output_frame.notify = output_frame;
	wl_signal_add(&a->output->events.frame, &a->output_frame);

	a->seat = wlr_seat_create(srv->display, name);
	wlr_keyboard_init(&a->keyboard, &keyboard_impl, "heddle-keyboard");
	struct xkb_context *ctx = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
	struct xkb_keymap *km = xkb_keymap_new_from_names(ctx, NULL, XKB_KEYMAP_COMPILE_NO_FLAGS);
	wlr_keyboard_set_keymap(&a->keyboard, km);
	xkb_keymap_unref(km);
	xkb_context_unref(ctx);
	wlr_keyboard_set_repeat_info(&a->keyboard, 0, 0); // 注入的按键不需要自动重复
	wlr_seat_set_keyboard(a->seat, &a->keyboard);
	wlr_seat_set_capabilities(a->seat, WL_SEAT_CAPABILITY_POINTER | WL_SEAT_CAPABILITY_KEYBOARD);
	a->request_set_selection.notify = seat_request_set_selection;
	wl_signal_add(&a->seat->events.request_set_selection, &a->request_set_selection);
	a->request_set_primary_selection.notify = seat_request_set_primary_selection;
	wl_signal_add(&a->seat->events.request_set_primary_selection, &a->request_set_primary_selection);
	a->px = srv->width / 2.0;
	a->py = srv->height / 2.0;

	a->grace_timer = wl_event_loop_add_timer(srv->loop, handle_grace_timer, a);
	a->tick_timer = wl_event_loop_add_timer(srv->loop, handle_tick, a);
	a->running = false;
	// 启动时放行一个 grace 期，让应用画出首帧。
	agent_note_action(a);
	wlr_output_schedule_frame(a->output);

	wl_list_insert(srv->agents.prev, &a->link);
	wlr_log(WLR_INFO, "agent %s: WAYLAND_DISPLAY=%s output=%s", name, a->sock_path, a->output->name);
	return true;
}

static int handle_signal(int sig, void *data) {
	wl_display_terminate(data);
	return 0;
}

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

int main(int argc, char **argv) {
	struct server srv = {0};
	g_srv = &srv;
	srv.width = 1920;
	srv.height = 1080;
	srv.refresh_mhz = 60000;
	srv.policy = POLICY_ALWAYS;
	srv.grace_ms = 2000;
	srv.maximize = true;
	const char *names[64];
	int nnames = 0;
	enum wlr_log_importance loglevel = WLR_INFO;
	const char *rt = NULL;

	int opt;
	while ((opt = getopt(argc, argv, "a:s:r:p:g:SwR:vh")) != -1) {
		switch (opt) {
		case 'a':
			if (nnames < 64) names[nnames++] = optarg;
			break;
		case 's':
			if (sscanf(optarg, "%dx%d", &srv.width, &srv.height) != 2) { usage(argv[0]); return 1; }
			break;
		case 'r': srv.refresh_mhz = atoi(optarg) * 1000; break;
		case 'p':
			if (!strcmp(optarg, "always")) srv.policy = POLICY_ALWAYS;
			else if (!strcmp(optarg, "observe")) srv.policy = POLICY_OBSERVE;
			else { usage(argv[0]); return 1; }
			break;
		case 'g': srv.grace_ms = atoi(optarg); break;
		case 'S': srv.use_suspend = true; break;
		case 'w': srv.maximize = false; break;
		case 'R': rt = optarg; break;
		case 'v': loglevel = WLR_DEBUG; break;
		default: usage(argv[0]); return opt == 'h' ? 0 : 1;
		}
	}
	if (nnames == 0) {
		names[nnames++] = "a0";
	}
	wlr_log_init(loglevel, NULL);
	// 单个全屏窗口时 wlroots 会直接把客户端缓冲区送上输出（direct scanout），
	// 截图要读的是合成器自己的帧，客户端缓冲区在 pixman 下转纹理会失败。
	setenv("WLR_SCENE_DISABLE_DIRECT_SCANOUT", "1", 0);

	if (rt) {
		snprintf(srv.runtime_dir, sizeof(srv.runtime_dir), "%s", rt);
	} else {
		const char *xdg = getenv("XDG_RUNTIME_DIR");
		if (!xdg) {
			fprintf(stderr, "XDG_RUNTIME_DIR not set; pass -R\n");
			return 1;
		}
		snprintf(srv.runtime_dir, sizeof(srv.runtime_dir), "%s/heddle", xdg);
	}
	mkdir(srv.runtime_dir, 0700);

	wl_list_init(&srv.agents);
	wl_list_init(&srv.clients);
	srv.display = wl_display_create();
	srv.loop = wl_display_get_event_loop(srv.display);
	wl_display_set_global_filter(srv.display, global_filter, &srv);

	srv.backend = wlr_headless_backend_create(srv.loop);
	srv.renderer = wlr_pixman_renderer_create();
	if (!srv.backend || !srv.renderer) {
		wlr_log(WLR_ERROR, "failed to create backend/renderer");
		return 1;
	}
	wlr_renderer_init_wl_display(srv.renderer, srv.display);
	srv.allocator = wlr_allocator_autocreate(srv.backend, srv.renderer);
	if (!srv.allocator) {
		wlr_log(WLR_ERROR, "failed to create allocator");
		return 1;
	}

	srv.compositor = wlr_compositor_create(srv.display, 6, srv.renderer);
	wlr_subcompositor_create(srv.display);
	wlr_data_device_manager_create(srv.display);
	wlr_primary_selection_v1_device_manager_create(srv.display);
	wlr_viewporter_create(srv.display);
	wlr_single_pixel_buffer_manager_v1_create(srv.display);
	// 不创建 screencopy、virtual-keyboard、virtual-pointer、data-control：
	// 截图和输入只走控制套接字，避免应用跨 agent 读画面或注入。

	srv.new_surface.notify = handle_new_surface;
	wl_signal_add(&srv.compositor->events.new_surface, &srv.new_surface);

	srv.xdg_shell = wlr_xdg_shell_create(srv.display, 6);
	srv.new_toplevel.notify = handle_new_toplevel;
	wl_signal_add(&srv.xdg_shell->events.new_toplevel, &srv.new_toplevel);
	srv.new_popup.notify = handle_new_popup;
	wl_signal_add(&srv.xdg_shell->events.new_popup, &srv.new_popup);

	if (!wlr_backend_start(srv.backend)) {
		wlr_log(WLR_ERROR, "failed to start backend");
		return 1;
	}
	for (int i = 0; i < nnames; i++) {
		if (agent_by_name(&srv, names[i]) || !agent_create(&srv, names[i])) {
			wlr_log(WLR_ERROR, "failed to create agent %s", names[i]);
			return 1;
		}
	}

	char ctl_path[300];
	snprintf(ctl_path, sizeof(ctl_path), "%s/control", srv.runtime_dir);
	srv.ctl_fd = listen_unix(ctl_path);
	if (srv.ctl_fd < 0) {
		wlr_log_errno(WLR_ERROR, "listen %s", ctl_path);
		return 1;
	}
	srv.ctl_src = wl_event_loop_add_fd(srv.loop, srv.ctl_fd, WL_EVENT_READABLE, handle_ctl_accept, &srv);
	signal(SIGPIPE, SIG_IGN);
	wl_event_loop_add_signal(srv.loop, SIGTERM, handle_signal, srv.display);
	wl_event_loop_add_signal(srv.loop, SIGINT, handle_signal, srv.display);

	wlr_log(WLR_INFO, "heddle: control socket %s, policy %s", ctl_path,
		srv.policy == POLICY_ALWAYS ? "always" : "observe");
	// 就绪标记，供脚本等待
	char ready[320];
	snprintf(ready, sizeof(ready), "%s/ready", srv.runtime_dir);
	FILE *rf = fopen(ready, "w");
	if (rf) {
		fprintf(rf, "%d\n", getpid());
		fclose(rf);
	}

	wl_display_run(srv.display);

	unlink(ready);
	unlink(ctl_path);
	struct agent *a;
	wl_list_for_each(a, &srv.agents, link) {
		unlink(a->sock_path);
	}
	wl_display_destroy_clients(srv.display);
	wlr_backend_destroy(srv.backend);
	wl_display_destroy(srv.display);
	return 0;
}
