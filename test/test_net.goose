// Tests for the net package. Servers run on worker threads, so build it:
//   goose build test/test_net.goose && test/test_net

import net from "..";
import net.http;
import net.hub;
import std;

var failures = 0;
var checks = 0;

fn check(ok: bool, what: const u8[:]) {
    checks += 1;
    if !ok { failures += 1; print("FAIL ", what); }
}

fn check_eq(got: const u8[:], want: const u8[:], what: const u8[:]) {
    checks += 1;
    if got != want { failures += 1; print("FAIL ", what, ": got '", got, "' want '", want, "'"); }
}

fn check_status(got: http::Status, ok: bool, want: http::Status, what: const u8[:]) {
    checks += 1;
    if !ok || got != want {
        failures += 1;
        print("FAIL ", what, ": got ", got, " (ok ", ok, ") want ", want, " ", http::last_error());
    }
}

// An HTTP server that reflects what it was sent.
thread_fn http_worker(server: net::Socket) {
    http::serve(server, false) { req, res =>
        if req.path == "/echo" {
            http::text(res, http::Status.Ok, req.method_text);
            res.body.push(' ');
            res.body.append(req.body);
        } else if req.path == "/method" {
            match req.method {
                Get => res.body.append("get"),
                Post, Put, Patch => res.body.append("write"),
                Delete => res.body.append("delete"),
                Other => res.body.append("other"),
                _ => res.body.append("rest"),
            }
        } else if req.path == "/query" {
            let v, has = http::query_param(req, "q");
            if has { http::url_decode(res.body, v); } else { res.body.append("-"); }
        } else if req.path == "/header" {
            let v, has = http::header(req, "X-Test");
            res.body.append(v);
        } else if req.path == "/created" {
            http::json_reply(res, http::Status.Created);
            res.body.append("{\"ok\":true}");
            http::set_header(res, "Location", "/created/1");
        } else if req.path == "/cookie" {
            let v, has = http::cookie(req, "a");
            res.body.append(v);
            http::add_header(res, "Set-Cookie", "x=1; Path=/");
            http::add_header(res, "Set-Cookie", "y=2; Path=/");
        } else if req.path == "/empty" {
            res.status = http::Status.NoContent;
        } else if req.path == "/teapot" {
            http::text(res, http::Status.Other { code: 418 }, "short and stout");
        } else {
            http::json_error(res, http::Status.NotFound, "nope");
        }
    };
}

// A raw server that answers every connection with a chunked response.
thread_fn chunked_worker(server: net::Socket) {
    loop {
        let conn = server.accept();
        if !conn.ok() { continue; }
        var raw: u8[>..] = [];
        while find(raw, "\r\n\r\n") < 0 { if conn.recv(raw) <= 0 { break; } }
        conn.send("HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n5\r\nhello\r\n7;x=1\r\n, goose\r\n0\r\n\r\n");
        conn.close();
    }
}

// Replies to /chunked with a chunked body, to /raw as an HTTP/1.0 stream, and to /upgrade with a 101.
thread_fn stream_worker(server: net::Socket) {
    let lp = net::connection_loop(server, 5000);
    var raw: u8[>..] = [];
    loop {
        raw.clear();
        let conn, ev = lp.next(raw);
        if ev != net::Event.Request { lp.close_connection(conn); continue; }
        let req, outcome = http::parse_request(raw);
        var res = http::new_response();
        if req.path == "/chunked" {
            let chunked = http::supports_chunked(req);
            let head_only = req.method == http::Method.Head;
            http::begin_chunked(conn, res, head_only, false, chunked);
            if !head_only {
                http::send_chunk(conn, "hello", chunked);
                http::send_chunk(conn, "", chunked);
                http::send_chunk(conn, ", goose", chunked);
                http::end_chunked(conn, chunked);
            }
            lp.close_connection(conn);
        } else if req.path == "/upgrade" {
            res.status = http::Status.SwitchingProtocols;
            http::set_header(res, "Upgrade", "test");
            http::send_head(conn, res, "Upgrade");
            lp.close_connection(conn);
        } else if req.path == "/detach" {
            var rest: u8[>..] = [];
            lp.detach(conn, rest);
            var reply: u8[>..] = [];
            format(reply, "open=", lp.open_connections(), " rest=", rest);
            conn.send(reply);
            conn.close();
        } else {
            lp.close_connection(conn);
        }
    }
}

var ws_hub_handle: u64 = 0;

