# Changelog

## Unreleased

- `http::Status.SwitchingProtocols` (101).
- `http::send_head`, `begin_chunked`, `send_chunk`, `end_chunked`, `format_chunk` and `supports_chunked` for streamed replies, with an unframed HTTP/1.0 mode.
- `http::accepts(req, coding)` parses `Accept-Encoding`.
- `net::detach` (`gnet_loop_detach`) takes a connection out of a `Loop` without closing it and hands back its unread bytes.

- `net::Hub` (`import net.hub`): `hub`, `add`, `send`, `publish`, `close`, `count` and `next` for chunked streams and WebSockets on one poll thread, with `HubKind` and `HubEvent`. C core in `src/hub_core.c`, glue in `src/hub.c`.
- `http::websocket_accept(key)` computes `Sec-WebSocket-Accept`.
- `examples/ws_echo.goose`.
- Opt-in spool mode for the connection loop: `set_uploads` sends multipart bodies over a threshold to a file (`Event.Spooled`, `spool_path`), with a separate size limit. Off unless called.

## v0.2.1

No code change. Tagged with goose-web v0.2.1, which pins one tag for all three packages.

## v0.2.0

No code change. Tagged with goose-web v0.2.0, which pins one tag for all three packages.

## v0.1.1

The repo moved from `jheinz-bzs` to `josephheinz`. No code change.

## v0.1.0

First tagged release: TCP sockets, the connection loop, an HTTP/1.1 server and a client.
