// A WebSocket echo server: /ws upgrades, anything else gets a hint.
//   goose build examples/ws_echo.goose && examples/ws_echo
//   then, in a browser console: s = new WebSocket("ws://localhost:8080/ws"); s.onmessage = e => console.log(e.data); s.send("hi")
//
// The HTTP loop hands each upgraded socket to the hub, whose thread echoes every message.

import net from "..";
import net.http;
import net.hub;
import std;

thread_fn hub_worker(hub_handle: u64) {
    let h = net::Hub { handle: hub_handle };
    loop {
        var msg: u8[>..] = [];
        var topic: u8[>..] = [];
        let id, ev = h.next(msg, topic);
        match ev {
            Text => { h.send(id, msg); },
            Binary => { h.send(id, msg, true); },
            _ => {},
        }
    }
}

fn main() {
    let server = net::listen(8080);
    guard server.ok() else { print(net::last_error()); exit(1); }
    let h = net::hub();
    thread_spawn(hub_worker, h.handle);
    print("listening on ws://localhost:", server.local_port(), "/ws");
    let lp = net::connection_loop(server);
    var raw: u8[>..] = [];
    loop {
        raw.clear();
        let conn, ev = lp.next(raw);
        if ev != net::Event.Request { lp.close_connection(conn); continue; }
        let req, outcome = http::parse_request(raw);
        var res = http::new_response();
        let key, has_key = http::header(req, "Sec-WebSocket-Key");
        let upgrade, has_upgrade = http::header(req, "Upgrade");
        if req.path != "/ws" || !has_key || !has_upgrade {
            http::text(res, http::Status.NotFound, "connect a WebSocket to /ws\n");
            http::send_response(conn, res);
            lp.close_connection(conn);
            continue;
        }
        res.status = http::Status.SwitchingProtocols;
        http::set_header(res, "Upgrade", "websocket");
        http::add_header(res, "Sec-WebSocket-Accept", http::websocket_accept(key));
        if !http::send_head(conn, res, "Upgrade") { lp.close_connection(conn); continue; }
        var rest: u8[>..] = [];
        lp.detach(conn, rest);
        h.add(conn, net::HubKind.WebSocket, "echo", rest);
    }
}
