/* Hub core: long-lived connections (chunked streams and WebSockets) on one poll thread.
   Plain C with no goose.h so it can be unit-tested; src/hub.c wraps it for Goose.

   Threads: hub_add, hub_send, hub_publish, hub_close and hub_count may be called from any
   thread. hub_next must be called by one thread only; it does all socket I/O. */
#ifndef GNET_HUB_CORE_H
#define GNET_HUB_CORE_H

#include <stddef.h>
#include <stdint.h>

/* A growable byte buffer owned by the caller (zero-initialise, then hub_buf_free). */
typedef struct { uint8_t *p; size_t len, cap; } hub_buf;

int  hub_buf_reserve(hub_buf *b, size_t extra);              /* 1 ok, 0 out of memory */
int  hub_buf_append(hub_buf *b, const void *data, size_t n); /* 1 ok, 0 out of memory */
void hub_buf_free(hub_buf *b);

/* Connection kinds and events; the Goose enums mirror these values. */
enum { HUB_STREAM = 0, HUB_WEBSOCKET = 1 };
enum { HUB_OPEN = 0, HUB_TEXT = 1, HUB_BINARY = 2, HUB_CLOSE = 3, HUB_FAILED = 4 };

#define HUB_MAX_TOPIC 64
#define HUB_MAX_QUEUE (64u * 1024 * 1024)   /* payload bytes queued for the hub thread */

typedef struct hub hub;

/* ping_ms <= 0 turns pings, heartbeats and the silence timeout off. */
hub *hub_new(int64_t max_conns, int64_t max_message, int64_t out_limit, int64_t ping_ms);
/* Closes every remaining fd. No thread may be inside any hub call. */
void hub_free(hub *h);

/* The hub owns fd from now on. Returns the connection id (never reused), or 0 when refused
   (hub full, bad kind, topic over 64 bytes, out of memory); the fd is closed then.
   pending is input already read from the socket. heartbeat applies to streams. */
uint64_t hub_add(hub *h, int fd, int kind, const uint8_t *topic, size_t topic_len,
                 const uint8_t *pending, size_t pending_len,
                 const uint8_t *heartbeat, size_t heartbeat_len);
/* 0 when the queue is over HUB_MAX_QUEUE; 1 otherwise (an unknown id is dropped later). */
int  hub_send(hub *h, uint64_t id, const uint8_t *data, size_t n, int binary);
int  hub_publish(hub *h, const uint8_t *topic, size_t topic_len,
                 const uint8_t *data, size_t n, int binary);
void hub_close(hub *h, uint64_t id);                          /* graceful */
/* Connections with this topic, including ones not yet taken in; topic_len 0 counts all. */
int64_t hub_count(hub *h, const uint8_t *topic, size_t topic_len);

/* Runs the poll loop until an event. Returns the event; *id is the connection (0 for
   HUB_FAILED, which means the poll itself failed). out gets the message (Text, Binary),
   topic the connection's topic; both are reset first. */
int  hub_next(hub *h, hub_buf *out, hub_buf *topic, uint64_t *id);

/* --- Codecs, public for tests --- */

void hub_sha1(const uint8_t *data, size_t n, uint8_t out[20]);
size_t hub_base64(const uint8_t *data, size_t n, char *out);  /* writes 4*ceil(n/3) bytes, no NUL */
void hub_ws_accept_key(const uint8_t *key, size_t keylen, char out[28]);
int  hub_utf8_valid(const uint8_t *p, size_t n);              /* strict: no overlongs or surrogates */

enum { WS_CONT = 0, WS_TEXT = 1, WS_BINARY = 2, WS_CLOSE = 8, WS_PING = 9, WS_PONG = 10 };

typedef struct {
    int fin, opcode, masked;
    uint64_t len;
    uint8_t mask[4];
} hub_ws_frame;

/* Parses a frame header. Returns its length in bytes, 0 when more input is needed, -1 on a
   protocol error (reserved bits or opcode, fragmented or long control frame, 64-bit length
   with the top bit set, or an unmasked frame when require_mask). */
long hub_ws_parse_head(const uint8_t *in, size_t n, int require_mask, hub_ws_frame *f);
void hub_ws_unmask(uint8_t *p, size_t n, const uint8_t mask[4]);
/* Header and whole payload: returns bytes consumed, 0 when incomplete, -1 on error. The
   payload is unmasked and appended to payload (cleared first). */
long hub_ws_decode(const uint8_t *in, size_t n, int require_mask, hub_ws_frame *f,
                   hub_buf *payload);
/* Appends one frame; mask is NULL for a server frame, 4 bytes for a client frame. */
int  hub_ws_encode(hub_buf *out, int opcode, int fin, const uint8_t *p, size_t n,
                   const uint8_t *mask);

#endif
