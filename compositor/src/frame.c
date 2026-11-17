#include "frame.h"

#include <stdlib.h>
#include <time.h>
#include <wlr/types/wlr_compositor.h>
#include <wlr/types/wlr_output.h>
#include <wlr/types/wlr_scene.h>
#include <wlr/types/wlr_xdg_shell.h>

#include "agent.h"
#include "screenshot.h"
#include "util.h"
#include "window.h"

/* ---------- 放行与停帧 ---------- */

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

void agent_update_running(struct agent *a) {
	agent_set_running(a, agent_should_run(a));
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

void agent_run_for(struct agent *a, int ms) {
	int64_t until = now_ms() + ms;
	if (until > a->run_until_ms) {
		a->run_until_ms = until;
	}
	wl_event_source_timer_update(a->grace_timer, ms > 0 ? ms : 1);
	agent_update_running(a);
}

// 动作之后放行 grace_ms，让依赖帧时钟的逻辑在模型等待期跑完。
void agent_note_action(struct agent *a) {
	agent_run_for(a, a->srv->grace_ms);
}

void agent_gate(struct agent *a) {
	a->run_until_ms = 0;
	agent_update_running(a);
}

/* ---------- 每帧 ---------- */

// 有损坏就合成一帧并提交到输出；成功时 last_buffer 换成新帧（截图读它）。
static void render_if_damaged(struct agent *a) {
	struct wlr_scene_output *so = a->scene_output;
	if (!a->output->needs_frame && !pixman_region32_not_empty(&so->pending_commit_damage) &&
			a->last_buffer) {
		return;
	}
	struct wlr_output_state state;
	wlr_output_state_init(&state);
	if (wlr_scene_output_build_state(so, &state, NULL)) {
		struct wlr_buffer *buf = (state.committed & WLR_OUTPUT_STATE_BUFFER) ? state.buffer : NULL;
		if (buf) {
			wlr_buffer_lock(buf);
		}
		if (wlr_output_commit_state(a->output, &state) && buf) {
			if (a->last_buffer) {
				wlr_buffer_unlock(a->last_buffer);
			}
			a->last_buffer = buf;
			buf = NULL;
			a->frames_rendered++;
		}
		if (buf) {
			wlr_buffer_unlock(buf);
		}
	}
	wlr_output_state_finish(&state);
}

static void output_frame(struct wl_listener *listener, void *data) {
	struct agent *a = wl_container_of(listener, a, output_frame);
	if (!a->running) {
		a->frames_skipped++;
		return;
	}
	render_if_damaged(a);
	struct timespec now;
	clock_gettime(CLOCK_MONOTONIC, &now);
	wlr_scene_output_send_frame_done(a->scene_output, &now);
	a->frame_done_sent++;
	if (a->shot) {
		shot_try_finish(a);
	}
}

void frame_init_agent(struct agent *a) {
	a->output_frame.notify = output_frame;
	wl_signal_add(&a->output->events.frame, &a->output_frame);
	a->grace_timer = wl_event_loop_add_timer(a->srv->loop, handle_grace_timer, a);
	a->running = false;
	// 启动时放行一个 grace 期，让应用画出首帧。
	agent_note_action(a);
	wlr_output_schedule_frame(a->output);
}

/* ---------- 表面提交：统计与稳定判定 ---------- */

struct surface_info {
	struct server *srv;
	struct wlr_surface *surface;
	struct wl_listener commit;
	struct wl_listener destroy;
};

static void surface_commit(struct wl_listener *listener, void *data) {
	struct surface_info *si = wl_container_of(listener, si, commit);
	struct wlr_surface *surface = si->surface;
	struct client_info *ci = client_lookup(si->srv, wl_resource_get_client(surface->resource));
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
	// 截图期间小于阈值的损坏（光标闪烁）不算"画面在变"。
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
	struct server *srv = wl_container_of(listener, srv, new_surface);
	struct wlr_surface *surface = data;
	struct surface_info *si = calloc(1, sizeof(*si));
	si->srv = srv;
	si->surface = surface;
	si->commit.notify = surface_commit;
	wl_signal_add(&surface->events.commit, &si->commit);
	si->destroy.notify = surface_destroy;
	wl_signal_add(&surface->events.destroy, &si->destroy);
}

void frame_init_server(struct server *srv) {
	srv->new_surface.notify = handle_new_surface;
	wl_signal_add(&srv->compositor->events.new_surface, &srv->new_surface);
}
