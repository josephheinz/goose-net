// http: a small HTTP/1.1 server and client on top of net. The client speaks plain http:// only, with no TLS.

namespace http;

import std;
import net;
import json;

const MAX_HEAD = 65536;

// ---------------------------------------------------------------- methods

enum Method { Get, Head, Post, Put, Patch, Delete, Options, Other }

fn parse_method(s: const u8[:]) -> Method {
    if s == "GET" { return Method.Get; }
    if s == "HEAD" { return Method.Head; }
    if s == "POST" { return Method.Post; }
    if s == "PUT" { return Method.Put; }
    if s == "PATCH" { return Method.Patch; }
    if s == "DELETE" { return Method.Delete; }
    if s == "OPTIONS" { return Method.Options; }
    return Method.Other;
}

fn name(m: Method) -> const u8[:] {
    match m {
        Get => "GET",
        Head => "HEAD",
        Post => "POST",
        Put => "PUT",
        Patch => "PATCH",
        Delete => "DELETE",
        Options => "OPTIONS",
        Other => "OTHER",
    }
}

fn format(out: u8[>..]&, m: Method) { out.append(name(m)); }

// ---------------------------------------------------------------- statuses

// The status codes this library names. Any other code is Other { code }.
enum Status {
    Ok, Created, Accepted, NoContent,
    MovedPermanently, Found, SeeOther, NotModified, TemporaryRedirect, PermanentRedirect,
    BadRequest, Unauthorized, Forbidden, NotFound, MethodNotAllowed, RequestTimeout,
    Conflict, PayloadTooLarge, UnprocessableEntity, HeaderFieldsTooLarge,
    InternalServerError, NotImplemented, ServiceUnavailable,
    Other { code: i64 }
}

fn code(s: Status) -> i64 {
    match s {
        Ok => 200, Created => 201, Accepted => 202, NoContent => 204,
        MovedPermanently => 301, Found => 302, SeeOther => 303, NotModified => 304,
        TemporaryRedirect => 307, PermanentRedirect => 308,
        BadRequest => 400, Unauthorized => 401, Forbidden => 403, NotFound => 404,
        MethodNotAllowed => 405, RequestTimeout => 408, Conflict => 409,
        PayloadTooLarge => 413, UnprocessableEntity => 422, HeaderFieldsTooLarge => 431,
        InternalServerError => 500, NotImplemented => 501, ServiceUnavailable => 503,
        Other o => o.code,
    }
}

fn reason(s: Status) -> const u8[:] {
    match s {
        Ok => "OK", Created => "Created", Accepted => "Accepted", NoContent => "No Content",
        MovedPermanently => "Moved Permanently", Found => "Found", SeeOther => "See Other",
        NotModified => "Not Modified", TemporaryRedirect => "Temporary Redirect",
        PermanentRedirect => "Permanent Redirect",
        BadRequest => "Bad Request", Unauthorized => "Unauthorized", Forbidden => "Forbidden",
        NotFound => "Not Found", MethodNotAllowed => "Method Not Allowed",
        RequestTimeout => "Request Timeout", Conflict => "Conflict",
        PayloadTooLarge => "Payload Too Large", UnprocessableEntity => "Unprocessable Entity",
        HeaderFieldsTooLarge => "Request Header Fields Too Large",
        InternalServerError => "Internal Server Error", NotImplemented => "Not Implemented",
        ServiceUnavailable => "Service Unavailable",
        Other => "Unknown",
    }
}

fn status_from_code(c: i64) -> Status {
    match c {
        200 => Status.Ok, 201 => Status.Created, 202 => Status.Accepted, 204 => Status.NoContent,
        301 => Status.MovedPermanently, 302 => Status.Found, 303 => Status.SeeOther,
        304 => Status.NotModified, 307 => Status.TemporaryRedirect, 308 => Status.PermanentRedirect,
        400 => Status.BadRequest, 401 => Status.Unauthorized, 403 => Status.Forbidden,
        404 => Status.NotFound, 405 => Status.MethodNotAllowed, 408 => Status.RequestTimeout,
        409 => Status.Conflict, 413 => Status.PayloadTooLarge, 422 => Status.UnprocessableEntity,
        431 => Status.HeaderFieldsTooLarge, 500 => Status.InternalServerError,
        501 => Status.NotImplemented, 503 => Status.ServiceUnavailable,
        _ => Status.Other { code: c },
    }
}

