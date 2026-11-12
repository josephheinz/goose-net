// Tests for the net package. Servers run on worker threads, so build it:
//   goose build test/test_net.goose && test/test_net

import net from "..";
import net.http;
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
    check(http::code(http::Status.PermanentRedirect) == 308, "code 308");
    check_eq(http::reason(http::Status.SeeOther), "See Other", "reason 303");
    let named = [http::Status.Ok, http::Status.Accepted, http::Status.Found, http::Status.Conflict,
                 http::Status.SeeOther, http::Status.TemporaryRedirect, http::Status.PermanentRedirect,
                 http::Status.HeaderFieldsTooLarge, http::Status.ServiceUnavailable];
    for s in named { check(http::status_from_code(http::code(s)) == s, "status round trip"); }
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

fn main() {
    enum_tests();
    unit_tests();
    tcp_tests();

    let server = net::listen(0, "127.0.0.1");
    check(server.ok(), "listen on a free port");
    let port = server.local_port();
    check(port > 0, "local_port");
    let raw_server = net::listen(0, "127.0.0.1");
    thread_spawn(http_worker, server);
    thread_spawn(chunked_worker, raw_server);

    http_tests(str("http://127.0.0.1:", port), str("http://127.0.0.1:", raw_server.local_port(), "/"));
    keep_alive_tests(port);

    print(checks - failures, "/", checks, " checks passed");
    if failures > 0 { exit(1); }
}
