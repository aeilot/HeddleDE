#include "json.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void sb_putn(struct sbuf *b, const char *s, size_t n) {
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

void sb_printf(struct sbuf *b, const char *fmt, ...) {
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

void sb_json_str(struct sbuf *b, const char *s) {
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

void sb_free(struct sbuf *b) {
	free(b->data);
	b->data = NULL;
	b->len = b->cap = 0;
}
