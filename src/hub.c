/* Goose glue over hub_core: the handle is the hub pointer, like gnet_loop. */
#include "goose.h"
#include "hub_core.h"

#include <stdint.h>

typedef struct { const uint8_t *data; int64_t len; } gnet_bytes;

uint64_t gnet_hub_new(int64_t max_conns, int64_t max_message, int64_t out_limit, int64_t ping_ms) {
    return (uint64_t)(uintptr_t)hub_new(max_conns, max_message, out_limit, ping_ms);
}

uint64_t gnet_hub_add(uint64_t h, int32_t fd, int32_t kind, gnet_bytes topic, gnet_bytes pending,
                      gnet_bytes heartbeat) {
    return hub_add((hub *)(uintptr_t)h, fd, kind, topic.data, (size_t)topic.len,
                   pending.data, (size_t)pending.len, heartbeat.data, (size_t)heartbeat.len);
}

int32_t gnet_hub_send(uint64_t h, uint64_t id, gnet_bytes data, int32_t binary) {
    return hub_send((hub *)(uintptr_t)h, id, data.data, (size_t)data.len, binary);
}

int32_t gnet_hub_publish(uint64_t h, gnet_bytes topic, gnet_bytes data, int32_t binary) {
    return hub_publish((hub *)(uintptr_t)h, topic.data, (size_t)topic.len, data.data, (size_t)data.len, binary);
}

void gnet_hub_close(uint64_t h, uint64_t id) { hub_close((hub *)(uintptr_t)h, id); }

int64_t gnet_hub_count(uint64_t h, gnet_bytes topic) {
    return hub_count((hub *)(uintptr_t)h, topic.data, (size_t)topic.len);
}

/* Only the hub thread calls this, so the buffers live for one call and are freed after the copy. */
int32_t gnet_hub_next(uint64_t h, gs_rref out, gs_rref topic, uint64_t *id) {
    hub_buf msg = {0}, top = {0};
    int ev = hub_next((hub *)(uintptr_t)h, &msg, &top, id);
    if (msg.len) gs_bld_append(out, msg.p, msg.len);
    if (top.len) gs_bld_append(topic, top.p, top.len);
    hub_buf_free(&msg);
    hub_buf_free(&top);
    return ev;
}

void gnet_ws_accept_key(gnet_bytes key, gs_rref out) {
    char accept[28];
    hub_ws_accept_key(key.data, (size_t)key.len, accept);
    gs_bld_append(out, accept, sizeof accept);
}
