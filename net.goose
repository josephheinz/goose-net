// net: TCP sockets for Goose.
//
//   import net from "../goose-net";
//
//   let server = net::listen(8080);
//   guard server.ok() else { abort(net::last_error()); }
//   let conn = server.accept();
//   var data: u8[>..] = [];
//   conn.recv(data);
//   conn.send("hello\n");
//   conn.close();
//
// A Socket is a plain file descriptor, so it copies freely and can be handed
// to a worker thread. Calls that fail leave a message in last_error().
// POSIX only (macOS and Linux).

namespace net;

import std;

struct Socket { fd: i32 }

extern fn c_listen(host: const u8[:], port: i32, backlog: i32) -> i32 = "gnet_listen";
extern fn c_connect(host: const u8[:], port: i32, timeout_ms: i64) -> i32 = "gnet_connect";
extern fn c_accept(fd: i32) -> i32 = "gnet_accept";
extern fn c_recv(fd: i32, out: u8[>..]&, max: i64) -> i64 = "gnet_recv";
extern fn c_send_all(fd: i32, data: const u8[:]) -> i32 = "gnet_send_all";
extern fn c_send2(fd: i32, a: const u8[:], b: const u8[:]) -> i32 = "gnet_send2";
extern fn c_close(fd: i32) = "gnet_close";
extern fn c_shutdown_write(fd: i32) = "gnet_shutdown_write";
extern fn c_set_timeout(fd: i32, ms: i64) -> i32 = "gnet_set_timeout";
extern fn c_local_port(fd: i32) -> i32 = "gnet_local_port";
extern fn c_peer(fd: i32, out: u8[>..]&) -> i32 = "gnet_peer";
extern fn c_error(out: u8[>..]&) = "gnet_error";

// A socket that is not open. ok() is false for it.
fn invalid() -> Socket { Socket { fd: -1 } }

// True when the socket is open.
fn ok(s: Socket) -> bool { s.fd >= 0 }

// Listens for TCP connections. host "" or "0.0.0.0" takes every IPv4
// interface, "::" every IPv6 one. Port 0 picks a free port: see local_port.
fn listen(port: i64, host: const u8[:] = "0.0.0.0", backlog: i64 = 128) -> Socket {
    Socket { fd: c_listen(host, port as i32, backlog as i32) }
}

// Waits for the next client.
fn accept(s: Socket) -> Socket { Socket { fd: c_accept(s.fd) } }

// Connects to host:port. host is a name or an address. Gives up after
// timeout_ms, or never when it is negative.
fn connect(host: const u8[:], port: i64, timeout_ms: i64 = 5000) -> Socket {
    Socket { fd: c_connect(host, port as i32, timeout_ms) }
}

// Appends up to max bytes that have arrived, waiting for at least one.
// Returns the count, 0 when the peer has closed, and -1 on an error or a
// timeout.
fn recv(s: Socket, out: u8[>..]&, max: i64 = 65536) -> i64 { c_recv(s.fd, out, max) }

// Reads until the peer closes. False on an error, or when more than limit
// bytes arrive.
fn recv_all(s: Socket, out: u8[>..]&, limit: i64 = 67108864) -> bool {
    let start = out.len;
    loop {
        let n = c_recv(s.fd, out, 65536);
        if n == 0 { return true; }
        if n < 0 { return false; }
        if out.len - start > limit { return false; }
    }
}

// Sends every byte. False when the connection failed.
fn send(s: Socket, data: const u8[:]) -> bool { c_send_all(s.fd, data) != 0 }

// Sends a and then b in one system call where it can, without joining them.
fn send(s: Socket, a: const u8[:], b: const u8[:]) -> bool { c_send2(s.fd, a, b) != 0 }

// Closes the socket.
fn close(s: Socket) { c_close(s.fd); }

// Tells the peer no more data is coming, while still reading its reply.
fn shutdown_write(s: Socket) { c_shutdown_write(s.fd); }

// Makes recv and send give up after ms milliseconds; 0 waits forever.
fn set_timeout(s: Socket, ms: i64) -> bool { c_set_timeout(s.fd, ms) != 0 }

// The port the socket is bound to, or -1.
fn local_port(s: Socket) -> i64 { c_local_port(s.fd) }

// The peer's address as "ip:port", or "" when it is unknown.
fn peer_address(s: Socket) -> u8[>..] {
    var out: u8[>..] = [];
    c_peer(s.fd, out);
    return out;
}

// What the last failed call on this thread went wrong with.
fn last_error() -> u8[>..] {
    var out: u8[>..] = [];
    c_error(out);
    return out;
}

fn format(out: u8[>..]&, s: Socket) { format(out, "Socket { ", s.fd, " }"); }

// ---------------------------------------------------------------- connection loop

// A loop over a listening socket and every connection it accepted. It
// buffers what arrives and gives back one complete HTTP/1.x request at a
// time (the head and a Content-Length body), from whichever connection has
// one, round robin. Connections stay open between requests, for keep-alive.
// The loop makes the listening socket non-blocking.
struct Loop { handle: u64 }

// What Loop.next found.
enum Event {
    Request,        // a whole request, from conn
    TooLarge,       // its body is over the limit
    HeadTooLarge,   // its head is over 64 KB
    Unsupported,    // a chunked request body
    BadLength,      // a malformed Content-Length
    Failed          // poll failed
}

extern fn c_loop_new(listen_fd: i32, idle_ms: i64, max_request: i64, accept_max: i64) -> u64 = "gnet_loop_new";
extern fn c_loop_next(h: u64, out: u8[>..]&, fd: i32&) -> i32 = "gnet_loop_next";
extern fn c_loop_close(h: u64, fd: i32) = "gnet_loop_close";
extern fn c_loop_free(h: u64) = "gnet_loop_free";
extern fn c_loop_count(h: u64) -> i32 = "gnet_loop_count";

// Starts a loop on server. Connections idle for idle_ms are closed; 0 keeps
// them open. Bodies over max_request are rejected. accept_max caps the
// connections taken per wake (0 = all waiting); with several loops on one
// listening socket, 1 spreads a burst across them.
fn connection_loop(server: Socket, idle_ms: i64 = 5000, max_request: i64 = 1048576, accept_max: i64 = 0) -> Loop {
    Loop { handle: c_loop_new(server.fd, idle_ms, max_request, accept_max) }
}

// Waits for the next request and appends its bytes to out. The Socket is
// the connection it came on: answer with send, then either leave it open or
// call close_connection. After anything but Event.Request, close it.
fn next(l: Loop, out: u8[>..]&) -> Socket, Event {
    var fd: i32 = -1;
    let ev = c_loop_next(l.handle, out, fd);
    let event = match ev {
        1 => Event.Request,
        2 => Event.TooLarge,
        3 => Event.HeadTooLarge,
        4 => Event.Unsupported,
        5 => Event.BadLength,
        _ => Event.Failed,
    };
    return Socket { fd: fd }, event;
}

// Closes a connection the loop gave back and forgets it.
fn close_connection(l: Loop, conn: Socket) { c_loop_close(l.handle, conn.fd); }

// How many connections are open.
fn open_connections(l: Loop) -> i64 { c_loop_count(l.handle) }

// Closes every connection and frees the loop. The listening socket stays open.
fn free(l: Loop) { c_loop_free(l.handle); }