// The hub thread: echoes text, and closes a connection that sends "bye".
thread_fn ws_hub_worker(handle: u64) {
    let h = net::Hub { handle: handle };
    loop {
        var msg: u8[>..] = [];
        var topic: u8[>..] = [];
        let id, ev = h.next(msg, topic);
        match ev {
            Text => {
                if msg == "bye" { h.close(id); } else { h.send(id, msg); }
            },
            Binary => { h.send(id, msg, true); },
            _ => {},
        }
    }
}

// Upgrades /ws to topic "room" and /other to topic "other"; every upgraded socket goes to the hub.
thread_fn ws_server(server: net::Socket) {
    let h = net::Hub { handle: ws_hub_handle };
    let lp = net::connection_loop(server, 5000);
    var raw: u8[>..] = [];
    loop {
        raw.clear();
        let conn, ev = lp.next(raw);
        if ev != net::Event.Request { lp.close_connection(conn); continue; }
        let req, outcome = http::parse_request(raw);
        let key, has_key = http::header(req, "Sec-WebSocket-Key");
        if !has_key { lp.close_connection(conn); continue; }
        var res = http::new_response();
        res.status = http::Status.SwitchingProtocols;
        http::set_header(res, "Upgrade", "websocket");
        http::add_header(res, "Sec-WebSocket-Accept", http::websocket_accept(key));
        http::send_head(conn, res, "Upgrade");
        var rest: u8[>..] = [];
        lp.detach(conn, rest);
        h.add(conn, net::HubKind.WebSocket, if req.path == "/other" { "other" } else { "room" }, rest);
    }
}

// Reads until n whole responses (head plus Content-Length body) are in out.
fn read_responses(conn: net::Socket, n: i64, out: u8[>..]&) -> bool {
    var pos = 0;
    var done = 0;
    while done < n {
        let he = find(out[pos..], "\r\n\r\n");
        if he >= 0 {
            let total = he + 4 + max(http::content_length(out[pos..pos + he]), 0);
            if out.len - pos >= total { pos += total; done += 1; continue; }
        }
        if conn.recv(out) <= 0 { return false; }
    }
    return true;
}

fn count_of(s: const u8[:], sub: const u8[:]) -> i64 {
    var n = 0;
    var pos = 0;
    loop {
        let k = find(s[pos..], sub);
        if k < 0 { return n; }
        n += 1;
        pos += k + sub.len;
    }
}

// The peer closed: recv sees the end of the stream.
fn closed_by_peer(conn: net::Socket) -> bool {
    var rest: u8[>..] = [];
    return conn.recv(rest) == 0;
}

