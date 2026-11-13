# Changelog

## v1.0.0

No code change. Tagged with gweb v1.0.0. From here gweb, goose-net and goose-sqlite follow semver together (gweb docs/site/stability.md).

## v0.7.0

No code change. Tagged with gweb v0.7.0.

## v0.6.0

No code change. Tagged with gweb v0.6.0.

## v0.5.1

Tagged with gweb v0.5.1. Tests only: the C test macro `WAIT_FOR` builds under GCC `-Werror`, and the WebSocket count test waits on time, not a spin count.

## v0.5.0

No code change. Tagged with gweb v0.5.0. CI: GitHub Actions test on macOS and Linux and publish a release for each `v*` tag.

## v0.4.0

No code change. Tagged with gweb v0.4.0 (the renamed goose-web), which pins one tag for all three packages.

## v0.3.0

- `http::Status.SwitchingProtocols` (101).
- `http::send_head`, `begin_chunked`, `send_chunk`, `end_chunked`, `format_chunk` and `supports_chunked` for streamed replies, with an unframed HTTP/1.0 mode.
- `http::accepts(req, coding)` parses `Accept-Encoding`.
- `net::detach` (`gnet_loop_detach`) takes a connection out of a `Loop` without closing it and hands back its unread bytes.

- `net::Hub` (`import net.hub`): `hub`, `add`, `send`, `publish`, `close`, `count` and `next` for chunked streams and WebSockets on one poll thread, with `HubKind` and `HubEvent`. C core in `src/hub_core.c`, glue in `src/hub.c`.
- `http::websocket_accept(key)` computes `Sec-WebSocket-Accept`.
- `examples/ws_echo.goose`.
- Opt-in spool mode for the connection loop: `set_uploads` sends multipart bodies over a threshold to a file (`Event.Spooled`, `spool_path`), with a separate size limit. Off unless called.
- `net.lifecycle`: `stop_on_signals`, `request_stop`, `stop_phase`, `stopping`, `draining`, `wait_stop`, `listen_inherited`, `enable_handoff` and `ready`. Graceful stop on SIGTERM and SIGINT, systemd socket activation, `sd_notify`, and a zero-downtime restart on SIGUSR2.
- `Loop.next` drains when the process stops: it closes idle connections, finishes requests already under way, then returns the new `Event.Stopped`. `http::serve` returns on it.
- `test/c/run.sh`: C tests for the native code.

## v0.2.1

No code change. Tagged with goose-web v0.2.1, which pins one tag for all three packages.

## v0.2.0

No code change. Tagged with goose-web v0.2.0, which pins one tag for all three packages.

## v0.1.1

The repo moved from `jheinz-bzs` to `josephheinz`. No code change.

## v0.1.0

First tagged release: TCP sockets, the connection loop, an HTTP/1.1 server and a client.
