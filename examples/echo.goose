// A TCP echo server: each line a client sends comes back.
//   goose build examples/echo.goose && examples/echo 9000
//   nc localhost 9000

import net from "..";
import std;
import os;

fn main() {
    var port = 9000;
    let a = args();
    if a.len > 1 {
        let p, ok = parse_int(a[1]);
        if ok { port = p; }
    }
    let server = net::listen(port);
    guard server.ok() else { print(net::last_error()); exit(1); }
    print("echo server on port ", server.local_port());
    loop {
        let conn = server.accept();
        if !conn.ok() { continue; }
        print("client ", conn.peer_address());
        var data: u8[>..] = [];
        loop {
            data.clear();
            if conn.recv(data) <= 0 { break; }
            if !conn.send(data) { break; }
        }
        conn.close();
    }
}