fn keep_alive_tests(port: i64) {
    let get11 = "GET /echo HTTP/1.1\r\nHost: x\r\n\r\n";

    // Two keep-alive connections take turns: neither blocks the other.
    let a = net::connect("127.0.0.1", port);
    let b = net::connect("127.0.0.1", port);
    a.set_timeout(3000);
    b.set_timeout(3000);
    var ra: u8[>..] = [];
    var rb: u8[>..] = [];
    check(a.send(get11) && read_responses(a, 1, ra), "keep-alive: first request on A");
    check(b.send(get11) && read_responses(b, 1, rb), "keep-alive: B is served while A stays open");
    ra.clear();
    check(a.send(get11) && read_responses(a, 1, ra), "keep-alive: second request on A");
    check(find(ra, "Connection: keep-alive") >= 0, "keep-alive: header says keep-alive");

    // Three pipelined requests in one write, three answers in order.
    ra.clear();
    check(a.send("GET /echo HTTP/1.1\r\nHost: x\r\n\r\nPOST /echo HTTP/1.1\r\nContent-Length: 2\r\n\r\nhiGET /method HTTP/1.1\r\n\r\n")
          && read_responses(a, 3, ra), "pipelined: three responses");
    check(count_of(ra, "HTTP/1.1 200 OK") == 3, "pipelined: three 200s");
    let p1 = find(ra, "GET ");
    let p2 = find(ra, "POST hi");
    let p3 = find(ra, "get");
    check(p1 >= 0 && p2 > p1 && p3 > p2, "pipelined: answers in order");
    a.close();
    b.close();

    // HTTP/1.1 with Connection: close is closed after its response.
    let c = net::connect("127.0.0.1", port);
    c.set_timeout(3000);
    var rc: u8[>..] = [];
    check(c.send("GET /echo HTTP/1.1\r\nConnection: close\r\n\r\n") && read_responses(c, 1, rc), "close: response");
    check(find(rc, "Connection: close") >= 0, "close: header says close");
    check(closed_by_peer(c), "close: server closed the connection");
    c.close();

    // HTTP/1.0 closes unless it asks for keep-alive.
    let d = net::connect("127.0.0.1", port);
    d.set_timeout(3000);
    var rd: u8[>..] = [];
    check(d.send("GET /echo HTTP/1.0\r\n\r\n") && read_responses(d, 1, rd) && closed_by_peer(d), "HTTP/1.0 closes by default");
    d.close();
    let e = net::connect("127.0.0.1", port);
    e.set_timeout(3000);
    var re: u8[>..] = [];
    check(e.send("GET /echo HTTP/1.0\r\nConnection: keep-alive\r\n\r\n") && read_responses(e, 1, re), "HTTP/1.0 keep-alive: first");
    re.clear();
    check(e.send("GET /echo HTTP/1.0\r\nConnection: keep-alive\r\n\r\n") && read_responses(e, 1, re), "HTTP/1.0 keep-alive: second");
    e.close();

    // A body over the limit gets 413 and the connection is closed.
    let f = net::connect("127.0.0.1", port);
    f.set_timeout(3000);
    var rf: u8[>..] = [];
    check(f.send("POST /echo HTTP/1.1\r\nContent-Length: 99999999\r\n\r\n") && read_responses(f, 1, rf), "413: response");
    check(starts_with(rf, "HTTP/1.1 413"), "413: status");
    check(closed_by_peer(f), "413: connection closed");
    f.close();

    // Many keep-alive connections at once, each with several requests.
    var conns: net::Socket[>..] = [];
    for i in 50 { let s = net::connect("127.0.0.1", port); s.set_timeout(3000); conns.push(s); }
    var served = 0;
    for round in 4 {
        for s in conns { s.send(get11); }
        for s in conns {
            var r: u8[>..] = [];
            if read_responses(s, 1, r) && starts_with(r, "HTTP/1.1 200") { served += 1; }
        }
    }
    check(served == 200, "50 keep-alive connections x 4 rounds");
    for s in conns { s.close(); }
}

fn enum_tests() {
    check(http::parse_method("GET") == http::Method.Get, "parse_method GET");
    check(http::parse_method("PATCH") == http::Method.Patch, "parse_method PATCH");
    check(http::parse_method("get") == http::Method.Other, "parse_method is case-sensitive");
    check(http::parse_method("BREW") == http::Method.Other, "parse_method unknown");
    check_eq(http::name(http::Method.Delete), "DELETE", "method name");
    check_eq(str(http::Method.Options), "OPTIONS", "method format");

    check(http::code(http::Status.NotFound) == 404, "code NotFound");
    check(http::code(http::Status.Other { code: 418 }) == 418, "code Other");
    check_eq(http::reason(http::Status.MethodNotAllowed), "Method Not Allowed", "reason");
    check(http::status_from_code(201) == http::Status.Created, "status_from_code named");
    let teapot: http::Status = http::Status.Other { code: 418 };
    let other: http::Status = http::Status.Other { code: 419 };
    check(http::status_from_code(418) == teapot, "status_from_code other");
    check(http::status_from_code(418) != other, "Other payloads differ");
    check_eq(str(http::Status.NotFound), "404 Not Found", "status format");
    check(http::is_success(http::Status.NoContent), "is_success 204");
    check(!http::is_success(http::Status.BadRequest), "is_success 400");
    check(!http::has_body(http::Status.NotModified), "304 has no body");

    // Every named status survives code -> status_from_code.
    check(http::code(http::Status.SwitchingProtocols) == 101, "code 101");
    check_eq(str(http::Status.SwitchingProtocols), "101 Switching Protocols", "status format 101");
    check(http::has_body(http::Status.SwitchingProtocols) == false, "101 has no body");
    check(http::code(http::Status.PermanentRedirect) == 308, "code 308");
    check_eq(http::reason(http::Status.SeeOther), "See Other", "reason 303");
    let named = [http::Status.Ok, http::Status.Accepted, http::Status.Found, http::Status.Conflict,
                 http::Status.SeeOther, http::Status.TemporaryRedirect, http::Status.PermanentRedirect,
                 http::Status.HeaderFieldsTooLarge, http::Status.ServiceUnavailable,
                 http::Status.SwitchingProtocols];
    for s in named { check(http::status_from_code(http::code(s)) == s, "status round trip"); }
}

