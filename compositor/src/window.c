#include "window.h"

#include <stdlib.h>
#include <wlr/types/wlr_output.h>
#include <wlr/types/wlr_scene.h>
#include <wlr/types/wlr_seat.h>
#include <wlr/types/wlr_xdg_shell.h>

#include "agent.h"
#include "util.h"

/* ---------- 命中测试与焦点 ---------- */

struct toplevel *toplevel_at(struct agent *a, double x, double y,
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
	// 往上找到窗口的根节点（node.data 指向 toplevel）
	struct wlr_scene_tree *tree = node->parent;
	while (tree && !tree->node.data) {
		tree = tree->node.parent;
	}
	return tree ? tree->node.data : NULL;
}

void focus_toplevel(struct toplevel *t) {
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

/* ---------- toplevel ---------- */

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
	if (a->seat->keyboard_state.focused_surface != t->xdg->base->surface) {
		return;
	}
	// 失去焦点的窗口不再可见：焦点交给下一个已映射的窗口
	wlr_seat_keyboard_notify_clear_focus(a->seat);
	struct toplevel *o;
	wl_list_for_each(o, &a->toplevels, link) {
		if (o != t && o->mapped) {
			focus_toplevel(o);
			break;
		}
	}
}

// 首次提交时定尺寸：默认最大化到输出大小，-w 时由客户端自定。
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
	struct wl_client *client = wl_resource_get_client(xdg->resource);
	struct agent *a = client_agent(srv, client);
	if (!a) {
		wl_client_post_implementation_error(client, "heddle: unknown client");
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

/* ---------- popup ---------- */

struct popup {
	struct server *srv;
	struct wlr_xdg_popup *xdg;
	struct wl_listener commit, destroy;
};

// 首次提交时把弹出菜单约束在输出范围内。
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
	struct agent *a = client_agent(p->srv, wl_resource_get_client(p->xdg->resource));
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
	struct server *srv = wl_container_of(listener, srv, new_popup);
	struct wlr_xdg_popup *xdg = data;
	struct popup *p = calloc(1, sizeof(*p));
	p->srv = srv;
	p->xdg = xdg;
	// 挂在父表面的场景树下，随父窗口移动
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

void window_init(struct server *srv) {
	srv->xdg_shell = wlr_xdg_shell_create(srv->display, 6);
	srv->new_toplevel.notify = handle_new_toplevel;
	wl_signal_add(&srv->xdg_shell->events.new_toplevel, &srv->new_toplevel);
	srv->new_popup.notify = handle_new_popup;
	wl_signal_add(&srv->xdg_shell->events.new_popup, &srv->new_popup);
}
