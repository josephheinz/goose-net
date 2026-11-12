# Changelog

## v0.3.0 (unreleased)

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
