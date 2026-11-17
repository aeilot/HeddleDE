#include "util.h"

#include <errno.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

int64_t now_ms(void) {
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

uint32_t now_msec32(void) {
	return (uint32_t)now_ms();
}

uint64_t region_area(pixman_region32_t *r) {
	int n;
	pixman_box32_t *boxes = pixman_region32_rectangles(r, &n);
	uint64_t area = 0;
	for (int i = 0; i < n; i++) {
		area += (uint64_t)(boxes[i].x2 - boxes[i].x1) * (uint64_t)(boxes[i].y2 - boxes[i].y1);
	}
	return area;
}

int listen_unix(const char *path) {
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
