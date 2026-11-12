// net::Hub: long-lived connections (chunked streams and WebSockets) served by one poll thread.
// Any thread may add, send, publish, close and count; one thread calls next, which does all I/O.

namespace net;

import std;

struct Hub { handle: u64 }

enum HubKind { Stream, WebSocket }

enum HubEvent { Open, Text, Binary, Close, Failed }

extern fn c_hub_new(max_conns: i64, max_message: i64, out_limit: i64, ping_ms: i64) -> u64 = "gnet_hub_new";
extern fn c_hub_add(h: u64, fd: i32, kind: i32, topic: const u8[:], pending: const u8[:], heartbeat: const u8[:]) -> u64 = "gnet_hub_add";
extern fn c_hub_send(h: u64, id: u64, data: const u8[:], binary: i32) -> i32 = "gnet_hub_send";
extern fn c_hub_publish(h: u64, topic: const u8[:], data: const u8[:], binary: i32) -> i32 = "gnet_hub_publish";
extern fn c_hub_close(h: u64, id: u64) = "gnet_hub_close";
extern fn c_hub_count(h: u64, topic: const u8[:]) -> i64 = "gnet_hub_count";
extern fn c_hub_next(h: u64, out: u8[>..]&, topic: u8[>..]&, id: u64&) -> i32 = "gnet_hub_next";

// ping_ms <= 0 turns off pings, stream heartbeats and the silence timeout (WebSockets are
// closed after 2.5 x ping_ms without input).
fn hub(max_conns: i64 = 10000, max_message: i64 = 65536, out_limit: i64 = 1048576, ping_ms: i64 = 30000) -> Hub {
    Hub { handle: c_hub_new(max_conns, max_message, out_limit, ping_ms) }
}

// The hub owns the socket from now on. Returns the connection id (never reused), or 0 when the hub is
// full or the topic is over 64 bytes; the socket is closed then. pending is input already read from it
// (the rest from net::detach). heartbeat is sent as a chunk every ping_ms on a Stream.
fn add(h: Hub, conn: Socket, kind: HubKind, topic: const u8[:], pending: const u8[:], heartbeat: const u8[:] = "") -> u64 {
    let k: i32 = match kind { Stream => 0, WebSocket => 1 };
    c_hub_add(h.handle, conn.fd, k, topic, pending, heartbeat)
}

// False when the hub's queue is over its limit. An id that is gone is dropped silently.
fn send(h: Hub, id: u64, data: const u8[:], binary: bool = false) -> bool {
    c_hub_send(h.handle, id, data, if binary { 1 } else { 0 }) != 0
}

// To every connection whose topic is exactly topic.
fn publish(h: Hub, topic: const u8[:], data: const u8[:], binary: bool = false) -> bool {
    c_hub_publish(h.handle, topic, data, if binary { 1 } else { 0 }) != 0
}

// Closes gracefully: a close frame, or the final chunk, then the socket.
fn close(h: Hub, id: u64) { c_hub_close(h.handle, id); }

// Connections with this topic, including ones next has not reported yet; "" counts all.
fn count(h: Hub, topic: const u8[:]) -> i64 { c_hub_count(h.handle, topic) }

// Hub thread only. Runs the poll loop until an event and returns the connection id and the event.
// out gets the message for Text and Binary; topic gets the connection's topic. Failed (id 0) means
// poll itself failed.
fn next(h: Hub, out: u8[>..]&, topic: u8[>..]&) -> u64, HubEvent {
    var id: u64 = 0;
    let ev = c_hub_next(h.handle, out, topic, id);
    let event = match ev {
        0 => HubEvent.Open,
        1 => HubEvent.Text,
        2 => HubEvent.Binary,
        3 => HubEvent.Close,
        _ => HubEvent.Failed,
    };
    return id, event;
}
