// Part of goose-net. Import with `import net.lifecycle;`.
// Graceful shutdown, listener inheritance and a restart that drops no connection.

namespace net;

import std;
import net;

extern fn c_stop_on_signals(delay_ms: i64, drain_ms: i64) = "gnet_stop_on_signals";
extern fn c_request_stop() = "gnet_request_stop";
extern fn c_stop_phase() -> i64 = "gnet_stop_phase";
extern fn c_wait_stop(ms: i64) -> i32 = "gnet_wait_stop";
extern fn c_listen_inherited(host: const u8[:], port: i32, backlog: i32, inherited: i32&) -> i32 = "gnet_listen_inherited";
extern fn c_enable_handoff(fd: i32, argv: const u8[:]) -> i32 = "gnet_enable_handoff";
extern fn c_ready() = "gnet_ready";

// Handles SIGTERM and SIGINT on a thread of its own: phase 1 for delay_ms, then phase 2 for up to drain_ms,
// then the process exits. A second signal exits at once. Call it first in main, before any thread starts.
fn stop_on_signals(delay_ms: i64 = 0, drain_ms: i64 = 10000) { c_stop_on_signals(delay_ms, drain_ms); }

// The same as a SIGTERM.
fn request_stop() { c_request_stop(); }

// 0 running, 1 stopping (still accepting), 2 draining (no new connections).
fn stop_phase() -> i64 { c_stop_phase() }

fn stopping() -> bool { c_stop_phase() > 0 }

fn draining() -> bool { c_stop_phase() > 1 }

// Sleeps up to ms; true once stopping.
fn wait_stop(ms: i64) -> bool { c_wait_stop(ms) != 0 }

// Like listen, but takes fd 3 when systemd (LISTEN_FDS) or a handoff passed one. The bool says it was inherited.
fn listen_inherited(port: i64, host: const u8[:] = "0.0.0.0", backlog: i64 = 128) -> Socket, bool {
    var inherited: i32 = 0;
    let fd = c_listen_inherited(host, port as i32, backlog as i32, inherited);
    return Socket { fd: fd }, inherited != 0;
}

// SIGUSR2 starts this program again with the same arguments and hands it server; once the new process calls
// ready, this one stops. argv is the program arguments joined by NUL bytes.
fn enable_handoff(server: Socket, argv: const u8[:]) { c_enable_handoff(server.fd, argv); }

// Call once start-up is done and the workers run: tells systemd (READY=1) and, in a handoff child, stops the old process.
fn ready() { c_ready(); }
