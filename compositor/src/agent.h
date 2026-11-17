// agent 的创建，以及"哪个客户端属于哪个 agent"。
#ifndef HEDDLE_AGENT_H
#define HEDDLE_AGENT_H

#include "heddle.h"

// 建 agent 的套接字、无头输出、场景图、seat，并加入 srv->agents。
bool agent_create(struct server *srv, const char *name);
struct agent *agent_by_name(struct server *srv, const char *name);

struct client_info *client_lookup(struct server *srv, const struct wl_client *client);
struct agent *client_agent(struct server *srv, const struct wl_client *client);

// wl_display_set_global_filter 的回调：seat 与输出只给所属 agent 的客户端看到。
bool agent_global_filter(const struct wl_client *client, const struct wl_global *global, void *data);

#endif