fn format(out: u8[>..]&, s: Status) { format(out, code(s), " ", reason(s)); }

fn is_success(s: Status) -> bool { let c = code(s); c >= 200 && c < 300 }

fn has_body(s: Status) -> bool {
    match s { NoContent, NotModified => false, _ => code(s) >= 200 }
}

// ---------------------------------------------------------------- shared

fn lower(c: u8) -> u8 { if c >= 'A' && c <= 'Z' { c + 32 } else { c } }

fn eq_nocase(a: const u8[:], b: const u8[:]) -> bool {
    if a.len != b.len { return false; }
    for i in a.len { if lower(a[i]) != lower(b[i]) { return false; } }
    return true;
}

fn find_header(head: const u8[:], name: const u8[:]) -> const u8[:], bool {
    each_split(head, "\r\n") { line =>
        let key, value, has = split_once(line, ':');
        if has && eq_nocase(trim(key), name) { return trim(value), true; }
    };
    return "", false;
}

// Content-Length in a header block: 0 when absent, -1 when malformed.
fn content_length(head: const u8[:]) -> i64 {
    let value, has = find_header(head, "content-length");
    if !has { return 0; }
    let n, ok = parse_int(value);
    if !ok || n < 0 { return -1; }
    return n;
}

fn is_chunked(head: const u8[:]) -> bool {
    let value, has = find_header(head, "transfer-encoding");
    return has && find(value, "chunked") >= 0;
}

fn decode_chunked(s: const u8[:], out: u8[>..]&) -> bool {
    var pos = 0;
    loop {
        let rel = find(s[pos..], "\r\n");
        if rel < 0 { return false; }
        let size_text, ext, has_ext = split_once(s[pos..pos + rel], ';');
        let size, ok = parse_int(trim(size_text), 16);
        if !ok || size < 0 { return false; }
        pos += rel + 2;
        if size == 0 { return true; }
        if pos + size > s.len { return false; }
        out.append(s[pos..pos + size]);
        pos += size + 2;
    }
}

// Decodes %XX escapes and '+' (as a space).
fn url_decode(out: u8[>..]&, s: const u8[:]) {
    var i = 0;
    while i < s.len {
        let c = s[i];
        if c == '+' { out.push(' '); i += 1; }
        else if c == '%' && i + 2 < s.len && ascii::hex_value(s[i + 1]) >= 0 && ascii::hex_value(s[i + 2]) >= 0 {
            out.push((ascii::hex_value(s[i + 1]) * 16 + ascii::hex_value(s[i + 2])) as u8);
            i += 3;
        } else { out.push(c); i += 1; }
    }
}

// ---------------------------------------------------------------- server

// The slices are views into the buffer read_request filled.
struct Request {
    method: Method,
    method_text: const u8[:], // as sent, for Method.Other
    target: const u8[:],
    path: const u8[:],
    query: const u8[:],
    version: const u8[:],
    head: const u8[:],        // the header lines, CRLF separated
    body: const u8[:]
}

fn empty_request() -> Request {
    Request { method: Method.Other, method_text: "", target: "", path: "", query: "", version: "", head: "", body: "" }
}

enum Read {
    Ready,
    Closed,                      // the client went away or timed out
    Reject { status: Status }    // answer with this status and close
}

fn reject(s: Status) -> Request, Read { return empty_request(), Read.Reject { status: s }; }