// One http::cookie lookup on a request with the given head lines.
fn cookie_check(head: const u8[:], name: const u8[:], want: const u8[:], want_has: bool, what: const u8[:]) {
    var raw: u8[..512] = "";
    format(raw, "GET / HTTP/1.1\r\n", head, "\r\n\r\n");
    let req, outcome = http::parse_request(raw);
    let v, has = http::cookie(req, name);
    check(has == want_has, str(what, ": presence"));
    check_eq(v, want, str(what, ": value"));
}

fn cookie_tests() {
    cookie_check("Cookie: a=1; b=2", "b", "2", true, "cookie: second pair");
    cookie_check("Cookie: a=1; a=2", "a", "1", true, "cookie: first duplicate wins");
    cookie_check("Cookie:   a =  1  ;  b=2 ", "a", "1", true, "cookie: whitespace trimmed");
    cookie_check("Cookie: a=\"x y\"", "a", "x y", true, "cookie: quotes removed");
    cookie_check("Cookie: junk; a=1", "a", "1", true, "cookie: pair without = skipped");
    cookie_check("Cookie: junk; a=1", "junk", "", false, "cookie: pair without = is absent");
    cookie_check("Cookie: =x; a=1", "", "", false, "cookie: empty name never matches");
    cookie_check("Cookie: Theme=dark", "theme", "", false, "cookie: case-sensitive");
    cookie_check("Cookie: a=; b=2", "a", "", true, "cookie: empty value is present");
    cookie_check("Cookie: a=1\r\nCookie: b=2", "b", "", false, "cookie: second Cookie header ignored");
    cookie_check("Cookie: a=1\r\nCookie: b=2", "a", "1", true, "cookie: first Cookie header read");
    cookie_check("Host: x", "a", "", false, "cookie: no Cookie header");
    cookie_check("Cookie: a=b=c", "a", "b=c", true, "cookie: value splits at the first =");

    var res = http::new_response();
    check(http::add_header(res, "Set-Cookie", "a=1"), "add_header: normal line");
    check_eq(res.headers, "Set-Cookie: a=1\r\n", "add_header: line bytes");
    check(http::add_header(res, "Set-Cookie", "b=2"), "add_header: repeated name");
    check_eq(res.headers, "Set-Cookie: a=1\r\nSet-Cookie: b=2\r\n", "add_header: two lines");

    var full = http::new_response();
    var filler: u8[..4000] = "";
    for i in 0..4000 { filler.push('x'); }
    full.headers.append(filler);
    check(!http::add_header(full, "Set-Cookie", "a=1; Path=/; Max-Age=31536000; HttpOnly; SameSite=Lax; Secure; Domain=example.com; padding=aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"), "add_header: refuses a line that does not fit");
    check(full.headers.len == 4000, "add_header: unchanged after the refusal");

    var bad = http::new_response();
    check(!http::add_header(bad, "X", "a\rb"), "add_header: refuses CR");
    check(!http::add_header(bad, "X", "a\nb"), "add_header: refuses LF");
    check(!http::add_header(bad, "X", "a\0b"), "add_header: refuses NUL");
    check(!http::add_header(bad, "", "v"), "add_header: refuses an empty name");
    check(!http::add_header(bad, "X:Y", "v"), "add_header: refuses ':' in the name");
    check(!http::add_header(bad, "X Y", "v"), "add_header: refuses a space in the name");
    check(bad.headers.len == 0, "add_header: unchanged after every refusal");
}

fn cookie_wire_test(port: i64) {
    let c = net::connect("127.0.0.1", port, 2000);
    check(c.ok(), "cookie wire: connect");
    var r: u8[>..] = [];
    check(c.send("GET /cookie HTTP/1.1\r\nHost: x\r\nCookie: a=1; b=2\r\nConnection: close\r\n\r\n") && read_responses(c, 1, r), "cookie wire: response");
    let he = find(r, "\r\n\r\n");
    check(he >= 0, "cookie wire: head found");
    check_eq(r[he + 4..], "1", "cookie wire: body is the cookie value");
    check(count_of(r, "Set-Cookie:") == 2, "cookie wire: exactly two Set-Cookie lines");
    let first = find(r, "Set-Cookie: x=1; Path=/\r\n");
    let second = find(r, "Set-Cookie: y=2; Path=/\r\n");
    check(first >= 0 && second > first, "cookie wire: both lines, in order");
    c.close();
}

