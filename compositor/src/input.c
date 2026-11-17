#include "input.h"

#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <wlr/interfaces/wlr_keyboard.h>
#include <wlr/types/wlr_data_device.h>
#include <wlr/types/wlr_output.h>
#include <wlr/types/wlr_primary_selection.h>
#include <wlr/types/wlr_seat.h>
#include <xkbcommon/xkbcommon.h>

#include "util.h"
#include "window.h"

/* ---------- 指针 ---------- */

void pointer_move(struct agent *a, double x, double y) {
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

void pointer_button(struct agent *a, uint32_t button, bool pressed) {
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

void pointer_scroll(struct agent *a, int dx, int dy) {
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

/* ---------- 键盘 ---------- */

void key_event(struct agent *a, uint32_t evdev_code, bool pressed) {
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
					*evdev = kc - 8; // xkb 键码 = evdev 键码 + 8
					*shift = level == 1;
					return true;
				}
			}
		}
	}
	return false;
}

static const char *key_alias(const char *name) {
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

bool name_to_key(struct agent *a, const char *name, uint32_t *evdev, bool *shift) {
	const char *n = key_alias(name);
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

bool key_combo(struct agent *a, const char *combo, char *err, size_t errlen) {
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
		// 单个需要 Shift 的键（如 "A"、"exclam"）自动补上 Shift
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

/* ---------- 文本 ---------- */

// 解一个 UTF-8 码点并前移 *p；非法序列返回 U+FFFD。
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

// 待做：键位表里没有的字符，像 wtype 那样临时上传键位表。
int type_text(struct agent *a, const char *text) {
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

/* ---------- seat ---------- */

static void keyboard_led_update(struct wlr_keyboard *kb, uint32_t leds) {
}

static const struct wlr_keyboard_impl keyboard_impl = {
	.name = "heddle-keyboard",
	.led_update = keyboard_led_update,
};

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

void input_init_agent(struct agent *a) {
	struct server *srv = a->srv;
	a->seat = wlr_seat_create(srv->display, a->name);

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
}
