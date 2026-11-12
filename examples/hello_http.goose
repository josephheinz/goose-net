// The smallest HTTP server.
//   goose build examples/hello_http.goose && examples/hello_http
//   curl localhost:8080/hello?name=goose
//
// Note: `return` inside the serve block would return from main, so the
// block uses if/else. Larger servers call a handler function instead.

import net from "..";
import net.http;
import std;

fn main() {
    let server = net::listen(8080);
    guard server.ok() else { print(net::last_error()); exit(1); }
    print("listening on http://localhost:", server.local_port());
    http::serve(server) { req, res =>
        if req.path != "/hello" {
            http::json_error(res, http::Status.NotFound, "not found");
        } else {
            match req.method {
                Get, Head => {
                    let name, has = http::query_param(req, "name");
                    res.body.append("hello, ");
                    if has { http::url_decode(res.body, name); } else { res.body.append("world"); }
                    res.body.push('\n');
                },
                _ => http::json_error(res, http::Status.MethodNotAllowed, "GET only"),
            }
        }
    };
}