// One http::accepts lookup on a request with the given head lines.
fn accepts_check(head: const u8[:], coding: const u8[:], want: bool, what: const u8[:]) {
    var raw: u8[..512] = "";
    format(raw, "GET / HTTP/1.1\r\n", head, "\r\n\r\n");
    let req, outcome = http::parse_request(raw);
    check(http::accepts(req, coding) == want, what);
}

fn accepts_tests() {
    accepts_check("Accept-Encoding: gzip", "gzip", true, "accepts: gzip");
    accepts_check("Accept-Encoding: gzip, deflate", "gzip", true, "accepts: in a list");
    accepts_check("Accept-Encoding: gzip;q=0", "gzip", false, "accepts: q=0 refuses");
    accepts_check("Accept-Encoding: gzip;q=0.0", "gzip", false, "accepts: q=0.0 refuses");
    accepts_check("Accept-Encoding: gzip; q=0.001", "gzip", true, "accepts: small q accepts");
    accepts_check("Accept-Encoding: *", "gzip", true, "accepts: star matches");
    accepts_check("Accept-Encoding: *;q=0", "gzip", false, "accepts: star with q=0");
    accepts_check("Accept-Encoding: gzip;q=0, *", "gzip", false, "accepts: explicit refusal beats star");
    accepts_check("Accept-Encoding: br, gzip;q=0.5", "gzip", true, "accepts: weighted entry");
    accepts_check("Accept-Encoding: br, gzip;q=0.5", "br", true, "accepts: other coding");
    accepts_check("Accept-Encoding: br, deflate", "gzip", false, "accepts: not listed");
    accepts_check("Accept-Encoding: identity", "gzip", false, "accepts: identity only");
    accepts_check("Host: x", "gzip", false, "accepts: header absent");
    accepts_check("Accept-Encoding: GZIP", "gzip", true, "accepts: coding is case-insensitive");
    accepts_check("accept-encoding:  Gzip ;Q=0", "gzip", false, "accepts: Q and spaces");
    accepts_check("Accept-Encoding: x-gzip", "gzip", false, "accepts: no partial match");
}

fn chunk_format_tests() {
    var out: u8[>..] = [];
    http::format_chunk(out, "hello");
    check_eq(out, "5\r\nhello\r\n", "format_chunk: small");
    out.clear();
    http::format_chunk(out, "");
    check(out.len == 0, "format_chunk: empty appends nothing");
    var big: u8[>..] = [];
    for i in 255 { big.push('x'); }
    out.clear();
    http::format_chunk(out, big);
    check(starts_with(out, "ff\r\nxxx") && out.len == 2 + 2 + 255 + 2, "format_chunk: hex size ff");
    big.push('x');
    out.clear();
    http::format_chunk(out, big);
    check(starts_with(out, "100\r\n"), "format_chunk: hex size 100");
    out.clear();
    http::format_chunk(out, "a");
    http::format_chunk(out, "bc");
    var body: u8[>..] = [];
    out.append("0\r\n\r\n");
    check(http::decode_chunked(out, body), "format_chunk: decode_chunked reads it back");
    check_eq(body, "abc", "format_chunk: round trip");
}

fn unit_tests() {
    let host, port, target, ok = http::parse_url("http://example.com:8080/a/b?c=1");
    check(ok, "parse_url ok");
    check_eq(host, "example.com", "parse_url host");
    check(port == 8080, "parse_url port");
    check_eq(target, "/a/b?c=1", "parse_url target");
    let h2, p2, t2, ok2 = http::parse_url("http://localhost");
    check(ok2 && p2 == 80, "parse_url default port");
    check_eq(t2, "/", "parse_url default path");
    let h3, p3, t3, ok3 = http::parse_url("https://example.com/");
    check(!ok3, "parse_url rejects https");

    var decoded: u8[>..] = [];
    http::url_decode(decoded, "a%20b+c%2Fd%zz");
    check_eq(decoded, "a b c/d%zz", "url_decode");

    var body: u8[>..] = [];
    check(http::decode_chunked("3\r\nabc\r\n2\r\nde\r\n0\r\n\r\n", body), "decode_chunked ok");
    check_eq(body, "abcde", "decode_chunked data");
    body.clear();
    check(!http::decode_chunked("5\r\nab", body), "decode_chunked short");

    let id, has_id = http::path_param("/todos/42", "/todos/");
    check(has_id, "path_param found");
    check_eq(id, "42", "path_param value");
    let n1, h1 = http::path_param("/todos/", "/todos/");
    check(!h1, "path_param empty");
    let n2, h4 = http::path_param("/todos/4/x", "/todos/");
    check(!h4, "path_param nested");

    check(http::eq_nocase("Content-Length", "content-length"), "eq_nocase");
    let v, has_v = http::find_header("Host: a\r\ncontent-type:  text/x \r\n", "Content-Type");
    check(has_v, "find_header found");
    check_eq(v, "text/x", "find_header value");
    check(http::content_length("Content-Length: 12") == 12, "content_length");
    check(http::content_length("Content-Length: x") == -1, "content_length bad");
}