fn read_request(conn: net::Socket, raw: u8[>..]&, limit: i64 = 1048576) -> Request, Read {
    var head_end = -1;
    var want = -1;
    loop {
        let n = conn.recv(raw);
        if n <= 0 { return empty_request(), Read.Closed; }
        if head_end < 0 {
            head_end = find(raw, "\r\n\r\n");
            if head_end < 0 && raw.len > MAX_HEAD { return reject(Status.HeaderFieldsTooLarge); }
            if head_end >= 0 {
                if is_chunked(raw[..head_end]) { return reject(Status.NotImplemented); }
                let cl = content_length(raw[..head_end]);
                if cl < 0 { return reject(Status.BadRequest); }
                if cl > limit { return reject(Status.PayloadTooLarge); }
                want = head_end + 4 + cl;
            }
        }
        if want >= 0 && raw.len >= want { break; }
    }

    return parse_request(raw[..want]);
}

// Parses raw, which holds exactly one request: the head, the blank line and
// the body. Every slice of the result points into raw.
fn parse_request(raw: const u8[:]) -> Request, Read {
    let head_end = find(raw, "\r\n\r\n");
    if head_end < 0 { return reject(Status.BadRequest); }
    let line, head, has_head = split_once(raw[..head_end], "\r\n");
    let parts = words(line);
    if parts.len != 3 || !starts_with(parts[2], "HTTP/1.") { return reject(Status.BadRequest); }
    let path, query, has_query = split_once(parts[1], '?');
    let req = Request {
        method: parse_method(parts[0]), method_text: parts[0], target: parts[1],
        path: path, query: query, version: parts[2], head: head, body: raw[head_end + 4..]
    };
    return req, Read.Ready;
}

// True for HTTP/1.1 unless "Connection: close"; HTTP/1.0 needs "Connection: keep-alive".
fn wants_keep_alive(req: const Request&) -> bool {
    let value, has = find_header(req.head, "connection");
    if req.version == "HTTP/1.1" { return !(has && eq_nocase(value, "close")); }
    return has && eq_nocase(value, "keep-alive");
}

fn header(req: const Request&, name: const u8[:]) -> const u8[:], bool {
    return find_header(req.head, name);
}

// The raw (still URL encoded) value of a query parameter.
fn query_param(req: const Request&, name: const u8[:]) -> const u8[:], bool {
    each_split(req.query, '&') { pair =>
        let key, value, has = split_once(pair, '=');
        if key == name { return value, true; }
    };
    return "", false;
}

// A view into the first Cookie header (RFC 6265 section 5.4), with one pair of double quotes removed.
// Names match exactly; the first pair with that name wins and a pair without '=' is skipped.
fn cookie(req: const Request&, name: const u8[:]) -> const u8[:], bool {
    let line, has = find_header(req.head, "cookie");
    if !has || name.len == 0 { return "", false; }
    each_split(line, ';') { pair =>
        let key, value, eq = split_once(pair, '=');
        if eq && trim(key) == name {
            let v = trim(value);
            if v.len >= 2 && v[0] == '"' && v[v.len - 1] == '"' { return v[1..v.len - 1], true; }
            return v, true;
        }
    };
    return "", false;
}

// When path is prefix followed by one more segment, that segment.
fn path_param(path: const u8[:], prefix: const u8[:]) -> const u8[:], bool {
    if !starts_with(path, prefix) || path.len == prefix.len { return "", false; }
    let rest = path[prefix.len..];
    if find(rest, "/") >= 0 { return "", false; }
    return rest, true;
}

// What a handler fills in. headers holds extra "Name: value\r\n" lines.
struct Response { status: Status, content_type: u8[..128], headers: u8[..4096], body: u8[>..] }

fn new_response() -> Response {
    Response { status: Status.Ok, content_type: "text/plain; charset=utf-8", headers: "", body: [] }
}

fn set_header(res: Response&, name: const u8[:], value: const u8[:]) {
    format(res.headers, name, ": ", value, "\r\n");
}

