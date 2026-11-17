// 拼 JSON 回复用的增长字符串。
#ifndef HEDDLE_JSON_H
#define HEDDLE_JSON_H

#include <stddef.h>

struct sbuf {
	char *data;
	size_t len, cap;
};

void sb_putn(struct sbuf *b, const char *s, size_t n);
void sb_printf(struct sbuf *b, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
// 写一个带引号、已转义的 JSON 字符串；s 为 NULL 时写 ""。
void sb_json_str(struct sbuf *b, const char *s);
void sb_free(struct sbuf *b);

#endif