fn tcp_tests() {
    let bad = net::connect("127.0.0.1", 1, 1000);
    check(!bad.ok(), "connect to a closed port fails");
    check(net::last_error().len > 0, "last_error explains the failure");
    let nohost = net::connect("no-such-host.invalid", 80, 1000);
    check(!nohost.ok(), "connect to an unknown host fails");
}

fn http_tests(base: const u8[:], chunked_url: const u8[:]) {
    var out: u8[>..] = [];
    var s, ok = http::get(str(base, "/echo"), out);
    check_status(s, ok, http::Status.Ok, "GET");
    check_eq(out, "GET ", "GET body");

    out.clear();
    s, ok = http::post(str(base, "/echo"), "{\"a\":1}", out);
    check_status(s, ok, http::Status.Ok, "POST");
    check_eq(out, "POST {\"a\":1}", "POST body");

    out.clear();
    s, ok = http::put(str(base, "/echo"), "xyz", out);
    check_status(s, ok, http::Status.Ok, "PUT");
    check_eq(out, "PUT xyz", "PUT body");

    out.clear();
    s, ok = http::delete(str(base, "/method"), out);
    check_status(s, ok, http::Status.Ok, "DELETE");
    check_eq(out, "delete", "server matched Method.Delete");

    out.clear();
    s, ok = http::request(http::Method.Patch, str(base, "/method"), "{}", out);
    check_eq(out, "write", "server matched Method.Patch");

    out.clear();
    s, ok = http::request(http::Method.Head, str(base, "/echo"), "", out);
    check_status(s, ok, http::Status.Ok, "HEAD");
    check(out.len == 0, "HEAD has no body");

    out.clear();
    s, ok = http::request(http::Method.Other, str(base, "/echo"), "", out);
    check(!ok, "client refuses Method.Other");

    out.clear();
    s, ok = http::get(str(base, "/query?x=1&q=hello%20goose"), out);
    check_status(s, ok, http::Status.Ok, "query");
    check_eq(out, "hello goose", "query value");

    out.clear();
    s, ok = http::get(str(base, "/created"), out);
    check_status(s, ok, http::Status.Created, "201");
    check_eq(out, "{\"ok\":true}", "201 body");

    out.clear();
    s, ok = http::get(str(base, "/empty"), out);
    check_status(s, ok, http::Status.NoContent, "204");
    check(out.len == 0, "204 has no body");

    out.clear();
    s, ok = http::get(str(base, "/teapot"), out);
    let teapot: http::Status = http::Status.Other { code: 418 };
    check_status(s, ok, teapot, "418 as Status.Other");
    check_eq(out, "short and stout", "418 body");

    out.clear();
    s, ok = http::get(str(base, "/missing"), out);
    check_status(s, ok, http::Status.NotFound, "404");
    check_eq(out, "{\"error\":\"nope\"}", "404 body");

    // A big body crosses intact.
    var big: u8[>..] = [];
    for i in 200000 { big.push(('a' + i % 26) as u8); }
    out.clear();
    s, ok = http::post(str(base, "/echo"), big, out);
    check_status(s, ok, http::Status.Ok, "big POST");
    check(out.len == big.len + 5 && out[5..] == big, "big POST body");

    out.clear();
    s, ok = http::get(chunked_url, out);
    check_status(s, ok, http::Status.Ok, "chunked");
    check_eq(out, "hello, goose", "chunked body");

    out.clear();
    s, ok = http::get("http://127.0.0.1:1/", out);
    check(!ok, "client reports a refused connection");
    check(http::last_error().len > 0, "client error message");
}