// Adds a response header line after checking it: false, with res unchanged,
// when the line does not fit in res.headers (set_header would abort), when
// name is empty or holds a control byte, a space or ':', or when value holds
// CR, LF or NUL. Call it once per line for a header that repeats, such as
// Set-Cookie.
fn add_header(res: Response&, name: const u8[:], value: const u8[:]) -> bool {
    if name.len == 0 || name.len + value.len + 4 > res.headers.cap - res.headers.len { return false; }
    for c in name { if c <= ' ' || c == ':' || c >= 127 { return false; } }
    for c in value { if c == '\r' || c == '\n' || c == 0 { return false; } }
    format(res.headers, name, ": ", value, "\r\n");
    return true;
}

fn text(res: Response&, status: Status, s: const u8[:]) {
    res.status = status;
    res.content_type.clear();
    res.content_type.append("text/plain; charset=utf-8");
    res.body.append(s);
}

fn json_reply(res: Response&, status: Status) {
    res.status = status;
    res.content_type.clear();
    res.content_type.append("application/json");
}

// A JSON reply of {"error": msg}.
fn json_error(res: Response&, status: Status, msg: const u8[:]) {
    json_reply(res, status);
    res.body.clear();
    res.body.push('{');
    json::format_key(res.body, "error");
    json::format_string(res.body, msg);
    res.body.push('}');
}

fn send_response(conn: net::Socket, res: const Response&, head_only: bool = false, keep_alive: bool = false) -> bool {
    return send_response_with(conn, res, res.body, head_only, keep_alive);
}

// Sends body instead of res.body, so a body that lives elsewhere (an embedded file) is never copied;
// head and body go out in one writev.
fn send_response_with(conn: net::Socket, res: const Response&, body: const u8[:], head_only: bool = false,
                      keep_alive: bool = false) -> bool {
    var head: u8[>..] = [];
    format(head, "HTTP/1.1 ", res.status, "\r\n");
    let has = has_body(res.status);
    if has {
        format(head, "Content-Type: ", res.content_type, "\r\nContent-Length: ", body.len, "\r\n");
    }
    head.append(res.headers);
    if keep_alive { head.append("Connection: keep-alive\r\n\r\n"); }
    else { head.append("Connection: close\r\n\r\n"); }
    if !has || head_only { return conn.send(head); }
    return conn.send(head, body);
}

// Accepts connections forever, calling F(req: const Request&, res: Response&) on this thread, one request at a time.
// keep_alive = false closes each connection after its response; timeout_ms closes idle ones.
fn serve<F>(server: net::Socket, log: bool = true, timeout_ms: i64 = 5000, limit: i64 = 1048576,
            keep_alive: bool = true, accept_max: i64 = 0) {
    let lp = net::connection_loop(server, timeout_ms, limit, accept_max);
    var raw: u8[>..] = [];
    loop {
        raw.clear();
        let conn, ev = lp.next(raw);
        var res = new_response();
        match ev {
            Request => {
                let req, outcome = parse_request(raw);
                var keep = false;
                match outcome {
                    Ready => {
                        F(req, &res);
                        keep = keep_alive && wants_keep_alive(req);
                    },
                    Reject r => json_error(res, r.status, reason(r.status)),
                    Closed => {},
                }
                let sent = send_response(conn, res, req.method == Method.Head, keep);
                if log { print(req.method_text, " ", req.target, " -> ", res.status); }
                if !sent || !keep { lp.close_connection(conn); }
            },
            Failed => {},
            _ => {
                let status = match ev {
                    TooLarge => Status.PayloadTooLarge,
                    HeadTooLarge => Status.HeaderFieldsTooLarge,
                    Unsupported => Status.NotImplemented,
                    _ => Status.BadRequest,
                };
                json_error(res, status, reason(status));
                send_response(conn, res);
                lp.close_connection(conn);
            },
        }
    }
}

// ---------------------------------------------------------------- client

// Fixed size, so the client also works on thread_fn workers, which may not
// reach a growable global.
var client_error: u8[..256];

