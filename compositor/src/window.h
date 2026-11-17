// 窗口：xdg toplevel 与 popup、焦点、命中测试。
#ifndef HEDDLE_WINDOW_H
#define HEDDLE_WINDOW_H

#include "heddle.h"

struct wlr_surface;

struct toplevel {
	struct wl_list link; // agent.toplevels
	struct agent *agent;
	uint32_t id;
	struct wlr_xdg_toplevel *xdg;
	struct wlr_scene_tree *tree;
	bool mapped;
	struct wl_listener map, unmap, commit, destroy;
};

// 创建 xdg_wm_base 并注册新窗口与弹出菜单的处理。
void window_init(struct server *srv);

// 输出坐标 (x, y) 处的表面及其所属窗口；*sx/*sy 为表面内坐标。
struct toplevel *toplevel_at(struct agent *a, double x, double y,
	struct wlr_surface **surface, double *sx, double *sy);
// 置顶、激活，并把键盘焦点给它。
void focus_toplevel(struct toplevel *t);

#endif