fn stream_tests(port: i64) {
    var out: u8[>..] = [];
    let s, ok = http::get(str("http://127.0.0.1:", port, "/chunked"), out);
    check_status(s, ok, http::Status.Ok, "chunked reply");
    check_eq(out, "hello, goose", "chunked reply decoded by the client");

    // The raw head, to see the framing.
    let a = net::connect("127.0.0.1", port);
    a.set_timeout(3000);
    var ra: u8[>..] = [];
    check(a.send("GET /chunked HTTP/1.1\r\nHost: x\r\n\r\n") && a.recv_all(ra), "chunked wire: response");
    check(find(ra, "Transfer-Encoding: chunked\r\n") >= 0, "chunked wire: header");
    check(find(ra, "Content-Length") < 0, "chunked wire: no Content-Length");
    check(find(ra, "\r\n\r\n5\r\nhello\r\n7\r\n, goose\r\n0\r\n\r\n") >= 0, "chunked wire: chunks, empty one skipped");
    a.close();

    // HTTP/1.0: no framing, the close ends the body.
    let b = net::connect("127.0.0.1", port);
    b.set_timeout(3000);
    var rb: u8[>..] = [];
    check(b.send("GET /chunked HTTP/1.0\r\n\r\n") && b.recv_all(rb), "HTTP/1.0 stream: response");
    check(find(rb, "Transfer-Encoding") < 0 && find(rb, "Connection: close\r\n") >= 0, "HTTP/1.0 stream: no chunked header");
    check(ends_with(rb, "\r\n\r\nhello, goose"), "HTTP/1.0 stream: raw body");
    b.close();

    // HEAD gets the head and no body.
    let h = net::connect("127.0.0.1", port);
    h.set_timeout(3000);
    var rh: u8[>..] = [];
    check(h.send("HEAD /chunked HTTP/1.1\r\n\r\n") && h.recv_all(rh), "HEAD stream: response");
    check(find(rh, "hello") < 0, "HEAD stream: no body");
    h.close();

    // send_head writes no body headers.
    let u = net::connect("127.0.0.1", port);
    u.set_timeout(3000);
    var ru: u8[>..] = [];
    check(u.send("GET /upgrade HTTP/1.1\r\n\r\n") && u.recv_all(ru), "send_head: response");
    check_eq(ru, "HTTP/1.1 101 Switching Protocols\r\nUpgrade: test\r\nConnection: Upgrade\r\n\r\n", "send_head: exact bytes");
    u.close();
}

fn detach_test(port: i64) {
    // The bytes sent after the request land in rest, and the connection is out of the loop but open.
    let c = net::connect("127.0.0.1", port);
    c.set_timeout(3000);
    check(c.send("GET /detach HTTP/1.1\r\nHost: x\r\n\r\nfirst-frame"), "detach: send");
    var r: u8[>..] = [];
    check(c.recv_all(r), "detach: reply");
    check_eq(r, "open=0 rest=first-frame", "detach: rest and loop count");
    c.close();

    // Nothing after the request: rest stays empty.
    let d = net::connect("127.0.0.1", port);
    d.set_timeout(3000);
    var rd: u8[>..] = [];
    check(d.send("GET /detach HTTP/1.1\r\n\r\n") && d.recv_all(rd), "detach: bare request");
    check_eq(rd, "open=0 rest=", "detach: empty rest");
    d.close();
}

// A client frame with mask 01 02 03 04, or without one when masked is false. Payloads under 126 bytes.
fn ws_frame(opcode: i64, payload: const u8[:], masked: bool = true) -> u8[>..] {
    var f: u8[>..] = [];
    f.push((128 + opcode) as u8);
    if masked {
        f.push((128 + payload.len) as u8);
        for m in 4 { f.push((m + 1) as u8); }
        for i in payload.len { f.push((payload[i] ^ ((i % 4 + 1) as u8))); }
    } else {
        f.push(payload.len as u8);
        f.append(payload);
    }
    return f;
}

fn close_code(code: i64) -> u8[>..] {
    var p: u8[>..] = [];
    p.push((code / 256) as u8);
    p.push((code % 256) as u8);
    return p;
}

// Reads one server frame (unmasked, under 126 bytes) into payload; returns its opcode, or -1.
fn ws_read(conn: net::Socket, payload: u8[>..]&) -> i64 {
    var buf: u8[>..] = [];
    while buf.len < 2 || buf.len < 2 + (buf[1] as i64) {
        if conn.recv(buf) <= 0 { return -1; }
    }
    let n = buf[1] as i64;
    payload.append(buf[2..2 + n]);
    return (buf[0] as i64) & 15;
}

