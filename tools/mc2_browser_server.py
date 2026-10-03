"""Static MC2 host with a same-origin frozen-session diagnostic endpoint."""

from __future__ import annotations

import argparse
import json
import pathlib
import threading
import time
from http.server import SimpleHTTPRequestHandler, ThreadingHTTPServer


class MC2Handler(SimpleHTTPRequestHandler):
    protocol_version = "HTTP/1.1"
    diagnostic_file: pathlib.Path
    diagnostic_lock = threading.Lock()

    def end_headers(self) -> None:
        # The shell, the wasm and its glue are replaced on every rebuild, and
        # http.server sends no cache directives at all -- browsers then apply
        # heuristic caching and can quietly keep serving a stale build, which
        # makes a bug report describe an artifact nobody can identify. Game data
        # under /game-data/ is content-versioned by the manifest and stays
        # cacheable; everything else must always be revalidated.
        if not self.path.startswith("/game-data/"):
            self.send_header("Cache-Control", "no-store, must-revalidate")
        super().end_headers()

    def do_GET(self) -> None:
        if self.path.split("?", 1)[0] == "/__mc2_diag/latest":
            payload = "{}"
            try:
                lines = self.diagnostic_file.read_text(encoding="utf-8").splitlines()
                if lines:
                    payload = lines[-1]
            except FileNotFoundError:
                pass
            encoded = payload.encode("utf-8")
            self.send_response(200)
            self.send_header("Content-Type", "application/json; charset=utf-8")
            self.send_header("Cache-Control", "no-store")
            self.send_header("Content-Length", str(len(encoded)))
            self.end_headers()
            self.wfile.write(encoded)
            return
        super().do_GET()

    def do_POST(self) -> None:
        if self.path.split("?", 1)[0] != "/__mc2_diag":
            self.send_error(404)
            return
        try:
            size = int(self.headers.get("Content-Length", "0"))
        except ValueError:
            size = 0
        if size < 1 or size > 65536:
            self.send_error(400, "diagnostic payload must be 1..65536 bytes")
            return
        raw = self.rfile.read(size)
        try:
            record = json.loads(raw.decode("utf-8"))
        except (UnicodeDecodeError, json.JSONDecodeError):
            self.send_error(400, "invalid JSON")
            return
        record["serverTime"] = time.time()
        line = json.dumps(record, separators=(",", ":"), ensure_ascii=True)
        self.diagnostic_file.parent.mkdir(parents=True, exist_ok=True)
        with self.diagnostic_lock:
            with self.diagnostic_file.open("a", encoding="utf-8", newline="\n") as stream:
                stream.write(line + "\n")
        self.send_response(204)
        self.send_header("Content-Length", "0")
        self.end_headers()

    def log_message(self, _format: str, *args: object) -> None:
        pass


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--directory", required=True)
    parser.add_argument("--diagnostics", required=True)
    parser.add_argument("--bind", default="127.0.0.1")
    parser.add_argument("--port", type=int, default=4175)
    args = parser.parse_args()

    root = pathlib.Path(args.directory).resolve()
    MC2Handler.diagnostic_file = pathlib.Path(args.diagnostics).resolve()

    def handler(*handler_args: object, **handler_kwargs: object) -> MC2Handler:
        return MC2Handler(*handler_args, directory=str(root), **handler_kwargs)

    server = ThreadingHTTPServer((args.bind, args.port), handler)
    server.serve_forever()


if __name__ == "__main__":
    main()
