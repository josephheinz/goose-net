# goose-net

TCP sockets and a small HTTP/1.1 server and client for [Goose](https://aardappel.github.io/goose/).
POSIX only (macOS and Linux). The native part is `src/net.c`, which `goose` compiles with the system C compiler.

## Use it

In your program's root file:

```goose
import net from "josephheinz/goose-net@v0.5.0";   // or a path to a checkout
import net.http;                                 // only if you want HTTP
```

### TCP: `net`

```goose
let server = net::listen(9000);              // port 0 picks a free port
guard server.ok() else { abort(net::last_error()); }
let conn = server.accept();
var data: u8[>..] = [];
conn.recv(data);                              // appends; 0 = closed, -1 = error
conn.send(data);
conn.close();

let c = net::connect("example.com", 80, 5000);
```

| Function | What it does |
|---|---|
| `listen(port, host = "0.0.0.0", backlog = 128) -> Socket` | Listen for TCP connections. |
| `accept(s) -> Socket` | Wait for the next client. |
| `connect(host, port, timeout_ms = 5000) -> Socket` | Connect by name or address, IPv4 or IPv6. |
| `recv(s, out&, max = 65536) -> i64` | Append bytes that arrived. |
| `recv_all(s, out&, limit = 64 MB) -> bool` | Read until the peer closes. |
| `send(s, data) -> bool` | Send every byte. |
| `close(s)`, `shutdown_write(s)` | Close, or half-close. |
| `set_timeout(s, ms) -> bool` | Read and write timeout. 0 waits forever. |
| `local_port(s)`, `peer_address(s)` | Addresses. |
| `ok(s)`, `last_error()` | Check a socket, and why the last call failed. |

A `Socket` is a plain file descriptor, so it can go to a `thread_fn` worker.

### Connection loop: `net::Loop`

One thread, many open connections. The loop polls the listening socket and every connection,
buffers what arrives, and gives back one complete HTTP/1.x request at a time, round robin.
`http::serve` is built on it.

```goose
let lp = net::connection_loop(server, 5000, 1048576);   // idle timeout ms, max body
var raw: u8[>..] = [];
loop {
    raw.clear();
    let conn, ev = lp.next(raw);          // raw holds exactly one request
    match ev {
        Request => { conn.send(reply); },  // the connection stays open
        _ => lp.close_connection(conn),
    }
}
```

`net::Event` is `Request`, `TooLarge`, `HeadTooLarge`, `Unsupported` (a chunked body), `BadLength`, `Failed` or `Stopped` (the process is stopping and the loop has drained: see Lifecycle).
The loop makes the listening socket non-blocking.

### Lifecycle: `net.lifecycle`

```goose
import net.lifecycle;

net::stop_on_signals(0, 10000);                      // first in main
let server, inherited = net::listen_inherited(8080);
// ... start the workers ...
net::ready();
```

| Function | What it does |
|---|---|
| `stop_on_signals(delay_ms = 0, drain_ms = 10000)` | SIGTERM or SIGINT starts a stop; a second one exits at once. |
| `request_stop()` | The same as a SIGTERM. |
| `stop_phase()`, `stopping()`, `draining()` | 0 running, 1 stopping (still accepting), 2 draining. |
| `wait_stop(ms) -> bool` | Sleep up to `ms`; true once stopping. |
| `listen_inherited(port, host, backlog) -> Socket, bool` | Like `listen`, but takes fd 3 from systemd or a handoff. |
| `enable_handoff(server, argv)` | SIGUSR2 starts the program again on the same listener; `argv` is the arguments joined by NUL bytes. |
| `ready()` | Sends `READY=1` to systemd. In a handoff child it also stops the old process. |

Phase 1 lasts `delay_ms`, so a load balancer can stop routing. In phase 2 loops stop accepting and close idle
connections, and `Loop.next` returns `Event.Stopped` once no connection is left or `drain_ms` has passed.
The process exits after `delay_ms + drain_ms + 5000` ms whatever the loops do.
`http::serve` returns on `Stopped`.
The C test suite is `test/c/run.sh`.

### HTTP server: `http`

```goose
http::serve(net::listen(8080)) { req, res =>
    if req.path != "/hello" {
        http::json_error(res, http::Status.NotFound, "not found");
    } else {
        match req.method {
            Get, Head => http::text(res, http::Status.Ok, "hello\n"),
            _ => http::json_error(res, http::Status.MethodNotAllowed, "GET only"),
        }
    }
};
```

`return` inside the block returns from the function that calls `serve`, so a block uses `if`/`else`.
Larger servers call a handler function from the block, and return early there.

`req` has `method` (a `Method`), `method_text`, `target`, `path`, `query`, `head` and `body`.
The slices are views into the request buffer.
`res` has `status` (a `Status`), `content_type`, `headers` and `body`. The default is `Status.Ok` and plain text.

### Enums

```goose
enum Method { Get, Head, Post, Put, Patch, Delete, Options, Other }
enum Status { SwitchingProtocols, Ok, Created, Accepted, NoContent, MovedPermanently, Found, NotModified,
              BadRequest, Unauthorized, Forbidden, NotFound, MethodNotAllowed, RequestTimeout,
              Conflict, PayloadTooLarge, UnprocessableEntity, HeaderFieldsTooLarge,
              InternalServerError, NotImplemented, ServiceUnavailable, Other { code: i64 } }
```

| Function | What it does |
|---|---|
| `parse_method(s)`, `name(m)` | Text to `Method` and back. Unknown methods are `Method.Other`. |
| `code(s)`, `reason(s)` | `404` and `"Not Found"`. |
| `status_from_code(c)` | The named variant, or `Status.Other { code: c }`. |
| `is_success(s)`, `has_body(s)` | 2xx, and whether the status carries a body. |

`print` shows a `Method` as `GET` and a `Status` as `404 Not Found`.
A variant literal has its own variant type, so give it the enum type before you compare:
`let teapot: http::Status = http::Status.Other { code: 418 };`.

### Server helpers

| Helper | What it does |
|---|---|
| `header(req, name)` | A request header, case-insensitive. |
| `query_param(req, name)` | A raw query value. Decode it with `url_decode(out&, s)`. |
| `cookie(req, name)` | A raw request cookie value, as a view into the request, and whether it was there. The first pair with that name wins; the first `Cookie` header is read. |
| `path_param(path, prefix)` | `path_param("/todos/42", "/todos/")` gives `"42"`. |
| `text(res, status, s)` | A plain text reply. |
| `json_reply(res, status)` | Set the JSON content type. Write the JSON into `res.body`. |
| `json_error(res, status, msg)` | Reply with `{"error": msg}`. |
| `set_header(res, name, value)` | Add a response header. |
| `accepts(req, coding)` | Whether `Accept-Encoding` allows `coding` (`"gzip"`): explicit entry first, then `*`, and `q=0` refuses. False without the header. |
| `add_header(res, name, value)` | `set_header` with checks: false, with `res` unchanged, when the line does not fit or holds a bad byte. Call it once per line for a repeated header such as `Set-Cookie`. |

### Streaming replies

| Function | What it does |
|---|---|
| `begin_chunked(conn, res, head_only = false, keep_alive = false, chunked = true)` | Send the head of a streamed reply (`Transfer-Encoding: chunked`, no length). `res.body` is ignored. |
| `send_chunk(conn, data, chunked = true)`, `end_chunked(conn, chunked = true)` | Send one chunk, then the final `0` chunk. Empty `data` sends nothing. |
| `format_chunk(out&, data)` | Append one chunk to a buffer. |
| `supports_chunked(req)` | False for HTTP/1.0. Pass it as `chunked`: the reply then has no framing and `Connection: close`, and closing the socket ends the body. |
| `send_head(conn, res, connection)` | Status line, `res.headers` and `Connection: <connection>`, with no body headers. For a `101`. |

`net::detach(lp, conn, rest&)` takes a connection out of the loop without closing it and appends the bytes the client sent after the request to `rest`.
Take the head and the tail from a handler that needs the socket for itself, as an upgrade does.

`serve(server, log = true, timeout_ms = 5000, limit = 1 MB, keep_alive = true)` runs your block on one thread, one request at a time.
A poll loop keeps every connection open, so keep-alive clients send many requests on one connection, and pipelined requests are answered in order.
HTTP/1.1 connections stay open unless the client sends `Connection: close`. HTTP/1.0 connections stay open only with `Connection: keep-alive`.
Idle connections close after `timeout_ms`. `keep_alive = false` closes every connection after its response.
It answers bad requests with 400, 413, 431 or 501 and closes them. A `HEAD` request gets the headers only.

Other helpers: `parse_request(raw)` parses one whole request, `wants_keep_alive(req)` reads the client's choice,
and `read_request(conn, raw&, limit)` reads one request from a socket you accepted yourself.

### Long-lived connections: `net::Hub`

`import net.hub;` gives one poll thread that serves many open connections: chunked event streams and WebSockets (RFC 6455, no extensions).
Take a connection out of a `Loop` with `net::detach`, answer the upgrade with `http::send_head`, then hand it to the hub.

```goose
let h = net::hub();                                   // max_conns, max_message, out_limit, ping_ms
thread_spawn(hub_thread, h.handle);                   // the one thread that calls next
// in the request loop, after send_head(conn, res, "Upgrade") and lp.detach(conn, rest):
h.add(conn, net::HubKind.WebSocket, "room", rest);
h.publish("room", "hello");                           // any thread
// hub_thread: let id, ev = h.next(msg, topic);       // Open, Text, Binary, Close or Failed
```

| Function | What it does |
|---|---|
| `hub(max_conns = 10000, max_message = 65536, out_limit = 1048576, ping_ms = 30000)` | Make a hub. `ping_ms <= 0` turns off pings, heartbeats and the silence timeout. |
| `add(h, conn, kind, topic, pending, heartbeat = "")` | Give the hub the socket (`HubKind.Stream` or `WebSocket`). Returns an id that is never reused, or 0 when full or the topic is over 64 bytes (the socket is closed). `pending` is the `rest` from `detach`. |
| `send(h, id, data, binary = false)`, `publish(h, topic, data, binary = false)` | Queue a message for one connection, or for every connection with exactly that topic. False when the queue is over 64 MB. |
| `close(h, id)` | Close gracefully: a close frame, or the final chunk. |
| `count(h, topic)` | Connections with the topic; `""` counts all. |
| `next(h, out&, topic&)` | Hub thread only. Runs the poll loop until an event and returns `id, HubEvent`. `out` gets a `Text` or `Binary` message. |

`add`, `send`, `publish`, `close` and `count` work from any thread; `next` must be called from one.
A `Stream` sends each message as one chunk and the `heartbeat` every `ping_ms`.
A `WebSocket` closes with 1002 on an unmasked frame, 1007 on bad UTF-8 and 1009 past `max_message`, answers pings, echoes a close,
and is closed after 2.5 x `ping_ms` of silence. A connection whose output passes `out_limit` is closed.
`Open` is reported when the hub takes a connection in, and `Close` once for each opened connection.
`http::websocket_accept(key)` gives the `Sec-WebSocket-Accept` value. `examples/ws_echo.goose` is a complete server.

### HTTP client: `http`

```goose
var body: u8[>..] = [];
let status, ok = http::get("http://localhost:8080/todos", body);
if !ok { print(http::last_error()); }
else if status == http::Status.Ok { print(body); }
http::post(url, "{\"title\":\"x\"}", body);
http::request(http::Method.Patch, url, payload, body, "application/json", 10000);
```

The client decodes `Content-Length` and chunked bodies. It speaks plain `http://` only.

## Examples and tests

Run these from the repository root.

```bash
goose build examples/echo.goose && examples/echo 9000
```

```bash
goose build examples/hello_http.goose && examples/hello_http
```

```bash
goose build examples/ws_echo.goose && examples/ws_echo
```

```bash
goose build test/test_net.goose && test/test_net
```

The hub's C core has its own test: `cc -std=c11 -Wall -Wextra -pthread src/hub_core.c test/hub_core_test.c -o hub_test && ./hub_test`.

The tests use worker threads, so they need `goose build`. The examples also run with `goose examples/hello_http.goose`.

## Limits

- No TLS, so no `https://`.
- The handler runs on one thread. A slow handler delays every connection.
- Requests with a chunked body get 501.
- Responses are written with a blocking send under a 5 second timeout, so a client that stops reading delays the others.
- The connection loop uses `poll()`, which suits hundreds of connections. Tens of thousands would need kqueue or epoll.
- The client reads the whole response into memory and sends `Connection: close`.