fn ws_connect(port: i64, path: const u8[:]) -> net::Socket {
    let c = net::connect("127.0.0.1", port);
    c.set_timeout(3000);
    var head: u8[>..] = [];
    var req: u8[>..] = [];
    format(req, "GET ", path, " HTTP/1.1\r\nHost: x\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n",
           "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\nSec-WebSocket-Version: 13\r\n\r\n");
    check(c.send(req), "ws: send handshake");
    while find(head, "\r\n\r\n") < 0 { if c.recv(head) <= 0 { break; } }
    check(starts_with(head, "HTTP/1.1 101 Switching Protocols\r\n"), "ws: 101");
    check(find(head, "Sec-WebSocket-Accept: s3pPLMBiTxaQ9kYGzzhZRbK+xOo=\r\n") >= 0, "ws: accept header");
    return c;
}

fn ws_expect(c: net::Socket, opcode: i64, text: const u8[:], what: const u8[:]) {
    var p: u8[>..] = [];
    let op = ws_read(c, p);
    check(op == opcode, str(what, ": opcode ", op));
    check_eq(p, text, what);
}

fn websocket_tests(port: i64, h: net::Hub) {
    check_eq(http::websocket_accept("dGhlIHNhbXBsZSBub25jZQ=="), "s3pPLMBiTxaQ9kYGzzhZRbK+xOo=", "websocket_accept: RFC 6455 example");

    let a = ws_connect(port, "/ws");
    check(a.send(ws_frame(1, "hello")), "ws: send text");
    ws_expect(a, 1, "hello", "ws: echo");

    let b = ws_connect(port, "/ws");
    let o = ws_connect(port, "/other");
    // count includes connections the hub thread has not taken in yet, so this wait is for the adds.
    var spins = 0;
    while h.count("room") < 2 && spins < 50000000 { spins += 1; }
    check(h.count("room") == 2 && h.count("other") == 1 && h.count("") == 3, "ws: counts by topic");

    check(h.publish("room", "news"), "ws: publish");
    ws_expect(a, 1, "news", "ws: publish to first client");
    ws_expect(b, 1, "news", "ws: publish to second client");
    // The "other" client got nothing from publish: its first frame is its own echo.
    check(o.send(ws_frame(1, "mine")), "ws: other send");
    ws_expect(o, 1, "mine", "ws: other topic did not get the publish");

    // A client close is echoed, then the socket closes.
    check(a.send(ws_frame(8, close_code(1000))), "ws: send close");
    ws_expect(a, 8, close_code(1000), "ws: close echoed");
    check(closed_by_peer(a), "ws: socket closed after close");
    a.close();

    // The server closes on request.
    check(b.send(ws_frame(1, "bye")), "ws: send bye");
    var pb: u8[>..] = [];
    check(ws_read(b, pb) == 8, "ws: server close frame");
    b.close();

    // An unmasked client frame is a protocol error: close 1002.
    check(o.send(ws_frame(1, "raw", false)), "ws: send unmasked");
    ws_expect(o, 8, close_code(1002), "ws: unmasked closes with 1002");
    o.close();
}

fn main() {
    enum_tests();
    unit_tests();
    cookie_tests();
    accepts_tests();
    chunk_format_tests();
    tcp_tests();

    let server = net::listen(0, "127.0.0.1");
    check(server.ok(), "listen on a free port");
    let port = server.local_port();
    check(port > 0, "local_port");
    let raw_server = net::listen(0, "127.0.0.1");
    thread_spawn(http_worker, server);
    thread_spawn(chunked_worker, raw_server);
    let stream_server = net::listen(0, "127.0.0.1");
    thread_spawn(stream_worker, stream_server);

    ws_hub_handle = net::hub(1000, 65536, 1048576, 0).handle;
    thread_spawn(ws_hub_worker, ws_hub_handle);
    let ws_listener = net::listen(0, "127.0.0.1");
    thread_spawn(ws_server, ws_listener);

    http_tests(str("http://127.0.0.1:", port), str("http://127.0.0.1:", raw_server.local_port(), "/"));
    keep_alive_tests(port);
    cookie_wire_test(port);
    stream_tests(stream_server.local_port());
    detach_test(stream_server.local_port());
    websocket_tests(ws_listener.local_port(), net::Hub { handle: ws_hub_handle });

    print(checks - failures, "/", checks, " checks passed");
    if failures > 0 { exit(1); }
}
