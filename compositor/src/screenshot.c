#include "screenshot.h"

#include <drm_fourcc.h>
#include <errno.h>
#include <png.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wlr/render/wlr_texture.h>
#include <wlr/types/wlr_output.h>
#include <wlr/types/wlr_scene.h>

#include "control.h"
#include "frame.h"
#include "json.h"
#include "util.h"

#define TICK_MS 20

/* ---------- PNG 编码 ---------- */

// 写到 path.tmp 再改名，读者不会看到写了一半的文件。
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

/* ---------- 等稳定 ---------- */

static void shot_reply(struct agent *a, struct shot *s, const char *status, int64_t latency) {
	char err[256] = "";
	if (!a->last_buffer) {
		conn_reply_error(s->conn, "screenshot failed: no frame rendered");
		return;
	}
	if (!write_png(s->path, a->last_buffer, a->srv->renderer, err, sizeof(err))) {
		conn_reply_error(s->conn, "screenshot failed: %s", err);
		return;
	}
	struct sbuf b = {0};
	sb_printf(&b, "{\"ok\":true,\"status\":\"%s\",\"latency_ms\":%lld,"
		"\"frames\":%llu,\"width\":%d,\"height\":%d,\"cursor\":[%d,%d],\"path\":",
		status, (long long)latency,
		(unsigned long long)(a->frames_rendered - s->frames_at_start),
		a->output->width, a->output->height, (int)a->px, (int)a->py);
	sb_json_str(&b, s->path);
	sb_printf(&b, "}");
	conn_send(s->conn, &b);
}

static void shot_complete(struct agent *a, const char *status) {
	struct shot *s = a->shot;
	a->shot = NULL;
	wl_event_source_timer_update(a->tick_timer, 0);
	int64_t latency = now_ms() - s->start_ms;
	if (strcmp(status, "stable") == 0) {
		a->shots_stable++;
	} else {
		a->shots_timeout++;
	}
	if (s->conn) {
		shot_reply(a, s, status, latency);
		conn_unblock(s->conn);
	} else if (a->last_buffer) {
		// 没人等回复也照常写文件
		char err[256];
		write_png(s->path, a->last_buffer, a->srv->renderer, err, sizeof(err));
	}
	free(s);
	agent_update_running(a);
}

void shot_try_finish(struct agent *a) {
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
		wl_event_source_timer_update(a->tick_timer, TICK_MS);
	}
	return 0;
}

void shot_start(struct agent *a, struct conn *c, const char *path,
		int settle_ms, int timeout_ms, uint64_t threshold_px) {
	struct shot *s = calloc(1, sizeof(*s));
	s->conn = c;
	snprintf(s->path, sizeof(s->path), "%s", path);
	s->settle_ms = settle_ms;
	s->timeout_ms = timeout_ms;
	s->threshold_px = threshold_px;
	s->start_ms = now_ms();
	s->frames_at_start = a->frames_rendered;
	a->shot = s;

	bool was_running = a->running;
	agent_update_running(a); // 停帧中则放行（放行时会安排一帧）
	if (was_running) {
		wlr_output_schedule_frame(a->output);
	}
	wl_event_source_timer_update(a->tick_timer, TICK_MS);
}

void shot_detach_conn(struct server *srv, struct conn *c) {
	struct agent *a;
	wl_list_for_each(a, &srv->agents, link) {
		if (a->shot && a->shot->conn == c) {
			a->shot->conn = NULL;
		}
	}
}

void screenshot_init_agent(struct agent *a) {
	a->tick_timer = wl_event_loop_add_timer(a->srv->loop, handle_tick, a);
}