fn last_error() -> const u8[:] { client_error }

fn client_fail(msg: const u8[:]) -> Status, bool {
    client_error.clear();
    client_error.append(msg[..min(msg.len, 256)]);
    return Status.Other { code: 0 }, false;
}

fn parse_url(url: const u8[:]) -> const u8[:], i64, const u8[:], bool {
    if !starts_with(url, "http://") { return "", 0, "", false; }
    let rest = url[7..];
    var slash = find(rest, "/");
    if slash < 0 { slash = rest.len; }
    let authority = rest[..slash];
    let target = if slash < rest.len { rest[slash..] } else { "/" };
    let host, port_text, has_port = split_once(authority, ':');
    var port = 80;
    if has_port {
        let p, ok = parse_int(port_text);
        if !ok || p <= 0 || p > 65535 { return "", 0, "", false; }
        port = p;
    }
    if host.len == 0 { return "", 0, "", false; }
    return host, port, target, true;
}

// Returns the status and true, or false with the reason in last_error(). content_type is sent only with a body.
fn request(method: Method, url: const u8[:], body: const u8[:], out: u8[>..]&,
           content_type: const u8[:] = "application/json", timeout_ms: i64 = 10000) -> Status, bool {
    if method == Method.Other { return client_fail("http: Method.Other cannot be sent"); }
    let host, port, target, ok = parse_url(url);
    if !ok { return client_fail("http: only http://host[:port]/path URLs are supported"); }
    let conn = net::connect(host, port, timeout_ms);
    if !conn.ok() { return client_fail(net::last_error()); }
    conn.set_timeout(timeout_ms);

    var req: u8[>..] = [];
    format(req, method, " ", target, " HTTP/1.1\r\nHost: ", host);
    if port != 80 { format(req, ":", port); }
    req.append("\r\nUser-Agent: goose-net\r\nAccept: */*\r\nConnection: close\r\n");
    let sends_body = match method { Post, Put, Patch => true, _ => body.len > 0 };
    if sends_body {
        format(req, "Content-Type: ", content_type, "\r\nContent-Length: ", body.len, "\r\n");
    }
    req.append("\r\n");
    req.append(body);
    if !conn.send(req) { conn.close(); return client_fail(net::last_error()); }

    var raw: u8[>..] = [];
    let got = conn.recv_all(raw);
    conn.close();
    if !got { return client_fail(net::last_error()); }

    let head_end = find(raw, "\r\n\r\n");
    if head_end < 0 { return client_fail("http: incomplete response head"); }
    let status_line, head, has_head = split_once(raw[..head_end], "\r\n");
    let parts = words(status_line);
    if parts.len < 2 { return client_fail("http: bad status line"); }
    let n_status, status_ok = parse_int(parts[1]);
    if !status_ok { return client_fail("http: bad status code"); }
    let status = status_from_code(n_status);

    if method == Method.Head || !has_body(status) { return status, true; }
    let payload = raw[head_end + 4..];
    if is_chunked(head) {
        if !decode_chunked(payload, out) { return client_fail("http: bad chunked body"); }
    } else {
        let value, has_length = find_header(head, "content-length");
        if has_length {
            let n, n_ok = parse_int(value);
            if !n_ok || n < 0 || n > payload.len { return client_fail("http: body shorter than Content-Length"); }
            out.append(payload[..n]);
        } else {
            out.append(payload);
        }
    }
    return status, true;
}

fn get(url: const u8[:], out: u8[>..]&) -> Status, bool { return request(Method.Get, url, "", out); }

fn post(url: const u8[:], body: const u8[:], out: u8[>..]&) -> Status, bool { return request(Method.Post, url, body, out); }

fn put(url: const u8[:], body: const u8[:], out: u8[>..]&) -> Status, bool { return request(Method.Put, url, body, out); }

fn delete(url: const u8[:], out: u8[>..]&) -> Status, bool { return request(Method.Delete, url, "", out); }
